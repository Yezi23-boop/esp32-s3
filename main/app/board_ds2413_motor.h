#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief 初始化 DS2413 马达控制通路，并立即将马达置为关闭状态。
 *
 * 该入口属于 Board Foundation 阶段的板级硬件初始化：它只负责让
 * GPIO18 上的 DS2413 可访问，并把马达写入安全关闭态，不拥有任何
 * 提醒策略、UI 状态或告警节奏。
 *
 * 当前板卡连接关系：
 * - 1-Wire IO 使用 GPIO18，外部 R22 4.7k 上拉；
 * - DS2413 family code 使用板测值 0xBA；
 * - PIOA 控制 Q1 基极，`release` 打开马达，`pull-low` 关闭马达；
 * - PIOB 当前未使用，始终保持 release。
 *
 * @return ESP_OK 表示 DS2413 已找到且马达已写入关闭状态。
 *
 * @note 仅允许在任务上下文调用；内部会创建 FreeRTOS mutex、访问 RMT
 *       1-Wire bus，并可能因总线事务阻塞短时间。不得在 ISR 中调用。
 */
esp_err_t board_ds2413_motor_init(void);

/**
 * @brief 设置马达开关状态。
 *
 * @param[in] enabled true 表示释放 PIOA 打开马达，false 表示拉低 PIOA 关闭马达。
 * @return ESP_OK 表示 DS2413 写入并回读校验成功。
 *
 * @note 调用前必须已成功执行 `board_ds2413_motor_init()`；函数内部会持有
 *       DS2413 bus mutex，适合任务上下文，不适合 ISR 或持有其他长锁时调用。
 */
esp_err_t board_ds2413_motor_set_enabled(bool enabled);

/**
 * @brief 输出一次马达脉冲，结束后保证马达关闭。
 *
 * @param[in] on_ms 马达开启时长，单位 ms。
 * @return ESP_OK 表示脉冲输出完成并成功关闭马达。
 *
 * @note 该函数通过 `vTaskDelay()` 保持开启时长，会阻塞当前任务；若未来用于
 *       告警节奏，应由提醒 owner 放到独立 worker/task 中调度。
 */
esp_err_t board_ds2413_motor_pulse(uint32_t on_ms);

#ifdef __cplusplus
}
#endif
