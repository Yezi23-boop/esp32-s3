#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * 板级电源观测接口：
 * - 负责把 AXP2101 的原始快照转成上层更容易消费的板级状态；
 * - 该层不创建后台任务，只提供 probe、单次刷新和最近缓存读取；
 * - `power_service` 会在此基础上再做轮询、去抖和发布。
 */

#ifdef __cplusplus
extern "C"
{
#endif

    /* AXP2101 单次采样转换后的板级快照。 */
    typedef struct
    {
        bool available;              /**< 当前快照是否来自一次成功采样。 */
        bool battery_data_valid;     /**< 电池电量/电压字段是否可信。 */
        bool snapshot_stale;         /**< true 表示最新采样失败，当前使用的是历史快照。 */
        bool charging;               /**< PMIC 判定当前处于充电中。 */
        bool discharging;            /**< PMIC 判定当前处于放电中。 */
        bool external_power_present; /**< 是否检测到外部供电，例如 USB/VBUS。 */
        bool battery_present;        /**< 是否检测到电池在位。 */
        uint16_t battery_mv;         /**< 电池电压，单位为毫伏。 */
        uint16_t system_mv;          /**< 系统母线电压，单位为毫伏。 */
        uint8_t battery_percent;     /**< 电量百分比；仅在 `battery_data_valid=true` 时有效，否则为 `UINT8_MAX`。 */
    } board_power_state_t;

    /**
     * @brief 初始化板级电源观测模块。
     *
     * 初始化阶段会探测 AXP2101 是否存在，但不会创建后台任务。
     *
     * @return `ESP_OK` 表示模块可继续使用；
     *         `ESP_ERR_NOT_FOUND` 表示未探测到 AXP2101；
     *         其他错误表示 probe 失败。
     */
    esp_err_t board_power_init(void);

    /**
     * @brief 主动刷新一次板级电源快照。
     * @param[out] state 输出状态快照。
     * @return `ESP_OK` 表示本次拿到了最新采样；
     *         其他错误表示采样失败，此时 `state` 可能退化为历史快照或 unsampled 状态。
     */
    esp_err_t board_power_refresh(board_power_state_t *state);

    /**
     * @brief 获取最近一次缓存的板级电源状态。
     * @return 最近一次成功采样的缓存；若从未成功采样，则返回统一的 unsampled 占位状态。
     */
    const board_power_state_t *board_power_get_cached_state(void);

#ifdef __cplusplus
}
#endif
