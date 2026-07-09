#pragma once

#include <string>

namespace official_chat {

/**
 * @brief 系统软硬件级身份标识的组合中心。
 *
 * 为出厂硬件、OTA和激活验证服务提供唯一的标识符，如 MAC、SN。
 * 提供组装公私钥或者验证握手所需 Payload 的封装。
 */
class SystemIdentity {
 public:
  /**
   * @brief 获取硬件唯一的底层标识，通常基于 ESP 的出厂 MAC 衍生得到。
   * @return 12或16进制字符串形态的硬件指纹，例如 MAC 最后两段。
   */
  static std::string GetDeviceId();

  /**
   * @brief 获取标准统一的用户代理串 (User-Agent)。
   *
   * 必须在一切发起 HTTP 或者 WebSocket 握手时注入 Header。包含了设备固件框架及通信组件版本。
   */
  static std::string GetUserAgent();

  /**
   * @brief 生成或拉取 MQTT 必用的 Client ID。
   *
   * 保证设备重连或者多台设备并联时不会顶替其它终端，通常是固件前缀+MAC/SN。
   */
  static std::string GetOrCreateClientId();

  /**
   * @brief 判断该设备在量产烧录时是否拥有明确追溯用的 SN（保存在 efuse 或是定制的 NVS 中）。
   */
  static bool HasSerialNumber();
  static std::string GetSerialNumber();

  /**
   * @brief 获取目前内部网络通信的安全或者序列版本号。
   */
  static int GetActivationVersion();

  /**
   * @brief 打包发送给服务端的鉴权或握手校验数据。
   * @param[in] challenge 服务端给出的动态随机验证挑战，作为盐值使用。
   * @return JSON 或是 Base64 格式的回摆票据。
   */
  static std::string BuildActivationPayload(const std::string &challenge);
};

}  // namespace official_chat
