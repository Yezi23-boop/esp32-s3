#include "ui_font_assets.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "cbin_font_bridge.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "gui_guider.h"

static const char *TAG = "ui_font_assets";
// 这里通过 cbin_font_bridge_create 间接使用上游的 cbin_font_create。

#ifndef UI_FONT_ASSETS_BUILTIN_TEXT_NAME
#define UI_FONT_ASSETS_BUILTIN_TEXT_NAME "font_puhui_common_20_4.bin"
#endif

#ifndef UI_FONT_ASSETS_BUILTIN_TEXT_START_SYMBOL
#define UI_FONT_ASSETS_BUILTIN_TEXT_START_SYMBOL "_binary_font_puhui_common_20_4_bin_start"
#endif

#ifndef UI_FONT_ASSETS_BUILTIN_TEXT_END_SYMBOL
#define UI_FONT_ASSETS_BUILTIN_TEXT_END_SYMBOL "_binary_font_puhui_common_20_4_bin_end"
#endif

extern const uint8_t s_builtin_text_font_start[] asm(
    UI_FONT_ASSETS_BUILTIN_TEXT_START_SYMBOL);
extern const uint8_t s_builtin_text_font_end[] asm(
    UI_FONT_ASSETS_BUILTIN_TEXT_END_SYMBOL);

enum {
    UI_FONT_ASSETS_NAME_LEN = 32,
    UI_FONT_ASSETS_ENTRY_SIZE = 44,
    UI_FONT_ASSETS_HEADER_SIZE = 12,
};

typedef struct {
    char name[UI_FONT_ASSETS_NAME_LEN + 1];
    uint32_t size;
    uint32_t offset;
    uint16_t width;
    uint16_t height;
} ui_font_assets_entry_t;

typedef struct {
    bool init_done;
    bool ready;
    esp_err_t init_error;
    const esp_partition_t *partition;
    esp_partition_mmap_handle_t mmap_handle;
    const uint8_t *mmap_root;
    lv_font_t *builtin_text_font;
    lv_font_t *title_font;
    lv_font_t *body_font;
    lv_font_t *meta_font;
    lv_font_t *icon_font;
} ui_font_assets_runtime_t;

static ui_font_assets_runtime_t s_runtime = {0};

/**
 * @brief 返回随固件嵌入的默认 cbin 中文字体。
 *
 * assets 分区只作为运行时可替换资源；缺少或校验失败时，AI UI 仍然依赖
 * 这个内置 cbin 字体保证中文文本可显示。
 *
 * @return 内置 cbin 字体指针；极端内存不足时返回 GUI Guider 字体兜底。
 */
static const lv_font_t *ui_font_assets_builtin_text(void) {
    if (s_runtime.builtin_text_font != NULL) {
        return s_runtime.builtin_text_font;
    }
    return &lv_font_SourceHanSerifSC_Regular_22;
}

/**
 * @brief 返回内置图标/符号文本的兜底字体。
 *
 * 当前 xiaozhi 字体只承接中文文本默认路径，图标字体仍沿用 GUI Guider
 * 生成资源，避免把文本字体误用于符号位图。
 *
 * @return 编译进固件的图标兜底字体指针。
 */
static const lv_font_t *ui_font_assets_compiled_icon(void) {
    return &lv_font_montserratMedium_16;
}

static uint32_t ui_font_assets_read_u32(const uint8_t *data) {
    return ((uint32_t)data[0]) | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint32_t ui_font_assets_compute_checksum(const uint8_t *data,
                                                uint32_t length) {
    uint32_t checksum = 0;
    for (uint32_t i = 0; i < length; ++i) {
        checksum += data[i];
    }
    return checksum & 0xFFFF;
}

static bool ui_font_assets_read_entry(const uint8_t *table, size_t index,
                                      ui_font_assets_entry_t *entry) {
    const uint8_t *cursor = table + (index * UI_FONT_ASSETS_ENTRY_SIZE);
    memcpy(entry->name, cursor, UI_FONT_ASSETS_NAME_LEN);
    entry->name[UI_FONT_ASSETS_NAME_LEN] = '\0';
    entry->size = ui_font_assets_read_u32(cursor + UI_FONT_ASSETS_NAME_LEN);
    entry->offset = ui_font_assets_read_u32(cursor + UI_FONT_ASSETS_NAME_LEN + 4);
    entry->width = (uint16_t)(cursor[UI_FONT_ASSETS_NAME_LEN + 8] |
                              ((uint16_t)cursor[UI_FONT_ASSETS_NAME_LEN + 9] << 8));
    entry->height = (uint16_t)(cursor[UI_FONT_ASSETS_NAME_LEN + 10] |
                               ((uint16_t)cursor[UI_FONT_ASSETS_NAME_LEN + 11] << 8));
    return true;
}

static bool ui_font_assets_entry_matches(const ui_font_assets_entry_t *entry,
                                         const char *name) {
    return strncmp(entry->name, name, UI_FONT_ASSETS_NAME_LEN) == 0;
}

static bool ui_font_assets_find_entry(const uint8_t *table, size_t total_files,
                                      const char *name,
                                      ui_font_assets_entry_t *entry) {
    for (size_t i = 0; i < total_files; ++i) {
        ui_font_assets_entry_t candidate = {0};
        ui_font_assets_read_entry(table, i, &candidate);
        if (ui_font_assets_entry_matches(&candidate, name)) {
            if (entry != NULL) {
                *entry = candidate;
            }
            return true;
        }
    }
    return 0;
}

static const uint8_t *ui_font_assets_entry_data(
    const uint8_t *combined_data, size_t combined_len,
    const ui_font_assets_entry_t *entry) {
    if (entry->offset > combined_len) {
        return NULL;
    }
    if (entry->size > combined_len - entry->offset) {
        return NULL;
    }
    if (entry->size < 2) {
        return NULL;
    }
    if (entry->offset + 2 > combined_len) {
        return NULL;
    }
    if (entry->size > combined_len - entry->offset - 2) {
        return NULL;
    }
    return combined_data + entry->offset + 2;
}

static void ui_font_assets_destroy_font(lv_font_t **font) {
    if (font != NULL && *font != NULL) {
        cbin_font_bridge_destroy(*font);
        *font = NULL;
    }
}

static void ui_font_assets_reset_runtime(void) {
    if (s_runtime.icon_font != NULL && s_runtime.icon_font != s_runtime.title_font) {
        cbin_font_bridge_destroy(s_runtime.icon_font);
    }
    s_runtime.icon_font = NULL;

    ui_font_assets_destroy_font(&s_runtime.title_font);
    s_runtime.body_font = NULL;
    s_runtime.meta_font = NULL;

    if (s_runtime.mmap_root != NULL) {
        esp_partition_munmap(s_runtime.mmap_handle);
    }
    s_runtime.mmap_root = NULL;
    s_runtime.mmap_handle = 0;
    s_runtime.partition = NULL;
    s_runtime.ready = false;
}

/**
 * @brief 从编译期嵌入的 cbin 数据创建默认文本字体。
 *
 * `BUILTIN_TEXT_FONT` 由 `main/CMakeLists.txt` 选择，C 侧只依赖稳定的
 * 二进制符号定义；这样更换内置字体时无需修改源码。
 *
 * @return 创建成功返回 ESP_OK；内存不足时返回 ESP_ERR_NO_MEM。
 */
static esp_err_t ui_font_assets_create_builtin_text(void) {
    if (s_runtime.builtin_text_font != NULL) {
        return ESP_OK;
    }

    const size_t font_size =
        (size_t)(s_builtin_text_font_end - s_builtin_text_font_start);
    s_runtime.builtin_text_font =
        cbin_font_bridge_create((void *)s_builtin_text_font_start);
    if (s_runtime.builtin_text_font == NULL) {
        ESP_LOGE(TAG, "failed to create builtin text font: %s",
                 UI_FONT_ASSETS_BUILTIN_TEXT_NAME);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "builtin text font ready: %s, size=%u",
             UI_FONT_ASSETS_BUILTIN_TEXT_NAME, (unsigned int)font_size);
    return ESP_OK;
}

static esp_err_t ui_font_assets_load_from_partition(void) {
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "assets");
    if (partition == NULL) {
        ESP_LOGW(TAG, "assets partition not found, fallback to compiled fonts");
        return ESP_ERR_NOT_FOUND;
    }

    const void *mapped_root = NULL;
    esp_partition_mmap_handle_t mmap_handle = 0;
    esp_err_t err = esp_partition_mmap(partition, 0, partition->size,
                                       ESP_PARTITION_MMAP_DATA, &mapped_root,
                                       &mmap_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_partition_mmap failed for assets: %s",
                 esp_err_to_name(err));
        return err;
    }

    s_runtime.partition = partition;
    s_runtime.mmap_handle = mmap_handle;
    s_runtime.mmap_root = (const uint8_t *)mapped_root;

    const uint8_t *root = s_runtime.mmap_root;
    const uint32_t total_files = ui_font_assets_read_u32(root);
    const uint32_t stored_checksum = ui_font_assets_read_u32(root + 4);
    const uint32_t stored_len = ui_font_assets_read_u32(root + 8);
    const size_t table_len = (size_t)total_files * UI_FONT_ASSETS_ENTRY_SIZE;
    const size_t image_len = UI_FONT_ASSETS_HEADER_SIZE + stored_len;

    if (total_files == 0 || image_len > partition->size) {
        ESP_LOGE(TAG, "invalid assets package: files=%" PRIu32 ", stored_len=%" PRIu32,
                 total_files, stored_len);
        return ESP_ERR_INVALID_SIZE;
    }

    if (stored_len < table_len) {
        ESP_LOGE(TAG,
                 "invalid assets package layout: stored_len=%" PRIu32
                 " smaller than table_len=%u",
                 stored_len, (unsigned int)table_len);
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *table = root + UI_FONT_ASSETS_HEADER_SIZE;
    const size_t combined_len = stored_len - table_len;
    const uint8_t *combined_data = table + table_len;
    const uint32_t calculated_checksum =
        ui_font_assets_compute_checksum(root + UI_FONT_ASSETS_HEADER_SIZE, stored_len);

    if (calculated_checksum != stored_checksum) {
        ESP_LOGE(TAG,
                 "assets checksum mismatch: calculated=0x%08" PRIx32
                 ", stored=0x%08" PRIx32,
                 calculated_checksum, stored_checksum);
        return ESP_ERR_INVALID_CRC;
    }

    ESP_LOGI(TAG,
             "assets image loaded: files=%" PRIu32 ", checksum=0x%08" PRIx32
             ", stored_len=%" PRIu32 ", data_len=%u",
             total_files, stored_checksum, stored_len, (unsigned int)combined_len);

    ui_font_assets_entry_t index_entry = {0};
    if (!ui_font_assets_find_entry(table, total_files, "index.json", &index_entry)) {
        ESP_LOGE(TAG, "index.json not found in assets package");
        return ESP_ERR_NOT_FOUND;
    }

    const uint8_t *index_data =
        ui_font_assets_entry_data(combined_data, combined_len, &index_entry);
    if (index_data == NULL) {
        ESP_LOGE(TAG, "index.json entry is out of range");
        return ESP_ERR_INVALID_SIZE;
    }

    cJSON *index_root = cJSON_ParseWithLength((const char *)index_data, index_entry.size);
    if (index_root == NULL) {
        ESP_LOGE(TAG, "failed to parse index.json");
        return ESP_FAIL;
    }

    cJSON *text_item = cJSON_GetObjectItem(index_root, "text_font");
    if (!cJSON_IsString(text_item) || text_item->valuestring == NULL ||
        text_item->valuestring[0] == '\0') {
        cJSON_Delete(index_root);
        ESP_LOGE(TAG, "index.json missing text_font");
        return ESP_ERR_INVALID_ARG;
    }
    const char *text_font_name = text_item->valuestring;
    char text_font_name_copy[UI_FONT_ASSETS_NAME_LEN + 1] = {0};
    strncpy(text_font_name_copy, text_font_name, UI_FONT_ASSETS_NAME_LEN);

    ui_font_assets_entry_t text_entry = {0};
    if (!ui_font_assets_find_entry(table, total_files, text_font_name, &text_entry)) {
        cJSON_Delete(index_root);
        ESP_LOGE(TAG, "text font asset not found: %s", text_font_name);
        return ESP_ERR_NOT_FOUND;
    }

    const uint8_t *text_data =
        ui_font_assets_entry_data(combined_data, combined_len, &text_entry);
    if (text_data == NULL) {
        cJSON_Delete(index_root);
        ESP_LOGE(TAG, "text font asset is out of range: %s",
                 text_font_name);
        return ESP_ERR_INVALID_SIZE;
    }

    s_runtime.title_font = cbin_font_bridge_create((void *)text_data);
    if (s_runtime.title_font == NULL) {
        cJSON_Delete(index_root);
        ESP_LOGE(TAG, "failed to create title font from %s", text_item->valuestring);
        return ESP_ERR_NO_MEM;
    }
    s_runtime.body_font = s_runtime.title_font;
    s_runtime.meta_font = s_runtime.title_font;

    cJSON *icon_item = cJSON_GetObjectItem(index_root, "icon_font");
    if (cJSON_IsString(icon_item) && icon_item->valuestring != NULL &&
        icon_item->valuestring[0] != '\0') {
        ui_font_assets_entry_t icon_entry = {0};
        if (ui_font_assets_find_entry(table, total_files, icon_item->valuestring,
                                      &icon_entry)) {
            const uint8_t *icon_data =
                ui_font_assets_entry_data(combined_data, combined_len, &icon_entry);
            if (icon_data != NULL) {
                s_runtime.icon_font = cbin_font_bridge_create((void *)icon_data);
                if (s_runtime.icon_font == NULL) {
                    ESP_LOGW(TAG, "failed to create icon font from %s, keep fallback",
                             icon_item->valuestring);
                }
            } else {
                ESP_LOGW(TAG, "icon font asset out of range: %s",
                         icon_item->valuestring);
            }
        } else {
            ESP_LOGW(TAG, "icon font asset not found: %s", icon_item->valuestring);
        }
    } else {
        ESP_LOGI(TAG, "index.json has no icon_font, use compiled icon fallback");
    }

    cJSON_Delete(index_root);
    s_runtime.ready = true;
    s_runtime.init_error = ESP_OK;
    ESP_LOGI(TAG, "font assets ready: title/body/meta use %s", text_font_name_copy);
    return ESP_OK;
}

/**
 * @brief 初始化运行时字体替换层。
 *
 * 该函数先从固件内嵌 cbin 数据创建默认字体，再尝试从 assets 分区加载
 * 可替换字体；无分区、CRC 失败或资源缺失都不会影响主路径。
 *
 * @return 默认内置字体可用返回 ESP_OK，否则返回具体错误码。
 */
esp_err_t ui_font_assets_init(void) {
    if (s_runtime.init_done) {
        return s_runtime.builtin_text_font != NULL ? ESP_OK : ESP_ERR_NO_MEM;
    }

    s_runtime.init_done = true;
    esp_err_t builtin_err = ui_font_assets_create_builtin_text();
    if (builtin_err != ESP_OK) {
        return builtin_err;
    }

    s_runtime.init_error = ui_font_assets_load_from_partition();
    if (s_runtime.init_error != ESP_OK) {
        ESP_LOGW(TAG, "runtime font assets unavailable, keep builtin font: %s",
                 esp_err_to_name(s_runtime.init_error));
        ui_font_assets_reset_runtime();
    }
    return ESP_OK;
}

/**
 * @brief 查询 assets 运行时字体是否已经覆盖默认编译字体。
 *
 * 返回 true 仅代表可替换字体资源加载成功；返回 false 不代表 UI 字体不可用，
 * 因为默认 cbin 字体已经嵌入固件。
 *
 * @return assets 运行时字体可用返回 true，否则返回 false。
 */
bool ui_font_assets_ready(void) {
    if (!s_runtime.init_done) {
        (void)ui_font_assets_init();
    }
    return s_runtime.ready;
}

/**
 * @brief 获取 AI 页面标题字体。
 *
 * 优先使用 assets 分区加载出的运行时替换字体；不可用时回落到
 * 固件内嵌 cbin 字体。
 *
 * @return 标题字体指针。
 */
const lv_font_t *ui_font_assets_title(void) {
    if (!s_runtime.init_done) {
        (void)ui_font_assets_init();
    }
    return s_runtime.ready && s_runtime.title_font != NULL
               ? s_runtime.title_font
               : ui_font_assets_builtin_text();
}

/**
 * @brief 获取 AI 页面正文和按钮字体。
 *
 * 优先使用 assets 分区加载出的运行时替换字体；不可用时回落到
 * 固件内嵌 cbin 字体。
 *
 * @return 正文字体指针。
 */
const lv_font_t *ui_font_assets_body(void) {
    if (!s_runtime.init_done) {
        (void)ui_font_assets_init();
    }
    return s_runtime.ready && s_runtime.body_font != NULL ? s_runtime.body_font
                                                          : ui_font_assets_builtin_text();
}

/**
 * @brief 获取 AI 页面辅助信息字体。
 *
 * 优先使用 assets 分区加载出的运行时替换字体；不可用时回落到
 * 固件内嵌 cbin 字体。
 *
 * @return 辅助信息字体指针。
 */
const lv_font_t *ui_font_assets_meta(void) {
    if (!s_runtime.init_done) {
        (void)ui_font_assets_init();
    }
    return s_runtime.ready && s_runtime.meta_font != NULL
               ? s_runtime.meta_font
               : ui_font_assets_builtin_text();
}

/**
 * @brief 获取 AI 页面图标字体。
 *
 * 如果 assets 分区提供了图标字体则使用运行时替换版本；否则继续使用
 * GUI Guider 生成的内置符号字体。
 *
 * @return 图标字体指针。
 */
const lv_font_t *ui_font_assets_icon(void) {
    if (!s_runtime.init_done) {
        (void)ui_font_assets_init();
    }
    return s_runtime.ready && s_runtime.icon_font != NULL ? s_runtime.icon_font
                                                          : ui_font_assets_compiled_icon();
}
