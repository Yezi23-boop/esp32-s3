/**
 * @file espdl_feature_pipeline.h
 * @brief ESP-DL Fbank 特征提取管线（C API）。
 *
 * 从 16kHz 单声道 float PCM 提取 [98, 40] Fbank 特征，
 * 参数与 AudioClassification-Pytorch 训练配置对齐：
 *   - num_mel_bins=40, frame_length=25ms, frame_shift=10ms
 *   - window_type=povey, snip_edges=True, remove_dc_offset=True
 *   - 时间均值归一化
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 采样率，与训练配置一致。 */
#define ESPDL_SAMPLE_RATE_HZ       16000
/** 音频窗口长度（秒）。 */
#define ESPDL_WINDOW_SECONDS       1
/** 1 秒 16kHz 单声道样本数。 */
#define ESPDL_WINDOW_SAMPLES       16000
/** Kaldi snip_edges=True 下 1 秒音频的帧数。 */
#define ESPDL_FEATURE_FRAME_COUNT  98
/** Fbank mel bin 数量。 */
#define ESPDL_FEATURE_BIN_COUNT    40

/** Fbank 特征结构体。 */
typedef struct {
    float *values;   /**< 行主序 [frames, bins] Fbank 数据。 */
    int frames;      /**< 帧数。 */
    int bins;        /**< 每帧维度。 */
} espdl_feature_frame_t;

/**
 * @brief 从 16kHz 单声道 float PCM 提取 Fbank 特征。
 *
 * 输入必须已经是 16kHz 单声道，长度为 ESPDL_WINDOW_SAMPLES。
 * 输出特征会做逐 bin 时间均值归一化。
 *
 * @param[in] pcm 16kHz 单声道 float PCM，范围 [-1, 1]。
 * @param[in] sample_count PCM 样本数，必须等于 ESPDL_WINDOW_SAMPLES。
 * @param[out] out_feature 输出 Fbank 特征。values 缓冲由调用方管理，至少 98*40 个 float。
 * @return ESP_OK 表示成功。
 */
esp_err_t espdl_feature_build_fbank(const float *pcm,
                                    size_t sample_count,
                                    espdl_feature_frame_t *out_feature);

/**
 * @brief 从 int16 PCM 提取 Fbank 特征。
 *
 * 与 espdl_feature_build_fbank 等价，但输入为 I2S/麦克风常见的 int16 格式。
 * 内部会先归一化到 [-1, 1] 再提取。
 *
 * @param[in] pcm_s16 16kHz 单声道 int16 PCM。
 * @param[in] sample_count PCM 样本数，必须等于 ESPDL_WINDOW_SAMPLES。
 * @param[out] out_feature 输出 Fbank 特征。
 * @return ESP_OK 表示成功。
 */
esp_err_t espdl_feature_build_fbank_from_int16(const int16_t *pcm_s16,
                                               size_t sample_count,
                                               espdl_feature_frame_t *out_feature);

#ifdef __cplusplus
}
#endif
