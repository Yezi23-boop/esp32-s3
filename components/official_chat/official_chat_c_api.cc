#include "official_chat.h"

#include <new>

#include "application.h"
#include "settings.h"

/**
 * @brief 不透明 C 句柄结构体定义。
 *
 * 包装真正的由 C++ 实现的 Application 对象，对外部调用者隐藏底层复杂性。
 */
struct official_chat_handle {
  official_chat::Application app;
};

namespace {

/**
 * @brief 判断空字符串保护函数。
 * @return 字符串未定义或长度为0时返回 true。
 */
bool IsNullOrEmpty(const char *value) {
  return value == nullptr || value[0] == '\0';
}

/**
 * @brief 辅助存取 NVS KV 值的内部函数。
 * 如果值有效则写入设置，如果为空则抹除该字段。
 */
void SetOptionalString(official_chat::Settings *settings, const char *key,
                       const char *value) {
  if (settings == nullptr || key == nullptr || key[0] == '\0') {
    return;
  }
  if (IsNullOrEmpty(value)) {
    settings->EraseKey(key);
    return;
  }
  settings->SetString(key, value);
}

official_chat_state_t ToPublicState(official_chat::DeviceState state) {
  switch (state) {
    case official_chat::DeviceState::kActivating:
      return OFFICIAL_CHAT_STATE_ACTIVATING;
    case official_chat::DeviceState::kUpgrading:
      return OFFICIAL_CHAT_STATE_UPGRADING;
    case official_chat::DeviceState::kIdle:
      return OFFICIAL_CHAT_STATE_IDLE;
    case official_chat::DeviceState::kConnecting:
      return OFFICIAL_CHAT_STATE_CONNECTING;
    case official_chat::DeviceState::kListening:
      return OFFICIAL_CHAT_STATE_LISTENING;
    case official_chat::DeviceState::kSpeaking:
      return OFFICIAL_CHAT_STATE_SPEAKING;
    case official_chat::DeviceState::kUnknown:
    default:
      return OFFICIAL_CHAT_STATE_UNKNOWN;
  }
}

}  // namespace

official_chat_handle_t official_chat_create(const official_chat_config_t *config) {
  official_chat_handle *handle = new (std::nothrow) official_chat_handle();
  const official_chat_config_t default_config = {
      .speak_volume = 60,
      .record_gain_db = 24.0f,
      .websocket_url = nullptr,
      .access_token = nullptr,
      .ota_url = nullptr,
      .ensure_time_valid = nullptr,
      .apply_server_time = nullptr,
      .time_user_ctx = nullptr,
  };
  const official_chat_config_t *effective_config =
      config != nullptr ? config : &default_config;

  if (handle == nullptr) {
    return nullptr;
  }
  if (handle->app.Initialize(*effective_config) != ESP_OK) {
    delete handle;
    return nullptr;
  }
  return handle;
}

void official_chat_destroy(official_chat_handle_t handle) {
  delete handle;
}

esp_err_t official_chat_set_event_callback(official_chat_handle_t handle,
                                           official_chat_event_callback_t callback,
                                           void *user_data) {
  if (handle == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  return handle->app.SetEventCallback(callback, user_data);
}

esp_err_t official_chat_start(official_chat_handle_t handle) {
  if (handle == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  return handle->app.Start();
}

esp_err_t official_chat_prepare_shutdown(official_chat_handle_t handle) {
  if (handle == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  return handle->app.PrepareForShutdown();
}

esp_err_t official_chat_prepare_audio_channel(official_chat_handle_t handle) {
  if (handle == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  return handle->app.PrepareAudioChannel();
}

esp_err_t official_chat_start_listening(official_chat_handle_t handle) {
  if (handle == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  return handle->app.StartListening();
}

esp_err_t official_chat_start_synthetic_wakeword(official_chat_handle_t handle) {
  if (handle == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  return handle->app.StartSyntheticWakeWord();
}

esp_err_t official_chat_toggle_chat(official_chat_handle_t handle) {
  if (handle == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  return handle->app.ToggleChat();
}

esp_err_t official_chat_stop_listening(official_chat_handle_t handle) {
  if (handle == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  return handle->app.StopListening();
}

esp_err_t official_chat_set_device_aec_enabled(official_chat_handle_t handle,
                                               bool enabled) {
  if (handle == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  return handle->app.SetDeviceAecEnabled(enabled);
}

bool official_chat_get_device_aec_enabled(official_chat_handle_t handle) {
  if (handle == nullptr) {
    return false;
  }
  return handle->app.GetDeviceAecEnabled();
}

official_chat_state_t official_chat_get_state(official_chat_handle_t handle) {
  if (handle == nullptr) {
    return OFFICIAL_CHAT_STATE_UNKNOWN;
  }
  return ToPublicState(handle->app.GetState());
}

bool official_chat_is_audio_channel_ready(official_chat_handle_t handle) {
  if (handle == nullptr) {
    return false;
  }
  return handle->app.IsAudioChannelReady();
}

esp_err_t official_chat_set_mqtt_protocol_config(
    const official_chat_mqtt_config_t *config) {
  if (config == nullptr || IsNullOrEmpty(config->endpoint) ||
      IsNullOrEmpty(config->publish_topic)) {
    return ESP_ERR_INVALID_ARG;
  }

  official_chat::Settings mqtt_settings("mqtt", true);
  official_chat::Settings websocket_settings("websocket", true);

  mqtt_settings.SetString("endpoint", config->endpoint);
  mqtt_settings.SetString("publish_topic", config->publish_topic);
  SetOptionalString(&mqtt_settings, "client_id", config->client_id);
  SetOptionalString(&mqtt_settings, "username", config->username);
  SetOptionalString(&mqtt_settings, "password", config->password);
  if (config->keepalive > 0) {
    mqtt_settings.SetInt("keepalive", config->keepalive);
  } else {
    mqtt_settings.EraseKey("keepalive");
  }

  websocket_settings.EraseAll();
  return ESP_OK;
}

esp_err_t official_chat_set_websocket_protocol_config(
    const official_chat_websocket_config_t *config) {
  if (config == nullptr || IsNullOrEmpty(config->url)) {
    return ESP_ERR_INVALID_ARG;
  }

  official_chat::Settings mqtt_settings("mqtt", true);
  official_chat::Settings websocket_settings("websocket", true);

  websocket_settings.SetString("url", config->url);
  SetOptionalString(&websocket_settings, "token", config->token);
  websocket_settings.SetInt("version", config->version > 0 ? config->version : 2);

  mqtt_settings.EraseAll();
  return ESP_OK;
}

esp_err_t official_chat_reload_protocol(official_chat_handle_t handle) {
  if (handle == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  return handle->app.ReloadProtocol();
}
