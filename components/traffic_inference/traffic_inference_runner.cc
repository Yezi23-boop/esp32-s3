#include "traffic_inference.h"
#include "traffic_inference_runner_internal.h"

#include <cstring>

#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "edge-impulse-sdk/dsp/numpy.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "model-parameters/model_metadata.h"

static const char *TAG = "traffic_inference";

namespace {

constexpr const char *kHornLabel = "horn";
constexpr const char *kSirenLabel = "siren";
constexpr const char *kBackgroundLabel = "background";
constexpr const char *kDecisionThresholdPass = "threshold_pass";
constexpr const char *kDecisionThresholdFallback = "threshold_fallback";

struct sample_signal_context_t {
  const int16_t *samples;
  size_t sample_count;
};

int fill_signal_data(const sample_signal_context_t &context,
                     size_t offset,
                     size_t length,
                     float *out_ptr) {
  if (out_ptr == nullptr || context.samples == nullptr) {
    return EIDSP_PARAMETER_INVALID;
  }

  if ((offset + length) > context.sample_count) {
    return EIDSP_OUT_OF_BOUNDS;
  }

  return ei::numpy::int16_to_float(context.samples + offset, out_ptr, length);
}

const ei_impulse_result_classification_t *find_top_classification(
    const ei_impulse_result_t &result) {
  const ei_impulse_result_classification_t *top = nullptr;

  for (size_t idx = 0; idx < EI_CLASSIFIER_LABEL_COUNT; ++idx) {
    const ei_impulse_result_classification_t *candidate = &result.classification[idx];
    if (top == nullptr || candidate->value > top->value) {
      top = candidate;
    }
  }

  return top;
}

const ei_impulse_result_classification_t *find_classification_by_label(
    const ei_impulse_result_t &result, const char *label) {
  if (label == nullptr) {
    return nullptr;
  }

  for (size_t idx = 0; idx < EI_CLASSIFIER_LABEL_COUNT; ++idx) {
    const char *candidate_label = result.classification[idx].label;
    if (candidate_label != nullptr && std::strcmp(candidate_label, label) == 0) {
      return &result.classification[idx];
    }
  }

  return nullptr;
}

void log_inference_summary(const traffic_inference_demo_sample_t *sample,
                           const ei_impulse_result_t &result) {
  const traffic_inference_confidence_summary_t confidence =
      traffic_inference_describe_confidence(result);
  const char *top_label =
      confidence.selected_label != nullptr ? confidence.selected_label : "<unknown>";
  const float top_score = confidence.selected_score;
  const char *expected_label = sample != nullptr ? sample->expected_label : nullptr;
  const char *source_file = sample != nullptr ? sample->source_file : nullptr;
  const bool matched = expected_label != nullptr && std::strcmp(top_label, expected_label) == 0;

  ESP_LOGI(TAG,
           "confidence expected=%s predicted=%s matched=%s selected_score=%.5f "
           "top_raw_label=%s top_raw_score=%.5f decision_reason=%s "
           "horn=%.5f horn_pass=%s horn_threshold=%.2f siren=%.5f siren_pass=%s "
           "siren_threshold=%.2f background=%.5f dsp_ms=%d classify_ms=%d "
           "classify_us=%lld source=%s",
           expected_label != nullptr ? expected_label : "",
           top_label,
           matched ? "yes" : "no",
           static_cast<double>(top_score),
           confidence.top_raw_label != nullptr ? confidence.top_raw_label : "<unknown>",
           static_cast<double>(confidence.top_raw_score),
           confidence.decision_reason != nullptr ? confidence.decision_reason : "",
           static_cast<double>(confidence.horn_score),
           confidence.horn_passed_threshold ? "yes" : "no",
           static_cast<double>(kTrafficInferenceHornThreshold),
           static_cast<double>(confidence.siren_score),
           confidence.siren_passed_threshold ? "yes" : "no",
           static_cast<double>(kTrafficInferenceSirenThreshold),
           static_cast<double>(confidence.background_score),
           result.timing.dsp,
           result.timing.classification,
           static_cast<long long>(result.timing.classification_us),
           source_file != nullptr ? source_file : "");

  if (!matched) {
    ESP_LOGW(TAG,
             "prediction mismatch expected=%s predicted=%s source=%s",
             expected_label != nullptr ? expected_label : "",
             top_label,
             source_file != nullptr ? source_file : "");
  }
}

}  // namespace

traffic_inference_confidence_summary_t traffic_inference_describe_confidence(
    const ei_impulse_result_t &result) {
  const ei_impulse_result_classification_t *horn =
      find_classification_by_label(result, kHornLabel);
  const ei_impulse_result_classification_t *siren =
      find_classification_by_label(result, kSirenLabel);
  const ei_impulse_result_classification_t *background =
      find_classification_by_label(result, kBackgroundLabel);
  const ei_impulse_result_classification_t *top =
      find_top_classification(result);
  const traffic_inference_selected_classification_t selected =
      traffic_inference_select_thresholded_classification(result);

  return {
      .horn_score = horn != nullptr ? horn->value : 0.0f,
      .siren_score = siren != nullptr ? siren->value : 0.0f,
      .background_score = background != nullptr ? background->value : 0.0f,
      .horn_passed_threshold =
          horn != nullptr && horn->value >= kTrafficInferenceHornThreshold,
      .siren_passed_threshold =
          siren != nullptr && siren->value >= kTrafficInferenceSirenThreshold,
      .top_raw_label = top != nullptr ? top->label : kBackgroundLabel,
      .top_raw_score = top != nullptr ? top->value : 0.0f,
      .selected_label = selected.label != nullptr ? selected.label : kBackgroundLabel,
      .selected_score = selected.score,
      .decision_reason =
          (selected.label != nullptr &&
           std::strcmp(selected.label, kBackgroundLabel) != 0)
              ? kDecisionThresholdPass
              : kDecisionThresholdFallback,
  };
}

traffic_inference_selected_classification_t
traffic_inference_select_thresholded_classification(
    const ei_impulse_result_t &result) {
  const ei_impulse_result_classification_t *horn =
      find_classification_by_label(result, kHornLabel);
  const ei_impulse_result_classification_t *siren =
      find_classification_by_label(result, kSirenLabel);
  const ei_impulse_result_classification_t *background =
      find_classification_by_label(result, kBackgroundLabel);
  const ei_impulse_result_classification_t *selected = background;

  if (horn != nullptr && horn->value >= kTrafficInferenceHornThreshold &&
      (siren == nullptr || horn->value >= siren->value ||
       siren->value < kTrafficInferenceSirenThreshold)) {
    selected = horn;
  } else if (siren != nullptr && siren->value >= kTrafficInferenceSirenThreshold) {
    selected = siren;
  }

  if (selected != nullptr) {
    return {
        .label = selected->label,
        .score = selected->value,
        .index = static_cast<size_t>(selected - result.classification),
    };
  }

  const ei_impulse_result_classification_t *top = find_top_classification(result);
  return {
      .label = top != nullptr ? top->label : kBackgroundLabel,
      .score = top != nullptr ? top->value : 0.0f,
      .index = top != nullptr ? static_cast<size_t>(top - result.classification) : 0U,
  };
}

esp_err_t traffic_inference_run_raw_samples(const int16_t *samples,
                                            size_t sample_count,
                                            ei_impulse_result_t *result) {
  if (samples == nullptr || result == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  if (sample_count != EI_CLASSIFIER_RAW_SAMPLE_COUNT) {
    ESP_LOGE(TAG,
             "sample length mismatch expected=%u actual=%u",
             (unsigned int)EI_CLASSIFIER_RAW_SAMPLE_COUNT,
             (unsigned int)sample_count);
    return ESP_ERR_INVALID_ARG;
  }

  sample_signal_context_t context = {
      .samples = samples,
      .sample_count = sample_count,
  };

  signal_t signal = {};
  signal.get_data = [&context](size_t offset, size_t length, float *out_ptr) -> int {
    return fill_signal_data(context, offset, length, out_ptr);
  };
  signal.total_length = sample_count;

  EI_IMPULSE_ERROR infer_ret = run_classifier(&signal, result, false);
  if (infer_ret != EI_IMPULSE_OK) {
    ESP_LOGE(TAG,
             "run_classifier failed err=%d sample_count=%u",
             infer_ret,
             (unsigned int)sample_count);
    return ESP_FAIL;
  }

  return ESP_OK;
}

esp_err_t traffic_inference_run_single_sample(
    const traffic_inference_demo_sample_t *sample) {
  if (sample == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  if (sample->sample_rate_hz != EI_CLASSIFIER_FREQUENCY) {
    ESP_LOGE(TAG,
             "sample rate mismatch expected=%d actual=%d source=%s",
             EI_CLASSIFIER_FREQUENCY,
             sample->sample_rate_hz,
             sample->source_file != nullptr ? sample->source_file : "");
    return ESP_ERR_INVALID_ARG;
  }

  ei_impulse_result_t result = {};
  esp_err_t raw_ret = traffic_inference_run_raw_samples(
      sample->samples, sample->sample_count, &result);
  if (raw_ret != ESP_OK) {
    ESP_LOGE(TAG,
             "raw inference failed expected=%s source=%s",
             sample->expected_label != nullptr ? sample->expected_label : "",
             sample->source_file != nullptr ? sample->source_file : "");
    return raw_ret;
  }

  log_inference_summary(sample, result);

  return ESP_OK;
}
