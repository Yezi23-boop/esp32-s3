#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "model-parameters/model_metadata.h"

// Centralized runtime knobs for traffic inference. Prefer editing these values
// here instead of scattering literal constants across runner/sliding-window/
// postprocess code paths.
#define TRAFFIC_INFERENCE_RUNTIME_WINDOW_SAMPLES EI_CLASSIFIER_RAW_SAMPLE_COUNT
#define TRAFFIC_INFERENCE_RUNTIME_STRIDE_MS 300U
#define TRAFFIC_INFERENCE_RUNTIME_STRIDE_SAMPLES \
    ((EI_CLASSIFIER_FREQUENCY * TRAFFIC_INFERENCE_RUNTIME_STRIDE_MS) / 1000U)
#define TRAFFIC_INFERENCE_RUNTIME_RESULT_QUEUE_DEPTH 5U

#define TRAFFIC_INFERENCE_HORN_THRESHOLD 0.70f
#define TRAFFIC_INFERENCE_SIREN_THRESHOLD 0.80f
#define TRAFFIC_INFERENCE_POSTPROCESS_ACTIVATE_HITS 1U
#define TRAFFIC_INFERENCE_POSTPROCESS_HOLD_MISSES 1U
#define TRAFFIC_INFERENCE_POSTPROCESS_RELEASE_MISSES 3U

#ifdef __cplusplus

#include "edge-impulse-sdk/classifier/ei_classifier_types.h"

inline constexpr size_t kTrafficInferenceRuntimeWindowSamples =
    TRAFFIC_INFERENCE_RUNTIME_WINDOW_SAMPLES;
inline constexpr size_t kTrafficInferenceRuntimeStrideSamples =
    TRAFFIC_INFERENCE_RUNTIME_STRIDE_SAMPLES;
inline constexpr size_t kTrafficInferenceRuntimeResultQueueDepth =
    TRAFFIC_INFERENCE_RUNTIME_RESULT_QUEUE_DEPTH;

inline constexpr float kTrafficInferenceHornThreshold =
    TRAFFIC_INFERENCE_HORN_THRESHOLD;
inline constexpr float kTrafficInferenceSirenThreshold =
    TRAFFIC_INFERENCE_SIREN_THRESHOLD;
inline constexpr unsigned int kTrafficInferencePostprocessActivateHits =
    TRAFFIC_INFERENCE_POSTPROCESS_ACTIVATE_HITS;
inline constexpr unsigned int kTrafficInferencePostprocessHoldMisses =
    TRAFFIC_INFERENCE_POSTPROCESS_HOLD_MISSES;
inline constexpr unsigned int kTrafficInferencePostprocessReleaseMisses =
    TRAFFIC_INFERENCE_POSTPROCESS_RELEASE_MISSES;

struct traffic_inference_selected_classification_t
{
    const char *label;
    float score;
    size_t index;
};

struct traffic_inference_confidence_summary_t
{
    float horn_score;
    float siren_score;
    float background_score;
    bool horn_passed_threshold;
    bool siren_passed_threshold;
    const char *top_raw_label;
    float top_raw_score;
    const char *selected_label;
    float selected_score;
    const char *decision_reason;
};

esp_err_t traffic_inference_run_raw_samples(
    const int16_t *samples,
    size_t sample_count,
    ei_impulse_result_t *result);

traffic_inference_confidence_summary_t
traffic_inference_describe_confidence(const ei_impulse_result_t &result);

traffic_inference_selected_classification_t
traffic_inference_select_thresholded_classification(
    const ei_impulse_result_t &result);

#endif // __cplusplus
