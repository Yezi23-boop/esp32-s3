#include "services/fall_detection_service.h"

#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fall_model_runner.h"
#include "features/alerts/app_alert_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "services/sensors/imu_service.h"
#include "services/memory_watch/watch_endpoint_service.h"

static const char *k_fall_app_danger_type = "fall";
static const char *k_fall_app_message = "检测到跌倒";

extern const uint8_t cnn_v1_recall90_6ch_2s_with_test_espdl[]
    asm("_binary_cnn_v1_recall90_6ch_2s_with_test_espdl_start");

static const char *TAG = "fall_detection";

static const UBaseType_t k_window_queue_length = 1U;
static const uint32_t k_task_stack_bytes = 6144U;
static const UBaseType_t k_task_priority = 2U;
static const char *k_model_name = "cnn_v1_recall90_6ch_2s";
/* V1 recall90 调试模型输入：50Hz × 2s × 6ch = 100 × 6 = 600。 */
static const uint16_t k_model_pre_event_frames = 35U;
static const float k_degrees_to_radians = 0.017453292519943295f;
static const float k_radians_to_degrees = 57.29577951308232f;
static const float k_fall_clear_threshold = 0.50f; // 后续事件窗口低风险时允许提前解除已确认跌倒。
static const uint32_t k_fall_clear_window_count = 2U; // 仅作为事件窗口低风险恢复证据。
static const TickType_t k_alert_receive_timeout_ticks = pdMS_TO_TICKS(250); // 兼作一键销毁请求的最大响应延迟。
static const int64_t k_fall_alert_auto_clear_us = 5LL * 1000LL * 1000LL; // 本地红屏/告警最多保持 5 秒。
static const uint16_t k_post_gravity_start_frame = 150U; // 事件后 2~3 秒，用于姿态变化。
static const uint16_t k_post_gravity_frame_count = 50U;
static const uint16_t k_low_motion_start_frame = 150U; // 事件后 2~5 秒，用于摔后静止确认。
static const uint16_t k_low_motion_frame_count = 150U;
static const float k_posture_change_threshold_deg = 45.0f;
static const float k_low_motion_gyro_mean_max_radps = 0.44f;
static const float k_low_motion_gyro_peak_max_radps = 1.40f;
static const float k_low_motion_acc_norm_std_max_mps2 = 1.96f;
static const float k_strong_fall_candidate_threshold = 0.90f;

typedef enum
{
    FALL_DETECTION_WINDOW_CONTRACT_OK = 0,
    FALL_DETECTION_WINDOW_CONTRACT_BASIC_MISMATCH = 1U << 0,
    FALL_DETECTION_WINDOW_CONTRACT_MODEL_SLICE = 1U << 1,
    FALL_DETECTION_WINDOW_CONTRACT_POST_CHECK = 1U << 2,
} fall_detection_window_contract_error_t;

typedef enum
{
    FALL_DETECTION_ALERT_EVENT_NONE = 0,
    FALL_DETECTION_ALERT_EVENT_CONFIRMED,
    FALL_DETECTION_ALERT_EVENT_CLEARED,
} fall_detection_alert_event_type_t;

typedef struct
{
    fall_detection_alert_event_type_t type;
    uint32_t alert_sequence;
    uint32_t window_sequence;
    uint32_t clear_window_count;
    float fall_prob;
    int64_t window_end_time_us;
} fall_detection_alert_event_t;

typedef struct
{
    bool is_candidate;
    bool low_motion;
    bool posture_change;
    bool strong_fallback;
    bool confirmed;
    float gyro_norm_mean_radps;
    float gyro_norm_max_radps;
    float acc_norm_std_mps2;
    float posture_angle_deg;
} fall_detection_post_check_t;

typedef struct
{
    bool started;
    bool destroy_requested;
    TaskHandle_t task_handle;
    portMUX_TYPE lock;
    fall_detection_service_snapshot_t snapshot;
    QueueHandle_t window_queue;
    StaticQueue_t window_queue_control;
    uint8_t *window_queue_storage;
    fall_model_runner_t *runner;
    imu_service_accel_window_t *current_window;
    float *model_input;
    int64_t last_alert_time_us;
} fall_detection_context_t;

static fall_detection_context_t s_fall_detection = {
    .started = false,
    .destroy_requested = false,
    .task_handle = NULL,
    .lock = portMUX_INITIALIZER_UNLOCKED,
    .snapshot = {
        .state = FALL_DETECTION_SERVICE_STATE_STOPPED,
        .model_ready = false,
        .self_test_passed = false,
        .threshold = FALL_MODEL_THRESHOLD_DEFAULT,
        .alert_state = FALL_DETECTION_ALERT_STATE_IDLE,
        .last_error = ESP_OK,
        .last_alert_error = ESP_OK,
        .label_index = -1,
    },
    .window_queue = NULL,
    .window_queue_storage = NULL,
    .runner = NULL,
    .current_window = NULL,
    .model_input = NULL,
    .last_alert_time_us = 0,
};

static void fall_detection_store_state(fall_detection_service_state_t state,
                                       esp_err_t error)
{
    taskENTER_CRITICAL(&s_fall_detection.lock);
    s_fall_detection.snapshot.state = state;
    s_fall_detection.snapshot.last_error = error;
    taskEXIT_CRITICAL(&s_fall_detection.lock);
}

static void fall_detection_store_model_ready(bool self_test_passed)
{
    taskENTER_CRITICAL(&s_fall_detection.lock);
    s_fall_detection.snapshot.model_ready = true;
    s_fall_detection.snapshot.self_test_passed = self_test_passed;
    s_fall_detection.snapshot.threshold = FALL_MODEL_THRESHOLD_DEFAULT;
    s_fall_detection.snapshot.last_error = ESP_OK;
    taskEXIT_CRITICAL(&s_fall_detection.lock);
}

static void fall_detection_store_window_received(
    const imu_service_accel_window_t *window)
{
    taskENTER_CRITICAL(&s_fall_detection.lock);
    s_fall_detection.snapshot.window_count++;
    s_fall_detection.snapshot.last_window_sequence = window->sequence;
    s_fall_detection.snapshot.last_window_end_time_us = window->end_time_us;
    taskEXIT_CRITICAL(&s_fall_detection.lock);
}

static void fall_detection_store_result(const imu_service_accel_window_t *window,
                                        const fall_model_result_t *result)
{
    taskENTER_CRITICAL(&s_fall_detection.lock);
    s_fall_detection.snapshot.state = FALL_DETECTION_SERVICE_STATE_RUNNING;
    s_fall_detection.snapshot.inference_count++;
    s_fall_detection.snapshot.last_window_sequence = window->sequence;
    s_fall_detection.snapshot.last_window_end_time_us = window->end_time_us;
    s_fall_detection.snapshot.label_index = result->label_index;
    s_fall_detection.snapshot.confidence = result->confidence;
    s_fall_detection.snapshot.adl_prob = result->adl_prob;
    s_fall_detection.snapshot.fall_prob = result->fall_prob;
    s_fall_detection.snapshot.infer_us = result->infer_us;
    s_fall_detection.snapshot.last_error = ESP_OK;
    taskEXIT_CRITICAL(&s_fall_detection.lock);
}

static void fall_detection_store_alert_error(esp_err_t error)
{
    taskENTER_CRITICAL(&s_fall_detection.lock);
    s_fall_detection.snapshot.last_alert_error = error;
    taskEXIT_CRITICAL(&s_fall_detection.lock);
}

static void fall_detection_store_inference_error(esp_err_t error)
{
    taskENTER_CRITICAL(&s_fall_detection.lock);
    s_fall_detection.snapshot.state = FALL_DETECTION_SERVICE_STATE_ERROR;
    s_fall_detection.snapshot.inference_error_count++;
    s_fall_detection.snapshot.last_error = error;
    taskEXIT_CRITICAL(&s_fall_detection.lock);
}

static esp_err_t fall_detection_prepare_buffers(void)
{
    if (s_fall_detection.window_queue_storage == NULL)
    {
        s_fall_detection.window_queue_storage =
            (uint8_t *)heap_caps_malloc(
                k_window_queue_length * sizeof(imu_service_accel_window_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_fall_detection.window_queue_storage == NULL)
        {
            ESP_LOGE(TAG, "window queue psram allocation failed");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_fall_detection.current_window == NULL)
    {
        s_fall_detection.current_window =
            (imu_service_accel_window_t *)heap_caps_calloc(
                1U, sizeof(imu_service_accel_window_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_fall_detection.current_window == NULL)
        {
            ESP_LOGE(TAG, "current window psram allocation failed");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_fall_detection.model_input == NULL)
    {
        s_fall_detection.model_input =
            (float *)heap_caps_calloc(FALL_MODEL_INPUT_ELEMENTS,
                                      sizeof(float),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_fall_detection.model_input == NULL)
        {
            ESP_LOGE(TAG, "model input psram allocation failed");
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

static QueueHandle_t fall_detection_prepare_window_queue(void)
{
    if (s_fall_detection.window_queue != NULL)
    {
        return s_fall_detection.window_queue;
    }

    s_fall_detection.window_queue = xQueueCreateStatic(
        k_window_queue_length,
        sizeof(imu_service_accel_window_t),
        s_fall_detection.window_queue_storage,
        &s_fall_detection.window_queue_control);
    return s_fall_detection.window_queue;
}

static void fall_detection_release_runtime_resources(void)
{
    if (s_fall_detection.runner != NULL)
    {
        fall_model_runner_destroy(s_fall_detection.runner);
        s_fall_detection.runner = NULL;
    }

    if (s_fall_detection.window_queue != NULL)
    {
        vQueueDelete(s_fall_detection.window_queue);
        s_fall_detection.window_queue = NULL;
    }

    if (s_fall_detection.window_queue_storage != NULL)
    {
        heap_caps_free(s_fall_detection.window_queue_storage);
        s_fall_detection.window_queue_storage = NULL;
    }

    if (s_fall_detection.current_window != NULL)
    {
        heap_caps_free(s_fall_detection.current_window);
        s_fall_detection.current_window = NULL;
    }

    if (s_fall_detection.model_input != NULL)
    {
        heap_caps_free(s_fall_detection.model_input);
        s_fall_detection.model_input = NULL;
    }

    memset(&s_fall_detection.window_queue_control, 0,
           sizeof(s_fall_detection.window_queue_control));
}

static void fall_detection_store_destroyed(esp_err_t error)
{
    taskENTER_CRITICAL(&s_fall_detection.lock);
    s_fall_detection.started = false;
    s_fall_detection.destroy_requested = false;
    s_fall_detection.task_handle = NULL;
    s_fall_detection.snapshot.state = FALL_DETECTION_SERVICE_STATE_STOPPED;
    s_fall_detection.snapshot.model_ready = false;
    s_fall_detection.snapshot.self_test_passed = false;
    s_fall_detection.snapshot.window_count = 0;
    s_fall_detection.snapshot.inference_count = 0;
    s_fall_detection.snapshot.inference_error_count = 0;
    s_fall_detection.snapshot.last_window_sequence = 0;
    s_fall_detection.snapshot.last_window_end_time_us = 0;
    s_fall_detection.snapshot.label_index = -1;
    s_fall_detection.snapshot.confidence = 0.0f;
    s_fall_detection.snapshot.adl_prob = 0.0f;
    s_fall_detection.snapshot.fall_prob = 0.0f;
    s_fall_detection.snapshot.threshold = FALL_MODEL_THRESHOLD_DEFAULT;
    s_fall_detection.snapshot.infer_us = 0;
    s_fall_detection.snapshot.alert_state = FALL_DETECTION_ALERT_STATE_IDLE;
    s_fall_detection.snapshot.clear_window_count = 0;
    s_fall_detection.snapshot.last_alert_window_sequence = 0;
    s_fall_detection.snapshot.last_alert_fall_prob = 0.0f;
    s_fall_detection.snapshot.last_alert_error = ESP_OK;
    s_fall_detection.snapshot.last_error = error;
    s_fall_detection.last_alert_time_us = 0;
    taskEXIT_CRITICAL(&s_fall_detection.lock);
}

static void fall_detection_store_start_error(esp_err_t error)
{
    taskENTER_CRITICAL(&s_fall_detection.lock);
    s_fall_detection.started = false;
    s_fall_detection.destroy_requested = false;
    s_fall_detection.task_handle = NULL;
    s_fall_detection.snapshot.state = FALL_DETECTION_SERVICE_STATE_ERROR;
    s_fall_detection.snapshot.model_ready = false;
    s_fall_detection.snapshot.self_test_passed = false;
    s_fall_detection.snapshot.alert_state = FALL_DETECTION_ALERT_STATE_IDLE;
    s_fall_detection.snapshot.clear_window_count = 0;
    s_fall_detection.snapshot.last_error = error;
    s_fall_detection.last_alert_time_us = 0;
    taskEXIT_CRITICAL(&s_fall_detection.lock);
}

static bool fall_detection_destroy_requested(void)
{
    taskENTER_CRITICAL(&s_fall_detection.lock);
    const bool requested = s_fall_detection.destroy_requested;
    taskEXIT_CRITICAL(&s_fall_detection.lock);
    return requested;
}

static uint32_t fall_detection_validate_window_contract(
    const imu_service_accel_window_t *window)
{
    uint32_t errors = FALL_DETECTION_WINDOW_CONTRACT_OK;
    if (window->frame_count != IMU_SERVICE_WINDOW_FRAME_COUNT ||
        window->sample_rate_hz != IMU_SERVICE_SAMPLE_RATE_HZ ||
        window->trigger_frame_index != IMU_SERVICE_EVENT_PRE_FRAMES)
    {
        errors |= FALL_DETECTION_WINDOW_CONTRACT_BASIC_MISMATCH;
    }

    if (window->trigger_frame_index < k_model_pre_event_frames)
    {
        errors |= FALL_DETECTION_WINDOW_CONTRACT_MODEL_SLICE;
    }
    else
    {
        const uint16_t model_start_frame =
            (uint16_t)(window->trigger_frame_index - k_model_pre_event_frames);
        if ((model_start_frame + FALL_MODEL_FRAME_COUNT) > window->frame_count)
        {
            errors |= FALL_DETECTION_WINDOW_CONTRACT_MODEL_SLICE;
        }
    }

    if ((k_post_gravity_start_frame + k_post_gravity_frame_count) >
            window->frame_count ||
        (k_low_motion_start_frame + k_low_motion_frame_count) >
            window->frame_count ||
        IMU_SERVICE_EVENT_PRE_FRAMES > window->frame_count)
    {
        errors |= FALL_DETECTION_WINDOW_CONTRACT_POST_CHECK;
    }
    return errors;
}

/**
 * @brief 从 6s 事件窗口提取 2s 子窗口并填充为 V1 6ch 模型输入。
 *
 * 布局按帧交错：[accX, accY, accZ, gyroX, gyroY, gyroZ] × 100帧。
 * 输入窗口已是 imu_service 输出的修正后右手系板级物理轴，禁止再套旧 raw chip
 * 轴的 X 反号。坐标映射沿用当前 V1 训练约定：
 *   model accX  =  imu accX
 *   model accY  =  imu accY
 *   model accZ  = -imu accZ
 *   model gyroX =  imu gyroX
 *   model gyroY =  imu gyroY
 *   model gyroZ = -imu gyroZ
 * gyro 从 deg/s 转换为 rad/s。
 */
static void fall_detection_fill_model_input(
    const imu_service_accel_window_t *window,
    float input[FALL_MODEL_INPUT_ELEMENTS])
{
    const uint16_t model_start_frame =
        (uint16_t)(window->trigger_frame_index - k_model_pre_event_frames);
    for (uint16_t frame = 0; frame < FALL_MODEL_FRAME_COUNT; ++frame)
    {
        const uint16_t window_frame = (uint16_t)(model_start_frame + frame);
        const uint16_t base = frame * FALL_MODEL_CHANNEL_COUNT;
        /* 加速度：m/s^2，按坐标映射 */
        input[base + 0U] =  window->accel[window_frame].x;
        input[base + 1U] =  window->accel[window_frame].y;
        input[base + 2U] = -window->accel[window_frame].z;
        /* 陀螺仪：deg/s -> rad/s，按坐标映射 */
        input[base + 3U] =  window->gyro[window_frame].x * k_degrees_to_radians;
        input[base + 4U] =  window->gyro[window_frame].y * k_degrees_to_radians;
        input[base + 5U] = -window->gyro[window_frame].z * k_degrees_to_radians;
    }
}

static float fall_detection_accel_norm_mps2(const imu_sensor_accel_t *accel)
{
    return sqrtf((accel->x * accel->x) +
                 (accel->y * accel->y) +
                 (accel->z * accel->z));
}

static float fall_detection_gyro_norm_radps(const imu_sensor_gyro_t *gyro)
{
    const float x = gyro->x * k_degrees_to_radians;
    const float y = gyro->y * k_degrees_to_radians;
    const float z = gyro->z * k_degrees_to_radians;
    return sqrtf((x * x) + (y * y) + (z * z));
}

static imu_sensor_accel_t fall_detection_average_accel(
    const imu_service_accel_window_t *window,
    uint16_t start_frame,
    uint16_t frame_count)
{
    imu_sensor_accel_t average = {0};
    for (uint16_t frame = 0; frame < frame_count; ++frame)
    {
        const imu_sensor_accel_t *accel = &window->accel[start_frame + frame];
        average.x += accel->x;
        average.y += accel->y;
        average.z += accel->z;
    }
    const float scale = 1.0f / (float)frame_count;
    average.x *= scale;
    average.y *= scale;
    average.z *= scale;
    return average;
}

static float fall_detection_gravity_angle_deg(const imu_sensor_accel_t *pre,
                                              const imu_sensor_accel_t *post)
{
    const float pre_norm = fall_detection_accel_norm_mps2(pre);
    const float post_norm = fall_detection_accel_norm_mps2(post);
    if (pre_norm <= 0.001f || post_norm <= 0.001f)
    {
        return 0.0f;
    }

    const float dot = (pre->x * post->x) +
                      (pre->y * post->y) +
                      (pre->z * post->z);
    float cosine = dot / (pre_norm * post_norm);
    if (cosine > 1.0f)
    {
        cosine = 1.0f;
    }
    else if (cosine < -1.0f)
    {
        cosine = -1.0f;
    }
    return acosf(cosine) * k_radians_to_degrees;
}

static fall_detection_post_check_t fall_detection_run_post_check(
    const imu_service_accel_window_t *window,
    const fall_model_result_t *result)
{
    fall_detection_post_check_t check = {
        .is_candidate = result->fall_prob >= FALL_MODEL_THRESHOLD_DEFAULT,
    };
    if (!check.is_candidate)
    {
        return check;
    }

    float gyro_norm_sum = 0.0f;
    float gyro_norm_max = 0.0f;
    float acc_norm_sum = 0.0f;
    float acc_norm_square_sum = 0.0f;
    for (uint16_t frame = 0; frame < k_low_motion_frame_count; ++frame)
    {
        const uint16_t window_frame =
            (uint16_t)(k_low_motion_start_frame + frame);
        const float gyro_norm =
            fall_detection_gyro_norm_radps(&window->gyro[window_frame]);
        const float acc_norm =
            fall_detection_accel_norm_mps2(&window->accel[window_frame]);
        gyro_norm_sum += gyro_norm;
        if (gyro_norm > gyro_norm_max)
        {
            gyro_norm_max = gyro_norm;
        }
        acc_norm_sum += acc_norm;
        acc_norm_square_sum += acc_norm * acc_norm;
    }

    const float low_motion_count = (float)k_low_motion_frame_count;
    check.gyro_norm_mean_radps = gyro_norm_sum / low_motion_count;
    check.gyro_norm_max_radps = gyro_norm_max;
    const float acc_norm_mean = acc_norm_sum / low_motion_count;
    float acc_norm_variance =
        (acc_norm_square_sum / low_motion_count) -
        (acc_norm_mean * acc_norm_mean);
    if (acc_norm_variance < 0.0f)
    {
        acc_norm_variance = 0.0f;
    }
    check.acc_norm_std_mps2 = sqrtf(acc_norm_variance);
    check.low_motion =
        check.gyro_norm_mean_radps < k_low_motion_gyro_mean_max_radps &&
        check.gyro_norm_max_radps < k_low_motion_gyro_peak_max_radps &&
        check.acc_norm_std_mps2 < k_low_motion_acc_norm_std_max_mps2;

    const imu_sensor_accel_t pre_gravity =
        fall_detection_average_accel(window, 0U, IMU_SERVICE_EVENT_PRE_FRAMES);
    const imu_sensor_accel_t post_gravity =
        fall_detection_average_accel(window,
                                     k_post_gravity_start_frame,
                                     k_post_gravity_frame_count);
    check.posture_angle_deg =
        fall_detection_gravity_angle_deg(&pre_gravity, &post_gravity);
    check.posture_change =
        check.posture_angle_deg > k_posture_change_threshold_deg;

    check.strong_fallback =
        result->fall_prob >= k_strong_fall_candidate_threshold &&
        check.low_motion;
    check.confirmed =
        check.low_motion &&
        (check.posture_change || check.strong_fallback);
    return check;
}

static fall_detection_alert_event_t fall_detection_update_alert_state(
    const imu_service_accel_window_t *window,
    const fall_model_result_t *result,
    const fall_detection_post_check_t *post_check)
{
    const bool is_event_window = window->trigger_flags != 0U;
    fall_detection_alert_event_t event = {
        .type = FALL_DETECTION_ALERT_EVENT_NONE,
        .window_sequence = window->sequence,
        .fall_prob = result->fall_prob,
        .window_end_time_us = window->end_time_us,
    };

    taskENTER_CRITICAL(&s_fall_detection.lock);
    if (s_fall_detection.snapshot.alert_state ==
        FALL_DETECTION_ALERT_STATE_IDLE)
    {
        s_fall_detection.snapshot.clear_window_count = 0;
        if (is_event_window &&
            post_check->is_candidate &&
            post_check->confirmed)
        {
            s_fall_detection.snapshot.alert_state =
                FALL_DETECTION_ALERT_STATE_CONFIRMED;
            s_fall_detection.snapshot.alert_sequence++;
            s_fall_detection.snapshot.last_alert_window_sequence =
                window->sequence;
            s_fall_detection.snapshot.last_alert_fall_prob =
                result->fall_prob;
            s_fall_detection.snapshot.last_alert_error = ESP_OK;
            s_fall_detection.last_alert_time_us = window->end_time_us;

            event.type = FALL_DETECTION_ALERT_EVENT_CONFIRMED;
            event.alert_sequence = s_fall_detection.snapshot.alert_sequence;
        }
    }
    else if (result->fall_prob < k_fall_clear_threshold)
    {
        s_fall_detection.snapshot.clear_window_count++;
        event.clear_window_count =
            s_fall_detection.snapshot.clear_window_count;
        if (s_fall_detection.snapshot.clear_window_count >=
            k_fall_clear_window_count)
        {
            s_fall_detection.snapshot.alert_state =
                FALL_DETECTION_ALERT_STATE_IDLE;
            s_fall_detection.snapshot.clear_window_count = 0;
            s_fall_detection.last_alert_time_us = 0;
            event.type = FALL_DETECTION_ALERT_EVENT_CLEARED;
            event.alert_sequence = s_fall_detection.snapshot.alert_sequence;
        }
    }
    else
    {
        s_fall_detection.snapshot.clear_window_count = 0;
    }
    taskEXIT_CRITICAL(&s_fall_detection.lock);

    return event;
}

static fall_detection_alert_event_t fall_detection_update_alert_timeout(
    int64_t now_us)
{
    fall_detection_alert_event_t event = {
        .type = FALL_DETECTION_ALERT_EVENT_NONE,
        .window_end_time_us = now_us,
    };

    taskENTER_CRITICAL(&s_fall_detection.lock);
    if (s_fall_detection.snapshot.alert_state ==
            FALL_DETECTION_ALERT_STATE_CONFIRMED &&
        s_fall_detection.last_alert_time_us > 0 &&
        (now_us - s_fall_detection.last_alert_time_us) >=
            k_fall_alert_auto_clear_us)
    {
        s_fall_detection.snapshot.alert_state =
            FALL_DETECTION_ALERT_STATE_IDLE;
        s_fall_detection.snapshot.clear_window_count = 0;
        s_fall_detection.last_alert_time_us = 0;

        event.type = FALL_DETECTION_ALERT_EVENT_CLEARED;
        event.alert_sequence = s_fall_detection.snapshot.alert_sequence;
        event.window_sequence =
            s_fall_detection.snapshot.last_alert_window_sequence;
        event.fall_prob = s_fall_detection.snapshot.last_alert_fall_prob;
    }
    taskEXIT_CRITICAL(&s_fall_detection.lock);

    return event;
}

static esp_err_t fall_detection_raise_local_alert(void)
{
    const app_alert_request_t request = {
        .source = APP_ALERT_SOURCE_FALL_DETECTION,
        .severity = APP_ALERT_SEVERITY_DANGER,
        .label = APP_ALERT_LABEL_FALL,
    };
    return app_alert_manager_raise(&request);
}

static esp_err_t fall_detection_post_app_alert(
    const fall_detection_alert_event_t *event)
{
    const watch_endpoint_danger_alert_t alert = {
        .danger_type = k_fall_app_danger_type,
        .danger_prob = event->fall_prob,
        .alert_sequence = event->alert_sequence,
        .message = k_fall_app_message,
    };
    return watch_endpoint_service_post_danger_alert(&alert);
}

static void fall_detection_handle_alert_event(
    const fall_detection_alert_event_t *event)
{
    if (event->type == FALL_DETECTION_ALERT_EVENT_CONFIRMED)
    {
        ESP_LOGW(TAG,
                 "跌倒告警已确认: 告警序号=%u 窗口序号=%u 跌倒概率=%.4f 阈值=%.2f 窗口结束_us=%lld",
                 (unsigned)event->alert_sequence,
                 (unsigned)event->window_sequence,
                 (double)event->fall_prob,
                 (double)FALL_MODEL_THRESHOLD_DEFAULT,
                 (long long)event->window_end_time_us);

        esp_err_t local_ret = fall_detection_raise_local_alert();
        if (local_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "本地跌倒告警失败: 窗口序号=%u 错误=%s",
                     (unsigned)event->window_sequence,
                     esp_err_to_name(local_ret));
        }

        esp_err_t app_ret = fall_detection_post_app_alert(event);
        if (app_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "跌倒App上传失败: 窗口序号=%u 错误=%s",
                     (unsigned)event->window_sequence,
                     esp_err_to_name(app_ret));
        }
        else
        {
            ESP_LOGI(TAG,
                     "跌倒App上传已入队: 告警序号=%u 窗口序号=%u 重试=0",
                     (unsigned)event->alert_sequence,
                     (unsigned)event->window_sequence);
        }

        ESP_LOGI(TAG,
                 "跌倒危险语言播发已跳过: 告警序号=%u 窗口序号=%u 默认保留App上传",
                 (unsigned)event->alert_sequence,
                 (unsigned)event->window_sequence);
        fall_detection_store_alert_error(local_ret != ESP_OK ? local_ret
                                                             : app_ret);
    }
    else if (event->type == FALL_DETECTION_ALERT_EVENT_CLEARED)
    {
        esp_err_t clear_ret =
            app_alert_manager_clear(APP_ALERT_SOURCE_FALL_DETECTION);
        if (clear_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "跌倒告警清除失败: 窗口序号=%u 错误=%s",
                     (unsigned)event->window_sequence,
                     esp_err_to_name(clear_ret));
        }
        fall_detection_store_alert_error(clear_ret);
        ESP_LOGI(TAG,
                 "跌倒告警已清除: 告警序号=%u 窗口序号=%u 低风险窗口数=%u 跌倒概率=%.4f 清除阈值=%.2f",
                 (unsigned)event->alert_sequence,
                 (unsigned)event->window_sequence,
                 (unsigned)event->clear_window_count,
                 (double)event->fall_prob,
                 (double)k_fall_clear_threshold);
    }
}

static void fall_detection_clear_alert_for_destroy(void)
{
    bool had_active_alert = false;

    taskENTER_CRITICAL(&s_fall_detection.lock);
    if (s_fall_detection.snapshot.alert_state ==
        FALL_DETECTION_ALERT_STATE_CONFIRMED)
    {
        s_fall_detection.snapshot.alert_state =
            FALL_DETECTION_ALERT_STATE_IDLE;
        s_fall_detection.snapshot.clear_window_count = 0;
        s_fall_detection.last_alert_time_us = 0;
        had_active_alert = true;
    }
    taskEXIT_CRITICAL(&s_fall_detection.lock);

    if (had_active_alert)
    {
        const esp_err_t clear_ret =
            app_alert_manager_clear(APP_ALERT_SOURCE_FALL_DETECTION);
        fall_detection_store_alert_error(clear_ret);
        if (clear_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "销毁时清除跌倒告警失败: %s",
                     esp_err_to_name(clear_ret));
        }
    }
}

static void fall_detection_task(void *arg)
{
    (void)arg;

    while (1)
    {
        if (fall_detection_destroy_requested())
        {
            break;
        }

        if (xQueueReceive(s_fall_detection.window_queue,
                          s_fall_detection.current_window,
                          k_alert_receive_timeout_ticks) != pdTRUE)
        {
            const fall_detection_alert_event_t timeout_event =
                fall_detection_update_alert_timeout(esp_timer_get_time());
            fall_detection_handle_alert_event(&timeout_event);
            continue;
        }

        if (fall_detection_destroy_requested())
        {
            break;
        }

        const imu_service_accel_window_t *window =
            s_fall_detection.current_window;
        fall_detection_store_window_received(window);

        const uint32_t contract_errors =
            fall_detection_validate_window_contract(window);
        if (contract_errors != FALL_DETECTION_WINDOW_CONTRACT_OK)
        {
            ESP_LOGW(TAG,
                     "事件窗口契约不匹配: 序号=%u 帧数=%u 采样率=%u 触发帧=%u 错误=0x%02x",
                     (unsigned)window->sequence,
                     (unsigned)window->frame_count,
                     (unsigned)window->sample_rate_hz,
                     (unsigned)window->trigger_frame_index,
                     (unsigned)contract_errors);
            fall_detection_store_inference_error(ESP_ERR_INVALID_SIZE);
            continue;
        }

        if (window->trigger_flags == 0U)
        {
            ESP_LOGW(TAG,
                     "非事件窗口已拒绝: 序号=%u 来源采样=%u flags=0x%02x",
                     (unsigned)window->sequence,
                     (unsigned)window->source_sample_count,
                     (unsigned)window->trigger_flags);
            continue;
        }

        fall_detection_fill_model_input(window,
                                        s_fall_detection.model_input);

        fall_model_result_t result = {0};
        const esp_err_t ret = fall_model_runner_run(
            s_fall_detection.runner, s_fall_detection.model_input, &result);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "推理失败: 窗口序号=%u 错误=%s",
                     (unsigned)window->sequence,
                     esp_err_to_name(ret));
            fall_detection_store_inference_error(ret);
            continue;
        }

        fall_detection_store_result(window, &result);
        ESP_LOGI(TAG,
                 "跌倒表 | 窗口=%-4u 来源采样=%-6u 触发采样=%-6u flags=0x%02x | 判定=%s(%d) | 置信度=%.4f 日常=%.4f 跌倒=%.4f 阈值=%.2f | 推理_ms=%6.2f 触发_us=%lld 结束_us=%lld",
                 (unsigned)window->sequence,
                 (unsigned)window->source_sample_count,
                 (unsigned)window->trigger_sample_count,
                 (unsigned)window->trigger_flags,
                 fall_model_label_name(result.label_index),
                 result.label_index,
                 (double)result.confidence,
                 (double)result.adl_prob,
                 (double)result.fall_prob,
                 (double)FALL_MODEL_THRESHOLD_DEFAULT,
                 (double)result.infer_us / 1000.0,
                 (long long)window->trigger_time_us,
                 (long long)window->end_time_us);

        const fall_detection_post_check_t post_check =
            fall_detection_run_post_check(window, &result);
        if (post_check.is_candidate)
        {
            ESP_LOGI(TAG,
                     "候选表 | 窗口=%-4u 来源采样=%-6u flags=0x%02x | fall_prob=%.4f 阈值=%.2f 强兜底阈值=%.2f",
                     (unsigned)window->sequence,
                     (unsigned)window->source_sample_count,
                     (unsigned)window->trigger_flags,
                     (double)result.fall_prob,
                     (double)FALL_MODEL_THRESHOLD_DEFAULT,
                     (double)k_strong_fall_candidate_threshold);
            ESP_LOGI(TAG,
                     "post检查表 | 窗口=%-4u low_motion=%u posture_change=%u strong=%u confirmed=%u | gyro_mean=%.3f_radps gyro_max=%.3f_radps acc_std=%.3f_mps2 posture_angle=%.1f_deg | trigger_acc=%.2f_mps2 trigger_gyro=%.2f_radps trigger_jerk=%.2f_mps2pf",
                     (unsigned)window->sequence,
                     (unsigned)(post_check.low_motion ? 1U : 0U),
                     (unsigned)(post_check.posture_change ? 1U : 0U),
                     (unsigned)(post_check.strong_fallback ? 1U : 0U),
                     (unsigned)(post_check.confirmed ? 1U : 0U),
                     (double)post_check.gyro_norm_mean_radps,
                     (double)post_check.gyro_norm_max_radps,
                     (double)post_check.acc_norm_std_mps2,
                     (double)post_check.posture_angle_deg,
                     (double)window->trigger_acc_norm_mps2,
                     (double)window->trigger_gyro_norm_radps,
                     (double)window->trigger_jerk_mps2_per_frame);
        }

        const fall_detection_alert_event_t alert_event =
            fall_detection_update_alert_state(window, &result, &post_check);
        fall_detection_handle_alert_event(&alert_event);

        const fall_detection_alert_event_t timeout_event =
            fall_detection_update_alert_timeout(esp_timer_get_time());
        fall_detection_handle_alert_event(&timeout_event);
    }

    fall_detection_clear_alert_for_destroy();
    fall_detection_release_runtime_resources();
    fall_detection_store_destroyed(ESP_OK);
    ESP_LOGI(TAG, "已销毁: 模型runner和PSRAM窗口缓冲已释放");
    vTaskDelete(NULL);
}

esp_err_t fall_detection_service_start(void)
{
    taskENTER_CRITICAL(&s_fall_detection.lock);
    const bool already_started = s_fall_detection.started;
    const fall_detection_service_state_t state =
        s_fall_detection.snapshot.state;
    taskEXIT_CRITICAL(&s_fall_detection.lock);
    if (already_started)
    {
        return ESP_OK;
    }
    if (state == FALL_DETECTION_SERVICE_STATE_STOPPING)
    {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_fall_detection.lock);
    s_fall_detection.destroy_requested = false;
    s_fall_detection.snapshot.state = FALL_DETECTION_SERVICE_STATE_STARTING;
    s_fall_detection.snapshot.last_error = ESP_OK;
    taskEXIT_CRITICAL(&s_fall_detection.lock);

    esp_err_t ret = fall_detection_prepare_buffers();
    if (ret != ESP_OK)
    {
        fall_detection_release_runtime_resources();
        fall_detection_store_start_error(ret);
        return ret;
    }

    QueueHandle_t queue = fall_detection_prepare_window_queue();
    if (queue == NULL)
    {
        fall_detection_release_runtime_resources();
        fall_detection_store_start_error(ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    ret = fall_model_runner_create(
        &s_fall_detection.runner,
        cnn_v1_recall90_6ch_2s_with_test_espdl,
        k_model_name);
    if (ret != ESP_OK)
    {
        fall_detection_release_runtime_resources();
        fall_detection_store_start_error(ret);
        return ret;
    }

    ret = fall_model_runner_self_test(s_fall_detection.runner);
    if (ret != ESP_OK)
    {
        fall_detection_release_runtime_resources();
        fall_detection_store_start_error(ret);
        return ret;
    }
    fall_detection_store_model_ready(true);

    ret = imu_service_set_window_queue(queue);
    if (ret != ESP_OK)
    {
        fall_detection_release_runtime_resources();
        fall_detection_store_start_error(ret);
        return ret;
    }

    const BaseType_t created = xTaskCreateWithCaps(
        fall_detection_task,
        "fall_detect",
        k_task_stack_bytes,
        NULL,
        k_task_priority,
        &s_fall_detection.task_handle,
        MALLOC_CAP_SPIRAM);
    if (created != pdPASS)
    {
        (void)imu_service_set_window_queue(NULL);
        fall_detection_release_runtime_resources();
        fall_detection_store_start_error(ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    taskENTER_CRITICAL(&s_fall_detection.lock);
    s_fall_detection.started = true;
    s_fall_detection.snapshot.state = FALL_DETECTION_SERVICE_STATE_RUNNING;
    s_fall_detection.snapshot.last_error = ESP_OK;
    taskEXIT_CRITICAL(&s_fall_detection.lock);

    ESP_LOGI(TAG, "已启动: 模型=%s 阈值=%.2f 队列长度=%u",
             k_model_name,
             (double)FALL_MODEL_THRESHOLD_DEFAULT,
             (unsigned)k_window_queue_length);
    return ESP_OK;
}

esp_err_t fall_detection_service_destroy(void)
{
    TaskHandle_t task = NULL;
    bool task_active = false;
    bool has_runtime_resources = false;

    taskENTER_CRITICAL(&s_fall_detection.lock);
    task = s_fall_detection.task_handle;
    task_active = task != NULL;
    has_runtime_resources = s_fall_detection.started ||
                            task_active ||
                            s_fall_detection.runner != NULL ||
                            s_fall_detection.window_queue != NULL ||
                            s_fall_detection.window_queue_storage != NULL ||
                            s_fall_detection.current_window != NULL ||
                            s_fall_detection.model_input != NULL;
    if (task_active)
    {
        s_fall_detection.destroy_requested = true;
        s_fall_detection.snapshot.state =
            FALL_DETECTION_SERVICE_STATE_STOPPING;
        s_fall_detection.snapshot.last_error = ESP_OK;
    }
    taskEXIT_CRITICAL(&s_fall_detection.lock);

    if (!has_runtime_resources)
    {
        fall_detection_store_destroyed(ESP_OK);
        return ESP_OK;
    }

    /*
     * 只断开 fall 模型窗口消费者，保留 imu_service 后台采样。
     * runner/queue/buffer 由 fall task 在退出点释放，避免推理中途被外部删除。
     */
    (void)imu_service_set_window_queue(NULL);

    if (task_active)
    {
        xTaskNotifyGive(task);
        ESP_LOGI(TAG, "正在销毁: 已断开IMU窗口队列，等待fall任务释放模型资源");
        return ESP_OK;
    }

    fall_detection_clear_alert_for_destroy();
    fall_detection_release_runtime_resources();
    fall_detection_store_destroyed(ESP_OK);
    ESP_LOGI(TAG, "已销毁: 模型runner和PSRAM窗口缓冲已释放");
    return ESP_OK;
}

esp_err_t fall_detection_service_get_snapshot(fall_detection_service_snapshot_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_fall_detection.lock);
    *out = s_fall_detection.snapshot;
    taskEXIT_CRITICAL(&s_fall_detection.lock);
    return ESP_OK;
}

const char *fall_detection_service_state_text(fall_detection_service_state_t state)
{
    switch (state)
    {
    case FALL_DETECTION_SERVICE_STATE_STOPPED:
        return "stopped";
    case FALL_DETECTION_SERVICE_STATE_STARTING:
        return "starting";
    case FALL_DETECTION_SERVICE_STATE_RUNNING:
        return "running";
    case FALL_DETECTION_SERVICE_STATE_STOPPING:
        return "stopping";
    case FALL_DETECTION_SERVICE_STATE_ERROR:
        return "error";
    default:
        return "unknown";
    }
}
