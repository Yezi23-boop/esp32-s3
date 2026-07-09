#include "traffic_inference_sliding_window.h"

#include <cstring>

#include "esp_log.h"
#include "traffic_inference_runner_internal.h"

namespace {

constexpr size_t kWindowSamples =
    TRAFFIC_INFERENCE_SLIDING_WINDOW_WINDOW_SAMPLES;
constexpr size_t kStrideSamples =
    TRAFFIC_INFERENCE_SLIDING_WINDOW_STRIDE_SAMPLES;
constexpr size_t kBufferCapacity = kWindowSamples + kStrideSamples;
constexpr size_t kQueueDepth =
    TRAFFIC_INFERENCE_SLIDING_WINDOW_RESULT_QUEUE_DEPTH;
const char *kTag = "traffic_window";

void zero_state(traffic_inference_sliding_window_t *state) {
  std::memset(state, 0, sizeof(*state));
}

size_t queue_next_index(size_t index) {
  return (index + 1U) % kQueueDepth;
}

void copy_scores(const ei_impulse_result_t &result,
                 traffic_inference_sliding_window_result_t *out_result) {
  for (size_t idx = 0; idx < EI_CLASSIFIER_LABEL_COUNT; ++idx) {
    out_result->classification_scores[idx] = result.classification[idx].value;
  }
}

esp_err_t enqueue_result(traffic_inference_sliding_window_t *state,
                         const ei_impulse_result_t &result) {
  if (state->queued_result_count >= kQueueDepth) {
    return TRAFFIC_INFERENCE_SLIDING_WINDOW_ERR_BACKPRESSURE;
  }

  traffic_inference_sliding_window_result_t *slot =
      &state->result_queue[state->queue_write_index];
  std::memset(slot, 0, sizeof(*slot));

  const traffic_inference_confidence_summary_t confidence =
      traffic_inference_describe_confidence(result);
  slot->window_start_offset = state->buffer_start_offset;
  slot->window_end_offset = state->buffer_start_offset + kWindowSamples;
  slot->top_label = confidence.selected_label;
  slot->top_score = confidence.selected_score;
  slot->dsp_time_ms = result.timing.dsp;
  slot->classification_time_ms = result.timing.classification;
  slot->classification_time_us = result.timing.classification_us;
  copy_scores(result, slot);

  state->queue_write_index = queue_next_index(state->queue_write_index);
  state->queued_result_count += 1U;

  ESP_LOGI(kTag,
           "\n"
           "  %-8s | [%u,%u)\n"
           "  %-8s | %-10s sel=%.5f   raw=%s/%.5f\n"
           "  %-8s | %s\n"
           "  %-8s | horn=%.5f (%s @ %.2f)  siren=%.5f (%s @ %.2f)  bg=%.5f\n"
           "  %-8s | dsp=%d ms  cls=%d ms",
           "win",
           static_cast<unsigned int>(slot->window_start_offset),
           static_cast<unsigned int>(slot->window_end_offset),
           "pred",
           slot->top_label != nullptr ? slot->top_label : "<unknown>",
           static_cast<double>(slot->top_score),
           confidence.top_raw_label != nullptr ? confidence.top_raw_label : "<unknown>",
           static_cast<double>(confidence.top_raw_score),
           "decision",
           confidence.decision_reason != nullptr ? confidence.decision_reason : "",
           "scores",
           static_cast<double>(confidence.horn_score),
           confidence.horn_passed_threshold ? "yes" : "no",
           static_cast<double>(kTrafficInferenceHornThreshold),
           static_cast<double>(confidence.siren_score),
           confidence.siren_passed_threshold ? "yes" : "no",
           static_cast<double>(kTrafficInferenceSirenThreshold),
           static_cast<double>(confidence.background_score),
           "timing",
           slot->dsp_time_ms,
           slot->classification_time_ms);

  return ESP_OK;
}

void compact_after_stride(traffic_inference_sliding_window_t *state) {
  const size_t remaining_samples = state->buffered_sample_count - kStrideSamples;
  if (remaining_samples > 0U) {
    std::memmove(state->pcm_buffer,
                 state->pcm_buffer + kStrideSamples,
                 remaining_samples * sizeof(state->pcm_buffer[0]));
  }
  state->buffered_sample_count = remaining_samples;
  state->buffer_start_offset += kStrideSamples;
}

esp_err_t process_ready_windows(traffic_inference_sliding_window_t *state) {
  while (state->buffered_sample_count >= kWindowSamples) {
    if (state->queued_result_count >= kQueueDepth) {
      return TRAFFIC_INFERENCE_SLIDING_WINDOW_ERR_BACKPRESSURE;
    }

    ei_impulse_result_t result = {};
    esp_err_t infer_ret = traffic_inference_run_raw_samples(
        state->pcm_buffer, kWindowSamples, &result);
    if (infer_ret != ESP_OK) {
      return infer_ret;
    }

    esp_err_t enqueue_ret = enqueue_result(state, result);
    if (enqueue_ret != ESP_OK) {
      return enqueue_ret;
    }

    compact_after_stride(state);
  }

  return ESP_OK;
}

}  // namespace

esp_err_t traffic_inference_sliding_window_init(
    traffic_inference_sliding_window_t *state) {
  return traffic_inference_sliding_window_reset(state);
}

esp_err_t traffic_inference_sliding_window_reset(
    traffic_inference_sliding_window_t *state) {
  if (state == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  zero_state(state);
  return ESP_OK;
}

esp_err_t traffic_inference_sliding_window_append_samples(
    traffic_inference_sliding_window_t *state,
    const int16_t *samples,
    size_t sample_count,
    size_t *consumed_samples) {
  if (state == nullptr || consumed_samples == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  *consumed_samples = 0U;

  if (sample_count == 0U) {
    return ESP_OK;
  }

  if (samples == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  while (*consumed_samples < sample_count) {
    if (state->queued_result_count >= kQueueDepth) {
      return TRAFFIC_INFERENCE_SLIDING_WINDOW_ERR_BACKPRESSURE;
    }

    const size_t free_capacity = kBufferCapacity - state->buffered_sample_count;
    if (free_capacity == 0U) {
      return TRAFFIC_INFERENCE_SLIDING_WINDOW_ERR_BACKPRESSURE;
    }

    const size_t remaining_input = sample_count - *consumed_samples;
    const size_t to_copy =
        remaining_input < free_capacity ? remaining_input : free_capacity;
    std::memcpy(state->pcm_buffer + state->buffered_sample_count,
                samples + *consumed_samples,
                to_copy * sizeof(samples[0]));
    state->buffered_sample_count += to_copy;
    *consumed_samples += to_copy;

    esp_err_t process_ret = process_ready_windows(state);
    if (process_ret == TRAFFIC_INFERENCE_SLIDING_WINDOW_ERR_BACKPRESSURE) {
      if (*consumed_samples == sample_count) {
        return ESP_OK;
      }
      return process_ret;
    }
    if (process_ret != ESP_OK) {
      return process_ret;
    }
  }

  return ESP_OK;
}

esp_err_t traffic_inference_sliding_window_pop_result(
    traffic_inference_sliding_window_t *state,
    traffic_inference_sliding_window_result_t *out_result) {
  if (state == nullptr || out_result == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  if (state->queued_result_count == 0U) {
    return TRAFFIC_INFERENCE_SLIDING_WINDOW_ERR_QUEUE_EMPTY;
  }

  *out_result = state->result_queue[state->queue_read_index];
  state->queue_read_index = queue_next_index(state->queue_read_index);
  state->queued_result_count -= 1U;

  return ESP_OK;
}
