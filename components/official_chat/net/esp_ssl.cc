#include "net/esp_ssl.h"

#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#include <esp_crt_bundle.h>
#include <esp_log.h>

namespace official_chat {

namespace {

constexpr char kTag[] = "official_ssl";
constexpr EventBits_t kReceiveTaskExitBit = BIT0;
constexpr TickType_t kReceiveTaskStopTimeoutTicks = pdMS_TO_TICKS(10000);
constexpr size_t kReceiveBufferSize = 1500;
constexpr int kSocketTimeoutMs = 200;

}  // namespace

EspSsl::EspSsl() {
  event_group_ = xEventGroupCreate();
}

EspSsl::~EspSsl() {
  Disconnect();
  if (event_group_ != nullptr) {
    vEventGroupDelete(event_group_);
    event_group_ = nullptr;
  }
}

bool EspSsl::Connect(const std::string &host, int port) {
  Disconnect();

  tls_client_ = esp_tls_init();
  if (tls_client_ == nullptr) {
    last_error_ = ESP_FAIL;
    ESP_LOGE(kTag, "failed to initialize tls client");
    return false;
  }

  esp_tls_cfg_t cfg = {};
  cfg.crt_bundle_attach = esp_crt_bundle_attach;

  const int ret =
      esp_tls_conn_new_sync(host.c_str(), host.length(), port, &cfg, tls_client_);
  if (ret != 1) {
    esp_tls_error_handle_t last_error = nullptr;
    if (esp_tls_get_error_handle(tls_client_, &last_error) == ESP_OK &&
        last_error != nullptr) {
      int tls_error_code = 0;
      int tls_error_flags = 0;
      last_error_ = esp_tls_get_and_clear_last_error(
          last_error, &tls_error_code, &tls_error_flags);
    } else {
      last_error_ = ESP_FAIL;
    }
    ESP_LOGE(kTag, "failed to connect tls to %s:%d err=0x%x", host.c_str(),
             port, last_error_);
    esp_tls_conn_destroy(tls_client_);
    tls_client_ = nullptr;
    return false;
  }

  connected_ = true;
  local_close_in_progress_ = false;
  int sockfd = -1;
  if (esp_tls_get_conn_sockfd(tls_client_, &sockfd) == ESP_OK && sockfd >= 0) {
    struct timeval timeout = {};
    timeout.tv_sec = 0;
    timeout.tv_usec = kSocketTimeoutMs * 1000;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  }
  if (event_group_ != nullptr) {
    xEventGroupClearBits(event_group_, kReceiveTaskExitBit);
  }
  if (xTaskCreate(
          [](void *arg) {
            auto *self = static_cast<EspSsl *>(arg);
            self->ReceiveTask();
            if (self->event_group_ != nullptr) {
              xEventGroupSetBits(self->event_group_, kReceiveTaskExitBit);
            }
            self->receive_task_handle_ = nullptr;
            vTaskDelete(nullptr);
          },
          "oc_ssl_rx", 4096, this, 1, &receive_task_handle_) != pdPASS) {
    last_error_ = ENOMEM;
    ESP_LOGE(kTag, "failed to create ssl receive task");
    connected_ = false;
    esp_tls_conn_destroy(tls_client_);
    tls_client_ = nullptr;
    return false;
  }

  return true;
}

void EspSsl::Disconnect() {
  connected_ = false;
  local_close_in_progress_ = true;

  if (receive_task_handle_ != nullptr && event_group_ != nullptr) {
    const EventBits_t bits =
        xEventGroupWaitBits(event_group_, kReceiveTaskExitBit, pdFALSE, pdFALSE,
                            kReceiveTaskStopTimeoutTicks);
    if ((bits & kReceiveTaskExitBit) == 0) {
      ESP_LOGW(kTag, "timed out waiting for ssl receive task exit");
    }
  }

  if (tls_client_ != nullptr) {
    int sockfd = -1;
    if (esp_tls_get_conn_sockfd(tls_client_, &sockfd) == ESP_OK && sockfd >= 0) {
      shutdown(sockfd, SHUT_RDWR);
      close(sockfd);
    }
  }

  if (tls_client_ != nullptr) {
    esp_tls_conn_destroy(tls_client_);
    tls_client_ = nullptr;
  }
  local_close_in_progress_ = false;
}

int EspSsl::Send(const std::string &data) {
  if (!connected_ || tls_client_ == nullptr) {
    return -1;
  }

  size_t total_sent = 0;
  while (total_sent < data.size()) {
    const int ret =
        esp_tls_conn_write(tls_client_, data.data() + total_sent,
                           data.size() - total_sent);
    if (ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
      continue;
    }
    if (ret <= 0) {
      last_error_ = ret;
      ESP_LOGE(kTag, "ssl send failed: ret=%d", ret);
      return ret;
    }
    total_sent += static_cast<size_t>(ret);
  }

  return static_cast<int>(total_sent);
}

void EspSsl::ReceiveTask() {
  std::string data;
  data.resize(kReceiveBufferSize);

  while (connected_) {
    const int ret = esp_tls_conn_read(tls_client_, data.data(), data.size());
    if (ret == ESP_TLS_ERR_SSL_WANT_READ || ret == ESP_TLS_ERR_SSL_TIMEOUT) {
      continue;
    }
    if (ret <= 0) {
      if (ret < 0 && connected_ && !local_close_in_progress_) {
        last_error_ = ret;
        ESP_LOGE(kTag, "ssl receive failed: ret=%d", ret);
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

int EspSsl::GetLastError() {
  return last_error_;
}

void EspSsl::SetLocalCloseInProgress(bool enable) {
  local_close_in_progress_ = enable;
}

}  // namespace official_chat
