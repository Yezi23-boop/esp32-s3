#include "haptic_alert_player.h"

#include <stdbool.h>

#include "app/board_ds2413_motor.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "services/power_policy.h"

#define TAG "haptic_alert_player"

#define HAPTIC_ALERT_TASK_STACK_SIZE 3072U
#define HAPTIC_ALERT_TASK_PRIORITY 4U

// 首次危险强震模式：强感知但短促，避免把持续提醒策略提前塞进 V1。
static const uint32_t kInitialDangerOnMs = 220U;
static const uint32_t kInitialDangerGapMs = 90U;

typedef struct
{
    TaskHandle_t task_handle; /**< 当前短生命周期震动任务句柄。 */
    bool initialized;         /**< 模块是否已初始化。 */
    bool playing;             /**< 是否已有震动任务在运行。 */
    portMUX_TYPE lock;        /**< 保护跨任务访问的播放状态。 */
} haptic_alert_player_state_t;

static haptic_alert_player_state_t s_haptic_state = {
    .task_handle = NULL,
    .initialized = false,
    .playing = false,
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

/**
 * @brief 执行一段马达开启时间，失败时立即返回。
 *
 * @param[in] on_ms 马达开启时长，单位 ms。
 * @return ESP_OK 表示该段震动完成。
 */
static esp_err_t haptic_alert_player_run_on_segment(uint32_t on_ms)
{
    esp_err_t ret = board_ds2413_motor_set_enabled(true);
    if (ret != ESP_OK)
    {
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(on_ms));

    ret = board_ds2413_motor_set_enabled(false);
    if (ret != ESP_OK)
    {
        return ret;
    }
    return ESP_OK;
}

/**
 * @brief 首次危险强震后台任务。
 *
 * 该 task 允许 `vTaskDelay()`，因此不会阻塞 ESP-DL 推理回调或告警编排线程。
 */
static void haptic_alert_player_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "initial danger haptic started");

    esp_err_t ret = haptic_alert_player_run_on_segment(kInitialDangerOnMs);
    if (ret == ESP_OK)
    {
        vTaskDelay(pdMS_TO_TICKS(kInitialDangerGapMs));
        ret = haptic_alert_player_run_on_segment(kInitialDangerOnMs);
    }

    // 兜底关断：即使某一段 DS2413 写入失败，也不能把马达留在开启态。
    esp_err_t off_ret = board_ds2413_motor_set_enabled(false);
    if (off_ret != ESP_OK)
    {
        ESP_LOGW(TAG, "haptic motor fallback off failed: %s",
                 esp_err_to_name(off_ret));
    }

    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "initial danger haptic failed: %s",
                 esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI(TAG, "initial danger haptic finished");
    }

    taskENTER_CRITICAL(&s_haptic_state.lock);
    s_haptic_state.playing = false;
    s_haptic_state.task_handle = NULL;
    taskEXIT_CRITICAL(&s_haptic_state.lock);

    vTaskDelete(NULL);
}

esp_err_t haptic_alert_player_init(void)
{
    taskENTER_CRITICAL(&s_haptic_state.lock);
    s_haptic_state.initialized = true;
    taskEXIT_CRITICAL(&s_haptic_state.lock);
    return ESP_OK;
}

esp_err_t haptic_alert_player_play_initial_danger_once(void)
{
    const power_policy_budget_t budget = power_policy_get_budget();
    if (!budget.haptic_alert_allowed)
    {
        ESP_LOGI(TAG, "haptic skipped by power budget");
        return ESP_OK;
    }

    taskENTER_CRITICAL(&s_haptic_state.lock);
    const bool initialized = s_haptic_state.initialized;
    const bool already_playing = s_haptic_state.playing;
    if (initialized && !already_playing)
    {
        s_haptic_state.playing = true;
    }
    taskEXIT_CRITICAL(&s_haptic_state.lock);

    ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, TAG,
                        "haptic alert player not initialized");

    if (already_playing)
    {
        ESP_LOGI(TAG, "initial danger haptic already active");
        return ESP_OK;
    }

    TaskHandle_t created_handle = NULL;
    const BaseType_t task_created = xTaskCreateWithCaps(
        haptic_alert_player_task,
        "haptic_alert",
        HAPTIC_ALERT_TASK_STACK_SIZE,
        NULL,
        HAPTIC_ALERT_TASK_PRIORITY,
        &created_handle,
        MALLOC_CAP_SPIRAM);
    if (task_created != pdPASS)
    {
        taskENTER_CRITICAL(&s_haptic_state.lock);
        s_haptic_state.playing = false;
        s_haptic_state.task_handle = NULL;
        taskEXIT_CRITICAL(&s_haptic_state.lock);
        ESP_LOGE(TAG, "failed to create haptic alert task");
        return ESP_ERR_NO_MEM;
    }

    taskENTER_CRITICAL(&s_haptic_state.lock);
    if (s_haptic_state.playing)
    {
        s_haptic_state.task_handle = created_handle;
    }
    taskEXIT_CRITICAL(&s_haptic_state.lock);
    return ESP_OK;
}
