#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

namespace official_chat {

class Tcp;

class WebsocketClient {
 public:
  using ConnectedCallback = std::function<void()>;
  using DisconnectedCallback = std::function<void()>;
  using DataCallback =
      std::function<void(const char *data, size_t len, bool binary)>;
  using ErrorCallback = std::function<void(int error_code)>;

  WebsocketClient();
  ~WebsocketClient();

  void SetHeader(const char *key, const char *value);
  void OnConnected(ConnectedCallback callback);
  void OnDisconnected(DisconnectedCallback callback);
  void OnData(DataCallback callback);
  void OnError(ErrorCallback callback);

  bool Connect(const char *url);
  bool Send(const char *data, size_t len, bool binary);
  bool Send(const std::string &text);
  void Close();

  bool IsConnected() const;
  int GetLastError() const;

 private:
  void OnTcpData(const std::string &data);
  void ResetClient();
  bool SendControlFrame(uint8_t opcode, const void *data, size_t len);

  std::unique_ptr<Tcp> tcp_;
  EventGroupHandle_t connect_events_ = nullptr;
  std::map<std::string, std::string> headers_;
  std::string receive_buffer_;
  std::vector<char> current_message_;
  std::mutex send_mutex_;
  ConnectedCallback on_connected_;
  DisconnectedCallback on_disconnected_;
  DataCallback on_data_;
  ErrorCallback on_error_;
  bool connected_ = false;
  bool handshake_completed_ = false;
  bool is_fragmented_ = false;
  bool is_binary_ = false;
  bool local_close_in_progress_ = false;
  bool shutting_down_ = false;
  int last_error_ = 0;
};

}  // namespace official_chat
