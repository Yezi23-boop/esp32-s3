#ifndef DISPLAY_ALERT_ADAPTER_H
#define DISPLAY_ALERT_ADAPTER_H

#include <stdbool.h>

#include "esp_err.h"

/*
 * 显示告警适配层：
 * - 对外暴露“显示 / 隐藏危险覆盖层”的语义接口；
 * - 实际 LVGL 对象创建和显示必须在 UI 线程中完成；
 * - 因此该模块采用 pending 标记，把跨线程请求延迟到 UI 线程落地。
 */

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 初始化显示告警适配层。
     *
     * 该初始化只重置内部状态，不立即创建 LVGL 覆盖层对象；
     * 覆盖层会在首次真正需要显示时按需创建。
     *
     * @return `ESP_OK` 表示初始化成功。
     */
    esp_err_t display_alert_adapter_init(void);

    /**
     * @brief 请求显示危险覆盖层。
     *
     * 该接口只设置跨线程 pending 标志，真正的 LVGL 对象操作会延迟到 UI 线程执行。
     *
     * @return `ESP_OK` 表示请求已接受；
     *         `ESP_ERR_INVALID_STATE` 表示模块尚未初始化。
     */
    esp_err_t display_alert_adapter_show_danger_overlay(void);

    /**
     * @brief 请求隐藏危险覆盖层。
     * @return `ESP_OK` 表示请求已接受；
     *         `ESP_ERR_INVALID_STATE` 表示模块尚未初始化。
     */
    esp_err_t display_alert_adapter_hide_danger_overlay(void);

    /**
     * @brief 请求显示低电量提示。
     *
     * 该提示是普通可见提醒，不使用危险红色覆盖层，也不抢占 P0 告警。
     *
     * @return `ESP_OK` 表示请求已接受；
     *         `ESP_ERR_INVALID_STATE` 表示模块尚未初始化。
     */
    esp_err_t display_alert_adapter_show_low_battery_warning(void);

    /**
     * @brief 请求隐藏低电量提示。
     * @return `ESP_OK` 表示请求已接受；
     *         `ESP_ERR_INVALID_STATE` 表示模块尚未初始化。
     */
    esp_err_t display_alert_adapter_hide_low_battery_warning(void);

    /**
     * @brief 设置危险覆盖层显示抑制开关。
     * @param[in] suppressed true 表示后续即使收到 show 请求也不真正显示覆盖层。
     * @return `ESP_OK` 表示设置成功；
     *         `ESP_ERR_INVALID_STATE` 表示模块尚未初始化。
     */
    esp_err_t display_alert_adapter_set_suppressed(bool suppressed);

    /**
     * @brief 在 LVGL 线程中处理待执行的显示/隐藏请求。
     *
     * @return 无返回值。
     *
     * @note 必须在 LVGL 所在线程周期调用，不能在其他线程直接操作覆盖层对象。
     */
    void display_alert_adapter_process_ui(void);

#ifdef __cplusplus
}
#endif

#endif // DISPLAY_ALERT_ADAPTER_H
