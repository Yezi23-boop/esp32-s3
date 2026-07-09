#pragma once

#include <esp_tls.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include "net/tcp.h"

namespace official_chat {

class EspSsl : public Tcp {
 public:
  EspSsl();
  ~EspSsl() override;

  bool Connect(const std::string &host, int port) override;
  void Disconnect() override;
  int Send(const std::string &data) override;
  int GetLastError() override;
  void SetLocalCloseInProgress(bool enable) override;

 private:
  void ReceiveTask();

  esp_tls_t *tls_client_ = nullptr;
  EventGroupHandle_t event_group_ = nullptr;
  TaskHandle_t receive_task_handle_ = nullptr;
  int last_error_ = 0;
  bool local_close_in_progress_ = false;
};

}  // namespace official_chat
