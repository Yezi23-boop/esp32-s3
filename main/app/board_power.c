#include "board_power.h"

#include "axp2101.h"

/*
 * 板级电源观测实现说明：
 * - 原始数据来自 AXP2101 快照；
 * - 该模块负责把 PMIC 语义转换成上层稳定字段，并维护最近一次成功快照缓存；
 * - 当采样失败时优先回退到历史状态，让上层有机会展示“上次已知状态”。
 */

static board_power_state_t s_cached_state = {0}; // 最近一次成功采样后的缓存状态，供服务层和 UI 查询。
static const board_power_state_t s_unsampled_state = {
    .available = false,
    .battery_data_valid = false,
    .snapshot_stale = false,
    .battery_percent = UINT8_MAX,
};
static bool s_initialized = false;      // 模块是否已完成 probe/init。
static bool s_has_cached_state = false; // 是否已有至少一次成功快照。

/**
 * @brief 返回“尚未成功采样”的占位状态。
 * @return 统一的 unsampled 状态指针。
 */
static const board_power_state_t *board_power_get_unsampled_state(void)
{
    return &s_unsampled_state;
}

/**
 * @brief 将底层 PMIC 快照转换成板级统一状态。
 *
 * 该转换过程会同步完成字段有效性判定，避免上层重复理解底层驱动的取值约束。
 *
 * @param[in] snapshot AXP2101 原始快照。
 * @return 适合上层消费的板级状态。
 */
static board_power_state_t board_power_from_snapshot(
    const axp2101_snapshot_t *snapshot)
{
    board_power_state_t state = {0};
    // AXP 上报的电量百分比只有落在 [0, 100] 时才可作为有效值发布。
    bool battery_percent_valid = snapshot->battery_percent >= 0 &&
                                 snapshot->battery_percent <= 100;

    state.available = true;
    state.snapshot_stale = false;
    state.charging = snapshot->charging;
    state.discharging = snapshot->discharging;
    state.external_power_present = snapshot->vbus_good;
    state.battery_present = snapshot->battery_present;
    state.battery_mv = snapshot->battery_mv;
    state.system_mv = snapshot->vsys_mv;
    state.battery_data_valid = state.battery_present &&
                               snapshot->battery_mv > 0 &&
                               battery_percent_valid;
    state.battery_percent = state.battery_data_valid
                                ? (uint8_t)snapshot->battery_percent
                                : UINT8_MAX;
    return state;
}

/**
 * @brief 初始化板级电源观测模块。
 * @return `ESP_OK` 表示已探测到 AXP2101 且可继续采样；
 *         `ESP_ERR_NOT_FOUND` 表示未探测到器件；
 *         其他错误表示 probe 失败。
 */
esp_err_t board_power_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    bool present = false;
    esp_err_t ret = axp2101_probe(&present);
    if (ret != ESP_OK)
    {
        return ret;
    }
    if (!present)
    {
        return ESP_ERR_NOT_FOUND;
    }

    s_initialized = true;
    return ESP_OK;
}

/**
 * @brief 主动刷新一次板级电源快照。
 *
 * 若本次采样失败但历史上存在成功快照，则会回退到最近一次已知状态并标记 `snapshot_stale`，
 * 这样上层仍能显示“最近一次已知值”，而不是直接失去全部电源信息。
 *
 * @param[out] state 输出参数，接收本次发布给上层的状态。
 * @return `ESP_OK` 表示拿到了最新采样；
 *         其他错误表示本次采样失败，此时 `state` 可能是历史快照或 unsampled 状态。
 */
esp_err_t board_power_refresh(board_power_state_t *state)
{
    if (state == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        *state = *board_power_get_unsampled_state();
        return ESP_ERR_INVALID_STATE;
    }

    axp2101_snapshot_t snapshot = {0}; // 原始 PMIC 采样结果。
    esp_err_t ret = axp2101_read_snapshot(&snapshot);
    if (ret == ESP_OK)
    {
        s_cached_state = board_power_from_snapshot(&snapshot);
        s_has_cached_state = true;
        *state = s_cached_state;
        return ESP_OK;
    }

    if (s_has_cached_state)
    {
        board_power_state_t stale_state = s_cached_state;
        stale_state.snapshot_stale = true;
        stale_state.available = false;
        *state = stale_state;
        return ret;
    }

    *state = *board_power_get_unsampled_state();
    return ret;
}

/**
 * @brief 获取最近一次缓存的板级电源状态。
 * @return 最近一次成功采样的缓存；若从未成功采样，则返回统一的 unsampled 占位状态。
 */
const board_power_state_t *board_power_get_cached_state(void)
{
    if (!s_has_cached_state)
    {
        return board_power_get_unsampled_state();
    }
    return &s_cached_state;
}
