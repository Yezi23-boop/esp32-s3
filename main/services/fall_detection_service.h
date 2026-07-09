#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Fall detection runtime service state.
 */
typedef enum
{
    FALL_DETECTION_SERVICE_STATE_STOPPED = 0, /**< Service has not started. */
    FALL_DETECTION_SERVICE_STATE_STARTING,    /**< Model and queue are being prepared. */
    FALL_DETECTION_SERVICE_STATE_RUNNING,     /**< Waiting for IMU windows or running inference. */
    FALL_DETECTION_SERVICE_STATE_STOPPING,    /**< Destroy requested; runtime task is releasing resources. */
    FALL_DETECTION_SERVICE_STATE_ERROR,       /**< Last start or inference path hit an error. */
} fall_detection_service_state_t;

/**
 * @brief Fall alert state derived from model probabilities.
 */
typedef enum
{
    FALL_DETECTION_ALERT_STATE_IDLE = 0, /**< No active fall alert. */
    FALL_DETECTION_ALERT_STATE_CONFIRMED, /**< Fall alert has been raised once and is awaiting clear. */
} fall_detection_alert_state_t;

/**
 * @brief Read-only fall detection service snapshot.
 */
typedef struct
{
    fall_detection_service_state_t state; /**< Current service state. */
    bool model_ready;                     /**< ESP-DL model loaded successfully. */
    bool self_test_passed;                /**< Embedded ESP-DL test vector passed. */
    uint32_t window_count;                /**< Windows received from `imu_service`. */
    uint32_t inference_count;             /**< Successful inference count. */
    uint32_t inference_error_count;       /**< Failed inference count. */
    uint32_t last_window_sequence;        /**< Latest consumed IMU window sequence. */
    int64_t last_window_end_time_us;      /**< Latest consumed window end timestamp. */
    int label_index;                      /**< Latest decision: 0=ADL, 1=FALL. */
    float confidence;                     /**< Latest selected-label probability. */
    float adl_prob;                       /**< Latest ADL probability. */
    float fall_prob;                      /**< Latest FALL probability. */
    float threshold;                      /**< FALL probability threshold. */
    int64_t infer_us;                     /**< Latest inference time, in microseconds. */
    fall_detection_alert_state_t alert_state; /**< Current fall alert state. */
    uint32_t alert_sequence;              /**< Monotonic fall alert sequence for danger alert upload dedup/debug. */
    uint32_t clear_window_count;          /**< Consecutive low-FALL windows while alert is confirmed. */
    uint32_t last_alert_window_sequence;  /**< IMU window sequence that last confirmed fall. */
    float last_alert_fall_prob;           /**< FALL probability that last confirmed fall. */
    esp_err_t last_alert_error;           /**< Latest local alert or danger alert upload error; `ESP_OK` when accepted. */
    esp_err_t last_error;                 /**< Latest error code; `ESP_OK` when healthy. */
} fall_detection_service_snapshot_t;

/**
 * @brief Start the fall detection runtime service.
 *
 * The service consumes 50Hz event windows from `imu_service`.
 * It does not read IMU hardware directly. Confirmed falls are routed to the
 * common alert manager and watch endpoint exactly once per confirmed episode,
 * while the alert manager keeps fall alerts silent by default and does not
 * play the dangerous-sound warning audio.
 */
esp_err_t fall_detection_service_start(void);

/**
 * @brief 一键销毁跌倒检测运行时并释放模型 RAM。
 *
 * 这是给 UI 或其他 service 调用的对外销毁入口。它只会把 fall 窗口队列
 * 从 `imu_service` 断开，不会停止 IMU 后台采样。若 fall task 正在运行，
 * 销毁是异步完成的：任务观察到请求后在安全点退出循环，释放 ESP-DL
 * runner 和 PSRAM 缓冲，最后把快照状态标记为 STOPPED。
 */
esp_err_t fall_detection_service_destroy(void);

/**
 * @brief Copy the current fall detection service snapshot.
 */
esp_err_t fall_detection_service_get_snapshot(fall_detection_service_snapshot_t *out);

/**
 * @brief Convert service state to stable log text.
 */
const char *fall_detection_service_state_text(fall_detection_service_state_t state);

#ifdef __cplusplus
}
#endif
