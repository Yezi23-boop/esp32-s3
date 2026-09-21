#ifndef OTA_PROVIDER_H
#define OTA_PROVIDER_H

#include "esp_err.h"
#include "services/ota/ota_update_plan.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** provider 只上报 OTA 会话的阶段结果，具体协议状态码由 provider 自己映射。 */
    typedef enum
    {
        OTA_PROVIDER_STATUS_DOWNLOAD_FAILURE = 0,
        OTA_PROVIDER_STATUS_ACTIVATE_FAILURE,
        OTA_PROVIDER_STATUS_SUCCESS,
    } ota_provider_status_t;

/** @brief 按编译期来源检查更新，生成来源无关的 OTA 计划。 */
esp_err_t ota_provider_check(const char *current_version,
                             ota_update_plan_t *out_plan);

/** @brief 为已检查的计划补齐下载 URL 和当前会话 Authorization。 */
esp_err_t ota_provider_prepare_download(ota_update_plan_t *plan);

/** @brief 在切换启动槽前保存 provider 需要的 pending 状态。 */
esp_err_t ota_provider_store_pending(const ota_update_plan_t *plan);

/**
 * @brief 上报当前 OTA 会话终态。
 *
 * service 不携带 OneNET 的 tid/step；没有远端状态接口的 provider 为空操作。
 */
esp_err_t ota_provider_report_status(const ota_update_plan_t *plan,
                                     ota_provider_status_t status);

/** @brief 清除一次未完成激活留下的 provider pending 状态。 */
esp_err_t ota_provider_clear_pending(void);

/** @brief 启动后上报上一次已激活 provider 任务；非 OneNET 来源为空操作。 */
void ota_provider_report_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_PROVIDER_H */
