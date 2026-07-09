#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "traffic_inference_sliding_window.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_NONE = 0,
    TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_HORN = 1,
    TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_SIREN = 2,
} traffic_inference_postprocess_stable_label_t;

typedef enum {
    TRAFFIC_INFERENCE_POSTPROCESS_EVENT_NONE = 0,
    TRAFFIC_INFERENCE_POSTPROCESS_EVENT_START = 1,
    TRAFFIC_INFERENCE_POSTPROCESS_EVENT_ACTIVE = 2,
    TRAFFIC_INFERENCE_POSTPROCESS_EVENT_END = 3,
} traffic_inference_postprocess_event_t;

typedef enum {
    TRAFFIC_INFERENCE_POSTPROCESS_ALERT_ACTION_NONE = 0,
    TRAFFIC_INFERENCE_POSTPROCESS_ALERT_ACTION_RAISE = 1,
    TRAFFIC_INFERENCE_POSTPROCESS_ALERT_ACTION_CLEAR = 2,
} traffic_inference_postprocess_alert_action_t;

typedef struct {
    traffic_inference_postprocess_stable_label_t stable_label;
    traffic_inference_postprocess_stable_label_t pending_label;
    unsigned int pending_hits;
    unsigned int consecutive_misses;
    bool alert_latched;
} traffic_inference_postprocess_state_t;

typedef struct {
    traffic_inference_postprocess_stable_label_t raw_label;
    traffic_inference_postprocess_stable_label_t stable_label;
    traffic_inference_postprocess_event_t event;
    bool alert_fired;
    float confidence_score;
} traffic_inference_postprocess_output_t;

typedef struct {
    traffic_inference_postprocess_stable_label_t raw_label;
    traffic_inference_postprocess_stable_label_t stable_label;
    float horn_score;
    float siren_score;
} traffic_inference_postprocess_snapshot_t;

typedef struct {
    traffic_inference_postprocess_stable_label_t label;
    traffic_inference_postprocess_event_t event;
    traffic_inference_postprocess_alert_action_t action;
    float confidence_score;
} traffic_inference_postprocess_alert_t;

typedef void (*traffic_inference_postprocess_alert_callback_t)(
    const traffic_inference_postprocess_alert_t *alert,
    void *user_data);

esp_err_t traffic_inference_postprocess_reset(
    traffic_inference_postprocess_state_t *state);
esp_err_t traffic_inference_postprocess_update(
    traffic_inference_postprocess_state_t *state,
    const traffic_inference_sliding_window_result_t *input,
    traffic_inference_postprocess_output_t *out);
esp_err_t traffic_inference_postprocess_set_alert_callback(
    traffic_inference_postprocess_alert_callback_t callback,
    void *user_data);
esp_err_t traffic_inference_postprocess_dispatch_alert(
    const traffic_inference_postprocess_output_t *out);
traffic_inference_postprocess_snapshot_t
traffic_inference_postprocess_get_latest_snapshot(void);

const char *traffic_inference_postprocess_stable_label_to_string(
    traffic_inference_postprocess_stable_label_t label);
const char *traffic_inference_postprocess_event_to_string(
    traffic_inference_postprocess_event_t event);
const char *traffic_inference_postprocess_alert_action_to_string(
    traffic_inference_postprocess_alert_action_t action);

#ifdef __cplusplus
}
#endif
