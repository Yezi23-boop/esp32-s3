#include "official_chat_websocket_transport.h"

#include <utility>

#include "net/websocket_client.h"

namespace official_chat {

class WebsocketTransport::Impl {
 public:
  WebsocketClient client;
};

WebsocketTransport::WebsocketTransport() : impl_(std::make_unique<Impl>()) {}

WebsocketTransport::~WebsocketTransport() = default;

void WebsocketTransport::SetHeader(const char *key, const char *value) {
  impl_->client.SetHeader(key, value);
}

void WebsocketTransport::OnConnected(ConnectedCallback callback) {
  impl_->client.OnConnected(std::move(callback));
}

void WebsocketTransport::OnDisconnected(DisconnectedCallback callback) {
  impl_->client.OnDisconnected(std::move(callback));
}

void WebsocketTransport::OnData(DataCallback callback) {
  impl_->client.OnData(std::move(callback));
}

void WebsocketTransport::OnError(ErrorCallback callback) {
  impl_->client.OnError(std::move(callback));
}

bool WebsocketTransport::Connect(const char *url) {
  return impl_->client.Connect(url);
}

bool WebsocketTransport::Send(const char *data, size_t len, bool binary) {
  return impl_->client.Send(data, len, binary);
}

bool WebsocketTransport::Send(const std::string &text) {
  return impl_->client.Send(text);
}

void WebsocketTransport::Close() {
  impl_->client.Close();
}

bool WebsocketTransport::IsConnected() const {
  return impl_->client.IsConnected();
}

int WebsocketTransport::GetLastError() const {
  return impl_->client.GetLastError();
}

}  // namespace official_chat
