#include "danger_detection_service.h"

#include <stdbool.h>
#include <string.h>

#include "features/alerts/app_alert_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "services/watch_endpoint_service.h"
#include "traffic_audio_runtime.h"
#include "traffic_inference_postprocess.h"
#include "espdl_audio_runtime.h"
#include "espdl_model_runner.h"
#include "danger_sample_recorder.h"

#define TAG "danger_detection"
#define DANGER_DETECTION_STOP_TIMEOUT_MS 2000U /**< 默认停止等待时间，单位为毫秒。 */
static const char kDangerAlertCloudMessage[] =
    "检测到危险声音，请注意周围环境";

/*
 * 危险检测服务实现说明：
 * - 支持两种推理后端：Edge Impulse (traffic_inference) 和 ESP-DL 单模型 (espdl_inference)
 * - 统一协调交通声音运行时、后处理告警回调和应用级告警管理器；
 * - 对外发布快照时，会把运行时状态与后处理分数整合到同一结构；
 * - 快照通过临界区保护，避免 UI 在读取时看到半更新状态。
 */

static const danger_detection_policy_profile_t k_espdl_policy_profiles[] = {
    {
        .deployment_profile_id = "espdl_dscnn_v59_core_conservative_t95",
        .danger_class_profile = "core_siren_horn_alarm",
        .sensitivity_mode = DANGER_DETECTION_SENSITIVITY_CONSERVATIVE,
        .single_window_threshold = 0.95f,
        .confirm_windows = 2U,
        .clear_windows = 3U,
        .alert_hold_ms = 2000U,
        .cooldown_ms = 3000U,
    },
    {
        .deployment_profile_id = "espdl_dscnn_v59_core_standard_t90",
        .danger_class_profile = "core_siren_horn_alarm",
        .sensitivity_mode = DANGER_DETECTION_SENSITIVITY_STANDARD,
        .single_window_threshold = 0.90f,
        .confirm_windows = 2U,
        .clear_windows = 3U,
        .alert_hold_ms = 2000U,
        .cooldown_ms = 3000U,
    },
    {
        .deployment_profile_id = "espdl_dscnn_v59_core_sensitive_t85",
        .danger_class_profile = "core_siren_horn_alarm",
        .sensitivity_mode = DANGER_DETECTION_SENSITIVITY_SENSITIVE,
        .single_window_threshold = 0.85f,
        .confirm_windows = 2U,
        .clear_windows = 3U,
        .alert_hold_ms = 2000U,
        .cooldown_ms = 3000U,
    },
};

typedef struct
{
    bool initialized;                     /**< 服务是否完成初始化。 */
    bool callback_registered;             /**< 后处理告警回调是否已注册。 */
    bool runtime_started;                 /**< 音频运行时是否已启动。 */
    danger_detection_backend_t active_backend; /**< 当前活跃的推理后端。 */
    danger_detection_sensitivity_mode_t sensitivity_mode; /**< 当前用户级灵敏度。 */
    danger_detection_snapshot_t snapshot; /**< 对外发布的快照。 */
    portMUX_TYPE lock;                    /**< 快照临界区锁，保护共享状态一致性。 */
} danger_detection_service_state_t;

static danger_detection_service_state_t s_service_state = {
    .initialized = false,
    .callback_registered = false,
    .runtime_started = false,
    .active_backend = DANGER_DETECTION_BACKEND_EDGE_IMPULSE,
    .sensitivity_mode = DANGER_DETECTION_SENSITIVITY_STANDARD,
    .snapshot = {
        .state = DANGER_DETECTION_STATE_IDLE,
        .risk_state = DANGER_DETECTION_RISK_OFF,
        .stable_label = DANGER_DETECTION_LABEL_NONE,
        .last_detected_label = DANGER_DETECTION_LABEL_NONE,
        .last_detected_confidence = 0.0f,
        .horn_confidence = 0.0f,
        .siren_confidence = 0.0f,
        .danger_confidence = 0.0f,
        .alert_sequence = 0U,
        .last_error = ESP_OK,
        .danger_overlay_active = false,
        .active_backend = DANGER_DETECTION_BACKEND_EDGE_IMPULSE,
    },
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static uint32_t s_espdl_danger_window_count = 0U; /**< ESP-DL 连续 danger 窗口计数。 */
static uint32_t s_espdl_clear_window_count = 0U;  /**< ESP-DL 连续 non-danger 窗口计数。 */
static TickType_t s_espdl_hold_until_tick = 0;    /**< ESP-DL 告警保持到期 tick。 */
static TickType_t s_espdl_cooldown_until_tick = 0; /**< ESP-DL 冷却到期 tick。 */

static const danger_detection_policy_profile_t *
danger_detection_profile_for_mode(danger_detection_sensitivity_mode_t mode)
{
    for (size_t i = 0; i < sizeof(k_espdl_policy_profiles) /
                               sizeof(k_espdl_policy_profiles[0]); ++i)
    {
        if (k_espdl_policy_profiles[i].sensitivity_mode == mode)
        {
            return &k_espdl_policy_profiles[i];
        }
    }
    return &k_espdl_policy_profiles[DANGER_DETECTION_SENSITIVITY_STANDARD];
}

static bool danger_detection_is_valid_sensitivity_mode(
    danger_detection_sensitivity_mode_t mode)
{
    return mode == DANGER_DETECTION_SENSITIVITY_CONSERVATIVE ||
           mode == DANGER_DETECTION_SENSITIVITY_STANDARD ||
           mode == DANGER_DETECTION_SENSITIVITY_SENSITIVE;
}

/**
 * @brief 重置 ESP-DL 后处理短状态。
 *
 * 这些计数器不属于 UI 快照，而是运行时回调内部的抗抖状态；启动、停止或错误
 * 退出时都要清零，避免上一次页面会话的窗口计数影响下一次识别。
 */
static void danger_detection_reset_espdl_postprocess(void)
{
    s_espdl_danger_window_count = 0U;
    s_espdl_clear_window_count = 0U;
    s_espdl_hold_until_tick = 0;
    s_espdl_cooldown_until_tick = 0;
}

static bool danger_detection_service_allows_alert_commit(void)
{
    bool runtime_started = false;
    danger_detection_state_t state = DANGER_DETECTION_STATE_IDLE;

    taskENTER_CRITICAL(&s_service_state.lock);
    runtime_started = s_service_state.runtime_started;
    state = s_service_state.snapshot.state;
    taskEXIT_CRITICAL(&s_service_state.lock);

    return runtime_started &&
           state != DANGER_DETECTION_STATE_STOPPING &&
           state != DANGER_DETECTION_STATE_ERROR;
}

/**
 * @brief ESP-DL PCM tap 到 recorder PCM tap 的窄适配层。
 *
 * 两个模块的 meta 字段布局当前一致，但类型不相同；这里显式拷贝字段，
 * 避免通过不兼容函数指针强转调用带来的 ABI 风险。
 */
static void danger_detection_on_espdl_pcm_tap(
    const int16_t *pcm_data,
    size_t samples,
    const espdl_audio_pcm_window_meta_t *meta,
    void *user_data)
{
    (void)user_data;

    if (meta == NULL)
    {
        return;
    }

    danger_sample_pcm_tap_callback_t recorder_callback =
        danger_sample_recorder_get_pcm_callback();
    if (recorder_callback == NULL)
    {
        return;
    }

    const danger_sample_pcm_window_meta_t recorder_meta = {
        .absolute_sample_index = meta->absolute_sample_index,
        .window_samples = meta->window_samples,
        .stride_samples = meta->stride_samples,
    };
    recorder_callback(pcm_data, samples, &recorder_meta, NULL);
}

static const char *danger_detection_cloud_type_from_stable_label(
    traffic_inference_postprocess_stable_label_t label)
{
    switch (label)
    {
    case TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_HORN:
        return "horn";
    case TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_SIREN:
        return "siren";
    case TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_NONE:
    default:
        return "danger";
    }
}

static void danger_detection_post_cloud_alert(const char *danger_type,
                                              float danger_prob,
                                              uint32_t alert_sequence)
{
    const watch_endpoint_danger_alert_t alert = {
        .danger_type = danger_type != NULL ? danger_type : "danger",
        .danger_prob = danger_prob,
        .alert_sequence = alert_sequence,
        .message = kDangerAlertCloudMessage,
    };
    const esp_err_t err = watch_endpoint_service_post_danger_alert(&alert);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "danger cloud alert dispatch skipped: %s",
                 esp_err_to_name(err));
    }
}

const char *danger_detection_risk_state_text(
    danger_detection_risk_state_t risk_state)
{
    switch (risk_state)
    {
    case DANGER_DETECTION_RISK_MONITORING:
        return "MONITORING";
    case DANGER_DETECTION_RISK_SUSPICIOUS:
        return "SUSPICIOUS";
    case DANGER_DETECTION_RISK_ALERTING:
        return "ALERTING";
    case DANGER_DETECTION_RISK_COOLDOWN:
        return "COOLDOWN";
    case DANGER_DETECTION_RISK_OFF:
    default:
        return "OFF";
    }
}

const danger_detection_policy_profile_t *
danger_detection_service_get_policy_profile(void)
{
    danger_detection_sensitivity_mode_t mode;

    taskENTER_CRITICAL(&s_service_state.lock);
    mode = s_service_state.sensitivity_mode;
    taskEXIT_CRITICAL(&s_service_state.lock);
    return danger_detection_profile_for_mode(mode);
}

esp_err_t danger_detection_service_set_sensitivity_mode(
    danger_detection_sensitivity_mode_t mode)
{
    if (!danger_detection_is_valid_sensitivity_mode(mode))
    {
        return ESP_ERR_INVALID_ARG;
    }

    const danger_detection_policy_profile_t *profile =
        danger_detection_profile_for_mode(mode);
    danger_detection_backend_t backend = DANGER_DETECTION_BACKEND_EDGE_IMPULSE;

    taskENTER_CRITICAL(&s_service_state.lock);
    s_service_state.sensitivity_mode = mode;
    backend = s_service_state.active_backend;
    danger_detection_reset_espdl_postprocess();
    taskEXIT_CRITICAL(&s_service_state.lock);

    esp_err_t ret = espdl_audio_runtime_set_danger_threshold(
        profile->single_window_threshold);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ESP_LOGI(TAG, "danger sensitivity mode=%d threshold=%.2f backend=%d",
             mode, profile->single_window_threshold, backend);
    return ESP_OK;
}

danger_detection_sensitivity_mode_t
danger_detection_service_get_sensitivity_mode(void)
{
    danger_detection_sensitivity_mode_t mode;

    taskENTER_CRITICAL(&s_service_state.lock);
    mode = s_service_state.sensitivity_mode;
    taskEXIT_CRITICAL(&s_service_state.lock);
    return mode;
}

/**
 * @brief 将后处理稳定标签映射成服务层标签。
 */
static danger_detection_label_t danger_detection_map_label(
    traffic_inference_postprocess_stable_label_t label)
{
    switch (label)
    {
    case TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_HORN:
        return DANGER_DETECTION_LABEL_HORN;
    case TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_SIREN:
        return DANGER_DETECTION_LABEL_SIREN;
    case TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_NONE:
    default:
        return DANGER_DETECTION_LABEL_NONE;
    }
}

/**
 * @brief 将运行时状态映射成服务层状态。
 */
static danger_detection_state_t danger_detection_map_runtime_state(
    traffic_audio_runtime_state_t runtime_state,
    danger_detection_state_t fallback)
{
    switch (runtime_state)
    {
    case TRAFFIC_AUDIO_RUNTIME_STATE_STARTING:
        return DANGER_DETECTION_STATE_STARTING;
    case TRAFFIC_AUDIO_RUNTIME_STATE_RUNNING:
        return DANGER_DETECTION_STATE_RUNNING;
    case TRAFFIC_AUDIO_RUNTIME_STATE_STOPPING:
        return DANGER_DETECTION_STATE_STOPPING;
    case TRAFFIC_AUDIO_RUNTIME_STATE_FAILED:
        return DANGER_DETECTION_STATE_ERROR;
    case TRAFFIC_AUDIO_RUNTIME_STATE_IDLE:
    default:
        return fallback;
    }
}

/**
 * @brief 将 ESPDL 运行时状态映射成服务层状态。
 */
static danger_detection_state_t danger_detection_map_espdl_state(
    espdl_audio_runtime_state_t runtime_state,
    danger_detection_state_t fallback)
{
    switch (runtime_state)
    {
    case ESPDL_AUDIO_RUNTIME_STATE_STARTING:
        return DANGER_DETECTION_STATE_STARTING;
    case ESPDL_AUDIO_RUNTIME_STATE_RUNNING:
        return DANGER_DETECTION_STATE_RUNNING;
    case ESPDL_AUDIO_RUNTIME_STATE_STOPPING:
        return DANGER_DETECTION_STATE_STOPPING;
    case ESPDL_AUDIO_RUNTIME_STATE_FAILED:
        return DANGER_DETECTION_STATE_ERROR;
    case ESPDL_AUDIO_RUNTIME_STATE_IDLE:
    default:
        return fallback;
    }
}

/**
 * @brief 统一更新服务状态和最近错误码。
 */
static void danger_detection_set_state(danger_detection_state_t state,
                                       esp_err_t last_error)
{
    taskENTER_CRITICAL(&s_service_state.lock);
    s_service_state.snapshot.state = state;
    s_service_state.snapshot.last_error = last_error;
    if (state == DANGER_DETECTION_STATE_IDLE ||
        state == DANGER_DETECTION_STATE_ERROR ||
        state == DANGER_DETECTION_STATE_STOPPING)
    {
        s_service_state.snapshot.risk_state = DANGER_DETECTION_RISK_OFF;
    }
    else if (state == DANGER_DETECTION_STATE_RUNNING &&
             s_service_state.snapshot.risk_state == DANGER_DETECTION_RISK_OFF)
    {
        s_service_state.snapshot.risk_state =
            DANGER_DETECTION_RISK_MONITORING;
    }
    taskEXIT_CRITICAL(&s_service_state.lock);
}

/**
 * @brief Edge Impulse 后处理告警回调。
 */
static void danger_detection_on_alert(
    const traffic_inference_postprocess_alert_t *alert,
    void *user_data)
{
    (void)user_data;

    if (alert == NULL)
    {
        return;
    }

    if (alert->action == TRAFFIC_INFERENCE_POSTPROCESS_ALERT_ACTION_RAISE)
    {
        uint32_t alert_sequence = 0U;
        app_alert_request_t request = {
            .source = APP_ALERT_SOURCE_TRAFFIC_AUDIO,
            .severity = APP_ALERT_SEVERITY_DANGER,
            .label = alert->label ==
                             TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_HORN
                         ? APP_ALERT_LABEL_HORN
                         : APP_ALERT_LABEL_SIREN,
        };
        (void)app_alert_manager_raise(&request);

        taskENTER_CRITICAL(&s_service_state.lock);
        s_service_state.snapshot.state = DANGER_DETECTION_STATE_RUNNING;
        s_service_state.snapshot.risk_state = DANGER_DETECTION_RISK_ALERTING;
        s_service_state.snapshot.stable_label =
            danger_detection_map_label(alert->label);
        s_service_state.snapshot.last_detected_label =
            danger_detection_map_label(alert->label);
        s_service_state.snapshot.last_detected_confidence =
            alert->confidence_score;
        s_service_state.snapshot.alert_sequence += 1U;
        alert_sequence = s_service_state.snapshot.alert_sequence;
        s_service_state.snapshot.danger_overlay_active = true;
        taskEXIT_CRITICAL(&s_service_state.lock);
        danger_detection_post_cloud_alert(
            danger_detection_cloud_type_from_stable_label(alert->label),
            alert->confidence_score,
            alert_sequence);
    }
    else if (alert->action ==
             TRAFFIC_INFERENCE_POSTPROCESS_ALERT_ACTION_CLEAR)
    {
        (void)app_alert_manager_clear(APP_ALERT_SOURCE_TRAFFIC_AUDIO);

        taskENTER_CRITICAL(&s_service_state.lock);
        s_service_state.snapshot.state = DANGER_DETECTION_STATE_RUNNING;
        s_service_state.snapshot.risk_state = DANGER_DETECTION_RISK_MONITORING;
        s_service_state.snapshot.stable_label = DANGER_DETECTION_LABEL_NONE;
        s_service_state.snapshot.danger_overlay_active = false;
        taskEXIT_CRITICAL(&s_service_state.lock);
    }
}

/**
 * @brief ESP-DL 单模型推理结果回调。
 *
 * 将 ESP-DL active 模型结果映射到 danger_detection_snapshot，并在检测到
 * danger 时触发应用级告警。当前 active 模型为 V3.4 T90 sharp，保持单模型
 * 常驻，避免多模型同时占用 RAM。
 */
static void danger_detection_on_espdl_result(
    const espdl_model_result_t *result,
    void *user_data)
{
    (void)user_data;

    if (result == NULL)
    {
        return;
    }

    taskENTER_CRITICAL(&s_service_state.lock);
    const bool runtime_started = s_service_state.runtime_started;
    const danger_detection_state_t service_state =
        s_service_state.snapshot.state;
    taskEXIT_CRITICAL(&s_service_state.lock);

    if (!runtime_started ||
        service_state == DANGER_DETECTION_STATE_STOPPING ||
        service_state == DANGER_DETECTION_STATE_ERROR)
    {
        return;
    }

    const bool is_danger = result->label_index == 1;
    const float danger_prob = result->probabilities[1];
    const TickType_t now_tick = xTaskGetTickCount();
    const danger_detection_policy_profile_t *profile =
        danger_detection_service_get_policy_profile();
    const TickType_t hold_ticks = pdMS_TO_TICKS(profile->alert_hold_ms);
    const TickType_t cooldown_ticks = pdMS_TO_TICKS(profile->cooldown_ms);
    bool should_raise_alert = false;
    bool should_clear_alert = false;
    uint32_t alert_sequence = 0U;
    danger_detection_risk_state_t old_risk_state =
        DANGER_DETECTION_RISK_OFF;
    danger_detection_risk_state_t new_risk_state =
        DANGER_DETECTION_RISK_MONITORING;

    taskENTER_CRITICAL(&s_service_state.lock);

    old_risk_state = s_service_state.snapshot.risk_state;
    new_risk_state = old_risk_state;
    s_service_state.snapshot.danger_confidence = danger_prob;
    s_service_state.snapshot.state = DANGER_DETECTION_STATE_RUNNING;
    s_service_state.snapshot.active_backend = DANGER_DETECTION_BACKEND_ESPDL;
    const bool cooldown_active =
        old_risk_state == DANGER_DETECTION_RISK_COOLDOWN &&
        (int32_t)(now_tick - s_espdl_cooldown_until_tick) < 0;

    if (is_danger)
    {
        if (s_espdl_danger_window_count < UINT32_MAX)
        {
            s_espdl_danger_window_count++;
        }
        s_espdl_clear_window_count = 0U;
        s_espdl_hold_until_tick = now_tick + hold_ticks;
    }
    else
    {
        s_espdl_danger_window_count = 0U;
        if (s_espdl_clear_window_count < UINT32_MAX)
        {
            s_espdl_clear_window_count++;
        }
    }

    if (cooldown_active)
    {
        /*
         * 冷却期仍记录模型证据和窗口计数，但不重复触发强提醒。
         * 若危险证据持续到冷却结束，后续窗口会重新走连续确认逻辑。
         */
        new_risk_state = DANGER_DETECTION_RISK_COOLDOWN;
    }
    else if (is_danger &&
        s_espdl_danger_window_count >= profile->confirm_windows &&
        !s_service_state.snapshot.danger_overlay_active)
    {
        /* 连续 danger 窗口达到门限后才触发，降低单窗误报概率。 */
        new_risk_state = DANGER_DETECTION_RISK_ALERTING;
        s_service_state.snapshot.stable_label = DANGER_DETECTION_LABEL_DANGER;
        s_service_state.snapshot.last_detected_label = DANGER_DETECTION_LABEL_DANGER;
        s_service_state.snapshot.last_detected_confidence = danger_prob;
        s_service_state.snapshot.alert_sequence += 1U;
        alert_sequence = s_service_state.snapshot.alert_sequence;
        s_service_state.snapshot.danger_overlay_active = true;
        should_raise_alert = true;
    }
    else if (is_danger && s_service_state.snapshot.danger_overlay_active)
    {
        /* 告警保持期间继续刷新置信度，但不重复增加 alert_sequence。 */
        new_risk_state = DANGER_DETECTION_RISK_ALERTING;
        s_service_state.snapshot.stable_label = DANGER_DETECTION_LABEL_DANGER;
        s_service_state.snapshot.last_detected_confidence = danger_prob;
    }
    else if (is_danger)
    {
        /*
         * 首个高风险窗只进入可疑态。正式告警仍需连续窗口确认，
         * 这样可以吸收人声爆破音、摩擦和短促外放尖峰。
         */
        new_risk_state = DANGER_DETECTION_RISK_SUSPICIOUS;
    }
    else if (!is_danger && s_service_state.snapshot.danger_overlay_active)
    {
        const bool hold_expired =
            (int32_t)(now_tick - s_espdl_hold_until_tick) >= 0;
        if (hold_expired &&
            s_espdl_clear_window_count >= profile->clear_windows)
        {
            /* danger 消失也要连续确认，避免边界窗口导致 UI 来回抖动。 */
            s_service_state.snapshot.stable_label = DANGER_DETECTION_LABEL_NONE;
            s_service_state.snapshot.danger_overlay_active = false;
            s_espdl_cooldown_until_tick = now_tick + cooldown_ticks;
            new_risk_state = DANGER_DETECTION_RISK_COOLDOWN;
            should_clear_alert = true;
        }
        else
        {
            new_risk_state = DANGER_DETECTION_RISK_ALERTING;
        }
    }
    else if (old_risk_state == DANGER_DETECTION_RISK_COOLDOWN)
    {
        const bool cooldown_expired =
            (int32_t)(now_tick - s_espdl_cooldown_until_tick) >= 0;
        new_risk_state = cooldown_expired
                             ? DANGER_DETECTION_RISK_MONITORING
                             : DANGER_DETECTION_RISK_COOLDOWN;
    }
    else
    {
        new_risk_state = DANGER_DETECTION_RISK_MONITORING;
    }

    s_service_state.snapshot.risk_state = new_risk_state;
    taskEXIT_CRITICAL(&s_service_state.lock);

    if (old_risk_state != new_risk_state)
    {
        ESP_LOGI(TAG,
                 "danger risk: %s -> %s, prob=%.4f, danger_windows=%lu, clear_windows=%lu",
                 danger_detection_risk_state_text(old_risk_state),
                 danger_detection_risk_state_text(new_risk_state),
                 danger_prob,
                 (unsigned long)s_espdl_danger_window_count,
                 (unsigned long)s_espdl_clear_window_count);
    }

    if (should_raise_alert)
    {
        if (!danger_detection_service_allows_alert_commit())
        {
            return;
        }

        /* 在锁外触发告警，避免回调内部长时间持锁 */
        app_alert_request_t request = {
            .source = APP_ALERT_SOURCE_TRAFFIC_AUDIO,
            .severity = APP_ALERT_SEVERITY_DANGER,
            .label = APP_ALERT_LABEL_DANGER,
        };
        (void)app_alert_manager_raise(&request);
        danger_detection_post_cloud_alert("danger", danger_prob,
                                          alert_sequence);

        /* 触发样本录制。 */
        esp_err_t capture_ret = danger_sample_recorder_capture(
            result->label_index, danger_prob,
            result->window_end_sample_index);
        if (capture_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "样本录制失败: %s", esp_err_to_name(capture_ret));
            /* 不阻止告警流程，录制失败不影响主功能。 */
        }

        ESP_LOGW(TAG,
                 "ESPDL danger 检测: profile=%s, class=%s, danger_prob=%.4f, windows=%lu",
                 profile->deployment_profile_id,
                 profile->danger_class_profile,
                 danger_prob,
                 (unsigned long)s_espdl_danger_window_count);
    }
    else if (should_clear_alert)
    {
        if (!danger_detection_service_allows_alert_commit())
        {
            return;
        }

        (void)app_alert_manager_clear(APP_ALERT_SOURCE_TRAFFIC_AUDIO);
        ESP_LOGI(TAG,
                 "ESPDL danger 清除: clear_windows=%lu",
                 (unsigned long)s_espdl_clear_window_count);
    }
}

/**
 * @brief 初始化危险检测服务。
 */
esp_err_t danger_detection_service_init(void)
{
    if (s_service_state.initialized)
    {
        return ESP_OK;
    }

    esp_err_t ret = app_alert_manager_init();
    if (ret != ESP_OK)
    {
        danger_detection_set_state(DANGER_DETECTION_STATE_ERROR, ret);
        return ret;
    }

    /* 初始化危险样本录制器。 */
    ret = danger_sample_recorder_init(NULL);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "危险样本录制器初始化失败，样本录制功能不可用: %s",
                 esp_err_to_name(ret));
        /* 不阻止服务启动，录制功能可选。 */
    }

    taskENTER_CRITICAL(&s_service_state.lock);
    s_service_state.initialized = true;
    s_service_state.snapshot.state = DANGER_DETECTION_STATE_IDLE;
    s_service_state.snapshot.risk_state = DANGER_DETECTION_RISK_OFF;
    s_service_state.snapshot.stable_label = DANGER_DETECTION_LABEL_NONE;
    s_service_state.snapshot.last_detected_label = DANGER_DETECTION_LABEL_NONE;
    s_service_state.snapshot.last_detected_confidence = 0.0f;
    s_service_state.snapshot.horn_confidence = 0.0f;
    s_service_state.snapshot.siren_confidence = 0.0f;
    s_service_state.snapshot.danger_confidence = 0.0f;
    s_service_state.snapshot.alert_sequence = 0U;
    s_service_state.snapshot.last_error = ESP_OK;
    s_service_state.snapshot.danger_overlay_active = false;
    s_service_state.snapshot.active_backend = DANGER_DETECTION_BACKEND_EDGE_IMPULSE;
    danger_detection_reset_espdl_postprocess();
    taskEXIT_CRITICAL(&s_service_state.lock);
    return ESP_OK;
}

/**
 * @brief 启动 Edge Impulse 后端运行时。
 */
static esp_err_t start_edge_impulse_backend(void)
{
    traffic_audio_runtime_config_t config = {
        .input_chunk_frames = 0U,
        .read_timeout_ms = 250U,
        .task_stack_size = 8192U,
        .task_priority = 5U,
    };

    danger_detection_set_state(DANGER_DETECTION_STATE_STARTING, ESP_OK);

    esp_err_t ret = traffic_inference_postprocess_set_alert_callback(
        danger_detection_on_alert, NULL);
    if (ret != ESP_OK)
    {
        danger_detection_set_state(DANGER_DETECTION_STATE_ERROR, ret);
        return ret;
    }

    taskENTER_CRITICAL(&s_service_state.lock);
    s_service_state.callback_registered = true;
    taskEXIT_CRITICAL(&s_service_state.lock);

    ret = traffic_audio_runtime_start(&config);
    if (ret != ESP_OK)
    {
        (void)traffic_inference_postprocess_set_alert_callback(NULL, NULL);
        taskENTER_CRITICAL(&s_service_state.lock);
        s_service_state.callback_registered = false;
        taskEXIT_CRITICAL(&s_service_state.lock);
        danger_detection_set_state(DANGER_DETECTION_STATE_ERROR, ret);
        return ret;
    }

    taskENTER_CRITICAL(&s_service_state.lock);
    s_service_state.runtime_started = true;
    s_service_state.active_backend = DANGER_DETECTION_BACKEND_EDGE_IMPULSE;
    s_service_state.snapshot.risk_state = DANGER_DETECTION_RISK_MONITORING;
    taskEXIT_CRITICAL(&s_service_state.lock);

    ESP_LOGI(TAG, "Edge Impulse 运行时已启动");
    return ESP_OK;
}

/**
 * @brief 启动 ESP-DL 单模型后端运行时。
 */
static esp_err_t start_espdl_backend(void)
{
    espdl_audio_runtime_config_t config = {
        .input_chunk_frames = 0U,
        .read_timeout_ms = 250U,
        .task_stack_size = 12288U,  /* 单模型保留余量，避免 UI/音频任务栈挤压。 */
        .task_priority = 5U,
    };

    danger_detection_set_state(DANGER_DETECTION_STATE_STARTING, ESP_OK);

    const danger_detection_policy_profile_t *profile =
        danger_detection_service_get_policy_profile();
    esp_err_t ret = espdl_audio_runtime_set_danger_threshold(
        profile->single_window_threshold);
    if (ret != ESP_OK)
    {
        danger_detection_set_state(DANGER_DETECTION_STATE_ERROR, ret);
        return ret;
    }

    /* 注册 ESPDL 结果回调 */
    ret = espdl_audio_runtime_set_result_callback(
        danger_detection_on_espdl_result, NULL);
    if (ret != ESP_OK)
    {
        danger_detection_set_state(DANGER_DETECTION_STATE_ERROR, ret);
        return ret;
    }

    taskENTER_CRITICAL(&s_service_state.lock);
    s_service_state.callback_registered = true;
    taskEXIT_CRITICAL(&s_service_state.lock);

    /* 注册 PCM tap 回调到 ESP-DL runtime（由 service 层桥接 recorder）。 */
    danger_sample_recorder_reset_session();
    ret = espdl_audio_runtime_set_pcm_tap_callback(
        danger_detection_on_espdl_pcm_tap, NULL);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "PCM tap 回调注册失败，样本录制功能不可用: %s",
                 esp_err_to_name(ret));
        /* 不阻止服务启动，录制功能可选。 */
    }

    ret = espdl_audio_runtime_start(&config);
    if (ret != ESP_OK)
    {
        (void)espdl_audio_runtime_set_result_callback(NULL, NULL);
        (void)espdl_audio_runtime_set_pcm_tap_callback(NULL, NULL);
        taskENTER_CRITICAL(&s_service_state.lock);
        s_service_state.callback_registered = false;
        taskEXIT_CRITICAL(&s_service_state.lock);
        danger_detection_set_state(DANGER_DETECTION_STATE_ERROR, ret);
        return ret;
    }

    taskENTER_CRITICAL(&s_service_state.lock);
    s_service_state.runtime_started = true;
    s_service_state.active_backend = DANGER_DETECTION_BACKEND_ESPDL;
    s_service_state.snapshot.risk_state = DANGER_DETECTION_RISK_MONITORING;
    taskEXIT_CRITICAL(&s_service_state.lock);

    ESP_LOGI(TAG, "ESP-DL 单模型运行时已启动");
    return ESP_OK;
}

/**
 * @brief 启动危险检测运行时（默认 ESP-DL 单模型）。
 */
esp_err_t danger_detection_service_start(void)
{
    return danger_detection_service_start_with_backend(
        DANGER_DETECTION_BACKEND_ESPDL);
}

/**
 * @brief 使用指定后端启动危险检测运行时。
 */
esp_err_t danger_detection_service_start_with_backend(
    danger_detection_backend_t backend)
{
    esp_err_t ret = danger_detection_service_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    taskENTER_CRITICAL(&s_service_state.lock);
    const bool already_running = s_service_state.runtime_started;
    taskEXIT_CRITICAL(&s_service_state.lock);
    if (already_running)
    {
        return ESP_OK;
    }

    /* 重置快照 */
    taskENTER_CRITICAL(&s_service_state.lock);
    s_service_state.snapshot.stable_label = DANGER_DETECTION_LABEL_NONE;
    s_service_state.snapshot.risk_state = DANGER_DETECTION_RISK_OFF;
    s_service_state.snapshot.last_detected_label = DANGER_DETECTION_LABEL_NONE;
    s_service_state.snapshot.last_detected_confidence = 0.0f;
    s_service_state.snapshot.horn_confidence = 0.0f;
    s_service_state.snapshot.siren_confidence = 0.0f;
    s_service_state.snapshot.danger_confidence = 0.0f;
    s_service_state.snapshot.alert_sequence = 0U;
    s_service_state.snapshot.danger_overlay_active = false;
    danger_detection_reset_espdl_postprocess();
    taskEXIT_CRITICAL(&s_service_state.lock);

    switch (backend)
    {
    case DANGER_DETECTION_BACKEND_ESPDL:
        return start_espdl_backend();
    case DANGER_DETECTION_BACKEND_EDGE_IMPULSE:
    default:
        return start_edge_impulse_backend();
    }
}

/**
 * @brief 停止危险检测运行时。
 */
esp_err_t danger_detection_service_stop(uint32_t timeout_ms)
{
    if (!s_service_state.initialized)
    {
        return ESP_OK;
    }

    taskENTER_CRITICAL(&s_service_state.lock);
    const bool runtime_started = s_service_state.runtime_started;
    const bool callback_registered = s_service_state.callback_registered;
    const danger_detection_backend_t backend = s_service_state.active_backend;
    taskEXIT_CRITICAL(&s_service_state.lock);

    if (!runtime_started && !callback_registered)
    {
        danger_sample_recorder_reset_session();
        danger_detection_set_state(DANGER_DETECTION_STATE_IDLE, ESP_OK);
        return ESP_OK;
    }

    danger_detection_set_state(DANGER_DETECTION_STATE_STOPPING, ESP_OK);
    (void)app_alert_manager_clear(APP_ALERT_SOURCE_TRAFFIC_AUDIO);

    /* 注销 PCM tap 回调（在 runtime stop 之前）。 */
    if (backend == DANGER_DETECTION_BACKEND_ESPDL)
    {
        (void)espdl_audio_runtime_set_pcm_tap_callback(NULL, NULL);
    }

    esp_err_t ret = ESP_OK;
    if (runtime_started)
    {
        const uint32_t effective_timeout =
            timeout_ms == 0U ? DANGER_DETECTION_STOP_TIMEOUT_MS : timeout_ms;

        if (backend == DANGER_DETECTION_BACKEND_ESPDL)
        {
            ret = espdl_audio_runtime_stop(effective_timeout);
        }
        else
        {
            ret = traffic_audio_runtime_stop(effective_timeout);
        }
    }

    if (ret != ESP_OK)
    {
        danger_detection_set_state(DANGER_DETECTION_STATE_ERROR, ret);
        return ret;
    }

    if (callback_registered)
    {
        if (backend == DANGER_DETECTION_BACKEND_ESPDL)
        {
            (void)espdl_audio_runtime_set_result_callback(NULL, NULL);
        }
        else
        {
            esp_err_t clear_ret =
                traffic_inference_postprocess_set_alert_callback(NULL, NULL);
            if (ret == ESP_OK && clear_ret != ESP_OK)
            {
                ret = clear_ret;
            }
        }
    }

    if (ret != ESP_OK)
    {
        taskENTER_CRITICAL(&s_service_state.lock);
        s_service_state.runtime_started = false;
        taskEXIT_CRITICAL(&s_service_state.lock);
        danger_detection_set_state(DANGER_DETECTION_STATE_ERROR, ret);
        return ret;
    }

    taskENTER_CRITICAL(&s_service_state.lock);
    s_service_state.callback_registered = false;
    s_service_state.runtime_started = false;
    s_service_state.snapshot.stable_label = DANGER_DETECTION_LABEL_NONE;
    s_service_state.snapshot.risk_state = DANGER_DETECTION_RISK_OFF;
    s_service_state.snapshot.last_detected_label = DANGER_DETECTION_LABEL_NONE;
    s_service_state.snapshot.last_detected_confidence = 0.0f;
    s_service_state.snapshot.horn_confidence = 0.0f;
    s_service_state.snapshot.siren_confidence = 0.0f;
    s_service_state.snapshot.danger_confidence = 0.0f;
    s_service_state.snapshot.alert_sequence = 0U;
    s_service_state.snapshot.danger_overlay_active = false;
    danger_detection_reset_espdl_postprocess();
    taskEXIT_CRITICAL(&s_service_state.lock);

    /* 普通后台开关 stop 只重置会话，不销毁 recorder worker/queue。 */
    danger_sample_recorder_reset_session();

    danger_detection_set_state(DANGER_DETECTION_STATE_IDLE, ESP_OK);
    (void)app_alert_manager_clear(APP_ALERT_SOURCE_TRAFFIC_AUDIO);
    ESP_LOGI(TAG, "危险检测运行时已停止 (backend=%d)", backend);
    return ESP_OK;
}

/**
 * @brief 获取当前危险检测快照。
 */
danger_detection_snapshot_t danger_detection_service_get_snapshot(void)
{
    danger_detection_snapshot_t snapshot;
    bool runtime_started = false;
    danger_detection_backend_t backend = DANGER_DETECTION_BACKEND_EDGE_IMPULSE;

    taskENTER_CRITICAL(&s_service_state.lock);
    snapshot = s_service_state.snapshot;
    runtime_started = s_service_state.runtime_started;
    backend = s_service_state.active_backend;
    taskEXIT_CRITICAL(&s_service_state.lock);

    if (runtime_started)
    {
        if (backend == DANGER_DETECTION_BACKEND_ESPDL)
        {
            /* ESPDL 后端：查询运行时状态 */
            espdl_audio_runtime_state_t espdl_state =
                espdl_audio_runtime_get_state();
            snapshot.state = danger_detection_map_espdl_state(
                espdl_state, snapshot.state);
            snapshot.active_backend = DANGER_DETECTION_BACKEND_ESPDL;
            if (espdl_state == ESPDL_AUDIO_RUNTIME_STATE_FAILED &&
                snapshot.last_error == ESP_OK)
            {
                snapshot.last_error = ESP_FAIL;
            }
        }
        else
        {
            /* Edge Impulse 后端：查询运行时状态和分数 */
            traffic_audio_runtime_state_t runtime_state =
                traffic_audio_runtime_get_state();
            const traffic_inference_postprocess_snapshot_t postprocess_snapshot =
                traffic_inference_postprocess_get_latest_snapshot();
            snapshot.state = danger_detection_map_runtime_state(runtime_state,
                                                                snapshot.state);
            snapshot.horn_confidence = postprocess_snapshot.horn_score;
            snapshot.siren_confidence = postprocess_snapshot.siren_score;
            snapshot.active_backend = DANGER_DETECTION_BACKEND_EDGE_IMPULSE;
            if (runtime_state == TRAFFIC_AUDIO_RUNTIME_STATE_FAILED &&
                snapshot.last_error == ESP_OK)
            {
                snapshot.last_error = ESP_FAIL;
            }
        }
    }

    return snapshot;
}
