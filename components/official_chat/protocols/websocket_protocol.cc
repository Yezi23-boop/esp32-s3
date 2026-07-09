#include "protocols/websocket_protocol.h"

#include <arpa/inet.h>
#include <cstring>

#include <esp_log.h>

#include "system_util.h"

namespace official_chat {

namespace {

constexpr char kTag[] = "official_ws";

}  // namespace

WebsocketProtocol::WebsocketProtocol(std::string url, std::string token,
                                     int version)
    : url_(std::move(url)), token_(std::move(token)), version_(version) {
  event_group_handle_ = xEventGroupCreate();
}

WebsocketProtocol::~WebsocketProtocol() {
  websocket_.reset();
  if (event_group_handle_ != nullptr) {
    vEventGroupDelete(event_group_handle_);
  }
}

bool WebsocketProtocol::Start() {
  return true;
}

bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
  if (!IsAudioChannelOpened() || packet == nullptr) {
    return false;
  }

  if (version_ == 2) {
    std::string serialized;
    serialized.resize(sizeof(BinaryProtocol2) + packet->payload.size());
    auto *bp2 = reinterpret_cast<BinaryProtocol2 *>(serialized.data());
    bp2->version = htons(static_cast<uint16_t>(version_));
    bp2->type = 0;
    bp2->reserved = 0;
    bp2->timestamp = htonl(packet->timestamp);
    bp2->payload_size = htonl(static_cast<uint32_t>(packet->payload.size()));
    memcpy(bp2->payload, packet->payload.data(), packet->payload.size());
    const bool sent = websocket_->Send(serialized.data(), serialized.size(), true);
    ESP_LOGI(kTag, "send audio packet bytes=%u ok=%d timestamp=%lu",
             static_cast<unsigned>(packet->payload.size()), sent ? 1 : 0,
             static_cast<unsigned long>(packet->timestamp));
    return sent;
  }

  if (version_ == 3) {
    std::string serialized;
    serialized.resize(sizeof(BinaryProtocol3) + packet->payload.size());
    auto *bp3 = reinterpret_cast<BinaryProtocol3 *>(serialized.data());
    bp3->type = 0;
    bp3->reserved = 0;
    bp3->payload_size = htons(static_cast<uint16_t>(packet->payload.size()));
    memcpy(bp3->payload, packet->payload.data(), packet->payload.size());
    const bool sent = websocket_->Send(serialized.data(), serialized.size(), true);
    ESP_LOGI(kTag, "send audio packet bytes=%u ok=%d timestamp=%lu",
             static_cast<unsigned>(packet->payload.size()), sent ? 1 : 0,
             static_cast<unsigned long>(packet->timestamp));
    return sent;
  }

  const bool sent = websocket_->Send(
      reinterpret_cast<const char *>(packet->payload.data()),
      packet->payload.size(), true);
  ESP_LOGI(kTag, "send audio packet bytes=%u ok=%d timestamp=%lu",
           static_cast<unsigned>(packet->payload.size()), sent ? 1 : 0,
           static_cast<unsigned long>(packet->timestamp));
  return sent;
}

bool WebsocketProtocol::SendText(const std::string &text) {
  if (!IsAudioChannelOpened()) {
    return false;
  }
  if (!websocket_->Send(text)) {
    ESP_LOGE(kTag, "failed to send text payload");
    SetError("failed to send websocket text");
    return false;
  }
  return true;
}

bool WebsocketProtocol::OpenAudioChannel() {
  if (url_.empty()) {
    SetError("missing websocket url");
    return false;
  }

  ESP_LOGI(kTag, "opening websocket audio channel");
  error_occurred_ = false;
  xEventGroupClearBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);

  websocket_ = std::make_unique<WebsocketClient>();
  if (websocket_ == nullptr) {
    SetError("failed to allocate websocket client");
    return false;
  }

  if (!token_.empty()) {
    std::string auth = token_;
    if (auth.find(' ') == std::string::npos) {
      auth = "Bearer " + auth;
    }
    websocket_->SetHeader("Authorization", auth.c_str());
  }
  websocket_->SetHeader("Protocol-Version", std::to_string(version_).c_str());
  websocket_->SetHeader("Device-Id", get_mac_address());
  websocket_->SetHeader("Client-Id", generate_uuid());

  websocket_->OnConnected([this]() {
    if (on_connected_) {
      on_connected_();
    }
  });
  websocket_->OnDisconnected([this]() {
    if (on_disconnected_) {
      on_disconnected_();
    }
    if (on_audio_channel_closed_) {
      on_audio_channel_closed_();
    }
  });
  websocket_->OnError([this](int error_code) {
    ESP_LOGE(kTag, "websocket error: %d", error_code);
    SetError("websocket transport error");
  });
  websocket_->OnData([this](const char *data, size_t len, bool binary) {
    if (binary) {
      if (on_incoming_audio_ == nullptr || data == nullptr || len == 0) {
        return;
      }
      if (version_ == 2 && len >= sizeof(BinaryProtocol2)) {
        auto *bp2 = (BinaryProtocol2 *)data;
        const uint32_t payload_size = ntohl(bp2->payload_size);
        auto *payload = bp2->payload;
        on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
            .sample_rate = server_sample_rate_,
            .frame_duration = server_frame_duration_,
            .timestamp = ntohl(bp2->timestamp),
            .payload = std::vector<uint8_t>(payload, payload + payload_size),
        }));
      } else if (version_ == 3 && len >= sizeof(BinaryProtocol3)) {
        auto *bp3 = (BinaryProtocol3 *)data;
        const uint16_t payload_size = ntohs(bp3->payload_size);
        auto *payload = bp3->payload;
        on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
            .sample_rate = server_sample_rate_,
            .frame_duration = server_frame_duration_,
            .timestamp = 0,
            .payload = std::vector<uint8_t>(payload, payload + payload_size),
        }));
      } else {
        on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
            .sample_rate = server_sample_rate_,
            .frame_duration = server_frame_duration_,
            .timestamp = 0,
            .payload =
                std::vector<uint8_t>((const uint8_t *)data,
                                     (const uint8_t *)data + len),
        }));
      }
    } else {
      cJSON *root = cJSON_ParseWithLength(data, len);
      if (root == nullptr) {
        ESP_LOGE(kTag, "failed to parse incoming websocket json");
        return;
      }
      cJSON *type = cJSON_GetObjectItem(root, "type");
      if (cJSON_IsString(type)) {
        if (strcmp(type->valuestring, "hello") == 0) {
          ParseServerHello(root);
        } else {
          ESP_LOGI(kTag, "received websocket json type: %s", type->valuestring);
          if (on_incoming_json_) {
            on_incoming_json_(root);
          }
        }
      } else {
        ESP_LOGW(kTag, "received websocket json without type field");
      }
      cJSON_Delete(root);
    }
    last_incoming_time_ = std::chrono::steady_clock::now();
  });

  ESP_LOGI(kTag, "connecting to websocket server: %s with version: %d",
           url_.c_str(), version_);
  if (!websocket_->Connect(url_.c_str())) {
    ESP_LOGE(kTag, "failed to connect websocket, err=%d",
             websocket_->GetLastError());
    SetError("websocket connect failed");
    return false;
  }

  std::string hello = GetHelloMessage();
  ESP_LOGI(kTag, "sending websocket hello");
  if (!SendText(hello)) {
    return false;
  }

  ESP_LOGI(kTag, "waiting for websocket server hello");
  const EventBits_t bits = xEventGroupWaitBits(
      event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE,
      pdFALSE, pdMS_TO_TICKS(10000));
  if ((bits & WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT) == 0) {
    ESP_LOGE(kTag,
             "failed to receive server hello, connected=%d last_error=%d",
             websocket_ != nullptr && websocket_->IsConnected(),
             websocket_ != nullptr ? websocket_->GetLastError() : -1);
    SetError("server hello timeout");
    return false;
  }

  if (on_audio_channel_opened_) {
    on_audio_channel_opened_();
  }
  return true;
}

void WebsocketProtocol::CloseAudioChannel(bool send_goodbye) {
  (void)send_goodbye;
  if (websocket_ != nullptr) {
    websocket_->Close();
    websocket_.reset();
  }
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
  return websocket_ != nullptr && websocket_->IsConnected() &&
         !error_occurred_ && !IsTimeout();
}

std::string WebsocketProtocol::GetHelloMessage() {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "type", "hello");
  cJSON_AddNumberToObject(root, "version", version_);
  cJSON_AddStringToObject(root, "transport", "websocket");
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

void WebsocketProtocol::ParseServerHello(const cJSON *root) {
  const cJSON *transport = cJSON_GetObjectItem(root, "transport");
  if (!cJSON_IsString(transport) ||
      strcmp(transport->valuestring, "websocket") != 0) {
    ESP_LOGE(kTag, "unsupported transport in server hello");
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

  ESP_LOGI(kTag,
           "received websocket server hello: session_id=%s sample_rate=%d "
           "frame_duration=%d",
           session_id_.empty() ? "<none>" : session_id_.c_str(),
           server_sample_rate_, server_frame_duration_);
  xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}

}  // namespace official_chat
