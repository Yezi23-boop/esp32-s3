#include "services/memory_watch_ws_client.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "cJSON.h"
#include "esp_log.h"
#include "official_chat_websocket_transport.h"

namespace {

constexpr char kTag[] = "memory_watch_ws";
std::unique_ptr<official_chat::WebsocketTransport> g_ws;
memory_watch_ws_event_cb_t g_event_cb = nullptr;
memory_watch_ws_disconnect_cb_t g_disconnect_cb = nullptr;
void *g_user_ctx = nullptr;

void CopyText(char *dst, size_t dst_size, const char *src) {
  if (dst == nullptr || dst_size == 0) {
    return;
  }
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }
  std::snprintf(dst, dst_size, "%s", src);
}

bool IsSafeText(const char *value) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  for (const char *p = value; *p != '\0'; ++p) {
    if (*p == '\r' || *p == '\n') {
      return false;
    }
  }
  return true;
}

esp_err_t BuildWsUrl(const memory_watch_voice_client_config_t *config,
                     std::string *out_url) {
  if (config == nullptr || out_url == nullptr || !IsSafeText(config->base_url)) {
    return ESP_ERR_INVALID_ARG;
  }

  std::string base(config->base_url);
  if (base.rfind("https://", 0) == 0) {
    base.replace(0, 8, "wss://");
  } else if (base.rfind("http://", 0) == 0) {
    if (!config->allow_insecure_http) {
      ESP_LOGW(kTag, "reject ws over insecure HTTP endpoint");
      return ESP_ERR_INVALID_ARG;
    }
    base.replace(0, 7, "ws://");
  } else if (base.rfind("wss://", 0) == 0) {
    // Already WS URL.
  } else if (base.rfind("ws://", 0) == 0) {
    if (!config->allow_insecure_http) {
      ESP_LOGW(kTag, "reject insecure ws endpoint");
      return ESP_ERR_INVALID_ARG;
    }
  } else {
    return ESP_ERR_INVALID_ARG;
  }

  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }
  *out_url = base + MEMORY_WATCH_WS_PATH;
  return ESP_OK;
}

std::string JsonStringField(cJSON *root, const char *name) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
  if (cJSON_IsString(item) && item->valuestring != nullptr) {
    return item->valuestring;
  }
  return "";
}

memory_watch_ws_event_kind_t MapEventKind(const std::string &type,
                                          const std::string &role) {
  if (type == "asr_result") {
    return MEMORY_WATCH_WS_EVENT_TURN_ASR_READY;
  }
  if (type == "conversation_message" && role == "assistant") {
    return MEMORY_WATCH_WS_EVENT_TURN_REPLY_MESSAGE;
  }
  if (type == "error") {
    return MEMORY_WATCH_WS_EVENT_TURN_ERROR;
  }
  return MEMORY_WATCH_WS_EVENT_UNKNOWN;
}

void DispatchJson(const char *data, size_t len) {
  if (data == nullptr || len == 0 || g_event_cb == nullptr) {
    return;
  }
  cJSON *root = cJSON_ParseWithLength(data, len);
  if (root == nullptr) {
    ESP_LOGW(kTag, "ignore invalid websocket json");
    return;
  }

  memory_watch_ws_event_t event = {};
  const std::string type = JsonStringField(root, "type");
  const std::string role = JsonStringField(root, "role");
  event.kind = MapEventKind(type, role);
  CopyText(event.type, sizeof(event.type), type.c_str());
  CopyText(event.request_id, sizeof(event.request_id),
           JsonStringField(root, "request_id").c_str());
  CopyText(event.message_id, sizeof(event.message_id),
           JsonStringField(root, "message_id").c_str());
  CopyText(event.role, sizeof(event.role), role.c_str());
  CopyText(event.status, sizeof(event.status),
           JsonStringField(root, "status").c_str());
  CopyText(event.text, sizeof(event.text),
           JsonStringField(root, "text").c_str());
  CopyText(event.error_code, sizeof(event.error_code),
           JsonStringField(root, "error_code").c_str());

  g_event_cb(&event, g_user_ctx);
  cJSON_Delete(root);
}

esp_err_t SendJsonObject(cJSON *root) {
  if (root == nullptr || g_ws == nullptr || !g_ws->IsConnected()) {
    return ESP_ERR_INVALID_STATE;
  }
  char *rendered = cJSON_PrintUnformatted(root);
  if (rendered == nullptr) {
    return ESP_ERR_NO_MEM;
  }
  const bool sent = g_ws->Send(rendered);
  cJSON_free(rendered);
  return sent ? ESP_OK : ESP_FAIL;
}

esp_err_t AddString(cJSON *root, const char *name, const char *value) {
  if (value == nullptr) {
    value = "";
  }
  return cJSON_AddStringToObject(root, name, value) != nullptr ? ESP_OK
                                                               : ESP_ERR_NO_MEM;
}

}  // namespace

extern "C" esp_err_t memory_watch_ws_client_connect(
    const memory_watch_ws_client_config_t *config) {
  if (config == nullptr || config->event_cb == nullptr ||
      !IsSafeText(config->endpoint.device_id) ||
      !IsSafeText(config->endpoint.device_token)) {
    return ESP_ERR_INVALID_ARG;
  }

  std::string url;
  esp_err_t err = BuildWsUrl(&config->endpoint, &url);
  if (err != ESP_OK) {
    return err;
  }

  memory_watch_ws_client_close();
  g_event_cb = config->event_cb;
  g_disconnect_cb = config->disconnect_cb;
  g_user_ctx = config->user_ctx;

  g_ws = std::make_unique<official_chat::WebsocketTransport>();
  if (g_ws == nullptr) {
    return ESP_ERR_NO_MEM;
  }
  g_ws->OnDisconnected([]() {
    if (g_disconnect_cb != nullptr) {
      g_disconnect_cb(g_user_ctx);
    }
  });
  g_ws->OnData([](const char *data, size_t len, bool binary) {
    if (!binary) {
      DispatchJson(data, len);
    }
  });
  g_ws->OnError([](int error_code) {
    ESP_LOGW(kTag, "websocket transport error=%d", error_code);
  });

  ESP_LOGI(kTag, "connecting memory watch websocket");
  if (!g_ws->Connect(url.c_str())) {
    ESP_LOGW(kTag, "websocket connect failed err=%d", g_ws->GetLastError());
    memory_watch_ws_client_close();
    return ESP_FAIL;
  }

  cJSON *auth = cJSON_CreateObject();
  if (auth == nullptr) {
    memory_watch_ws_client_close();
    return ESP_ERR_NO_MEM;
  }
  err = AddString(auth, "type", "auth");
  if (err == ESP_OK) {
    err = AddString(auth, "device_id", config->endpoint.device_id);
  }
  if (err == ESP_OK) {
    err = AddString(auth, "device_token", config->endpoint.device_token);
  }
  if (err == ESP_OK && config->last_seen_conversation_id != nullptr &&
      config->last_seen_conversation_id[0] != '\0') {
    err = AddString(auth, "last_seen_conversation_id",
                    config->last_seen_conversation_id);
  }
  if (err == ESP_OK) {
    err = SendJsonObject(auth);
  }
  cJSON_Delete(auth);
  if (err != ESP_OK) {
    memory_watch_ws_client_close();
  }
  return err;
}

extern "C" esp_err_t memory_watch_ws_client_send_audio_start(
    const char *request_id) {
  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) {
    return ESP_ERR_NO_MEM;
  }
  esp_err_t err = AddString(root, "type", "audio_start");
  if (err == ESP_OK) {
    err = AddString(root, "request_id", request_id);
  }
  if (err == ESP_OK) {
    err = AddString(root, "format", "ogg_opus");
  }
  if (err == ESP_OK) {
    err = SendJsonObject(root);
  }
  cJSON_Delete(root);
  return err;
}

extern "C" esp_err_t memory_watch_ws_client_send_audio_chunk(
    const uint8_t *audio, size_t audio_len) {
  if (g_ws == nullptr || !g_ws->IsConnected() || audio == nullptr ||
      audio_len == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  return g_ws->Send(reinterpret_cast<const char *>(audio), audio_len, true)
             ? ESP_OK
             : ESP_FAIL;
}

extern "C" esp_err_t memory_watch_ws_client_send_audio_end(
    const char *request_id) {
  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) {
    return ESP_ERR_NO_MEM;
  }
  esp_err_t err = AddString(root, "type", "audio_end");
  if (err == ESP_OK) {
    err = AddString(root, "request_id", request_id);
  }
  if (err == ESP_OK) {
    err = SendJsonObject(root);
  }
  cJSON_Delete(root);
  return err;
}

extern "C" esp_err_t memory_watch_ws_client_send_audio_turn(
    const char *request_id, const uint8_t *audio, size_t audio_len,
    size_t chunk_size) {
  if (request_id == nullptr || audio == nullptr || audio_len == 0 ||
      chunk_size == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  esp_err_t err = memory_watch_ws_client_send_audio_start(request_id);
  for (size_t offset = 0; err == ESP_OK && offset < audio_len;) {
    const size_t remaining = audio_len - offset;
    const size_t part_len = remaining > chunk_size ? chunk_size : remaining;
    err = memory_watch_ws_client_send_audio_chunk(audio + offset, part_len);
    offset += part_len;
  }
  if (err == ESP_OK) {
    err = memory_watch_ws_client_send_audio_end(request_id);
  }
  return err;
}

extern "C" esp_err_t memory_watch_ws_client_send_ack(const char *message_id) {
  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) {
    return ESP_ERR_NO_MEM;
  }
  esp_err_t err = AddString(root, "type", "ack");
  if (err == ESP_OK) {
    err = AddString(root, "scope", "conversation");
  }
  if (err == ESP_OK) {
    err = AddString(root, "message_id", message_id);
  }
  if (err == ESP_OK) {
    err = SendJsonObject(root);
  }
  cJSON_Delete(root);
  return err;
}

extern "C" void memory_watch_ws_client_close(void) {
  if (g_ws != nullptr) {
    g_ws->Close();
    g_ws.reset();
  }
  g_event_cb = nullptr;
  g_disconnect_cb = nullptr;
  g_user_ctx = nullptr;
}

extern "C" bool memory_watch_ws_client_is_connected(void) {
  return g_ws != nullptr && g_ws->IsConnected();
}
