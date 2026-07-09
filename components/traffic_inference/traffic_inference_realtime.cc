#include "traffic_inference_realtime.h"

#include <array>
#include <cstdint>
#include <vector>

#include "audio_codec.h"
#include "audio_platform_config.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "model-parameters/model_metadata.h"
#include "traffic_inference_postprocess.h"
#include "traffic_inference_sliding_window.h"

static const char *TAG = "traffic_inference";

namespace {

constexpr size_t kHardwareInputChannels = AUDIO_PLATFORM_HW_INPUT_CHANNELS;
constexpr size_t kHardwareFrameBytes =
    AUDIO_PLATFORM_HW_INPUT_CHANNELS * sizeof(int16_t);
constexpr size_t kTargetSampleRate = EI_CLASSIFIER_FREQUENCY;
constexpr size_t kResampleInputGroup = 3U;
constexpr size_t kResampleOutputGroup = 2U;
constexpr uint32_t kDefaultReadTimeoutMs = 250U;

struct ResampleState {
  std::array<int16_t, kResampleInputGroup> pending_input = {};
  size_t pending_count = 0U;
};

const char *stable_label_to_zh(
    traffic_inference_postprocess_stable_label_t label) {
  switch (label) {
    case TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_HORN:
      return "喇叭";
    case TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_SIREN:
      return "警笛";
    case TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_NONE:
    default:
      return "无";
  }
}

const char *event_to_zh(traffic_inference_postprocess_event_t event) {
  switch (event) {
    case TRAFFIC_INFERENCE_POSTPROCESS_EVENT_START:
      return "开始";
    case TRAFFIC_INFERENCE_POSTPROCESS_EVENT_ACTIVE:
      return "持续";
    case TRAFFIC_INFERENCE_POSTPROCESS_EVENT_END:
      return "结束";
    case TRAFFIC_INFERENCE_POSTPROCESS_EVENT_NONE:
    default:
      return "无";
  }
}

const char *bool_to_zh(bool value) {
  return value ? "是" : "否";
}

int primary_mic_channel_index() {
  for (size_t idx = 0; AUDIO_PLATFORM_ADC_CHANNEL_FORMAT[idx] != '\0'; ++idx) {
    if (AUDIO_PLATFORM_ADC_CHANNEL_FORMAT[idx] == 'M') {
      return static_cast<int>(idx);
    }
  }
  return 0;
}

size_t default_input_chunk_frames() {
  return static_cast<size_t>(AUDIO_PLATFORM_HW_SAMPLE_RATE / 10);
}

TickType_t timeout_to_ticks(uint32_t timeout_ms) {
  return pdMS_TO_TICKS(timeout_ms == 0U ? kDefaultReadTimeoutMs : timeout_ms);
}

size_t chunk_frames_or_default(const traffic_inference_realtime_config_t *config) {
  if (config == nullptr || config->input_chunk_frames == 0U) {
    return default_input_chunk_frames();
  }
  return config->input_chunk_frames;
}

size_t max_iterations_or_default(
    const traffic_inference_realtime_config_t *config) {
  if (config == nullptr) {
    return 0U;
  }
  return config->max_read_iterations;
}

esp_err_t drain_postprocess_results(
    traffic_inference_sliding_window_t *state,
    traffic_inference_postprocess_state_t *postprocess_state,
    size_t *total_results) {
  traffic_inference_sliding_window_result_t result = {};
  traffic_inference_postprocess_output_t postprocess_output = {};

  while (true) {
    esp_err_t ret = traffic_inference_sliding_window_pop_result(state, &result);
    if (ret == TRAFFIC_INFERENCE_SLIDING_WINDOW_ERR_QUEUE_EMPTY) {
      return ESP_OK;
    }
    if (ret != ESP_OK) {
      return ret;
    }

    ret = traffic_inference_postprocess_update(
        postprocess_state, &result, &postprocess_output);
    if (ret != ESP_OK) {
      return ret;
    }

    ESP_LOGI(TAG,
             "\n"
             "  %-8s | [%u,%u)\n"
             "  %-8s | raw=%s  stable=%s  event=%s  alert=%s",
             "win",
             static_cast<unsigned int>(result.window_start_offset),
             static_cast<unsigned int>(result.window_end_offset),
             "state",
             stable_label_to_zh(postprocess_output.raw_label),
             stable_label_to_zh(postprocess_output.stable_label),
             event_to_zh(postprocess_output.event),
             bool_to_zh(postprocess_output.alert_fired));

    ret = traffic_inference_postprocess_dispatch_alert(&postprocess_output);
    if (ret != ESP_OK) {
      return ret;
    }

    if (total_results != nullptr) {
      *total_results += 1U;
    }
  }
}

esp_err_t append_converted_samples(
    traffic_inference_sliding_window_t *state,
    traffic_inference_postprocess_state_t *postprocess_state,
    const std::vector<int16_t> &samples,
    size_t *total_results) {
  size_t total_consumed = 0U;

  while (total_consumed < samples.size()) {
    size_t consumed_now = 0U;
    esp_err_t append_ret = traffic_inference_sliding_window_append_samples(
        state,
        samples.data() + total_consumed,
        samples.size() - total_consumed,
        &consumed_now);
    total_consumed += consumed_now;

    esp_err_t drain_ret =
        drain_postprocess_results(state, postprocess_state, total_results);
    if (drain_ret != ESP_OK) {
      return drain_ret;
    }

    if (append_ret == ESP_OK) {
      continue;
    }

    if (append_ret == TRAFFIC_INFERENCE_SLIDING_WINDOW_ERR_BACKPRESSURE &&
        consumed_now > 0U) {
      continue;
    }

    return append_ret;
  }

  return ESP_OK;
}

esp_err_t extract_primary_mic_channel(const std::vector<int16_t> &interleaved,
                                      std::vector<int16_t> *mono_output) {
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

void resample_24k_to_16k(const std::vector<int16_t> &mono_input,
                         ResampleState *state,
                         std::vector<int16_t> *resampled_output) {
  resampled_output->clear();
  if (state == nullptr || resampled_output == nullptr) {
    return;
  }

  resampled_output->reserve(
      ((mono_input.size() + state->pending_count) * kResampleOutputGroup) /
          kResampleInputGroup +
      kResampleOutputGroup);

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

}  // namespace

/**
 * @brief 运行旧 traffic/Edge Impulse 实时滑窗推理循环。
 *
 * 该调试路径会读取麦克风，因此进入循环前必须通过 audio_codec 申请 input
 * session。这样它和 ESP-DL 危险声音运行时不能同时抢占 I2S RX，退出时也不会
 * 误释放其他模块仍持有的 codec 生命周期引用。
 *
 * @param[in] config 实时推理配置，NULL 表示使用默认读块和无限循环。
 * @param[in] should_stop 外部停止回调，可为 NULL。
 * @param[in] user_data 传给停止回调的用户上下文。
 * @return `ESP_OK` 表示正常结束；其他错误表示 codec、滑窗或后处理失败。
 */
esp_err_t traffic_inference_run_realtime_sliding_window_loop(
    const traffic_inference_realtime_config_t *config,
    traffic_inference_realtime_should_stop_fn should_stop,
    void *user_data) {
  const size_t chunk_frames = chunk_frames_or_default(config);
  const size_t max_iterations = max_iterations_or_default(config);
  const TickType_t read_timeout_ticks =
      timeout_to_ticks(config != nullptr ? config->read_timeout_ms : 0U);
  const size_t raw_buffer_words = chunk_frames * kHardwareInputChannels;
  const size_t raw_buffer_bytes = raw_buffer_words * sizeof(int16_t);

  std::vector<int16_t> raw_interleaved(raw_buffer_words);
  std::vector<int16_t> mono_samples;
  std::vector<int16_t> resampled_samples;
  ResampleState resample_state = {};
  size_t total_results = 0U;
  esp_err_t ret = ESP_OK;
  bool input_acquired = false;
  traffic_inference_postprocess_state_t postprocess_state = {};
  traffic_inference_sliding_window_t *sliding_state =
      static_cast<traffic_inference_sliding_window_t *>(heap_caps_malloc(
          sizeof(traffic_inference_sliding_window_t),
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

  if (sliding_state == nullptr) {
    ESP_LOGE(TAG,
             "realtime demo failed to allocate sliding state in PSRAM bytes=%u",
             static_cast<unsigned int>(sizeof(traffic_inference_sliding_window_t)));
    return ESP_ERR_NO_MEM;
  }

  ret = traffic_inference_sliding_window_init(sliding_state);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "realtime demo sliding init failed: %s", esp_err_to_name(ret));
    heap_caps_free(sliding_state);
    return ret;
  }

  ret = traffic_inference_postprocess_reset(&postprocess_state);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "realtime demo postprocess init failed: %s",
             esp_err_to_name(ret));
    heap_caps_free(sliding_state);
    return ret;
  }

  ret = audio_codec_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "realtime demo codec init failed: %s", esp_err_to_name(ret));
    heap_caps_free(sliding_state);
    return ret;
  }

  ret = audio_codec_acquire_input(AUDIO_CODEC_OWNER_TRAFFIC_INFERENCE, 0U);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "realtime demo codec input session failed: %s",
             esp_err_to_name(ret));
    (void)audio_codec_deinit();
    heap_caps_free(sliding_state);
    return ret;
  }
  input_acquired = true;

  ESP_LOGI(TAG,
           "starting realtime sliding-window traffic inference demo "
           "hardware=%dHz/%dch target=%uHz/%u samples chunk_frames=%u timeout_ms=%u",
           AUDIO_PLATFORM_HW_SAMPLE_RATE,
           AUDIO_PLATFORM_HW_INPUT_CHANNELS,
           static_cast<unsigned int>(kTargetSampleRate),
           static_cast<unsigned int>(EI_CLASSIFIER_RAW_SAMPLE_COUNT),
           static_cast<unsigned int>(chunk_frames),
           static_cast<unsigned int>(
               config != nullptr && config->read_timeout_ms != 0U
                   ? config->read_timeout_ms
                   : kDefaultReadTimeoutMs));

  for (size_t iteration = 0U;
       max_iterations == 0U || iteration < max_iterations;
       ++iteration) {
    if (should_stop != nullptr && should_stop(user_data)) {
      ret = ESP_OK;
      break;
    }

    size_t bytes_read = 0U;
    ret = audio_codec_read(
        raw_interleaved.data(), raw_buffer_bytes, &bytes_read, read_timeout_ticks);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "realtime demo codec read failed: %s", esp_err_to_name(ret));
      break;
    }
    if (bytes_read == 0U) {
      ESP_LOGE(TAG, "realtime demo codec read made no progress");
      ret = ESP_ERR_TIMEOUT;
      break;
    }
    if ((bytes_read % kHardwareFrameBytes) != 0U) {
      ESP_LOGE(TAG,
               "realtime demo codec read returned partial frame bytes=%u frame_bytes=%u",
               static_cast<unsigned int>(bytes_read),
               static_cast<unsigned int>(kHardwareFrameBytes));
      ret = ESP_ERR_INVALID_SIZE;
      break;
    }

    raw_interleaved.resize(bytes_read / sizeof(int16_t));
    ret = extract_primary_mic_channel(raw_interleaved, &mono_samples);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "realtime demo primary mic extract failed: %s",
               esp_err_to_name(ret));
      break;
    }

    resample_24k_to_16k(mono_samples, &resample_state, &resampled_samples);
    if (!resampled_samples.empty()) {
      ret = append_converted_samples(
          sliding_state, &postprocess_state, resampled_samples, &total_results);
      if (ret != ESP_OK) {
        ESP_LOGE(TAG, "realtime demo append failed: %s", esp_err_to_name(ret));
        break;
      }
    }

    raw_interleaved.resize(raw_buffer_words);
  }

  if (ret == ESP_OK) {
    ret = drain_postprocess_results(
        sliding_state, &postprocess_state, &total_results);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "realtime demo final drain failed: %s", esp_err_to_name(ret));
    }
  }

  if (input_acquired) {
    const esp_err_t release_ret =
        audio_codec_release_input(AUDIO_CODEC_OWNER_TRAFFIC_INFERENCE);
    if (ret == ESP_OK && release_ret != ESP_OK) {
      ret = release_ret;
    }
  }

  const esp_err_t deinit_ret = audio_codec_deinit();
  if (ret == ESP_OK && deinit_ret != ESP_OK) {
    ret = deinit_ret;
  }

  ESP_LOGI(TAG,
           "realtime demo exiting status=%s emitted_windows=%u",
           esp_err_to_name(ret),
           static_cast<unsigned int>(total_results));
  heap_caps_free(sliding_state);
  return ret;
}

esp_err_t traffic_inference_run_realtime_sliding_window_demo(
    const traffic_inference_realtime_config_t *config) {
  return traffic_inference_run_realtime_sliding_window_loop(config, nullptr,
                                                            nullptr);
}
