#include "traffic_inference_postprocess.h"

#include <cstring>

#include "edge-impulse-sdk/classifier/ei_model_types.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "model-parameters/model_metadata.h"
#include "traffic_inference_runner_internal.h"

extern ei_impulse_handle_t &ei_default_impulse;

namespace {

constexpr float kSupportScoreThreshold = EI_CLASSIFIER_THRESHOLD;
constexpr char kTag[] = "traffic_postprocess";

using stable_label_t = traffic_inference_postprocess_stable_label_t;
using event_t = traffic_inference_postprocess_event_t;
using alert_action_t = traffic_inference_postprocess_alert_action_t;

typedef struct {
  int background_index;
  int horn_index;
  int siren_index;
} label_indices_t;

traffic_inference_postprocess_alert_callback_t s_alert_callback = nullptr;
void *s_alert_callback_user_data = nullptr;
traffic_inference_postprocess_snapshot_t s_latest_snapshot = {
    .raw_label = TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_NONE,
    .stable_label = TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_NONE,
    .horn_score = 0.0f,
    .siren_score = 0.0f,
};
portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;

stable_label_t normalize_label(const char *label) {
  if (label == nullptr) {
    return TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_NONE;
  }
  if (std::strcmp(label, "horn") == 0) {
    return TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_HORN;
  }
  if (std::strcmp(label, "siren") == 0) {
    return TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_SIREN;
  }
  if (std::strcmp(label, "background") == 0) {
    return TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_NONE;
  }
  return TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_NONE;
}

void clear_pending(traffic_inference_postprocess_state_t *state) {
  state->pending_label = TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_NONE;
  state->pending_hits = 0U;
}

void set_output_defaults(traffic_inference_postprocess_output_t *out,
                         stable_label_t raw_label,
                         stable_label_t stable_label) {
  out->raw_label = raw_label;
  out->stable_label = stable_label;
  out->event = TRAFFIC_INFERENCE_POSTPROCESS_EVENT_NONE;
  out->alert_fired = false;
  out->confidence_score = 0.0f;
}

const char *label_name_for(stable_label_t label) {
  switch (label) {
    case TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_NONE:
      return "background";
    case TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_HORN:
      return "horn";
    case TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_SIREN:
      return "siren";
    default:
      return nullptr;
  }
}

int find_label_index(const char *label_name) {
  if (label_name == nullptr || ei_default_impulse.impulse == nullptr ||
      ei_default_impulse.impulse->categories == nullptr) {
    return -1;
  }

  for (uint16_t idx = 0; idx < ei_default_impulse.impulse->label_count; ++idx) {
    const char *category = ei_default_impulse.impulse->categories[idx];
    if (category != nullptr && std::strcmp(category, label_name) == 0) {
      return static_cast<int>(idx);
    }
  }

  return -1;
}

const label_indices_t &label_indices() {
  static const label_indices_t kIndices = []() {
    label_indices_t indices = {
        .background_index = find_label_index("background"),
        .horn_index = find_label_index("horn"),
        .siren_index = find_label_index("siren"),
    };

    if (indices.background_index < 0 || indices.horn_index < 0 ||
        indices.siren_index < 0) {
      ESP_LOGW(kTag,
               "failed to resolve model label indices background=%d horn=%d siren=%d",
               indices.background_index,
               indices.horn_index,
               indices.siren_index);
    }
    return indices;
  }();
  return kIndices;
}

int index_for_label(stable_label_t label) {
  const label_indices_t &indices = label_indices();

  switch (label) {
    case TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_NONE:
      return indices.background_index;
    case TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_HORN:
      return indices.horn_index;
    case TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_SIREN:
      return indices.siren_index;
    default:
      return -1;
  }
}

float score_for_label(const traffic_inference_sliding_window_result_t *input,
                      stable_label_t label) {
  if (input == nullptr) {
    return 0.0f;
  }

  const int index = index_for_label(label);
  if (index < 0 || static_cast<size_t>(index) >= EI_CLASSIFIER_LABEL_COUNT) {
    return 0.0f;
  }

  return input->classification_scores[index];
}

bool has_support(const traffic_inference_sliding_window_result_t *input,
                 stable_label_t label) {
  const float support_score = score_for_label(input, label);
  return support_score >= kSupportScoreThreshold;
}

stable_label_t effective_label_for(const traffic_inference_postprocess_state_t *state,
                                   const traffic_inference_sliding_window_result_t *input,
                                   stable_label_t raw_label) {
  if (state == nullptr || input == nullptr) {
    return raw_label;
  }

  if (raw_label != TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_NONE) {
    return raw_label;
  }

  if (state->stable_label != TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_NONE &&
      has_support(input, state->stable_label)) {
    return state->stable_label;
  }

  if (state->pending_label != TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_NONE &&
      has_support(input, state->pending_label)) {
    return state->pending_label;
  }

  return raw_label;
}

alert_action_t alert_action_for_output(
    const traffic_inference_postprocess_output_t *out) {
  if (out == nullptr) {
    return TRAFFIC_INFERENCE_POSTPROCESS_ALERT_ACTION_NONE;
  }

  if (out->alert_fired) {
    return TRAFFIC_INFERENCE_POSTPROCESS_ALERT_ACTION_RAISE;
  }

  if (out->event == TRAFFIC_INFERENCE_POSTPROCESS_EVENT_END) {
    return TRAFFIC_INFERENCE_POSTPROCESS_ALERT_ACTION_CLEAR;
  }

  return TRAFFIC_INFERENCE_POSTPROCESS_ALERT_ACTION_NONE;
}

}  // namespace

esp_err_t traffic_inference_postprocess_reset(
    traffic_inference_postprocess_state_t *state) {
  if (state == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  state->stable_label = TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_NONE;
  state->pending_label = TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_NONE;
  state->pending_hits = 0U;
  state->consecutive_misses = 0U;
  state->alert_latched = false;
  return ESP_OK;
}

esp_err_t traffic_inference_postprocess_update(
    traffic_inference_postprocess_state_t *state,
    const traffic_inference_sliding_window_result_t *input,
    traffic_inference_postprocess_output_t *out) {
  if (state == nullptr || input == nullptr || out == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  const stable_label_t raw_label = normalize_label(input->top_label);
  const stable_label_t effective_label =
      effective_label_for(state, input, raw_label);
  set_output_defaults(out, raw_label, state->stable_label);
  out->confidence_score = score_for_label(input, effective_label);
  taskENTER_CRITICAL(&s_snapshot_lock);
  s_latest_snapshot.raw_label = raw_label;
  s_latest_snapshot.stable_label = state->stable_label;
  s_latest_snapshot.horn_score = score_for_label(
      input, TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_HORN);
  s_latest_snapshot.siren_score = score_for_label(
      input, TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_SIREN);
  taskEXIT_CRITICAL(&s_snapshot_lock);

  if (state->stable_label == TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_NONE) {
    state->consecutive_misses = 0U;
    if (effective_label == TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_NONE) {
      clear_pending(state);
      return ESP_OK;
    }

    if (state->pending_label == effective_label) {
      state->pending_hits += 1U;
    } else {
      state->pending_label = effective_label;
      state->pending_hits = 1U;
    }

    if (state->pending_hits >= kTrafficInferencePostprocessActivateHits) {
      state->stable_label = effective_label;
      clear_pending(state);
      out->stable_label = state->stable_label;
      out->event = TRAFFIC_INFERENCE_POSTPROCESS_EVENT_START;
      out->confidence_score = score_for_label(input, state->stable_label);
      taskENTER_CRITICAL(&s_snapshot_lock);
      s_latest_snapshot.stable_label = state->stable_label;
      taskEXIT_CRITICAL(&s_snapshot_lock);
      if (!state->alert_latched) {
        out->alert_fired = true;
        state->alert_latched = true;
      }
    }
    return ESP_OK;
  }

  if (effective_label == state->stable_label) {
    state->consecutive_misses = 0U;
    clear_pending(state);
    out->stable_label = state->stable_label;
    out->event = TRAFFIC_INFERENCE_POSTPROCESS_EVENT_ACTIVE;
    out->confidence_score = score_for_label(input, state->stable_label);
    taskENTER_CRITICAL(&s_snapshot_lock);
    s_latest_snapshot.stable_label = state->stable_label;
    taskEXIT_CRITICAL(&s_snapshot_lock);
    return ESP_OK;
  }

  state->consecutive_misses += 1U;
  if (effective_label == TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_NONE) {
    clear_pending(state);
  } else if (state->pending_label == effective_label) {
    state->pending_hits += 1U;
  } else {
    state->pending_label = effective_label;
    state->pending_hits = 1U;
  }

  if (state->consecutive_misses <= kTrafficInferencePostprocessHoldMisses) {
    out->stable_label = state->stable_label;
    out->event = TRAFFIC_INFERENCE_POSTPROCESS_EVENT_ACTIVE;
    out->confidence_score = score_for_label(input, state->stable_label);
    taskENTER_CRITICAL(&s_snapshot_lock);
    s_latest_snapshot.stable_label = state->stable_label;
    taskEXIT_CRITICAL(&s_snapshot_lock);
    return ESP_OK;
  }

  if (state->consecutive_misses >= kTrafficInferencePostprocessReleaseMisses) {
    const stable_label_t next_candidate = state->pending_label;
    state->stable_label = TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_NONE;
    state->consecutive_misses = 0U;
    state->alert_latched = false;
    out->stable_label = TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_NONE;
    out->event = TRAFFIC_INFERENCE_POSTPROCESS_EVENT_END;
    taskENTER_CRITICAL(&s_snapshot_lock);
    s_latest_snapshot.stable_label =
        TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_NONE;
    taskEXIT_CRITICAL(&s_snapshot_lock);
    if (next_candidate == TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_NONE) {
      clear_pending(state);
    } else {
      state->pending_label = next_candidate;
      state->pending_hits = 1U;
    }
  }

  return ESP_OK;
}

esp_err_t traffic_inference_postprocess_set_alert_callback(
    traffic_inference_postprocess_alert_callback_t callback,
    void *user_data) {
  s_alert_callback = callback;
  s_alert_callback_user_data = user_data;
  return ESP_OK;
}

esp_err_t traffic_inference_postprocess_dispatch_alert(
    const traffic_inference_postprocess_output_t *out) {
  if (out == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  const alert_action_t action = alert_action_for_output(out);
  if (action == TRAFFIC_INFERENCE_POSTPROCESS_ALERT_ACTION_NONE ||
      s_alert_callback == nullptr) {
    return ESP_OK;
  }

  const traffic_inference_postprocess_alert_t alert = {
      .label = action == TRAFFIC_INFERENCE_POSTPROCESS_ALERT_ACTION_RAISE
                   ? out->stable_label
                   : TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_NONE,
      .event = out->event,
      .action = action,
      .confidence_score = action == TRAFFIC_INFERENCE_POSTPROCESS_ALERT_ACTION_RAISE
                              ? out->confidence_score
                              : 0.0f,
  };
  s_alert_callback(&alert, s_alert_callback_user_data);
  return ESP_OK;
}

traffic_inference_postprocess_snapshot_t
traffic_inference_postprocess_get_latest_snapshot(void) {
  taskENTER_CRITICAL(&s_snapshot_lock);
  const traffic_inference_postprocess_snapshot_t snapshot = s_latest_snapshot;
  taskEXIT_CRITICAL(&s_snapshot_lock);
  return snapshot;
}

const char *traffic_inference_postprocess_stable_label_to_string(
    traffic_inference_postprocess_stable_label_t label) {
  switch (label) {
    case TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_HORN:
      return "horn";
    case TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_SIREN:
      return "siren";
    case TRAFFIC_INFERENCE_POSTPROCESS_STABLE_LABEL_NONE:
    default:
      return "none";
  }
}

const char *traffic_inference_postprocess_event_to_string(
    traffic_inference_postprocess_event_t event) {
  switch (event) {
    case TRAFFIC_INFERENCE_POSTPROCESS_EVENT_START:
      return "start";
    case TRAFFIC_INFERENCE_POSTPROCESS_EVENT_ACTIVE:
      return "active";
    case TRAFFIC_INFERENCE_POSTPROCESS_EVENT_END:
      return "end";
    case TRAFFIC_INFERENCE_POSTPROCESS_EVENT_NONE:
    default:
      return "none";
  }
}

const char *traffic_inference_postprocess_alert_action_to_string(
    traffic_inference_postprocess_alert_action_t action) {
  switch (action) {
    case TRAFFIC_INFERENCE_POSTPROCESS_ALERT_ACTION_RAISE:
      return "raise";
    case TRAFFIC_INFERENCE_POSTPROCESS_ALERT_ACTION_CLEAR:
      return "clear";
    case TRAFFIC_INFERENCE_POSTPROCESS_ALERT_ACTION_NONE:
    default:
      return "none";
  }
}
