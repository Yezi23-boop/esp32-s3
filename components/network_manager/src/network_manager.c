/**
 * @file network_manager.c
 * @brief `network_manager` 统一网络门面。
 */

#include "network_manager.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "ap_portal_adapter.h"
#include "ble_control.h"
#include "ble_presence.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "network_provisioning_adapter.h"
#include "wifi_control.h"

/** @brief 当前网络门面的主状态快照；高频只读路径允许无锁读取这个值。 */
static volatile network_manager_state_t s_state = NETWORK_MANAGER_STATE_IDLE;
/** @brief 当前默认 provisioning transport。 */
static network_manager_provisioning_transport_t s_default_transport =
    NETWORK_MANAGER_PROVISIONING_TRANSPORT_BLE;
/** @brief 后台监控任务句柄，用来自动推进 latest 失败后的回退流程。 */
static TaskHandle_t s_monitor_task = NULL;
/** @brief `network_manager` 状态门的静态 mutex 存储。 */
static StaticSemaphore_t s_manager_mutex_buffer;
/** @brief 串行化 `network_manager` 状态迁移与后台监控刷新的 mutex。 */
static SemaphoreHandle_t s_manager_mutex = NULL;
/** @brief 保护 `network_manager` mutex 创建路径的最小临界区锁。 */
static portMUX_TYPE s_manager_bootstrap_lock = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t network_manager_ensure_mutex(void);
static esp_err_t network_manager_sync_ip(network_manager_status_t *status);
static void network_manager_sync_ble_active_from_adapter(void);
static esp_err_t network_manager_sync_ble_presence(bool allow_start);
static esp_err_t network_manager_stop_active_transport(void);
static esp_err_t network_manager_start_selected_transport(void);
static esp_err_t network_manager_start_selected_transport_auto(void);
static esp_err_t network_manager_start_explicit_transport(
    network_manager_provisioning_transport_t transport);
static void network_manager_refresh_runtime_state(bool sync_ble_presence);
static esp_err_t network_manager_connect_entry(
    const network_credentials_entry_t *entry,
    bool allow_transport_fallback);
static esp_err_t network_manager_ensure_monitor_task(void);
static void network_manager_task(void *arg);
static void network_manager_clear_pending_provisioned_entry(void);
static void network_manager_drop_pending_provisioned_entry_for_manual_action(void);
static void network_manager_on_provisioning_event(
    network_provisioning_adapter_event_t event,
    const wifi_sta_config_t *wifi_sta_config, void *user_ctx);

/** @brief 最近一次通过 provisioning 收到、等待在连网成功后写入 recent list 的 Wi-Fi 记录。 */
static network_credentials_entry_t s_pending_provisioned_entry = {0};
/** @brief 当前是否存在待在连网成功后写入 recent list 的 provisioning 记录。 */
static bool s_pending_provisioned_entry_valid = false;
/** @brief 当前是否已经由 `network_manager` 发起了这条 provisioning Wi-Fi 记录的连接请求。 */
static bool s_pending_provisioned_entry_connecting = false;

/**
 * @brief 清理当前 pending provisioning Wi-Fi 记录。
 *
 * 该辅助函数统一收口 provisioning 过程中暂存的凭据缓存，避免 recent 落盘失败、连接失败
 * 或新的 provisioning 会话开始时把旧凭据残留到后续状态机里。
 *
 * @return 无返回值。
 */
static void network_manager_clear_pending_provisioned_entry(void)
{
    s_pending_provisioned_entry_valid = false;
    s_pending_provisioned_entry_connecting = false;
    memset(&s_pending_provisioned_entry, 0, sizeof(s_pending_provisioned_entry));
}

/**
 * @brief 在用户显式切换联网动作前丢弃上一轮 provisioning 暂存凭据。
 *
 * `pending provisioning entry` 只应该服务“当前这轮门户/BLE 下发的凭据，等待 STA
 * 真正连上后再写 recent”。一旦用户显式点击：
 * - `Use Saved Wi-Fi`
 * - `Disconnect`
 * - `Reprovision`
 *
 * 就说明用户已经放弃上一轮 provisioning 结果，继续保留旧 pending 会带来两个风险：
 * 1. 旧 STA 结果迟到时，可能把已经放弃的 SSID 误提升为最新 recent；
 * 2. UI 看起来是在执行新动作，底层 recent 落盘却仍受旧凭据影响。
 *
 * 该辅助函数只在已经持有 `s_manager_mutex` 的手动动作路径下调用。
 *
 * @return 无返回值。
 */
static void network_manager_drop_pending_provisioned_entry_for_manual_action(void)
{
    if (!s_pending_provisioned_entry_valid &&
        !s_pending_provisioned_entry_connecting)
    {
        return;
    }

    network_manager_clear_pending_provisioned_entry();
}

/**
 * @brief 确保 `network_manager` 状态 mutex 已创建。
 *
 * 该 mutex 用来串行化后台监控任务与外部 API 对主状态机的并发读写。
 *
 * @return `ESP_OK` 表示 mutex 可用；其他错误表示创建失败。
 */
static esp_err_t network_manager_ensure_mutex(void)
{
    if (s_manager_mutex != NULL)
    {
        return ESP_OK;
    }

    taskENTER_CRITICAL(&s_manager_bootstrap_lock);
    if (s_manager_mutex == NULL)
    {
        s_manager_mutex = xSemaphoreCreateMutexStatic(&s_manager_mutex_buffer);
    }
    taskEXIT_CRITICAL(&s_manager_bootstrap_lock);

    return s_manager_mutex != NULL ? ESP_OK : ESP_FAIL;
}

/**
 * @brief 将当前 IPv4 地址同步到状态快照。
 *
 * 若当前没有有效 IP，本函数会把输出缓冲区清空，而不是把旧 IP 残留给 UI。
 *
 * @param[out] status 输出状态结构。
 * @return `ESP_OK` 表示已成功同步；`ESP_ERR_INVALID_ARG` 表示参数非法。
 */
static esp_err_t network_manager_sync_ip(network_manager_status_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(status->ip, 0, sizeof(status->ip));
    if (wifi_control_is_connected())
    {
        (void)wifi_control_get_ip(status->ip, sizeof(status->ip));
    }

    return ESP_OK;
}

/**
 * @brief 用 adapter 当前 transport 同步 BLE active 状态。
 *
 * 这样 `ble_control_is_active()` 始终表达“BLE provisioning transport 是否真的在跑”，
 * 而不是只表达用户偏好。
 *
 * @return 无返回值。
 */
static void network_manager_sync_ble_active_from_adapter(void)
{
    const network_provisioning_transport_t transport =
        network_provisioning_adapter_get_transport();

    (void)ble_control_set_active(
        transport == NETWORK_PROVISIONING_TRANSPORT_BLE);
}

/**
 * @brief 按当前 owner 状态同步普通 BLE presence 广播。
 *
 * 主界面蓝牙开关的语义是“允许普通蓝牙可发现”；Wi-Fi 配网页面的 BLE
 * Provision 才是官方 provisioning owner。这里集中处理两者互斥：
 * - BLE provisioning 正在运行时，presence 必须停止，避免抢 NimBLE host；
 * - BLE enabled 且没有 BLE provisioning 时，presence 可以运行，和 Wi-Fi STA 并存；
 * - BLE disabled 时，presence 必须停止。
 *
 * @return `ESP_OK` 表示同步成功；其他错误表示 presence 启停失败。
 *
 * @note 调用方通常已经持有 `s_manager_mutex`，本函数自身不会访问 manager 共享字段。
 */
static esp_err_t network_manager_sync_ble_presence(bool allow_start)
{
    esp_err_t ret = ESP_OK;
    const bool ble_transport_active =
        network_provisioning_adapter_is_active() &&
        network_provisioning_adapter_get_transport() ==
            NETWORK_PROVISIONING_TRANSPORT_BLE;

    if (!ble_control_is_enabled() || ble_transport_active)
    {
        return ble_presence_stop();
    }

    if (!allow_start)
    {
        return ESP_OK;
    }

    ret = ble_presence_start();
    if (ret != ESP_OK)
    {
        (void)ble_control_set_active(false);
        (void)ble_control_set_enabled(false);
    }
    return ret;
}

/**
 * @brief 停止当前 active provisioning transport。
 *
 * 统一收口 stop/deinit 失败路径，避免上层继续误以为 transport 已经停干净。
 *
 * @return `ESP_OK` 表示 transport 已停止或本就空闲；其他错误表示 stop/deinit 失败。
 */
static esp_err_t network_manager_stop_active_transport(void)
{
    esp_err_t ret = ESP_OK;
    const network_provisioning_transport_t active_transport =
        network_provisioning_adapter_get_transport();

    if (!network_provisioning_adapter_is_active())
    {
        (void)ble_control_set_active(false);
        return ESP_OK;
    }

    ret = network_provisioning_adapter_stop();
    if (ret == ESP_OK)
    {
        if (active_transport == NETWORK_PROVISIONING_TRANSPORT_SOFTAP)
        {
            /* SoftAP 路径使用自定义门户 HTTPD 作为官方 provisioning 的宿主；
             * transport 停止后要同步收口门户服务，避免下一轮切换时继续持有旧 handle。 */
            ret = ap_portal_adapter_stop();
        }
    }

    if (ret == ESP_OK)
    {
        (void)ble_control_set_active(false);
        ret = network_manager_sync_ble_presence(false);
    }
    else
    {
        s_state = NETWORK_MANAGER_STATE_ERROR;
    }

    return ret;
}

/**
 * @brief 按当前默认 transport 启动 provisioning。
 *
 * 这条路径只承接 `BLE / SOFTAP` 两种 transport，不再保留历史 `AUTO` 语义。
 *
 * @return `ESP_OK` 表示 provisioning transport 已启动；其他错误表示启动失败。
 */
static esp_err_t network_manager_start_selected_transport(void)
{
    esp_err_t ret = ESP_OK;

    switch (s_default_transport)
    {
    case NETWORK_MANAGER_PROVISIONING_TRANSPORT_BLE:
        if (!ble_control_is_enabled())
        {
            s_state = NETWORK_MANAGER_STATE_ERROR;
            return ESP_ERR_INVALID_STATE;
        }

        ret = ble_presence_stop();
        if (ret != ESP_OK)
        {
            s_state = NETWORK_MANAGER_STATE_ERROR;
            return ret;
        }

        ret = network_provisioning_adapter_start_ble();
        if (ret == ESP_OK)
        {
            (void)ble_control_set_active(true);
            s_state = NETWORK_MANAGER_STATE_PROVISIONING_BLE;
        }
        else
        {
            (void)ble_control_set_active(false);
            s_state = NETWORK_MANAGER_STATE_ERROR;
        }
        return ret;

    case NETWORK_MANAGER_PROVISIONING_TRANSPORT_SOFTAP:
        /* SoftAP 与普通 BLE presence 共享片内资源；先确认 BLE host 已停止，
         * 再创建门户和 AP，避免两个 transport 并发初始化。 */
        ret = ble_presence_stop();
        if (ret != ESP_OK)
        {
            s_state = NETWORK_MANAGER_STATE_ERROR;
            return ret;
        }
        /* 先确保自定义门户 HTTPD 已创建，再把同一个 handle 复用给官方 SoftAP
         * provisioning。否则设备端即使起了 AP，也没有我们的页面资源和入口。 */
        ret = ap_portal_adapter_start();
        if (ret != ESP_OK)
        {
            s_state = NETWORK_MANAGER_STATE_ERROR;
            return ret;
        }

        ret = network_provisioning_adapter_start_softap();
        if (ret == ESP_OK)
        {
            (void)ble_control_set_active(false);
            (void)network_manager_sync_ble_presence(false);
            s_state = NETWORK_MANAGER_STATE_PROVISIONING_SOFTAP;
        }
        else
        {
            (void)ap_portal_adapter_stop();
            s_state = NETWORK_MANAGER_STATE_ERROR;
        }
        return ret;
    }

    s_state = NETWORK_MANAGER_STATE_ERROR;
    return ESP_ERR_INVALID_ARG;
}

/**
 * @brief 处理开机或 latest 失败后的自动配网路径。
 *
 * 当前产品语义要求“只有进入 Wi-Fi 管理页并点击 BLE Provision，才启动微信
 * 小程序配网”。因此自动路径不再启动默认 transport，而是停在合法空闲态，
 * 等待用户明确选择 BLE 配网或 AP 网页兜底。
 *
 * @return 固定返回 `ESP_OK`，表示自动路径已收敛到空闲态。
 */
static esp_err_t network_manager_start_selected_transport_auto(void)
{
    (void)ble_control_set_active(false);
    (void)network_manager_sync_ble_presence(false);
    s_state = NETWORK_MANAGER_STATE_IDLE;
    return ESP_OK;
}

/**
 * @brief 按用户在 Wi-Fi 配网页面点击的入口显式启动指定配网 transport。
 *
 * 显式入口和开机自动回退不同：它代表用户已经进入 Wi-Fi 管理页并选择
 * “BLE 配网”或“AP 兜底”，因此这里会暂停 STA 自动重连并收口旧的
 * provisioning 会话，但不会主动断开已经连上的 Wi-Fi。这样 BLE 配网广播
 * 可以和现有 Wi-Fi 连接短时间并存，直到用户真正下发新凭据。
 *
 * @param[in] transport 用户选择的配网 transport。
 * @return `ESP_OK` 表示 transport 已启动；其他错误表示当前状态或底层启动失败。
 */
static esp_err_t network_manager_start_explicit_transport(
    network_manager_provisioning_transport_t transport)
{
    esp_err_t ret = ESP_OK;

    if (transport > NETWORK_MANAGER_PROVISIONING_TRANSPORT_SOFTAP)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (transport == NETWORK_MANAGER_PROVISIONING_TRANSPORT_BLE &&
        !ble_control_is_enabled())
    {
        (void)ble_control_set_active(false);
        return ESP_ERR_INVALID_STATE;
    }

    network_manager_drop_pending_provisioned_entry_for_manual_action();
    wifi_control_set_auto_reconnect_enabled(false);

    ret = network_manager_stop_active_transport();
    if (ret != ESP_OK)
    {
        return ret;
    }

    s_default_transport = transport;
    return network_manager_start_selected_transport();
}

/**
 * @brief 根据底层组件当前运行态刷新主状态机。
 *
 * 当前阶段最关键的策略是：
 * - 若 provisioning transport 已启动，则把主状态同步到 BLE / SOFTAP；
 * - 若 provisioning 触发的 Wi-Fi 已真实连上，则在这里更新 recent list；
 * - 若 latest Wi-Fi 失败，则停在空闲态，等待用户从 Wi-Fi 管理页选择配网方式。
 *
 * @param[in] sync_ble_presence true 表示允许本次刷新同步普通 BLE presence 广播；
 *            false 表示只刷新状态快照，避免 getter 或 UI 轮询隐式启停 NimBLE。
 * @return 无返回值。
 */
static void network_manager_refresh_runtime_state(bool sync_ble_presence)
{
    const wifi_control_state_t wifi_state = wifi_control_get_state();
    const bool wifi_connected = wifi_control_is_connected();

    network_manager_sync_ble_active_from_adapter();
    if (sync_ble_presence)
    {
        (void)network_manager_sync_ble_presence(false);
    }

    if (wifi_connected || wifi_state == WIFI_CONTROL_STATE_CONNECTED)
    {
        if (s_pending_provisioned_entry_valid &&
            s_pending_provisioned_entry_connecting)
        {
            if (network_credentials_save_or_promote(
                    s_pending_provisioned_entry.ssid,
                    s_pending_provisioned_entry.password) == ESP_OK)
            {
                network_manager_clear_pending_provisioned_entry();
            }
            else
            {
                network_manager_clear_pending_provisioned_entry();
            }
        }

        s_state = NETWORK_MANAGER_STATE_CONNECTED;
        return;
    }

    if (s_pending_provisioned_entry_valid &&
        s_pending_provisioned_entry_connecting &&
        wifi_state == WIFI_CONTROL_STATE_CONNECT_FAIL)
    {
        network_manager_clear_pending_provisioned_entry();
    }

    if (network_provisioning_adapter_is_active())
    {
        const network_provisioning_transport_t transport =
            network_provisioning_adapter_get_transport();
        if (transport == NETWORK_PROVISIONING_TRANSPORT_BLE)
        {
            s_state = NETWORK_MANAGER_STATE_PROVISIONING_BLE;
            return;
        }
        if (transport == NETWORK_PROVISIONING_TRANSPORT_SOFTAP)
        {
            s_state = NETWORK_MANAGER_STATE_PROVISIONING_SOFTAP;
            return;
        }
    }

    if (s_state == NETWORK_MANAGER_STATE_CONNECTING_LATEST &&
        wifi_state == WIFI_CONTROL_STATE_CONNECT_FAIL)
    {
        (void)network_manager_start_selected_transport_auto();
    }
}

/**
 * @brief 用一条 recent Wi-Fi 记录发起连接。
 *
 * 该辅助函数统一负责 recent 网络连接前的 transport 收口和状态迁移。
 *
 * @param[in] entry 目标 recent Wi-Fi 记录。
 * @param[in] allow_transport_fallback true 表示允许在同步连接失败时直接进入 provisioning；
 *             false 表示把错误直接返回给调用方，由 UI 决定下一步动作。
 * @return `ESP_OK` 表示连接请求已下发；其他错误表示条目非法或连接失败。
 */
static esp_err_t network_manager_connect_entry(
    const network_credentials_entry_t *entry,
    bool allow_transport_fallback)
{
    esp_err_t ret = ESP_OK;

    if (entry == NULL || entry->ssid[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    ret = network_manager_stop_active_transport();
    if (ret != ESP_OK)
    {
        return ret;
    }

    wifi_control_set_auto_reconnect_enabled(true);

    ret = wifi_control_connect(entry->ssid, entry->password);
    if (ret == ESP_OK)
    {
        s_state = NETWORK_MANAGER_STATE_CONNECTING_LATEST;
    }
    else
    {
        /*
         * 这里要显式区分“自动 latest 尝试”和“用户手动点击 Use Saved Wi-Fi / recent 网络”：
         * - 自动入口允许在同步连接失败时立刻退回 provisioning；
         * - 手动入口应把失败直接暴露给 UI，避免用户只是想重试已保存凭据，却被悄悄带进配网流程。
         */
        s_state = NETWORK_MANAGER_STATE_ERROR;
        if (allow_transport_fallback)
        {
            s_state = NETWORK_MANAGER_STATE_CONNECTING_LATEST;
            return network_manager_start_selected_transport_auto();
        }
        return ret;
    }

    return ret;
}

/**
 * @brief 确保后台监控任务已创建。
 *
 * 该任务负责在 latest Wi-Fi 连接失败后自动把状态机推进到当前默认 transport，
 * 避免把“自动回退”做成“只有查询状态时才触发”。
 *
 * @return `ESP_OK` 表示监控任务已可用；其他错误表示任务创建失败。
 */
static esp_err_t network_manager_ensure_monitor_task(void)
{
    BaseType_t task_ret = pdPASS;

    if (s_monitor_task != NULL)
    {
        return ESP_OK;
    }

    /* 栈缩为 3072B：高压实测 free=3192B（78% 空闲），缩 1KB 仍有余量。
     * 栈迁 PSRAM：省 internal RAM，该任务不直接做 NVS/flash 写操作。 */
    task_ret = xTaskCreateWithCaps(network_manager_task, "network_mgr", 3072, NULL, 5,
                                   &s_monitor_task, MALLOC_CAP_SPIRAM);
    return task_ret == pdPASS ? ESP_OK : ESP_FAIL;
}

/**
 * @brief `network_manager` 后台监控任务。
 *
 * 该任务周期性刷新底层运行态，确保 latest 连接失败后能自动回退到已设置的
 * provisioning transport，而不是依赖调用方主动轮询状态。
 *
 * @param[in] arg 未使用。
 * @return 无返回值。
 */
static void network_manager_task(void *arg)
{
    (void)arg;

    while (true)
    {
        if (s_manager_mutex != NULL &&
            xSemaphoreTake(s_manager_mutex, portMAX_DELAY) == pdTRUE)
        {
            network_manager_refresh_runtime_state(true);
            xSemaphoreGive(s_manager_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

/**
 * @brief 处理来自 adapter 的 provisioning 事件。
 *
 * 当前阶段只承接“收到凭据”事件；真正的连网成功/失败改由 `wifi_control` 真实运行态判定，
 * 用于把 recent Wi-Fi 列表的更新时间收敛到“真正连上后”。
 *
 * @param[in] event adapter 事件类型。
 * @param[in] wifi_sta_config Wi-Fi STA 凭据，仅在 `WIFI_CRED_RECV` 下有效。
 * @param[in] user_ctx 未使用。
 * @return 无返回值。
 */
static void network_manager_on_provisioning_event(
    network_provisioning_adapter_event_t event,
    const wifi_sta_config_t *wifi_sta_config, void *user_ctx)
{
    (void)user_ctx;

    if (network_manager_ensure_mutex() != ESP_OK)
    {
        s_state = NETWORK_MANAGER_STATE_ERROR;
        return;
    }
    if (xSemaphoreTake(s_manager_mutex, portMAX_DELAY) != pdTRUE)
    {
        s_state = NETWORK_MANAGER_STATE_ERROR;
        return;
    }

    switch (event)
    {
    case NETWORK_PROVISIONING_ADAPTER_EVENT_WIFI_CRED_RECV:
        if (wifi_sta_config != NULL)
        {
            esp_err_t ret = ESP_OK;

            network_manager_clear_pending_provisioned_entry();
            snprintf(s_pending_provisioned_entry.ssid,
                     sizeof(s_pending_provisioned_entry.ssid), "%s",
                     (const char *)wifi_sta_config->ssid);
            snprintf(s_pending_provisioned_entry.password,
                     sizeof(s_pending_provisioned_entry.password), "%s",
                     (const char *)wifi_sta_config->password);
            s_pending_provisioned_entry_valid = true;
            s_pending_provisioned_entry_connecting = false;
            /* 显式进入配网前会暂停自动重连，收到新凭据后必须恢复，
             * 否则首次连接失败或后续掉线都不会走 STA runtime 的重试闭环。 */
            wifi_control_set_auto_reconnect_enabled(true);
            ret = wifi_control_connect((const char *)wifi_sta_config->ssid,
                                       (const char *)wifi_sta_config->password);
            if (ret == ESP_OK)
            {
                s_pending_provisioned_entry_connecting = true;
            }
            else
            {
                network_manager_clear_pending_provisioned_entry();
                s_state = NETWORK_MANAGER_STATE_ERROR;
            }
        }
        break;
    }

    xSemaphoreGive(s_manager_mutex);
}

/**
 * @brief 启动统一网络编排层。
 *
 * 当前阶段会准备底层控制层，并优先尝试 latest Wi-Fi；若没有 latest，则直接
 * 进入默认 provisioning transport。
 *
 * @return `ESP_OK` 表示入口调用成功；其他错误表示底层初始化或 transport 启动失败。
 */
esp_err_t network_manager_start(void)
{
    network_credentials_entry_t latest = {0};
    esp_err_t ret = network_manager_ensure_mutex();
    if (ret != ESP_OK)
    {
        s_state = NETWORK_MANAGER_STATE_ERROR;
        return ret;
    }

    if (xSemaphoreTake(s_manager_mutex, portMAX_DELAY) != pdTRUE)
    {
        s_state = NETWORK_MANAGER_STATE_ERROR;
        return ESP_FAIL;
    }

    ret = ble_control_init();
    if (ret != ESP_OK)
    {
        s_state = NETWORK_MANAGER_STATE_ERROR;
        xSemaphoreGive(s_manager_mutex);
        return ret;
    }

    ret = wifi_control_init();
    if (ret != ESP_OK)
    {
        s_state = NETWORK_MANAGER_STATE_ERROR;
        xSemaphoreGive(s_manager_mutex);
        return ret;
    }

    ret = network_credentials_init();
    if (ret != ESP_OK)
    {
        s_state = NETWORK_MANAGER_STATE_ERROR;
        xSemaphoreGive(s_manager_mutex);
        return ret;
    }

    ret = network_provisioning_adapter_init();
    if (ret != ESP_OK)
    {
        s_state = NETWORK_MANAGER_STATE_ERROR;
        xSemaphoreGive(s_manager_mutex);
        return ret;
    }

    ret = network_provisioning_adapter_set_event_callback(
        network_manager_on_provisioning_event, NULL);
    if (ret != ESP_OK)
    {
        s_state = NETWORK_MANAGER_STATE_ERROR;
        xSemaphoreGive(s_manager_mutex);
        return ret;
    }

    ret = network_manager_ensure_monitor_task();
    if (ret != ESP_OK)
    {
        s_state = NETWORK_MANAGER_STATE_ERROR;
        xSemaphoreGive(s_manager_mutex);
        return ret;
    }

    ret = network_credentials_get_latest(&latest);
    if (ret == ESP_OK)
    {
        (void)network_manager_sync_ble_presence(false);
        ret = network_manager_connect_entry(&latest, true);
        xSemaphoreGive(s_manager_mutex);
        return ret;
    }
    if (ret != ESP_ERR_NOT_FOUND)
    {
        s_state = NETWORK_MANAGER_STATE_ERROR;
        xSemaphoreGive(s_manager_mutex);
        return ret;
    }

    ret = network_manager_start_selected_transport_auto();
    xSemaphoreGive(s_manager_mutex);
    return ret;
}

/**
 * @brief 获取当前网络主状态。
 * @return 当前 `network_manager` 状态。
 */
network_manager_state_t network_manager_get_state(void)
{
    network_manager_state_t state = s_state;

    if (network_manager_ensure_mutex() != ESP_OK)
    {
        return NETWORK_MANAGER_STATE_ERROR;
    }
    if (xSemaphoreTake(s_manager_mutex, portMAX_DELAY) != pdTRUE)
    {
        return NETWORK_MANAGER_STATE_ERROR;
    }

    network_manager_refresh_runtime_state(false);
    state = s_state;
    xSemaphoreGive(s_manager_mutex);
    return state;
}

/**
 * @brief 读取当前网络主状态的无锁快照。
 *
 * 该接口不刷新底层状态机，也不申请 `s_manager_mutex`，因此适合 UI 刷新策略这种
 * 高频轻量轮询场景。返回值只保证“最近一次已发布状态”的快照语义，不保证与
 * `wifi_control`、`network_provisioning_adapter` 的瞬时内部状态完全同步。
 *
 * @return 当前 `network_manager` 状态快照。
 */
network_manager_state_t network_manager_get_state_cached(void)
{
    return s_state;
}

/**
 * @brief 获取当前网络状态快照。
 *
 * 当前阶段已开始桥接底层控制层，状态快照会反映 latest 连接态、BLE 开关态、
 * active transport 与 IP。
 *
 * @param[out] status 输出状态结构。
 * @return `ESP_OK` 表示成功；`ESP_ERR_INVALID_ARG` 表示参数非法。
 */
esp_err_t network_manager_get_status(network_manager_status_t *status)
{
    esp_err_t ret = ESP_OK;

    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    ret = network_manager_ensure_mutex();
    if (ret != ESP_OK)
    {
        return ret;
    }
    if (xSemaphoreTake(s_manager_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    memset(status, 0, sizeof(*status));
    network_manager_refresh_runtime_state(false);
    status->state = s_state;
    status->wifi_connected = wifi_control_is_connected();
    status->ble_enabled = ble_control_is_enabled();
    status->ble_active = ble_control_is_active();
    status->default_transport = s_default_transport;
    (void)network_manager_sync_ip(status);
    xSemaphoreGive(s_manager_mutex);
    return ESP_OK;
}

/**
 * @brief 再次尝试最近一次成功连接的 Wi-Fi。
 *
 * 该接口会读取 latest Wi-Fi 并重新发起连接；若当前已经有 provisioning transport
 * 在跑，会先收口 transport 再转回 latest 连接路径。
 *
 * @return `ESP_OK` 表示连接请求已下发；其他错误表示没有 latest 或底层连接失败。
 */
esp_err_t network_manager_use_latest_wifi(void)
{
    network_credentials_entry_t latest = {0};
    esp_err_t ret = network_manager_ensure_mutex();
    if (ret != ESP_OK)
    {
        return ret;
    }
    if (xSemaphoreTake(s_manager_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }
    ret = network_credentials_get_latest(&latest);

    if (ret != ESP_OK)
    {
        if (ret == ESP_ERR_NOT_FOUND)
        {
            s_state = NETWORK_MANAGER_STATE_IDLE;
        }
        else
        {
            s_state = NETWORK_MANAGER_STATE_ERROR;
        }
        xSemaphoreGive(s_manager_mutex);
        return ret;
    }

    network_manager_drop_pending_provisioned_entry_for_manual_action();
    ret = network_manager_connect_entry(&latest, false);
    xSemaphoreGive(s_manager_mutex);
    return ret;
}

/**
 * @brief 主动断开当前网络连接，并进入用户断开态。
 *
 * 该接口会主动断开当前 Wi-Fi，并暂停自动重连，直到上层明确要求重新联网或重新配网。
 *
 * @return `ESP_OK` 表示断网请求成功；其他错误表示底层断开失败。
 */
esp_err_t network_manager_disconnect(void)
{
    esp_err_t ret = network_manager_ensure_mutex();
    if (ret != ESP_OK)
    {
        return ret;
    }
    if (xSemaphoreTake(s_manager_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    network_manager_drop_pending_provisioned_entry_for_manual_action();
    ret = wifi_control_disconnect();
    if (ret != ESP_OK)
    {
        s_state = NETWORK_MANAGER_STATE_ERROR;
        xSemaphoreGive(s_manager_mutex);
        return ret;
    }

    wifi_control_set_auto_reconnect_enabled(false);
    ret = network_manager_stop_active_transport();
    if (ret != ESP_OK)
    {
        xSemaphoreGive(s_manager_mutex);
        return ret;
    }

    s_state = NETWORK_MANAGER_STATE_DISCONNECTED_BY_USER;
    xSemaphoreGive(s_manager_mutex);
    return ESP_OK;
}

/**
 * @brief 重新进入当前默认 provisioning transport。
 *
 * 该接口会停止当前 active transport，然后立即按当前默认 transport 重新进入配网。
 *
 * @return `ESP_OK` 表示 transport 启动成功；其他错误表示 transport 启动失败。
 */
esp_err_t network_manager_reprovision(void)
{
    esp_err_t ret = network_manager_ensure_mutex();
    if (ret != ESP_OK)
    {
        return ret;
    }
    if (xSemaphoreTake(s_manager_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    ret = network_manager_start_explicit_transport(s_default_transport);
    xSemaphoreGive(s_manager_mutex);
    return ret;
}

/**
 * @brief 显式启动 BLE 配网会话。
 *
 * 该入口只服务 Wi-Fi 配网页面的“BLE 配网”按钮；主界面蓝牙开关不会调用它。
 * 如果 BLE 总开关关闭，本函数返回 `ESP_ERR_INVALID_STATE`，由 UI 提醒用户先打开蓝牙。
 *
 * @return `ESP_OK` 表示 BLE 配网广播已启动；其他错误表示当前状态或底层启动失败。
 */
esp_err_t network_manager_start_ble_provisioning(void)
{
    esp_err_t ret = network_manager_ensure_mutex();
    if (ret != ESP_OK)
    {
        return ret;
    }
    if (xSemaphoreTake(s_manager_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    ret = network_manager_start_explicit_transport(
        NETWORK_MANAGER_PROVISIONING_TRANSPORT_BLE);
    xSemaphoreGive(s_manager_mutex);
    return ret;
}

/**
 * @brief 显式启动 SoftAP 配网会话。
 *
 * 该入口用于 Wi-Fi 配网页面的 AP 网页兜底，不依赖“先选择 transport，再
 * Reprovision”的两步 UI 状态。
 *
 * @return `ESP_OK` 表示 SoftAP 配网已启动；其他错误表示底层启动失败。
 */
esp_err_t network_manager_start_softap_provisioning(void)
{
    esp_err_t ret = network_manager_ensure_mutex();
    if (ret != ESP_OK)
    {
        return ret;
    }
    if (xSemaphoreTake(s_manager_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    ret = network_manager_start_explicit_transport(
        NETWORK_MANAGER_PROVISIONING_TRANSPORT_SOFTAP);
    xSemaphoreGive(s_manager_mutex);
    return ret;
}

/**
 * @brief 停止当前 active provisioning transport。
 * @return `ESP_OK` 表示已停止或本就空闲；其他错误表示 stop/deinit 失败。
 */
esp_err_t network_manager_stop_provisioning(void)
{
    esp_err_t ret = network_manager_ensure_mutex();
    if (ret != ESP_OK)
    {
        return ret;
    }
    if (xSemaphoreTake(s_manager_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    ret = network_manager_stop_active_transport();
    if (ret == ESP_OK)
    {
        network_manager_refresh_runtime_state(false);
    }
    xSemaphoreGive(s_manager_mutex);
    return ret;
}

/**
 * @brief 设置 BLE 总开关偏好。
 *
 * 该接口只表达“手机蓝牙开关”式语义：
 * - 开启时启动普通 BLE 可发现广播，但不启动小程序配网 GATT 服务；
 * - 关闭时停止普通 BLE 广播；如果 BLE 配网会话正在运行，也会立即停止 BLE transport；
 * - Wi-Fi STA 连接状态不会被这个开关改变，因此 Wi-Fi 与 BLE enabled 可以并存。
 *
 * @param[in] enabled 目标 BLE 总开关状态。
 * @return `ESP_OK` 表示更新成功；其他错误表示底层 stop 或偏好持久化失败。
 */
esp_err_t network_manager_set_ble_enabled(bool enabled)
{
    esp_err_t ret = network_manager_ensure_mutex();
    if (ret != ESP_OK)
    {
        return ret;
    }
    if (xSemaphoreTake(s_manager_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    ret = ble_control_set_enabled(enabled);
    if (ret != ESP_OK)
    {
        xSemaphoreGive(s_manager_mutex);
        return ret;
    }

    if (!enabled)
    {
        const bool ble_transport_active =
            network_provisioning_adapter_is_active() &&
            network_provisioning_adapter_get_transport() ==
                NETWORK_PROVISIONING_TRANSPORT_BLE;

        if (ble_transport_active)
        {
            ret = network_manager_stop_active_transport();
            if (ret != ESP_OK)
            {
                xSemaphoreGive(s_manager_mutex);
                return ret;
            }
        }

        ret = ble_presence_stop();
        if (ret != ESP_OK)
        {
            xSemaphoreGive(s_manager_mutex);
            return ret;
        }

        network_manager_refresh_runtime_state(false);
        if (!wifi_control_is_connected() &&
            !network_provisioning_adapter_is_active() &&
            s_state != NETWORK_MANAGER_STATE_CONNECTING_LATEST &&
            s_state != NETWORK_MANAGER_STATE_DISCONNECTED_BY_USER)
        {
            s_state = NETWORK_MANAGER_STATE_IDLE;
        }

        xSemaphoreGive(s_manager_mutex);
        return ESP_OK;
    }

    ret = network_manager_sync_ble_presence(true);
    if (ret != ESP_OK)
    {
        xSemaphoreGive(s_manager_mutex);
        return ret;
    }

    network_manager_refresh_runtime_state(false);
    xSemaphoreGive(s_manager_mutex);
    return ESP_OK;
}

/**
 * @brief 查询 BLE 总开关偏好。
 * @return 当前 BLE 总开关偏好。
 */
bool network_manager_is_ble_enabled(void)
{
    return ble_control_is_enabled();
}

/**
 * @brief 查询 BLE transport 当前是否活跃。
 * @return 当前 BLE transport 是否真的活跃。
 */
bool network_manager_is_ble_active(void)
{
    if (network_manager_ensure_mutex() != ESP_OK)
    {
        return false;
    }
    if (xSemaphoreTake(s_manager_mutex, portMAX_DELAY) != pdTRUE)
    {
        return false;
    }

    network_manager_sync_ble_active_from_adapter();
    const bool active = ble_control_is_active();
    xSemaphoreGive(s_manager_mutex);
    return active;
}

/**
 * @brief 设置默认 provisioning transport。
 *
 * 当前阶段先做内存态切换，后续再接入持久化。
 *
 * @param[in] transport 目标 transport。
 * @return `ESP_OK` 表示更新成功。
 */
esp_err_t network_manager_set_default_transport(
    network_manager_provisioning_transport_t transport)
{
    esp_err_t ret = network_manager_ensure_mutex();
    if (ret != ESP_OK)
    {
        return ret;
    }
    if (xSemaphoreTake(s_manager_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    if (transport > NETWORK_MANAGER_PROVISIONING_TRANSPORT_SOFTAP)
    {
        xSemaphoreGive(s_manager_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    s_default_transport = transport;
    xSemaphoreGive(s_manager_mutex);
    return ESP_OK;
}

/**
 * @brief 获取默认 provisioning transport。
 * @return 当前默认 transport。
 */
network_manager_provisioning_transport_t network_manager_get_default_transport(void)
{
    network_manager_provisioning_transport_t transport = s_default_transport;

    if (network_manager_ensure_mutex() != ESP_OK)
    {
        return NETWORK_MANAGER_PROVISIONING_TRANSPORT_BLE;
    }
    if (xSemaphoreTake(s_manager_mutex, portMAX_DELAY) != pdTRUE)
    {
        return NETWORK_MANAGER_PROVISIONING_TRANSPORT_BLE;
    }

    transport = s_default_transport;
    xSemaphoreGive(s_manager_mutex);
    return transport;
}

/**
 * @brief 读取当前保存的 recent Wi-Fi 列表。
 *
 * 当前阶段直接桥接 `network_credentials`。
 *
 * @param[out] entries 输出数组，可为 `NULL`。
 * @param[in] max_entries 调用方可接收的最大条目数。
 * @param[out] out_count 当前真实 recent 条目数。
 * @return `ESP_OK` 表示成功；其他错误表示 recent 列表读取失败。
 */
esp_err_t network_manager_get_recent_networks(
    network_credentials_entry_t *entries, size_t max_entries, size_t *out_count)
{
    return network_credentials_list(entries, max_entries, out_count);
}

/**
 * @brief 按 recent 列表索引发起连接。
 *
 * 当前阶段已接入 `network_credentials` 与 `wifi_control`，支持按索引读取并连接 recent Wi-Fi。
 *
 * @param[in] index recent 列表索引。
 * @return `ESP_OK` 表示连接请求已接收；其他错误表示 recent 列表读取或连接失败。
 */
esp_err_t network_manager_connect_recent_by_index(size_t index)
{
    network_credentials_entry_t entries[NETWORK_CREDENTIALS_MAX_NETWORKS] = {0};
    size_t count = 0;
    esp_err_t ret = network_manager_ensure_mutex();
    if (ret != ESP_OK)
    {
        return ret;
    }
    if (xSemaphoreTake(s_manager_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    ret =
        network_credentials_list(entries, NETWORK_CREDENTIALS_MAX_NETWORKS,
                                 &count);

    if (ret != ESP_OK)
    {
        s_state = NETWORK_MANAGER_STATE_ERROR;
        xSemaphoreGive(s_manager_mutex);
        return ret;
    }
    if (index >= count)
    {
        xSemaphoreGive(s_manager_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    ret = network_manager_connect_entry(&entries[index], false);
    xSemaphoreGive(s_manager_mutex);
    return ret;
}
