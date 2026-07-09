#include "traffic_audio_runtime.h"

#include <atomic>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "traffic_inference_realtime.h"

namespace {

constexpr char kTag[] = "traffic_audio_runtime";
constexpr uint32_t kDefaultTaskStackSize = 8192U;
constexpr UBaseType_t kDefaultTaskPriority = 5U;
constexpr TickType_t kStopPollIntervalTicks = pdMS_TO_TICKS(10);

struct runtime_control_t {
  std::atomic<int> state = {TRAFFIC_AUDIO_RUNTIME_STATE_IDLE};
  std::atomic<bool> stop_requested = {false};
  TaskHandle_t task_handle = nullptr;
  traffic_audio_runtime_config_t config = {};
  esp_err_t last_result = ESP_OK;
};

runtime_control_t s_runtime = {};

traffic_audio_runtime_state_t current_state() {
  return static_cast<traffic_audio_runtime_state_t>(s_runtime.state.load());
}

traffic_inference_realtime_config_t make_realtime_config() {
  traffic_inference_realtime_config_t config = {};
  config.input_chunk_frames = s_runtime.config.input_chunk_frames;
  config.read_timeout_ms = s_runtime.config.read_timeout_ms;
  config.max_read_iterations = 0U;
  return config;
}

bool should_stop_runtime(void *user_data) {
  (void)user_data;
  return s_runtime.stop_requested.load();
}

void reset_runtime_after_exit() {
  s_runtime.stop_requested.store(false);
  s_runtime.task_handle = nullptr;
}

void runtime_task(void *arg) {
  (void)arg;

  s_runtime.state.store(TRAFFIC_AUDIO_RUNTIME_STATE_RUNNING);
  const traffic_inference_realtime_config_t realtime_config =
      make_realtime_config();
  const esp_err_t ret = traffic_inference_run_realtime_sliding_window_loop(
      &realtime_config, should_stop_runtime, nullptr);
  s_runtime.last_result = ret;

  if (ret == ESP_OK || s_runtime.stop_requested.load()) {
    s_runtime.state.store(TRAFFIC_AUDIO_RUNTIME_STATE_IDLE);
  } else {
    ESP_LOGE(kTag, "runtime loop failed: %s", esp_err_to_name(ret));
    s_runtime.state.store(TRAFFIC_AUDIO_RUNTIME_STATE_FAILED);
  }

  reset_runtime_after_exit();
  vTaskDelete(nullptr);
}

}  // namespace

extern "C" {

esp_err_t traffic_audio_runtime_start(
    const traffic_audio_runtime_config_t *config) {
  if (s_runtime.task_handle != nullptr ||
      current_state() != TRAFFIC_AUDIO_RUNTIME_STATE_IDLE) {
    return ESP_ERR_INVALID_STATE;
  }

  s_runtime.config.input_chunk_frames =
      config != nullptr ? config->input_chunk_frames : 0U;
  s_runtime.config.read_timeout_ms =
      config != nullptr ? config->read_timeout_ms : 0U;
  s_runtime.config.task_stack_size =
      (config != nullptr && config->task_stack_size != 0U)
          ? config->task_stack_size
          : kDefaultTaskStackSize;
  s_runtime.config.task_priority =
      (config != nullptr && config->task_priority != 0U)
          ? config->task_priority
          : kDefaultTaskPriority;
  s_runtime.last_result = ESP_OK;
  s_runtime.stop_requested.store(false);
  s_runtime.state.store(TRAFFIC_AUDIO_RUNTIME_STATE_STARTING);

  BaseType_t created = xTaskCreate(runtime_task,
                                   "traffic_audio_rt",
                                   s_runtime.config.task_stack_size,
                                   nullptr,
                                   s_runtime.config.task_priority,
                                   &s_runtime.task_handle);
  if (created != pdPASS) {
    s_runtime.task_handle = nullptr;
    s_runtime.state.store(TRAFFIC_AUDIO_RUNTIME_STATE_FAILED);
    return ESP_ERR_NO_MEM;
  }

  return ESP_OK;
}

esp_err_t traffic_audio_runtime_stop(uint32_t timeout_ms) {
  if (s_runtime.task_handle == nullptr) {
    if (current_state() == TRAFFIC_AUDIO_RUNTIME_STATE_IDLE) {
      return ESP_OK;
    }
    return ESP_ERR_INVALID_STATE;
  }

  s_runtime.stop_requested.store(true);
  s_runtime.state.store(TRAFFIC_AUDIO_RUNTIME_STATE_STOPPING);

  const TickType_t deadline =
      xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms == 0U ? 1000U : timeout_ms);
  while (s_runtime.task_handle != nullptr) {
    if (xTaskGetTickCount() >= deadline) {
      return ESP_ERR_TIMEOUT;
    }
    vTaskDelay(kStopPollIntervalTicks);
  }

  return ESP_OK;
}

bool traffic_audio_runtime_is_running(void) {
  const traffic_audio_runtime_state_t state = current_state();
  return state == TRAFFIC_AUDIO_RUNTIME_STATE_STARTING ||
         state == TRAFFIC_AUDIO_RUNTIME_STATE_RUNNING ||
         state == TRAFFIC_AUDIO_RUNTIME_STATE_STOPPING;
}

traffic_audio_runtime_state_t traffic_audio_runtime_get_state(void) {
  return current_state();
}

}  // extern "C"
