#include "services/memory_watch_recorder.h"

#include <string.h>

#include "audio_codec.h"
#include "audio_platform_config.h"
#include "encoder/impl/esp_opus_enc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_audio_types.h"
#include "freertos/FreeRTOS.h"
#include "services/background_service_manager.h"

static const char *TAG = "memory_watch_rec";

typedef struct
{
    const memory_watch_recorder_config_t *config;
    uint32_t ogg_bytes;
} memory_watch_recorder_write_ctx_t;

static esp_err_t memory_watch_recorder_write_proxy(const uint8_t *data,
                                                   size_t len,
                                                   void *user_ctx)
{
    memory_watch_recorder_write_ctx_t *ctx =
        (memory_watch_recorder_write_ctx_t *)user_ctx;
    if (ctx == NULL || ctx->config == NULL || ctx->config->write_cb == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ctx->config->write_cb(data, len,
                                          ctx->config->write_user_ctx);
    if (ret == ESP_OK)
    {
        ctx->ogg_bytes += (uint32_t)len;
    }
    return ret;
}

static esp_err_t memory_watch_recorder_audio_err_to_esp(esp_audio_err_t ret)
{
    switch (ret)
    {
    case ESP_AUDIO_ERR_OK:
        return ESP_OK;
    case ESP_AUDIO_ERR_MEM_LACK:
        return ESP_ERR_NO_MEM;
    case ESP_AUDIO_ERR_INVALID_PARAMETER:
        return ESP_ERR_INVALID_ARG;
    default:
        return ESP_FAIL;
    }
}

static uint32_t memory_watch_recorder_normalize_min_duration(
    const memory_watch_recorder_config_t *config)
{
    if (config->min_duration_ms == 0U)
    {
        return MEMORY_WATCH_RECORDER_DEFAULT_MIN_DURATION_MS;
    }
    return config->min_duration_ms;
}

static uint32_t memory_watch_recorder_normalize_max_duration(
    const memory_watch_recorder_config_t *config)
{
    if (config->max_duration_ms == 0U)
    {
        return MEMORY_WATCH_RECORDER_DEFAULT_MAX_DURATION_MS;
    }
    return config->max_duration_ms;
}

static uint32_t memory_watch_recorder_normalize_input_timeout(
    const memory_watch_recorder_config_t *config)
{
    if (config->input_timeout_ms == 0U)
    {
        return MEMORY_WATCH_RECORDER_DEFAULT_INPUT_TIMEOUT_MS;
    }
    return config->input_timeout_ms;
}

static uint32_t memory_watch_recorder_normalize_read_timeout(
    const memory_watch_recorder_config_t *config)
{
    if (config->read_timeout_ms == 0U)
    {
        return MEMORY_WATCH_RECORDER_DEFAULT_READ_TIMEOUT_MS;
    }
    return config->read_timeout_ms;
}

static bool memory_watch_recorder_should_stop(
    const memory_watch_recorder_config_t *config,
    uint32_t duration_ms)
{
    const uint32_t min_duration_ms =
        memory_watch_recorder_normalize_min_duration(config);
    if (duration_ms < min_duration_ms)
    {
        return false;
    }
    if (config->should_stop_cb == NULL)
    {
        return false;
    }
    return config->should_stop_cb(config->should_stop_user_ctx);
}

static bool memory_watch_recorder_should_abort(
    const memory_watch_recorder_config_t *config)
{
    if (config->should_abort_cb == NULL)
    {
        return false;
    }
    return config->should_abort_cb(config->should_abort_user_ctx);
}

static void memory_watch_recorder_convert_hw_to_opus_frame(
    const int16_t *hw_pcm,
    size_t hw_frame_count,
    int16_t *opus_pcm,
    size_t opus_sample_count)
{
    /*
     * 当前板级输入格式是 "MR"：第 0 通道是主麦，第 1 通道是参考。
     * V1 只上传用户语音，因此先取主麦通道，再做 24 kHz -> 16 kHz 线性重采样。
     */
    for (size_t i = 0; i < opus_sample_count; ++i)
    {
        const uint64_t pos_q16 =
            ((uint64_t)i * AUDIO_PLATFORM_HW_SAMPLE_RATE * 65536ULL) /
            AUDIO_PLATFORM_SR_SAMPLE_RATE;
        size_t index = (size_t)(pos_q16 >> 16);
        const uint32_t frac = (uint32_t)(pos_q16 & 0xffffU);
        if (index >= hw_frame_count)
        {
            index = hw_frame_count - 1U;
        }
        size_t next_index = index + 1U;
        if (next_index >= hw_frame_count)
        {
            next_index = index;
        }

        const int32_t sample_a =
            hw_pcm[index * AUDIO_PLATFORM_HW_INPUT_CHANNELS];
        const int32_t sample_b =
            hw_pcm[next_index * AUDIO_PLATFORM_HW_INPUT_CHANNELS];
        opus_pcm[i] =
            (int16_t)(sample_a + (((sample_b - sample_a) * (int32_t)frac) >> 16));
    }
}

static esp_err_t memory_watch_recorder_open_encoder(void **encoder,
                                                    int *input_bytes,
                                                    int *output_bytes)
{
    if (encoder == NULL || input_bytes == NULL || output_bytes == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_opus_enc_config_t opus_cfg = {
        .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,
        .channel = ESP_AUDIO_MONO,
        .bits_per_sample = ESP_AUDIO_BIT16,
        .bitrate = ESP_OPUS_BITRATE_AUTO,
        .frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
        .application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO,
        .complexity = 0,
        .enable_fec = false,
        .enable_dtx = true,
        .enable_vbr = true,
    };
    esp_audio_err_t audio_ret = esp_opus_enc_open(
        &opus_cfg, sizeof(esp_opus_enc_config_t), encoder);
    esp_err_t ret = memory_watch_recorder_audio_err_to_esp(audio_ret);
    if (ret != ESP_OK || *encoder == NULL)
    {
        ESP_LOGE(TAG, "failed to open opus encoder: %d", audio_ret);
        return ret != ESP_OK ? ret : ESP_ERR_NO_MEM;
    }

    audio_ret = esp_opus_enc_get_frame_size(*encoder, input_bytes,
                                            output_bytes);
    ret = memory_watch_recorder_audio_err_to_esp(audio_ret);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "failed to query opus frame size: %d", audio_ret);
        esp_opus_enc_close(*encoder);
        *encoder = NULL;
    }
    return ret;
}

static esp_err_t memory_watch_recorder_encode_frame(
    void *encoder,
    const int16_t *pcm,
    int input_bytes,
    uint8_t *opus_out,
    int output_bytes,
    uint32_t *encoded_bytes)
{
    if (encoder == NULL || pcm == NULL || opus_out == NULL ||
        encoded_bytes == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_audio_enc_in_frame_t in_frame = {
        .buffer = (uint8_t *)pcm,
        .len = (uint32_t)input_bytes,
    };
    esp_audio_enc_out_frame_t out_frame = {
        .buffer = opus_out,
        .len = (uint32_t)output_bytes,
        .encoded_bytes = 0,
        .pts = 0,
    };
    esp_audio_err_t audio_ret =
        esp_opus_enc_process(encoder, &in_frame, &out_frame);
    esp_err_t ret = memory_watch_recorder_audio_err_to_esp(audio_ret);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "opus encode failed: %d", audio_ret);
        return ret;
    }
    *encoded_bytes = out_frame.encoded_bytes;
    return ESP_OK;
}

esp_err_t memory_watch_recorder_capture_ogg_opus(
    const memory_watch_recorder_config_t *config,
    memory_watch_recorder_result_t *out_result)
{
    if (config == NULL || config->write_cb == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "capture: start: max_dur=%lu min_dur=%lu input_timeout=%lu read_timeout=%lu",
             (unsigned long)config->max_duration_ms,
             (unsigned long)config->min_duration_ms,
             (unsigned long)config->input_timeout_ms,
             (unsigned long)config->read_timeout_ms);

    const uint32_t max_duration_ms =
        memory_watch_recorder_normalize_max_duration(config);
    const uint32_t min_duration_ms =
        memory_watch_recorder_normalize_min_duration(config);
    if (min_duration_ms > max_duration_ms ||
        max_duration_ms > MEMORY_WATCH_RECORDER_DEFAULT_MAX_DURATION_MS)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (out_result != NULL)
    {
        memset(out_result, 0, sizeof(*out_result));
    }

    bool foreground_active = false;
    bool codec_ready = false;
    bool input_acquired = false;
    void *encoder = NULL;
    int encoder_input_bytes = 0;
    int encoder_output_bytes = 0;
    int16_t *hw_pcm = NULL;
    int16_t *opus_pcm = NULL;
    uint8_t *opus_out = NULL;
    esp_err_t ret = ESP_OK;

    ret = background_service_manager_set_foreground_audio_active(
        true, "memory_watch_recording");
    if (ret != ESP_OK)
    {
        return ret;
    }
    foreground_active = true;

    ret = audio_codec_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "capture: audio_codec_init failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    codec_ready = true;

    ret = audio_codec_acquire_input(
        AUDIO_CODEC_OWNER_HERMES,
        memory_watch_recorder_normalize_input_timeout(config));
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "capture: acquire_input failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    input_acquired = true;

    if (config->record_gain_db > 0.0f)
    {
        (void)audio_codec_set_record_gain(config->record_gain_db);
    }

    ret = memory_watch_recorder_open_encoder(&encoder, &encoder_input_bytes,
                                             &encoder_output_bytes);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "capture: open_encoder failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    const size_t opus_sample_count =
        (AUDIO_PLATFORM_SR_SAMPLE_RATE *
         MEMORY_WATCH_RECORDER_OPUS_FRAME_DURATION_MS) /
        1000U;
    const size_t hw_frame_count =
        (AUDIO_PLATFORM_HW_SAMPLE_RATE *
         MEMORY_WATCH_RECORDER_OPUS_FRAME_DURATION_MS) /
        1000U;
    const size_t hw_bytes =
        hw_frame_count * AUDIO_PLATFORM_HW_INPUT_CHANNELS * sizeof(int16_t);
    const size_t opus_pcm_bytes = opus_sample_count * sizeof(int16_t);
    if ((size_t)encoder_input_bytes != opus_pcm_bytes)
    {
        ESP_LOGE(TAG, "capture: frame size mismatch: enc_in=%d opus_pcm=%u",
                 encoder_input_bytes, (unsigned int)opus_pcm_bytes);
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    ESP_LOGI(TAG, "capture: params ok: max_dur=%lu min_dur=%lu hw_bytes=%u opus_pcm=%u enc_in=%d enc_out=%d max_pkts=%lu",
             (unsigned long)max_duration_ms, (unsigned long)min_duration_ms,
             (unsigned int)hw_bytes, (unsigned int)opus_pcm_bytes,
             encoder_input_bytes, encoder_output_bytes,
             (unsigned long)(max_duration_ms / MEMORY_WATCH_RECORDER_OPUS_FRAME_DURATION_MS));

    /* 三个缓冲区全部放在 PSRAM，释放 internal RAM。
     * hw_pcm 由 audio_codec_read 填充（CPU 拷贝，非 DMA 直写），PSRAM 可用。 */
    ESP_LOGI(TAG, "capture: alloc: hw=%u opus_pcm=%u opus_out=%d internal_free=%lu",
             (unsigned int)hw_bytes, (unsigned int)opus_pcm_bytes,
             encoder_output_bytes,
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    hw_pcm = (int16_t *)heap_caps_malloc(hw_bytes, MALLOC_CAP_SPIRAM |
                                                     MALLOC_CAP_8BIT);
    opus_pcm = (int16_t *)heap_caps_malloc(opus_pcm_bytes, MALLOC_CAP_SPIRAM |
                                                            MALLOC_CAP_8BIT);
    opus_out = (uint8_t *)heap_caps_malloc((size_t)encoder_output_bytes,
                                           MALLOC_CAP_SPIRAM |
                                               MALLOC_CAP_8BIT);
    if (hw_pcm == NULL || opus_pcm == NULL || opus_out == NULL)
    {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    memory_watch_recorder_write_ctx_t write_ctx = {
        .config = config,
        .ogg_bytes = 0,
    };
    memory_watch_ogg_opus_muxer_t muxer;
    memory_watch_ogg_opus_muxer_config_t muxer_config = {
        .serial = config->ogg_serial,
        .input_sample_rate_hz = AUDIO_PLATFORM_SR_SAMPLE_RATE,
        .pre_skip_samples = 0,
        .frame_duration_ms = MEMORY_WATCH_RECORDER_OPUS_FRAME_DURATION_MS,
        .channel_count = 1,
        .write_cb = memory_watch_recorder_write_proxy,
        .user_ctx = &write_ctx,
    };
    ret = memory_watch_ogg_opus_muxer_init(&muxer, &muxer_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "capture: muxer_init failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    const uint32_t max_packets =
        max_duration_ms / MEMORY_WATCH_RECORDER_OPUS_FRAME_DURATION_MS;
    const TickType_t read_timeout_ticks = pdMS_TO_TICKS(
        memory_watch_recorder_normalize_read_timeout(config));
    uint32_t encoded_packets = 0;
    uint32_t opus_bytes = 0;
    for (uint32_t packet_index = 0; packet_index < max_packets; ++packet_index)
    {
        if (memory_watch_recorder_should_abort(config))
        {
            ret = ESP_ERR_INVALID_STATE;
            goto cleanup;
        }

        const uint32_t duration_after_packet =
            (packet_index + 1U) * MEMORY_WATCH_RECORDER_OPUS_FRAME_DURATION_MS;
        const bool final_packet =
            (packet_index + 1U) >= max_packets ||
            memory_watch_recorder_should_stop(config, duration_after_packet);

        size_t bytes_read = 0;
        ret = audio_codec_read(hw_pcm, hw_bytes, &bytes_read,
                               read_timeout_ticks);
        if (ret != ESP_OK || bytes_read != hw_bytes)
        {
            ESP_LOGE(TAG, "capture: audio_read fail: ret=%s bytes_read=%u expected=%u pkt=%lu",
                     esp_err_to_name(ret), (unsigned int)bytes_read,
                     (unsigned int)hw_bytes, (unsigned long)packet_index);
            ret = ret != ESP_OK ? ret : ESP_ERR_TIMEOUT;
            goto cleanup;
        }

        memory_watch_recorder_convert_hw_to_opus_frame(
            hw_pcm, hw_frame_count, opus_pcm, opus_sample_count);

        uint32_t encoded_bytes = 0;
        ret = memory_watch_recorder_encode_frame(
            encoder, opus_pcm, encoder_input_bytes, opus_out,
            encoder_output_bytes, &encoded_bytes);
        if (ret != ESP_OK)
        {
            goto cleanup;
        }

        ret = memory_watch_ogg_opus_muxer_write_audio_packet(
            &muxer, opus_out, encoded_bytes, final_packet);
        if (ret != ESP_OK)
        {
            goto cleanup;
        }

        encoded_packets++;
        opus_bytes += encoded_bytes;
        if (final_packet)
        {
            break;
        }
    }

    if (encoded_packets == 0U)
    {
        ESP_LOGE(TAG, "capture: no packets encoded");
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    ESP_LOGI(TAG, "capture: done: packets=%lu opus_bytes=%lu ogg_bytes=%lu dur_ms=%lu",
             (unsigned long)encoded_packets, (unsigned long)opus_bytes,
             (unsigned long)write_ctx.ogg_bytes,
             (unsigned long)(encoded_packets * MEMORY_WATCH_RECORDER_OPUS_FRAME_DURATION_MS));

    if (out_result != NULL)
    {
        out_result->duration_ms =
            encoded_packets * MEMORY_WATCH_RECORDER_OPUS_FRAME_DURATION_MS;
        out_result->opus_packets = encoded_packets;
        out_result->opus_bytes = opus_bytes;
        out_result->ogg_bytes = write_ctx.ogg_bytes;
    }

cleanup:
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "capture: cleanup with error: %s (fg=%d codec=%d input=%d enc=%p)",
                 esp_err_to_name(ret), foreground_active, codec_ready,
                 input_acquired, encoder);
    }
    if (encoder != NULL)
    {
        esp_opus_enc_close(encoder);
    }
    if (opus_out != NULL)
    {
        heap_caps_free(opus_out);
    }
    if (opus_pcm != NULL)
    {
        heap_caps_free(opus_pcm);
    }
    if (hw_pcm != NULL)
    {
        heap_caps_free(hw_pcm);
    }
    if (input_acquired)
    {
        (void)audio_codec_release_input(AUDIO_CODEC_OWNER_HERMES);
    }
    if (codec_ready)
    {
        (void)audio_codec_deinit();
    }
    if (foreground_active)
    {
        (void)background_service_manager_set_foreground_audio_active(
            false, "memory_watch_recording_done");
    }
    return ret;
}
