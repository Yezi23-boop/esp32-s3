#ifndef MAIN_APP_RESOURCE_FS_H_
#define MAIN_APP_RESOURCE_FS_H_

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 挂载通用资源 LittleFS 分区到 `/resources`。
 *
 * 该分区面向 LVGL binfont、图片、音频片段和其他可替换资源；LVGL 通过
 * POSIX `A:` 盘符访问 `/resources` 下的文件。
 *
 * @return `ESP_OK` 表示挂载成功；其他值表示资源分区暂不可用。
 */
esp_err_t resource_fs_init(void);

#ifdef __cplusplus
}
#endif

#endif  // MAIN_APP_RESOURCE_FS_H_
