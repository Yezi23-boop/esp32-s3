/**
 * @file sd_manager.h
 * @brief SD 卡管理器对外接口。
 */

#pragma once

#include <stdbool.h>
#include <stdio.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 初始化 SD 卡并挂载文件系统。
     * @return `ESP_OK` 表示挂载成功；其他错误表示总线初始化或挂载失败。
     */
    esp_err_t sd_manager_init(void);

    /**
     * @brief 卸载 SD 卡文件系统并释放资源。
     * @return 无返回值。
     */
    void sd_manager_deinit(void);

    /**
     * @brief 列出指定目录的所有内容。
     * @param[in] path 目录路径。
     * @return 无返回值。
     */
    void sd_manager_list_dir(const char *path);

    /**
     * @brief 检查指定文件是否存在。
     * @param[in] file_path 文件路径。
     * @return true 表示文件存在。
     */
    bool sd_manager_file_exists(const char *file_path);

    /**
     * @brief 从 SD 卡读取文件内容。
     * @param[in] file_path 文件路径。
     * @param[out] buffer 接收数据的缓冲区。
     * @param[in] buffer_size 缓冲区大小，单位为字节。
     * @param[out] bytes_read 实际读取的字节数，可为 NULL。
     * @return `ESP_OK` 表示成功；其他错误表示参数、打开文件或读取失败。
     */
    esp_err_t sd_manager_read_file(const char *file_path, void *buffer, size_t buffer_size, size_t *bytes_read);

    /**
     * @brief 将数据写入 SD 卡文件。
     * @param[in] file_path 文件路径。
     * @param[in] data 要写入的数据。
     * @param[in] data_size 数据大小，单位为字节。
     * @return `ESP_OK` 表示成功；其他错误表示参数、打开文件或写入失败。
     */
    esp_err_t sd_manager_write_file(const char *file_path, const void *data, size_t data_size);

    /**
     * @brief 创建新目录。
     * @param[in] dir_path 目录路径。
     * @return `ESP_OK` 表示成功；其他错误表示创建失败。
     */
    esp_err_t sd_manager_create_dir(const char *dir_path);

    /**
     * @brief 删除文件。
     * @param[in] file_path 文件路径。
     * @return `ESP_OK` 表示成功；其他错误表示删除失败。
     */
    esp_err_t sd_manager_delete_file(const char *file_path);

    /**
     * @brief 获取文件大小。
     * @param[in] file_path 文件路径。
     * @param[out] file_size 用于接收文件大小的指针。
     * @return `ESP_OK` 表示成功；其他错误表示参数或查询失败。
     */
    esp_err_t sd_manager_get_file_size(const char *file_path, size_t *file_size);

    /**
     * @brief 重命名或移动文件。
     * @param[in] old_path 原文件路径。
     * @param[in] new_path 新文件路径。
     * @return `ESP_OK` 表示成功；其他错误表示参数或重命名失败。
     */
    esp_err_t sd_manager_rename_file(const char *old_path, const char *new_path);

#ifdef __cplusplus
}
#endif
