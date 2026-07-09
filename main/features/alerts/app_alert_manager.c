#include "app_alert_manager.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "audio_alert_player.h"
#include "audio_codec.h"
#include "display_alert_adapter.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "haptic_alert_player.h"
#include "mp3_player.h"
#include "ui_refresh_policy.h"

#define TAG "app_alert_manager"

/*
 * 告警管理器实现说明：
 * - 负责把“危险声音触发”这类业务事件转换成统一告警表现；
 * - 当前表现包含屏幕红色覆盖层和单次提示音；
 * - 通过来源去重，避免同一来源连续上报时反复重启动画或音频。
 */

typedef struct
{
    bool initialized;                   /**< 管理器是否已初始化。 */
    bool active;                        /**< 当前是否存在活动告警。 */
    bool traffic_audio_overlay_enabled; /**< 是否允许危险音触发屏幕红色覆盖层。 */
    bool low_battery_warning_visible;   /**< 当前低电量可见提示是否已请求显示。 */
    app_alert_request_t active_request; /**< 当前活动告警的来源与标签。 */
    uint32_t generation;                /**< 告警状态事务序号，用于丢弃过期异步显示结果。 */
    portMUX_TYPE lock;                  /**< 保护跨任务访问的告警状态。 */
} app_alert_manager_state_t;

static app_alert_manager_state_t s_alert_manager_state = {
    .initialized = false,
    .active = false,
    .traffic_audio_overlay_enabled = true,
    .low_battery_warning_visible = false,
    .active_request = {
        .source = APP_ALERT_SOURCE_NONE,
        .severity = APP_ALERT_SEVERITY_NONE,
        .label = APP_ALERT_LABEL_NONE,
    },
    .generation = 0U,
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

/**
 * @brief 将告警标签转换成中文日志文本。
 * @param[in] label 告警标签。
 * @return 适合日志打印的中文名称。
 */
static const char *app_alert_label_to_zh(app_alert_label_t label)
{
    switch (label)
    {
    case APP_ALERT_LABEL_HORN:
        return "喇叭";
    case APP_ALERT_LABEL_SIREN:
        return "警笛";
    case APP_ALERT_LABEL_DANGER:
        return "危险声音";
    case APP_ALERT_LABEL_FALL:
        return "跌倒";
    case APP_ALERT_LABEL_NONE:
    default:
        return "无";
    }
}

/**
 * @brief P0 危险提醒播放前让普通播放输出让路。
 *
 * `app_alert_manager` 是告警编排 owner，负责判断 P0 是否需要抢占普通播放；
 * `audio_alert_player` 只负责拿到 output session 后播放提示音。
 */
static void app_alert_manager_preempt_normal_audio_output(
    const app_alert_request_t *request)
{
    if (request == NULL || request->severity != APP_ALERT_SEVERITY_DANGER)
    {
        return;
    }

    audio_codec_session_snapshot_t snapshot = {0};
    esp_err_t ret = audio_codec_get_session_snapshot(&snapshot);
    if (ret != ESP_OK || !snapshot.output_active)
    {
        return;
    }

    if (snapshot.output_owner != AUDIO_CODEC_OWNER_AUDIO_PLAYER)
    {
        ESP_LOGI(TAG,
                 "resource_preempt_skip: resource=audio_output requester=alert_player owner=%s",
                 audio_codec_owner_to_text(snapshot.output_owner));
        return;
    }

    ESP_LOGW(TAG,
             "resource_preempt: resource=audio_output requester=alert_player owner=audio_player reason=p0_alert");
    ret = mp3_player_stop();
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "resource_preempt failed: resource=audio_output owner=audio_player ret=%s",
                 esp_err_to_name(ret));
    }
}

/**
 * @brief 初始化应用级告警管理器。
 * @return `ESP_OK` 表示初始化成功或已初始化；其他错误表示依赖模块初始化失败。
 */
esp_err_t app_alert_manager_init(void)
{
    esp_err_t ret;

    taskENTER_CRITICAL(&s_alert_manager_state.lock);
    const bool already_initialized = s_alert_manager_state.initialized;
    taskEXIT_CRITICAL(&s_alert_manager_state.lock);

    if (already_initialized)
    {
        return ESP_OK;
    }

    ret = audio_alert_player_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = haptic_alert_player_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = display_alert_adapter_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    taskENTER_CRITICAL(&s_alert_manager_state.lock);
    s_alert_manager_state.traffic_audio_overlay_enabled = true;
    s_alert_manager_state.low_battery_warning_visible = false;
    s_alert_manager_state.active = false;
    memset(&s_alert_manager_state.active_request, 0,
           sizeof(s_alert_manager_state.active_request));
    s_alert_manager_state.generation = 0U;
    s_alert_manager_state.initialized = true;
    taskEXIT_CRITICAL(&s_alert_manager_state.lock);
    return ESP_OK;
}

esp_err_t app_alert_manager_set_low_battery_warning(bool visible,
                                                    uint8_t battery_percent,
                                                    uint16_t battery_mv)
{
    bool initialized = false;
    bool changed = false;

    taskENTER_CRITICAL(&s_alert_manager_state.lock);
    initialized = s_alert_manager_state.initialized;
    if (initialized &&
        s_alert_manager_state.low_battery_warning_visible != visible)
    {
        s_alert_manager_state.low_battery_warning_visible = visible;
        changed = true;
    }
    taskEXIT_CRITICAL(&s_alert_manager_state.lock);

    ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, TAG,
                        "app alert manager not initialized");

    if (!changed)
    {
        return ESP_OK;
    }

    if (visible)
    {
        ui_refresh_policy_notify_activity();
    }

    esp_err_t ret = visible
                        ? display_alert_adapter_show_low_battery_warning()
                        : display_alert_adapter_hide_low_battery_warning();
    if (ret != ESP_OK)
    {
        return ret;
    }

    if (visible)
    {
        ESP_LOGW(TAG, "low_battery_visible_warn: soc=%u vbat=%umV",
                 battery_percent, battery_mv);
    }
    else
    {
        ESP_LOGI(TAG, "low_battery_visible_clear");
    }
    return ESP_OK;
}

/**
 * @brief 上报一次告警请求。
 * @param[in] request 告警请求，必须包含来源和严重级别。
 * @return 参数非法或依赖未初始化时返回错误。
 *
 * 同一来源重复上报时只更新标签，不重新触发音频和覆盖层，避免提示抖动。
 */
esp_err_t app_alert_manager_raise(const app_alert_request_t *request)
{
    ESP_RETURN_ON_FALSE(request != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "alert request is required");
    ESP_RETURN_ON_FALSE(request->source != APP_ALERT_SOURCE_NONE,
                        ESP_ERR_INVALID_ARG, TAG, "alert source is required");
    ESP_RETURN_ON_FALSE(request->severity != APP_ALERT_SEVERITY_NONE,
                        ESP_ERR_INVALID_ARG, TAG,
                        "alert severity is required");

    bool initialized = false;
    bool same_source_active = false;
    bool overlay_enabled = true;
    uint32_t request_generation = 0U;

    taskENTER_CRITICAL(&s_alert_manager_state.lock);
    initialized = s_alert_manager_state.initialized;
    if (initialized)
    {
        same_source_active =
            s_alert_manager_state.active &&
            s_alert_manager_state.active_request.source == request->source;
        overlay_enabled =
            !(request->source == APP_ALERT_SOURCE_TRAFFIC_AUDIO &&
              !s_alert_manager_state.traffic_audio_overlay_enabled);
        s_alert_manager_state.active_request = *request;
        if (!same_source_active)
        {
            s_alert_manager_state.generation++;
            s_alert_manager_state.active = true;
        }
        request_generation = s_alert_manager_state.generation;
    }
    taskEXIT_CRITICAL(&s_alert_manager_state.lock);

    ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, TAG,
                        "app alert manager not initialized");

    if (request->severity == APP_ALERT_SEVERITY_DANGER)
    {
        ui_refresh_policy_notify_activity();
    }

    if (same_source_active)
    {
        ESP_LOGI(TAG, "更新危险告警 类别=%s",
                 app_alert_label_to_zh(request->label));
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;
    if (overlay_enabled)
    {
        ret = display_alert_adapter_show_danger_overlay();
        if (ret != ESP_OK)
        {
            taskENTER_CRITICAL(&s_alert_manager_state.lock);
            if (s_alert_manager_state.generation == request_generation &&
                s_alert_manager_state.active_request.source == request->source)
            {
                s_alert_manager_state.active = false;
                memset(&s_alert_manager_state.active_request, 0,
                       sizeof(s_alert_manager_state.active_request));
            }
            taskEXIT_CRITICAL(&s_alert_manager_state.lock);
            return ret;
        }
    }

    taskENTER_CRITICAL(&s_alert_manager_state.lock);
    const bool request_still_active =
        s_alert_manager_state.generation == request_generation &&
        s_alert_manager_state.active &&
        s_alert_manager_state.active_request.source == request->source;
    taskEXIT_CRITICAL(&s_alert_manager_state.lock);
    if (!request_still_active)
    {
        if (overlay_enabled)
        {
            (void)display_alert_adapter_hide_danger_overlay();
        }
        return ESP_OK;
    }

    if (request->severity == APP_ALERT_SEVERITY_DANGER)
    {
        ret = haptic_alert_player_play_initial_danger_once();
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "haptic alert start failed: %s",
                     esp_err_to_name(ret));
        }
    }

    const bool play_warning_audio =
        request->source != APP_ALERT_SOURCE_FALL_DETECTION;
    if (play_warning_audio)
    {
        app_alert_manager_preempt_normal_audio_output(request);
        ret = audio_alert_player_play_warning_once();
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "warning audio playback start failed: %s",
                     esp_err_to_name(ret));
        }
    }
    else
    {
        ESP_LOGI(TAG, "跳过危险提示音 类别=%s",
                 app_alert_label_to_zh(request->label));
    }

    ESP_LOGW(TAG, "进入危险告警 类别=%s", app_alert_label_to_zh(request->label));
    return ESP_OK;
}

/**
 * @brief 清除指定来源对应的活动告警。
 * @param[in] source 告警来源。
 * @return 若当前活动告警并非该来源，则静默返回 `ESP_OK`。
 */
esp_err_t app_alert_manager_clear(app_alert_source_t source)
{
    bool initialized = false;
    bool active_for_source = false;
    bool overlay_enabled = true;
    uint32_t clear_generation = 0U;

    taskENTER_CRITICAL(&s_alert_manager_state.lock);
    initialized = s_alert_manager_state.initialized;
    active_for_source =
        s_alert_manager_state.active &&
        s_alert_manager_state.active_request.source == source;
    overlay_enabled =
        !(source == APP_ALERT_SOURCE_TRAFFIC_AUDIO &&
          !s_alert_manager_state.traffic_audio_overlay_enabled);
    if (active_for_source)
    {
        clear_generation = ++s_alert_manager_state.generation;
    }
    taskEXIT_CRITICAL(&s_alert_manager_state.lock);

    ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, TAG,
                        "app alert manager not initialized");

    if (!active_for_source)
    {
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;
    if (overlay_enabled)
    {
        ret = display_alert_adapter_hide_danger_overlay();
        if (ret != ESP_OK)
        {
            return ret;
        }
    }

    taskENTER_CRITICAL(&s_alert_manager_state.lock);
    if (s_alert_manager_state.generation == clear_generation &&
        s_alert_manager_state.active &&
        s_alert_manager_state.active_request.source == source)
    {
        memset(&s_alert_manager_state.active_request, 0,
               sizeof(s_alert_manager_state.active_request));
        s_alert_manager_state.active = false;
    }
    taskEXIT_CRITICAL(&s_alert_manager_state.lock);

    ESP_LOGI(TAG, "退出危险告警");
    return ESP_OK;
}

/**
 * @brief 设置 traffic_audio 来源的屏幕覆盖层开关。
 * @param[in] enabled true 表示允许显示危险覆盖层。
 * @return `ESP_OK` 表示设置成功；其他错误表示管理器尚未初始化。
 */
esp_err_t app_alert_manager_set_traffic_audio_overlay_enabled(bool enabled)
{
    bool initialized = false;
    bool traffic_audio_active = false;

    taskENTER_CRITICAL(&s_alert_manager_state.lock);
    initialized = s_alert_manager_state.initialized;
    if (initialized)
    {
        s_alert_manager_state.traffic_audio_overlay_enabled = enabled;
        traffic_audio_active =
            s_alert_manager_state.active &&
            s_alert_manager_state.active_request.source ==
                APP_ALERT_SOURCE_TRAFFIC_AUDIO;
    }
    taskEXIT_CRITICAL(&s_alert_manager_state.lock);

    ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, TAG,
                        "app alert manager not initialized");

    if (!enabled && traffic_audio_active)
    {
        (void)display_alert_adapter_hide_danger_overlay();
    }
    else if (enabled && traffic_audio_active)
    {
        (void)display_alert_adapter_show_danger_overlay();
    }
    return ESP_OK;
}
