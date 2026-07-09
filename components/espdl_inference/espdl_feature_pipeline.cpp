/**
 * @file espdl_feature_pipeline.cpp
 * @brief ESP-DL Fbank 特征提取管线实现。
 *
 * 使用 esp-dl 的 dl::audio::Fbank 提取与训练侧对齐的 Fbank 特征，
 * 参数严格对齐 AudioClassification-Pytorch 的训练配置。
 */

#include "espdl_feature_pipeline.h"

#include <algorithm>
#include <vector>

#include "dl_fbank.hpp"
#include "dl_speech_features.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "espdl_feature";

/**
 * @brief 生成与训练配置对齐的 Fbank 参数。
 *
 * 参数来源：AudioClassification-Pytorch/configs/espdl/training/dstcn/dstcn_small_espdl_edge_mix_teacher_1s.yml
 *   preprocess_conf.method_args:
 *     sample_frequency: 16000, num_mel_bins: 40
 *     frame_length: 25.0, frame_shift: 10.0
 *     window_type: povey, snip_edges: True, remove_dc_offset: True
 *
 * @return esp-dl Fbank 配置。
 */
static dl::audio::SpeechFeatureConfig make_fbank_config()
{
    dl::audio::SpeechFeatureConfig config;
    config.sample_rate = ESPDL_SAMPLE_RATE_HZ;
    config.frame_length = 25;
    config.frame_shift = 10;
    config.num_mel_bins = ESPDL_FEATURE_BIN_COUNT;
    config.low_freq = 20.0f;
    config.high_freq = 0.0f;
    config.window_type = dl::audio::WinType::POVEY;
    config.use_energy = false;
    config.use_power = true;
    config.use_log_fbank = 1;
    config.remove_dc_offset = true;
    return config;
}

/**
 * @brief 对 [frames, bins] 特征做逐 bin 均值归一化。
 *
 * 训练侧 AudioFeaturizer 执行 feature - feature.mean(1)，
 * 即每个 mel bin 减去整窗时间均值；板端必须同步，否则输入分布会漂移。
 *
 * @param[in,out] feature 行主序特征缓冲。
 * @param[in] frames 特征帧数。
 * @param[in] bins 每帧维度。
 */
static void normalize_feature_by_time_mean(float *feature, int frames, int bins)
{
    if (feature == nullptr || frames <= 0 || bins <= 0) {
        return;
    }

    for (int bin = 0; bin < bins; ++bin) {
        float sum = 0.0f;
        for (int frame = 0; frame < frames; ++frame) {
            sum += feature[frame * bins + bin];
        }
        const float mean = sum / static_cast<float>(frames);
        for (int frame = 0; frame < frames; ++frame) {
            feature[frame * bins + bin] -= mean;
        }
    }
}

extern "C" {

esp_err_t espdl_feature_build_fbank(const float *pcm,
                                    size_t sample_count,
                                    espdl_feature_frame_t *out_feature)
{
    if (pcm == nullptr || out_feature == nullptr || out_feature->values == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sample_count != static_cast<size_t>(ESPDL_WINDOW_SAMPLES)) {
        ESP_LOGE(TAG, "PCM 长度必须为 %d，实际为 %u",
                 ESPDL_WINDOW_SAMPLES, static_cast<unsigned>(sample_count));
        return ESP_ERR_INVALID_ARG;
    }

    /* 预检 internal RAM：Fbank 构造函数用 MALLOC_CAP_INTERNAL 分配 m_cache(2KB,
     * 16B 对齐)、fft_config、mel_filter、win_func。高压并发时 internal RAM 不足
     * 会导致 heap_caps_aligned_alloc 返回 NULL，而 esp-dl Fbank 构造函数不检查
     * 返回值（同库 Spectrogram 类有 assert 但 Fbank 没有），后续 process_frame
     * 中 memcpy(m_cache=NULL) 触发 StoreProhibited 崩溃。
     * 阈值 6144B 覆盖 m_cache(2KB) + fft_config + mel_filter + win_func + 余量。 */
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (internal_free < 6144U || internal_largest < 4096U) {
        ESP_LOGE(TAG, "internal RAM 不足，跳过 Fbank 提取: free=%u largest=%u",
                 static_cast<unsigned>(internal_free),
                 static_cast<unsigned>(internal_largest));
        return ESP_ERR_NO_MEM;
    }

    dl::audio::Fbank fbank(make_fbank_config());
    const std::vector<int> shape = fbank.get_output_shape(static_cast<int>(sample_count));
    if (shape.size() != 2 || shape[0] != ESPDL_FEATURE_FRAME_COUNT ||
        shape[1] != ESPDL_FEATURE_BIN_COUNT) {
        ESP_LOGE(TAG, "Fbank 形状不匹配: got [%d, %d], expect [%d, %d]",
                 shape.size() > 0 ? shape[0] : -1,
                 shape.size() > 1 ? shape[1] : -1,
                 ESPDL_FEATURE_FRAME_COUNT, ESPDL_FEATURE_BIN_COUNT);
        return ESP_ERR_INVALID_SIZE;
    }

    out_feature->frames = shape[0];
    out_feature->bins = shape[1];

    const esp_err_t ret = fbank.process(pcm, static_cast<int>(sample_count),
                                        out_feature->values);
    if (ret != ESP_OK) {
        return ret;
    }

    normalize_feature_by_time_mean(out_feature->values,
                                   out_feature->frames, out_feature->bins);
    return ESP_OK;
}

esp_err_t espdl_feature_build_fbank_from_int16(const int16_t *pcm_s16,
                                               size_t sample_count,
                                               espdl_feature_frame_t *out_feature)
{
    if (pcm_s16 == nullptr || out_feature == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sample_count != static_cast<size_t>(ESPDL_WINDOW_SAMPLES)) {
        ESP_LOGE(TAG, "PCM 长度必须为 %d，实际为 %u",
                 ESPDL_WINDOW_SAMPLES, static_cast<unsigned>(sample_count));
        return ESP_ERR_INVALID_ARG;
    }

    /* int16 PCM 转 float PCM，归一化到 [-1, 1] */
    std::vector<float> pcm_float(ESPDL_WINDOW_SAMPLES);
    constexpr float kScale = 1.0f / 32768.0f;
    for (size_t i = 0; i < sample_count; ++i) {
        pcm_float[i] = static_cast<float>(pcm_s16[i]) * kScale;
    }

    return espdl_feature_build_fbank(pcm_float.data(), sample_count, out_feature);
}

}  // extern "C"
