#include "net/websocket_client.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <esp_log.h>

#include "net/esp_ssl.h"
#include "net/esp_tcp.h"

namespace official_chat {

namespace {

constexpr EventBits_t kHandshakeOkBit = BIT0;
constexpr EventBits_t kHandshakeFailBit = BIT1;
constexpr TickType_t kHandshakeTimeoutTicks = pdMS_TO_TICKS(10000);
constexpr char kTag[] = "official_ws_client";

struct ParsedUri {
  bool secure = false;
  std::string host;
  int port = 0;
  std::string path = "/";
};

std::string Base64Encode(const unsigned char *data, size_t len) {
  constexpr char kBase64Chars[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string encoded;
  unsigned char char_array_3[3];
  unsigned char char_array_4[4];

  size_t i = 0;
  while (i < len) {
    const size_t chunk_size = std::min(static_cast<size_t>(3), len - i);
    for (size_t j = 0; j < 3; ++j) {
      char_array_3[j] = (j < chunk_size) ? data[i + j] : 0;
    }

    char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
    char_array_4[1] =
        ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
    char_array_4[2] =
        ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
    char_array_4[3] = char_array_3[2] & 0x3f;

    for (size_t j = 0; j < 4; ++j) {
      encoded.push_back(j <= chunk_size ? kBase64Chars[char_array_4[j]] : '=');
    }

    i += chunk_size;
  }
  return encoded;
}

std::string GenerateWebsocketKey() {
  unsigned char key[16];
  for (auto &byte : key) {
    byte = static_cast<unsigned char>(rand() & 0xFF);
  }
  return Base64Encode(key, sizeof(key));
}

bool ParseUri(const char *url, ParsedUri *parsed) {
  if (url == nullptr || parsed == nullptr) {
    return false;
  }

  std::string uri(url);
  const size_t scheme_pos = uri.find("://");
  if (scheme_pos == std::string::npos) {
    return false;
  }

  const std::string scheme = uri.substr(0, scheme_pos);
  parsed->secure = (scheme == "wss" || scheme == "https");
  if (scheme != "ws" && scheme != "wss" && scheme != "http" &&
      scheme != "https") {
    return false;
  }

  size_t pos = scheme_pos + 3;
  size_t path_pos = uri.find('/', pos);
  std::string host_port;
  if (path_pos == std::string::npos) {
    host_port = uri.substr(pos);
    parsed->path = "/";
  } else {
    host_port = uri.substr(pos, path_pos - pos);
    parsed->path = uri.substr(path_pos);
  }

  const size_t colon_pos = host_port.find(':');
  if (colon_pos == std::string::npos) {
    parsed->host = host_port;
    parsed->port = parsed->secure ? 443 : 80;
  } else {
    parsed->host = host_port.substr(0, colon_pos);
    parsed->port = std::stoi(host_port.substr(colon_pos + 1));
  }

  return !parsed->host.empty();
}

}  // namespace

WebsocketClient::WebsocketClient() {
  connect_events_ = xEventGroupCreate();
}

WebsocketClient::~WebsocketClient() {
  ResetClient();
  if (connect_events_ != nullptr) {
    vEventGroupDelete(connect_events_);
    connect_events_ = nullptr;
  }
}

void WebsocketClient::SetHeader(const char *key, const char *value) {
  if (key == nullptr || value == nullptr) {
    return;
  }
  headers_[key] = value;
}

void WebsocketClient::OnConnected(ConnectedCallback callback) {
  on_connected_ = std::move(callback);
}

void WebsocketClient::OnDisconnected(DisconnectedCallback callback) {
  on_disconnected_ = std::move(callback);
}

void WebsocketClient::OnData(DataCallback callback) {
  on_data_ = std::move(callback);
}

void WebsocketClient::OnError(ErrorCallback callback) {
  on_error_ = std::move(callback);
}

bool WebsocketClient::Connect(const char *url) {
  if (connect_events_ == nullptr || url == nullptr) {
    return false;
  }

  ResetClient();
  xEventGroupClearBits(connect_events_, kHandshakeOkBit | kHandshakeFailBit);

  ParsedUri parsed;
  if (!ParseUri(url, &parsed)) {
    last_error_ = -1;
    ESP_LOGE(kTag, "invalid websocket url: %s", url);
    return false;
  }

  if (parsed.secure) {
    tcp_ = std::make_unique<EspSsl>();
  } else {
    tcp_ = std::make_unique<EspTcp>();
  }
  if (tcp_ == nullptr) {
    last_error_ = -1;
    ESP_LOGE(kTag, "failed to create tcp transport");
    return false;
  }

  tcp_->OnStream([this](const std::string &data) { OnTcpData(data); });
  tcp_->OnDisconnected([this]() {
    if (shutting_down_) {
      return;
    }
    if (!connected_) {
      xEventGroupSetBits(connect_events_, kHandshakeFailBit);
      return;
    }
    connected_ = false;
    handshake_completed_ = false;
    if (!local_close_in_progress_) {
      ESP_LOGW(kTag, "websocket disconnected");
    }
    if (!local_close_in_progress_ && on_disconnected_) {
      on_disconnected_();
    }
  });

  if (!tcp_->Connect(parsed.host, parsed.port)) {
    last_error_ = tcp_->GetLastError();
    ESP_LOGE(kTag, "websocket event error: %d", last_error_);
    if (on_error_) {
      on_error_(last_error_);
    }
    ResetClient();
    return false;
  }

  headers_["Upgrade"] = "websocket";
  headers_["Connection"] = "Upgrade";
  headers_["Sec-WebSocket-Version"] = "13";
  headers_["Sec-WebSocket-Key"] = GenerateWebsocketKey();

  std::string request = "GET " + parsed.path + " HTTP/1.1\r\n";
  if (headers_.find("Host") == headers_.end()) {
    request += "Host: " + parsed.host + "\r\n";
  }
  for (const auto &header : headers_) {
    request += header.first + ": " + header.second + "\r\n";
  }
  request += "\r\n";

  if (tcp_->Send(request) < 0) {
    last_error_ = tcp_->GetLastError();
    ESP_LOGE(kTag, "websocket event error: %d", last_error_);
    if (on_error_) {
      on_error_(last_error_);
    }
    ResetClient();
    return false;
  }

  ESP_LOGI(kTag, "waiting for websocket connected event");
  const EventBits_t bits =
      xEventGroupWaitBits(connect_events_, kHandshakeOkBit | kHandshakeFailBit,
                          pdTRUE, pdFALSE, kHandshakeTimeoutTicks);
  if ((bits & kHandshakeOkBit) != 0) {
    return true;
  }

  ESP_LOGE(kTag, "websocket connect timeout or failed, last_error=%d",
           last_error_);
  ResetClient();
  return false;
}

bool WebsocketClient::Send(const char *data, size_t len, bool binary) {
  if (!connected_ || tcp_ == nullptr || data == nullptr || len == 0) {
    return false;
  }
  if (len > 65535) {
    ESP_LOGE(kTag, "websocket payload too large: %u",
             static_cast<unsigned>(len));
    return false;
  }

  std::string frame;
  frame.reserve(len + 8);
  frame.push_back(static_cast<char>(0x80 | (binary ? 0x02 : 0x01)));

  if (len < 126) {
    frame.push_back(static_cast<char>(0x80 | len));
  } else {
    frame.push_back(static_cast<char>(0x80 | 126));
    frame.push_back(static_cast<char>((len >> 8) & 0xFF));
    frame.push_back(static_cast<char>(len & 0xFF));
  }

  uint8_t mask[4];
  for (auto &byte : mask) {
    byte = static_cast<uint8_t>(rand() & 0xFF);
  }
  frame.append(reinterpret_cast<const char *>(mask), sizeof(mask));

  for (size_t i = 0; i < len; ++i) {
    frame.push_back(static_cast<char>(data[i] ^ mask[i % sizeof(mask)]));
  }

  std::lock_guard<std::mutex> lock(send_mutex_);
  return tcp_->Send(frame) >= 0;
}

bool WebsocketClient::Send(const std::string &text) {
  return Send(text.data(), text.size(), false);
}

bool WebsocketClient::IsConnected() const {
  return connected_;
}

int WebsocketClient::GetLastError() const {
  return last_error_;
}

void WebsocketClient::Close() {
  if (tcp_ == nullptr) {
    ResetClient();
    return;
  }

  local_close_in_progress_ = true;
  ESP_LOGI(kTag, "closing websocket locally");
  tcp_->SetLocalCloseInProgress(true);
  if (connected_) {
    (void)SendControlFrame(0x8, nullptr, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  ResetClient();
}

void WebsocketClient::OnTcpData(const std::string &data) {
  receive_buffer_ += data;

  if (!handshake_completed_) {
    const size_t header_end = receive_buffer_.find("\r\n\r\n");
    if (header_end == std::string::npos) {
      return;
    }

    const std::string handshake_response =
        receive_buffer_.substr(0, header_end + 4);
    receive_buffer_.erase(0, header_end + 4);

    if (handshake_response.find("HTTP/1.1 101") == std::string::npos &&
        handshake_response.find("HTTP/1.0 101") == std::string::npos) {
      last_error_ = -1;
      ESP_LOGE(kTag, "websocket event error: %d", last_error_);
      if (on_error_) {
        on_error_(last_error_);
      }
      xEventGroupSetBits(connect_events_, kHandshakeFailBit);
      return;
    }

    handshake_completed_ = true;
    connected_ = true;
    ESP_LOGI(kTag, "websocket connected");
    xEventGroupSetBits(connect_events_, kHandshakeOkBit);
    if (on_connected_) {
      on_connected_();
    }
  }

  size_t buffer_offset = 0;
  const auto *buffer =
      reinterpret_cast<const uint8_t *>(receive_buffer_.data());
  const size_t buffer_size = receive_buffer_.size();

  while (buffer_offset < buffer_size) {
    if ((buffer_size - buffer_offset) < 2) {
      break;
    }

    const uint8_t opcode = buffer[buffer_offset] & 0x0F;
    const bool fin = (buffer[buffer_offset] & 0x80) != 0;
    const bool masked = (buffer[buffer_offset + 1] & 0x80) != 0;
    uint64_t payload_length = buffer[buffer_offset + 1] & 0x7F;

    size_t header_length = 2;
    if (payload_length == 126) {
      if ((buffer_size - buffer_offset) < 4) {
        break;
      }
      payload_length = (static_cast<uint64_t>(buffer[buffer_offset + 2]) << 8) |
                       buffer[buffer_offset + 3];
      header_length += 2;
    } else if (payload_length == 127) {
      if ((buffer_size - buffer_offset) < 10) {
        break;
      }
      payload_length = 0;
      for (int i = 0; i < 8; ++i) {
        payload_length =
            (payload_length << 8) | buffer[buffer_offset + 2 + i];
      }
      header_length += 8;
    }

    uint8_t mask_key[4] = {0};
    if (masked) {
      if ((buffer_size - buffer_offset) < (header_length + 4)) {
        break;
      }
      memcpy(mask_key, buffer + buffer_offset + header_length, sizeof(mask_key));
      header_length += sizeof(mask_key);
    }

    if ((buffer_size - buffer_offset) < (header_length + payload_length)) {
      break;
    }

    std::vector<char> payload(static_cast<size_t>(payload_length));
    if (payload_length > 0) {
      memcpy(payload.data(), buffer + buffer_offset + header_length,
             static_cast<size_t>(payload_length));
      if (masked) {
        for (size_t i = 0; i < payload.size(); ++i) {
          payload[i] ^= static_cast<char>(mask_key[i % sizeof(mask_key)]);
        }
      }
    }

    switch (opcode) {
      case 0x0:
      case 0x1:
      case 0x2:
        if (opcode != 0x0 && is_fragmented_) {
          ESP_LOGW(kTag, "received new websocket frame while fragmenting");
          break;
        }
        if (opcode != 0x0) {
          is_fragmented_ = !fin;
          is_binary_ = (opcode == 0x2);
          current_message_.clear();
        }
        current_message_.insert(current_message_.end(), payload.begin(),
                                payload.end());
        if (fin) {
          if (on_data_ && !current_message_.empty()) {
            on_data_(current_message_.data(), current_message_.size(),
                     is_binary_);
          }
          current_message_.clear();
          is_fragmented_ = false;
        }
        break;
      case 0x8:
        connected_ = false;
        handshake_completed_ = false;
        if (local_close_in_progress_) {
          ESP_LOGI(kTag, "websocket close acknowledged");
        } else {
          ESP_LOGW(kTag, "websocket disconnected");
        }
        if (!shutting_down_ && !local_close_in_progress_ && on_disconnected_) {
          on_disconnected_();
        }
        break;
      case 0x9:
        SendControlFrame(0xA, payload.data(), payload.size());
        break;
      case 0xA:
        break;
      default:
        ESP_LOGW(kTag, "unknown websocket opcode: %d", opcode);
        break;
    }

    buffer_offset += header_length + static_cast<size_t>(payload_length);
  }

  if (buffer_offset > 0) {
    receive_buffer_.erase(0, buffer_offset);
  }
}

void WebsocketClient::ResetClient() {
  shutting_down_ = true;
  connected_ = false;
  handshake_completed_ = false;
  current_message_.clear();
  receive_buffer_.clear();
  is_fragmented_ = false;
  is_binary_ = false;
  if (tcp_ != nullptr) {
    tcp_->SetLocalCloseInProgress(local_close_in_progress_);
    tcp_->Disconnect();
    tcp_.reset();
  }
  local_close_in_progress_ = false;
  shutting_down_ = false;
}

bool WebsocketClient::SendControlFrame(uint8_t opcode, const void *data,
                                       size_t len) {
  if (!connected_ || tcp_ == nullptr || len > 125) {
    return false;
  }

  std::string frame;
  frame.reserve(len + 6);
  frame.push_back(static_cast<char>(0x80 | opcode));
  frame.push_back(static_cast<char>(0x80 | len));

  uint8_t mask[4];
  for (auto &byte : mask) {
    byte = static_cast<uint8_t>(rand() & 0xFF);
  }
  frame.append(reinterpret_cast<const char *>(mask), sizeof(mask));

  const auto *payload = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < len; ++i) {
    frame.push_back(static_cast<char>(payload[i] ^ mask[i % sizeof(mask)]));
  }

  std::lock_guard<std::mutex> lock(send_mutex_);
  return tcp_->Send(frame) >= 0;
}

}  // namespace official_chat
