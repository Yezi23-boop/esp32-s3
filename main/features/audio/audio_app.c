/**
 * @file audio_app.c
 * @brief 音频应用层实现 (录音/播放控制)
 */

#include "audio_app.h"
#include "audio_codec.h"
#include "audio_platform_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "services/background_service_manager.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sd_manager.h>
#include <sys/types.h>
#include <sys/stat.h>

static const char *TAG = "audio_app";

/*
 * 音频应用层实现说明：
 * - 当前主要封装录音控制，负责创建录音任务、维护录音状态和写出 WAV 文件；
 * - 驱动初始化与音频流读写仍由 `audio_codec` 提供；
 * - 应用层只处理文件组织、WAV 头回填和状态切换。
 */

static TaskHandle_t s_record_task_handle = NULL; /* 录音任务句柄；任务退出前会自行清空。 */
static volatile bool s_is_recording = false;    /* 录音运行标志，UI/API 写入，录音任务轮询读取。 */
static char s_record_filename[128] = {0};       /* 当前录音输出路径缓存，供后台任务打开文件。 */

static void audio_app_release_recording_resources(bool input_acquired,
                                                  const char *reason)
{
    if (input_acquired)
    {
        (void)audio_codec_release_input(AUDIO_CODEC_OWNER_AUDIO_RECORDER);
    }
    (void)background_service_manager_set_foreground_audio_active(
        false, reason != NULL ? reason : "recording_done");
}

/* PCM WAV 头布局；字段含义需与播放器常见 RIFF/WAVE 约定保持一致。 */
typedef struct
{
    char riff_tag[4];         /**< 固定为 "RIFF"，标识 RIFF 容器。 */
    uint32_t riff_len;        /**< 文件总长度减去前 8 字节。 */
    char wave_tag[4];         /**< 固定为 "WAVE"，标识 WAV 类型。 */
    char fmt_tag[4];          /**< 固定为 "fmt "，标识格式块。 */
    uint32_t fmt_len;         /**< PCM 格式块长度，当前固定为 16 字节。 */
    uint16_t audio_fmt;       /**< 音频格式，PCM 固定为 1。 */
    uint16_t channels;        /**< 声道数。 */
    uint32_t sample_rate;     /**< 采样率，单位为 Hz。 */
    uint32_t byte_rate;       /**< 每秒字节数，用于播放器快速定位数据吞吐。 */
    uint16_t block_align;     /**< 单个采样帧的字节对齐大小。 */
    uint16_t bits_per_sample; /**< 单样本位宽，单位为 bit。 */
    char data_tag[4];         /**< 固定为 "data"，标识数据块。 */
    uint32_t data_len;        /**< PCM 数据区长度，单位为字节。 */
} wav_header_t;

/**
 * @brief 生成标准 PCM WAV 文件头。
 * @param[out] header 输出头结构。
 * @param[in] data_len PCM 数据区字节数。
 * @param[in] sample_rate 采样率，单位为 Hz。
 * @param[in] channels 声道数。
 * @param[in] bits 位深，单位为 bit。
 * @return 无返回值。
 */
static void generate_wav_header(wav_header_t *header, uint32_t data_len, uint32_t sample_rate, uint16_t channels, uint16_t bits)
{
    memcpy(header->riff_tag, "RIFF", 4);
    header->riff_len = data_len + sizeof(wav_header_t) - 8;
    memcpy(header->wave_tag, "WAVE", 4);
    memcpy(header->fmt_tag, "fmt ", 4);
    header->fmt_len = 16;
    header->audio_fmt = 1;
    header->channels = channels;
    header->sample_rate = sample_rate;
    header->byte_rate = sample_rate * channels * bits / 8;
    header->block_align = channels * bits / 8;
    header->bits_per_sample = bits;
    memcpy(header->data_tag, "data", 4);
    header->data_len = data_len;
}

/**
 * @brief 录音后台任务。
 * @param[in] arg 未使用，保留任务签名。
 * @return 无返回值。
 *
 * 任务流程：
 * 1. 创建目标文件并预留 WAV 头；
 * 2. 循环读取音频数据写入文件；
 * 3. 停止后回填 WAV 头并关闭文件。
 *
 * @note 该任务会阻塞执行文件 I/O 和音频读取，因此只应在普通任务上下文创建。
 */
static void record_task(void *arg)
{
    bool input_acquired = false;
    esp_err_t ret = background_service_manager_set_foreground_audio_active(
        true, "recording");
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "无法暂停后台安全监听: %s", esp_err_to_name(ret));
        s_is_recording = false;
        audio_app_release_recording_resources(false, "recording_failed");
        s_record_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    ret = audio_codec_acquire_input(AUDIO_CODEC_OWNER_AUDIO_RECORDER, 500U);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "录音输入资源被占用: %s", esp_err_to_name(ret));
        s_is_recording = false;
        audio_app_release_recording_resources(false, "recording_failed");
        s_record_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    input_acquired = true;

    FILE *f = fopen(s_record_filename, "wb");
    if (f == NULL)
    {
        ESP_LOGE(TAG, "无法创建录音文件: %s", s_record_filename);
        s_is_recording = false;
        audio_app_release_recording_resources(input_acquired, "recording_failed");
        s_record_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    wav_header_t header;
    memset(&header, 0, sizeof(wav_header_t));
    fwrite(&header, 1, sizeof(wav_header_t), f);

    ESP_LOGI(TAG, "开始录音: %s", s_record_filename);
    /* 默认增益提升到 36dB，是为了兼容当前低灵敏度驻极体麦克风链路。 */
    audio_codec_set_record_gain(36.0f);

    size_t buf_size = 4096U; /* 单次读写分片大小，单位为字节；兼顾 I/O 开销与实时性。 */
    uint8_t *buffer = (uint8_t *)malloc(buf_size);
    if (buffer == NULL)
    {
        ESP_LOGE(TAG, "内存不足");
        fclose(f);
        s_is_recording = false;
        audio_app_release_recording_resources(input_acquired, "recording_failed");
        s_record_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    size_t total_bytes = 0;
    const TickType_t read_timeout_ticks = pdMS_TO_TICKS(100); /* 单次音频读取超时，单位为 tick。 */

    while (s_is_recording)
    {
        size_t bytes_read = 0;
        esp_err_t read_res =
            audio_codec_read(buffer, buf_size, &bytes_read, read_timeout_ticks);

        if (read_res == ESP_OK || (read_res == ESP_ERR_TIMEOUT && bytes_read > 0))
        {
            fwrite(buffer, 1, bytes_read, f);
            total_bytes += bytes_read;
        }
        else if (read_res != ESP_ERR_TIMEOUT)
        {
            ESP_LOGW(TAG, "读取音频数据失败: %s", esp_err_to_name(read_res));
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    ESP_LOGI(TAG, "录音结束，正在保存... 总大小: %d 字节", total_bytes);

    generate_wav_header(&header, total_bytes, AUDIO_PLATFORM_HW_SAMPLE_RATE,
                        AUDIO_PLATFORM_HW_INPUT_CHANNELS,
                        AUDIO_PLATFORM_HW_BITS_PER_SAMPLE);
    fseek(f, 0, SEEK_SET);
    fwrite(&header, 1, sizeof(wav_header_t), f);

    fclose(f);
    free(buffer);
    audio_app_release_recording_resources(input_acquired, "recording_done");

    s_record_task_handle = NULL;
    ESP_LOGI(TAG, "录音文件已保存");
    vTaskDelete(NULL);
}

/**
 * @brief 初始化音频应用层。
 * @return 当前实现始终返回 `ESP_OK`，保留统一初始化入口。
 */
esp_err_t audio_app_init(void)
{
    ESP_LOGI(TAG, "音频应用初始化");

    return ESP_OK;
}

/**
 * @brief 启动一次录音任务。
 *
 * 该接口先更新共享输出路径和录音状态，再创建后台任务。
 * 若任务创建失败，会回滚录音状态，避免 UI 误判为“正在录音”。
 *
 * @param[in] filename 录音输出文件路径。
 * @return `ESP_OK` 表示录音任务已创建；
 *         `ESP_ERR_INVALID_STATE` 表示已有录音任务在运行；
 *         `ESP_ERR_INVALID_ARG` 表示文件路径为空；
 *         `ESP_FAIL` 表示任务创建失败。
 *
 * @note 仅允许在任务上下文调用；函数不会等待录音结束。
 */
esp_err_t audio_app_start_record(const char *filename)
{
    if (s_is_recording)
    {
        ESP_LOGW(TAG, "正在录音中，请先停止");
        return ESP_ERR_INVALID_STATE;
    }

    if (filename == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_record_filename, filename, sizeof(s_record_filename) - 1);
    s_record_filename[sizeof(s_record_filename) - 1] = '\0';
    s_is_recording = true;

    BaseType_t ret = xTaskCreate(record_task, "RecTask", 4096, NULL, 5, &s_record_task_handle);
    if (ret != pdPASS)
    {
        ESP_LOGE(TAG, "创建录音任务失败");
        s_is_recording = false;
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief 请求停止当前录音。
 *
 * 这里不直接删除录音任务，而是让任务在下一轮循环自然退出。
 * 这样可以确保 WAV 头回填和文件关闭逻辑一定有机会执行完。
 *
 * @return 即使当前没有录音，也返回 `ESP_OK` 以简化上层状态切换。
 */
esp_err_t audio_app_stop_record(void)
{
    if (!s_is_recording)
    {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "请求停止录音...");
    s_is_recording = false;

    return ESP_OK;
}

/**
 * @brief 查询当前是否正在录音。
 * @return true 表示录音任务仍在运行中；false 表示当前未持有录音循环。
 */
bool audio_app_is_recording(void)
{
    return s_is_recording;
}
