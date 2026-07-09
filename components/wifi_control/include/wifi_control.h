#ifndef WIFI_CONTROL_H
#define WIFI_CONTROL_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Wi-Fi STA 运行时的最小状态枚举。
 *
 * 这里只描述 STA 控制面的运行状态，不承载 BLE/AP provisioning 语义。
 */
typedef enum
{
    WIFI_CONTROL_STATE_IDLE = 0, /**< 尚未完成初始化或尚未进入控制流程。 */
    WIFI_CONTROL_STATE_CONNECTING, /**< 正在连接目标 Wi-Fi。 */
    WIFI_CONTROL_STATE_CONNECTED, /**< 已连接并拿到有效 IP。 */
    WIFI_CONTROL_STATE_DISCONNECTED, /**< 当前未连接。 */
    WIFI_CONTROL_STATE_CONNECT_FAIL, /**< 自动重连失败并已到达重试上限。 */
} wifi_control_state_t;

/**
 * @brief 初始化 Wi-Fi STA runtime control。
 *
 * 该接口只准备 Wi-Fi 驱动、事件回调和 STA 默认状态，不会启动 BLE 或 AP 门户。
 *
 * @return `ESP_OK` 表示初始化成功；其他错误表示底层 Wi-Fi、事件或网络栈初始化失败。
 */
esp_err_t wifi_control_init(void);

/**
 * @brief 使用显式 SSID 和密码发起 STA 连接。
 *
 * 该接口只做 STA 连接控制，不保存凭据，也不触发任何 provisioning 语义。
 *
 * @param[in] ssid 目标 SSID。
 * @param[in] password 目标密码。
 * @return `ESP_OK` 表示连接请求已下发；其他错误表示参数非法或底层配置失败。
 */
esp_err_t wifi_control_connect(const char *ssid, const char *password);

/**
 * @brief 主动断开当前 STA 连接。
 *
 * 若当前本就未连接，也会返回 `ESP_OK`，以便上层把它视为幂等控制操作。
 *
 * @return `ESP_OK` 表示断开请求已处理；其他错误表示底层 Wi-Fi 句柄不可用。
 */
esp_err_t wifi_control_disconnect(void);

/**
 * @brief 设置断线后的自动重连开关。
 *
 * 关闭后，Wi-Fi 断开事件只会更新状态，不再自动调用 `esp_wifi_connect()`。
 *
 * @param[in] enabled true 表示允许自动重连；false 表示关闭自动重连。
 * @return 无返回值。
 */
void wifi_control_set_auto_reconnect_enabled(bool enabled);

/**
 * @brief 查询当前是否允许自动重连。
 * @return true 表示允许自动重连。
 */
bool wifi_control_is_auto_reconnect_enabled(void);

/**
 * @brief 查询当前 STA 是否已连接。
 * @return true 表示已经拿到有效 IP。
 */
bool wifi_control_is_connected(void);

/**
 * @brief 获取当前 STA 的 IPv4 字符串。
 *
 * 该接口只读当前运行态，不会触发任何连接或 provisioning 行为。
 *
 * @param[out] ip_str 输出缓冲区。
 * @param[in] ip_str_len 输出缓冲区长度，至少 16 字节。
 * @return `ESP_OK` 表示成功；其他错误表示当前未连接或参数非法。
 */
esp_err_t wifi_control_get_ip(char *ip_str, size_t ip_str_len);

/**
 * @brief 设置当前 Wi-Fi STA 的省电模式。
 *
 * 该接口只改动运行时 `esp_wifi_set_ps()` 配置，不会触发重新连接、
 * provisioning 或凭据持久化行为。
 *
 * @param[in] enabled true 表示开启 `WIFI_PS_MIN_MODEM`；false 表示关闭省电。
 * @return `ESP_OK` 表示设置成功；其他错误表示底层 Wi-Fi 尚未就绪或驱动调用失败。
 */
esp_err_t wifi_control_set_power_save(bool enabled);

/**
 * @brief 查询当前 Wi-Fi STA 的运行状态。
 * @return 运行状态枚举快照。
 */
wifi_control_state_t wifi_control_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_CONTROL_H */
