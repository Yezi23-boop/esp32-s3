#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TRAFFIC_AUDIO_RUNTIME_STATE_IDLE = 0,
    TRAFFIC_AUDIO_RUNTIME_STATE_STARTING,
    TRAFFIC_AUDIO_RUNTIME_STATE_RUNNING,
    TRAFFIC_AUDIO_RUNTIME_STATE_STOPPING,
    TRAFFIC_AUDIO_RUNTIME_STATE_FAILED,
} traffic_audio_runtime_state_t;

typedef struct {
    size_t input_chunk_frames;
    uint32_t read_timeout_ms;
    uint32_t task_stack_size;
    UBaseType_t task_priority;
} traffic_audio_runtime_config_t;

esp_err_t traffic_audio_runtime_start(
    const traffic_audio_runtime_config_t *config);
esp_err_t traffic_audio_runtime_stop(uint32_t timeout_ms);
bool traffic_audio_runtime_is_running(void);
traffic_audio_runtime_state_t traffic_audio_runtime_get_state(void);

#ifdef __cplusplus
}
#endif
