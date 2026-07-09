#ifndef AUDIO_MIC_TEST_SERVICE_H
#define AUDIO_MIC_TEST_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_MIC_TEST_SERVICE_PATH_MAX 160U
#define AUDIO_MIC_TEST_SERVICE_REASON_MAX 32U
#define AUDIO_MIC_TEST_SERVICE_MAX_CHANNELS 4U

typedef enum {
    AUDIO_MIC_TEST_STATE_IDLE = 0,
    AUDIO_MIC_TEST_STATE_RUNNING,
    AUDIO_MIC_TEST_STATE_PASSED,
    AUDIO_MIC_TEST_STATE_FAILED,
} audio_mic_test_state_t;

typedef struct {
    float rms;             /**< 该通道 RMS 幅度，单位为 PCM LSB。 */
    int32_t peak;          /**< 该通道绝对峰值，单位为 PCM LSB。 */
    float zero_percent;    /**< 精确 0 样本占比，用于识别静音/断线。 */
    uint32_t clip_count;   /**< 接近 int16 满幅的样本数。 */
    uint32_t samples;      /**< 参与统计的样本数。 */
} audio_mic_test_channel_stats_t;

typedef struct {
    audio_mic_test_state_t state; /**< 当前测试状态。 */
    esp_err_t last_error;         /**< 最近一次失败的 ESP-IDF 错误码。 */
    char reason[AUDIO_MIC_TEST_SERVICE_REASON_MAX]; /**< 简短失败原因。 */
    char wav_path[AUDIO_MIC_TEST_SERVICE_PATH_MAX]; /**< 最近一次 WAV 路径。 */
    char json_path[AUDIO_MIC_TEST_SERVICE_PATH_MAX]; /**< 最近一次 JSON 路径。 */
    uint32_t duration_ms;         /**< 目标录制时长，单位毫秒。 */
    uint32_t bytes_read;          /**< 实际读取的 PCM 字节数。 */
    uint32_t target_bytes;        /**< 目标 PCM 字节数。 */
    uint32_t sample_rate_hz;      /**< WAV 采样率。 */
    uint16_t channels;            /**< WAV 声道数。 */
    uint16_t bits_per_sample;     /**< WAV 位深。 */
    uint8_t mic_channel_index;    /**< `AUDIO_PLATFORM_ADC_CHANNEL_FORMAT` 中 M 的通道索引。 */
    audio_mic_test_channel_stats_t
        channel_stats[AUDIO_MIC_TEST_SERVICE_MAX_CHANNELS];
} audio_mic_test_snapshot_t;

/**
 * @brief 启动一次手动麦克风硬件测试。
 *
 * 测试在独立 FreeRTOS task 中执行，调用者可以来自 UI 事件上下文。运行中重复
 * 调用返回 `ESP_ERR_INVALID_STATE`，不会创建第二个录音任务。
 */
esp_err_t audio_mic_test_service_start(void);

/**
 * @brief 获取麦克风测试只读快照。
 *
 * @param[out] out_snapshot 输出快照。
 * @return `ESP_OK` 表示成功；`ESP_ERR_INVALID_ARG` 表示输出参数为空。
 */
esp_err_t audio_mic_test_service_get_snapshot(
    audio_mic_test_snapshot_t *out_snapshot);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_MIC_TEST_SERVICE_H
