#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t input_chunk_frames;
    uint32_t read_timeout_ms;
    size_t max_read_iterations;
} traffic_inference_realtime_config_t;

typedef bool (*traffic_inference_realtime_should_stop_fn)(void *user_data);

esp_err_t traffic_inference_run_realtime_sliding_window_loop(
    const traffic_inference_realtime_config_t *config,
    traffic_inference_realtime_should_stop_fn should_stop,
    void *user_data);

esp_err_t traffic_inference_run_realtime_sliding_window_demo(
    const traffic_inference_realtime_config_t *config);

#ifdef __cplusplus
}
#endif
