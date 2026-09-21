#include "mcp_server.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include <esp_log.h>

#include "audio_codec.h"
#include "wifi_control.h"

namespace official_chat {

namespace {

constexpr char kTag[] = "official_mcp";
constexpr char kProtocolVersion[] = "2024-11-05"; /**< 支持的最新 MCP 协议版本号。 */
constexpr char kToolGetDeviceStatus[] = "self.get_device_status";
constexpr char kToolGetSpeakerVolume[] = "self.audio_speaker.get_volume";
constexpr char kToolSetSpeakerVolume[] = "self.audio_speaker.set_volume";
constexpr char kToolGetNetworkStatus[] = "self.system.get_network_status";

/**
 * @brief 将设备枚举状态转化为字符串说明
 *
 * 为 JSON RPC 的可观测内容做枚举的直观解析输出。
 *
 * @param[in] state 要转换的当前设备运行时枚举值
 * @return 恒有字符串字面量返回 ("idle", "connecting", etc.)
 */
const char *DeviceStateToString(DeviceState state) {
  switch (state) {
    case DeviceState::kIdle:
      return "idle";
    case DeviceState::kConnecting:
      return "connecting";
    case DeviceState::kListening:
      return "listening";
    case DeviceState::kSpeaking:
      return "speaking";
    case DeviceState::kUnknown:
    default:
      return "unknown";
  }
}

/**
 * @brief 为受支持的内置工具生成符合 JSON schema 规范的描述模板。
 *
 * 主要是提供在 JSON RPC 交互中的 Tool 结构描述。
 * 
 * @param[in] tool_name 工具名称枚举
 * @return 组装完成的 schema cJSON 树对象。生命周期交由调用方清理，可能返回不包含具体 property 的基础 object。
 */
cJSON *CreateToolSchema(const std::string &tool_name) {
  cJSON *schema = cJSON_CreateObject();
  cJSON_AddStringToObject(schema, "type", "object");
  cJSON *properties = cJSON_CreateObject();
  cJSON_AddItemToObject(schema, "properties", properties);

  if (tool_name == kToolSetSpeakerVolume) {
    cJSON *volume = cJSON_CreateObject();
    cJSON_AddStringToObject(volume, "type", "integer");
    cJSON_AddNumberToObject(volume, "minimum", 0);
    cJSON_AddNumberToObject(volume, "maximum", 100);
    cJSON_AddItemToObject(properties, "volume", volume);

    cJSON *required = cJSON_CreateArray();
    cJSON_AddItemToArray(required, cJSON_CreateString("volume"));
    cJSON_AddItemToObject(schema, "required", required);
  }

  if (tool_name == kToolGetSpeakerVolume) {
    return schema;
  }

  if (tool_name == kToolGetNetworkStatus) {
    return schema;
  }

  return schema;
}

/**
 * @brief 输出 cJSON 数据到 C 字符串并在转储之后立即删除 json 树对象
 *
 * 因为经常是转储为 std::string 然后发往回调，避免写错 delete 导致内存泄漏，利用 RAII 处理。
 * 
 * @param[in,out] json 准备解析和删除的根指针。
 * @return String 成功时内容字符串。如果是 Null 或者分配报错，会返回默认的 "{}"。
 */
std::string JsonToStringAndDelete(cJSON *json) {
  char *json_str = cJSON_PrintUnformatted(json);
  std::string output = json_str != nullptr ? json_str : "{}";
  if (json_str != nullptr) {
    cJSON_free(json_str);
  }
  cJSON_Delete(json);
  return output;
}

}  // namespace

McpServer &McpServer::GetInstance() {
  static McpServer instance;
  return instance;
}

void McpServer::SetSendCallback(SendCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  send_callback_ = std::move(callback);
}

void McpServer::SetServerInfo(std::string name, std::string version) {
  std::lock_guard<std::mutex> lock(mutex_);
  server_name_ = std::move(name);
  server_version_ = std::move(version);
}

void McpServer::SetRuntimeStatusCallback(RuntimeStatusCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  runtime_status_callback_ = std::move(callback);
}

void McpServer::AddCommonTools() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (common_tools_added_) {
    return;
  }
  AddToolLocked(kToolGetDeviceStatus,
                "Provides the current device status, including audio, "
                "network, chat runtime, and unsupported board-level "
                "capabilities.");
  AddToolLocked(kToolGetSpeakerVolume,
                "Get the current volume of the audio speaker.");
  AddToolLocked(kToolSetSpeakerVolume,
                "Set the volume of the audio speaker. Volume must be between "
                "0 and 100.");
  AddToolLocked(kToolGetNetworkStatus,
                "Get the current Wi-Fi connectivity status for the device.");
  common_tools_added_ = true;
}

void McpServer::AddToolLocked(std::string name, std::string description) {
  const auto it = std::find_if(
      tools_.begin(), tools_.end(),
      [&name](const ToolDefinition &tool) { return tool.name == name; });
  if (it != tools_.end()) {
    return;
  }
  ToolDefinition tool;
  tool.name = std::move(name);
  tool.description = std::move(description);
  tools_.push_back(std::move(tool));
}

void McpServer::ParseMessage(const cJSON *json) {
  if (!cJSON_IsObject(json)) {
    ESP_LOGW(kTag, "ignoring invalid mcp payload");
    return;
  }

  const cJSON *version = cJSON_GetObjectItemCaseSensitive(json, "jsonrpc");
  if (!cJSON_IsString(version) || std::strcmp(version->valuestring, "2.0") != 0) {
    ESP_LOGW(kTag, "invalid mcp jsonrpc version");
    return;
  }

  const cJSON *method = cJSON_GetObjectItemCaseSensitive(json, "method");
  if (!cJSON_IsString(method)) {
    ESP_LOGW(kTag, "missing mcp method");
    return;
  }

  const std::string method_str = method->valuestring;
  ESP_LOGI(kTag, "received mcp method: %s", method_str.c_str());
  if (method_str.rfind("notifications", 0) == 0) {
    return;
  }

  const cJSON *id = cJSON_GetObjectItemCaseSensitive(json, "id");
  if (!cJSON_IsNumber(id)) {
    ESP_LOGW(kTag, "missing mcp id for method: %s", method_str.c_str());
    return;
  }
  const int id_int = id->valueint;

  if (method_str == "initialize") {
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "protocolVersion", kProtocolVersion);
    cJSON *capabilities = cJSON_CreateObject();
    cJSON_AddItemToObject(capabilities, "tools", cJSON_CreateObject());
    cJSON_AddItemToObject(result, "capabilities", capabilities);

    cJSON *server_info = cJSON_CreateObject();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cJSON_AddStringToObject(server_info, "name", server_name_.c_str());
      cJSON_AddStringToObject(server_info, "version", server_version_.c_str());
    }
    cJSON_AddItemToObject(result, "serverInfo", server_info);
    ReplyResult(id_int, result);
    return;
  }

  if (method_str == "tools/list") {
    ReplyResult(id_int, BuildToolsListResult());
    return;
  }

  if (method_str == "tools/call") {
    HandleToolCall(id_int, cJSON_GetObjectItemCaseSensitive(json, "params"));
    return;
  }

  ReplyError(id_int, "Method not implemented: " + method_str);
}

cJSON *McpServer::BuildToolsListResult() {
  cJSON *result = cJSON_CreateObject();
  cJSON *tools = cJSON_CreateArray();
  cJSON_AddItemToObject(result, "tools", tools);

  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto &tool : tools_) {
    cJSON *tool_json = cJSON_CreateObject();
    cJSON_AddStringToObject(tool_json, "name", tool.name.c_str());
    cJSON_AddStringToObject(tool_json, "description", tool.description.c_str());
    cJSON_AddItemToObject(tool_json, "inputSchema",
                          CreateToolSchema(tool.name));
    cJSON_AddItemToArray(tools, tool_json);
  }

  return result;
}

cJSON *McpServer::BuildTextToolResult(const std::string &text) {
  cJSON *result = cJSON_CreateObject();
  cJSON *content = cJSON_CreateArray();
  cJSON *text_item = cJSON_CreateObject();
  cJSON_AddStringToObject(text_item, "type", "text");
  cJSON_AddStringToObject(text_item, "text", text.c_str());
  cJSON_AddItemToArray(content, text_item);
  cJSON_AddItemToObject(result, "content", content);
  cJSON_AddBoolToObject(result, "isError", false);
  return result;
}

cJSON *McpServer::BuildDeviceStatusJson() {
  cJSON *status = cJSON_CreateObject();
  cJSON *audio = cJSON_CreateObject();
  cJSON *network = BuildNetworkStatusJson();
  cJSON *chat = cJSON_CreateObject();
  cJSON *screen = cJSON_CreateObject();
  cJSON *battery = cJSON_CreateObject();
  int volume = 0;
  const esp_err_t ret = audio_codec_get_volume(&volume);
  RuntimeStatus runtime_status;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (runtime_status_callback_) {
      runtime_status = runtime_status_callback_();
    }
  }

  cJSON_AddStringToObject(status, "type", "device_status");

  cJSON_AddBoolToObject(audio, "supported", true);
  cJSON_AddBoolToObject(audio, "volume_read_ok", ret == ESP_OK);
  if (ret == ESP_OK) {
    cJSON_AddNumberToObject(audio, "speaker_volume", volume);
  } else {
    cJSON_AddStringToObject(audio, "speaker_volume", "unknown");
  }
  cJSON_AddStringToObject(audio, "audio_path", "audio_codec");
  cJSON_AddItemToObject(status, "audio", audio);

  cJSON_AddItemToObject(status, "network", network);

  cJSON_AddBoolToObject(chat, "supported", true);
  cJSON_AddStringToObject(chat, "state",
                          DeviceStateToString(runtime_status.chat_state));
  cJSON_AddBoolToObject(chat, "device_aec_enabled",
                        runtime_status.device_aec_enabled);
  cJSON_AddItemToObject(status, "chat", chat);

  cJSON_AddBoolToObject(screen, "supported", false);
  cJSON_AddStringToObject(screen, "brightness", "unknown");
  cJSON_AddItemToObject(status, "screen", screen);

  cJSON_AddBoolToObject(battery, "supported", false);
  cJSON_AddStringToObject(battery, "level", "unknown");
  cJSON_AddStringToObject(battery, "charging", "unknown");
  cJSON_AddItemToObject(status, "battery", battery);

  return status;
}

cJSON *McpServer::BuildNetworkStatusJson() {
  cJSON *network = cJSON_CreateObject();
  char ip[16] = {0};
  const bool connected = wifi_control_is_connected();
  const esp_err_t ip_err = wifi_control_get_ip(ip, sizeof(ip));

  cJSON_AddBoolToObject(network, "supported", true);
  cJSON_AddBoolToObject(network, "wifi_connected", connected);
  if (connected && ip_err == ESP_OK && ip[0] != '\0') {
    cJSON_AddStringToObject(network, "ip", ip);
  } else {
    cJSON_AddStringToObject(network, "ip", "unknown");
  }
  return network;
}

void McpServer::HandleToolCall(int id, const cJSON *params) {
  if (!cJSON_IsObject(params)) {
    ReplyError(id, "Missing tool call params");
    return;
  }

  const cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
  if (!cJSON_IsString(name)) {
    ReplyError(id, "Missing tool name");
    return;
  }

  const std::string tool_name = name->valuestring;
  const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(params, "arguments");

  if (tool_name == kToolGetDeviceStatus) {
    ReplyResult(id, BuildTextToolResult(JsonToStringAndDelete(BuildDeviceStatusJson())));
    return;
  }

  if (tool_name == kToolGetSpeakerVolume) {
    cJSON *result = cJSON_CreateObject();
    int volume = 0;
    const esp_err_t ret = audio_codec_get_volume(&volume);
    cJSON_AddBoolToObject(result, "supported", true);
    cJSON_AddBoolToObject(result, "volume_read_ok", ret == ESP_OK);
    if (ret == ESP_OK) {
      cJSON_AddNumberToObject(result, "speaker_volume", volume);
    } else {
      cJSON_AddStringToObject(result, "speaker_volume", "unknown");
    }
    ReplyResult(id, BuildTextToolResult(JsonToStringAndDelete(result)));
    return;
  }

  if (tool_name == kToolSetSpeakerVolume) {
    if (!cJSON_IsObject(arguments)) {
      ReplyError(id, "Missing arguments for self.audio_speaker.set_volume");
      return;
    }

    const cJSON *volume = cJSON_GetObjectItemCaseSensitive(arguments, "volume");
    if (!cJSON_IsNumber(volume)) {
      ReplyError(id, "Missing integer volume");
      return;
    }

    const int volume_value = volume->valueint;
    if (volume_value < 0 || volume_value > 100) {
      ReplyError(id, "Volume must be between 0 and 100");
      return;
    }

    if (audio_codec_set_volume_preference(volume_value) != ESP_OK) {
      ReplyError(id, "Failed to set speaker volume");
      return;
    }

    ReplyResult(id, BuildTextToolResult("true"));
    return;
  }

  if (tool_name == kToolGetNetworkStatus) {
    ReplyResult(id, BuildTextToolResult(JsonToStringAndDelete(BuildNetworkStatusJson())));
    return;
  }

  ReplyError(id, "Tool not implemented: " + tool_name);
}

void McpServer::ReplyResult(int id, cJSON *result) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "jsonrpc", "2.0");
  cJSON_AddNumberToObject(root, "id", id);
  cJSON_AddItemToObject(root, "result", result);
  if (!SendEnvelope(root)) {
    cJSON_Delete(root);
  }
}

void McpServer::ReplyError(int id, const std::string &message) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "jsonrpc", "2.0");
  cJSON_AddNumberToObject(root, "id", id);
  cJSON *error = cJSON_CreateObject();
  cJSON_AddStringToObject(error, "message", message.c_str());
  cJSON_AddItemToObject(root, "error", error);
  if (!SendEnvelope(root)) {
    cJSON_Delete(root);
  }
}

bool McpServer::SendEnvelope(cJSON *root) {
  char *json_str = cJSON_PrintUnformatted(root);
  if (json_str == nullptr) {
    ESP_LOGE(kTag, "failed to serialize mcp payload");
    return false;
  }

  SendCallback callback;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    callback = send_callback_;
  }

  if (!callback) {
    ESP_LOGW(kTag, "mcp callback not ready");
    cJSON_free(json_str);
    return false;
  }

  ESP_LOGI(kTag, "sending mcp response");
  callback(std::string(json_str));
  cJSON_free(json_str);
  cJSON_Delete(root);
  return true;
}

}  // namespace official_chat
