/**
 * @file wifi_control.c
 * @brief 纯 Wi-Fi STA runtime control 组件。
 */

#include "wifi_control_internal.h"

#include <string.h>
#include <stdio.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"

/** @brief 组件日志标签。 */
static const char *TAG = "wifi_ctrl";

/** @brief 单实例运行时上下文，只保存 STA 控制面状态。 */
static wifi_control_runtime_t s_runtime = {
    .initialized = false,
    .init_in_progress = false,
    .connected = false,
    .auto_reconnect_enabled = true,
    .reconnect_suppressed = false,
    .reconnect_after_disconnect = false,
    .retry_count = 0,
    .state = WIFI_CONTROL_STATE_IDLE,
};

/** @brief STA 默认网络接口句柄，仅创建一次并长期复用。 */
static esp_netif_t *s_sta_netif = NULL;
/** @brief 保护 `s_runtime` 的最小临界区锁。 */
static portMUX_TYPE s_runtime_lock = portMUX_INITIALIZER_UNLOCKED;

/** @brief 是否已经完成过 Wi-Fi 驱动初始化。 */
static bool s_wifi_driver_initialized = false;
/** @brief 是否已经注册过 WIFI_EVENT 处理器。 */
static bool s_wifi_event_handler_registered = false;
/** @brief 是否已经注册过 IP_EVENT 处理器。 */
static bool s_wifi_ip_event_handler_registered = false;
/** @brief 是否已经调用过 `esp_wifi_start()`。 */
static bool s_wifi_started = false;

static esp_err_t wifi_control_ensure_nvs_ready(void);
static esp_err_t wifi_control_ensure_stack_ready(void);
static esp_err_t wifi_control_cleanup_partial_init(void);
static esp_err_t wifi_control_request_disconnect(bool reconnect_after_disconnect);
static void wifi_control_runtime_set_state(wifi_control_state_t state);
static bool wifi_control_runtime_is_initialized(void);
static bool wifi_control_runtime_begin_init(void);
static void wifi_control_runtime_end_init(void);
static void wifi_control_runtime_set_initialized(bool initialized);
static void wifi_control_runtime_wait_for_init_completion(void);
static void wifi_control_runtime_set_connected(bool connected);
static void wifi_control_runtime_set_retry_count(uint8_t retry_count);
static void wifi_control_runtime_set_auto_reconnect_enabled(bool enabled);
static void wifi_control_runtime_set_reconnect_markers(bool suppressed,
                                                       bool reconnect_after_disconnect);
static void wifi_control_runtime_clear_reconnect_markers(void);
static bool wifi_control_runtime_get_auto_reconnect_enabled(void);
static bool wifi_control_runtime_get_connected(void);
static bool wifi_control_runtime_needs_disconnect_before_connect(void);
static bool wifi_control_runtime_should_suppress_disconnect(
    bool *reconnect_after_disconnect);
static wifi_control_state_t wifi_control_runtime_get_state(void);
static void wifi_control_copy_credentials(char *ssid_dst, size_t ssid_dst_len,
                                          char *password_dst,
                                          size_t password_dst_len,
                                          const char *ssid,
                                          const char *password);
static void wifi_control_event_handler(void *arg, esp_event_base_t event_base,
                                       int32_t event_id, void *event_data);

/**
 * @brief 将运行状态统一写回内部上下文。
 *
 * 这个小函数把状态变更集中在一个出口，避免事件回调里到处散写同一组字段。
 *
 * @param[in] state 目标状态。
 * @return 无返回值。
 */
static void wifi_control_runtime_set_state(wifi_control_state_t state)
{
    taskENTER_CRITICAL(&s_runtime_lock);
    s_runtime.state = state;
    taskEXIT_CRITICAL(&s_runtime_lock);
}

/**
 * @brief 查询当前是否已经完成初始化。
 *
 * 这里通过统一的临界区读取初始化门闩，避免公开 API 与事件线程在并发场景下
 * 直接裸读共享标志。
 *
 * @return true 表示初始化已经完成。
 */
static bool wifi_control_runtime_is_initialized(void)
{
    bool initialized = false;

    taskENTER_CRITICAL(&s_runtime_lock);
    initialized = s_runtime.initialized;
    taskEXIT_CRITICAL(&s_runtime_lock);

    return initialized;
}

/**
 * @brief 尝试占用初始化门闩。
 *
 * 只有一个调用者允许进入真正的 init 流程，其他并发调用者会等待当前初始化结束。
 *
 * @return true 表示当前调用者拿到了 init 门闩；false 表示已有其他线程在初始化。
 */
static bool wifi_control_runtime_begin_init(void)
{
    bool can_begin = false;

    taskENTER_CRITICAL(&s_runtime_lock);
    if (!s_runtime.initialized && !s_runtime.init_in_progress)
    {
        s_runtime.init_in_progress = true;
        can_begin = true;
    }
    taskEXIT_CRITICAL(&s_runtime_lock);

    return can_begin;
}

/**
 * @brief 释放初始化门闩。
 * @return 无返回值。
 */
static void wifi_control_runtime_end_init(void)
{
    taskENTER_CRITICAL(&s_runtime_lock);
    s_runtime.init_in_progress = false;
    taskEXIT_CRITICAL(&s_runtime_lock);
}

/**
 * @brief 写回初始化完成标志。
 *
 * 初始化是否完成同样属于共享运行态，统一通过临界区更新，避免和并发 init 判定
 * 形成不一致的观察结果。
 *
 * @param[in] initialized true 表示初始化成功完成。
 * @return 无返回值。
 */
static void wifi_control_runtime_set_initialized(bool initialized)
{
    taskENTER_CRITICAL(&s_runtime_lock);
    s_runtime.initialized = initialized;
    taskEXIT_CRITICAL(&s_runtime_lock);
}

/**
 * @brief 等待其他线程完成初始化门闩。
 *
 * 该等待只用于并发进入场景，避免重复 init / register / start。
 * 当前线程不会持有临界区，因此不会阻塞事件回调对运行态的读取。
 *
 * @return 无返回值。
 */
static void wifi_control_runtime_wait_for_init_completion(void)
{
    while (true)
    {
        bool init_in_progress = false;

        taskENTER_CRITICAL(&s_runtime_lock);
        init_in_progress = s_runtime.init_in_progress;
        taskEXIT_CRITICAL(&s_runtime_lock);

        if (!init_in_progress)
        {
            return;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/**
 * @brief 统一写回 STA 是否已经连上。
 * @param[in] connected 目标状态。
 * @return 无返回值。
 */
static void wifi_control_runtime_set_connected(bool connected)
{
    taskENTER_CRITICAL(&s_runtime_lock);
    s_runtime.connected = connected;
    taskEXIT_CRITICAL(&s_runtime_lock);
}

/**
 * @brief 统一写回自动重试计数。
 * @param[in] retry_count 目标重试计数。
 * @return 无返回值。
 */
static void wifi_control_runtime_set_retry_count(uint8_t retry_count)
{
    taskENTER_CRITICAL(&s_runtime_lock);
    s_runtime.retry_count = retry_count;
    taskEXIT_CRITICAL(&s_runtime_lock);
}

/**
 * @brief 统一写回自动重连开关。
 * @param[in] enabled 目标开关值。
 * @return 无返回值。
 */
static void wifi_control_runtime_set_auto_reconnect_enabled(bool enabled)
{
    taskENTER_CRITICAL(&s_runtime_lock);
    s_runtime.auto_reconnect_enabled = enabled;
    taskEXIT_CRITICAL(&s_runtime_lock);
}

/**
 * @brief 统一写回断开抑制标志。
 *
 * 这两个标志必须成对更新，避免事件线程读到半套状态。
 *
 * @param[in] suppressed 是否抑制后续自动重连判断。
 * @param[in] reconnect_after_disconnect 是否准备在断开后立刻进入新连接。
 * @return 无返回值。
 */
static void wifi_control_runtime_set_reconnect_markers(
    bool suppressed, bool reconnect_after_disconnect)
{
    taskENTER_CRITICAL(&s_runtime_lock);
    s_runtime.reconnect_suppressed = suppressed;
    s_runtime.reconnect_after_disconnect = reconnect_after_disconnect;
    taskEXIT_CRITICAL(&s_runtime_lock);
}

/**
 * @brief 清空断开抑制标志。
 * @return 无返回值。
 */
static void wifi_control_runtime_clear_reconnect_markers(void)
{
    wifi_control_runtime_set_reconnect_markers(false, false);
}

/**
 * @brief 读取自动重连开关。
 * @return true 表示允许自动重连。
 */
static bool wifi_control_runtime_get_auto_reconnect_enabled(void)
{
    bool enabled = false;

    taskENTER_CRITICAL(&s_runtime_lock);
    enabled = s_runtime.auto_reconnect_enabled;
    taskEXIT_CRITICAL(&s_runtime_lock);
    return enabled;
}

/**
 * @brief 读取 STA 连接态。
 * @return true 表示已经拿到有效 IP。
 */
static bool wifi_control_runtime_get_connected(void)
{
    bool connected = false;

    taskENTER_CRITICAL(&s_runtime_lock);
    connected = s_runtime.connected;
    taskEXIT_CRITICAL(&s_runtime_lock);
    return connected;
}

/**
 * @brief 判断新连接请求前是否需要先显式断开旧 STA 状态。
 *
 * 冷启动或上一次已经进入失败/断开态时，不应再下发一次“重连前断开”。
 * 否则 ESP-IDF 后续上报的首次真实断连事件可能被 suppress 标记误判为
 * 清理流程的一部分，导致开机 latest Wi-Fi 首次失败后没有进入自动重试。
 *
 * @return true 表示当前存在已连接或正在连接的旧 STA 状态，需要先断开。
 */
static bool wifi_control_runtime_needs_disconnect_before_connect(void)
{
    bool connected = false;
    wifi_control_state_t state = WIFI_CONTROL_STATE_IDLE;

    taskENTER_CRITICAL(&s_runtime_lock);
    connected = s_runtime.connected;
    state = s_runtime.state;
    taskEXIT_CRITICAL(&s_runtime_lock);

    return connected || state == WIFI_CONTROL_STATE_CONNECTED ||
           state == WIFI_CONTROL_STATE_CONNECTING;
}

/**
 * @brief 判断当前是否处于“显式断开后立刻重连”的抑制窗口。
 * @param[out] reconnect_after_disconnect 是否准备立即进入新连接。
 * @return true 表示当前断开事件应该被当成连接切换流程的一部分。
 */
static bool wifi_control_runtime_should_suppress_disconnect(
    bool *reconnect_after_disconnect)
{
    bool suppressed = false;
    bool reconnecting = false;

    taskENTER_CRITICAL(&s_runtime_lock);
    suppressed = s_runtime.reconnect_suppressed;
    reconnecting = s_runtime.reconnect_after_disconnect;
    taskEXIT_CRITICAL(&s_runtime_lock);

    if (reconnect_after_disconnect != NULL)
    {
        *reconnect_after_disconnect = reconnecting;
    }
    return suppressed;
}

/**
 * @brief 读取当前 Wi-Fi STA 状态。
 * @return 运行状态枚举。
 */
static wifi_control_state_t wifi_control_runtime_get_state(void)
{
    wifi_control_state_t state = WIFI_CONTROL_STATE_IDLE;

    taskENTER_CRITICAL(&s_runtime_lock);
    state = s_runtime.state;
    taskEXIT_CRITICAL(&s_runtime_lock);
    return state;
}

/**
 * @brief 确保 NVS 已可用。
 *
 * 这里不保存任何 Wi-Fi 凭据，只是保证 Wi-Fi 驱动所需的基础 NVS 环境存在。
 *
 * @return `ESP_OK` 表示 NVS 可用；其他错误表示初始化或擦除失败。
 */
static esp_err_t wifi_control_ensure_nvs_ready(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS 初始化需要擦除，正在重试");
        ret = nvs_flash_erase();
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "擦除 NVS 失败: %s", esp_err_to_name(ret));
            return ret;
        }
        ret = nvs_flash_init();
    }

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "初始化 NVS 失败: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}

/**
 * @brief 确保 Wi-Fi 事件栈和 STA 默认网卡已准备好。
 *
 * 这里接受 `ESP_ERR_INVALID_STATE` 作为“已经初始化过”的正常结果，避免重复 init 直接报错。
 *
 * @return `ESP_OK` 表示准备完成；其他错误表示事件栈或 netif 创建失败。
 */
static esp_err_t wifi_control_ensure_stack_ready(void)
{
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "初始化 esp_netif 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "创建默认事件循环失败: %s", esp_err_to_name(ret));
        return ret;
    }

    if (s_sta_netif == NULL)
    {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (s_sta_netif == NULL)
        {
            ESP_LOGE(TAG, "创建默认 STA netif 失败");
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

/**
 * @brief 回收初始化过程中已经拿到的资源。
 *
 * 这条路径只做 best-effort cleanup，目的是让下一次 `wifi_control_init()` 能干净重试。
 *
 * @return `ESP_OK` 表示清理流程完成；其他返回值表示某个收尾步骤失败。
 */
static esp_err_t wifi_control_cleanup_partial_init(void)
{
    esp_err_t cleanup_ret = ESP_OK;

    if (s_wifi_started)
    {
        esp_err_t stop_ret = esp_wifi_stop();
        if (cleanup_ret == ESP_OK && stop_ret != ESP_OK)
        {
            cleanup_ret = stop_ret;
        }
        s_wifi_started = false;
    }

    if (s_wifi_ip_event_handler_registered)
    {
        esp_err_t unregister_ret = esp_event_handler_unregister(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_control_event_handler);
        if (cleanup_ret == ESP_OK && unregister_ret != ESP_OK)
        {
            cleanup_ret = unregister_ret;
        }
        s_wifi_ip_event_handler_registered = false;
    }

    if (s_wifi_event_handler_registered)
    {
        esp_err_t unregister_ret =
            esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                         &wifi_control_event_handler);
        if (cleanup_ret == ESP_OK && unregister_ret != ESP_OK)
        {
            cleanup_ret = unregister_ret;
        }
        s_wifi_event_handler_registered = false;
    }

    if (s_wifi_driver_initialized)
    {
        esp_err_t deinit_ret = esp_wifi_deinit();
        if (cleanup_ret == ESP_OK && deinit_ret != ESP_OK)
        {
            cleanup_ret = deinit_ret;
        }
        s_wifi_driver_initialized = false;
    }

    wifi_control_runtime_set_connected(false);
    wifi_control_runtime_set_retry_count(0);
    wifi_control_runtime_clear_reconnect_markers();
    wifi_control_runtime_set_state(WIFI_CONTROL_STATE_IDLE);

    return cleanup_ret;
}

/**
 * @brief 通过显式断开动作，阻止下一次 DISCONNECTED 事件触发自动重连。
 *
 * 该辅助函数既服务于 `wifi_control_disconnect()`，也服务于显式重连前的清理步骤。
 *
 * @param[in] reconnect_after_disconnect true 表示断开后马上准备进入一次新的连接请求。
 * @return `ESP_OK` 表示断开请求已下发或本就未连接；其他错误表示底层断开失败。
 */
static esp_err_t wifi_control_request_disconnect(bool reconnect_after_disconnect)
{
    esp_err_t ret = ESP_OK;

    wifi_control_runtime_set_reconnect_markers(true,
                                               reconnect_after_disconnect);

    ret = esp_wifi_disconnect();
    if (ret == ESP_ERR_WIFI_NOT_CONNECT)
    {
        wifi_control_runtime_clear_reconnect_markers();
        wifi_control_runtime_set_connected(false);
        wifi_control_runtime_set_state(WIFI_CONTROL_STATE_DISCONNECTED);
        return ESP_OK;
    }

    if (ret != ESP_OK)
    {
        wifi_control_runtime_clear_reconnect_markers();
    }

    return ret;
}

/**
 * @brief 安全复制 SSID 和密码。
 *
 * 该辅助函数统一处理空指针回退，避免多个路径各自拼接凭据字符串。
 *
 * @param[out] ssid_dst SSID 输出缓冲区。
 * @param[in] ssid_dst_len SSID 输出缓冲区长度。
 * @param[out] password_dst 密码输出缓冲区。
 * @param[in] password_dst_len 密码输出缓冲区长度。
 * @param[in] ssid 源 SSID。
 * @param[in] password 源密码。
 * @return 无返回值。
 */
static void wifi_control_copy_credentials(char *ssid_dst, size_t ssid_dst_len,
                                          char *password_dst,
                                          size_t password_dst_len,
                                          const char *ssid,
                                          const char *password)
{
    snprintf(ssid_dst, ssid_dst_len, "%s", ssid != NULL ? ssid : "");
    snprintf(password_dst, password_dst_len, "%s",
             password != NULL ? password : "");
}

/**
 * @brief Wi-Fi/IP 事件统一处理入口。
 *
 * 这里只翻译 STA 运行态，不携带 provisioning 语义，也不保存凭据。
 *
 * @param[in] arg 未使用。
 * @param[in] event_base 事件基类。
 * @param[in] event_id 事件 ID。
 * @param[in] event_data 事件附带数据。
 * @return 无返回值。
 */
static void wifi_control_event_handler(void *arg, esp_event_base_t event_base,
                                       int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA 已启动，等待显式连接请求");
            wifi_control_runtime_set_state(WIFI_CONTROL_STATE_DISCONNECTED);
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "STA 已连接到路由器");
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
        {
            const wifi_event_sta_disconnected_t *disconnected =
                (const wifi_event_sta_disconnected_t *)event_data;
            const uint8_t reason =
                disconnected != NULL ? disconnected->reason : 0U;
            bool reconnecting = false;
            const bool suppress_disconnect =
                wifi_control_runtime_should_suppress_disconnect(&reconnecting);
            const bool auto_reconnect_enabled =
                wifi_control_runtime_get_auto_reconnect_enabled();
            uint8_t retry_count = 0;

            taskENTER_CRITICAL(&s_runtime_lock);
            s_runtime.connected = false;
            retry_count = s_runtime.retry_count;
            taskEXIT_CRITICAL(&s_runtime_lock);

            ESP_LOGW(TAG,
                     "STA 断开连接: reason=%u suppress=%d auto_reconnect=%d retry=%u",
                     (unsigned)reason, suppress_disconnect ? 1 : 0,
                     auto_reconnect_enabled ? 1 : 0, (unsigned)retry_count);

            if (suppress_disconnect)
            {
                wifi_control_runtime_clear_reconnect_markers();
                wifi_control_runtime_set_state(reconnecting
                                                   ? WIFI_CONTROL_STATE_CONNECTING
                                                   : WIFI_CONTROL_STATE_DISCONNECTED);
                break;
            }

            if (auto_reconnect_enabled && retry_count < WIFI_CONTROL_MAX_RETRY)
            {
                esp_err_t ret = esp_wifi_connect();
                if (ret == ESP_OK)
                {
                    wifi_control_runtime_set_retry_count(retry_count + 1);
                    wifi_control_runtime_set_state(WIFI_CONTROL_STATE_CONNECTING);
                    ESP_LOGI(TAG, "自动重连 Wi-Fi... (%u/%u)",
                             (unsigned)(retry_count + 1),
                             (unsigned)WIFI_CONTROL_MAX_RETRY);
                }
                else
                {
                    ESP_LOGW(TAG, "自动重连失败: %s", esp_err_to_name(ret));
                    wifi_control_runtime_set_state(WIFI_CONTROL_STATE_CONNECT_FAIL);
                }
                break;
            }

            if (auto_reconnect_enabled)
            {
                wifi_control_runtime_set_state(WIFI_CONTROL_STATE_CONNECT_FAIL);
            }
            else
            {
                wifi_control_runtime_set_state(WIFI_CONTROL_STATE_DISCONNECTED);
            }
            break;
        }
        default:
            break;
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        if (event != NULL)
        {
            ESP_LOGI(TAG, "获取到 STA IP: " IPSTR, IP2STR(&event->ip_info.ip));
        }
        wifi_control_runtime_set_connected(true);
        wifi_control_runtime_set_retry_count(0);
        wifi_control_runtime_clear_reconnect_markers();
        wifi_control_runtime_set_state(WIFI_CONTROL_STATE_CONNECTED);
    }
}

/**
 * @brief 初始化 Wi-Fi STA runtime control。
 *
 * 这里只初始化 Wi-Fi 驱动、事件栈和 STA 默认网卡，不会触发 BLE/AP 语义。
 *
 * @return `ESP_OK` 表示初始化成功；其他错误表示底层 Wi-Fi、事件或网络栈初始化失败。
 */
esp_err_t wifi_control_init(void)
{
    esp_err_t ret = ESP_OK;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    if (wifi_control_runtime_is_initialized())
    {
        return ESP_OK;
    }

    while (!wifi_control_runtime_begin_init())
    {
        wifi_control_runtime_wait_for_init_completion();
        if (wifi_control_runtime_is_initialized())
        {
            return ESP_OK;
        }
    }

    ret = wifi_control_ensure_nvs_ready();
    if (ret != ESP_OK)
    {
        goto fail;
    }

    ret = wifi_control_ensure_stack_ready();
    if (ret != ESP_OK)
    {
        goto fail;
    }

    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "初始化 Wi-Fi 驱动失败: %s", esp_err_to_name(ret));
        goto fail;
    }
    s_wifi_driver_initialized = true;

    ret = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "设置 Wi-Fi 存储策略失败: %s", esp_err_to_name(ret));
        goto fail;
    }

    ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     &wifi_control_event_handler, NULL);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "注册 Wi-Fi 事件失败: %s", esp_err_to_name(ret));
        goto fail;
    }
    s_wifi_event_handler_registered = true;

    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     &wifi_control_event_handler, NULL);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "注册 IP 事件失败: %s", esp_err_to_name(ret));
        goto fail;
    }
    s_wifi_ip_event_handler_registered = true;

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "设置 STA 模式失败: %s", esp_err_to_name(ret));
        goto fail;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "启动 Wi-Fi 失败: %s", esp_err_to_name(ret));
        goto fail;
    }
    s_wifi_started = true;

    wifi_control_runtime_set_initialized(true);
    wifi_control_runtime_set_connected(false);
    wifi_control_runtime_clear_reconnect_markers();
    wifi_control_runtime_set_retry_count(0);
    wifi_control_runtime_set_state(WIFI_CONTROL_STATE_DISCONNECTED);
    wifi_control_runtime_end_init();

    ESP_LOGI(TAG, "Wi-Fi STA runtime control 初始化完成");
    return ESP_OK;

fail:
    (void)wifi_control_cleanup_partial_init();
    wifi_control_runtime_end_init();
    return ret;
}

/**
 * @brief 使用显式 SSID 和密码发起 STA 连接。
 *
 * 该接口只控制 STA 运行时，不保存凭据，也不迁移任何 provisioning 语义。
 *
 * @param[in] ssid 目标 SSID。
 * @param[in] password 目标密码。
 * @return `ESP_OK` 表示连接请求已下发；其他错误表示参数非法或底层配置失败。
 */
esp_err_t wifi_control_connect(const char *ssid, const char *password)
{
    esp_err_t ret = ESP_OK;
    wifi_config_t wifi_config = {0};
    bool needs_disconnect_before_connect = false;

    if (ssid == NULL || password == NULL || ssid[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    ret = wifi_control_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    needs_disconnect_before_connect =
        wifi_control_runtime_needs_disconnect_before_connect();
    wifi_control_runtime_set_state(WIFI_CONTROL_STATE_CONNECTING);
    wifi_control_runtime_set_retry_count(0);
    ESP_LOGI(TAG, "connect request: ssid=%s pre_disconnect=%d", ssid,
             needs_disconnect_before_connect ? 1 : 0);

    if (needs_disconnect_before_connect)
    {
        ret = wifi_control_request_disconnect(true);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "重连前断开当前 STA 失败: %s", esp_err_to_name(ret));
            return ret;
        }
    }
    else
    {
        /*
         * 冷启动 latest Wi-Fi 连接不需要清理旧连接；明确清空 suppress 标记，
         * 确保第一次认证失败能进入自动重连分支。
         */
        wifi_control_runtime_clear_reconnect_markers();
        wifi_control_runtime_set_connected(false);
    }

    wifi_control_copy_credentials((char *)wifi_config.sta.ssid,
                                  sizeof(wifi_config.sta.ssid),
                                  (char *)wifi_config.sta.password,
                                  sizeof(wifi_config.sta.password), ssid,
                                  password);

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "设置 STA 配置失败: %s", esp_err_to_name(ret));
        wifi_control_runtime_clear_reconnect_markers();
        wifi_control_runtime_set_state(WIFI_CONTROL_STATE_CONNECT_FAIL);
        return ret;
    }

    ret = esp_wifi_connect();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "发起 STA 连接失败: %s", esp_err_to_name(ret));
        wifi_control_runtime_clear_reconnect_markers();
        wifi_control_runtime_set_state(WIFI_CONTROL_STATE_CONNECT_FAIL);
        return ret;
    }

    ESP_LOGI(TAG, "已下发 STA 连接请求: ssid=%s", ssid);
    return ESP_OK;
}

/**
 * @brief 主动断开当前 STA 连接。
 *
 * 若当前本就未连接，也会返回 `ESP_OK`，以便上层把它视为幂等控制操作。
 *
 * @return `ESP_OK` 表示断开请求已处理；其他错误表示底层 Wi-Fi 句柄不可用。
 */
esp_err_t wifi_control_disconnect(void)
{
    esp_err_t ret = ESP_OK;

    ret = wifi_control_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    wifi_control_runtime_set_retry_count(0);
    ret = wifi_control_request_disconnect(false);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "断开 STA 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "已下发 STA 断开请求");
    return ESP_OK;
}

/**
 * @brief 设置断线后的自动重连开关。
 *
 * 关闭后，Wi-Fi 断开事件只会更新状态，不再自动调用 `esp_wifi_connect()`。
 *
 * @param[in] enabled true 表示允许自动重连；false 表示关闭自动重连。
 * @return 无返回值。
 */
void wifi_control_set_auto_reconnect_enabled(bool enabled)
{
    wifi_control_runtime_set_auto_reconnect_enabled(enabled);
    ESP_LOGI(TAG, "auto reconnect enabled=%d", enabled ? 1 : 0);
}

/**
 * @brief 查询当前是否允许自动重连。
 * @return true 表示允许自动重连。
 */
bool wifi_control_is_auto_reconnect_enabled(void)
{
    return wifi_control_runtime_get_auto_reconnect_enabled();
}

/**
 * @brief 查询当前 STA 是否已连接。
 * @return true 表示已经拿到有效 IP。
 */
bool wifi_control_is_connected(void)
{
    return wifi_control_runtime_get_connected();
}

/**
 * @brief 获取当前 STA 的 IPv4 字符串。
 *
 * 该接口只读当前运行态，不会触发任何连接或 provisioning 行为。
 *
 * @param[out] ip_str 输出缓冲区。
 * @param[in] ip_str_len 输出缓冲区长度，至少 16 字节。
 * @return `ESP_OK` 表示成功；其他错误表示当前未连接或参数非法。
 */
esp_err_t wifi_control_get_ip(char *ip_str, size_t ip_str_len)
{
    esp_netif_ip_info_t ip_info = {0};

    if (!wifi_control_runtime_get_connected() || ip_str == NULL ||
        ip_str_len < 16)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_t *sta_netif = s_sta_netif;
    if (sta_netif == NULL)
    {
        sta_netif = esp_netif_get_handle_from_ifkey(WIFI_CONTROL_STA_IFKEY);
    }
    if (sta_netif == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK)
    {
        snprintf(ip_str, ip_str_len, IPSTR, IP2STR(&ip_info.ip));
        return ESP_OK;
    }

    return ESP_ERR_INVALID_STATE;
}

/**
 * @brief 设置当前 Wi-Fi STA 的省电模式。
 *
 * 这里复用已经初始化好的 STA runtime control，只切换 Wi-Fi modem 的省电策略，
 * 不改变连接状态机，也不承担任何 provisioning 语义。
 *
 * @param[in] enabled true 表示开启 `WIFI_PS_MIN_MODEM`；false 表示切回 `WIFI_PS_NONE`。
 * @return `ESP_OK` 表示设置成功；其他错误表示底层 Wi-Fi 驱动尚未就绪或配置失败。
 */
esp_err_t wifi_control_set_power_save(bool enabled)
{
    esp_err_t ret = wifi_control_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    /* 当前项目只需要“省电 / 不省电”两态，因此固定映射到 `MIN_MODEM` 与 `NONE`。 */
    ret = esp_wifi_set_ps(enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "set power save failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "power save enabled=%d", enabled ? 1 : 0);
    return ESP_OK;
}

/**
 * @brief 查询当前 Wi-Fi STA 的运行状态。
 * @return 运行状态枚举快照。
 */
wifi_control_state_t wifi_control_get_state(void)
{
    return wifi_control_runtime_get_state();
}
