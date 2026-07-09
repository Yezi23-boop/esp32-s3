#ifndef APP_ALERT_MANAGER_H
#define APP_ALERT_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * 应用级告警管理器：
 * - 聚合不同告警来源的请求；
 * - 协调声音提示与屏幕红色覆盖层；
 * - 对上层暴露统一 raise / clear 接口，避免业务直接操作多个子模块。
 */

#ifdef __cplusplus
extern "C"
{
#endif

    /* 告警来源枚举，用于区分不同业务链路。 */
    typedef enum
    {
        APP_ALERT_SOURCE_NONE = 0,
        APP_ALERT_SOURCE_TRAFFIC_AUDIO = 1, // 来自危险声音识别链路
        APP_ALERT_SOURCE_FALL_DETECTION = 2, // 来自 IMU 跌倒检测链路
    } app_alert_source_t;

    /* 告警严重级别，目前只有危险级。 */
    typedef enum
    {
        APP_ALERT_SEVERITY_NONE = 0,
        APP_ALERT_SEVERITY_DANGER = 1, // 危险级告警（需要红色覆盖层 + 提示音）
    } app_alert_severity_t;

    typedef enum
    {
        APP_ALERT_LABEL_NONE = 0,
        APP_ALERT_LABEL_HORN = 1,   // 喇叭类危险音
        APP_ALERT_LABEL_SIREN = 2,  // 警笛类危险音
        APP_ALERT_LABEL_DANGER = 3, // ESP-DL 二分类危险音，尚不细分具体声源
        APP_ALERT_LABEL_FALL = 4,   // IMU 跌倒检测确认
    } app_alert_label_t;

    /* 一次完整告警请求，包含来源、级别和语义标签。 */
    typedef struct
    {
        app_alert_source_t source;     /**< 告警来源模块。 */
        app_alert_severity_t severity; /**< 告警严重级别。 */
        app_alert_label_t label;       /**< 告警类别标签。 */
    } app_alert_request_t;

    /**
     * @brief 初始化应用级告警管理器及其依赖模块。
     * @return `ESP_OK` 表示初始化成功或已初始化；其他错误表示依赖模块初始化失败。
     */
    esp_err_t app_alert_manager_init(void);

    /**
     * @brief 上报一次告警请求。
     * @param[in] request 告警请求。
     * @return `ESP_OK` 表示已接受；
     *         `ESP_ERR_INVALID_ARG` 表示请求非法；
     *         `ESP_ERR_INVALID_STATE` 表示管理器尚未初始化。
     */
    esp_err_t app_alert_manager_raise(const app_alert_request_t *request);

    /**
     * @brief 按来源清除当前活动告警。
     * @param[in] source 告警来源。
     * @return `ESP_OK` 表示已清除或当前无需清除；
     *         `ESP_ERR_INVALID_STATE` 表示管理器尚未初始化。
     */
    esp_err_t app_alert_manager_clear(app_alert_source_t source);

    /**
     * @brief 设置低电量可见提示。
     *
     * 该接口只消费上层已经判定好的低电量预算，不读取 PMIC，不判断阈值，
     * 也不复用 P0 危险覆盖层。
     *
     * @param[in] visible true 表示显示低电量提示，false 表示隐藏。
     * @param[in] battery_percent 电量百分比；仅在 power_policy 提供有效电量时可信。
     * @param[in] battery_mv 电池电压，单位为毫伏。
     * @return `ESP_OK` 表示提示请求已接受。
     */
    esp_err_t app_alert_manager_set_low_battery_warning(bool visible,
                                                        uint8_t battery_percent,
                                                        uint16_t battery_mv);

    /**
     * @brief 单独开关 `traffic_audio` 对应的屏幕覆盖层。
     * @param[in] enabled true 表示允许显示覆盖层。
     * @return `ESP_OK` 表示设置成功；其他错误表示管理器尚未初始化。
     */
    esp_err_t app_alert_manager_set_traffic_audio_overlay_enabled(bool enabled);

#ifdef __cplusplus
}
#endif

#endif // APP_ALERT_MANAGER_H
