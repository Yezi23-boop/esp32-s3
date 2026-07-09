#pragma once

#include <string>

namespace official_chat {

/**
 * @brief 网络连接的根本协议类型。
 *
 * 目前项目支持基于 WebSocket 和 MQTT 两种连接范式。
 * 网络层行为将根据此处派生切换不同类实体和收发包序列限制。
 */
enum class ProtocolKind {
  kWebsocket, /**< 基于文本或二进制的长连接 WS。适用于语音大流直接发送。 */
  kMqtt,      /**< 轻量级发布/订阅机制。适合作状态机轮询或短片段指令的下达。 */
};

/**
 * @brief 协议被读取或启用的配置加载来源。
 *
 * 优先使用新下发的或 NVS 缓存内容；为空时再从代码表头加载出厂值。
 */
enum class ProtocolConfigSource {
  kNvsMqtt,        /**< 从本地持久化文件系统 (NVS) 中挂载恢复回来的 MQTT。 */
  kNvsWebsocket,   /**< 从本地持久化文件系统 (NVS) 中挂载恢复回来的 Websocket。 */
  kPublicConfig,   /**< API 参数显式传参或配网阶段服务端透传过来的设定。 */
  kBuiltinDefault, /**< 代码内部硬编码兜底常量宏。 */
};

/**
 * @brief 实例化及保持 MQTT 协议连接所需的详细运行时环境要求。
 */
struct MqttRuntimeConfig {
  std::string endpoint;       /**< 带有协议头的 IP 或域名端点（例如 mqtt://...）。 */
  std::string client_id;      /**< QoS 和 Session 保持必需的唯一客户端标识。 */
  std::string username;       /**< 可选的 IAM 或 SAS 提供方所需用户名。 */
  std::string password;       /**< 可选密码/签名/令牌。 */
  int keepalive = 240;        /**< 断线监控及心跳 PING 包时间戳，不可太小以免堵塞网络发送。 */
  std::string publish_topic;  /**< 指定流推及事件消息被派发到哪一个路由 Topic 上。 */

  /**
   * @brief 验证该配置是否具备尝试建立握手的基础完整条件。
   */
  bool IsValid() const;
};

/**
 * @brief websocket 连接的所需基础环境参数。
 */
struct WebsocketRuntimeConfig {
  std::string url;        /**< WS 或 WSS 长连接目标地址。 */
  std::string token;      /**< Auth 请求或鉴权 Header 的携带票据。 */
  int version = 2;

  bool IsValid() const;
};

struct WebsocketFallbackConfig {
  std::string url;
  std::string token;
  int version = 2;
  bool from_public_config = false;
};

struct ProtocolConfigSelection {
  ProtocolKind kind = ProtocolKind::kWebsocket;
  ProtocolConfigSource source = ProtocolConfigSource::kBuiltinDefault;
  MqttRuntimeConfig mqtt;
  WebsocketRuntimeConfig websocket;
  bool mqtt_config_present = false;
  bool websocket_config_present = false;
};

ProtocolConfigSelection LoadProtocolConfigSelection(
    const WebsocketFallbackConfig &fallback);

const char *ProtocolKindToString(ProtocolKind kind);
const char *ProtocolConfigSourceToString(ProtocolConfigSource source);

}  // namespace official_chat
