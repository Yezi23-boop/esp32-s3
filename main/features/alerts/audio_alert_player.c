#include "audio_alert_player.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_codec.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "assets/tishiyinpin_pcm.h"

#define TAG "audio_alert_player"
#define ALERT_PLAYER_TASK_STACK_SIZE 4096U
#define ALERT_PLAYER_TASK_PRIORITY 4U
#define ALERT_PLAYER_VOLUME_PERCENT 75

/*
 * 告警提示音播放器实现说明：
 * - 播放逻辑放在独立任务中，避免阻塞告警上报线程；
 * - 当前只支持单次播放固定 PCM 资源；
 * - 通过 `playing` 标志去重，避免多个告警源同时重复拉起同一提示音。
 */

typedef struct
{
    TaskHandle_t task_handle; // 当前播放任务句柄
    bool initialized;         // 模块是否已完成初始化
    bool playing;             // 是否有播放任务正在运行
    portMUX_TYPE lock;        // 保护播放状态，避免告警线程和播放任务并发写
} audio_alert_player_state_t;

static audio_alert_player_state_t s_player_state = {
    .task_handle = NULL,
    .initialized = false,
    .playing = false,
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

/**
 * @brief 实际执行 PCM 播放的后台任务。
 * @param arg 未使用，保留任务签名。
 */
static void audio_alert_player_task(void *arg)
{
    (void)arg;

    // 写入长度以字节为单位；样本点数量需乘以单样本位宽。
    const size_t pcm_bytes = kTishiyinpinPcmSampleCount * sizeof(int16_t);
    bool output_acquired = false; // P0 提醒播放期间独占 TX 输出链路。

    ESP_LOGI(TAG, "warning playback started bytes=%u rate=%u",
             (unsigned int)pcm_bytes,
             (unsigned int)kTishiyinpinPcmSampleRate);

    esp_err_t ret =
        audio_codec_acquire_output(AUDIO_CODEC_OWNER_ALERT_PLAYER, 500U);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "resource_acquire_denied: resource=audio_output owner=alert_player reason=p0_alert ret=%s",
                 esp_err_to_name(ret));
        goto done;
    }
    output_acquired = true;

    (void)audio_codec_set_pa_enable(true);                     // 打开功放
    (void)audio_codec_set_mute(false);                         // 取消静音
    (void)audio_codec_set_volume(ALERT_PLAYER_VOLUME_PERCENT); // 设置告警播报音量

    ret = audio_codec_write(kTishiyinpinPcmData, pcm_bytes);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "warning playback failed: %s", esp_err_to_name(ret));
    }
    else
    {
        ret = audio_codec_flush_output();
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "warning output flush failed: %s",
                     esp_err_to_name(ret));
        }
    }

    ESP_LOGI(TAG, "warning playback finished");
done:
    if (output_acquired)
    {
        ret = audio_codec_release_output(AUDIO_CODEC_OWNER_ALERT_PLAYER);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG,
                     "resource_release failed: resource=audio_output owner=alert_player ret=%s",
                     esp_err_to_name(ret));
        }
    }
    taskENTER_CRITICAL(&s_player_state.lock);
    s_player_state.playing = false;
    s_player_state.task_handle = NULL;
    taskEXIT_CRITICAL(&s_player_state.lock);
    vTaskDelete(NULL);
}

/**
 * @brief 初始化告警提示音播放器。
 * @return 当前实现始终返回 `ESP_OK`，保留接口便于后续扩展资源装载。
 */
esp_err_t audio_alert_player_init(void)
{
    taskENTER_CRITICAL(&s_player_state.lock);
    s_player_state.initialized = true;
    taskEXIT_CRITICAL(&s_player_state.lock);
    return ESP_OK;
}

/**
 * @brief 异步触发一次危险提示音播放。
 * @return 若已有播放任务运行，则直接返回 `ESP_OK`。
 */
esp_err_t audio_alert_player_play_warning_once(void)
{
    taskENTER_CRITICAL(&s_player_state.lock);
    const bool initialized = s_player_state.initialized;
    const bool already_playing = s_player_state.playing;
    if (initialized && !already_playing)
    {
        s_player_state.playing = true;
    }
    taskEXIT_CRITICAL(&s_player_state.lock);

    ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, TAG,
                        "audio alert player not initialized");

    if (already_playing)
    {
        ESP_LOGI(TAG, "warning playback already active");
        return ESP_OK;
    }

    TaskHandle_t created_handle = NULL;
    BaseType_t task_created = xTaskCreateWithCaps(audio_alert_player_task,
                                                  "audio_alert",
                                                  ALERT_PLAYER_TASK_STACK_SIZE,
                                                  NULL,
                                                  ALERT_PLAYER_TASK_PRIORITY,
                                                  &created_handle,
                                                  MALLOC_CAP_SPIRAM);
    if (task_created != pdPASS)
    {
        taskENTER_CRITICAL(&s_player_state.lock);
        s_player_state.playing = false;
        s_player_state.task_handle = NULL;
        taskEXIT_CRITICAL(&s_player_state.lock);
        ESP_LOGE(TAG, "failed to create warning playback task");
        return ESP_ERR_NO_MEM;
    }

    taskENTER_CRITICAL(&s_player_state.lock);
    if (s_player_state.playing)
    {
        s_player_state.task_handle = created_handle;
    }
    taskEXIT_CRITICAL(&s_player_state.lock);

    return ESP_OK;
}
