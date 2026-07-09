#include "resource_fs.h"

#include <stdbool.h>

#include "esp_littlefs.h"
#include "esp_log.h"

static const char *TAG = "RESOURCE_FS";
// 通用资源分区挂载点，必须与 `CONFIG_LV_FS_POSIX_PATH` 保持一致。
static const char *kResourceFsBasePath = "/resources";
// 分区表中的 LittleFS 分区名称。
static const char *kResourceFsPartitionLabel = "resources";

/**
 * @brief 挂载通用资源 LittleFS 分区到 `/resources`。
 *
 * 首次挂载失败时允许格式化，便于新分区首次烧录后自动可用；后续 LVGL 的
 * POSIX `A:` 盘符会把路径拼到 `/resources` 下读取资源文件。
 *
 * @return `ESP_OK` 表示挂载成功；其他值表示资源分区暂不可用。
 */
esp_err_t resource_fs_init(void)
{
    // 防止启动流程重复注册 VFS；重复注册同一路径会返回错误。
    static bool s_resource_fs_mounted = false;
    if (s_resource_fs_mounted) {
        return ESP_OK;
    }

    const esp_vfs_littlefs_conf_t config = {
        .base_path = kResourceFsBasePath,
        .partition_label = kResourceFsPartitionLabel,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed at %s: %s", kResourceFsBasePath,
                 esp_err_to_name(err));
        return err;
    }

    s_resource_fs_mounted = true;

    // 用挂载后的容量日志确认镜像是否按预期写入，单位字节。
    size_t total_bytes = 0;
    size_t used_bytes = 0;
    err = esp_littlefs_info(kResourceFsPartitionLabel, &total_bytes, &used_bytes);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS mounted: %s total=%u used=%u", kResourceFsBasePath,
                 (unsigned int)total_bytes, (unsigned int)used_bytes);
    } else {
        ESP_LOGW(TAG, "LittleFS info failed for %s: %s", kResourceFsPartitionLabel,
                 esp_err_to_name(err));
    }

    return ESP_OK;
}
