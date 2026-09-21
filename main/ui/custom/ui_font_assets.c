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
#include "spi_flash_mmap.h"

static const char *TAG = "ui_font_assets";
static const char kAssetsPartitionLabel[] = "assets";
static const char kExpectedBundle[] = "noto-v1";
static const char kExpectedCharset[] = "common";
static const char kTextFontName[] = "font_noto_sans_common_20_4.bin";
static const char kHermesFontName[] = "font_noto_sans_common_16_4.bin";

enum {
    UI_FONT_ASSETS_NAME_LEN = 32,
    UI_FONT_ASSETS_ENTRY_SIZE = 44,
    UI_FONT_ASSETS_HEADER_SIZE = 12,
    UI_FONT_ASSETS_INDEX_VERSION = 2,
    UI_FONT_ASSETS_BPP = 4,
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
    lv_font_t *text_font;
    lv_font_t *hermes_font;
} ui_font_assets_runtime_t;

static ui_font_assets_runtime_t s_runtime = {0};

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

static void ui_font_assets_read_entry(const uint8_t *table, size_t index,
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
}

static bool ui_font_assets_find_entry(const uint8_t *table, size_t total_files,
                                      const char *name,
                                      ui_font_assets_entry_t *entry) {
    for (size_t i = 0; i < total_files; ++i) {
        ui_font_assets_entry_t candidate = {0};
        ui_font_assets_read_entry(table, i, &candidate);
        if (strncmp(candidate.name, name, UI_FONT_ASSETS_NAME_LEN) == 0) {
            if (entry != NULL) {
                *entry = candidate;
            }
            return true;
        }
    }
    return false;
}

static const uint8_t *ui_font_assets_entry_data(
    const uint8_t *combined_data, size_t combined_len,
    const ui_font_assets_entry_t *entry) {
    if (entry->offset > combined_len) {
        return NULL;
    }

    const size_t available = combined_len - entry->offset;
    if (available < 2 || entry->size > available - 2) {
        return NULL;
    }

    const uint8_t *entry_data = combined_data + entry->offset;
    if (entry_data[0] != 'Z' || entry_data[1] != 'Z') {
        return NULL;
    }
    return entry_data + 2;
}

static void ui_font_assets_destroy_font(lv_font_t **font) {
    if (font != NULL && *font != NULL) {
        cbin_font_bridge_destroy(*font);
        *font = NULL;
    }
}

static void ui_font_assets_reset_runtime(void) {
    ui_font_assets_destroy_font(&s_runtime.hermes_font);
    ui_font_assets_destroy_font(&s_runtime.text_font);

    if (s_runtime.mmap_root != NULL) {
        esp_partition_munmap(s_runtime.mmap_handle);
    }
    s_runtime.mmap_root = NULL;
    s_runtime.mmap_handle = 0;
    s_runtime.partition = NULL;
    s_runtime.ready = false;
}

static bool ui_font_assets_json_string_matches(const cJSON *object,
                                               const char *key,
                                               const char *expected) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring != NULL &&
           strcmp(item->valuestring, expected) == 0;
}

static bool ui_font_assets_json_number_matches(const cJSON *object,
                                               const char *key, int expected) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(item) && item->valueint == expected;
}

static esp_err_t ui_font_assets_validate_index(const cJSON *index_root) {
    if (!ui_font_assets_json_number_matches(index_root, "version",
                                            UI_FONT_ASSETS_INDEX_VERSION) ||
        !ui_font_assets_json_string_matches(index_root, "bundle",
                                            kExpectedBundle)) {
        ESP_LOGE(TAG, "index.json version or bundle mismatch");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t ui_font_assets_load_font(
    const cJSON *index_root, const uint8_t *table, size_t total_files,
    const uint8_t *combined_data, size_t combined_len, const char *font_key,
    const char *meta_key, const char *expected_name, int expected_size,
    lv_font_t **font_out) {
    const cJSON *font_item = cJSON_GetObjectItemCaseSensitive(index_root, font_key);
    const cJSON *meta_item = cJSON_GetObjectItemCaseSensitive(index_root, meta_key);
    if (!cJSON_IsString(font_item) || font_item->valuestring == NULL ||
        strcmp(font_item->valuestring, expected_name) != 0 ||
        !cJSON_IsObject(meta_item) ||
        !ui_font_assets_json_string_matches(meta_item, "bundle", kExpectedBundle) ||
        !ui_font_assets_json_string_matches(meta_item, "charset", kExpectedCharset) ||
        !ui_font_assets_json_number_matches(meta_item, "size", expected_size) ||
        !ui_font_assets_json_number_matches(meta_item, "bpp", UI_FONT_ASSETS_BPP)) {
        ESP_LOGE(TAG, "index.json %s metadata mismatch", font_key);
        return ESP_ERR_INVALID_ARG;
    }

    ui_font_assets_entry_t entry = {0};
    if (!ui_font_assets_find_entry(table, total_files, expected_name, &entry)) {
        ESP_LOGE(TAG, "font asset not found: %s", expected_name);
        return ESP_ERR_NOT_FOUND;
    }

    const uint8_t *font_data =
        ui_font_assets_entry_data(combined_data, combined_len, &entry);
    if (font_data == NULL) {
        ESP_LOGE(TAG, "invalid ZZ cbin asset: %s", expected_name);
        return ESP_ERR_INVALID_SIZE;
    }

    *font_out = cbin_font_bridge_create((void *)font_data);
    if (*font_out == NULL) {
        ESP_LOGE(TAG, "failed to create cbin font: %s", expected_name);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t ui_font_assets_load_from_partition(void) {
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kAssetsPartitionLabel);
    if (partition == NULL) {
        ESP_LOGE(TAG, "assets partition not found");
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t header[UI_FONT_ASSETS_HEADER_SIZE] = {0};
    esp_err_t err = esp_partition_read(partition, 0, header, sizeof(header));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to read assets header: %s", esp_err_to_name(err));
        return err;
    }

    const uint32_t total_files = ui_font_assets_read_u32(header);
    const uint32_t stored_len = ui_font_assets_read_u32(header + 8);
    if (partition->size < UI_FONT_ASSETS_HEADER_SIZE || total_files == 0 ||
        total_files > UINT32_MAX / UI_FONT_ASSETS_ENTRY_SIZE ||
        stored_len > partition->size - UI_FONT_ASSETS_HEADER_SIZE) {
        ESP_LOGE(TAG, "invalid assets header: files=%" PRIu32 ", stored_len=%" PRIu32,
                 total_files, stored_len);
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t table_len = (size_t)total_files * UI_FONT_ASSETS_ENTRY_SIZE;
    if (stored_len < table_len) {
        ESP_LOGE(TAG, "invalid assets table length: stored=%" PRIu32 ", table=%u",
                 stored_len, (unsigned int)table_len);
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t image_len = UI_FONT_ASSETS_HEADER_SIZE + stored_len;
    const size_t required_pages =
        (image_len + SPI_FLASH_MMU_PAGE_SIZE - 1U) / SPI_FLASH_MMU_PAGE_SIZE;
    const uint32_t free_pages = spi_flash_mmap_get_free_pages(SPI_FLASH_MMAP_DATA);
    if (required_pages > free_pages) {
        ESP_LOGE(TAG, "assets mmap pages unavailable: need=%u free=%" PRIu32,
                 (unsigned int)required_pages, free_pages);
        return ESP_ERR_NO_MEM;
    }

    const void *mapped_root = NULL;
    esp_partition_mmap_handle_t mmap_handle = 0;
    err = esp_partition_mmap(partition, 0, image_len, ESP_PARTITION_MMAP_DATA,
                             &mapped_root, &mmap_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_partition_mmap failed for assets: %s",
                 esp_err_to_name(err));
        return err;
    }

    s_runtime.partition = partition;
    s_runtime.mmap_handle = mmap_handle;
    s_runtime.mmap_root = (const uint8_t *)mapped_root;

    const uint8_t *root = s_runtime.mmap_root;
    const uint32_t stored_checksum = ui_font_assets_read_u32(root + 4);
    const uint32_t mapped_files = ui_font_assets_read_u32(root);
    const uint32_t mapped_len = ui_font_assets_read_u32(root + 8);
    if (mapped_files != total_files || mapped_len != stored_len) {
        ESP_LOGE(TAG, "assets header changed while mapping");
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t calculated_checksum = ui_font_assets_compute_checksum(
        root + UI_FONT_ASSETS_HEADER_SIZE, stored_len);
    if (calculated_checksum != stored_checksum) {
        ESP_LOGE(TAG, "assets checksum mismatch: calculated=0x%08" PRIx32
                      ", stored=0x%08" PRIx32,
                 calculated_checksum, stored_checksum);
        return ESP_ERR_INVALID_CRC;
    }

    const uint8_t *table = root + UI_FONT_ASSETS_HEADER_SIZE;
    const size_t combined_len = stored_len - table_len;
    const uint8_t *combined_data = table + table_len;
    ui_font_assets_entry_t index_entry = {0};
    if (!ui_font_assets_find_entry(table, total_files, "index.json", &index_entry)) {
        ESP_LOGE(TAG, "index.json not found in assets package");
        return ESP_ERR_NOT_FOUND;
    }

    const uint8_t *index_data =
        ui_font_assets_entry_data(combined_data, combined_len, &index_entry);
    if (index_data == NULL) {
        ESP_LOGE(TAG, "invalid index.json asset");
        return ESP_ERR_INVALID_SIZE;
    }

    cJSON *index_root =
        cJSON_ParseWithLength((const char *)index_data, index_entry.size);
    if (index_root == NULL) {
        ESP_LOGE(TAG, "failed to parse index.json");
        return ESP_FAIL;
    }

    err = ui_font_assets_validate_index(index_root);
    if (err == ESP_OK) {
        err = ui_font_assets_load_font(index_root, table, total_files,
                                       combined_data, combined_len, "text_font",
                                       "text_font_meta", kTextFontName, 20,
                                       &s_runtime.text_font);
    }
    if (err == ESP_OK) {
        err = ui_font_assets_load_font(index_root, table, total_files,
                                       combined_data, combined_len,
                                       "hermes_text_font", "hermes_text_font_meta",
                                       kHermesFontName, 16,
                                       &s_runtime.hermes_font);
    }
    cJSON_Delete(index_root);
    if (err != ESP_OK) {
        return err;
    }

    s_runtime.ready = true;
    ESP_LOGI(TAG,
             "Noto font assets ready: bundle=%s text=common20 hermes=common16 "
             "image=%u pages=%u",
             kExpectedBundle, (unsigned int)image_len,
             (unsigned int)required_pages);
    return ESP_OK;
}

esp_err_t ui_font_assets_init(void) {
    if (s_runtime.init_done) {
        return s_runtime.init_error;
    }

    s_runtime.init_done = true;
    s_runtime.init_error = ui_font_assets_load_from_partition();
    if (s_runtime.init_error != ESP_OK) {
        ESP_LOGE(TAG, "Noto font assets unavailable: %s",
                 esp_err_to_name(s_runtime.init_error));
        ui_font_assets_reset_runtime();
    }
    return s_runtime.init_error;
}

bool ui_font_assets_ready(void) {
    if (!s_runtime.init_done) {
        (void)ui_font_assets_init();
    }
    return s_runtime.ready;
}

const lv_font_t *ui_font_assets_text(void) {
    if (!ui_font_assets_ready()) {
        return NULL;
    }
    return s_runtime.text_font;
}

const lv_font_t *ui_font_assets_hermes(void) {
    if (!ui_font_assets_ready()) {
        return NULL;
    }
    return s_runtime.hermes_font;
}

const lv_font_t *ui_font_assets_icon(void) {
    return &lv_font_montserratMedium_16;
}
