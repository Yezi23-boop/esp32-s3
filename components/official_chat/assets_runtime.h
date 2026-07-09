#pragma once

#include <functional>
#include <string>

#include <esp_err.h>

namespace official_chat {

class AssetsRuntime {
 public:
  /**
   * @brief 资源包下载和应用的结果。
   *
   * 记录是否触发了下载、是否应用成功，以及失败时的错误码和补充信息。
   */
  struct Result {
    bool attempted = false; /**< 是否检测到了下载任务并尝试执行下载。 */
    bool applied = false;   /**< 资源包是否已成功写入分区并可立即生效。 */
    esp_err_t error = ESP_OK; /**< 下载或写入过程中的底层错误码。 */
    std::string message;      /**< 提供给上层或用于日志输出的友好错误说明。 */
  };

  AssetsRuntime() = default;

  /**
   * @brief 检查是否存在未完成的资源包下载任务，并在需要时执行下载。
   *
   * 从设置存储中读取资源下载链接。如果存在未完成的任务，
   * 触发下载流程，更新 Flash 的 assets 分区并使之生效。
   *
   * @param[in] progress_callback 下载进度的回调函数，传入进度百分比与瞬时传输速度（字节数）。
   * @return Result 返回资源更新是否已触发和应用的状态。
   *
   * @note 调用此函数会阻塞当前任务直到下载完成或发生错误，不应在 UI 或音频播放临界途径内调用。
   */
  Result CheckAndApplyPendingDownload(
      std::function<void(int progress, size_t speed)> progress_callback);

 private:
  Result DownloadAndApply(const std::string &url,
                          std::function<void(int progress, size_t speed)>
                              progress_callback);
  bool RefreshSpeechModels();
};

}  // namespace official_chat
