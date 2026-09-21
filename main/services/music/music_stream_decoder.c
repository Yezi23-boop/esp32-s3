#include "music_stream_decoder.h"

#include "esp_audio_dec_default.h"
#include "esp_audio_dec_reg.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_log.h"
#include "esp_opus_dec.h"

static const char *TAG = "music_decoder";

struct music_stream_decoder
{
    esp_audio_simple_dec_handle_t handle;
};

static bool s_default_registered = false;

esp_err_t music_stream_decoder_open(music_stream_decoder_t **out_decoder)
{
    if (out_decoder == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_decoder = NULL;

    if (!s_default_registered)
    {
        if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK ||
            esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK)
        {
            esp_audio_simple_dec_unregister_default();
            esp_audio_dec_unregister_default();
            return ESP_ERR_NO_MEM;
        }
        s_default_registered = true;
    }

    music_stream_decoder_t *decoder = calloc(1, sizeof(*decoder));
    if (decoder == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_opus_dec_cfg_t opus_config = ESP_OPUS_DEC_CONFIG_DEFAULT();
    opus_config.sample_rate = 48000U;
    opus_config.channel = ESP_AUDIO_MONO;
    opus_config.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS;
    opus_config.self_delimited = false;
    esp_audio_simple_dec_cfg_t config = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_RAW_OPUS,
        .dec_cfg = &opus_config,
        .cfg_size = sizeof(opus_config),
        .use_frame_dec = true,
    };
    if (esp_audio_simple_dec_open(&config, &decoder->handle) !=
        ESP_AUDIO_ERR_OK)
    {
        free(decoder);
        return ESP_ERR_NO_MEM;
    }

    *out_decoder = decoder;
    ESP_LOGI(TAG, "48 kHz mono Opus stream decoder opened");
    return ESP_OK;
}

esp_err_t music_stream_decoder_process(
    music_stream_decoder_t *decoder, const uint8_t *packet,
    size_t packet_bytes, uint8_t *output, size_t output_capacity,
    size_t *decoded_bytes)
{
    if (decoded_bytes != NULL)
    {
        *decoded_bytes = 0;
    }
    if (decoder == NULL || decoder->handle == NULL ||
        packet == NULL || packet_bytes == 0U ||
        (output == NULL && output_capacity > 0))
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_audio_simple_dec_raw_t raw = {
        .buffer = (uint8_t *)packet,
        .len = packet_bytes,
        .eos = false,
        .consumed = 0,
    };
    esp_audio_simple_dec_out_t frame = {
        .buffer = output,
        .len = output_capacity,
        .needed_size = 0,
        .decoded_size = 0,
    };
    const esp_audio_err_t ret =
        esp_audio_simple_dec_process(decoder->handle, &raw, &frame);
    if (decoded_bytes != NULL)
    {
        *decoded_bytes = frame.decoded_size;
    }
    if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH)
    {
        ESP_LOGW(TAG, "PCM output buffer too small: need=%lu",
                 (unsigned long)frame.needed_size);
        return ESP_ERR_INVALID_SIZE;
    }
    if (ret != ESP_AUDIO_ERR_OK || raw.consumed != packet_bytes)
    {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t music_stream_decoder_get_info(
    const music_stream_decoder_t *decoder, music_stream_decoder_info_t *info)
{
    if (decoder == NULL || decoder->handle == NULL || info == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_audio_simple_dec_info_t decoder_info = {0};
    const esp_audio_err_t ret =
        esp_audio_simple_dec_get_info(decoder->handle, &decoder_info);
    if (ret != ESP_AUDIO_ERR_OK)
    {
        return ESP_ERR_NOT_FOUND;
    }
    info->sample_rate = decoder_info.sample_rate;
    info->bits_per_sample = decoder_info.bits_per_sample;
    info->channels = decoder_info.channel;
    info->bitrate = decoder_info.bitrate;
    return ESP_OK;
}

void music_stream_decoder_close(music_stream_decoder_t *decoder)
{
    if (decoder == NULL)
    {
        return;
    }
    if (decoder->handle != NULL)
    {
        esp_audio_simple_dec_close(decoder->handle);
    }
    free(decoder);
    if (s_default_registered)
    {
        esp_audio_simple_dec_unregister_default();
        esp_audio_dec_unregister_default();
        s_default_registered = false;
    }
    ESP_LOGI(TAG, "Opus stream decoder closed");
}
