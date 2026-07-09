#ifndef NETWORK_PROVISIONING_ADAPTER_H
#define NETWORK_PROVISIONING_ADAPTER_H

#include <stdbool.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief provisioning transport 的最小对外枚举。
 *
 * 这里只保留 `NONE / BLE / SOFTAP`，避免把历史 `AUTO` 扩散到新的上层架构里。
 */
typedef enum
{
    NETWORK_PROVISIONING_TRANSPORT_NONE = 0, /**< 当前没有 active transport。 */
    NETWORK_PROVISIONING_TRANSPORT_BLE,      /**< 官方 BLE provisioning transport。 */
    NETWORK_PROVISIONING_TRANSPORT_SOFTAP,    /**< 官方 SoftAP provisioning transport。 */
} network_provisioning_transport_t;

/**
 * @brief adapter 的运行状态枚举。
 *
 * 该状态只描述 adapter 自身生命周期，不替代上层网络状态机。
 */
typedef enum
{
    NETWORK_PROVISIONING_ADAPTER_STATE_IDLE = 0, /**< 空闲态，未持有 active provisioning 会话。 */
    NETWORK_PROVISIONING_ADAPTER_STATE_INITIALIZING, /**< 正在准备官方 provisioning manager。 */
    NETWORK_PROVISIONING_ADAPTER_STATE_ACTIVE_BLE, /**< 当前 active transport 为 BLE。 */
    NETWORK_PROVISIONING_ADAPTER_STATE_ACTIVE_SOFTAP, /**< 当前 active transport 为 SoftAP。 */
    NETWORK_PROVISIONING_ADAPTER_STATE_STOPPING, /**< 正在执行 stop/deinit 收尾。 */
    NETWORK_PROVISIONING_ADAPTER_STATE_ERROR, /**< 最近一次生命周期操作失败。 */
} network_provisioning_adapter_state_t;

/**
 * @brief adapter 的快照状态。
 */
typedef struct
{
    network_provisioning_adapter_state_t state; /**< 当前 adapter 状态。 */
    network_provisioning_transport_t transport;  /**< 当前 active transport。 */
    bool active;                                 /**< 是否处于 active provisioning 会话。 */
} network_provisioning_adapter_status_t;

/**
 * @brief adapter 向上层抛出的最小事件枚举。
 */
typedef enum
{
    NETWORK_PROVISIONING_ADAPTER_EVENT_WIFI_CRED_RECV = 0, /**< 官方 manager 收到了 Wi-Fi 凭据。 */
} network_provisioning_adapter_event_t;

/**
 * @brief adapter 事件回调函数类型。
 *
 * 当前阶段 adapter 只向上层上抛“收到 Wi-Fi 凭据”这一件事，把真正的连网成功/失败判定
 * 收口到 `network_manager + wifi_control`，避免让官方 manager 的事件语义直接扩散到上层。
 *
 * `wifi_sta_config` 只在 `NETWORK_PROVISIONING_ADAPTER_EVENT_WIFI_CRED_RECV` 事件下有效。
 *
 * @param[in] event adapter 事件类型。
 * @param[in] wifi_sta_config Wi-Fi STA 凭据，可为 `NULL`。
 * @param[in] user_ctx 调用方注册的用户上下文。
 * @return 无返回值。
 */
typedef void (*network_provisioning_adapter_event_cb_t)(
    network_provisioning_adapter_event_t event,
    const wifi_sta_config_t *wifi_sta_config, void *user_ctx);

/**
 * @brief 初始化 adapter 的单例状态。
 *
 * @return `ESP_OK` 表示初始化成功或已经初始化；其他值表示参数或内部状态错误。
 */
esp_err_t network_provisioning_adapter_init(void);

/**
 * @brief 注册 adapter 的上层事件回调。
 *
 * 该接口只保存回调和上下文，不会启动 provisioning。
 *
 * @param[in] event_cb 上层事件回调，可为 `NULL` 表示取消注册。
 * @param[in] user_ctx 用户上下文，可为 `NULL`。
 * @return `ESP_OK` 表示注册成功。
 */
esp_err_t network_provisioning_adapter_set_event_callback(
    network_provisioning_adapter_event_cb_t event_cb, void *user_ctx);

/**
 * @brief 启动 BLE provisioning。
 *
 * 调用前后都不允许同一实例内直接热切换到另一个 transport。
 *
 * @return `ESP_OK` 表示启动成功；其他值表示 manager 初始化或启动失败。
 */
esp_err_t network_provisioning_adapter_start_ble(void);

/**
 * @brief 启动 SoftAP provisioning。
 *
 * 调用前后都不允许同一实例内直接热切换到另一个 transport。
 *
 * @return `ESP_OK` 表示启动成功；其他值表示 manager 初始化或启动失败。
 */
esp_err_t network_provisioning_adapter_start_softap(void);

/**
 * @brief 停止当前 provisioning 会话，并显式反初始化官方 manager。
 *
 * 这条路径是 adapter 规定的标准收尾动作，保证 stop 和 deinit 总是一起发生。
 *
 * @return `ESP_OK` 表示当前会话已停止或本就处于空闲态；其他值表示底层 stop/deinit 失败。
 */
esp_err_t network_provisioning_adapter_stop(void);

/**
 * @brief 统一切换 provisioning transport。
 *
 * 该接口内部只走 `stop()` 再 `start_xxx()`，不允许在同一实例里热切换 scheme。
 *
 * @param[in] transport 目标 transport。
 * @return `ESP_OK` 表示切换成功；其他值表示停止、反初始化或重新启动失败。
 */
esp_err_t network_provisioning_adapter_switch_transport(
    network_provisioning_transport_t transport);

/**
 * @brief 查询当前是否存在 active provisioning 会话。
 * @return true 表示 BLE 或 SoftAP provisioning 正在运行。
 */
bool network_provisioning_adapter_is_active(void);

/**
 * @brief 查询当前 active transport。
 * @return `NETWORK_PROVISIONING_TRANSPORT_NONE` 表示空闲，其余值表示当前会话类型。
 */
network_provisioning_transport_t network_provisioning_adapter_get_transport(void);

/**
 * @brief 查询 adapter 的完整快照。
 * @return 当前运行状态快照。
 */
network_provisioning_adapter_status_t network_provisioning_adapter_get_status(void);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_PROVISIONING_ADAPTER_H */
