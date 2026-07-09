#ifndef DANGER_DETECTION_SERVICE_H
#define DANGER_DETECTION_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * 危险声音检测服务：
 * - 封装交通声音运行时与后处理告警回调；
 * - 对 UI 暴露统一状态快照，而不是底层推理链路细节；
 * - 负责把 horn / siren 等稳定标签转换成应用级告警动作；
 * - 支持两种推理后端：Edge Impulse (traffic_inference) 和 ESP-DL 单模型 (espdl_inference)。
 */

#ifdef __cplusplus
extern "C"
{
#endif

    /* 危险检测服务生命周期状态。 */
    typedef enum
    {
        DANGER_DETECTION_STATE_IDLE = 0,
        DANGER_DETECTION_STATE_STARTING,
        DANGER_DETECTION_STATE_RUNNING,
        DANGER_DETECTION_STATE_STOPPING,
        DANGER_DETECTION_STATE_ERROR,
    } danger_detection_state_t;

    /* 面向用户提醒策略的危险风险状态。 */
    typedef enum
    {
        DANGER_DETECTION_RISK_OFF = 0,     /**< 服务未运行或不可用。 */
        DANGER_DETECTION_RISK_MONITORING,  /**< 正在监听，当前无稳定危险证据。 */
        DANGER_DETECTION_RISK_SUSPICIOUS,  /**< 已出现高风险窗口，但尚未正式告警。 */
        DANGER_DETECTION_RISK_ALERTING,    /**< 已确认危险，提醒层应保持告警。 */
        DANGER_DETECTION_RISK_COOLDOWN,    /**< 刚解除告警，短暂抑制重复强提醒。 */
    } danger_detection_risk_state_t;

    /* 对外暴露的稳定危险标签。 */
    typedef enum
    {
        DANGER_DETECTION_LABEL_NONE = 0,
        DANGER_DETECTION_LABEL_HORN,  // 喇叭类稳定标签
        DANGER_DETECTION_LABEL_SIREN, // 警笛类稳定标签
        DANGER_DETECTION_LABEL_DANGER,// 通用危险标签（ESP-DL 二分类模式）
    } danger_detection_label_t;

    /* 推理后端类型。 */
    typedef enum
    {
        DANGER_DETECTION_BACKEND_EDGE_IMPULSE = 0, /**< Edge Impulse TFLite 3 分类。 */
        DANGER_DETECTION_BACKEND_ESPDL = 1,        /**< ESP-DL 单模型危险二分类。 */
    } danger_detection_backend_t;

    /* 用户级危险识别灵敏度模式。 */
    typedef enum
    {
        DANGER_DETECTION_SENSITIVITY_CONSERVATIVE = 0, /**< 保守：减少误报。 */
        DANGER_DETECTION_SENSITIVITY_STANDARD,         /**< 标准：日常推荐。 */
        DANGER_DETECTION_SENSITIVITY_SENSITIVE,        /**< 敏感：更容易触发。 */
    } danger_detection_sensitivity_mode_t;

    typedef struct
    {
        danger_detection_state_t state;               /**< 服务当前状态。 */
        danger_detection_risk_state_t risk_state;      /**< 危险提醒状态机当前状态。 */
        danger_detection_label_t stable_label;        /**< 后处理稳定标签。 */
        danger_detection_label_t last_detected_label; /**< 最近一次触发的标签。 */
        float last_detected_confidence;               /**< 最近一次触发的置信度。 */
        float horn_confidence;                        /**< 当前帧 horn 分数。 */
        float siren_confidence;                       /**< 当前帧 siren 分数。 */
        float danger_confidence;                      /**< ESP-DL danger 概率。 */
        uint32_t alert_sequence;                      /**< 告警触发序号，单调递增。 */
        esp_err_t last_error;                         /**< 最近一次错误码。 */
        bool danger_overlay_active;                   /**< UI 危险覆盖层当前是否激活。 */
        danger_detection_backend_t active_backend;    /**< 当前活跃的推理后端。 */
    } danger_detection_snapshot_t;

    /**
     * @brief 危险检测部署策略 profile。
     *
     * 该结构记录当前固件使用的 active danger 口径和后处理参数。它不是用户设置，
     * 而是发布版本的一部分，避免阈值、连续窗口和冷却策略散落在多个模块里。
     */
    typedef struct
    {
        const char *deployment_profile_id; /**< 后处理策略版本标识。 */
        const char *danger_class_profile;  /**< active danger 类别边界。 */
        danger_detection_sensitivity_mode_t sensitivity_mode; /**< 用户级灵敏度模式。 */
        float single_window_threshold;     /**< 单窗 danger 概率阈值。 */
        uint32_t confirm_windows;          /**< 进入正式告警所需连续 danger 窗口数。 */
        uint32_t clear_windows;            /**< 退出正式告警所需连续 non-danger 窗口数。 */
        uint32_t alert_hold_ms;            /**< 正式告警最短保持时间，单位毫秒。 */
        uint32_t cooldown_ms;              /**< 告警解除后的冷却时间，单位毫秒。 */
    } danger_detection_policy_profile_t;

    /**
     * @brief 初始化危险检测服务。
     * @return `ESP_OK` 表示初始化成功或已初始化；其他错误表示依赖模块初始化失败。
     */
    esp_err_t danger_detection_service_init(void);

    /**
     * @brief 启动危险检测运行时（默认使用 ESP-DL 单模型后端）。
     * @return `ESP_OK` 表示已成功启动或之前已在运行；其他错误表示启动失败。
     */
    esp_err_t danger_detection_service_start(void);

    /**
     * @brief 使用指定后端启动危险检测运行时。
     *
     * @param[in] backend 推理后端类型。
     * @return `ESP_OK` 表示已成功启动；其他错误表示启动失败。
     */
    esp_err_t danger_detection_service_start_with_backend(
        danger_detection_backend_t backend);

    /**
     * @brief 停止危险检测运行时。
     * @param[in] timeout_ms 等待底层运行时停止的超时，单位为毫秒；传 `0` 使用默认值。
     * @return `ESP_OK` 表示停止成功；其他错误表示停止或清理回调失败。
     */
    esp_err_t danger_detection_service_stop(uint32_t timeout_ms);

    /**
     * @brief 获取当前危险检测快照。
     * @return 当前服务状态、稳定标签和实时分数的组合快照。
     */
    danger_detection_snapshot_t danger_detection_service_get_snapshot(void);

    /**
     * @brief 获取当前危险检测部署策略 profile。
     *
     * @return 指向静态只读 profile 的指针，调用方不得修改。
     */
    const danger_detection_policy_profile_t *
    danger_detection_service_get_policy_profile(void);

    /**
     * @brief 设置用户级危险识别灵敏度模式。
     *
     * @param[in] mode 灵敏度模式。
     * @return ESP_OK 表示设置成功。
     */
    esp_err_t danger_detection_service_set_sensitivity_mode(
        danger_detection_sensitivity_mode_t mode);

    /**
     * @brief 获取当前用户级危险识别灵敏度模式。
     *
     * @return 当前灵敏度模式；默认是 `DANGER_DETECTION_SENSITIVITY_STANDARD`。
     */
    danger_detection_sensitivity_mode_t
    danger_detection_service_get_sensitivity_mode(void);

    /**
     * @brief 将危险风险状态转换成稳定英文标识。
     *
     * @param[in] risk_state 危险提醒状态机状态。
     * @return 静态字符串，适合日志、调试 UI 和串口输出使用。
     */
    const char *danger_detection_risk_state_text(
        danger_detection_risk_state_t risk_state);

#ifdef __cplusplus
}
#endif

#endif // DANGER_DETECTION_SERVICE_H
