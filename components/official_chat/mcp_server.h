#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <cJSON.h>

#include "device_state.h"

namespace official_chat {

/**
 * @brief 轻量级的 Model/Machine Context Protocol 服务端。
 *
 * 允许外部 Agent 或调试工具通过标准化 JSON-RPC 格式调用设备端能力。
 * 通常借助 MQTT 控制信道进行收发。所有对外的 Callback 注册必须支持跨线程并发。
 */
class McpServer {
 public:
  using SendCallback = std::function<void(const std::string &payload)>;
  
  /**
   * @brief MCP Server 查询所需的核心设备状态聚合体。
   */
  struct RuntimeStatus {
    DeviceState chat_state = DeviceState::kUnknown; /**< 主业务流转到的状态段。 */
    bool device_aec_enabled = false;                /**< 是否使能了硬件/DSP回声消除。 */
  };
  using RuntimeStatusCallback = std::function<RuntimeStatus()>;

  static McpServer &GetInstance();

  /**
   * @brief 注册底层发送通道的回调函数。
   *
   * 一般由 Application 传入 MQTT 发布方法，该回调用于回传 MCP 指令执行结果。
   */
  void SetSendCallback(SendCallback callback);

  /**
   * @brief 设置被该服务端宣告为外部可见的名称和版本特征。
   */
  void SetServerInfo(std::string name, std::string version);
  void SetRuntimeStatusCallback(RuntimeStatusCallback callback);

  /**
   * @brief 将常用工具（如查询网络、查询状态等）内置注入到可用 Tools 列表。
   */
  void AddCommonTools();

  /**
   * @brief 提供给外部协议层解析下发 JSON 数据包的入口。
   *
   * 负责核对 JSON-RPC `method` 和 `id` 并自动调用目标 Tools 处理。
   * @param[in] json 已解析完毕的合法 JSON 对象指针，函数内不会释放它回收所有权。
   */
  void ParseMessage(const cJSON *json);

 private:
  struct ToolDefinition {
    std::string name;
    std::string description;
  };

  McpServer() = default;

  void AddToolLocked(std::string name, std::string description);
  void HandleToolCall(int id, const cJSON *params);
  cJSON *BuildToolsListResult();
  cJSON *BuildTextToolResult(const std::string &text);
  cJSON *BuildDeviceStatusJson();
  cJSON *BuildNetworkStatusJson();
  void ReplyResult(int id, cJSON *result);
  void ReplyError(int id, const std::string &message);
  bool SendEnvelope(cJSON *root);

  std::mutex mutex_;
  SendCallback send_callback_;
  RuntimeStatusCallback runtime_status_callback_;
  std::string server_name_ = "official_chat";
  std::string server_version_ = "unknown";
  std::vector<ToolDefinition> tools_;
  bool common_tools_added_ = false;
};

}  // namespace official_chat
