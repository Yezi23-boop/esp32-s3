#include "board_metadata.h"

#include <cstddef>
#include <cstdint>

#include <cJSON.h>
#include <esp_log.h>

namespace official_chat {

namespace {

constexpr char kTag[] = "official_board_meta";
constexpr char kFallbackBoardName[] = "esp32-s3-touch-amoled-2.06";

extern const uint8_t kBoardMetadataJsonStart[] asm(
    "_binary_esp32_s3_touch_amoled_2_06_json_start");
extern const uint8_t kBoardMetadataJsonEnd[] asm(
    "_binary_esp32_s3_touch_amoled_2_06_json_end");

void ApplyDisplayMetadata(const cJSON *display, BoardMetadata *metadata) {
  if (!cJSON_IsObject(display) || metadata == nullptr) {
    return;
  }

  const cJSON *width = cJSON_GetObjectItem(display, "width");
  const cJSON *height = cJSON_GetObjectItem(display, "height");
  const cJSON *monochrome = cJSON_GetObjectItem(display, "monochrome");
  if (!cJSON_IsNumber(width) || !cJSON_IsNumber(height)) {
    return;
  }

  metadata->display_width = width->valueint;
  metadata->display_height = height->valueint;
  metadata->display_monochrome = cJSON_IsTrue(monochrome);
  metadata->has_display = metadata->display_width > 0 &&
                          metadata->display_height > 0;
}

}  // namespace

BoardMetadata LoadCurrentBoardMetadata() {
  BoardMetadata metadata;
  metadata.type = kFallbackBoardName;
  metadata.name = kFallbackBoardName;

  const char *json_start =
      reinterpret_cast<const char *>(kBoardMetadataJsonStart);
  const size_t json_size =
      static_cast<size_t>(kBoardMetadataJsonEnd - kBoardMetadataJsonStart);
  if (json_size == 0) {
    ESP_LOGW(kTag, "embedded board metadata is empty, fallback to defaults");
    return metadata;
  }

  cJSON *root = cJSON_ParseWithLength(json_start, json_size);
  if (root == nullptr) {
    ESP_LOGW(kTag, "failed to parse embedded board metadata, fallback");
    return metadata;
  }

  const cJSON *builds = cJSON_GetObjectItem(root, "builds");
  const cJSON *build = cJSON_IsArray(builds) ? cJSON_GetArrayItem(builds, 0)
                                             : nullptr;
  const cJSON *name = cJSON_IsObject(build) ? cJSON_GetObjectItem(build, "name")
                                            : nullptr;
  if (cJSON_IsString(name) && name->valuestring != nullptr &&
      name->valuestring[0] != '\0') {
    metadata.type = name->valuestring;
    metadata.name = name->valuestring;
  }

  if (cJSON_IsObject(build)) {
    ApplyDisplayMetadata(cJSON_GetObjectItem(build, "display"), &metadata);
  }
  if (!metadata.has_display) {
    ApplyDisplayMetadata(cJSON_GetObjectItem(root, "display"), &metadata);
  }

  cJSON_Delete(root);
  return metadata;
}

}  // namespace official_chat
