#include "danger_detection_controller.h"

#include <stdio.h>
#include <string.h>

#include "features/alerts/app_alert_manager.h"
#include "features/danger_detection/danger_detection_service.h"
#include "services/audio_diag/audio_mic_test_service.h"
#include "services/runtime/safety_monitor_policy.h"
#include "services/safety/safety_monitor_session.h"
#include "danger_detection_view.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "danger_ui";

static lv_ui *s_ui = NULL;
static danger_detection_view_t *s_view = NULL;

typedef struct {
    bool valid;
    bool alert_visible;
    bool safety_monitor_enabled;
    danger_detection_view_sensitivity_mode_t sensitivity_mode;
    char status_text[32];
    char category_text[32];
    char primary_result_text[16];
    char horn_confidence_text[16];
    char siren_confidence_text[16];
    char mic_test_status_text[32];
    bool mic_test_running;
} danger_detection_render_cache_t;

static danger_detection_render_cache_t s_render_cache = {0};

static void danger_detection_refresh_status(void);

static const char *danger_detection_status_text(
    const danger_detection_snapshot_t *snapshot,
    const safety_monitor_policy_snapshot_t *manager_snapshot)
{
    const danger_detection_state_t state =
        snapshot != NULL ? snapshot->state : DANGER_DETECTION_STATE_IDLE;

    if (state == DANGER_DETECTION_STATE_STOPPING) {
        return "正在停止";
    }

    if (manager_snapshot != NULL && !manager_snapshot->started) {
        return "后台未就绪";
    }

    if (manager_snapshot != NULL) {
        switch (manager_snapshot->block_reason) {
            case SAFETY_MONITOR_POLICY_BLOCK_USER_DISABLED:
                return "未开启";
            case SAFETY_MONITOR_POLICY_BLOCK_FOREGROUND_AUDIO:
                return "资源占用，暂时等待";
            case SAFETY_MONITOR_POLICY_BLOCK_RUNTIME_COORDINATOR:
                return "前台任务中，暂时等待";
            case SAFETY_MONITOR_POLICY_BLOCK_MUSIC_PLAYBACK:
                return "音乐播放中，安全告警关闭";
            case SAFETY_MONITOR_POLICY_BLOCK_POWER:
                if ((manager_snapshot->policy_flags &
                     POWER_POLICY_FLAG_MAINTENANCE) != 0U) {
                    return "维护中暂停";
                }
                return "资源占用，暂时等待";
            case SAFETY_MONITOR_POLICY_BLOCK_NOT_READY:
                return "后台未就绪";
            case SAFETY_MONITOR_POLICY_BLOCK_NONE:
            default:
                break;
        }
    }

    // policy 不再镜像 session.last_error，直接读源头 session 快照。
    if (safety_monitor_session_get_snapshot().last_error != ESP_OK) {
        return "监听异常";
    }

    if (snapshot == NULL) {
        return "未开启";
    }

    if (state == DANGER_DETECTION_STATE_ERROR) {
        return "启动失败";
    }
    if (state == DANGER_DETECTION_STATE_STARTING) {
        return "正在启动";
    }
    if (manager_snapshot != NULL &&
        manager_snapshot->should_run &&
        !safety_monitor_session_get_snapshot().runtime_running) {
        return "正在启动";
    }
    if (state == DANGER_DETECTION_STATE_RUNNING) {
        switch (snapshot->risk_state) {
            case DANGER_DETECTION_RISK_ALERTING:
                return "危险提醒中";
            case DANGER_DETECTION_RISK_SUSPICIOUS:
                return "正在确认";
            case DANGER_DETECTION_RISK_COOLDOWN:
                return "冷却中";
            case DANGER_DETECTION_RISK_MONITORING:
            case DANGER_DETECTION_RISK_OFF:
            default:
                break;
        }
        return "正在监听";
    }
    return "未开启";
}

/**
 * @brief 将服务层危险标签转换成页面展示文本。
 *
 * @param[in] label 服务层发布的稳定标签或最近触发标签。
 * @return 静态字符串；未知值按 NONE 处理，避免 UI 显示未初始化文本。
 */
static const char *danger_detection_label_text(danger_detection_label_t label)
{
    switch (label) {
        case DANGER_DETECTION_LABEL_HORN:
            return "HORN";
        case DANGER_DETECTION_LABEL_SIREN:
            return "SIREN";
        case DANGER_DETECTION_LABEL_DANGER:
            return "DANGER";
        case DANGER_DETECTION_LABEL_NONE:
        default:
            return "NONE";
    }
}

static bool danger_detection_page_alert_visible(
    const danger_detection_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return false;
    }

    /*
     * 专页上的红色危险态跟随服务层风险状态机，而不是本地 2 秒计时器。
     * 这样页面显示、全局提醒和后处理状态使用同一个生命周期。
     */
    return snapshot->state == DANGER_DETECTION_STATE_RUNNING &&
           snapshot->risk_state == DANGER_DETECTION_RISK_ALERTING;
}

static void danger_detection_format_confidence(float confidence,
                                               char *buffer,
                                               size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0U) {
        return;
    }

    if (confidence <= 0.0f) {
        snprintf(buffer, buffer_size, "--");
        return;
    }

    int confidence_tenths = (int)(confidence * 1000.0f + 0.5f);
    if (confidence_tenths < 0) {
        confidence_tenths = 0;
    }
    if (confidence_tenths > 1000) {
        confidence_tenths = 1000;
    }

    snprintf(buffer, buffer_size, "%d.%d%%", confidence_tenths / 10,
             confidence_tenths % 10);
}

static void danger_detection_copy_text(char *dst,
                                       size_t dst_size,
                                       const char *src)
{
    if (dst == NULL || dst_size == 0U) {
        return;
    }

    snprintf(dst, dst_size, "%s", src != NULL ? src : "");
}

static danger_detection_view_sensitivity_mode_t
danger_detection_view_mode_from_service(
    danger_detection_sensitivity_mode_t mode)
{
    switch (mode) {
        case DANGER_DETECTION_SENSITIVITY_CONSERVATIVE:
            return DANGER_DETECTION_VIEW_SENSITIVITY_CONSERVATIVE;
        case DANGER_DETECTION_SENSITIVITY_SENSITIVE:
            return DANGER_DETECTION_VIEW_SENSITIVITY_SENSITIVE;
        case DANGER_DETECTION_SENSITIVITY_STANDARD:
        default:
            return DANGER_DETECTION_VIEW_SENSITIVITY_STANDARD;
    }
}

static danger_detection_sensitivity_mode_t
danger_detection_service_mode_from_view(
    danger_detection_view_sensitivity_mode_t mode)
{
    switch (mode) {
        case DANGER_DETECTION_VIEW_SENSITIVITY_CONSERVATIVE:
            return DANGER_DETECTION_SENSITIVITY_CONSERVATIVE;
        case DANGER_DETECTION_VIEW_SENSITIVITY_SENSITIVE:
            return DANGER_DETECTION_SENSITIVITY_SENSITIVE;
        case DANGER_DETECTION_VIEW_SENSITIVITY_STANDARD:
        default:
            return DANGER_DETECTION_SENSITIVITY_STANDARD;
    }
}

static bool danger_detection_render_cache_matches(
    const danger_detection_render_cache_t *cache,
    const danger_detection_view_model_t *model)
{
    if (cache == NULL || model == NULL || !cache->valid) {
        return false;
    }

    return cache->alert_visible == model->alert_visible &&
           cache->safety_monitor_enabled == model->safety_monitor_enabled &&
           cache->sensitivity_mode == model->sensitivity_mode &&
           strcmp(cache->status_text, model->status_text) == 0 &&
           strcmp(cache->category_text, model->category_text) == 0 &&
           strcmp(cache->primary_result_text, model->primary_result_text) == 0 &&
           strcmp(cache->horn_confidence_text, model->horn_confidence_text) == 0 &&
           strcmp(cache->siren_confidence_text, model->siren_confidence_text) == 0 &&
           strcmp(cache->mic_test_status_text, model->mic_test_status_text) == 0 &&
           cache->mic_test_running == model->mic_test_running;
}

static void danger_detection_render_cache_store(
    danger_detection_render_cache_t *cache,
    const danger_detection_view_model_t *model)
{
    if (cache == NULL || model == NULL) {
        return;
    }

    cache->valid = true;
    cache->alert_visible = model->alert_visible;
    cache->safety_monitor_enabled = model->safety_monitor_enabled;
    cache->sensitivity_mode = model->sensitivity_mode;
    cache->mic_test_running = model->mic_test_running;
    danger_detection_copy_text(cache->status_text, sizeof(cache->status_text),
                               model->status_text);
    danger_detection_copy_text(cache->category_text, sizeof(cache->category_text),
                               model->category_text);
    danger_detection_copy_text(cache->primary_result_text,
                               sizeof(cache->primary_result_text),
                               model->primary_result_text);
    danger_detection_copy_text(cache->horn_confidence_text,
                               sizeof(cache->horn_confidence_text),
                               model->horn_confidence_text);
    danger_detection_copy_text(cache->siren_confidence_text,
                               sizeof(cache->siren_confidence_text),
                               model->siren_confidence_text);
    danger_detection_copy_text(cache->mic_test_status_text,
                               sizeof(cache->mic_test_status_text),
                               model->mic_test_status_text);
}

static void danger_detection_back_event(void *user_data)
{
    (void)user_data;

    if (s_ui == NULL || s_ui->screen_main == NULL) {
        ESP_LOGW(TAG, "screen_main not ready when leaving danger page");
        return;
    }

    (void)app_alert_manager_set_traffic_audio_overlay_enabled(true);
    s_render_cache.valid = false;
    lv_screen_load_anim(s_ui->screen_main, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0,
                        false);
}

static void danger_detection_safety_monitor_event(bool enabled,
                                                  void *user_data)
{
    (void)user_data;

    esp_err_t ret =
        safety_monitor_policy_set_enabled(enabled);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "safety monitor switch failed: enabled=%d err=%s",
                 enabled, esp_err_to_name(ret));
    }

    s_render_cache.valid = false;
    danger_detection_refresh_status();
}

static void danger_detection_sensitivity_event(
    danger_detection_view_sensitivity_mode_t mode,
    void *user_data)
{
    (void)user_data;

    const danger_detection_sensitivity_mode_t service_mode =
        danger_detection_service_mode_from_view(mode);
    const esp_err_t ret =
        danger_detection_service_set_sensitivity_mode(service_mode);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "sensitivity mode switch failed: mode=%d err=%s",
                 service_mode, esp_err_to_name(ret));
    }

    s_render_cache.valid = false;
    danger_detection_refresh_status();
}

static const char *danger_detection_mic_test_status_text(
    const audio_mic_test_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return "未测试";
    }

    switch (snapshot->state) {
        case AUDIO_MIC_TEST_STATE_RUNNING:
            return "测试中";
        case AUDIO_MIC_TEST_STATE_PASSED:
            return "通过";
        case AUDIO_MIC_TEST_STATE_FAILED:
            if (snapshot->reason[0] != '\0') {
                return snapshot->reason;
            }
            return "失败";
        case AUDIO_MIC_TEST_STATE_IDLE:
        default:
            return "未测试";
    }
}

static void danger_detection_mic_test_event(void *user_data)
{
    (void)user_data;

    const esp_err_t ret = audio_mic_test_service_start();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mic test start failed: %s", esp_err_to_name(ret));
    }

    s_render_cache.valid = false;
    danger_detection_refresh_status();
}

static void danger_detection_ensure_screen_created(void)
{
    if (s_view != NULL) {
        return;
    }

    static const danger_detection_view_config_t kConfig = {
        .back_action_cb = danger_detection_back_event,
        .safety_monitor_cb = danger_detection_safety_monitor_event,
        .sensitivity_cb = danger_detection_sensitivity_event,
        .mic_test_cb = danger_detection_mic_test_event,
        .user_data = NULL,
    };

    s_view = danger_detection_view_create(&kConfig);
    if (s_view == NULL) {
        ESP_LOGE(TAG, "danger_detection_view_create failed");
        return;
    }
}

static void danger_detection_refresh_status(void)
{
    if (s_view == NULL) {
        return;
    }

    const danger_detection_snapshot_t snapshot =
        danger_detection_service_get_snapshot();
    const safety_monitor_policy_snapshot_t manager_snapshot =
        safety_monitor_policy_get_snapshot();
    audio_mic_test_snapshot_t mic_snapshot = {0};
    (void)audio_mic_test_service_get_snapshot(&mic_snapshot);
    const char *last_result_text =
        danger_detection_label_text(snapshot.last_detected_label);

    char category_text[32];
    char horn_confidence_text[16];
    char siren_confidence_text[16];
    snprintf(category_text, sizeof(category_text), "CURRENT: %s",
             danger_detection_label_text(snapshot.stable_label));

    danger_detection_format_confidence(snapshot.horn_confidence,
                                       horn_confidence_text,
                                       sizeof(horn_confidence_text));
    danger_detection_format_confidence(snapshot.siren_confidence,
                                       siren_confidence_text,
                                       sizeof(siren_confidence_text));

    const danger_detection_view_model_t model = {
        .status_text = danger_detection_status_text(&snapshot,
                                                    &manager_snapshot),
        .category_text = category_text,
        .primary_result_text = last_result_text,
        .horn_confidence_text = horn_confidence_text,
        .siren_confidence_text = siren_confidence_text,
        .mic_test_status_text =
            danger_detection_mic_test_status_text(&mic_snapshot),
        .sensitivity_mode = danger_detection_view_mode_from_service(
            danger_detection_service_get_sensitivity_mode()),
        .safety_monitor_enabled =
            manager_snapshot.enabled_by_user,
        .mic_test_running =
            mic_snapshot.state == AUDIO_MIC_TEST_STATE_RUNNING,
        .alert_visible = manager_snapshot.enabled_by_user &&
                         danger_detection_page_alert_visible(&snapshot),
    };

    if (danger_detection_render_cache_matches(&s_render_cache, &model)) {
        return;
    }

    danger_detection_view_apply_model(s_view, &model);
    danger_detection_render_cache_store(&s_render_cache, &model);
}

void danger_detection_controller_init(lv_ui *ui)
{
    s_ui = ui;
    (void)safety_monitor_policy_init();
}

/**
 * @brief 打开危险识别页面并刷新后台 Safety Monitor 会话状态。
 *
 * 页面入口属于 UI 语义层，只表达"用户要查看危险识别状态"。实际音频资源、
 * 模型和告警状态由 safety_monitor_policy 与 danger_detection_service
 * 统一编排。进入页面不会自动开启监听，页面退出也不再拥有 stop 生命周期。
 */
void danger_detection_ui_open(void)
{
    danger_detection_ensure_screen_created();
    if (s_view == NULL) {
        return;
    }

    (void)app_alert_manager_set_traffic_audio_overlay_enabled(false);
    s_render_cache.valid = false;
    danger_detection_refresh_status();
    lv_screen_load_anim(danger_detection_view_get_screen(s_view),
                        LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}

void danger_detection_controller_poll_ui(void)
{
    if (s_view == NULL ||
        lv_screen_active() != danger_detection_view_get_screen(s_view)) {
        return;
    }

    danger_detection_refresh_status();
}
