#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const int16_t *samples;
    size_t sample_count;
    int sample_rate_hz;
    const char *expected_label;
    const char *source_file;
} traffic_inference_demo_sample_t;

esp_err_t traffic_inference_run_single_sample(
    const traffic_inference_demo_sample_t *sample);

esp_err_t traffic_inference_run_demo(void);
esp_err_t traffic_inference_run_sliding_window_demo(void);

#ifdef __cplusplus
}
#endif
