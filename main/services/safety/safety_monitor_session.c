#include "services/safety/safety_monitor_session.h"

#include "esp_log.h"
#include "features/danger_detection/danger_detection_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

static const char *TAG = "safety_monitor";
static const TickType_t k_start_retry_ticks = pdMS_TO_TICKS(5000);

typedef struct
{
    bool initialized;                   /**< 是否已初始化危险识别依赖。 */
    bool runtime_running;               /**< 最近一次确认的 runtime 运行状态。 */
    esp_err_t last_error;               /**< 最近一次 start/stop/recover 错误码。 */
    TickType_t last_start_attempt_tick; /**< 最近一次启动尝试 tick，用于失败退避。 */
    portMUX_TYPE lock;                  /**< 保护 session 快照。 */
} safety_monitor_session_state_t;

static safety_monitor_session_state_t s_session = {
    .initialized = false,
    .runtime_running = false,
    .last_error = ESP_OK,
    .last_start_attempt_tick = 0,
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static bool safety_monitor_session_snapshot_is_running(
    const danger_detection_snapshot_t *snapshot)
{
    return snapshot != NULL &&
           (snapshot->state == DANGER_DETECTION_STATE_STARTING ||
            snapshot->state == DANGER_DETECTION_STATE_RUNNING ||
            snapshot->state == DANGER_DETECTION_STATE_STOPPING);
}

static void safety_monitor_session_store(esp_err_t last_error,
                                         bool runtime_running)
{
    taskENTER_CRITICAL(&s_session.lock);
    s_session.last_error = last_error;
    s_session.runtime_running = runtime_running;
    taskEXIT_CRITICAL(&s_session.lock);
}

/**
 * @brief 清理危险识别错误态，为后台重试留出干净起点。
 *
 * danger_detection_service 内部 runtime_started 可能仍为 true，但底层 runtime
 * 已经进入 FAILED。此时必须先 stop 清理回调、音频 session 和短状态，再重新
 * start，避免把“已经在跑”的旧状态误判成恢复成功。
 */
static esp_err_t safety_monitor_session_recover_error(void)
{
    ESP_LOGW(TAG, "danger detection runtime is in error state, restarting");
    esp_err_t ret = danger_detection_service_stop(0U);
    safety_monitor_session_store(ret, ret != ESP_OK);

    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "resource_release failed before restart: %s",
                 esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t safety_monitor_session_start(const char *reason)
{
    TickType_t last_attempt = 0;
    esp_err_t last_error = ESP_OK;

    taskENTER_CRITICAL(&s_session.lock);
    last_attempt = s_session.last_start_attempt_tick;
    last_error = s_session.last_error;
    taskEXIT_CRITICAL(&s_session.lock);

    const TickType_t now = xTaskGetTickCount();
    if (last_error != ESP_OK &&
        last_attempt != 0 &&
        (now - last_attempt) < k_start_retry_ticks)
    {
        return last_error;
    }

    taskENTER_CRITICAL(&s_session.lock);
    s_session.last_start_attempt_tick = now;
    taskEXIT_CRITICAL(&s_session.lock);

    esp_err_t ret = danger_detection_service_start();
    const danger_detection_snapshot_t next_snapshot =
        danger_detection_service_get_snapshot();
    const bool next_running =
        safety_monitor_session_snapshot_is_running(&next_snapshot);

    safety_monitor_session_store(ret, next_running);

    if (ret != ESP_OK || !next_running)
    {
        const esp_err_t effective_ret =
            ret != ESP_OK ? ret : ESP_ERR_INVALID_STATE;
        safety_monitor_session_store(effective_ret, next_running);
        ESP_LOGW(TAG,
                 "resource_acquire_denied: danger_detection start failed: %s",
                 esp_err_to_name(effective_ret));
        return effective_ret;
    }

    ESP_LOGI(TAG, "background danger detection started: reason=%s",
             reason != NULL ? reason : "unknown");
    return ESP_OK;
}

static esp_err_t safety_monitor_session_stop(void)
{
    esp_err_t ret = danger_detection_service_stop(0U);

    if (ret != ESP_OK)
    {
        safety_monitor_session_store(ret, true);
        ESP_LOGW(TAG, "resource_release failed: danger_detection stop: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    safety_monitor_session_store(ESP_OK, false);
    ESP_LOGI(TAG, "background danger detection stopped");
    return ESP_OK;
}

esp_err_t safety_monitor_session_init(void)
{
    if (s_session.initialized)
    {
        return ESP_OK;
    }

    esp_err_t ret = danger_detection_service_init();
    if (ret != ESP_OK)
    {
        safety_monitor_session_store(ret, false);
        return ret;
    }

    taskENTER_CRITICAL(&s_session.lock);
    s_session.initialized = true;
    s_session.last_error = ESP_OK;
    taskEXIT_CRITICAL(&s_session.lock);
    return ESP_OK;
}

esp_err_t safety_monitor_session_apply(bool should_run, const char *reason)
{
    esp_err_t ret = safety_monitor_session_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    const danger_detection_snapshot_t snapshot =
        danger_detection_service_get_snapshot();
    bool previous_runtime_running = false;
    taskENTER_CRITICAL(&s_session.lock);
    previous_runtime_running = s_session.runtime_running;
    taskEXIT_CRITICAL(&s_session.lock);
    const bool service_running =
        safety_monitor_session_snapshot_is_running(&snapshot) ||
        previous_runtime_running;
    taskENTER_CRITICAL(&s_session.lock);
    s_session.runtime_running = service_running;
    taskEXIT_CRITICAL(&s_session.lock);

    if (should_run && snapshot.state == DANGER_DETECTION_STATE_ERROR)
    {
        ret = safety_monitor_session_recover_error();
        if (ret != ESP_OK)
        {
            return ret;
        }
        return safety_monitor_session_start(reason);
    }

    if (should_run && !service_running)
    {
        return safety_monitor_session_start(reason);
    }

    if (!should_run && service_running)
    {
        return safety_monitor_session_stop();
    }

    return ESP_OK;
}

safety_monitor_session_snapshot_t safety_monitor_session_get_snapshot(void)
{
    safety_monitor_session_snapshot_t snapshot;

    taskENTER_CRITICAL(&s_session.lock);
    snapshot.runtime_running = s_session.runtime_running;
    snapshot.last_error = s_session.last_error;
    taskEXIT_CRITICAL(&s_session.lock);

    return snapshot;
}
