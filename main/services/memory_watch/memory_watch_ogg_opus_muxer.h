#ifndef MEMORY_WATCH_OGG_OPUS_MUXER_H
#define MEMORY_WATCH_OGG_OPUS_MUXER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define MEMORY_WATCH_OGG_OPUS_MUXER_MAX_PACKET_BYTES (254U * 255U)

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Ogg Opus 输出回调。
     *
     * muxer 不持有文件或 HTTP client，调用方可把输出写入文件、内存缓冲区或
     * multipart body。回调必须完整消费本次传入的数据。
     */
    typedef esp_err_t (*memory_watch_ogg_write_cb_t)(const uint8_t *data,
                                                     size_t len,
                                                     void *user_ctx);

    /**
     * @brief Ogg Opus muxer 配置。
     */
    typedef struct
    {
        uint32_t serial;             /**< Ogg logical bitstream serial。 */
        uint32_t input_sample_rate_hz; /**< OpusHead 中记录的原始输入采样率。 */
        uint16_t pre_skip_samples;   /**< OpusHead pre-skip，单位为 48 kHz sample。 */
        uint16_t frame_duration_ms;  /**< 每个 Opus packet 覆盖的时长，单位毫秒。 */
        uint8_t channel_count;       /**< Opus 通道数，手表 V1 固定为 1。 */
        memory_watch_ogg_write_cb_t write_cb; /**< 输出回调。 */
        void *user_ctx;              /**< 透传给输出回调的上下文。 */
    } memory_watch_ogg_opus_muxer_config_t;

    /**
     * @brief Ogg Opus muxer 运行态。
     */
    typedef struct
    {
        memory_watch_ogg_opus_muxer_config_t config; /**< 初始化后的配置副本。 */
        uint32_t page_sequence;       /**< Ogg page sequence number。 */
        uint64_t granule_position;    /**< 48 kHz 时基下的累计 granule position。 */
        bool headers_written;         /**< 是否已输出 OpusHead / OpusTags。 */
        bool finished;                /**< 是否已输出 EOS audio page。 */
        bool failed;                  /**< 输出回调失败后禁止继续复用该流。 */
    } memory_watch_ogg_opus_muxer_t;

    /**
     * @brief 初始化 Ogg Opus muxer。
     * @param[out] muxer muxer 实例。
     * @param[in] config 配置，`write_cb` 不能为空。
     * @return `ESP_OK` 表示成功。
     */
    esp_err_t memory_watch_ogg_opus_muxer_init(
        memory_watch_ogg_opus_muxer_t *muxer,
        const memory_watch_ogg_opus_muxer_config_t *config);

    /**
     * @brief 写出 OpusHead 与 OpusTags 两个 header packet。
     * @return `ESP_OK` 表示成功。
     */
    esp_err_t memory_watch_ogg_opus_muxer_write_headers(
        memory_watch_ogg_opus_muxer_t *muxer);

    /**
     * @brief 写出一个 Opus audio packet。
     *
     * Ogg Opus 的 granule position 使用 48 kHz 时基；调用方应保证每个 packet
     * 对应 `frame_duration_ms` 的音频。最后一个 packet 传 `final_packet=true`
     * 以写出 EOS 标志。
     *
     * @param[in,out] muxer muxer 实例。
     * @param[in] packet Opus packet 数据。
     * @param[in] packet_size Opus packet 字节数，必须大于 0。
     * @param[in] final_packet 是否为最后一个 audio packet。
     * @return `ESP_OK` 表示成功。
     */
    esp_err_t memory_watch_ogg_opus_muxer_write_audio_packet(
        memory_watch_ogg_opus_muxer_t *muxer,
        const uint8_t *packet,
        size_t packet_size,
        bool final_packet);

#ifdef __cplusplus
}
#endif

#endif // MEMORY_WATCH_OGG_OPUS_MUXER_H
