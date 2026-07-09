#ifndef NETWORK_SERVICE_H
#define NETWORK_SERVICE_H

#include <stdbool.h>

#include "esp_err.h"

/*
 * 网络服务层：
 * 1. 当前阶段主要承担“云端业务是否真正可用”的就绪探测。
 * 2. 对历史调用方继续保留旧接口名，但实际联网控制统一桥接到 `network_manager`。
 * 3. 因此它现在是兼容 shim + service-ready 探测层，而不是新的网络主控制面。
 */

#ifdef __cplusplus
extern "C"
{
#endif

    /* 面向上层的网络服务状态：
     * 既描述当前联网方式，也描述云端依赖是否真正可用。 */
    typedef enum
    {
        NETWORK_SERVICE_STATE_OFFLINE = 0,      /* 尚未启动服务任务。 */
        NETWORK_SERVICE_STATE_BLE_PROVISIONING, /* 无凭据或主动请求 BLE 配网。 */
        NETWORK_SERVICE_STATE_BLE_DISABLED,     /* 无凭据且用户主动关闭 BLE 配网入口。 */
        NETWORK_SERVICE_STATE_CONNECTING,       /* 已有凭据，正在后台连 Wi-Fi。 */
        NETWORK_SERVICE_STATE_WIFI_READY,       /* 已连上 Wi-Fi，但云端依赖尚未全部验证。 */
        NETWORK_SERVICE_STATE_SERVICE_READY,    /* Wi-Fi 与关键业务域名探测均已通过。 */
        NETWORK_SERVICE_STATE_PORTAL_REQUIRED,  /* BLE 不可用或失败后，回退到 AP 门户模式。 */
        NETWORK_SERVICE_STATE_ERROR,            /* 启动底层配网流程失败。 */
    } network_service_state_t;

    typedef enum
    {
        NETWORK_SERVICE_PROVISION_TRANSPORT_AUTO = 0, /* 兼容保留；当前内部会映射到 BLE。 */
        NETWORK_SERVICE_PROVISION_TRANSPORT_BLE,      /* 兼容层 BLE 配网 transport。 */
        NETWORK_SERVICE_PROVISION_TRANSPORT_AP,       /* 兼容层 SoftAP 配网 transport。 */
    } network_service_provision_transport_t;

    typedef struct
    {
        bool wifi_connected;
        bool has_credentials;
        bool user_disconnect_latched;
        bool provisioning_active;
        bool ble_active;
        bool ap_active;
        network_service_provision_transport_t default_transport;
        char ip[16];
    } network_service_wifi_status_t;

    typedef struct
    {
        network_service_state_t state;       /* 当前兼容层状态。 */
        bool wifi_connected;                 /* 当前是否已获得 Wi-Fi STA 连接。 */
        bool service_ready;                  /* 关键云端依赖是否已通过探测。 */
        bool probe_active;                   /* 当前是否正在执行云端 ready 探测。 */
        bool probe_paused_by_budget;         /* 探测是否被 power budget 暂停。 */
        bool power_save_applied;             /* 最近一次下发的 Wi-Fi PS 目标值。 */
        esp_err_t last_error;                /* 最近一次服务层错误。 */
        esp_err_t last_probe_result;         /* 最近一次云端 ready 探测结果。 */
    } network_service_snapshot_t;

    /* 状态迁移主路径（当前项目）:
     * OFFLINE -> CONNECTING/BLE_PROVISIONING -> WIFI_READY -> SERVICE_READY
     * 失败或兜底时可转到 PORTAL_REQUIRED。 */

    /**
     * @brief 启动后台网络状态机。
     * @return `ESP_OK` 表示任务已启动或之前已启动；其他错误表示任务创建失败。
     */
    esp_err_t network_service_start(void);

    /**
     * @brief 获取当前网络服务状态。
     * @return 服务层状态枚举。
     */
    network_service_state_t network_service_get_state(void);

    /**
     * @brief 设置 BLE 总开关偏好。
     *
     * 当前接口只是兼容层桥接，真实语义由 `network_manager` 决定：
     * - 开启时只允许 BLE 功能，不会自动启动小程序配网；
     * - 关闭时会立即停止当前 BLE provisioning transport；
     * - Wi-Fi 连接状态不受该开关影响，允许 Wi-Fi 与 BLE enabled 并存。
     *
     * @param[in] enabled true 表示允许 BLE；false 表示关闭 BLE。
     * @return `ESP_OK` 表示状态已更新；其他错误表示偏好持久化或 BLE 停止失败。
     */
    esp_err_t network_service_set_ble_enabled(bool enabled);

    /**
     * @brief 查询 BLE 配网偏好是否为开启。
     * @return true 表示偏好开启；false 表示用户主动关闭。
     */
    bool network_service_is_ble_enabled(void);

    /**
     * @brief 查询 BLE 配网当前是否真的处于活动状态。
     * @return true 表示广播或连接仍然存在。
     */
    bool network_service_is_ble_active(void);

    /**
     * @brief 查询当前 Wi-Fi 是否已连接。
     * @return true 表示已拿到有效 STA IP。
     */
    bool network_service_is_wifi_connected(void);

    /**
     * @brief 获取 Wi-Fi 管理页使用的状态快照。
     * @param[out] status 输出结构体。
     * @return `ESP_OK` 表示成功；`ESP_ERR_INVALID_ARG` 表示参数非法。
     */
    esp_err_t network_service_get_wifi_status(network_service_wifi_status_t *status);

    /**
     * @brief 获取网络服务生命周期快照。
     *
     * getter 只复制 owner 已发布的状态，不做 DNS/I/O、不阻塞、不推进状态机。
     *
     * @param[out] snapshot 输出快照。
     * @return `ESP_OK` 表示成功；`ESP_ERR_INVALID_ARG` 表示参数非法。
     */
    esp_err_t network_service_get_snapshot(network_service_snapshot_t *snapshot);

    /**
     * @brief 再次使用已保存凭据发起联网。
     * @return 底层连接请求结果。
     */
    esp_err_t network_service_request_connect_with_saved_credentials(void);

    /**
     * @brief 断开当前网络连接，并暂停自动重连。
     * @return 底层断开结果。
     */
    esp_err_t network_service_request_disconnect(void);

    /**
     * @brief 重新进入配网流程。
     * @return 底层 transport 启动结果。
     */
    esp_err_t network_service_request_reprovision(void);

    /**
     * @brief 设置默认配网方式。
     * @param[in] transport 目标 transport。
     * @return `ESP_OK` 表示已更新。
     */
    esp_err_t network_service_set_default_provision_transport(
        network_service_provision_transport_t transport);

    /**
     * @brief 获取默认配网方式。
     * @return 当前默认 transport。
     */
    network_service_provision_transport_t
    network_service_get_default_provision_transport(void);

    /**
     * @brief 判断云端业务依赖是否真正可用。
     * @return true 表示 Wi-Fi 已连通且关键业务探测通过。
     */
    bool network_service_is_service_ready(void);

    /**
     * @brief 获取当前缓存的 IPv4 地址。
     * @param[out] ip_str 输出缓冲区。
     * @param[in] ip_str_len 输出缓冲区长度，单位为字节。
     * @return `ESP_OK` 表示已成功复制；
     *         `ESP_ERR_INVALID_ARG` 表示参数非法；
     *         `ESP_ERR_INVALID_STATE` 表示当前尚未拿到有效 IP。
     */
    esp_err_t network_service_get_ip(char *ip_str, size_t ip_str_len);

    /**
     * @brief 主动切换到 AP 门户配网。
     * @return 无返回值。
     */
    void network_service_request_portal(void);

    /**
     * @brief 主动启动 BLE 配网。
     *
     * 该兼容入口不会自动打开 BLE 总开关；如果蓝牙被用户关闭，会由底层返回
     * `ESP_ERR_INVALID_STATE` 并记录日志。
     *
     * @return 无返回值。
     */
    void network_service_request_ble(void);

#ifdef __cplusplus
}
#endif

#endif // NETWORK_SERVICE_H
