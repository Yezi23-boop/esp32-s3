#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "../traffic_inference_runner_internal.h"
#include "model-parameters/model_metadata.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TRAFFIC_INFERENCE_SLIDING_WINDOW_WINDOW_SAMPLES                        \
  TRAFFIC_INFERENCE_RUNTIME_WINDOW_SAMPLES
#define TRAFFIC_INFERENCE_SLIDING_WINDOW_STRIDE_SAMPLES                        \
  TRAFFIC_INFERENCE_RUNTIME_STRIDE_SAMPLES
#define TRAFFIC_INFERENCE_SLIDING_WINDOW_RESULT_QUEUE_DEPTH                    \
  TRAFFIC_INFERENCE_RUNTIME_RESULT_QUEUE_DEPTH

#define TRAFFIC_INFERENCE_SLIDING_WINDOW_ERR_QUEUE_EMPTY ESP_ERR_NOT_FOUND
#define TRAFFIC_INFERENCE_SLIDING_WINDOW_ERR_BACKPRESSURE ESP_ERR_NO_MEM

typedef struct {
    uint32_t window_start_offset;
    uint32_t window_end_offset;
    const char *top_label;
    float top_score;
    float classification_scores[EI_CLASSIFIER_LABEL_COUNT];
    int dsp_time_ms;
    int classification_time_ms;
    int64_t classification_time_us;
} traffic_inference_sliding_window_result_t;

typedef struct {
    int16_t pcm_buffer[TRAFFIC_INFERENCE_SLIDING_WINDOW_WINDOW_SAMPLES +
                       TRAFFIC_INFERENCE_SLIDING_WINDOW_STRIDE_SAMPLES];
    traffic_inference_sliding_window_result_t
        result_queue[TRAFFIC_INFERENCE_SLIDING_WINDOW_RESULT_QUEUE_DEPTH];
    uint32_t buffer_start_offset;
    size_t buffered_sample_count;
    size_t queue_read_index;
    size_t queue_write_index;
    size_t queued_result_count;
} traffic_inference_sliding_window_t;

esp_err_t traffic_inference_sliding_window_init(
    traffic_inference_sliding_window_t *state);
esp_err_t traffic_inference_sliding_window_reset(
    traffic_inference_sliding_window_t *state);
esp_err_t traffic_inference_sliding_window_append_samples(
    traffic_inference_sliding_window_t *state,
    const int16_t *samples,
    size_t sample_count,
    size_t *consumed_samples);
esp_err_t traffic_inference_sliding_window_pop_result(
    traffic_inference_sliding_window_t *state,
    traffic_inference_sliding_window_result_t *out_result);

#ifdef __cplusplus
}
#endif
