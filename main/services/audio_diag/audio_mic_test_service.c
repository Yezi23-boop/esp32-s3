#include "services/audio_diag/audio_mic_test_service.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/unistd.h>

#include "audio_codec.h"
#include "audio_platform_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "sd_manager.h"
#include "services/runtime/safety_monitor_policy.h"

static const char *TAG = "mic_test";

static const uint32_t kTestDurationMs = 5000U;
static const float kRecordGainDb = 36.0f;
static const uint32_t kInputAcquireTimeoutMs = 1000U;
static const uint32_t kReadTimeoutMs = 100U;
static const uint32_t kTaskStackSize = 8192U;
static const UBaseType_t kTaskPriority = 3U;
static const char *kOutputDir = "/sdcard/mic_tests";
static const uint32_t kPeakPassThreshold = 500U;
static const float kRmsPassThreshold = 50.0f;
static const float kZeroPassThreshold = 95.0f;
static const uint32_t kClipThreshold = 32760U;

typedef struct {
    audio_mic_test_snapshot_t snapshot;
    TaskHandle_t task_handle;
    bool running;
    portMUX_TYPE lock;
} audio_mic_test_service_state_t;

typedef struct {
    uint64_t sum_squares;
    int32_t peak;
    uint32_t zero_count;
    uint32_t clip_count;
    uint32_t samples;
} channel_accumulator_t;

typedef struct __attribute__((packed)) {
    char riff_tag[4];
    uint32_t file_size;
    char wave_tag[4];
    char fmt_tag[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data_tag[4];
    uint32_t data_size;
} wav_header_t;

static audio_mic_test_service_state_t s_mic_test = {
    .snapshot = {
        .state = AUDIO_MIC_TEST_STATE_IDLE,
        .last_error = ESP_OK,
        .reason = "未测试",
        .duration_ms = kTestDurationMs,
        .sample_rate_hz = AUDIO_PLATFORM_HW_SAMPLE_RATE,
        .channels = AUDIO_PLATFORM_HW_INPUT_CHANNELS,
        .bits_per_sample = AUDIO_PLATFORM_HW_BITS_PER_SAMPLE,
        .mic_channel_index = 0U,
    },
    .task_handle = NULL,
    .running = false,
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static uint8_t find_mic_channel_index(void)
{
    for (uint8_t i = 0U; AUDIO_PLATFORM_ADC_CHANNEL_FORMAT[i] != '\0'; ++i) {
        if (AUDIO_PLATFORM_ADC_CHANNEL_FORMAT[i] == 'M') {
            return i;
        }
    }
    return 0U;
}

static void snapshot_copy_path(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0U) {
        return;
    }
    const char *safe_src = src != NULL ? src : "";
    size_t i = 0U;
    for (; i + 1U < dst_size && safe_src[i] != '\0'; ++i) {
        dst[i] = safe_src[i];
    }
    dst[i] = '\0';
}

static void publish_state(audio_mic_test_state_t state,
                          esp_err_t err,
                          const char *reason)
{
    taskENTER_CRITICAL(&s_mic_test.lock);
    s_mic_test.snapshot.state = state;
    s_mic_test.snapshot.last_error = err;
    snapshot_copy_path(s_mic_test.snapshot.reason,
                       sizeof(s_mic_test.snapshot.reason), reason);
    taskEXIT_CRITICAL(&s_mic_test.lock);
}

static void publish_final_snapshot(const audio_mic_test_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_mic_test.lock);
    s_mic_test.snapshot = *snapshot;
    s_mic_test.task_handle = NULL;
    s_mic_test.running = false;
    taskEXIT_CRITICAL(&s_mic_test.lock);
}

static esp_err_t ensure_output_dir(void)
{
    return sd_manager_create_dir(kOutputDir);
}

static esp_err_t generate_output_paths(char *wav_path,
                                       size_t wav_path_size,
                                       char *json_path,
                                       size_t json_path_size)
{
    if (wav_path == NULL || json_path == NULL ||
        wav_path_size == 0U || json_path_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    char timestamp[24];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &timeinfo);

    int ret = snprintf(wav_path, wav_path_size, "%s/%s_mic_raw.wav",
                       kOutputDir, timestamp);
    if (ret < 0 || (size_t)ret >= wav_path_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    ret = snprintf(json_path, json_path_size, "%s/%s_mic_report.json",
                   kOutputDir, timestamp);
    if (ret < 0 || (size_t)ret >= json_path_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static void init_wav_header(wav_header_t *header, uint32_t data_bytes)
{
    if (header == NULL) {
        return;
    }

    *header = (wav_header_t) {
        .riff_tag = {'R', 'I', 'F', 'F'},
        .file_size = data_bytes + sizeof(wav_header_t) - 8U,
        .wave_tag = {'W', 'A', 'V', 'E'},
        .fmt_tag = {'f', 'm', 't', ' '},
        .fmt_size = 16U,
        .audio_format = 1U,
        .num_channels = AUDIO_PLATFORM_HW_INPUT_CHANNELS,
        .sample_rate = AUDIO_PLATFORM_HW_SAMPLE_RATE,
        .byte_rate = AUDIO_PLATFORM_HW_SAMPLE_RATE *
                     AUDIO_PLATFORM_HW_INPUT_CHANNELS *
                     (AUDIO_PLATFORM_HW_BITS_PER_SAMPLE / 8U),
        .block_align = AUDIO_PLATFORM_HW_INPUT_CHANNELS *
                       (AUDIO_PLATFORM_HW_BITS_PER_SAMPLE / 8U),
        .bits_per_sample = AUDIO_PLATFORM_HW_BITS_PER_SAMPLE,
        .data_tag = {'d', 'a', 't', 'a'},
        .data_size = data_bytes,
    };
}

static esp_err_t write_json_report(const audio_mic_test_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char json[1536];
    int offset = snprintf(
        json, sizeof(json),
        "{\n"
        "  \"status\": \"%s\",\n"
        "  \"reason\": \"%s\",\n"
        "  \"wav_path\": \"%s\",\n"
        "  \"duration_ms\": %u,\n"
        "  \"bytes_read\": %u,\n"
        "  \"target_bytes\": %u,\n"
        "  \"sample_rate\": %u,\n"
        "  \"channels\": %u,\n"
        "  \"bits_per_sample\": %u,\n"
        "  \"adc_format\": \"%s\",\n"
        "  \"mic_channel_index\": %u,\n"
        "  \"gain_db\": %.1f,\n"
        "  \"channel_stats\": [\n",
        snapshot->state == AUDIO_MIC_TEST_STATE_PASSED ? "pass" : "fail",
        snapshot->reason,
        snapshot->wav_path,
        (unsigned)snapshot->duration_ms,
        (unsigned)snapshot->bytes_read,
        (unsigned)snapshot->target_bytes,
        (unsigned)snapshot->sample_rate_hz,
        (unsigned)snapshot->channels,
        (unsigned)snapshot->bits_per_sample,
        AUDIO_PLATFORM_ADC_CHANNEL_FORMAT,
        (unsigned)snapshot->mic_channel_index,
        (double)kRecordGainDb);
    if (offset < 0 || (size_t)offset >= sizeof(json)) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (uint16_t ch = 0U; ch < snapshot->channels &&
                          ch < AUDIO_MIC_TEST_SERVICE_MAX_CHANNELS; ++ch) {
        const audio_mic_test_channel_stats_t *stats =
            &snapshot->channel_stats[ch];
        int ret = snprintf(
            json + offset, sizeof(json) - (size_t)offset,
            "    {\"channel\": %u, \"rms\": %.2f, \"peak\": %ld, "
            "\"zero_percent\": %.2f, \"clip_count\": %u, \"samples\": %u}%s\n",
            (unsigned)ch,
            (double)stats->rms,
            (long)stats->peak,
            (double)stats->zero_percent,
            (unsigned)stats->clip_count,
            (unsigned)stats->samples,
            ch + 1U < snapshot->channels ? "," : "");
        if (ret < 0 || (size_t)ret >= sizeof(json) - (size_t)offset) {
            return ESP_ERR_INVALID_SIZE;
        }
        offset += ret;
    }

    int ret = snprintf(json + offset, sizeof(json) - (size_t)offset,
                       "  ]\n}\n");
    if (ret < 0 || (size_t)ret >= sizeof(json) - (size_t)offset) {
        return ESP_ERR_INVALID_SIZE;
    }

    char tmp_path[AUDIO_MIC_TEST_SERVICE_PATH_MAX + 5U];
    ret = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", snapshot->json_path);
    if (ret < 0 || (size_t)ret >= sizeof(tmp_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = sd_manager_write_file(tmp_path, json, strlen(json));
    if (err != ESP_OK) {
        return err;
    }
    err = sd_manager_rename_file(tmp_path, snapshot->json_path);
    if (err != ESP_OK) {
        unlink(tmp_path);
    }
    return err;
}

static void accumulate_stats(channel_accumulator_t *accumulators,
                             size_t channel_count,
                             const int16_t *samples,
                             size_t sample_count)
{
    if (accumulators == NULL || samples == NULL || channel_count == 0U) {
        return;
    }

    for (size_t i = 0U; i < sample_count; ++i) {
        const size_t channel = i % channel_count;
        if (channel >= AUDIO_MIC_TEST_SERVICE_MAX_CHANNELS) {
            continue;
        }

        const int32_t value = samples[i];
        const int32_t abs_value = value < 0 ? -value : value;
        channel_accumulator_t *acc = &accumulators[channel];
        acc->sum_squares += (uint64_t)abs_value * (uint64_t)abs_value;
        if (abs_value > acc->peak) {
            acc->peak = abs_value;
        }
        if (value == 0) {
            acc->zero_count++;
        }
        if ((uint32_t)abs_value >= kClipThreshold) {
            acc->clip_count++;
        }
        acc->samples++;
    }
}

static void finalize_stats(const channel_accumulator_t *accumulators,
                           audio_mic_test_snapshot_t *snapshot)
{
    if (accumulators == NULL || snapshot == NULL) {
        return;
    }

    for (uint16_t ch = 0U; ch < snapshot->channels &&
                          ch < AUDIO_MIC_TEST_SERVICE_MAX_CHANNELS; ++ch) {
        const channel_accumulator_t *acc = &accumulators[ch];
        audio_mic_test_channel_stats_t *stats = &snapshot->channel_stats[ch];
        stats->peak = acc->peak;
        stats->clip_count = acc->clip_count;
        stats->samples = acc->samples;
        if (acc->samples > 0U) {
            stats->rms = sqrtf((float)acc->sum_squares / (float)acc->samples);
            stats->zero_percent =
                ((float)acc->zero_count * 100.0f) / (float)acc->samples;
        }
    }
}

static void decide_result(audio_mic_test_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    const uint32_t minimum_bytes = (snapshot->target_bytes * 90U) / 100U;
    const bool enough_bytes = snapshot->bytes_read >= minimum_bytes;
    const uint8_t mic_channel = snapshot->mic_channel_index;
    const audio_mic_test_channel_stats_t *mic_stats =
        &snapshot->channel_stats[mic_channel];
    bool other_channel_has_input = false;

    for (uint16_t ch = 0U; ch < snapshot->channels &&
                          ch < AUDIO_MIC_TEST_SERVICE_MAX_CHANNELS; ++ch) {
        if (ch == mic_channel) {
            continue;
        }
        const audio_mic_test_channel_stats_t *stats =
            &snapshot->channel_stats[ch];
        if ((uint32_t)stats->peak > kPeakPassThreshold &&
            stats->rms > kRmsPassThreshold &&
            stats->zero_percent < kZeroPassThreshold) {
            other_channel_has_input = true;
            break;
        }
    }

    if (!enough_bytes) {
        snapshot->state = AUDIO_MIC_TEST_STATE_FAILED;
        snapshot_copy_path(snapshot->reason, sizeof(snapshot->reason),
                           "读取不足");
    } else if ((uint32_t)mic_stats->peak <= kPeakPassThreshold ||
               mic_stats->rms <= kRmsPassThreshold ||
               mic_stats->zero_percent >= kZeroPassThreshold) {
        snapshot->state = AUDIO_MIC_TEST_STATE_FAILED;
        snapshot_copy_path(snapshot->reason, sizeof(snapshot->reason),
                           other_channel_has_input ? "通道不匹配" : "无输入");
    } else {
        snapshot->state = AUDIO_MIC_TEST_STATE_PASSED;
        snapshot_copy_path(snapshot->reason, sizeof(snapshot->reason), "通过");
    }
    snapshot->last_error = ESP_OK;
}

static void audio_mic_test_task(void *arg)
{
    (void)arg;

    audio_mic_test_snapshot_t snapshot = {
        .state = AUDIO_MIC_TEST_STATE_FAILED,
        .last_error = ESP_OK,
        .duration_ms = kTestDurationMs,
        .sample_rate_hz = AUDIO_PLATFORM_HW_SAMPLE_RATE,
        .channels = AUDIO_PLATFORM_HW_INPUT_CHANNELS,
        .bits_per_sample = AUDIO_PLATFORM_HW_BITS_PER_SAMPLE,
        .mic_channel_index = find_mic_channel_index(),
    };
    snapshot_copy_path(snapshot.reason, sizeof(snapshot.reason), "启动失败");

    bool foreground_audio_active = false;
    bool input_acquired = false;
    FILE *wav_file = NULL;
    int16_t *buffer = NULL;
    channel_accumulator_t accumulators[AUDIO_MIC_TEST_SERVICE_MAX_CHANNELS] = {0};

    ESP_LOGI(TAG, "MIC_TEST: START");
    publish_state(AUDIO_MIC_TEST_STATE_RUNNING, ESP_OK, "测试中");

    esp_err_t err = safety_monitor_policy_set_foreground_audio_active(
        true, "mic_test");
    if (err != ESP_OK) {
        snapshot.last_error = err;
        snapshot_copy_path(snapshot.reason, sizeof(snapshot.reason), "后台占用");
        goto finish;
    }
    foreground_audio_active = true;

    err = audio_codec_acquire_input(AUDIO_CODEC_OWNER_AUDIO_RECORDER,
                                    kInputAcquireTimeoutMs);
    if (err != ESP_OK) {
        snapshot.last_error = err;
        snapshot_copy_path(snapshot.reason, sizeof(snapshot.reason), "麦克风占用");
        goto finish;
    }
    input_acquired = true;
    ESP_LOGI(TAG, "MIC_TEST: ACQUIRED owner=audio_recorder");

    err = audio_codec_set_record_gain(kRecordGainDb);
    ESP_LOGI(TAG, "MIC_TEST: GAIN requested_db=%.1f result=%s",
             (double)kRecordGainDb, esp_err_to_name(err));
    if (err != ESP_OK) {
        snapshot.last_error = err;
        snapshot_copy_path(snapshot.reason, sizeof(snapshot.reason), "增益失败");
        goto finish;
    }

    err = ensure_output_dir();
    if (err != ESP_OK) {
        snapshot.last_error = err;
        snapshot_copy_path(snapshot.reason, sizeof(snapshot.reason), "SD失败");
        goto finish;
    }

    err = generate_output_paths(snapshot.wav_path, sizeof(snapshot.wav_path),
                                snapshot.json_path, sizeof(snapshot.json_path));
    if (err != ESP_OK) {
        snapshot.last_error = err;
        snapshot_copy_path(snapshot.reason, sizeof(snapshot.reason), "路径失败");
        goto finish;
    }

    const uint32_t bytes_per_sample = AUDIO_PLATFORM_HW_BITS_PER_SAMPLE / 8U;
    const uint32_t frame_bytes = AUDIO_PLATFORM_HW_INPUT_CHANNELS *
                                 bytes_per_sample;
    snapshot.target_bytes = (AUDIO_PLATFORM_HW_SAMPLE_RATE * frame_bytes *
                             kTestDurationMs) / 1000U;

    char tmp_wav_path[AUDIO_MIC_TEST_SERVICE_PATH_MAX + 5U];
    int ret = snprintf(tmp_wav_path, sizeof(tmp_wav_path), "%s.tmp",
                       snapshot.wav_path);
    if (ret < 0 || (size_t)ret >= sizeof(tmp_wav_path)) {
        snapshot.last_error = ESP_ERR_INVALID_SIZE;
        snapshot_copy_path(snapshot.reason, sizeof(snapshot.reason), "路径失败");
        goto finish;
    }

    wav_file = fopen(tmp_wav_path, "wb");
    if (wav_file == NULL) {
        snapshot.last_error = ESP_FAIL;
        snapshot_copy_path(snapshot.reason, sizeof(snapshot.reason), "SD失败");
        goto finish;
    }

    wav_header_t empty_header = {0};
    if (fwrite(&empty_header, 1, sizeof(empty_header), wav_file) !=
        sizeof(empty_header)) {
        snapshot.last_error = ESP_FAIL;
        snapshot_copy_path(snapshot.reason, sizeof(snapshot.reason), "SD失败");
        goto finish;
    }

    const size_t buffer_bytes = 4096U;
    buffer = (int16_t *)heap_caps_malloc(buffer_bytes, MALLOC_CAP_SPIRAM);
    if (buffer == NULL) {
        snapshot.last_error = ESP_ERR_NO_MEM;
        snapshot_copy_path(snapshot.reason, sizeof(snapshot.reason), "内存不足");
        goto finish;
    }

    ESP_LOGI(TAG,
             "MIC_TEST: RECORDING duration_ms=%u rate=%u channels=%u "
             "adc_format=%s mic_channel=%u gain_db=%.1f",
             (unsigned)kTestDurationMs,
             (unsigned)AUDIO_PLATFORM_HW_SAMPLE_RATE,
             (unsigned)AUDIO_PLATFORM_HW_INPUT_CHANNELS,
             AUDIO_PLATFORM_ADC_CHANNEL_FORMAT,
             (unsigned)snapshot.mic_channel_index,
             (double)kRecordGainDb);

    while (snapshot.bytes_read < snapshot.target_bytes) {
        const size_t remaining = snapshot.target_bytes - snapshot.bytes_read;
        const size_t target = remaining < buffer_bytes ? remaining : buffer_bytes;
        size_t bytes_read = 0U;
        err = audio_codec_read(buffer, target, &bytes_read,
                               pdMS_TO_TICKS(kReadTimeoutMs));
        if (err != ESP_OK && !(err == ESP_ERR_TIMEOUT && bytes_read > 0U)) {
            snapshot.last_error = err;
            snapshot_copy_path(snapshot.reason, sizeof(snapshot.reason),
                               "读取失败");
            goto finish;
        }
        if (bytes_read == 0U) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        const size_t aligned_bytes = bytes_read - (bytes_read % frame_bytes);
        if (aligned_bytes == 0U) {
            continue;
        }
        if (fwrite(buffer, 1, aligned_bytes, wav_file) != aligned_bytes) {
            snapshot.last_error = ESP_FAIL;
            snapshot_copy_path(snapshot.reason, sizeof(snapshot.reason), "SD失败");
            goto finish;
        }
        snapshot.bytes_read += (uint32_t)aligned_bytes;
        accumulate_stats(accumulators, snapshot.channels, buffer,
                         aligned_bytes / sizeof(int16_t));
    }

    wav_header_t header;
    init_wav_header(&header, snapshot.bytes_read);
    if (fseek(wav_file, 0, SEEK_SET) != 0 ||
        fwrite(&header, 1, sizeof(header), wav_file) != sizeof(header)) {
        snapshot.last_error = ESP_FAIL;
        snapshot_copy_path(snapshot.reason, sizeof(snapshot.reason), "SD失败");
        goto finish;
    }
    if (fclose(wav_file) != 0) {
        wav_file = NULL;
        snapshot.last_error = ESP_FAIL;
        snapshot_copy_path(snapshot.reason, sizeof(snapshot.reason), "SD失败");
        goto finish;
    }
    wav_file = NULL;

    err = sd_manager_rename_file(tmp_wav_path, snapshot.wav_path);
    if (err != ESP_OK) {
        unlink(tmp_wav_path);
        snapshot.last_error = err;
        snapshot_copy_path(snapshot.reason, sizeof(snapshot.reason), "SD失败");
        goto finish;
    }

    finalize_stats(accumulators, &snapshot);
    decide_result(&snapshot);

    err = write_json_report(&snapshot);
    if (err != ESP_OK) {
        snapshot.state = AUDIO_MIC_TEST_STATE_FAILED;
        snapshot.last_error = err;
        snapshot_copy_path(snapshot.reason, sizeof(snapshot.reason), "SD失败");
    }

finish:
    if (wav_file != NULL) {
        fclose(wav_file);
    }
    if (buffer != NULL) {
        heap_caps_free(buffer);
    }
    if (input_acquired) {
        (void)audio_codec_release_input(AUDIO_CODEC_OWNER_AUDIO_RECORDER);
    }
    if (foreground_audio_active) {
        (void)safety_monitor_policy_set_foreground_audio_active(
            false, "mic_test_done");
    }

    for (uint16_t ch = 0U; ch < snapshot.channels &&
                          ch < AUDIO_MIC_TEST_SERVICE_MAX_CHANNELS; ++ch) {
        const audio_mic_test_channel_stats_t *stats =
            &snapshot.channel_stats[ch];
        ESP_LOGI(TAG,
                 "MIC_TEST: CH%u rms=%.2f peak=%ld zero_pct=%.2f "
                 "clip=%u samples=%u role=%c",
                 (unsigned)ch,
                 (double)stats->rms,
                 (long)stats->peak,
                 (double)stats->zero_percent,
                 (unsigned)stats->clip_count,
                 (unsigned)stats->samples,
                 AUDIO_PLATFORM_ADC_CHANNEL_FORMAT[ch]);
    }
    ESP_LOGI(TAG,
             "MIC_TEST: SUMMARY mic_channel=%u bytes=%u/%u gain_db=%.1f",
             (unsigned)snapshot.mic_channel_index,
             (unsigned)snapshot.bytes_read,
             (unsigned)snapshot.target_bytes,
             (double)kRecordGainDb);
    ESP_LOGI(TAG, "MIC_TEST: WAV path=%s bytes=%u",
             snapshot.wav_path, (unsigned)snapshot.bytes_read);
    ESP_LOGI(TAG, "MIC_TEST: DONE status=%s reason=%s",
             snapshot.state == AUDIO_MIC_TEST_STATE_PASSED ? "pass" : "fail",
             snapshot.reason);

    publish_final_snapshot(&snapshot);
    vTaskDelete(NULL);
}

esp_err_t audio_mic_test_service_start(void)
{
    taskENTER_CRITICAL(&s_mic_test.lock);
    if (s_mic_test.running ||
        s_mic_test.snapshot.state == AUDIO_MIC_TEST_STATE_RUNNING) {
        taskEXIT_CRITICAL(&s_mic_test.lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_mic_test.running = true;
    s_mic_test.snapshot.state = AUDIO_MIC_TEST_STATE_RUNNING;
    s_mic_test.snapshot.last_error = ESP_OK;
    snapshot_copy_path(s_mic_test.snapshot.reason,
                       sizeof(s_mic_test.snapshot.reason), "测试中");
    s_mic_test.snapshot.bytes_read = 0U;
    s_mic_test.snapshot.target_bytes = 0U;
    memset(s_mic_test.snapshot.channel_stats, 0,
           sizeof(s_mic_test.snapshot.channel_stats));
    taskEXIT_CRITICAL(&s_mic_test.lock);

    TaskHandle_t task = NULL;
    const BaseType_t created = xTaskCreate(audio_mic_test_task, "mic_test",
                                           kTaskStackSize, NULL,
                                           kTaskPriority, &task);
    if (created != pdPASS) {
        taskENTER_CRITICAL(&s_mic_test.lock);
        s_mic_test.running = false;
        taskEXIT_CRITICAL(&s_mic_test.lock);
        publish_state(AUDIO_MIC_TEST_STATE_FAILED, ESP_ERR_NO_MEM, "任务失败");
        return ESP_ERR_NO_MEM;
    }

    taskENTER_CRITICAL(&s_mic_test.lock);
    if (s_mic_test.running) {
        s_mic_test.task_handle = task;
    }
    taskEXIT_CRITICAL(&s_mic_test.lock);
    return ESP_OK;
}

esp_err_t audio_mic_test_service_get_snapshot(
    audio_mic_test_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_mic_test.lock);
    *out_snapshot = s_mic_test.snapshot;
    taskEXIT_CRITICAL(&s_mic_test.lock);
    return ESP_OK;
}
