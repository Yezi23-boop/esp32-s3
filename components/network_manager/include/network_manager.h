#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "network_credentials.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief `network_manager` 对外暴露的主状态机。
 *
 * 该状态机表达的是项目网络产品语义，而不是底层 Wi-Fi/BLE driver 细节。
 */
typedef enum
{
    NETWORK_MANAGER_STATE_IDLE = 0, /**< 尚未启动网络编排。 */
    NETWORK_MANAGER_STATE_CONNECTING_LATEST, /**< 正在尝试最近一次成功连接的 Wi-Fi。 */
    NETWORK_MANAGER_STATE_CONNECTED, /**< 当前已成功连接 Wi-Fi。 */
    NETWORK_MANAGER_STATE_PROVISIONING_BLE, /**< 当前正在走 BLE 配网 transport。 */
    NETWORK_MANAGER_STATE_PROVISIONING_SOFTAP, /**< 当前正在走 SoftAP 配网 transport。 */
    NETWORK_MANAGER_STATE_DISCONNECTED_BY_USER, /**< 用户主动断网后暂停自动重连。 */
    NETWORK_MANAGER_STATE_ERROR, /**< 当前网络编排进入错误态。 */
} network_manager_state_t;

/**
 * @brief `network_manager` 可选择的配网 transport。
 *
 * 当前长期架构只保留 `BLE / SOFTAP`，不再保留 `AUTO`。
 */
typedef enum
{
    NETWORK_MANAGER_PROVISIONING_TRANSPORT_BLE = 0, /**< 默认走 BLE 配网 transport。 */
    NETWORK_MANAGER_PROVISIONING_TRANSPORT_SOFTAP, /**< 默认走 SoftAP 配网 transport。 */
} network_manager_provisioning_transport_t;

/**
 * @brief 对 UI / 业务层暴露的网络状态快照。
 */
typedef struct
{
    network_manager_state_t state; /**< 当前主状态机状态。 */
    bool wifi_connected; /**< 当前是否已连上 Wi-Fi。 */
    bool ble_enabled; /**< BLE 总开关偏好是否开启；不等同于正在配网。 */
    bool ble_active; /**< BLE 配网广播当前是否活跃。 */
    network_manager_provisioning_transport_t default_transport; /**< 当前默认配网 transport。 */
    char ip[16]; /**< 当前 IPv4 字符串，无 IP 时为空字符串。 */
} network_manager_status_t;

/**
 * @brief 启动统一网络编排层。
 *
 * 当前实现会完成底层控制层初始化，并按以下优先级推进：
 * - 若存在 latest Wi-Fi，则优先尝试最新一条 recent 记录；
 * - 若不存在 latest，保持空闲态等待用户进入 Wi-Fi 管理页选择配网方式；
 * - latest 失败后同样不自动启动 BLE 小程序配网，避免蓝牙开关和配网入口耦合。
 *
 * @return `ESP_OK` 表示入口调用成功；其他错误表示底层编排初始化失败。
 */
esp_err_t network_manager_start(void);

/**
 * @brief 获取当前网络主状态。
 * @return 当前 `network_manager` 状态。
 */
network_manager_state_t network_manager_get_state(void);

/**
 * @brief 读取当前网络主状态的无锁快照。
 *
 * 该接口只返回最近一次写入的 `network_manager` 主状态，不会主动刷新底层运行态，
 * 也不会申请 `s_manager_mutex`。因此它适合 UI 刷新策略、背光策略等高频轮询路径，
 * 用来避免把界面主循环绑到网络互斥锁上。
 *
 * 注意：这是一个“轻量快照”接口，返回值可能比 `network_manager_get_state()` 稍旧。
 * 若调用方需要“读状态前顺便推进底层状态机”的语义，仍应使用
 * `network_manager_get_state()`。
 *
 * @return 当前 `network_manager` 状态快照。
 */
network_manager_state_t network_manager_get_state_cached(void);

/**
 * @brief 获取当前网络状态快照。
 *
 * @param[out] status 输出状态结构。
 * @return `ESP_OK` 表示成功；`ESP_ERR_INVALID_ARG` 表示参数非法。
 */
esp_err_t network_manager_get_status(network_manager_status_t *status);

/**
 * @brief 再次尝试最近一次成功连接的 Wi-Fi。
 *
 * @return `ESP_OK` 表示请求已接收；其他错误表示当前条件不允许启动该流程。
 */
esp_err_t network_manager_use_latest_wifi(void);

/**
 * @brief 主动断开当前网络连接，并进入用户断开态。
 *
 * @return `ESP_OK` 表示请求已接收；其他错误表示底层断开失败。
 */
esp_err_t network_manager_disconnect(void);

/**
 * @brief 重新进入当前默认 provisioning transport。
 *
 * @return `ESP_OK` 表示请求已接收；其他错误表示 transport 启动失败。
 */
esp_err_t network_manager_reprovision(void);

/**
 * @brief 显式启动 BLE 配网会话。
 *
 * 该接口只应由 Wi-Fi 配网页面里的“BLE 配网”入口调用。主界面蓝牙开关
 * 只负责 BLE enabled 偏好，不会自动触发小程序配网广播。
 *
 * @return `ESP_OK` 表示 BLE 配网广播已启动；
 *         `ESP_ERR_INVALID_STATE` 表示 BLE 总开关当前关闭；
 *         其他错误表示底层 BLE provisioning 启动失败。
 */
esp_err_t network_manager_start_ble_provisioning(void);

/**
 * @brief 显式启动 SoftAP 配网会话。
 *
 * 该接口用于 AP 网页兜底入口，避免 UI 先切默认 transport 再调用
 * `network_manager_reprovision()` 的两步语义散落在页面层。
 *
 * @return `ESP_OK` 表示 SoftAP 配网已启动；其他错误表示启动失败。
 */
esp_err_t network_manager_start_softap_provisioning(void);

/**
 * @brief 停止当前 BLE 或 SoftAP provisioning 会话。
 *
 * 该接口只收口 active provisioning transport，不改变已保存 Wi-Fi 凭据；主要由
 * network service owner 在 Wi-Fi 管理页退出或配网完成后的生命周期收尾中调用。
 *
 * @return `ESP_OK` 表示已停止或本就空闲；其他错误表示 stop/deinit 失败。
 */
esp_err_t network_manager_stop_provisioning(void);

/**
 * @brief 设置 BLE 总开关偏好。
 *
 * 该接口现在只表达“手机蓝牙开关”式语义：
 * - 开启时启动普通 BLE 可发现广播，不自动进入小程序配网；
 * - 关闭时停止普通 BLE 广播；若当前正跑 BLE 配网广播，也会立即停止该会话；
 * - Wi-Fi 是否连接不影响 BLE enabled 偏好，两者允许并存。
 *
 * @param[in] enabled true 表示允许 BLE；false 表示关闭 BLE。
 * @return `ESP_OK` 表示更新成功；其他错误表示更新失败。
 */
esp_err_t network_manager_set_ble_enabled(bool enabled);

/**
 * @brief 查询 BLE 总开关偏好。
 * @return true 表示允许 BLE。
 */
bool network_manager_is_ble_enabled(void);

/**
 * @brief 查询 BLE transport 当前是否活跃。
 * @return true 表示 BLE transport 当前活跃。
 */
bool network_manager_is_ble_active(void);

/**
 * @brief 设置默认 provisioning transport。
 *
 * @param[in] transport 目标 provisioning transport。
 * @return `ESP_OK` 表示更新成功；其他错误表示参数非法或持久化失败。
 */
esp_err_t network_manager_set_default_transport(
    network_manager_provisioning_transport_t transport);

/**
 * @brief 获取默认 provisioning transport。
 * @return 当前默认 transport。
 */
network_manager_provisioning_transport_t network_manager_get_default_transport(void);

/**
 * @brief 读取当前保存的 recent Wi-Fi 列表。
 *
 * @param[out] entries 输出数组，可为 `NULL`。
 * @param[in] max_entries 调用方可接收的最大条目数。
 * @param[out] out_count 当前真实 recent 条目数。
 * @return `ESP_OK` 表示成功；其他错误表示 recent 列表读取失败。
 */
esp_err_t network_manager_get_recent_networks(
    network_credentials_entry_t *entries, size_t max_entries, size_t *out_count);

/**
 * @brief 按 recent 列表索引发起连接。
 *
 * 该接口为后续 Wi-Fi 管理页“手动点选最近网络”预留。
 *
 * @param[in] index recent 列表索引，`0` 表示最新一条。
 * @return `ESP_OK` 表示请求已接收；其他错误表示索引非法或当前不支持。
 */
esp_err_t network_manager_connect_recent_by_index(size_t index);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_MANAGER_H */
