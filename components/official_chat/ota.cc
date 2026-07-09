#include "ota.h"

#include <algorithm>
#include <ctime>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <utility>

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <sdkconfig.h>

#include "board_metadata.h"
#include "settings.h"
#include "system_identity.h"

extern "C" {
#include "system_util.h"
}

namespace official_chat {

namespace {

constexpr char kTag[] = "official_ota";
constexpr int kHttpBufferSize = 1024;                /**< HTTP 的接收和发送缓冲区大小，单位为字节，用于适配有限的 RAM 资源。 */
constexpr char kDefaultLanguageCode[] = "zh-CN";     /**< 设备在出厂或者未设置语言时的默认识别配置使用。 */
constexpr time_t kTlsValidEpochThreshold = 1704067200; /**< 2024-01-01 00:00:00 UTC；小于该时间通常说明系统仍停留在冷启动默认时钟，HTTPS 证书有效期校验不可信。 */
constexpr uint32_t kSntpSyncTimeoutMs = 15000;         /**< OTA 激活阶段最多等待 15 秒授时；既为 TLS 证书校验争取有效时间，又避免前台无限卡死。 */

/**
 * @brief 判断当前系统时钟是否已经进入可用于 TLS 证书校验的有效区间。
 *
 * HTTPS 证书链校验依赖系统时间；若设备仍停留在 1970 等冷启动时间，
 * `mbedtls_ssl_handshake` 很容易因证书有效期检查失败而直接中断。
 *
 * @param[in] unix_seconds 当前系统 Unix 时间戳，单位为秒。
 * @return `true` 表示时间已进入可信窗口；`false` 表示仍需继续授时。
 */
bool IsSystemTimeValid(const time_t unix_seconds) {
  return unix_seconds >= kTlsValidEpochThreshold;
}

/**
 * @brief 生成可读的 UTC 时间快照，便于串口定位 TLS 失败时的系统时钟状态。
 *
 * @param[in] unix_seconds 当前系统 Unix 时间戳，单位为秒。
 * @return 格式化后的 UTC 时间字符串；若转换失败则返回 `"invalid"`。
 */
std::string FormatUtcTimeSnapshot(const time_t unix_seconds) {
  std::array<char, 32> buffer = {};
  std::tm utc_time = {};
  if (gmtime_r(&unix_seconds, &utc_time) == nullptr) {
    return "invalid";
  }
  if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%d %H:%M:%S UTC",
                    &utc_time) == 0) {
    return "invalid";
  }
  return buffer.data();
}

/**
 * @brief 输出当前系统时间快照，帮助判断 TLS 失败是否由无效时钟引起。
 *
 * @param[in] stage 当前日志所处阶段，如 `before_tls_request`。
 */
void LogSystemTimeSnapshot(const char *stage) {
  time_t now = 0;
  time(&now);
  ESP_LOGI(kTag, "time snapshot stage=%s epoch=%lld utc=%s valid=%d",
           stage, static_cast<long long>(now),
           FormatUtcTimeSnapshot(now).c_str(), IsSystemTimeValid(now));
}

/**
 * @brief 在发起 HTTPS 请求前等待系统时间进入 TLS 可用区间。
 *
 * official_chat 不直接启动 SNTP，也不访问 RTC；时间 owner 由外层通过回调注入。
 *
 * @param[in] url 即将访问的请求 URL，仅用于诊断日志。
 * @param[in] ensure_time_valid 外层注入的时间确认回调。
 * @param[in] user_ctx 回调透传上下文。
 * @return `ESP_OK` 表示时间已有效；否则返回超时或状态错误码。
 */
esp_err_t EnsureSystemTimeValidForTls(
    const std::string &url, official_chat_ensure_time_cb_t ensure_time_valid,
    void *user_ctx) {
  if (url.rfind("https://", 0) != 0) {
    return ESP_OK;
  }

  LogSystemTimeSnapshot("before_tls_request");

  time_t now = 0;
  time(&now);
  if (IsSystemTimeValid(now)) {
    return ESP_OK;
  }

  ESP_LOGW(kTag,
           "system time invalid before HTTPS request, request external time owner url=%s",
           url.c_str());
  if (ensure_time_valid == nullptr) {
    ESP_LOGE(kTag, "no external time owner callback configured for HTTPS");
    return ESP_ERR_INVALID_STATE;
  }

  const esp_err_t ret = ensure_time_valid(kSntpSyncTimeoutMs, user_ctx);
  if (ret == ESP_OK) {
    LogSystemTimeSnapshot("after_external_time_ensure");
    ESP_LOGI(kTag, "time ensure callback ok before HTTPS");
    return ESP_OK;
  }

  LogSystemTimeSnapshot("after_external_time_ensure_failed");
  ESP_LOGE(kTag, "external time owner failed before HTTPS url=%s err=%s",
           url.c_str(), esp_err_to_name(ret));
  return ret;
}

/**
 * @brief 输出 HTTP/TLS 失败时的证书与时钟诊断信息。
 *
 * `esp_http_client_open()` 在证书校验失败时通常只返回 `ESP_ERR_HTTP_CONNECT`，
 * 因此这里额外抓取 errno、TLS 错误码和校验 flag，帮助区分是时间问题、
 * 信任链问题还是底层套接字问题。
 *
 * @param[in] client HTTP client 句柄。
 * @param[in] url 当前请求 URL。
 * @param[in] stage 失败阶段，例如 `open` 或 `fetch_headers`。
 * @param[in] err 当前 HTTP 层返回的错误码。
 */
void LogHttpTlsDiagnostics(esp_http_client_handle_t client,
                           const std::string &url,
                           const char *stage,
                           const esp_err_t err) {
  int tls_error_code = 0;
  int tls_flags = 0;
  const int socket_errno = esp_http_client_get_errno(client);
  const esp_err_t tls_err = esp_http_client_get_and_clear_last_tls_error(
      client, &tls_error_code, &tls_flags);

  LogSystemTimeSnapshot(stage);
  ESP_LOGE(kTag,
           "http tls diagnostics stage=%s url=%s err=%s errno=%d tls_err=%s tls_code=%d tls_flags=0x%x transport=%d",
           stage, url.c_str(), esp_err_to_name(err), socket_errno,
           esp_err_to_name(tls_err), tls_error_code, tls_flags,
           esp_http_client_get_transport_type(client));
  if (tls_error_code == -0x2700) {
    ESP_LOGE(kTag,
             "mbedtls -0x2700 usually means certificate validation failed; check system time and CA trust chain first");
  }
}

/**
 * @brief 按后缀拼装完整有效的网络 URL 请求目标。
 * 
 * 因为外部配置或系统配置结尾可能会出现或缺乏 '/'，统一容错处理。
 * 
 * @param[in] base 基地址 URL
 * @param[in] suffix 需要添加请求路径或者是文件名
 * @return 组装成功有效的可调用地址。
 */
std::string JoinUrl(const std::string &base, const char *suffix) {
  if (base.empty()) {
    return {};
  }
  if (base.back() == '/') {
    return base + suffix;
  }
  return base + "/" + suffix;
}

const char *GetLanguageCode() {
#if defined(CONFIG_OFFICIAL_CHAT_LANGUAGE_CODE)
  if (std::strlen(CONFIG_OFFICIAL_CHAT_LANGUAGE_CODE) > 0) {
    return CONFIG_OFFICIAL_CHAT_LANGUAGE_CODE;
  }
#endif
  return kDefaultLanguageCode;
}

std::string BuildCompileTime(const esp_app_desc_t *app_desc) {
  if (app_desc == nullptr) {
    return {};
  }
  return std::string(get_compile_time(app_desc)) + "Z";
}

std::string BuildElfSha256(const esp_app_desc_t *app_desc) {
  if (app_desc == nullptr) {
    return {};
  }

  std::array<char, 65> sha256 = {};
  for (size_t i = 0; i < 32; ++i) {
    std::snprintf(sha256.data() + (i * 2), sha256.size() - (i * 2), "%02x",
                  app_desc->app_elf_sha256[i]);
  }
  return sha256.data();
}

void AddChipInfoJson(cJSON *root) {
  esp_chip_info_t chip_info = {};
  esp_chip_info(&chip_info);

  cJSON *chip_info_json = cJSON_CreateObject();
  cJSON_AddNumberToObject(chip_info_json, "model", chip_info.model);
  cJSON_AddNumberToObject(chip_info_json, "cores", chip_info.cores);
  cJSON_AddNumberToObject(chip_info_json, "revision", chip_info.revision);
  cJSON_AddNumberToObject(chip_info_json, "features", chip_info.features);
  cJSON_AddItemToObject(root, "chip_info", chip_info_json);
}

void AddApplicationJson(cJSON *root, const esp_app_desc_t *app_desc) {
  cJSON *application = cJSON_CreateObject();
  if (app_desc != nullptr) {
    cJSON_AddStringToObject(application, "name", app_desc->project_name);
    cJSON_AddStringToObject(application, "version", app_desc->version);
    cJSON_AddStringToObject(application, "compile_time",
                            BuildCompileTime(app_desc).c_str());
    cJSON_AddStringToObject(application, "idf_version", app_desc->idf_ver);
    cJSON_AddStringToObject(application, "elf_sha256",
                            BuildElfSha256(app_desc).c_str());
  }
  cJSON_AddItemToObject(root, "application", application);
}

void AddPartitionTableJson(cJSON *root) {
  cJSON *partition_table = cJSON_CreateArray();
  esp_partition_iterator_t iterator =
      esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY,
                         nullptr);
  while (iterator != nullptr) {
    const esp_partition_t *partition = esp_partition_get(iterator);
    cJSON *partition_json = cJSON_CreateObject();
    cJSON_AddStringToObject(partition_json, "label", partition->label);
    cJSON_AddNumberToObject(partition_json, "type", partition->type);
    cJSON_AddNumberToObject(partition_json, "subtype", partition->subtype);
    cJSON_AddNumberToObject(partition_json, "address", partition->address);
    cJSON_AddNumberToObject(partition_json, "size", partition->size);
    cJSON_AddItemToArray(partition_table, partition_json);
    iterator = esp_partition_next(iterator);
  }
  cJSON_AddItemToObject(root, "partition_table", partition_table);
}

void AddRunningOtaJson(cJSON *root) {
  cJSON *ota = cJSON_CreateObject();
  const esp_partition_t *running_partition = esp_ota_get_running_partition();
  if (running_partition != nullptr) {
    cJSON_AddStringToObject(ota, "label", running_partition->label);
  }
  cJSON_AddItemToObject(root, "ota", ota);
}

void AddBoardMetadataJson(cJSON *root) {
  const BoardMetadata board_metadata = LoadCurrentBoardMetadata();

  cJSON *board = cJSON_CreateObject();
  cJSON_AddStringToObject(board, "type", board_metadata.type.c_str());
  cJSON_AddStringToObject(board, "name", board_metadata.name.c_str());
  cJSON_AddItemToObject(root, "board", board);

  if (board_metadata.has_display) {
    cJSON *display = cJSON_CreateObject();
    cJSON_AddBoolToObject(display, "monochrome",
                          board_metadata.display_monochrome);
    cJSON_AddNumberToObject(display, "width", board_metadata.display_width);
    cJSON_AddNumberToObject(display, "height", board_metadata.display_height);
    cJSON_AddItemToObject(root, "display", display);
  }
}

}  // namespace

Ota::Ota(std::string ota_url, official_chat_ensure_time_cb_t ensure_time_valid,
         official_chat_apply_server_time_cb_t apply_server_time,
         void *time_user_ctx)
    : ota_url_(std::move(ota_url)),
      ensure_time_valid_(ensure_time_valid),
      apply_server_time_(apply_server_time),
      time_user_ctx_(time_user_ctx) {
  if (ota_url_.empty()) {
    Settings settings("wifi", false);
    ota_url_ = settings.GetString("ota_url");
  }
}

std::vector<int> Ota::ParseVersion(const std::string &version) {
  std::vector<int> values;
  std::stringstream ss(version);
  std::string segment;
  while (std::getline(ss, segment, '.')) {
    char *end = nullptr;
    const long parsed = std::strtol(segment.c_str(), &end, 10);
    if (end == segment.c_str() || (end != nullptr && *end != '\0')) {
      values.push_back(0);
    } else {
      values.push_back(static_cast<int>(parsed));
    }
  }
  return values;
}

bool Ota::IsNewVersionAvailable(const std::string &current_version,
                                const std::string &new_version) {
  const std::vector<int> current = ParseVersion(current_version);
  const std::vector<int> newer = ParseVersion(new_version);
  for (size_t i = 0; i < std::min(current.size(), newer.size()); ++i) {
    if (newer[i] > current[i]) {
      return true;
    }
    if (newer[i] < current[i]) {
      return false;
    }
  }
  return newer.size() > current.size();
}

std::string Ota::BuildSystemInfoJson() const {
  cJSON *root = cJSON_CreateObject();
  const esp_app_desc_t *app_desc = esp_app_get_description();
  cJSON_AddNumberToObject(root, "version", 2);
  cJSON_AddStringToObject(root, "language", GetLanguageCode());
  cJSON_AddNumberToObject(root, "flash_size",
                          static_cast<double>(get_flash_size()));
  cJSON_AddStringToObject(
      root, "minimum_free_heap_size",
      std::to_string(get_minimum_free_heap_size()).c_str());
  cJSON_AddStringToObject(root, "mac_address",
                          SystemIdentity::GetDeviceId().c_str());
  cJSON_AddStringToObject(root, "uuid",
                          SystemIdentity::GetOrCreateClientId().c_str());
  cJSON_AddStringToObject(root, "chip_model_name", CONFIG_IDF_TARGET);
  AddChipInfoJson(root);
  AddApplicationJson(root, app_desc);
  AddPartitionTableJson(root);
  AddRunningOtaJson(root);
  AddBoardMetadataJson(root);

  char *json = cJSON_PrintUnformatted(root);
  std::string payload = json != nullptr ? json : "{}";
  if (json != nullptr) {
    cJSON_free(json);
  }
  cJSON_Delete(root);
  return payload;
}

/**
 * @brief 发起 JSON 请求，并在 TLS 失败时补充证书和时间诊断日志。
 *
 * @param[in] method HTTP 方法，目前只使用 `GET/POST`。
 * @param[in] url 请求目标 URL。
 * @param[in] payload 请求体 JSON；GET 时允许为空。
 * @param[out] status_code 输出 HTTP 状态码，可为空。
 * @param[out] response 输出响应体字符串。
 * @return `ESP_OK` 表示请求与读取均完成；否则返回具体错误码。
 */
esp_err_t Ota::PerformJsonRequest(const char *method, const std::string &url,
                                  const std::string &payload,
                                  int *status_code,
                                  std::string *response) const {
  const esp_err_t time_err =
      EnsureSystemTimeValidForTls(url, ensure_time_valid_, time_user_ctx_);
  if (time_err != ESP_OK) {
    ESP_LOGE(kTag, "skip https request due to invalid system time url=%s err=%s",
             url.c_str(), esp_err_to_name(time_err));
    return time_err;
  }

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = activation_timeout_ms_;
  config.buffer_size = kHttpBufferSize;
  config.buffer_size_tx = kHttpBufferSize;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    return ESP_ERR_NO_MEM;
  }

  esp_http_client_set_method(
      client, std::strcmp(method, "POST") == 0 ? HTTP_METHOD_POST
                                                 : HTTP_METHOD_GET);
  esp_http_client_set_header(
      client, "Activation-Version",
      SystemIdentity::GetActivationVersion() == 2 ? "2" : "1");
  esp_http_client_set_header(client, "Device-Id",
                             SystemIdentity::GetDeviceId().c_str());
  esp_http_client_set_header(client, "Client-Id",
                             SystemIdentity::GetOrCreateClientId().c_str());
  esp_http_client_set_header(client, "User-Agent",
                             SystemIdentity::GetUserAgent().c_str());
  esp_http_client_set_header(client, "Accept-Language", GetLanguageCode());
  esp_http_client_set_header(client, "Content-Type", "application/json");
  if (SystemIdentity::HasSerialNumber()) {
    esp_http_client_set_header(client, "Serial-Number",
                               SystemIdentity::GetSerialNumber().c_str());
  }
  if (!payload.empty()) {
    esp_http_client_set_post_field(client, payload.data(), payload.size());
  }

  esp_err_t err = esp_http_client_open(client, payload.empty() ? 0 : payload.size());
  if (err != ESP_OK) {
    LogHttpTlsDiagnostics(client, url, "open", err);
    esp_http_client_cleanup(client);
    return err;
  }

  if (!payload.empty()) {
    const int written =
        esp_http_client_write(client, payload.data(), payload.size());
    if (written < 0 || static_cast<size_t>(written) != payload.size()) {
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      return ESP_FAIL;
    }
  }

  if (status_code != nullptr) {
    const int fetch_result = esp_http_client_fetch_headers(client);
    if (fetch_result < 0) {
      *status_code = esp_http_client_get_status_code(client);
      LogHttpTlsDiagnostics(client, url, "fetch_headers", ESP_ERR_HTTP_FETCH_HEADER);
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      return ESP_FAIL;
    }
    *status_code = esp_http_client_get_status_code(client);
  }

  response->clear();
  char buffer[kHttpBufferSize];
  while (true) {
    const int read = esp_http_client_read(client, buffer, sizeof(buffer));
    if (read < 0) {
      LogHttpTlsDiagnostics(client, url, "read", ESP_FAIL);
      err = ESP_FAIL;
      break;
    }
    if (read == 0) {
      break;
    }
    response->append(buffer, read);
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return err;
}

void Ota::PersistProtocolConfig(const cJSON *root, const char *section_name) {
  const cJSON *section = cJSON_GetObjectItem(root, section_name);
  if (!cJSON_IsObject(section)) {
    return;
  }

  Settings settings(section_name, true);
  const cJSON *item = nullptr;
  cJSON_ArrayForEach(item, section) {
    if (cJSON_IsString(item)) {
      settings.SetString(item->string, item->valuestring);
    } else if (cJSON_IsNumber(item)) {
      settings.SetInt(item->string, item->valueint);
    } else if (cJSON_IsBool(item)) {
      settings.SetBool(item->string, cJSON_IsTrue(item));
    }
  }
}

/**
 * @brief 持久化 OTA 服务端回传的服务器时间，供后续协议和日志统一使用。
 *
 * @param[in] root OTA 版本检查接口返回的 JSON 根对象。
 */
void Ota::PersistServerTime(const cJSON *root) {
  const cJSON *server_time = cJSON_GetObjectItem(root, "server_time");
  if (!cJSON_IsObject(server_time)) {
    return;
  }

  const cJSON *timestamp = cJSON_GetObjectItem(server_time, "timestamp");
  if (!cJSON_IsNumber(timestamp)) {
    return;
  }

  if (apply_server_time_ == nullptr) {
    ESP_LOGW(kTag, "server_time ignored: no external time owner callback");
    return;
  }

  /*
   * server_time.timestamp 是 Unix epoch 毫秒，语义上已经是 UTC 绝对时间。
   * timezone_offset 只能用于显示本地时间，不能加到 epoch 后再写入系统时间，
   * 否则东八区会把系统时间写快 8 小时，并进一步污染 RTC 写回。
   */
  const int64_t unix_seconds =
      static_cast<int64_t>(timestamp->valuedouble / 1000);
  const esp_err_t ret = apply_server_time_(unix_seconds, time_user_ctx_);
  if (ret != ESP_OK) {
    ESP_LOGW(kTag, "server_time apply callback failed: %s",
             esp_err_to_name(ret));
    return;
  }

  has_server_time_ = true;
  LogSystemTimeSnapshot("server_time_applied");
}

/**
 * @brief 向 OTA 服务查询版本、激活挑战和协议配置。
 *
 * 该步骤会在首次 HTTPS 建连前确保系统时间进入 TLS 可用区间，
 * 避免因冷启动默认时钟导致证书有效期校验失败。
 *
 * @return `ESP_OK` 表示版本查询成功并已完成响应解析；否则返回错误码。
 */
esp_err_t Ota::CheckVersion() {
  const esp_app_desc_t *app_desc = esp_app_get_description();
  current_version_ = app_desc != nullptr ? app_desc->version : "0.0.0";
  has_activation_code_ = false;
  has_activation_challenge_ = false;
  has_mqtt_config_ = false;
  has_websocket_config_ = false;
  has_new_version_ = false;
  has_server_time_ = false;

  int status_code = 0;
  std::string response;
  const std::string payload = BuildSystemInfoJson();
  const esp_err_t err =
      PerformJsonRequest("POST", ota_url_, payload, &status_code, &response);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "version check request failed: %s", esp_err_to_name(err));
    return err;
  }
  if (status_code != 200) {
    ESP_LOGE(kTag, "version check status=%d body=%s", status_code,
             response.c_str());
    return ESP_FAIL;
  }

  cJSON *root = cJSON_Parse(response.c_str());
  if (root == nullptr) {
    return ESP_ERR_INVALID_RESPONSE;
  }

  const cJSON *activation = cJSON_GetObjectItem(root, "activation");
  if (cJSON_IsObject(activation)) {
    const cJSON *message = cJSON_GetObjectItem(activation, "message");
    if (cJSON_IsString(message)) {
      activation_message_ = message->valuestring;
    }
    const cJSON *code = cJSON_GetObjectItem(activation, "code");
    if (cJSON_IsString(code)) {
      activation_code_ = code->valuestring;
      has_activation_code_ = true;
    }
    const cJSON *challenge = cJSON_GetObjectItem(activation, "challenge");
    if (cJSON_IsString(challenge)) {
      activation_challenge_ = challenge->valuestring;
      has_activation_challenge_ = true;
    }
    const cJSON *timeout_ms = cJSON_GetObjectItem(activation, "timeout_ms");
    if (cJSON_IsNumber(timeout_ms)) {
      activation_timeout_ms_ = timeout_ms->valueint;
    }
  }

  PersistProtocolConfig(root, "mqtt");
  PersistProtocolConfig(root, "websocket");
  has_mqtt_config_ = cJSON_IsObject(cJSON_GetObjectItem(root, "mqtt"));
  has_websocket_config_ =
      cJSON_IsObject(cJSON_GetObjectItem(root, "websocket"));
  PersistServerTime(root);

  const cJSON *firmware = cJSON_GetObjectItem(root, "firmware");
  if (cJSON_IsObject(firmware)) {
    const cJSON *version = cJSON_GetObjectItem(firmware, "version");
    const cJSON *url = cJSON_GetObjectItem(firmware, "url");
    const cJSON *force = cJSON_GetObjectItem(firmware, "force");
    if (cJSON_IsString(version)) {
      firmware_version_ = version->valuestring;
    }
    if (cJSON_IsString(url)) {
      firmware_url_ = url->valuestring;
    }
    if (cJSON_IsString(version) && cJSON_IsString(url)) {
      has_new_version_ =
          IsNewVersionAvailable(current_version_, firmware_version_);
      if (cJSON_IsNumber(force) && force->valueint == 1) {
        has_new_version_ = true;
      }
    }
  }

  cJSON_Delete(root);
  return ESP_OK;
}

std::string Ota::BuildActivateUrl() const { return JoinUrl(ota_url_, "activate"); }

esp_err_t Ota::Activate() {
  if (!has_activation_challenge_) {
    ESP_LOGW(kTag, "no activation challenge found");
    return ESP_FAIL;
  }

  int status_code = 0;
  std::string response;
  const std::string payload =
      SystemIdentity::BuildActivationPayload(activation_challenge_);
  const esp_err_t err = PerformJsonRequest("POST", BuildActivateUrl(), payload,
                                           &status_code, &response);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "activation request failed: %s", esp_err_to_name(err));
    return err;
  }
  if (status_code == 202) {
    return ESP_ERR_TIMEOUT;
  }
  if (status_code != 200) {
    ESP_LOGE(kTag, "activation status=%d body=%s", status_code,
             response.c_str());
    return ESP_FAIL;
  }
  return ESP_OK;
}

void Ota::MarkCurrentVersionValid() {
  const esp_partition_t *partition = esp_ota_get_running_partition();
  if (partition == nullptr || std::strcmp(partition->label, "factory") == 0) {
    return;
  }

  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (esp_ota_get_state_partition(partition, &state) != ESP_OK) {
    return;
  }
  if (state == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
  }
}

bool Ota::Upgrade(const std::string &firmware_url,
                  std::function<void(int progress, size_t speed)> callback) {
  esp_http_client_config_t config = {};
  config.url = firmware_url.c_str();
  config.timeout_ms = 30000;
  config.buffer_size = kHttpBufferSize;
  config.buffer_size_tx = kHttpBufferSize;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    return false;
  }
  esp_http_client_set_method(client, HTTP_METHOD_GET);
  if (esp_http_client_open(client, 0) != ESP_OK) {
    esp_http_client_cleanup(client);
    return false;
  }
  if (esp_http_client_fetch_headers(client) < 0 ||
      esp_http_client_get_status_code(client) != 200) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }

  const esp_partition_t *partition = esp_ota_get_next_update_partition(nullptr);
  if (partition == nullptr) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }

  esp_ota_handle_t update_handle = 0;
  if (esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle) !=
      ESP_OK) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }

  const int content_length = esp_http_client_get_content_length(client);
  std::unique_ptr<char, decltype(&heap_caps_free)> buffer(
      static_cast<char *>(heap_caps_malloc(kHttpBufferSize, MALLOC_CAP_INTERNAL)),
      &heap_caps_free);
  if (!buffer) {
    esp_ota_abort(update_handle);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }

  size_t total_read = 0;
  size_t recent_read = 0;
  int64_t last_report_time = esp_timer_get_time();
  while (true) {
    const int read = esp_http_client_read(client, buffer.get(), kHttpBufferSize);
    if (read < 0) {
      esp_ota_abort(update_handle);
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      return false;
    }
    if (read == 0) {
      break;
    }
    if (esp_ota_write(update_handle, buffer.get(), read) != ESP_OK) {
      esp_ota_abort(update_handle);
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      return false;
    }
    total_read += read;
    recent_read += read;
    const int64_t now = esp_timer_get_time();
    if (now - last_report_time >= 1000000 || total_read == content_length) {
      const int progress =
          content_length > 0 ? static_cast<int>((total_read * 100) / content_length)
                             : 0;
      if (callback) {
        callback(progress, recent_read);
      }
      last_report_time = now;
      recent_read = 0;
    }
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  if (esp_ota_end(update_handle) != ESP_OK) {
    return false;
  }
  if (esp_ota_set_boot_partition(partition) != ESP_OK) {
    return false;
  }
  return true;
}

bool Ota::StartUpgrade(
    std::function<void(int progress, size_t speed)> callback) {
  return Upgrade(firmware_url_, std::move(callback));
}

}  // namespace official_chat
