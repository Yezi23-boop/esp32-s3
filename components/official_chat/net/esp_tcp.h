#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include "net/tcp.h"

namespace official_chat {

class EspTcp : public Tcp {
 public:
  EspTcp();
  ~EspTcp() override;

  bool Connect(const std::string &host, int port) override;
  void Disconnect() override;
  int Send(const std::string &data) override;
  int GetLastError() override;
  void SetLocalCloseInProgress(bool enable) override;

 private:
  void ReceiveTask();

  int tcp_fd_ = -1;
  EventGroupHandle_t event_group_ = nullptr;
  TaskHandle_t receive_task_handle_ = nullptr;
  int last_error_ = 0;
  bool local_close_in_progress_ = false;
};

}  // namespace official_chat
