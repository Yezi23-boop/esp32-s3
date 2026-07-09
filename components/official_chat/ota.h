#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <esp_err.h>

#include "official_chat.h"

struct cJSON;

namespace official_chat {

/**
 * @brief 基于 HTTP 的远程固件升级及激活配网逻辑。
 *
 * 负责跟出厂配置的服务器端进行交互，完成鉴权校验，并最终更新固件的分区指针。
 * 由于涉及大量 Flash 擦除与并发冲突，此类通常配合 Application 的状态锁机制统一调度。
 */
class Ota {
 public:
  /**
   * @brief 初始化 OTA 环境并传入升级的基准路由。
   *
   * @param[in] ota_url 配网及获取新版本的统一网关（如 https://api.xxx...）。
   */
  explicit Ota(std::string ota_url,
               official_chat_ensure_time_cb_t ensure_time_valid,
               official_chat_apply_server_time_cb_t apply_server_time,
               void *time_user_ctx);
  ~Ota() = default;

  /**
   * @brief 提取当前设备的固件版本与远程配置清单做比对。
   * @return 成功请求返回 ESP_OK。该函数可阻塞数十秒（等待 DNS 和 TLS 建连）。
   */
  esp_err_t CheckVersion();

  /**
   * @brief 发送激活请求，并从应答结果中持久化最新的业务配置（如连接凭据）。
   */
  esp_err_t Activate();

  /**
   * @brief 以异步块的形式，触发下载新版固件到备用 OTA 分区。
   * @param[in] callback 用于上报下载进度与速度到 UI。
   * @return true 为派发成功。
   */
  bool StartUpgrade(std::function<void(int progress, size_t speed)> callback);

  /**
   * @brief 提供给内部调用的实际 OTA 动作静态包裹。
   */
  static bool Upgrade(const std::string &firmware_url,
                      std::function<void(int progress, size_t speed)> callback);

  /**
   * @brief 在完成固件升级并且业务正常拉起后，主动确认当前闪存分区可信无异常回滚。
   */
  void MarkCurrentVersionValid();

  bool HasActivationChallenge() const { return has_activation_challenge_; }
  bool HasNewVersion() const { return has_new_version_; }
  bool HasMqttConfig() const { return has_mqtt_config_; }
  bool HasWebsocketConfig() const { return has_websocket_config_; }
  bool HasActivationCode() const { return has_activation_code_; }
  bool HasServerTime() const { return has_server_time_; }
  const std::string &GetFirmwareVersion() const { return firmware_version_; }
  const std::string &GetCurrentVersion() const { return current_version_; }
  const std::string &GetFirmwareUrl() const { return firmware_url_; }
  const std::string &GetActivationMessage() const { return activation_message_; }
  const std::string &GetActivationCode() const { return activation_code_; }
  int GetActivationTimeoutMs() const { return activation_timeout_ms_; }
  const std::string &GetOtaUrl() const { return ota_url_; }

 private:
  static std::vector<int> ParseVersion(const std::string &version);
  static bool IsNewVersionAvailable(const std::string &current_version,
                                    const std::string &new_version);
  esp_err_t PerformJsonRequest(const char *method, const std::string &url,
                               const std::string &payload, int *status_code,
                               std::string *response) const;
  std::string BuildSystemInfoJson() const;
  std::string BuildActivateUrl() const;
  void PersistProtocolConfig(const cJSON *root, const char *section_name);
  void PersistServerTime(const cJSON *root);

  std::string ota_url_;
  official_chat_ensure_time_cb_t ensure_time_valid_ = nullptr;
  official_chat_apply_server_time_cb_t apply_server_time_ = nullptr;
  void *time_user_ctx_ = nullptr;
  std::string activation_message_;
  std::string activation_code_;
  std::string current_version_;
  std::string firmware_version_;
  std::string firmware_url_;
  std::string activation_challenge_;
  bool has_new_version_ = false;
  bool has_mqtt_config_ = false;
  bool has_websocket_config_ = false;
  bool has_server_time_ = false;
  bool has_activation_code_ = false;   /**< 此布尔标志代表是否必须强依赖用户扫码激活的 PIN 码。 */
  bool has_activation_challenge_ = false;
  int activation_timeout_ms_ = 30000;  /**< 与后端建连并走完 HTTP 请求的总超时约束。 */
};

}  // namespace official_chat
