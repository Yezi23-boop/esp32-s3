#ifndef WIFI_CONTROL_INTERNAL_H
#define WIFI_CONTROL_INTERNAL_H

#include <stdint.h>

#include "wifi_control.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Wi-Fi 连接失败后允许的最大自动重试次数。 */
#define WIFI_CONTROL_MAX_RETRY 5

/** @brief STA 默认网络接口键名，用于查询当前 IP。 */
#define WIFI_CONTROL_STA_IFKEY "WIFI_STA_DEF"

/**
 * @brief Wi-Fi 控制面的内部运行时上下文。
 *
 * 该结构只保存纯 STA 运行态，不保存凭据，也不保存 BLE/AP 语义。
 */
typedef struct
{
    bool initialized; /**< 是否已经完成过一次显式初始化。 */
    bool init_in_progress; /**< 当前是否有线程正在执行初始化门闩。 */
    bool connected; /**< 当前是否已拿到有效 IP。 */
    bool auto_reconnect_enabled; /**< 是否允许断线后自动重连。 */
    bool reconnect_suppressed; /**< 当前是否正在执行显式断开或重连前置清理。 */
    bool reconnect_after_disconnect; /**< 当前抑制断开是为了随后立即重连。 */
    uint8_t retry_count; /**< 当前自动重连重试计数。 */
    wifi_control_state_t state; /**< 运行状态快照。 */
} wifi_control_runtime_t;

#ifdef __cplusplus
}
#endif

#endif /* WIFI_CONTROL_INTERNAL_H */
