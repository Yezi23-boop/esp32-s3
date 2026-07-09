/**
 * @file sd_manager.c
 * @brief ESP32 SD卡管理器实现 (SPI模式)
 * @details 实现SD卡的初始化、文件系统挂载、文件读写等核心功能
 */

#include "sd_manager.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"

#define TAG "sd_manager"
#define MOUNT_POINT "/sdcard"

// SPI 引脚定义来自当前板级原理图；这里固定走 SPI3，避免与显示链路占用同一主机。
#define PIN_NUM_MISO 3
#define PIN_NUM_MOSI 1
#define PIN_NUM_CLK 2
#define PIN_NUM_CS 17

static sdmmc_card_t *card = NULL;            // SD 卡设备句柄；非 NULL 表示已成功挂载。
static bool spi_initialized_by_sd = false;   // 标记 SPI 总线是否由本模块初始化，用于 deinit 决定是否释放总线。

/**
 * @brief 初始化 SD 卡并挂载文件系统。
 * @return `ESP_OK` 表示挂载成功；其他错误表示总线初始化或挂载失败。
 */
esp_err_t sd_manager_init(void)
{
    esp_err_t ret;

    // FAT 挂载参数倾向“稳定优先”而不是最大吞吐，避免在资源紧张时额外放大失败概率。
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024};

    ESP_LOGI(TAG, "初始化SD卡 (SPI模式)...");
    ESP_LOGI(TAG, "引脚配置: MOSI=%d, MISO=%d, CLK=%d, CS=%d",
             PIN_NUM_MOSI, PIN_NUM_MISO, PIN_NUM_CLK, PIN_NUM_CS);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    // 强制使用 SPI3_HOST，避免与屏幕的 SPI2_HOST/QSPI 链路冲突。
    host.slot = SPI3_HOST;

    /* 当前频率保持在 10MHz，优先换稳定性。
     * 若后续要提速，应逐步验证 CRC 错误和长时间读写稳定性。 */
    host.max_freq_khz = 10000;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK)
    {
        if (ret == ESP_ERR_INVALID_STATE)
        {
            ESP_LOGW(TAG, "SPI3总线已被初始化，跳过初始化步骤，尝试复用总线");
            spi_initialized_by_sd = false;
        }
        else
        {
            ESP_LOGE(TAG, "SPI3总线初始化失败: %s", esp_err_to_name(ret));
            return ret;
        }
    }
    else
    {
        spi_initialized_by_sd = true;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;

    ESP_LOGI(TAG, "正在挂载文件系统(SPI3)...");
    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(TAG, "挂载失败: 无法挂载文件系统。");
            ESP_LOGE(TAG, "如果这是新卡，可能需要先在电脑上格式化为FAT32。");
        }
        else
        {
            ESP_LOGE(TAG, "SD卡挂载失败 (%s). 请检查硬件连接。", esp_err_to_name(ret));
        }

        // 只有在本模块初始化了总线时才释放，避免误伤外部共享总线使用者。
        if (spi_initialized_by_sd)
        {
            spi_bus_free(host.slot);
        }
        return ret;
    }

    ESP_LOGI(TAG, "SD卡挂载成功！");
    sdmmc_card_print_info(stdout, card);

    return ESP_OK;
}

/**
 * @brief 卸载 SD 卡文件系统并释放资源。
 * @return 无返回值。
 */
void sd_manager_deinit(void)
{
    if (card != NULL)
    {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
        card = NULL;

        // 只有在本模块初始化了总线时才释放，避免误释放共享 SPI 主机。
        if (spi_initialized_by_sd)
        {
            spi_bus_free(SPI3_HOST);
            ESP_LOGI(TAG, "SPI3总线已释放");
        }

        ESP_LOGI(TAG, "SD卡已安全卸载");
    }
    else
    {
        ESP_LOGW(TAG, "SD卡未初始化，无需卸载");
    }
}

/**
 * @brief 列出指定目录内容。
 * @param[in] path 目录路径。
 * @return 无返回值。
 */
void sd_manager_list_dir(const char *path)
{
    if (path == NULL)
    {
        ESP_LOGE(TAG, "目录路径参数为空");
        return;
    }

    DIR *dir = opendir(path);

    if (dir != NULL)
    {
        struct dirent *ent;
        ESP_LOGI(TAG, "正在列出目录内容: %s", path);

        while ((ent = readdir(dir)) != NULL)
        {
            if (ent->d_type == DT_DIR)
            {
                ESP_LOGI(TAG, "  [DIR]  %s", ent->d_name);
            }
            else
            {
                ESP_LOGI(TAG, "  [FILE] %s", ent->d_name);
            }
        }

        closedir(dir);
    }
    else
    {
        ESP_LOGE(TAG, "无法打开目录: %s (可能不存在或未挂载)", path);
    }
}

/**
 * @brief 检查指定文件是否存在。
 * @param[in] file_path 文件路径。
 * @return true 表示文件存在。
 */
bool sd_manager_file_exists(const char *file_path)
{
    if (file_path == NULL)
    {
        ESP_LOGW(TAG, "文件路径参数为空");
        return false;
    }

    struct stat st;
    if (stat(file_path, &st) == 0)
    {
        return true;
    }
    return false;
}

/**
 * @brief 从 SD 卡读取文件内容。
 * @param[in] file_path 文件路径。
 * @param[out] buffer 接收数据的缓冲区。
 * @param[in] buffer_size 缓冲区大小，单位为字节。
 * @param[out] bytes_read 实际读取的字节数，可为 NULL。
 * @return `ESP_OK` 表示成功；其他错误表示参数、打开文件或读取失败。
 */
esp_err_t sd_manager_read_file(const char *file_path, void *buffer, size_t buffer_size, size_t *bytes_read)
{
    if (bytes_read != NULL)
    {
        *bytes_read = 0;
    }

    if (file_path == NULL || buffer == NULL || buffer_size == 0)
    {
        ESP_LOGW(TAG, "读取文件参数无效");
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(file_path, "rb");
    if (file == NULL)
    {
        ESP_LOGE(TAG, "打开文件失败: %s", file_path);
        return ESP_ERR_NOT_FOUND;
    }

    size_t read_len = fread(buffer, 1, buffer_size, file);
    if (ferror(file))
    {
        ESP_LOGE(TAG, "读取文件失败: %s", file_path);
        fclose(file);
        return ESP_FAIL;
    }

    fclose(file);

    if (bytes_read != NULL)
    {
        *bytes_read = read_len;
    }

    return ESP_OK;
}

/**
 * @brief 将数据写入 SD 卡文件。
 * @param[in] file_path 文件路径。
 * @param[in] data 要写入的数据。
 * @param[in] data_size 数据大小，单位为字节。
 * @return `ESP_OK` 表示成功；其他错误表示参数、打开文件或写入失败。
 */
esp_err_t sd_manager_write_file(const char *file_path, const void *data, size_t data_size)
{
    if (file_path == NULL || (data == NULL && data_size > 0))
    {
        ESP_LOGW(TAG, "写入文件参数无效");
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(file_path, "wb");
    if (file == NULL)
    {
        ESP_LOGE(TAG, "打开写入文件失败: %s", file_path);
        return ESP_FAIL;
    }

    if (data_size > 0)
    {
        size_t written = fwrite(data, 1, data_size, file);
        if (written != data_size || ferror(file))
        {
            ESP_LOGE(TAG, "写入文件失败: %s", file_path);
            fclose(file);
            return ESP_FAIL;
        }
    }

    if (fclose(file) != 0)
    {
        ESP_LOGE(TAG, "关闭写入文件失败: %s", file_path);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief 创建新目录。
 * @param[in] dir_path 目录路径。
 * @return `ESP_OK` 表示成功；其他错误表示创建失败。
 */
esp_err_t sd_manager_create_dir(const char *dir_path)
{
    if (dir_path == NULL)
    {
        ESP_LOGW(TAG, "创建目录参数无效");
        return ESP_ERR_INVALID_ARG;
    }

    if (mkdir(dir_path, 0775) == 0)
    {
        return ESP_OK;
    }

    if (errno == EEXIST)
    {
        struct stat st;
        if (stat(dir_path, &st) == 0 && S_ISDIR(st.st_mode))
        {
            return ESP_OK;
        }
    }

    ESP_LOGE(TAG, "创建目录失败: %s", dir_path);
    return ESP_FAIL;
}

/**
 * @brief 删除文件。
 * @param[in] file_path 文件路径。
 * @return `ESP_OK` 表示成功；其他错误表示删除失败。
 */
esp_err_t sd_manager_delete_file(const char *file_path)
{
    if (file_path == NULL)
    {
        ESP_LOGW(TAG, "删除文件参数无效");
        return ESP_ERR_INVALID_ARG;
    }

    if (remove(file_path) == 0)
    {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "删除文件失败: %s", file_path);
    return ESP_FAIL;
}

/**
 * @brief 获取文件大小。
 * @param[in] file_path 文件路径。
 * @param[out] file_size 用于接收文件大小的指针。
 * @return `ESP_OK` 表示成功；其他错误表示参数或查询失败。
 */
esp_err_t sd_manager_get_file_size(const char *file_path, size_t *file_size)
{
    if (file_path == NULL || file_size == NULL)
    {
        ESP_LOGW(TAG, "获取文件大小参数无效");
        return ESP_ERR_INVALID_ARG;
    }

    struct stat st;
    if (stat(file_path, &st) != 0)
    {
        ESP_LOGE(TAG, "获取文件大小失败: %s", file_path);
        return ESP_ERR_NOT_FOUND;
    }

    *file_size = (size_t)st.st_size;
    return ESP_OK;
}

/**
 * @brief 重命名或移动文件。
 * @param[in] old_path 原文件路径。
 * @param[in] new_path 新文件路径。
 * @return `ESP_OK` 表示成功；其他错误表示参数或重命名失败。
 */
esp_err_t sd_manager_rename_file(const char *old_path, const char *new_path)
{
    if (old_path == NULL || new_path == NULL)
    {
        ESP_LOGW(TAG, "重命名文件参数无效");
        return ESP_ERR_INVALID_ARG;
    }

    if (rename(old_path, new_path) == 0)
    {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "重命名文件失败: %s -> %s", old_path, new_path);
    return ESP_FAIL;
}
