#ifndef ONENET_OTA_PROVIDER_H
#define ONENET_OTA_PROVIDER_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "services/ota/ota_update_plan.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define ONENET_OTA_PRODUCT_ID_MAX 32U
#define ONENET_OTA_DEVICE_NAME_MAX 64U
#define ONENET_OTA_ACCESS_KEY_MAX 96U
#define ONENET_OTA_TARGET_VERSION_MAX 32U
#define ONENET_OTA_MD5_HEX_LEN 32U
#define ONENET_OTA_DOWNLOAD_URL_MAX 192U
#define ONENET_OTA_AUTHORIZATION_MAX 256U

    typedef struct
    {
        char target[ONENET_OTA_TARGET_VERSION_MAX];
        uint32_t task_id;
        size_t size;
        char md5[ONENET_OTA_MD5_HEX_LEN + 1U];
        int status;
        int package_type;
    } onenet_ota_task_t;

    typedef struct
    {
        uint32_t task_id;
        char target[ONENET_OTA_TARGET_VERSION_MAX];
    } onenet_ota_pending_t;

    /** @brief 上报当前应用版本和固定模组占位版本。 */
    esp_err_t onenet_ota_provider_report_version(const char *app_version);

    /**
     * @brief 检查 SOTA 完整包任务。
     * @return `ESP_ERR_NOT_FOUND` 表示当前没有任务。
     */
    esp_err_t onenet_ota_provider_check(const char *current_version,
                                        onenet_ota_task_t *out_task);

    /**
     * @brief 为已检查的 SOTA 任务生成下载 URL 和 Authorization。
     *
     * 输出只在当前 OTA 下载会话内使用；调用方不得记录 Authorization。
     */
    esp_err_t onenet_ota_provider_prepare_download(
        const onenet_ota_task_t *task, char *out_url, size_t url_size,
        char *out_authorization, size_t authorization_size);

    /**
     * @brief 将当前 OneNET 任务转换成公共 OTA 计划。
     *
     * 当前 OneNET 任务为完整包；未来增加 delta 时只扩展该 provider 的
     * JSON 映射，不改变 ota_service 的执行流程。
     */
    esp_err_t onenet_ota_provider_check_plan(const char *current_version,
                                             ota_update_plan_t *out_plan);

    /** @brief 为公共 OTA 计划生成 OneNET 下载地址和会话 Authorization。 */
    esp_err_t onenet_ota_provider_prepare_plan(ota_update_plan_t *plan);

    /** @brief 持久化待上报的 OneNET 任务，必须在切换启动槽前调用。 */
    esp_err_t onenet_ota_provider_store_pending(const onenet_ota_task_t *task);

    /** @brief 保存公共 OTA 计划中的 OneNET pending 任务。 */
    esp_err_t onenet_ota_provider_store_pending_plan(
        const ota_update_plan_t *plan);

    /** @brief 读取上次已激活但尚未向 OneNET 完成状态上报的任务。 */
    esp_err_t onenet_ota_provider_load_pending(onenet_ota_pending_t *out_pending);

    /** @brief 清除已成功上报的 OneNET pending 任务。 */
    esp_err_t onenet_ota_provider_clear_pending(void);

    /** @brief 上报 OneNET 下载/升级步进，step=100 表示升级成功。 */
    esp_err_t onenet_ota_provider_report_status(uint32_t task_id, int step);

#ifdef __cplusplus
}
#endif

#endif /* ONENET_OTA_PROVIDER_H */
