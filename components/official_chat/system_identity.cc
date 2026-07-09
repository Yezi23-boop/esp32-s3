#include "system_identity.h"

#include <cstdio>
#include <string>

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_efuse.h>
#include <esp_log.h>
#ifdef SOC_HMAC_SUPPORTED
#include <esp_hmac.h>
#endif

#include "settings.h"

extern "C" {
#include "system_util.h"
}

namespace official_chat {

namespace {

constexpr char kTag[] = "official_identity";
constexpr char kClientIdNamespace[] = "identity";
constexpr char kClientIdKey[] = "client_id";

std::string ReadSerialNumberInternal() {
#ifdef ESP_EFUSE_BLOCK_USR_DATA
  uint8_t serial_number[33] = {0};
  if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, serial_number,
                                32 * 8) != ESP_OK) {
    return {};
  }
  if (serial_number[0] == 0) {
    return {};
  }
  return std::string(reinterpret_cast<char *>(serial_number), 32);
#else
  return {};
#endif
}

}  // namespace

std::string SystemIdentity::GetDeviceId() { return get_mac_address(); }

std::string SystemIdentity::GetUserAgent() {
  const esp_app_desc_t *app_desc = esp_app_get_description();
  const char *project_name =
      app_desc != nullptr ? app_desc->project_name : "official_chat";
  const char *version = app_desc != nullptr ? app_desc->version : "unknown";
  char buffer[160];
  std::snprintf(buffer, sizeof(buffer), "%s/%s ESP-IDF/%s", project_name,
                version, esp_get_idf_version());
  return buffer;
}

std::string SystemIdentity::GetOrCreateClientId() {
  Settings settings(kClientIdNamespace, true);
  std::string client_id = settings.GetString(kClientIdKey);
  if (!client_id.empty()) {
    return client_id;
  }

  client_id = generate_uuid();
  settings.SetString(kClientIdKey, client_id);
  ESP_LOGI(kTag, "generated persistent client_id: %s", client_id.c_str());
  return client_id;
}

bool SystemIdentity::HasSerialNumber() { return !GetSerialNumber().empty(); }

std::string SystemIdentity::GetSerialNumber() {
  return ReadSerialNumberInternal();
}

int SystemIdentity::GetActivationVersion() {
  return HasSerialNumber() ? 2 : 1;
}

std::string SystemIdentity::BuildActivationPayload(
    const std::string &challenge) {
  if (!HasSerialNumber()) {
    return "{}";
  }

  std::string serial_number = GetSerialNumber();
  std::string hmac_hex;
#ifdef SOC_HMAC_SUPPORTED
  uint8_t hmac_result[32];
  const esp_err_t ret =
      esp_hmac_calculate(HMAC_KEY0,
                         reinterpret_cast<const uint8_t *>(challenge.data()),
                         challenge.size(), hmac_result);
  if (ret == ESP_OK) {
    char hex_buffer[3];
    for (size_t i = 0; i < sizeof(hmac_result); ++i) {
      std::snprintf(hex_buffer, sizeof(hex_buffer), "%02x", hmac_result[i]);
      hmac_hex.append(hex_buffer);
    }
  } else {
    ESP_LOGW(kTag, "failed to compute HMAC: %s", esp_err_to_name(ret));
  }
#endif

  cJSON *payload = cJSON_CreateObject();
  cJSON_AddStringToObject(payload, "algorithm", "hmac-sha256");
  cJSON_AddStringToObject(payload, "serial_number", serial_number.c_str());
  cJSON_AddStringToObject(payload, "challenge", challenge.c_str());
  cJSON_AddStringToObject(payload, "hmac", hmac_hex.c_str());
  char *json = cJSON_PrintUnformatted(payload);
  std::string result = json != nullptr ? json : "{}";
  if (json != nullptr) {
    cJSON_free(json);
  }
  cJSON_Delete(payload);
  return result;
}

}  // namespace official_chat
