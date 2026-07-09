/**
 * @file espdl_audio_runtime.cpp
 * @brief ESP-DL 单模型实时音频推理运行时实现。
 *
 * 在 FreeRTOS 后台任务中持续读取麦克风音频，重采样到 16kHz，
 * 提取 Fbank 特征后运行当前 active 的 V59 softdistill T90 单模型。
 *
 * 音频管线与 traffic_inference_realtime.cc 保持一致：
 *   ES7210 ADC (24kHz, 2ch TDM, "MR" 格式)
 *   → 提取主麦克风通道 (M 通道)
 *   → 3:2 重采样到 16kHz
 *   → 滑窗缓冲 1 秒
 *   → ESP-DL Fbank 特征提取
 *   → 单模型推理
 *   → 回调上报结果
 *
 * @note 该运行时与 traffic_audio_runtime 互斥，同一时刻只能运行一个。
 *       因为两者都独占音频编解码器 (audio_codec) 的读取通道。
 */

#include "espdl_audio_runtime.h"

#include <array>
#include <atomic>
#include <cstring>
#include <vector>

#include "audio_codec.h"
#include "audio_platform_config.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "espdl_feature_pipeline.h"
#include "espdl_model_runner.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Active 单模型 .espdl rodata 声明。 */
extern const uint8_t dscnn_v59_t90_espdl[] asm("_binary_edge_mix_teacher_dscnn_medium_v59_v54_anchor_softdistill_t90_20260608_espdl_start");

static const char *TAG = "espdl_runtime";

namespace {

/** 硬件输入通道数（TDM: MR = 麦克风 + 参考）。 */
constexpr size_t kHardwareInputChannels = AUDIO_PLATFORM_HW_INPUT_CHANNELS;
/** 每帧硬件字节数。 */
constexpr size_t kHardwareFrameBytes = kHardwareInputChannels * sizeof(int16_t);
/** 重采样参数：24kHz → 16kHz，3:2 比率。 */
constexpr size_t kResampleInputGroup = 3U;
constexpr size_t kResampleOutputGroup = 2U;
/** 默认读取超时。 */
constexpr uint32_t kDefaultReadTimeoutMs = 250U;
/** 默认任务栈大小。 */
constexpr uint32_t kDefaultTaskStackSize = 16384U;  /* ESP-DL 需要更大栈 */
/** 默认任务优先级。 */
constexpr UBaseType_t kDefaultTaskPriority = 5U;
/** 停止轮询间隔。 */
constexpr TickType_t kStopPollIntervalTicks = pdMS_TO_TICKS(10);
/** 滑窗步长（毫秒）。 */
constexpr uint32_t kStrideMs = 300U;
/** 滑窗步长（样本数）。 */
constexpr size_t kStrideSamples = (ESPDL_SAMPLE_RATE_HZ * kStrideMs) / 1000U;
/** non-danger 心跳日志间隔；danger 窗口仍逐窗打印，避免漏看确认过程。 */
constexpr int64_t kNonDangerLogIntervalUs = 3000LL * 1000LL;

/** 重采样状态。 */
struct ResampleState {
    std::array<int16_t, kResampleInputGroup> pending_input = {};
    size_t pending_count = 0U;
};

/** 运行时控制结构。 */
struct RuntimeControl {
    std::atomic<int> state = {ESPDL_AUDIO_RUNTIME_STATE_IDLE};
    std::atomic<bool> stop_requested = {false};
    TaskHandle_t task_handle = nullptr;
    espdl_audio_runtime_config_t config = {};
    espdl_model_runner_t *model_runner = nullptr;
    espdl_audio_runtime_result_callback_t result_callback = nullptr;
    void *callback_user_data = nullptr;
    espdl_audio_pcm_tap_callback_t pcm_tap_callback = nullptr;
    void *pcm_tap_user_data = nullptr;
    esp_err_t last_result = ESP_OK;
    std::atomic<float> danger_threshold = {ESPDL_DSCNN_DANGER_THRESHOLD};
    uint64_t absolute_sample_index = 0U;  /**< 已重采样输出的绝对样本计数。 */
};

RuntimeControl s_runtime = {};

/**
 * @brief 清理 stop 成功后才能释放的运行时资源。
 *
 * `espdl_audio_runtime_stop()` 可能先超时返回，此时后台任务仍负责释放 input
 * session 并把 task_handle 置空。下一次 stop 或 start 前需要补做 codec
 * deinit 和模型销毁，避免 service 层误以为资源已经完整释放。
 */
esp_err_t cleanup_stopped_runtime_resources()
{
    if (s_runtime.model_runner == nullptr) {
        return ESP_OK;
    }

    esp_err_t ret = audio_codec_deinit();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ESP-DL runtime cleanup deferred: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    espdl_model_runner_destroy(s_runtime.model_runner);
    s_runtime.model_runner = nullptr;
    return ESP_OK;
}

/**
 * @brief 返回主麦克风通道在 TDM 帧中的索引。
 */
int primary_mic_channel_index()
{
    for (size_t idx = 0; AUDIO_PLATFORM_ADC_CHANNEL_FORMAT[idx] != '\0'; ++idx) {
        if (AUDIO_PLATFORM_ADC_CHANNEL_FORMAT[idx] == 'M') {
            return static_cast<int>(idx);
        }
    }
    return 0;
}

/**
 * @brief 默认每次读取的硬件帧数。
 */
size_t default_input_chunk_frames()
{
    return static_cast<size_t>(AUDIO_PLATFORM_HW_SAMPLE_RATE / 10);
}

/**
 * @brief 从交织 TDM 数据提取主麦克风单声道数据。
 */
esp_err_t extract_primary_mic_channel(const std::vector<int16_t> &interleaved,
                                      std::vector<int16_t> *mono_output)
{
    if (mono_output == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    mono_output->clear();
    if (interleaved.empty()) {
        return ESP_OK;
    }
    if ((interleaved.size() % kHardwareInputChannels) != 0U) {
        return ESP_ERR_INVALID_SIZE;
    }

    const int primary_channel = primary_mic_channel_index();
    const size_t frame_count = interleaved.size() / kHardwareInputChannels;
    mono_output->reserve(frame_count);
    for (size_t frame = 0; frame < frame_count; ++frame) {
        const size_t sample_index =
            frame * kHardwareInputChannels + static_cast<size_t>(primary_channel);
        mono_output->push_back(interleaved[sample_index]);
    }
    return ESP_OK;
}

/**
 * @brief 24kHz → 16kHz 重采样（3:2 比率）。
 *
 * 每 3 个输入样本生成 2 个输出样本：
 *   out[0] = in[0]
 *   out[1] = (in[1] + in[2]) / 2
 */
void resample_24k_to_16k(const std::vector<int16_t> &mono_input,
                         ResampleState *state,
                         std::vector<int16_t> *resampled_output)
{
    resampled_output->clear();
    if (state == nullptr || resampled_output == nullptr) {
        return;
    }

    resampled_output->reserve(
        ((mono_input.size() + state->pending_count) * kResampleOutputGroup) /
            kResampleInputGroup + kResampleOutputGroup);

    for (int16_t sample : mono_input) {
        state->pending_input[state->pending_count++] = sample;
        if (state->pending_count < kResampleInputGroup) {
            continue;
        }

        const int32_t middle = static_cast<int32_t>(state->pending_input[1]);
        const int32_t tail = static_cast<int32_t>(state->pending_input[2]);
        resampled_output->push_back(state->pending_input[0]);
        resampled_output->push_back(static_cast<int16_t>((middle + tail) / 2));
        state->pending_count = 0U;
    }
}

/**
 * @brief 运行时主循环。
 *
 * 持续读取麦克风音频，维护 1 秒滑窗缓冲，当缓冲满时提取 Fbank
 * 特征并运行 active 单模型推理。
 *
 * @param[in] arg FreeRTOS 任务参数，当前未使用。
 */
void runtime_task(void *arg)
{
    (void)arg;

    s_runtime.state.store(ESPDL_AUDIO_RUNTIME_STATE_RUNNING);
    s_runtime.absolute_sample_index = 0U;

    const size_t chunk_frames = s_runtime.config.input_chunk_frames != 0
                                    ? s_runtime.config.input_chunk_frames
                                    : default_input_chunk_frames();
    const TickType_t read_timeout_ticks = pdMS_TO_TICKS(
        s_runtime.config.read_timeout_ms != 0
            ? s_runtime.config.read_timeout_ms
            : kDefaultReadTimeoutMs);
    const size_t raw_buffer_words = chunk_frames * kHardwareInputChannels;
    const size_t raw_buffer_bytes = raw_buffer_words * sizeof(int16_t);

    std::vector<int16_t> raw_interleaved(raw_buffer_words);
    std::vector<int16_t> mono_samples;
    std::vector<int16_t> resampled_samples;
    ResampleState resample_state = {};

    mono_samples.reserve(chunk_frames);
    resampled_samples.reserve(
        (chunk_frames * kResampleOutputGroup) / kResampleInputGroup +
        kResampleOutputGroup);

    /* 滑窗缓冲：1 秒 + 步长余量 */
    std::vector<int16_t> pcm_buffer;
    pcm_buffer.reserve(ESPDL_WINDOW_SAMPLES + kStrideSamples);

    /* 推理窗 PCM float 缓冲在任务启动时分配，避免每 300ms 窗口反复申请堆内存。 */
    std::vector<float> pcm_float(ESPDL_WINDOW_SAMPLES);

    /* Fbank 特征缓冲（静态分配到 PSRAM） */
    std::vector<float> fbank_values(
        ESPDL_FEATURE_FRAME_COUNT * ESPDL_FEATURE_BIN_COUNT);

    esp_err_t ret = ESP_OK;
    size_t total_inferences = 0U;
    int64_t last_non_danger_log_us = -kNonDangerLogIntervalUs;

    ESP_LOGI(TAG,
             "启动 ESPDL 实时推理: hw=%dHz/%dch, target=%dHz, "
             "chunk=%u, stride=%ums, model=dscnn_v59_t90",
             AUDIO_PLATFORM_HW_SAMPLE_RATE,
             AUDIO_PLATFORM_HW_INPUT_CHANNELS,
             ESPDL_SAMPLE_RATE_HZ,
             static_cast<unsigned>(chunk_frames),
             static_cast<unsigned>(kStrideMs));

    while (!s_runtime.stop_requested.load()) {
        /* 读取麦克风音频 */
        size_t bytes_read = 0U;
        ret = audio_codec_read(raw_interleaved.data(), raw_buffer_bytes,
                               &bytes_read, read_timeout_ticks);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "codec read 失败: %s", esp_err_to_name(ret));
            break;
        }
        if (bytes_read == 0U) {
            continue;
        }
        if ((bytes_read % kHardwareFrameBytes) != 0U) {
            ESP_LOGE(TAG, "codec read 返回不完整帧: bytes=%u",
                     static_cast<unsigned>(bytes_read));
            ret = ESP_ERR_INVALID_SIZE;
            break;
        }

        raw_interleaved.resize(bytes_read / sizeof(int16_t));

        /* 提取主麦克风通道 */
        ret = extract_primary_mic_channel(raw_interleaved, &mono_samples);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "麦克风通道提取失败: %s", esp_err_to_name(ret));
            break;
        }

        /* 24kHz → 16kHz 重采样 */
        resample_24k_to_16k(mono_samples, &resample_state, &resampled_samples);

        /* 发布连续 PCM chunk，再推进 absolute_sample_index。 */
        if (!resampled_samples.empty()) {
            const uint64_t chunk_first_sample_index =
                s_runtime.absolute_sample_index;
            if (s_runtime.pcm_tap_callback != nullptr) {
                const espdl_audio_pcm_window_meta_t meta = {
                    .absolute_sample_index = chunk_first_sample_index,
                    .window_samples =
                        static_cast<uint32_t>(resampled_samples.size()),
                    .stride_samples = 0U,
                };
                s_runtime.pcm_tap_callback(resampled_samples.data(),
                                           resampled_samples.size(),
                                           &meta,
                                           s_runtime.pcm_tap_user_data);
            }

            /* 追加到滑窗缓冲 */
            for (int16_t sample : resampled_samples) {
                pcm_buffer.push_back(sample);
            }
            s_runtime.absolute_sample_index += resampled_samples.size();
        }

        /* 当缓冲满一窗时执行推理 */
        while (pcm_buffer.size() >= ESPDL_WINDOW_SAMPLES) {
            if (s_runtime.stop_requested.load()) {
                break;
            }

            /* 转换为 float PCM */
            constexpr float kScale = 1.0f / 32768.0f;
            for (size_t i = 0; i < ESPDL_WINDOW_SAMPLES; ++i) {
                pcm_float[i] = static_cast<float>(pcm_buffer[i]) * kScale;
            }

            /* 提取 Fbank 特征 */
            const int64_t feature_start = esp_timer_get_time();
            espdl_feature_frame_t feature_out = {
                .values = fbank_values.data(),
                .frames = ESPDL_FEATURE_FRAME_COUNT,
                .bins = ESPDL_FEATURE_BIN_COUNT,
            };
            ret = espdl_feature_build_fbank(pcm_float.data(),
                                            ESPDL_WINDOW_SAMPLES,
                                            &feature_out);
            const int64_t feature_us = esp_timer_get_time() - feature_start;
            if (ret != ESP_OK) {
                if (ret == ESP_ERR_NO_MEM) {
                    /* internal RAM 瞬时不足（AI 对话+SSL 并发），跳过本窗不退出。
                     * 避免高压下 Fbank 构造分配 NULL 导致 runtime 永久退出。 */
                    ESP_LOGW(TAG, "Fbank 提取跳过(内存不足): %s", esp_err_to_name(ret));
                    /* 仍需步进滑窗，否则下一轮还是同一窗口 */
                    const size_t skip_remaining = pcm_buffer.size() - kStrideSamples;
                    if (skip_remaining > 0) {
                        std::memmove(pcm_buffer.data(),
                                     pcm_buffer.data() + kStrideSamples,
                                     skip_remaining * sizeof(int16_t));
                    }
                    pcm_buffer.resize(skip_remaining);
                    continue;
                }
                ESP_LOGE(TAG, "Fbank 提取失败: %s", esp_err_to_name(ret));
                break;
            }

            /* 单模型推理。 */
            const int64_t infer_start = esp_timer_get_time();
            const uint64_t window_start_sample_index =
                s_runtime.absolute_sample_index - pcm_buffer.size();
            const uint64_t window_end_sample_index =
                window_start_sample_index + ESPDL_WINDOW_SAMPLES;
            espdl_model_result_t result = {};
            ret = espdl_model_runner_run(s_runtime.model_runner, &feature_out,
                                         &result);
            const int64_t infer_us = esp_timer_get_time() - infer_start;
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "ESP-DL 单模型推理失败: %s", esp_err_to_name(ret));
                /* 不 break，继续下一个窗口 */
            } else {
                result.window_end_sample_index = window_end_sample_index;
                total_inferences++;
                const bool is_danger = result.label_index == 1;
                const int64_t now_us = esp_timer_get_time();
                const bool should_log =
                    is_danger ||
                    (now_us - last_non_danger_log_us) >= kNonDangerLogIntervalUs;
                if (should_log) {
                    ESP_LOGI(TAG,
                             "INFERENCE #%u: label=%s, confidence=%.4f, "
                             "danger=%.4f, fbank_ms=%.1f, infer_ms=%.1f",
                             static_cast<unsigned>(total_inferences),
                             espdl_model_runner_label_name(result.label_index),
                             result.confidence,
                             result.probabilities[1],
                             static_cast<double>(feature_us) / 1000.0,
                             static_cast<double>(infer_us) / 1000.0);
                    if (!is_danger) {
                        last_non_danger_log_us = now_us;
                    }
                }

                /* 通知回调 */
                if (s_runtime.result_callback != nullptr) {
                    s_runtime.result_callback(&result,
                                              s_runtime.callback_user_data);
                }
            }

            /* 滑窗步进：移除 stride 个样本 */
            const size_t remaining = pcm_buffer.size() - kStrideSamples;
            if (remaining > 0) {
                std::memmove(pcm_buffer.data(),
                             pcm_buffer.data() + kStrideSamples,
                             remaining * sizeof(int16_t));
            }
            pcm_buffer.resize(remaining);
        }

        raw_interleaved.resize(raw_buffer_words);
    }

    s_runtime.last_result = ret;
    if (ret == ESP_OK || s_runtime.stop_requested.load()) {
        s_runtime.state.store(ESPDL_AUDIO_RUNTIME_STATE_IDLE);
    } else {
        ESP_LOGE(TAG, "运行时循环失败: %s", esp_err_to_name(ret));
        s_runtime.state.store(ESPDL_AUDIO_RUNTIME_STATE_FAILED);
    }

    // 输入会话由后台任务释放，保证任务退出前不会被 stop 线程提前释放 RX 通道。
    (void)audio_codec_release_input(AUDIO_CODEC_OWNER_ESPDL_INFERENCE);

    s_runtime.stop_requested.store(false);
    s_runtime.task_handle = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

extern "C" {

/**
 * @brief 启动 ESP-DL 实时音频推理运行时。
 *
 * 该接口持有 audio_codec 生命周期引用，并申请独占 input session。session 的释放
 * 放在后台任务退出点，避免 stop 线程和读麦克风任务之间出现资源归属竞态。
 *
 * @param[in] config 运行时配置，传 NULL 时使用默认窗口和任务栈。
 * @return `ESP_OK` 表示启动成功；其他错误表示模型、codec 或任务创建失败。
 */
esp_err_t espdl_audio_runtime_start(const espdl_audio_runtime_config_t *config)
{
    if (s_runtime.task_handle != nullptr ||
        s_runtime.state.load() != ESPDL_AUDIO_RUNTIME_STATE_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = cleanup_stopped_runtime_resources();
    if (ret != ESP_OK) {
        return ret;
    }

    /* 初始化 active 单模型。当前只加载 V59 T90，避免多模型常驻导致 RAM 峰值过高。 */
    ret = espdl_model_runner_create(&s_runtime.model_runner,
                                    dscnn_v59_t90_espdl,
                                    "dscnn_v59_t90");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ESP-DL active 模型初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = espdl_model_runner_set_threshold(
        s_runtime.model_runner, s_runtime.danger_threshold.load());
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ESP-DL danger 阈值设置失败: %s", esp_err_to_name(ret));
        espdl_model_runner_destroy(s_runtime.model_runner);
        s_runtime.model_runner = nullptr;
        return ret;
    }

    /* 自检 */
    ret = espdl_model_runner_self_test(s_runtime.model_runner);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ESP-DL active 模型自检失败: %s", esp_err_to_name(ret));
        espdl_model_runner_destroy(s_runtime.model_runner);
        s_runtime.model_runner = nullptr;
        return ret;
    }

    /* 初始化音频编解码器 */
    ret = audio_codec_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "音频编解码器初始化失败: %s", esp_err_to_name(ret));
        espdl_model_runner_destroy(s_runtime.model_runner);
        s_runtime.model_runner = nullptr;
        return ret;
    }

    ret = audio_codec_acquire_input(AUDIO_CODEC_OWNER_ESPDL_INFERENCE, 0U);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ESP-DL 录音输入会话申请失败: %s", esp_err_to_name(ret));
        (void)audio_codec_deinit();
        espdl_model_runner_destroy(s_runtime.model_runner);
        s_runtime.model_runner = nullptr;
        return ret;
    }

    /* 保存配置 */
    s_runtime.config.input_chunk_frames =
        config != nullptr ? config->input_chunk_frames : 0U;
    s_runtime.config.read_timeout_ms =
        config != nullptr ? config->read_timeout_ms : 0U;
    s_runtime.config.task_stack_size =
        (config != nullptr && config->task_stack_size != 0U)
            ? config->task_stack_size
            : kDefaultTaskStackSize;
    s_runtime.config.task_priority =
        (config != nullptr && config->task_priority != 0U)
            ? config->task_priority
            : kDefaultTaskPriority;
    s_runtime.last_result = ESP_OK;
    s_runtime.stop_requested.store(false);
    s_runtime.state.store(ESPDL_AUDIO_RUNTIME_STATE_STARTING);

    /* 创建后台任务 */
    BaseType_t created = xTaskCreate(runtime_task,
                                     "espdl_audio_rt",
                                     s_runtime.config.task_stack_size,
                                     nullptr,
                                     s_runtime.config.task_priority,
                                     &s_runtime.task_handle);
    if (created != pdPASS) {
        s_runtime.task_handle = nullptr;
        s_runtime.state.store(ESPDL_AUDIO_RUNTIME_STATE_FAILED);
        (void)audio_codec_release_input(AUDIO_CODEC_OWNER_ESPDL_INFERENCE);
        (void)audio_codec_deinit();
        espdl_model_runner_destroy(s_runtime.model_runner);
        s_runtime.model_runner = nullptr;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ESPDL 实时运行时已启动");
    return ESP_OK;
}

/**
 * @brief 停止 ESP-DL 实时音频推理运行时。
 *
 * @param[in] timeout_ms 等待后台任务退出的超时，0 表示使用默认 2 秒。
 * @return `ESP_OK` 表示停止成功；`ESP_ERR_TIMEOUT` 表示任务未在期限内退出。
 */
esp_err_t espdl_audio_runtime_stop(uint32_t timeout_ms)
{
    if (s_runtime.task_handle == nullptr) {
        const int state = s_runtime.state.load();
        if (state == ESPDL_AUDIO_RUNTIME_STATE_IDLE ||
            state == ESPDL_AUDIO_RUNTIME_STATE_FAILED) {
            return cleanup_stopped_runtime_resources();
        }
        return ESP_ERR_INVALID_STATE;
    }

    s_runtime.stop_requested.store(true);
    s_runtime.state.store(ESPDL_AUDIO_RUNTIME_STATE_STOPPING);

    const TickType_t deadline =
        xTaskGetTickCount() +
        pdMS_TO_TICKS(timeout_ms == 0U ? 2000U : timeout_ms);
    while (s_runtime.task_handle != nullptr) {
        if (xTaskGetTickCount() >= deadline) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(kStopPollIntervalTicks);
    }

    /* 清理资源 */
    esp_err_t ret = cleanup_stopped_runtime_resources();
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "ESPDL 实时运行时已停止");
    return ESP_OK;
}

bool espdl_audio_runtime_is_running(void)
{
    const int state = s_runtime.state.load();
    return state == ESPDL_AUDIO_RUNTIME_STATE_STARTING ||
           state == ESPDL_AUDIO_RUNTIME_STATE_RUNNING ||
           state == ESPDL_AUDIO_RUNTIME_STATE_STOPPING;
}

espdl_audio_runtime_state_t espdl_audio_runtime_get_state(void)
{
    return static_cast<espdl_audio_runtime_state_t>(s_runtime.state.load());
}

esp_err_t espdl_audio_runtime_set_danger_threshold(float threshold)
{
    if (threshold < 0.0f || threshold > 1.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    s_runtime.danger_threshold.store(threshold);
    if (s_runtime.model_runner == nullptr) {
        return ESP_OK;
    }
    return espdl_model_runner_set_threshold(s_runtime.model_runner, threshold);
}

esp_err_t espdl_audio_runtime_set_result_callback(
    espdl_audio_runtime_result_callback_t callback,
    void *user_data)
{
    s_runtime.result_callback = callback;
    s_runtime.callback_user_data = user_data;
    return ESP_OK;
}

esp_err_t espdl_audio_runtime_set_pcm_tap_callback(
    espdl_audio_pcm_tap_callback_t callback,
    void *user_data)
{
    s_runtime.pcm_tap_callback = callback;
    s_runtime.pcm_tap_user_data = user_data;
    return ESP_OK;
}

}  // extern "C"
