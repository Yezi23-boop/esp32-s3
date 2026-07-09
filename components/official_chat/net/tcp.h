#pragma once

#include <functional>
#include <string>

namespace official_chat {

class Tcp {
 public:
  virtual ~Tcp() = default;

  virtual bool Connect(const std::string &host, int port) = 0;
  virtual void Disconnect() = 0;
  virtual int Send(const std::string &data) = 0;
  virtual int GetLastError() = 0;
  virtual void SetLocalCloseInProgress(bool enable) {
    (void)enable;
  }

  virtual void OnStream(std::function<void(const std::string &data)> callback) {
    stream_callback_ = std::move(callback);
  }

  virtual void OnDisconnected(std::function<void()> callback) {
    disconnect_callback_ = std::move(callback);
  }

  bool connected() const { return connected_; }

 protected:
  std::function<void(const std::string &data)> stream_callback_;
  std::function<void()> disconnect_callback_;
  bool connected_ = false;
};

}  // namespace official_chat
