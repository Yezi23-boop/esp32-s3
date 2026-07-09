#include "protocols/mqtt_protocol.h"

#include "sdkconfig.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#include <arpa/inet.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <unistd.h>

#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <freertos/idf_additions.h>

#include "system_util.h"

namespace official_chat {

namespace {

constexpr char kTag[] = "official_mqtt";
constexpr char kTransportUdp[] = "udp";
constexpr int kMqttConnectTimeoutMs = 10000;
constexpr int kServerHelloTimeoutMs = 10000;
constexpr int kReconnectIntervalMs = 60000;
constexpr configSTACK_DEPTH_TYPE kUdpReceiveTaskStackBytes = 4096;
constexpr UBaseType_t kUdpReceiveTaskPriority = 4;
constexpr int kUdpSocketRecvBufferBytes = 32768;
constexpr int kUdpSocketTimeoutMs = 1000;
constexpr int64_t kUdpAudioStallTimeoutUs =
    static_cast<int64_t>(CONFIG_OFFICIAL_CHAT_UDP_AUDIO_STALL_TIMEOUT_MS) *
    1000;
constexpr size_t kMaxTrackedOutboundPayloads = 8;

void LogUdpTaskHeapState(const char *reason) {
  ESP_LOGI(kTag,
           "udp task heap (%s): internal_free=%lu internal_largest=%lu spiram_free=%lu spiram_largest=%lu",
           reason != nullptr ? reason : "unknown",
           static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned long>(
               heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
           static_cast<unsigned long>(
               heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
}

struct ParsedEndpoint {
  std::string host;
  int port = 8883;
  bool use_tls = true;
};

ParsedEndpoint ParseEndpoint(const std::string &endpoint) {
  ParsedEndpoint parsed;
  std::string host_port = endpoint;

  const size_t scheme_pos = endpoint.find("://");
  if (scheme_pos != std::string::npos) {
    const std::string scheme = endpoint.substr(0, scheme_pos);
    host_port = endpoint.substr(scheme_pos + 3);
    if (scheme == "mqtt") {
      parsed.use_tls = false;
      parsed.port = 1883;
    } else if (scheme == "mqtts") {
      parsed.use_tls = true;
      parsed.port = 8883;
    }
  }

  const size_t slash_pos = host_port.find('/');
  if (slash_pos != std::string::npos) {
    host_port = host_port.substr(0, slash_pos);
  }

  const size_t colon_pos = host_port.rfind(':');
  if (colon_pos != std::string::npos) {
    parsed.host = host_port.substr(0, colon_pos);
    parsed.port = std::stoi(host_port.substr(colon_pos + 1));
    parsed.use_tls = parsed.port == 8883;
  } else {
    parsed.host = host_port;
  }

  return parsed;
}

}  // namespace

MqttProtocol::MqttProtocol(MqttRuntimeConfig config)
    : config_(std::move(config)), publish_topic_(config_.publish_topic) {
  event_group_handle_ = xEventGroupCreate();
  mbedtls_aes_init(&aes_ctx_);

  esp_timer_create_args_t timer_args = {};
  timer_args.callback = &MqttProtocol::ReconnectTimerCallback;
  timer_args.arg = this;
  timer_args.name = "mqtt_reconnect";
  esp_timer_create(&timer_args, &reconnect_timer_);
}

MqttProtocol::~MqttProtocol() {
  alive_.store(false);
  CancelReconnect();
  DisconnectUdpSocket();
  StopMqttClient();
  if (reconnect_timer_ != nullptr) {
    esp_timer_delete(reconnect_timer_);
    reconnect_timer_ = nullptr;
  }
  if (event_group_handle_ != nullptr) {
    vEventGroupDelete(event_group_handle_);
    event_group_handle_ = nullptr;
  }
  mbedtls_aes_free(&aes_ctx_);
}

bool MqttProtocol::Start() { return StartMqttClient(false, false); }

bool MqttProtocol::StartMqttClient(bool report_error, bool wait_for_connection) {
  if (!config_.IsValid()) {
    ESP_LOGW(kTag, "mqtt config is invalid");
    if (report_error) {
      SetError("mqtt config invalid");
    }
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mqtt_mutex_);
    if (mqtt_client_handle_ != nullptr) {
      StopMqttClientLocked();
    }

    const ParsedEndpoint endpoint = ParseEndpoint(config_.endpoint);
    if (endpoint.host.empty()) {
      ESP_LOGW(kTag, "mqtt endpoint host is empty");
      if (report_error) {
        SetError("mqtt endpoint missing");
      }
      return false;
    }

    esp_mqtt_client_config_t mqtt_config = {};
    mqtt_config.task.stack_size = 4096;
    mqtt_config.broker.address.hostname = endpoint.host.c_str();
    mqtt_config.broker.address.port = endpoint.port;
    mqtt_config.broker.address.transport = endpoint.use_tls
                                               ? MQTT_TRANSPORT_OVER_SSL
                                               : MQTT_TRANSPORT_OVER_TCP;
    if (endpoint.use_tls) {
      mqtt_config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }
    mqtt_config.credentials.client_id =
        config_.client_id.empty() ? generate_uuid() : config_.client_id.c_str();
    mqtt_config.credentials.username =
        config_.username.empty() ? nullptr : config_.username.c_str();
    mqtt_config.credentials.authentication.password =
        config_.password.empty() ? nullptr : config_.password.c_str();
    mqtt_config.session.keepalive = config_.keepalive;
    mqtt_config.network.disable_auto_reconnect = true;
    mqtt_config.network.timeout_ms = kMqttConnectTimeoutMs;

    mqtt_client_handle_ = esp_mqtt_client_init(&mqtt_config);
    if (mqtt_client_handle_ == nullptr) {
      ESP_LOGE(kTag, "failed to create mqtt client");
      if (report_error) {
        SetError("mqtt init failed");
      }
      return false;
    }

    esp_mqtt_client_register_event(mqtt_client_handle_, MQTT_EVENT_ANY,
                                   &MqttProtocol::MqttEventHandler, this);
    xEventGroupClearBits(event_group_handle_,
                         MQTT_PROTOCOL_CONNECTED_EVENT |
                             MQTT_PROTOCOL_DISCONNECTED_EVENT);
    manual_mqtt_stop_.store(false);
    mqtt_connected_.store(false);
    esp_err_t err = esp_mqtt_client_start(mqtt_client_handle_);
    if (err != ESP_OK) {
      ESP_LOGE(kTag, "failed to start mqtt client: %s", esp_err_to_name(err));
      manual_mqtt_stop_.store(true);
      esp_mqtt_client_destroy(mqtt_client_handle_);
      mqtt_client_handle_ = nullptr;
      if (report_error) {
        SetError("mqtt start failed");
      }
      return false;
    }
  }

  if (!wait_for_connection) {
    return true;
  }

  ESP_LOGI(kTag, "waiting for mqtt connected event");
  const EventBits_t bits = xEventGroupWaitBits(
      event_group_handle_,
      MQTT_PROTOCOL_CONNECTED_EVENT | MQTT_PROTOCOL_DISCONNECTED_EVENT, pdTRUE,
      pdFALSE, pdMS_TO_TICKS(kMqttConnectTimeoutMs));
  if ((bits & MQTT_PROTOCOL_CONNECTED_EVENT) == 0) {
    ESP_LOGE(kTag, "failed to connect mqtt endpoint within timeout");
    if (report_error) {
      SetError("mqtt connect timeout");
    }
    return false;
  }
  return true;
}

void MqttProtocol::StopMqttClient() {
  std::lock_guard<std::mutex> lock(mqtt_mutex_);
  StopMqttClientLocked();
}

void MqttProtocol::StopMqttClientLocked() {
  if (mqtt_client_handle_ == nullptr) {
    mqtt_connected_.store(false);
    return;
  }

  manual_mqtt_stop_.store(true);
  const esp_err_t stop_err = esp_mqtt_client_stop(mqtt_client_handle_);
  if (stop_err != ESP_OK) {
    ESP_LOGW(kTag, "esp_mqtt_client_stop failed during shutdown: %s",
             esp_err_to_name(stop_err));
  }
  esp_mqtt_client_destroy(mqtt_client_handle_);
  mqtt_client_handle_ = nullptr;
  mqtt_connected_.store(false);
  xEventGroupClearBits(event_group_handle_, MQTT_PROTOCOL_CONNECTED_EVENT |
                                               MQTT_PROTOCOL_DISCONNECTED_EVENT |
                                               MQTT_PROTOCOL_SERVER_HELLO_EVENT);
}

void MqttProtocol::MqttEventHandler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data) {
  (void)base;
  (void)event_id;
  auto *self = static_cast<MqttProtocol *>(handler_args);
  if (self == nullptr || !self->alive_.load()) {
    return;
  }
  self->HandleMqttEvent(static_cast<esp_mqtt_event_handle_t>(event_data));
}

void MqttProtocol::HandleMqttEvent(esp_mqtt_event_handle_t event) {
  if (event == nullptr) {
    return;
  }

  switch (event->event_id) {
    case MQTT_EVENT_CONNECTED: {
      mqtt_connected_.store(true);
      CancelReconnect();
      xEventGroupSetBits(event_group_handle_, MQTT_PROTOCOL_CONNECTED_EVENT);
      if (on_connected_) {
        on_connected_();
      }
      int msg_id =
          esp_mqtt_client_subscribe(mqtt_client_handle_, publish_topic_.c_str(), 0);
      ESP_LOGI(kTag, "mqtt connected, subscribe topic=%s msg_id=%d",
               publish_topic_.c_str(), msg_id);
      break;
    }
    case MQTT_EVENT_DISCONNECTED: {
      const bool manual = manual_mqtt_stop_.load();
      mqtt_connected_.store(false);
      xEventGroupSetBits(event_group_handle_, MQTT_PROTOCOL_DISCONNECTED_EVENT);
      xEventGroupClearBits(event_group_handle_, MQTT_PROTOCOL_CONNECTED_EVENT |
                                               MQTT_PROTOCOL_SERVER_HELLO_EVENT);
      if (on_disconnected_) {
        on_disconnected_();
      }
      const bool had_active_channel =
          udp_connected_.load() || !session_id_.empty();
      DisconnectUdpSocket();
      if (!manual && had_active_channel) {
        SetError("mqtt disconnected");
      }
      if (!manual) {
        ScheduleReconnect();
      }
      break;
    }
    case MQTT_EVENT_DATA: {
      std::string topic =
          event->topic != nullptr ? std::string(event->topic, event->topic_len) : "";
      std::string payload =
          event->data != nullptr ? std::string(event->data, event->data_len) : "";
      if (event->current_data_offset == 0) {
        incoming_topic_ = topic;
        incoming_payload_.clear();
      }
      incoming_payload_.append(payload);
      if (static_cast<int>(incoming_payload_.size()) < event->total_data_len) {
        return;
      }

      std::string full_payload = std::move(incoming_payload_);
      incoming_payload_.clear();
      if (ShouldIgnoreLoopbackPayload(full_payload)) {
        ESP_LOGD(kTag, "ignore loopback mqtt payload");
        return;
      }

      cJSON *root = cJSON_ParseWithLength(full_payload.data(), full_payload.size());
      if (root == nullptr) {
        ESP_LOGE(kTag, "failed to parse mqtt json payload");
        return;
      }

      cJSON *type = cJSON_GetObjectItem(root, "type");
      if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
      }

      const int64_t now_us = esp_timer_get_time();
      if (std::strcmp(type->valuestring, "hello") == 0) {
        ParseServerHello(root);
      } else if (std::strcmp(type->valuestring, "goodbye") == 0) {
        ESP_LOGI(kTag, "received mqtt goodbye");
        CloseAudioChannel(false);
      } else if (std::strcmp(type->valuestring, "tts") == 0) {
        const cJSON *state = cJSON_GetObjectItem(root, "state");
        if (cJSON_IsString(state)) {
          if (std::strcmp(state->valuestring, "start") == 0) {
            ESP_LOGI(kTag, "tts state=start audio_active=1");
            ResetUdpAudioStallTracking(true);
          } else if (std::strcmp(state->valuestring, "stop") == 0) {
            ESP_LOGI(kTag, "tts state=stop audio_active=0");
            ResetUdpAudioStallTracking(false);
          } else if (tts_downlink_active_.load(std::memory_order_acquire) &&
                     !udp_audio_stall_detected_.load(std::memory_order_acquire) &&
                     HasUdpAudioTimedOut(now_us)) {
            const int64_t silence_ms =
                (now_us - last_udp_audio_time_us_.load(std::memory_order_acquire)) /
                1000;
            udp_audio_stall_detected_.store(true, std::memory_order_release);
            ESP_LOGW(
                kTag,
                "udp audio stalled while mqtt tts text continues state=%s silence_ms=%lld session_id=%s",
                state->valuestring, static_cast<long long>(silence_ms),
                session_id_.empty() ? "<none>" : session_id_.c_str());
            CloseAudioChannel(false);
            SetError("udp audio stalled while mqtt tts text continues");
          }
        }
        if (on_incoming_json_ != nullptr) {
          ESP_LOGI(kTag, "received mqtt json type: %s", type->valuestring);
          on_incoming_json_(root);
        }
      } else if (on_incoming_json_ != nullptr) {
        ESP_LOGI(kTag, "received mqtt json type: %s", type->valuestring);
        on_incoming_json_(root);
      }
      cJSON_Delete(root);
      last_incoming_time_ = std::chrono::steady_clock::now();
      break;
    }
    case MQTT_EVENT_ERROR: {
      ESP_LOGE(kTag, "mqtt error event");
      SetError("mqtt transport error");
      break;
    }
    default:
      break;
  }
}

void MqttProtocol::ReconnectTimerCallback(void *arg) {
  auto *self = static_cast<MqttProtocol *>(arg);
  if (self == nullptr || !self->alive_.load() || self->mqtt_connected_.load()) {
    return;
  }
  ESP_LOGI(kTag, "mqtt reconnect timer fired");
  self->StartMqttClient(false, false);
}

void MqttProtocol::ScheduleReconnect() {
  if (reconnect_timer_ == nullptr || !alive_.load()) {
    return;
  }
  esp_timer_stop(reconnect_timer_);
  esp_timer_start_once(reconnect_timer_,
                       static_cast<uint64_t>(kReconnectIntervalMs) * 1000ULL);
}

void MqttProtocol::CancelReconnect() {
  if (reconnect_timer_ != nullptr) {
    esp_timer_stop(reconnect_timer_);
  }
}

bool MqttProtocol::SendText(const std::string &text) {
  std::lock_guard<std::mutex> lock(mqtt_mutex_);
  if (mqtt_client_handle_ == nullptr || !mqtt_connected_.load()) {
    return false;
  }

  int msg_id = esp_mqtt_client_publish(mqtt_client_handle_,
                                       publish_topic_.c_str(),
                                       text.data(), text.size(), 0, 0);
  if (msg_id < 0) {
    ESP_LOGE(kTag, "failed to publish mqtt payload");
    SetError("mqtt publish failed");
    return false;
  }
  RecordOutboundPayload(text);
  return true;
}

void MqttProtocol::RecordOutboundPayload(const std::string &payload) {
  std::lock_guard<std::mutex> lock(outbound_mutex_);
  if (recent_outbound_payloads_.size() >= kMaxTrackedOutboundPayloads) {
    recent_outbound_payloads_.pop_front();
  }
  recent_outbound_payloads_.push_back(payload);
}

bool MqttProtocol::ShouldIgnoreLoopbackPayload(const std::string &payload) {
  std::lock_guard<std::mutex> lock(outbound_mutex_);
  auto it = std::find(recent_outbound_payloads_.begin(),
                      recent_outbound_payloads_.end(), payload);
  if (it == recent_outbound_payloads_.end()) {
    return false;
  }
  recent_outbound_payloads_.erase(it);
  return true;
}

bool MqttProtocol::OpenAudioChannel() {
  if ((!mqtt_connected_.load()) &&
      !StartMqttClient(true, true)) {
    return false;
  }

  error_occurred_ = false;
  session_id_.clear();
  xEventGroupClearBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);

  const std::string message = GetHelloMessage();
  if (!SendText(message)) {
    return false;
  }

  ESP_LOGI(kTag, "waiting for mqtt server hello");
  const EventBits_t bits = xEventGroupWaitBits(
      event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE,
      pdMS_TO_TICKS(kServerHelloTimeoutMs));
  if ((bits & MQTT_PROTOCOL_SERVER_HELLO_EVENT) == 0) {
    ESP_LOGE(kTag, "failed to receive mqtt server hello");
    SetError("mqtt server hello timeout");
    return false;
  }

  if (!ConnectUdpSocket()) {
    SetError("udp connect failed");
    return false;
  }

  if (on_audio_channel_opened_) {
    on_audio_channel_opened_();
  }
  return true;
}

std::string MqttProtocol::GetHelloMessage() {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "type", "hello");
  cJSON_AddNumberToObject(root, "version", 3);
  cJSON_AddStringToObject(root, "transport", "udp");
  cJSON *features = cJSON_CreateObject();
  cJSON_AddBoolToObject(features, "mcp", true);
  cJSON_AddItemToObject(root, "features", features);
  cJSON *audio_params = cJSON_CreateObject();
  cJSON_AddStringToObject(audio_params, "format", "opus");
  cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
  cJSON_AddNumberToObject(audio_params, "channels", 1);
  cJSON_AddNumberToObject(audio_params, "frame_duration", 60);
  cJSON_AddItemToObject(root, "audio_params", audio_params);

  char *json_str = cJSON_PrintUnformatted(root);
  std::string message = json_str != nullptr ? json_str : "";
  if (json_str != nullptr) {
    cJSON_free(json_str);
  }
  cJSON_Delete(root);
  return message;
}

void MqttProtocol::ParseServerHello(const cJSON *root) {
  const cJSON *transport = cJSON_GetObjectItem(root, "transport");
  if (!cJSON_IsString(transport) ||
      std::strcmp(transport->valuestring, kTransportUdp) != 0) {
    ESP_LOGE(kTag, "unsupported mqtt hello transport");
    return;
  }

  const cJSON *session_id = cJSON_GetObjectItem(root, "session_id");
  if (cJSON_IsString(session_id)) {
    session_id_ = session_id->valuestring;
  }

  const cJSON *audio_params = cJSON_GetObjectItem(root, "audio_params");
  if (cJSON_IsObject(audio_params)) {
    const cJSON *sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
    if (cJSON_IsNumber(sample_rate)) {
      server_sample_rate_ = sample_rate->valueint;
    }
    const cJSON *frame_duration =
        cJSON_GetObjectItem(audio_params, "frame_duration");
    if (cJSON_IsNumber(frame_duration)) {
      server_frame_duration_ = frame_duration->valueint;
    }
  }

  const cJSON *udp = cJSON_GetObjectItem(root, "udp");
  if (!cJSON_IsObject(udp)) {
    ESP_LOGE(kTag, "mqtt hello missing udp section");
    return;
  }

  const cJSON *server = cJSON_GetObjectItem(udp, "server");
  const cJSON *port = cJSON_GetObjectItem(udp, "port");
  const cJSON *key = cJSON_GetObjectItem(udp, "key");
  const cJSON *nonce = cJSON_GetObjectItem(udp, "nonce");
  if (!cJSON_IsString(server) || !cJSON_IsNumber(port) || !cJSON_IsString(key) ||
      !cJSON_IsString(nonce)) {
    ESP_LOGE(kTag, "mqtt hello udp fields are invalid");
    return;
  }

  udp_server_ = server->valuestring;
  udp_port_ = port->valueint;
  aes_nonce_ = DecodeHexString(nonce->valuestring);
  const std::string aes_key = DecodeHexString(key->valuestring);
  if (aes_nonce_.size() != 16 || aes_key.size() != 16) {
    ESP_LOGE(kTag, "mqtt hello udp key/nonce length is invalid");
    return;
  }
  if (mbedtls_aes_setkey_enc(&aes_ctx_,
                             reinterpret_cast<const unsigned char *>(
                                 aes_key.data()),
                             128) != 0) {
    ESP_LOGE(kTag, "failed to set aes key");
    return;
  }

  local_sequence_ = 0;
  remote_sequence_ = 0;
  ResetUdpReceiveStatistics();
  ResetUdpAudioStallTracking(false);
  ESP_LOGI(kTag,
           "received mqtt server hello: session_id=%s sample_rate=%d "
           "frame_duration=%d",
           session_id_.empty() ? "<none>" : session_id_.c_str(),
           server_sample_rate_, server_frame_duration_);
  xEventGroupSetBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);
}

void MqttProtocol::ResetUdpReceiveStatistics() {
  udp_receive_stats_ = {};
  udp_gap_log_budget_ = 6;
  udp_old_log_budget_ = 4;
}

void MqttProtocol::ResetUdpAudioStallTracking(bool audio_active) {
  tts_downlink_active_.store(audio_active, std::memory_order_release);
  udp_audio_stall_detected_.store(false, std::memory_order_release);
  last_udp_audio_time_us_.store(esp_timer_get_time(), std::memory_order_release);
}

bool MqttProtocol::HasUdpAudioTimedOut(int64_t now_us) const {
  const int64_t last_audio_us =
      last_udp_audio_time_us_.load(std::memory_order_acquire);
  return last_audio_us > 0 && (now_us - last_audio_us) > kUdpAudioStallTimeoutUs;
}

void MqttProtocol::LogUdpReceiveStatistics(const char *reason) {
  if (udp_receive_stats_.packet_count == 0 && udp_receive_stats_.old_count == 0 &&
      udp_receive_stats_.gap_count == 0 &&
      udp_receive_stats_.timeout_count == 0 &&
      udp_receive_stats_.error_count == 0) {
    return;
  }
  ESP_LOGI(kTag,
           "udp receive stats (%s): packets=%lu old=%lu gap=%lu max_gap=%lu "
           "last_expected=%lu last_received=%lu timeouts=%lu errors=%lu",
           reason != nullptr ? reason : "<none>",
           static_cast<unsigned long>(udp_receive_stats_.packet_count),
           static_cast<unsigned long>(udp_receive_stats_.old_count),
           static_cast<unsigned long>(udp_receive_stats_.gap_count),
           static_cast<unsigned long>(udp_receive_stats_.max_gap),
           static_cast<unsigned long>(udp_receive_stats_.last_expected_sequence),
           static_cast<unsigned long>(udp_receive_stats_.last_received_sequence),
           static_cast<unsigned long>(udp_receive_stats_.timeout_count),
           static_cast<unsigned long>(udp_receive_stats_.error_count));
}

bool MqttProtocol::ConnectUdpSocket() {
  std::lock_guard<std::mutex> lock(channel_mutex_);
  DisconnectUdpSocket();

  struct hostent *server = gethostbyname(udp_server_.c_str());
  if (server == nullptr) {
    ESP_LOGE(kTag, "failed to resolve udp host: %s", udp_server_.c_str());
    return false;
  }

  struct sockaddr_in server_addr = {};
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(static_cast<uint16_t>(udp_port_));
  std::memcpy(&server_addr.sin_addr, server->h_addr, server->h_length);

  udp_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
  if (udp_socket_ < 0) {
    ESP_LOGE(kTag, "failed to create udp socket: errno=%d", errno);
    udp_socket_ = -1;
    return false;
  }

  const int recv_buffer_bytes = kUdpSocketRecvBufferBytes;
#if CONFIG_LWIP_SO_RCVBUF
  if (setsockopt(udp_socket_, SOL_SOCKET, SO_RCVBUF, &recv_buffer_bytes,
                 sizeof(recv_buffer_bytes)) < 0) {
    ESP_LOGW(kTag, "failed to set udp recv buffer: errno=%d", errno);
  }
#else
  ESP_LOGI(kTag,
           "skip udp recv buffer override because CONFIG_LWIP_SO_RCVBUF is disabled target=%d",
           recv_buffer_bytes);
#endif
  const struct timeval timeout = {
      .tv_sec = kUdpSocketTimeoutMs / 1000,
      .tv_usec = (kUdpSocketTimeoutMs % 1000) * 1000,
  };
  if (setsockopt(udp_socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                 sizeof(timeout)) < 0) {
    ESP_LOGW(kTag, "failed to set udp recv timeout: errno=%d", errno);
  }
  if (setsockopt(udp_socket_, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                 sizeof(timeout)) < 0) {
    ESP_LOGW(kTag, "failed to set udp send timeout: errno=%d", errno);
  }

  if (connect(udp_socket_, reinterpret_cast<struct sockaddr *>(&server_addr),
              sizeof(server_addr)) < 0) {
    ESP_LOGE(kTag, "failed to connect udp socket: errno=%d", errno);
    close(udp_socket_);
    udp_socket_ = -1;
    return false;
  }

  closing_udp_.store(false);
  udp_connected_.store(true);
  ResetUdpAudioStallTracking(false);
  xEventGroupClearBits(event_group_handle_, MQTT_PROTOCOL_RECEIVE_TASK_EXIT_EVENT);
  LogUdpTaskHeapState("before-create");
  if (xTaskCreate(&MqttProtocol::ReceiveTask, "mqtt_udp_rx",
                  kUdpReceiveTaskStackBytes, this,
                  kUdpReceiveTaskPriority, &receive_task_handle_) != pdPASS) {
    ESP_LOGE(kTag, "failed to create udp receive task");
    LogUdpTaskHeapState("create-failed");
    close(udp_socket_);
    udp_socket_ = -1;
    udp_connected_.store(false);
    return false;
  }
  ESP_LOGI(kTag,
           "udp socket ready recv_buf=%d rcvbuf_cfg=%d udp_recvmbox=%d timeout_ms=%d rx_task_prio=%u",
           recv_buffer_bytes, CONFIG_LWIP_SO_RCVBUF ? 1 : 0,
           CONFIG_LWIP_UDP_RECVMBOX_SIZE, kUdpSocketTimeoutMs,
           static_cast<unsigned>(kUdpReceiveTaskPriority));
  return true;
}

void MqttProtocol::DisconnectUdpSocket() {
  const bool had_channel = udp_connected_.load();
  if (had_channel) {
    LogUdpReceiveStatistics("channel-close");
  }
  ResetUdpAudioStallTracking(false);
  closing_udp_.store(true);
  udp_connected_.store(false);
  if (udp_socket_ >= 0) {
    close(udp_socket_);
    udp_socket_ = -1;
  }
  if (receive_task_handle_ != nullptr && event_group_handle_ != nullptr) {
    const EventBits_t bits = xEventGroupWaitBits(
        event_group_handle_, MQTT_PROTOCOL_RECEIVE_TASK_EXIT_EVENT, pdTRUE,
        pdFALSE, pdMS_TO_TICKS(1000));
    if ((bits & MQTT_PROTOCOL_RECEIVE_TASK_EXIT_EVENT) == 0) {
      ESP_LOGW(kTag, "udp receive task did not exit before timeout");
    }
    receive_task_handle_ = nullptr;
  }
  if (had_channel && on_audio_channel_closed_ != nullptr) {
    on_audio_channel_closed_();
  }
}

void MqttProtocol::ReceiveTask(void *arg) {
  auto *self = static_cast<MqttProtocol *>(arg);
  if (self != nullptr) {
    self->RunReceiveLoop();
    if (self->event_group_handle_ != nullptr) {
      xEventGroupSetBits(self->event_group_handle_,
                         MQTT_PROTOCOL_RECEIVE_TASK_EXIT_EVENT);
    }
    self->receive_task_handle_ = nullptr;
  }
  vTaskDelete(nullptr);
}

void MqttProtocol::RunReceiveLoop() {
  std::string data(1500, '\0');
  while (alive_.load() && udp_connected_.load()) {
    int ret = recv(udp_socket_, data.data(), data.size(), 0);
    if (ret <= 0) {
      if (ret < 0 &&
          (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT)) {
        udp_receive_stats_.timeout_count++;
        continue;
      }
      udp_receive_stats_.error_count++;
      ESP_LOGW(kTag, "udp receive loop exit ret=%d errno=%d", ret, errno);
      break;
    }
    udp_receive_stats_.packet_count++;
    if (static_cast<size_t>(ret) < aes_nonce_.size()) {
      udp_receive_stats_.error_count++;
      ESP_LOGE(kTag, "invalid udp packet size: %d", ret);
      continue;
    }

    const char *raw = data.data();
    if (static_cast<uint8_t>(raw[0]) != 0x01) {
      udp_receive_stats_.error_count++;
      ESP_LOGE(kTag, "invalid udp packet type: %u",
               static_cast<unsigned>(static_cast<uint8_t>(raw[0])));
      continue;
    }

    uint32_t timestamp = ntohl(*reinterpret_cast<const uint32_t *>(&raw[8]));
    uint32_t sequence = ntohl(*reinterpret_cast<const uint32_t *>(&raw[12]));
    if (sequence < remote_sequence_) {
      udp_receive_stats_.old_count++;
      udp_receive_stats_.last_expected_sequence = remote_sequence_;
      udp_receive_stats_.last_received_sequence = sequence;
      if (udp_old_log_budget_ > 0) {
        ESP_LOGW(kTag,
                 "received old udp sequence: %lu expected=%lu old_count=%lu",
                 static_cast<unsigned long>(sequence),
                 static_cast<unsigned long>(remote_sequence_),
                 static_cast<unsigned long>(udp_receive_stats_.old_count));
        udp_old_log_budget_--;
      }
      continue;
    }
    if (remote_sequence_ != 0 && sequence != remote_sequence_ + 1) {
      const uint32_t expected_sequence = remote_sequence_ + 1;
      const uint32_t gap =
          sequence > expected_sequence ? sequence - expected_sequence : 0;
      udp_receive_stats_.gap_count++;
      udp_receive_stats_.max_gap =
          std::max(udp_receive_stats_.max_gap, gap);
      udp_receive_stats_.last_expected_sequence = expected_sequence;
      udp_receive_stats_.last_received_sequence = sequence;
      ESP_LOGW(kTag,
               "received non-contiguous udp sequence: %lu expected=%lu gap=%lu gap_count=%lu max_gap=%lu",
               static_cast<unsigned long>(sequence),
               static_cast<unsigned long>(expected_sequence),
               static_cast<unsigned long>(gap),
               static_cast<unsigned long>(udp_receive_stats_.gap_count),
               static_cast<unsigned long>(udp_receive_stats_.max_gap));
    }

    const size_t encrypted_size = static_cast<size_t>(ret) - aes_nonce_.size();
    std::array<uint8_t, 16> nonce = {};
    std::memcpy(nonce.data(), raw, aes_nonce_.size());
    std::array<uint8_t, 16> stream_block = {};
    size_t nc_off = 0;

    auto packet = std::make_unique<AudioStreamPacket>();
    packet->sample_rate = server_sample_rate_;
    packet->frame_duration = server_frame_duration_;
    packet->timestamp = timestamp;
    packet->payload.resize(encrypted_size);
    if (mbedtls_aes_crypt_ctr(
            &aes_ctx_, encrypted_size, &nc_off, nonce.data(),
            stream_block.data(),
            reinterpret_cast<const uint8_t *>(raw + aes_nonce_.size()),
            packet->payload.data()) != 0) {
      udp_receive_stats_.error_count++;
      ESP_LOGE(kTag, "failed to decrypt udp audio");
      continue;
    }

    remote_sequence_ = sequence;
    last_incoming_time_ = std::chrono::steady_clock::now();
    last_udp_audio_time_us_.store(esp_timer_get_time(), std::memory_order_release);
    udp_audio_stall_detected_.store(false, std::memory_order_release);
    if (on_incoming_audio_ != nullptr) {
      on_incoming_audio_(std::move(packet));
    }
  }

  const bool unexpected_close = !closing_udp_.load() && alive_.load();
  udp_connected_.store(false);
  LogUdpReceiveStatistics(unexpected_close ? "unexpected-close" : "receive-loop-exit");
  if (unexpected_close) {
    SetError("udp receive failed");
  }
}

bool MqttProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
  std::lock_guard<std::mutex> lock(channel_mutex_);
  if (!IsAudioChannelOpened() || packet == nullptr || udp_socket_ < 0) {
    return false;
  }

  std::string nonce = aes_nonce_;
  *reinterpret_cast<uint16_t *>(&nonce[2]) =
      htons(static_cast<uint16_t>(packet->payload.size()));
  *reinterpret_cast<uint32_t *>(&nonce[8]) = htonl(packet->timestamp);
  *reinterpret_cast<uint32_t *>(&nonce[12]) = htonl(++local_sequence_);

  std::string encrypted(aes_nonce_.size() + packet->payload.size(), '\0');
  std::memcpy(encrypted.data(), nonce.data(), nonce.size());
  std::array<uint8_t, 16> stream_block = {};
  size_t nc_off = 0;
  if (mbedtls_aes_crypt_ctr(
          &aes_ctx_, packet->payload.size(), &nc_off,
          reinterpret_cast<uint8_t *>(nonce.data()), stream_block.data(),
          packet->payload.data(),
          reinterpret_cast<uint8_t *>(encrypted.data() + nonce.size())) != 0) {
    ESP_LOGE(kTag, "failed to encrypt udp audio");
    return false;
  }

  int ret = send(udp_socket_, encrypted.data(), encrypted.size(), 0);
  if (ret <= 0) {
    ESP_LOGE(kTag, "failed to send udp audio: ret=%d errno=%d", ret, errno);
    return false;
  }
  return true;
}

void MqttProtocol::CloseAudioChannel(bool send_goodbye) {
  DisconnectUdpSocket();
  if (send_goodbye && mqtt_connected_.load() && !session_id_.empty()) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", session_id_.c_str());
    cJSON_AddStringToObject(root, "type", "goodbye");
    char *json_str = cJSON_PrintUnformatted(root);
    const std::string message = json_str != nullptr ? json_str : "";
    if (json_str != nullptr) {
      cJSON_free(json_str);
    }
    cJSON_Delete(root);
    SendText(message);
  }
}

bool MqttProtocol::IsAudioChannelOpened() const {
  return udp_connected_.load() && udp_socket_ >= 0 && !error_occurred_ &&
         !IsTimeout();
}

std::string MqttProtocol::DecodeHexString(const std::string &hex_string) const {
  if ((hex_string.size() % 2) != 0) {
    return {};
  }

  auto char_to_hex = [](char value) -> uint8_t {
    if (value >= '0' && value <= '9') {
      return static_cast<uint8_t>(value - '0');
    }
    if (value >= 'A' && value <= 'F') {
      return static_cast<uint8_t>(value - 'A' + 10);
    }
    if (value >= 'a' && value <= 'f') {
      return static_cast<uint8_t>(value - 'a' + 10);
    }
    return 0;
  };

  std::string decoded;
  decoded.reserve(hex_string.size() / 2);
  for (size_t i = 0; i < hex_string.size(); i += 2) {
    decoded.push_back(static_cast<char>((char_to_hex(hex_string[i]) << 4) |
                                        char_to_hex(hex_string[i + 1])));
  }
  return decoded;
}

}  // namespace official_chat
