#ifndef BLE_CONTROL_H
#define BLE_CONTROL_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 BLE 总开关控制层。
 *
 * 该接口只负责读取/准备 BLE enabled 偏好与运行态锁，不会启动或停止 BLE。
 *
 * @return `ESP_OK` 表示初始化成功；其他错误表示 NVS 或内部状态准备失败。
 */
esp_err_t ble_control_init(void);

/**
 * @brief 设置 BLE 总开关偏好。
 *
 * 该接口只持久化 enabled 偏好，不触发 BLE 启停。
 *
 * @param[in] enabled true 表示允许 BLE；false 表示关闭 BLE。
 * @return `ESP_OK` 表示写入成功；其他错误表示 NVS 写入失败。
 */
esp_err_t ble_control_set_enabled(bool enabled);

/**
 * @brief 查询 BLE 总开关偏好。
 * @return true 表示允许 BLE。
 */
bool ble_control_is_enabled(void);

/**
 * @brief 设置 BLE 当前运行态。
 *
 * 该接口只用于上层同步 active 状态，不做持久化。
 *
 * @param[in] active true 表示 BLE 当前活跃。
 * @return `ESP_OK` 表示状态更新成功；其他错误表示内部状态未就绪。
 */
esp_err_t ble_control_set_active(bool active);

/**
 * @brief 查询 BLE 当前运行态。
 * @return true 表示 BLE 当前活跃。
 */
bool ble_control_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_CONTROL_H */
