#include "services/memory_watch/memory_watch_ogg_opus_muxer.h"

#include <string.h>

#define MEMORY_WATCH_OGG_CAPTURE_PATTERN "OggS"
#define MEMORY_WATCH_OGG_HEADER_BYTES 27U
#define MEMORY_WATCH_OGG_MAX_SEGMENTS 255U
#define MEMORY_WATCH_OGG_SEGMENT_BYTES 255U
#define MEMORY_WATCH_OGG_FLAG_BOS 0x02U
#define MEMORY_WATCH_OGG_FLAG_EOS 0x04U
#define MEMORY_WATCH_OGG_CRC_POLY 0x04c11db7U
#define MEMORY_WATCH_OPUS_GRANULE_HZ 48000U
#define MEMORY_WATCH_OPUS_HEAD_BYTES 19U

static esp_err_t memory_watch_ogg_mark_failed(
    memory_watch_ogg_opus_muxer_t *muxer, esp_err_t ret)
{
    if (muxer != NULL)
    {
        muxer->failed = true;
    }
    return ret;
}

static void memory_watch_ogg_write_le16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value & 0xffU);
    out[1] = (uint8_t)((value >> 8) & 0xffU);
}

static void memory_watch_ogg_write_le32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xffU);
    out[1] = (uint8_t)((value >> 8) & 0xffU);
    out[2] = (uint8_t)((value >> 16) & 0xffU);
    out[3] = (uint8_t)((value >> 24) & 0xffU);
}

static void memory_watch_ogg_write_le64(uint8_t *out, uint64_t value)
{
    for (size_t i = 0; i < 8U; ++i)
    {
        out[i] = (uint8_t)((value >> (8U * i)) & 0xffU);
    }
}

static uint32_t memory_watch_ogg_crc_update(uint32_t crc,
                                            const uint8_t *data,
                                            size_t len)
{
    for (size_t i = 0; i < len; ++i)
    {
        crc ^= ((uint32_t)data[i]) << 24;
        for (int bit = 0; bit < 8; ++bit)
        {
            if ((crc & 0x80000000U) != 0U)
            {
                crc = (crc << 1) ^ MEMORY_WATCH_OGG_CRC_POLY;
            }
            else
            {
                crc <<= 1;
            }
        }
    }
    return crc;
}

static esp_err_t memory_watch_ogg_write_page(
    memory_watch_ogg_opus_muxer_t *muxer,
    uint8_t header_type,
    uint64_t granule_position,
    const uint8_t *packet,
    size_t packet_size)
{
    if (muxer == NULL || packet == NULL || packet_size == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (muxer->failed || muxer->finished)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (packet_size > MEMORY_WATCH_OGG_OPUS_MUXER_MAX_PACKET_BYTES)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t full_segments = packet_size / MEMORY_WATCH_OGG_SEGMENT_BYTES;
    const size_t last_segment = packet_size % MEMORY_WATCH_OGG_SEGMENT_BYTES;
    const size_t segment_count = full_segments + 1U;
    if (segment_count > MEMORY_WATCH_OGG_MAX_SEGMENTS)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t header[MEMORY_WATCH_OGG_HEADER_BYTES +
                   MEMORY_WATCH_OGG_MAX_SEGMENTS] = {0};
    memcpy(header, MEMORY_WATCH_OGG_CAPTURE_PATTERN, 4U);
    header[4] = 0; /* Ogg bitstream version 0。 */
    header[5] = header_type;
    memory_watch_ogg_write_le64(&header[6], granule_position);
    memory_watch_ogg_write_le32(&header[14], muxer->config.serial);
    memory_watch_ogg_write_le32(&header[18], muxer->page_sequence);
    header[26] = (uint8_t)segment_count;

    for (size_t i = 0; i < full_segments; ++i)
    {
        header[MEMORY_WATCH_OGG_HEADER_BYTES + i] =
            MEMORY_WATCH_OGG_SEGMENT_BYTES;
    }
    header[MEMORY_WATCH_OGG_HEADER_BYTES + full_segments] =
        (uint8_t)last_segment;

    const size_t header_size = MEMORY_WATCH_OGG_HEADER_BYTES + segment_count;
    uint32_t crc = memory_watch_ogg_crc_update(0U, header, header_size);
    crc = memory_watch_ogg_crc_update(crc, packet, packet_size);
    memory_watch_ogg_write_le32(&header[22], crc);

    esp_err_t ret = muxer->config.write_cb(header, header_size,
                                           muxer->config.user_ctx);
    if (ret != ESP_OK)
    {
        return memory_watch_ogg_mark_failed(muxer, ret);
    }
    ret = muxer->config.write_cb(packet, packet_size,
                                 muxer->config.user_ctx);
    if (ret != ESP_OK)
    {
        return memory_watch_ogg_mark_failed(muxer, ret);
    }

    muxer->page_sequence++;
    return ESP_OK;
}

static esp_err_t memory_watch_ogg_write_opus_head(
    memory_watch_ogg_opus_muxer_t *muxer)
{
    uint8_t packet[MEMORY_WATCH_OPUS_HEAD_BYTES] = {0};
    memcpy(packet, "OpusHead", 8U);
    packet[8] = 1U; /* OpusHead version。 */
    packet[9] = muxer->config.channel_count;
    memory_watch_ogg_write_le16(&packet[10], muxer->config.pre_skip_samples);
    memory_watch_ogg_write_le32(&packet[12], muxer->config.input_sample_rate_hz);
    memory_watch_ogg_write_le16(&packet[16], 0U); /* output gain。 */
    packet[18] = 0U;                              /* channel mapping family。 */

    return memory_watch_ogg_write_page(muxer, MEMORY_WATCH_OGG_FLAG_BOS, 0U,
                                       packet, sizeof(packet));
}

static esp_err_t memory_watch_ogg_write_opus_tags(
    memory_watch_ogg_opus_muxer_t *muxer)
{
    static const char kVendor[] = "AI Memory Watch";
    const uint32_t vendor_len = (uint32_t)(sizeof(kVendor) - 1U);
    uint8_t packet[8U + 4U + sizeof(kVendor) - 1U + 4U] = {0};
    memcpy(packet, "OpusTags", 8U);
    memory_watch_ogg_write_le32(&packet[8], vendor_len);
    memcpy(&packet[12], kVendor, vendor_len);
    memory_watch_ogg_write_le32(&packet[12U + vendor_len], 0U);

    return memory_watch_ogg_write_page(muxer, 0U, 0U, packet, sizeof(packet));
}

esp_err_t memory_watch_ogg_opus_muxer_init(
    memory_watch_ogg_opus_muxer_t *muxer,
    const memory_watch_ogg_opus_muxer_config_t *config)
{
    if (muxer == NULL || config == NULL || config->write_cb == NULL ||
        config->input_sample_rate_hz == 0U || config->channel_count == 0U ||
        config->frame_duration_ms == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(muxer, 0, sizeof(*muxer));
    muxer->config = *config;
    return ESP_OK;
}

esp_err_t memory_watch_ogg_opus_muxer_write_headers(
    memory_watch_ogg_opus_muxer_t *muxer)
{
    if (muxer == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (muxer->failed || muxer->finished)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (muxer->headers_written)
    {
        return ESP_OK;
    }

    esp_err_t ret = memory_watch_ogg_write_opus_head(muxer);
    if (ret != ESP_OK)
    {
        return ret;
    }
    ret = memory_watch_ogg_write_opus_tags(muxer);
    if (ret != ESP_OK)
    {
        return ret;
    }

    muxer->headers_written = true;
    return ESP_OK;
}

esp_err_t memory_watch_ogg_opus_muxer_write_audio_packet(
    memory_watch_ogg_opus_muxer_t *muxer,
    const uint8_t *packet,
    size_t packet_size,
    bool final_packet)
{
    if (muxer == NULL || packet == NULL || packet_size == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (muxer->failed || muxer->finished)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = memory_watch_ogg_opus_muxer_write_headers(muxer);
    if (ret != ESP_OK)
    {
        return ret;
    }

    /*
     * RFC 7845 的 granule position 使用 48 kHz PCM sample 计数；V1 recorder
     * 后续按固定 frame_duration_ms 逐 packet 写入，因此这里做整数时基转换。
     */
    const uint64_t next_granule_position =
        muxer->granule_position +
        (uint64_t)muxer->config.frame_duration_ms *
        (MEMORY_WATCH_OPUS_GRANULE_HZ / 1000U);

    const uint8_t flags = final_packet ? MEMORY_WATCH_OGG_FLAG_EOS : 0U;
    ret = memory_watch_ogg_write_page(muxer, flags,
                                      next_granule_position,
                                      packet, packet_size);
    if (ret != ESP_OK)
    {
        return ret;
    }

    muxer->granule_position = next_granule_position;
    if (final_packet)
    {
        muxer->finished = true;
    }
    return ESP_OK;
}
