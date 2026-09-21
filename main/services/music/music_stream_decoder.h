#ifndef MUSIC_STREAM_DECODER_H
#define MUSIC_STREAM_DECODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct music_stream_decoder music_stream_decoder_t;

    /** 流式解码输出格式信息。 */
    typedef struct
    {
        uint32_t sample_rate;
        uint8_t bits_per_sample;
        uint8_t channels;
        uint32_t bitrate;
    } music_stream_decoder_info_t;

    /**
     * @brief 创建 48 kHz 单声道裸 Opus 解码器。
     *
     * 解码器只拥有 esp_audio_codec 的 decoder handle；每次只接收一个完整
     * Opus 包，容器边界由 music_stream_player 负责。
     *
     * @param[out] out_decoder 输出解码器句柄。
     * @return ESP_OK 表示成功；ESP_ERR_INVALID_ARG 表示参数为空；
     *         ESP_ERR_NO_MEM 表示解码器注册或创建失败。
     */
    esp_err_t music_stream_decoder_open(
        music_stream_decoder_t **out_decoder);

    /**
     * @brief 将一个完整裸 Opus 包解码为 PCM。
     *
     * RAW_OPUS 只支持一帧输入；函数验证 decoder 已完整消费该包，避免服务端
     * 帧边界异常时丢失或拼接压缩数据。
     *
     * @param[in] decoder 解码器句柄。
     * @param[in] packet Opus 包，不包含服务端长度前缀；函数不会修改内容。
     * @param[in] packet_bytes Opus 包字节数。
     * @param[out] output PCM 输出缓冲区。
     * @param[in] output_capacity 输出缓冲区容量，单位字节。
     * @param[out] decoded_bytes 实际输出的 PCM 字节数，可为 NULL。
     * @return ESP_OK 表示处理完成；ESP_ERR_INVALID_SIZE 表示输出缓冲区不足；
     *         其他错误表示 decoder 处理失败。
     */
    esp_err_t music_stream_decoder_process(
        music_stream_decoder_t *decoder, const uint8_t *packet,
        size_t packet_bytes, uint8_t *output, size_t output_capacity,
        size_t *decoded_bytes);

    /**
     * @brief 获取已解析出的音频格式。
     * @param[in] decoder 解码器句柄。
     * @param[out] info 输出格式信息。
     * @return ESP_OK 表示信息可用；ESP_ERR_NOT_FOUND 表示尚未解出首帧。
     */
    esp_err_t music_stream_decoder_get_info(
        const music_stream_decoder_t *decoder,
        music_stream_decoder_info_t *info);

    /**
     * @brief 关闭流式解码器并释放 parser/decoder。
     * @param[in] decoder 解码器句柄，可为 NULL。
     */
    void music_stream_decoder_close(music_stream_decoder_t *decoder);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_STREAM_DECODER_H */
