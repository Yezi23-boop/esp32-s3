#ifndef BLE_PRESENCE_H
#define BLE_PRESENCE_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动普通 BLE 可发现广播。
 *
 * 该入口用于主界面“蓝牙开关”的手机蓝牙式语义：打开后设备可以被 BLE
 * 扫描发现，但不会启动 Wi-Fi provisioning GATT 服务。调用方必须保证当前没有
 * 官方 BLE provisioning 会话在运行，否则两个 BLE owner 会争用 NimBLE host。
 *
 * @return `ESP_OK` 表示广播已启动或已在运行；其他错误表示 NimBLE 初始化失败。
 *
 * @note 可阻塞，必须在任务上下文调用，不能在 ISR 中调用。
 */
esp_err_t ble_presence_start(void);

/**
 * @brief 停止普通 BLE 可发现广播并释放 NimBLE host。
 *
 * 该入口用于关闭主界面蓝牙开关，或在进入官方 BLE provisioning 前让出 BLE
 * transport owner。停止后官方 provisioning adapter 可以重新初始化自己的 BLE
 * GATT 服务。
 *
 * @return `ESP_OK` 表示已停止或本就空闲；其他错误表示 NimBLE stop/deinit 失败。
 *
 * @note 可阻塞，必须在任务上下文调用，不能在 ISR 中调用。
 */
esp_err_t ble_presence_stop(void);

/**
 * @brief 查询普通 BLE 可发现广播是否正在运行。
 *
 * 这里的 active 只代表 `ble_presence` 自己持有 NimBLE host，不代表官方 BLE
 * provisioning transport 是否活跃。
 *
 * @return true 表示普通 BLE 可发现广播正在运行。
 */
bool ble_presence_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_PRESENCE_H */
