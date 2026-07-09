#pragma once

#include <memory>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include "net/websocket_client.h"
#include "protocols/protocol.h"

namespace official_chat {

constexpr EventBits_t WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT = BIT0;

class WebsocketProtocol : public Protocol {
 public:
  explicit WebsocketProtocol(std::string url = "", std::string token = "",
                             int version = 2);
  ~WebsocketProtocol() override;

  bool Start() override;
  bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
  bool OpenAudioChannel() override;
  void CloseAudioChannel(bool send_goodbye = true) override;
  bool IsAudioChannelOpened() const override;

 private:
  void ParseServerHello(const cJSON *root);
  bool SendText(const std::string &text) override;
  std::string GetHelloMessage();

  EventGroupHandle_t event_group_handle_ = nullptr;
  std::unique_ptr<WebsocketClient> websocket_;
  std::string url_;
  std::string token_;
  int version_ = 2;
};

}  // namespace official_chat
