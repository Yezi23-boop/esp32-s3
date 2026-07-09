#include "net/esp_tcp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

#include <esp_log.h>

namespace official_chat {

namespace {

constexpr char kTag[] = "official_tcp";
constexpr EventBits_t kReceiveTaskExitBit = BIT0;
constexpr TickType_t kReceiveTaskStopTimeoutTicks = pdMS_TO_TICKS(10000);
constexpr size_t kReceiveBufferSize = 1500;
constexpr int kSocketTimeoutMs = 200;

}  // namespace

EspTcp::EspTcp() {
  event_group_ = xEventGroupCreate();
}

EspTcp::~EspTcp() {
  Disconnect();
  if (event_group_ != nullptr) {
    vEventGroupDelete(event_group_);
    event_group_ = nullptr;
  }
}

bool EspTcp::Connect(const std::string &host, int port) {
  Disconnect();

  struct hostent *server = gethostbyname(host.c_str());
  if (server == nullptr) {
    last_error_ = h_errno;
    ESP_LOGE(kTag, "gethostbyname failed for %s", host.c_str());
    return false;
  }

  struct sockaddr_in server_addr = {};
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(static_cast<uint16_t>(port));
  memcpy(&server_addr.sin_addr, server->h_addr, server->h_length);

  tcp_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (tcp_fd_ < 0) {
    last_error_ = errno;
    ESP_LOGE(kTag, "failed to create tcp socket: errno=%d", last_error_);
    return false;
  }

  if (connect(tcp_fd_, reinterpret_cast<struct sockaddr *>(&server_addr),
              sizeof(server_addr)) < 0) {
    last_error_ = errno;
    ESP_LOGE(kTag, "failed to connect tcp to %s:%d errno=%d", host.c_str(),
             port, last_error_);
    close(tcp_fd_);
    tcp_fd_ = -1;
    return false;
  }

  struct timeval timeout = {};
  timeout.tv_sec = 0;
  timeout.tv_usec = kSocketTimeoutMs * 1000;
  setsockopt(tcp_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(tcp_fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  connected_ = true;
  local_close_in_progress_ = false;
  if (event_group_ != nullptr) {
    xEventGroupClearBits(event_group_, kReceiveTaskExitBit);
  }
  if (xTaskCreate(
          [](void *arg) {
            auto *self = static_cast<EspTcp *>(arg);
            self->ReceiveTask();
            if (self->event_group_ != nullptr) {
              xEventGroupSetBits(self->event_group_, kReceiveTaskExitBit);
            }
            self->receive_task_handle_ = nullptr;
            vTaskDelete(nullptr);
          },
          "oc_tcp_rx", 4096, this, 1, &receive_task_handle_) != pdPASS) {
    last_error_ = ENOMEM;
    ESP_LOGE(kTag, "failed to create tcp receive task");
    connected_ = false;
    close(tcp_fd_);
    tcp_fd_ = -1;
    return false;
  }

  return true;
}

void EspTcp::Disconnect() {
  connected_ = false;
  local_close_in_progress_ = true;

  if (receive_task_handle_ != nullptr && event_group_ != nullptr) {
    const EventBits_t bits =
        xEventGroupWaitBits(event_group_, kReceiveTaskExitBit, pdFALSE, pdFALSE,
                            kReceiveTaskStopTimeoutTicks);
    if ((bits & kReceiveTaskExitBit) == 0) {
      ESP_LOGW(kTag, "timed out waiting for tcp receive task exit");
    }
  }

  if (tcp_fd_ != -1) {
    shutdown(tcp_fd_, SHUT_RDWR);
    close(tcp_fd_);
    tcp_fd_ = -1;
  }
  local_close_in_progress_ = false;
}

int EspTcp::Send(const std::string &data) {
  if (!connected_ || tcp_fd_ < 0) {
    return -1;
  }

  size_t total_sent = 0;
  while (total_sent < data.size()) {
    const int ret =
        send(tcp_fd_, data.data() + total_sent, data.size() - total_sent, 0);
    if (ret <= 0) {
      last_error_ = errno;
      ESP_LOGE(kTag, "tcp send failed: ret=%d errno=%d", ret, last_error_);
      return ret;
    }
    total_sent += static_cast<size_t>(ret);
  }

  return static_cast<int>(total_sent);
}

void EspTcp::ReceiveTask() {
  std::string data;
  data.resize(kReceiveBufferSize);

  while (connected_) {
    const int ret = recv(tcp_fd_, data.data(), data.size(), 0);
    if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      continue;
    }
    if (ret <= 0) {
      if (ret < 0 && connected_ && !local_close_in_progress_) {
        last_error_ = errno;
        ESP_LOGE(kTag, "tcp receive failed: ret=%d errno=%d", ret, last_error_);
      }
      const bool should_notify = connected_;
      connected_ = false;
      if (should_notify && disconnect_callback_) {
        disconnect_callback_();
      }
      break;
    }

    if (stream_callback_) {
      data.resize(static_cast<size_t>(ret));
      stream_callback_(data);
      data.resize(kReceiveBufferSize);
    }
  }
}

int EspTcp::GetLastError() {
  return last_error_;
}

void EspTcp::SetLocalCloseInProgress(bool enable) {
  local_close_in_progress_ = enable;
}

}  // namespace official_chat
