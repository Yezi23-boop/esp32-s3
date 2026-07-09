#include "protocol_config.h"

#include <nvs_flash.h>

#include <esp_log.h>

namespace official_chat {

namespace {

constexpr char kTag[] = "official_cfg";
constexpr char kNamespaceMqtt[] = "mqtt";
constexpr char kNamespaceWebsocket[] = "websocket";
constexpr char kKeyEndpoint[] = "endpoint";
constexpr char kKeyClientId[] = "client_id";
constexpr char kKeyUsername[] = "username";
constexpr char kKeyPassword[] = "password";
constexpr char kKeyKeepalive[] = "keepalive";
constexpr char kKeyPublishTopic[] = "publish_topic";
constexpr char kKeyUrl[] = "url";
constexpr char kKeyToken[] = "token";
constexpr char kKeyVersion[] = "version";

class ReadOnlySettings {
 public:
  explicit ReadOnlySettings(const char *name_space) {
    esp_err_t err = nvs_open(name_space, NVS_READONLY, &handle_);
    if (err != ESP_OK) {
      handle_ = 0;
    }
  }

  ~ReadOnlySettings() {
    if (handle_ != 0) {
      nvs_close(handle_);
      handle_ = 0;
    }
  }

  bool valid() const { return handle_ != 0; }

  bool GetString(const char *key, std::string *value) const {
    if (handle_ == 0 || value == nullptr) {
      return false;
    }

    size_t length = 0;
    if (nvs_get_str(handle_, key, nullptr, &length) != ESP_OK || length == 0) {
      return false;
    }

    std::string buffer(length, '\0');
    if (nvs_get_str(handle_, key, buffer.data(), &length) != ESP_OK) {
      return false;
    }
    while (!buffer.empty() && buffer.back() == '\0') {
      buffer.pop_back();
    }
    *value = std::move(buffer);
    return true;
  }

  bool GetInt(const char *key, int *value) const {
    if (handle_ == 0 || value == nullptr) {
      return false;
    }

    int32_t result = 0;
    if (nvs_get_i32(handle_, key, &result) != ESP_OK) {
      return false;
    }
    *value = static_cast<int>(result);
    return true;
  }

 private:
  nvs_handle_t handle_ = 0;
};

bool LoadMqttConfigFromNvs(MqttRuntimeConfig *config) {
  if (config == nullptr) {
    return false;
  }

  ReadOnlySettings settings(kNamespaceMqtt);
  if (!settings.valid()) {
    return false;
  }

  bool found = false;
  found = settings.GetString(kKeyEndpoint, &config->endpoint) || found;
  found = settings.GetString(kKeyClientId, &config->client_id) || found;
  found = settings.GetString(kKeyUsername, &config->username) || found;
  found = settings.GetString(kKeyPassword, &config->password) || found;
  found = settings.GetString(kKeyPublishTopic, &config->publish_topic) || found;
  found = settings.GetInt(kKeyKeepalive, &config->keepalive) || found;
  return found;
}

bool LoadWebsocketConfigFromNvs(WebsocketRuntimeConfig *config) {
  if (config == nullptr) {
    return false;
  }

  ReadOnlySettings settings(kNamespaceWebsocket);
  if (!settings.valid()) {
    return false;
  }

  bool found = false;
  found = settings.GetString(kKeyUrl, &config->url) || found;
  found = settings.GetString(kKeyToken, &config->token) || found;
  found = settings.GetInt(kKeyVersion, &config->version) || found;
  if (config->version <= 0) {
    config->version = 2;
  }
  return found;
}

}  // namespace

bool MqttRuntimeConfig::IsValid() const {
  return !endpoint.empty() && !publish_topic.empty();
}

bool WebsocketRuntimeConfig::IsValid() const { return !url.empty(); }

ProtocolConfigSelection LoadProtocolConfigSelection(
    const WebsocketFallbackConfig &fallback) {
  ProtocolConfigSelection selection;
  selection.websocket.url = fallback.url;
  selection.websocket.token = fallback.token;
  selection.websocket.version = fallback.version > 0 ? fallback.version : 2;
  selection.source = fallback.from_public_config
                         ? ProtocolConfigSource::kPublicConfig
                         : ProtocolConfigSource::kBuiltinDefault;

  selection.mqtt_config_present = LoadMqttConfigFromNvs(&selection.mqtt);
  if (selection.mqtt_config_present && selection.mqtt.IsValid()) {
    selection.kind = ProtocolKind::kMqtt;
    selection.source = ProtocolConfigSource::kNvsMqtt;
    return selection;
  }

  selection.websocket_config_present =
      LoadWebsocketConfigFromNvs(&selection.websocket);
  if (selection.websocket_config_present && selection.websocket.IsValid()) {
    selection.kind = ProtocolKind::kWebsocket;
    selection.source = ProtocolConfigSource::kNvsWebsocket;
    return selection;
  }

  if (!selection.websocket.IsValid()) {
    ESP_LOGW(kTag, "no valid websocket fallback found");
  }
  return selection;
}

const char *ProtocolKindToString(ProtocolKind kind) {
  switch (kind) {
    case ProtocolKind::kMqtt:
      return "mqtt";
    case ProtocolKind::kWebsocket:
    default:
      return "websocket";
  }
}

const char *ProtocolConfigSourceToString(ProtocolConfigSource source) {
  switch (source) {
    case ProtocolConfigSource::kNvsMqtt:
      return "nvs_mqtt";
    case ProtocolConfigSource::kNvsWebsocket:
      return "nvs_websocket";
    case ProtocolConfigSource::kPublicConfig:
      return "public_config";
    case ProtocolConfigSource::kBuiltinDefault:
    default:
      return "builtin_default";
  }
}

}  // namespace official_chat
