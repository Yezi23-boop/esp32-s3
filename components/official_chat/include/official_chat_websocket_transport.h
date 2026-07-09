#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace official_chat {

/**
 * @brief official_chat 对外暴露的窄 WebSocket transport。
 *
 * 该类只包装底层 WebSocket 连接、文本/二进制发送和事件回调，不包含小智 AI
 * 的唤醒词、MCP、TTS 或业务协议语义。外部 owner 可以复用传输能力，但不能依赖
 * `net/` 私有实现细节。
 */
class WebsocketTransport {
 public:
  using ConnectedCallback = std::function<void()>;
  using DisconnectedCallback = std::function<void()>;
  using DataCallback = std::function<void(const char *data, size_t len,
                                          bool binary)>;
  using ErrorCallback = std::function<void(int error_code)>;

  WebsocketTransport();
  ~WebsocketTransport();

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
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace official_chat
