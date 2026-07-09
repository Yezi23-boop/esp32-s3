#include "protocols/protocol.h"

#include <esp_log.h>

namespace official_chat {

namespace {

constexpr char kTag[] = "official_protocol";

}  // namespace

void Protocol::OnIncomingJson(std::function<void(const cJSON *root)> callback) {
  on_incoming_json_ = std::move(callback);
}

void Protocol::OnIncomingAudio(
    std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback) {
  on_incoming_audio_ = std::move(callback);
}

void Protocol::OnAudioChannelOpened(std::function<void()> callback) {
  on_audio_channel_opened_ = std::move(callback);
}

void Protocol::OnAudioChannelClosed(std::function<void()> callback) {
  on_audio_channel_closed_ = std::move(callback);
}

void Protocol::OnNetworkError(
    std::function<void(const std::string &message)> callback) {
  on_network_error_ = std::move(callback);
}

void Protocol::OnConnected(std::function<void()> callback) {
  on_connected_ = std::move(callback);
}

void Protocol::OnDisconnected(std::function<void()> callback) {
  on_disconnected_ = std::move(callback);
}

void Protocol::SetError(const std::string &message) {
  error_occurred_ = true;
  if (on_network_error_ != nullptr) {
    on_network_error_(message);
  }
}

void Protocol::SendAbortSpeaking(AbortReason reason) {
  std::string message = "{\"session_id\":\"" + session_id_ +
                        "\",\"type\":\"abort\"";
  if (reason == kAbortReasonWakeWordDetected) {
    message += ",\"reason\":\"wake_word_detected\"";
  }
  message += "}";
  ESP_LOGI(kTag, "send abort speaking reason=%d bytes=%u",
           static_cast<int>(reason), static_cast<unsigned>(message.size()));
  SendText(message);
}

void Protocol::SendWakeWordDetected(const std::string &wake_word) {
  std::string message =
      "{\"session_id\":\"" + session_id_ +
      "\",\"type\":\"listen\",\"state\":\"detect\",\"text\":\"" + wake_word +
      "\"}";
  SendText(message);
}

void Protocol::SendStartListening(ListeningMode mode) {
  std::string message =
      "{\"session_id\":\"" + session_id_ + "\",\"type\":\"listen\",\"state\":\"start\"";
  if (mode == kListeningModeRealtime) {
    message += ",\"mode\":\"realtime\"";
  } else if (mode == kListeningModeAutoStop) {
    message += ",\"mode\":\"auto\"";
  } else {
    message += ",\"mode\":\"manual\"";
  }
  message += "}";
  const char *mode_name = mode == kListeningModeRealtime
                              ? "realtime"
                              : (mode == kListeningModeAutoStop ? "auto"
                                                                : "manual");
  ESP_LOGI(kTag, "send listen/start mode=%s bytes=%u", mode_name,
           static_cast<unsigned>(message.size()));
  SendText(message);
}

void Protocol::SendStopListening() {
  const std::string message =
      "{\"session_id\":\"" + session_id_ +
      "\",\"type\":\"listen\",\"state\":\"stop\"}";
  ESP_LOGI(kTag, "send listen/stop bytes=%u",
           static_cast<unsigned>(message.size()));
  SendText(message);
}

void Protocol::SendMcpMessage(const std::string &message) {
  const std::string envelope =
      "{\"session_id\":\"" + session_id_ +
      "\",\"type\":\"mcp\",\"payload\":" + message + "}";
  ESP_LOGI(kTag, "send mcp message bytes=%u",
           static_cast<unsigned>(envelope.size()));
  SendText(envelope);
}

bool Protocol::IsTimeout() const {
  constexpr int kTimeoutSeconds = 120;
  const auto now = std::chrono::steady_clock::now();
  const auto duration =
      std::chrono::duration_cast<std::chrono::seconds>(now -
                                                       last_incoming_time_);
  const bool timeout = duration.count() > kTimeoutSeconds;

  if (timeout) {
    ESP_LOGE(kTag, "channel timeout %ld seconds",
             static_cast<long>(duration.count()));
  }
  return timeout;
}

}  // namespace official_chat
