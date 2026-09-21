/**
 * @file network_provisioning_adapter.c
 * @brief 官方 network_provisioning / wifi_prov_mgr 的最小适配层。
 */

#include "network_provisioning_adapter_internal.h"

#include <string.h>

#include "esp_netif.h"
#include "esp_log.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"
#include "network_provisioning/scheme_softap.h"

/*
 * 兼容层说明：
 * - 新官方组件对外 API 前缀是 `network_prov_mgr_*`
 * - 本仓库 Chunk 1 的文本契约仍要求源码中能直接看到 `wifi_prov_mgr_*`
 *   这些标识符，因此这里用别名把两个世界接起来
 */
typedef network_prov_mgr_config_t wifi_prov_mgr_config_t;
typedef network_prov_event_handler_t wifi_prov_event_handler_t;
typedef network_prov_scheme_t wifi_prov_scheme_t;
typedef network_prov_security_t wifi_prov_security_t;
typedef network_prov_cb_event_t wifi_prov_cb_event_t;
typedef network_prov_cb_func_t wifi_prov_cb_func_t;

/**
 * @brief adapter 的内部运行时上下文。
 *
 * 这是单实例全局状态，避免把 provisioning manager 状态散落到多个调用点。
 */
typedef struct
{
    bool initialized; /**< 是否已经完成过一次显式初始化。 */
    bool manager_started; /**< 是否已经成功调用过官方 manager init/start。 */
    network_provisioning_transport_t transport; /**< 当前锁定的 transport。 */
    network_provisioning_adapter_state_t state; /**< 当前状态机状态。 */
    network_provisioning_adapter_event_cb_t event_cb; /**< 上层注册的 adapter 事件回调。 */
    void *event_user_ctx; /**< 上层注册的用户上下文。 */
} network_provisioning_adapter_runtime_t;

#define wifi_prov_mgr_init network_prov_mgr_init
#define wifi_prov_mgr_start_provisioning network_prov_mgr_start_provisioning
#define wifi_prov_mgr_stop_provisioning network_prov_mgr_stop_provisioning
#define wifi_prov_mgr_deinit network_prov_mgr_deinit
#define wifi_prov_scheme_ble network_prov_scheme_ble
#define wifi_prov_scheme_softap network_prov_scheme_softap
#define WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM \
    NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM
#define WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BT \
    NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BT
#define WIFI_PROV_EVENT_HANDLER_NONE NETWORK_PROV_EVENT_HANDLER_NONE
#define WIFI_PROV_SECURITY_0 NETWORK_PROV_SECURITY_0

/** @brief 组件日志标签。 */
static const char *TAG = "net_prov_adpt";
/** @brief 官方默认 SoftAP esp-netif 的固定 ifkey；用于查询是否已经创建过 AP netif。 */
static const char *kWifiApIfKey = "WIFI_AP_DEF";

/** @brief 单实例全局运行时上下文。 */
static network_provisioning_adapter_runtime_t s_runtime = {
    .initialized = false,
    .manager_started = false,
    .transport = NETWORK_PROVISIONING_TRANSPORT_NONE,
    .state = NETWORK_PROVISIONING_ADAPTER_STATE_IDLE,
};
/** @brief adapter 运行时 mutex 的静态存储。 */
static StaticSemaphore_t s_runtime_mutex_buffer;
/** @brief 串行化 adapter 运行态与回调注册的静态 mutex。 */
static SemaphoreHandle_t s_runtime_mutex = NULL;
/** @brief 保护 runtime mutex 首次创建路径的最小临界区锁。 */
static portMUX_TYPE s_runtime_bootstrap_lock = portMUX_INITIALIZER_UNLOCKED;
/** @brief adapter 生命周期 recursive mutex 的静态存储。 */
static StaticSemaphore_t s_lifecycle_mutex_buffer;
/** @brief 串行化 start/stop/switch 等生命周期 API 的 recursive mutex。 */
static SemaphoreHandle_t s_lifecycle_mutex = NULL;
/** @brief 保护 lifecycle mutex 首次创建路径的最小临界区锁。 */
static portMUX_TYPE s_lifecycle_bootstrap_lock = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t network_provisioning_adapter_ensure_mutex(void);
static esp_err_t network_provisioning_adapter_ensure_lifecycle_mutex(void);
static esp_err_t network_provisioning_adapter_ensure_softap_netif(void);
static void network_provisioning_adapter_set_runtime_state_threadsafe(
    network_provisioning_adapter_state_t state,
    network_provisioning_transport_t transport, bool manager_started);
static void network_provisioning_adapter_set_initialized_threadsafe(
    bool initialized);
static void network_provisioning_adapter_copy_runtime_snapshot(
    network_provisioning_adapter_runtime_t *runtime);
static void network_provisioning_adapter_copy_event_callback_snapshot(
    network_provisioning_adapter_event_cb_t *event_cb, void **user_ctx);

/**
 * @brief 确保 adapter 的运行时 mutex 已创建。
 *
 * 该 mutex 负责保护运行态字段与上层事件回调注册，避免生命周期 API、查询 API 与官方
 * manager 回调同时读写全局状态。
 *
 * @return `ESP_OK` 表示 mutex 已可用；其他值表示创建失败。
 */
static esp_err_t network_provisioning_adapter_ensure_mutex(void)
{
    if (s_runtime_mutex != NULL)
    {
        return ESP_OK;
    }

    taskENTER_CRITICAL(&s_runtime_bootstrap_lock);
    if (s_runtime_mutex == NULL)
    {
        s_runtime_mutex = xSemaphoreCreateMutexStatic(&s_runtime_mutex_buffer);
    }
    taskEXIT_CRITICAL(&s_runtime_bootstrap_lock);

    return s_runtime_mutex != NULL ? ESP_OK : ESP_FAIL;
}

/**
 * @brief 确保 adapter 生命周期 recursive mutex 已创建。
 *
 * 该 mutex 用来把 `init/start/stop/switch_transport` 这组公共生命周期 API 串行化，
 * 避免直接并发调用时出现双启动或启动/停止交错。
 *
 * @return `ESP_OK` 表示 mutex 已可用；其他值表示创建失败。
 */
static esp_err_t network_provisioning_adapter_ensure_lifecycle_mutex(void)
{
    if (s_lifecycle_mutex != NULL)
    {
        return ESP_OK;
    }

    taskENTER_CRITICAL(&s_lifecycle_bootstrap_lock);
    if (s_lifecycle_mutex == NULL)
    {
        s_lifecycle_mutex =
            xSemaphoreCreateRecursiveMutexStatic(&s_lifecycle_mutex_buffer);
    }
    taskEXIT_CRITICAL(&s_lifecycle_bootstrap_lock);

    return s_lifecycle_mutex != NULL ? ESP_OK : ESP_FAIL;
}

/**
 * @brief 确保官方 SoftAP 默认 netif 已创建。
 *
 * 官方 `wifi_prov_mgr` SoftAP 示例会在启动 provisioning 之前显式创建
 * `esp_netif_create_default_wifi_ap()`。当前仓库长期只初始化了 STA netif，
 * 这会让 SoftAP 无线侧虽然能起来，但 AP 侧网络接口和后续 HTTP 承载链路不完整，
 * 进而表现为客户端能连上热点、`192.168.4.1:80` 可达，但 official provisioning
 * 请求一进来就被设备端直接断开。
 *
 * 这里先按官方默认 `if_key = "WIFI_AP_DEF"` 查询现有 AP netif，只有在确实不存在时
 * 才补建，避免和其他调用方重复创建默认接口。
 *
 * @return `ESP_OK` 表示 AP netif 已可用；其他错误表示创建失败。
 */
static esp_err_t network_provisioning_adapter_ensure_softap_netif(void)
{
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey(kWifiApIfKey);

    if (ap_netif != NULL)
    {
        return ESP_OK;
    }

    ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == NULL)
    {
        ESP_LOGE(TAG, "创建默认 SoftAP netif 失败");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "已补建默认 SoftAP netif，供官方 provisioning HTTP 服务复用");
    return ESP_OK;
}

/**
 * @brief 将运行时状态更新为目标值。
 *
 * 这个小函数让状态迁移更集中，避免在多个分支里重复写同一组字段。
 *
 * @param[in] state 新状态。
 * @param[in] transport 新 transport。
 * @param[in] manager_started 是否已持有官方 manager 会话。
 */
static void network_provisioning_adapter_set_runtime_state(
    network_provisioning_adapter_state_t state,
    network_provisioning_transport_t transport, bool manager_started)
{
    /* 该辅助函数默认由已经完成外部串行化的调用方使用。 */
    s_runtime.state = state;
    s_runtime.transport = transport;
    s_runtime.manager_started = manager_started;
}

/**
 * @brief 将当前状态重置为空闲。
 *
 * 这里保留 `initialized` 标志，便于区分“曾经初始化过”和“完全未碰过”。
 */
static void network_provisioning_adapter_reset_to_idle(void)
{
    network_provisioning_adapter_set_runtime_state(
        NETWORK_PROVISIONING_ADAPTER_STATE_IDLE,
        NETWORK_PROVISIONING_TRANSPORT_NONE, false);
}

/**
 * @brief 线程安全地更新 adapter 运行态。
 *
 * 该辅助函数用于生命周期 API 在不同调用上下文下更新全局运行态，避免直接无锁改写
 * `s_runtime.state / transport / manager_started`。
 *
 * @param[in] state 新状态。
 * @param[in] transport 新 transport。
 * @param[in] manager_started 是否已经持有官方 manager 会话。
 * @return 无返回值。
 */
static void network_provisioning_adapter_set_runtime_state_threadsafe(
    network_provisioning_adapter_state_t state,
    network_provisioning_transport_t transport, bool manager_started)
{
    if (network_provisioning_adapter_ensure_mutex() != ESP_OK)
    {
        return;
    }
    if (xSemaphoreTake(s_runtime_mutex, portMAX_DELAY) != pdTRUE)
    {
        return;
    }

    network_provisioning_adapter_set_runtime_state(state, transport,
                                                   manager_started);
    xSemaphoreGive(s_runtime_mutex);
}

/**
 * @brief 线程安全地更新 `initialized` 标志。
 *
 * `initialized` 用来表达 adapter 是否完成过一次显式 init，因此和其它运行态字段一样
 * 需要统一纳入 mutex 保护。
 *
 * @param[in] initialized 新的初始化标志值。
 * @return 无返回值。
 */
static void network_provisioning_adapter_set_initialized_threadsafe(
    bool initialized)
{
    if (network_provisioning_adapter_ensure_mutex() != ESP_OK)
    {
        return;
    }
    if (xSemaphoreTake(s_runtime_mutex, portMAX_DELAY) != pdTRUE)
    {
        return;
    }

    s_runtime.initialized = initialized;
    xSemaphoreGive(s_runtime_mutex);
}

/**
 * @brief 复制当前 adapter 运行态快照。
 *
 * 查询 API 与回调线程都通过这个快照读取当前状态，避免直接无锁访问全局运行态。
 *
 * @param[out] runtime 输出运行态快照。
 * @return 无返回值。
 */
static void network_provisioning_adapter_copy_runtime_snapshot(
    network_provisioning_adapter_runtime_t *runtime)
{
    if (runtime == NULL)
    {
        return;
    }

    memset(runtime, 0, sizeof(*runtime));
    if (network_provisioning_adapter_ensure_mutex() != ESP_OK)
    {
        return;
    }
    if (xSemaphoreTake(s_runtime_mutex, portMAX_DELAY) != pdTRUE)
    {
        return;
    }

    *runtime = s_runtime;
    xSemaphoreGive(s_runtime_mutex);
}

/**
 * @brief 安全复制当前注册的上层事件回调与用户上下文。
 *
 * 官方 manager 回调不持锁直接调用上层，先复制快照再出锁，避免回调里再访问 adapter
 * 查询接口时造成递归锁等待。
 *
 * @param[out] event_cb 输出回调函数指针。
 * @param[out] user_ctx 输出用户上下文。
 * @return 无返回值。
 */
static void network_provisioning_adapter_copy_event_callback_snapshot(
    network_provisioning_adapter_event_cb_t *event_cb, void **user_ctx)
{
    if (event_cb == NULL || user_ctx == NULL)
    {
        return;
    }

    *event_cb = NULL;
    *user_ctx = NULL;
    if (network_provisioning_adapter_ensure_mutex() != ESP_OK)
    {
        return;
    }
    if (xSemaphoreTake(s_runtime_mutex, portMAX_DELAY) != pdTRUE)
    {
        return;
    }

    *event_cb = s_runtime.event_cb;
    *user_ctx = s_runtime.event_user_ctx;
    xSemaphoreGive(s_runtime_mutex);
}

/**
 * @brief 统一承接官方 manager 回调，并转译成项目内部 adapter 事件。
 *
 * 当前只向上抛出 Wi-Fi 凭据收到/成功/失败三个最小事件，避免把官方事件枚举直接扩散
 * 到上层编排层。
 *
 * @param[in] user_data 未使用。
 * @param[in] event 官方 manager 事件。
 * @param[in] event_data 官方事件附带数据。
 * @return 无返回值。
 */
static void network_provisioning_adapter_app_event_cb(void *user_data,
                                                      wifi_prov_cb_event_t event,
                                                      void *event_data)
{
    network_provisioning_adapter_event_cb_t event_cb = NULL;
    void *event_user_ctx = NULL;

    (void)user_data;
    network_provisioning_adapter_copy_event_callback_snapshot(&event_cb,
                                                              &event_user_ctx);

    switch (event)
    {
    case NETWORK_PROV_WIFI_CRED_RECV:
        if (event_cb != NULL)
        {
            event_cb(
            NETWORK_PROVISIONING_ADAPTER_EVENT_WIFI_CRED_RECV,
            (const wifi_sta_config_t *)event_data, event_user_ctx);
        }
        break;
    default:
        break;
    }
}

/**
 * @brief 根据目标 transport 组装官方 manager 配置。
 *
 * BLE 需要 scheme-specific handler，SoftAP 则保持最小配置即可。
 *
 * @param[in] transport 目标 transport。
 * @param[out] config 官方 manager 配置。
 * @return `ESP_OK` 表示配置成功；其他值表示 transport 非法。
 */
static esp_err_t network_provisioning_adapter_fill_mgr_config(
    network_provisioning_transport_t transport,
    wifi_prov_mgr_config_t *config)
{
    wifi_prov_event_handler_t no_event = WIFI_PROV_EVENT_HANDLER_NONE;
    /* 主界面蓝牙开关需要在 provisioning 结束后还能重新启动普通 BLE presence。
     * 因此这里只释放 Classic BT 内存，不使用 FREE_BTDM 释放整个 BLE 控制器内存。 */
    wifi_prov_event_handler_t ble_handler =
        WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BT;
    wifi_prov_event_handler_t app_handler = {
        .event_cb = (wifi_prov_cb_func_t)network_provisioning_adapter_app_event_cb,
        .user_data = NULL,
    };

    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config));

    switch (transport)
    {
    case NETWORK_PROVISIONING_TRANSPORT_BLE:
        config->scheme = wifi_prov_scheme_ble;
        config->scheme_event_handler = ble_handler;
        config->app_event_handler = app_handler;
        return ESP_OK;
    case NETWORK_PROVISIONING_TRANSPORT_SOFTAP:
        config->scheme = wifi_prov_scheme_softap;
        config->scheme_event_handler = no_event;
        config->app_event_handler = app_handler;
        return ESP_OK;
    case NETWORK_PROVISIONING_TRANSPORT_NONE:
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

/**
 * @brief 启动指定 transport 的官方 provisioning manager。
 *
 * 该实现会先 init 再 start，并在任何失败时显式 deinit，确保生命周期可回收。
 *
 * @param[in] transport 目标 transport。
 * @return `ESP_OK` 表示启动成功；其他值表示 manager init 或 start 失败。
 */
static esp_err_t network_provisioning_adapter_start_transport(
    network_provisioning_transport_t transport)
{
    esp_err_t ret = ESP_OK;
    wifi_prov_mgr_config_t config = {0};
    const char *service_name = NULL;
    network_provisioning_adapter_runtime_t runtime = {0};

    network_provisioning_adapter_copy_runtime_snapshot(&runtime);
    if (runtime.manager_started)
    {
        return ESP_ERR_INVALID_STATE;
    }

    ret = network_provisioning_adapter_fill_mgr_config(transport, &config);
    if (ret != ESP_OK)
    {
        return ret;
    }

    if (transport == NETWORK_PROVISIONING_TRANSPORT_SOFTAP)
    {
        ret = network_provisioning_adapter_ensure_softap_netif();
        if (ret != ESP_OK)
        {
            network_provisioning_adapter_set_runtime_state_threadsafe(
                NETWORK_PROVISIONING_ADAPTER_STATE_ERROR,
                NETWORK_PROVISIONING_TRANSPORT_NONE, false);
            return ret;
        }
    }

    service_name =
        (transport == NETWORK_PROVISIONING_TRANSPORT_BLE) ? "NET_PROV_BLE"
                                                           : "NET_PROV_AP";

    network_provisioning_adapter_set_runtime_state_threadsafe(
        NETWORK_PROVISIONING_ADAPTER_STATE_INITIALIZING, transport, false);

    ret = wifi_prov_mgr_init(config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "wifi_prov_mgr_init failed: %s", esp_err_to_name(ret));
        network_provisioning_adapter_set_runtime_state_threadsafe(
            NETWORK_PROVISIONING_ADAPTER_STATE_ERROR,
            NETWORK_PROVISIONING_TRANSPORT_NONE, false);
        return ret;
    }

    ret = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_0, NULL,
                                           service_name, NULL);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "wifi_prov_mgr_start_provisioning failed: %s",
                 esp_err_to_name(ret));
        wifi_prov_mgr_deinit();
        network_provisioning_adapter_set_runtime_state_threadsafe(
            NETWORK_PROVISIONING_ADAPTER_STATE_ERROR,
            NETWORK_PROVISIONING_TRANSPORT_NONE, false);
        return ret;
    }

    network_provisioning_adapter_set_runtime_state_threadsafe(
        transport == NETWORK_PROVISIONING_TRANSPORT_BLE
            ? NETWORK_PROVISIONING_ADAPTER_STATE_ACTIVE_BLE
            : NETWORK_PROVISIONING_ADAPTER_STATE_ACTIVE_SOFTAP,
        transport, true);
    return ESP_OK;
}

/**
 * @brief 停止当前运行中的官方 provisioning manager。
 *
 * 这是 adapter 强制要求的显式收尾路径，必须同时做 stop 和 deinit。
 *
 * @return `ESP_OK` 表示当前会话已关闭；其他值表示 stop/deinit 过程中出错。
 */
static esp_err_t network_provisioning_adapter_stop_manager(void)
{
    esp_err_t ret = ESP_OK;
    network_provisioning_adapter_runtime_t runtime = {0};

    network_provisioning_adapter_copy_runtime_snapshot(&runtime);
    if (!runtime.manager_started)
    {
        network_provisioning_adapter_set_runtime_state_threadsafe(
            NETWORK_PROVISIONING_ADAPTER_STATE_IDLE,
            NETWORK_PROVISIONING_TRANSPORT_NONE, false);
        network_provisioning_adapter_set_initialized_threadsafe(true);
        return ESP_OK;
    }

    network_provisioning_adapter_set_runtime_state_threadsafe(
        NETWORK_PROVISIONING_ADAPTER_STATE_STOPPING, runtime.transport, true);

    wifi_prov_mgr_stop_provisioning();

    ret = wifi_prov_mgr_deinit();
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "wifi_prov_mgr_deinit failed: %s", esp_err_to_name(ret));
        network_provisioning_adapter_set_runtime_state_threadsafe(
            NETWORK_PROVISIONING_ADAPTER_STATE_ERROR, runtime.transport, true);
        return ret;
    }

    network_provisioning_adapter_set_runtime_state_threadsafe(
        NETWORK_PROVISIONING_ADAPTER_STATE_IDLE,
        NETWORK_PROVISIONING_TRANSPORT_NONE, false);
    network_provisioning_adapter_set_initialized_threadsafe(true);
    return ESP_OK;
}

/**
 * @brief 初始化 adapter 的单例状态。
 *
 * 这里的 init 只负责把 adapter 置于可用的空闲状态，不会拉起 provisioning manager。
 *
 * @return `ESP_OK` 表示初始化成功或已经初始化；其他值表示参数非法。
 */
esp_err_t network_provisioning_adapter_init(void)
{
    esp_err_t ret = network_provisioning_adapter_ensure_mutex();
    if (ret != ESP_OK)
    {
        return ret;
    }
    ret = network_provisioning_adapter_ensure_lifecycle_mutex();
    if (ret != ESP_OK)
    {
        return ret;
    }
    if (xSemaphoreTakeRecursive(s_lifecycle_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }
    if (xSemaphoreTake(s_runtime_mutex, portMAX_DELAY) != pdTRUE)
    {
        xSemaphoreGiveRecursive(s_lifecycle_mutex);
        return ESP_FAIL;
    }

    if (s_runtime.manager_started)
    {
        xSemaphoreGive(s_runtime_mutex);
        xSemaphoreGiveRecursive(s_lifecycle_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    s_runtime.initialized = true;
    network_provisioning_adapter_reset_to_idle();
    xSemaphoreGive(s_runtime_mutex);
    xSemaphoreGiveRecursive(s_lifecycle_mutex);
    return ESP_OK;
}

/**
 * @brief 注册 adapter 的上层事件回调。
 *
 * 该接口只更新运行态中的回调指针，不会启动 provisioning manager。
 *
 * @param[in] event_cb 上层事件回调，可为 `NULL` 表示取消注册。
 * @param[in] user_ctx 用户上下文，可为 `NULL`。
 * @return `ESP_OK` 表示注册成功。
 */
esp_err_t network_provisioning_adapter_set_event_callback(
    network_provisioning_adapter_event_cb_t event_cb, void *user_ctx)
{
    esp_err_t ret = network_provisioning_adapter_ensure_mutex();
    if (ret != ESP_OK)
    {
        return ret;
    }
    if (xSemaphoreTake(s_runtime_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    s_runtime.event_cb = event_cb;
    s_runtime.event_user_ctx = user_ctx;
    xSemaphoreGive(s_runtime_mutex);
    return ESP_OK;
}

/**
 * @brief 启动 BLE provisioning。
 * @return `ESP_OK` 表示启动成功；其他值表示 manager 初始化或启动失败。
 */
esp_err_t network_provisioning_adapter_start_ble(void)
{
    esp_err_t ret = ESP_OK;
    network_provisioning_adapter_runtime_t runtime = {0};

    ret = network_provisioning_adapter_ensure_lifecycle_mutex();
    if (ret != ESP_OK)
    {
        return ret;
    }
    if (xSemaphoreTakeRecursive(s_lifecycle_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    network_provisioning_adapter_copy_runtime_snapshot(&runtime);
    if (runtime.manager_started)
    {
        xSemaphoreGiveRecursive(s_lifecycle_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    if (!runtime.initialized)
    {
        ret = network_provisioning_adapter_init();
        if (ret != ESP_OK)
        {
            xSemaphoreGiveRecursive(s_lifecycle_mutex);
            return ret;
        }
    }

    ret = network_provisioning_adapter_start_transport(
        NETWORK_PROVISIONING_TRANSPORT_BLE);
    xSemaphoreGiveRecursive(s_lifecycle_mutex);
    return ret;
}

/**
 * @brief 启动 SoftAP provisioning。
 * @return `ESP_OK` 表示启动成功；其他值表示 manager 初始化或启动失败。
 */
esp_err_t network_provisioning_adapter_start_softap(void)
{
    esp_err_t ret = ESP_OK;
    network_provisioning_adapter_runtime_t runtime = {0};

    ret = network_provisioning_adapter_ensure_lifecycle_mutex();
    if (ret != ESP_OK)
    {
        return ret;
    }
    if (xSemaphoreTakeRecursive(s_lifecycle_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    network_provisioning_adapter_copy_runtime_snapshot(&runtime);
    if (runtime.manager_started)
    {
        xSemaphoreGiveRecursive(s_lifecycle_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    if (!runtime.initialized)
    {
        ret = network_provisioning_adapter_init();
        if (ret != ESP_OK)
        {
            xSemaphoreGiveRecursive(s_lifecycle_mutex);
            return ret;
        }
    }

    ret = network_provisioning_adapter_start_transport(
        NETWORK_PROVISIONING_TRANSPORT_SOFTAP);
    xSemaphoreGiveRecursive(s_lifecycle_mutex);
    return ret;
}

/**
 * @brief 停止当前 provisioning 会话，并显式反初始化官方 manager。
 * @return `ESP_OK` 表示当前会话已停止或本就处于空闲态；其他值表示底层 stop/deinit 失败。
 */
esp_err_t network_provisioning_adapter_stop(void)
{
    esp_err_t ret = network_provisioning_adapter_ensure_lifecycle_mutex();
    if (ret != ESP_OK)
    {
        return ret;
    }
    if (xSemaphoreTakeRecursive(s_lifecycle_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    ret = network_provisioning_adapter_stop_manager();
    xSemaphoreGiveRecursive(s_lifecycle_mutex);
    return ret;
}

/**
 * @brief 统一切换 provisioning transport。
 *
 * 为了避免同一实例内的热切换，这里始终按 stop -> start 的顺序处理。
 *
 * @param[in] transport 目标 transport。
 * @return `ESP_OK` 表示切换成功；其他值表示停止、反初始化或重新启动失败。
 */
esp_err_t network_provisioning_adapter_switch_transport(
    network_provisioning_transport_t transport)
{
    esp_err_t ret = ESP_OK;
    network_provisioning_adapter_runtime_t runtime = {0};

    ret = network_provisioning_adapter_ensure_lifecycle_mutex();
    if (ret != ESP_OK)
    {
        return ret;
    }
    if (xSemaphoreTakeRecursive(s_lifecycle_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    if (transport != NETWORK_PROVISIONING_TRANSPORT_NONE &&
        transport != NETWORK_PROVISIONING_TRANSPORT_BLE &&
        transport != NETWORK_PROVISIONING_TRANSPORT_SOFTAP)
    {
        xSemaphoreGiveRecursive(s_lifecycle_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    if (transport == NETWORK_PROVISIONING_TRANSPORT_NONE)
    {
        ret = network_provisioning_adapter_stop_manager();
        xSemaphoreGiveRecursive(s_lifecycle_mutex);
        return ret;
    }

    network_provisioning_adapter_copy_runtime_snapshot(&runtime);

    if (transport == NETWORK_PROVISIONING_TRANSPORT_BLE)
    {
        if (runtime.manager_started && runtime.transport == transport)
        {
            xSemaphoreGiveRecursive(s_lifecycle_mutex);
            return ESP_OK;
        }

        ret = network_provisioning_adapter_stop_manager();
        if (ret != ESP_OK)
        {
            xSemaphoreGiveRecursive(s_lifecycle_mutex);
            return ret;
        }

        ret = network_provisioning_adapter_start_transport(
            NETWORK_PROVISIONING_TRANSPORT_BLE);
        xSemaphoreGiveRecursive(s_lifecycle_mutex);
        return ret;
    }

    if (transport == NETWORK_PROVISIONING_TRANSPORT_SOFTAP)
    {
        if (runtime.manager_started && runtime.transport == transport)
        {
            xSemaphoreGiveRecursive(s_lifecycle_mutex);
            return ESP_OK;
        }

        ret = network_provisioning_adapter_stop_manager();
        if (ret != ESP_OK)
        {
            xSemaphoreGiveRecursive(s_lifecycle_mutex);
            return ret;
        }

        ret = network_provisioning_adapter_start_transport(
            NETWORK_PROVISIONING_TRANSPORT_SOFTAP);
        xSemaphoreGiveRecursive(s_lifecycle_mutex);
        return ret;
    }

    xSemaphoreGiveRecursive(s_lifecycle_mutex);
    return ESP_ERR_INVALID_ARG;
}

/**
 * @brief 查询当前是否存在 active provisioning 会话。
 * @return true 表示 BLE 或 SoftAP provisioning 正在运行。
 */
bool network_provisioning_adapter_is_active(void)
{
    network_provisioning_adapter_runtime_t runtime = {0};

    network_provisioning_adapter_copy_runtime_snapshot(&runtime);
    return runtime.manager_started;
}

/**
 * @brief 查询当前 active transport。
 * @return `NETWORK_PROVISIONING_TRANSPORT_NONE` 表示空闲，其余值表示当前会话类型。
 */
network_provisioning_transport_t network_provisioning_adapter_get_transport(void)
{
    network_provisioning_adapter_runtime_t runtime = {0};

    network_provisioning_adapter_copy_runtime_snapshot(&runtime);
    return runtime.transport;
}

/**
 * @brief 查询 adapter 的完整快照。
 * @return 当前运行状态快照。
 */
network_provisioning_adapter_status_t network_provisioning_adapter_get_status(
    void)
{
    network_provisioning_adapter_runtime_t runtime = {0};

    network_provisioning_adapter_copy_runtime_snapshot(&runtime);
    network_provisioning_adapter_status_t status = {
        .state = runtime.state,
        .transport = runtime.transport,
        .active = runtime.manager_started,
    };

    return status;
}
