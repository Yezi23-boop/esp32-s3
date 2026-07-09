#include "assets_runtime.h"

#include <memory>
#include <string>

#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <esp_afe_sr_models.h>

#include "settings.h"

namespace official_chat {

namespace {

constexpr char kTag[] = "official_assets";
constexpr char kAssetsPartitionLabel[] = "assets"; /**< 约定在 partition-table 中必须包含的静态资源分区名称。 */
constexpr int kHttpBufferSize = 1024;              /**< HTTP 的接收和发送缓冲区大小，单位为字节，用于适配有限的 RAM 资源。 */

}  // namespace

AssetsRuntime::Result AssetsRuntime::CheckAndApplyPendingDownload(
    std::function<void(int progress, size_t speed)> progress_callback) {
  Settings settings("assets", true);
  std::string download_url = settings.GetString("download_url");
  if (download_url.empty()) {
    // 队列中无待下载的任务，保持现状退出。
    return {};
  }

  // 开始前先清除配置内 URL，避免下载失败时导致重复读取进入死循环。
  settings.EraseKey("download_url");
  return DownloadAndApply(download_url, std::move(progress_callback));
}

AssetsRuntime::Result AssetsRuntime::DownloadAndApply(
    const std::string &url,
    std::function<void(int progress, size_t speed)> progress_callback) {
  Result result;
  result.attempted = true;

  const esp_partition_t *partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, kAssetsPartitionLabel);
  if (partition == nullptr) {
    result.error = ESP_ERR_NOT_FOUND;
    result.message = "assets partition not found";
    return result;
  }

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = 30000;
  config.buffer_size = kHttpBufferSize;
  config.buffer_size_tx = kHttpBufferSize;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    result.error = ESP_ERR_NO_MEM;
    result.message = "failed to init http client";
    return result;
  }

  esp_http_client_set_method(client, HTTP_METHOD_GET);
  if (esp_http_client_open(client, 0) != ESP_OK) {
    esp_http_client_cleanup(client);
    result.error = ESP_FAIL;
    result.message = "failed to open assets download";
    return result;
  }

  if (esp_http_client_fetch_headers(client) < 0 ||
      esp_http_client_get_status_code(client) != 200) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    result.error = ESP_FAIL;
    result.message = "failed to fetch assets";
    return result;
  }

  const int content_length = esp_http_client_get_content_length(client);
  if (content_length <= 0 ||
      static_cast<size_t>(content_length) > partition->size) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    result.error = ESP_ERR_INVALID_SIZE;
    result.message = "assets size exceeds partition";
    return result;
  }

  if (esp_partition_erase_range(partition, 0, partition->size) != ESP_OK) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    result.error = ESP_FAIL;
    result.message = "failed to erase assets partition";
    return result;
  }

  std::unique_ptr<char, decltype(&heap_caps_free)> buffer(
      static_cast<char *>(heap_caps_malloc(kHttpBufferSize, MALLOC_CAP_INTERNAL)),
      &heap_caps_free);
  if (!buffer) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    result.error = ESP_ERR_NO_MEM;
    result.message = "failed to allocate assets buffer";
    return result;
  }

  size_t total_written = 0;
  size_t recent_written = 0;
  int64_t last_report_time = esp_timer_get_time();
  while (true) {
    const int read = esp_http_client_read(client, buffer.get(), kHttpBufferSize);
    if (read < 0) {
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      result.error = ESP_FAIL;
      result.message = "failed to read assets body";
      return result;
    }
    if (read == 0) {
      break;
    }
    if (esp_partition_write(partition, total_written, buffer.get(), read) !=
        ESP_OK) {
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      result.error = ESP_FAIL;
      result.message = "failed to write assets partition";
      return result;
    }
    total_written += read;
    recent_written += read;
    const int64_t now = esp_timer_get_time();
    if (now - last_report_time >= 1000000 ||
        total_written == static_cast<size_t>(content_length)) {
      const int progress = static_cast<int>((total_written * 100) / content_length);
      if (progress_callback) {
        progress_callback(progress, recent_written);
      }
      last_report_time = now;
      recent_written = 0;
    }
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  if (!RefreshSpeechModels()) {
    result.error = ESP_FAIL;
    result.message = "failed to refresh speech models";
    return result;
  }

  result.applied = true;
  result.message = "assets applied";
  return result;
}

bool AssetsRuntime::RefreshSpeechModels() {
  srmodel_list_t *models = esp_srmodel_init("model");
  if (models == nullptr) {
    ESP_LOGW(kTag, "failed to reload speech models after assets apply");
    return false;
  }
  esp_srmodel_deinit(models);
  return true;
}

}  // namespace official_chat
