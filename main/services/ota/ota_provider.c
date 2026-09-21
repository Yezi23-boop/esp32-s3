#include "services/ota/ota_provider.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "services/network/network_service.h"
#include "services/ota/onenet_ota_provider.h"
#include "services/ota/ota_transport.h"
#include "services/time/system_time_service.h"

static const char *TAG = "ota_provider";

static int ota_provider_status_to_step(ota_provider_status_t status)
{
    /* OneNET Fuse: 100 为当前已验证的成功终态，107/206 分别表示下载/升级失败。 */
    switch (status)
    {
    case OTA_PROVIDER_STATUS_DOWNLOAD_FAILURE:
        return 107;
    case OTA_PROVIDER_STATUS_ACTIVATE_FAILURE:
        return 206;
    case OTA_PROVIDER_STATUS_SUCCESS:
        return 100;
    default:
        return -1;
    }
}

esp_err_t ota_provider_check(const char *current_version,
                             ota_update_plan_t *out_plan)
{
    if (current_version == NULL || current_version[0] == '\0' ||
        out_plan == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_plan, 0, sizeof(*out_plan));
    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;
#if CONFIG_OTA_SOURCE_ONENET
    ret = onenet_ota_provider_check_plan(current_version, out_plan);
#elif CONFIG_OTA_SOURCE_REMOTE_MANIFEST
    const ota_transport_manifest_request_t request = {
        .manifest_url = CONFIG_OTA_SERVICE_REMOTE_MANIFEST_URL,
        .root_ca_pem = NULL,
        .use_cert_bundle = true,
        .allowed_host = CONFIG_OTA_SERVICE_REMOTE_ALLOWED_HOST,
        .current_version = current_version,
    };
    ret = ota_transport_fetch_manifest(&request, out_plan);
    if (ret == ESP_OK)
    {
        out_plan->source = OTA_UPDATE_SOURCE_REMOTE_MANIFEST;
        out_plan->root_ca_pem = NULL;
        out_plan->use_cert_bundle = true;
    }
#else
    ESP_LOGE(TAG, "no OTA source selected");
#endif

    if (ret != ESP_OK)
    {
        return ret;
    }
    if (!ota_transport_version_is_newer(current_version, out_plan->version))
    {
        ESP_LOGW(TAG, "provider returned non-newer target: current=%s target=%s",
                 current_version, out_plan->version);
        return ESP_ERR_INVALID_VERSION;
    }
    return ESP_OK;
}

esp_err_t ota_provider_prepare_download(ota_update_plan_t *plan)
{
    if (plan == NULL || plan->version[0] == '\0' || plan->size == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_OTA_SOURCE_ONENET
    return onenet_ota_provider_prepare_plan(plan);
#elif CONFIG_OTA_SOURCE_REMOTE_MANIFEST
    return plan->url[0] == '\0' ? ESP_ERR_INVALID_RESPONSE : ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t ota_provider_store_pending(const ota_update_plan_t *plan)
{
    if (plan == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_OTA_SOURCE_ONENET
    return onenet_ota_provider_store_pending_plan(plan);
#else
    (void)plan;
    return ESP_OK;
#endif
}

esp_err_t ota_provider_report_status(const ota_update_plan_t *plan,
                                     ota_provider_status_t status)
{
    const int step = ota_provider_status_to_step(status);
    if (plan == NULL || step < 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_OTA_SOURCE_ONENET
    if (plan->source != OTA_UPDATE_SOURCE_ONENET ||
        plan->provider_task_id == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return onenet_ota_provider_report_status(plan->provider_task_id, step);
#else
    (void)plan;
    (void)status;
    return ESP_OK;
#endif
}

esp_err_t ota_provider_clear_pending(void)
{
#if CONFIG_OTA_SOURCE_ONENET
    return onenet_ota_provider_clear_pending();
#else
    return ESP_OK;
#endif
}

void ota_provider_report_pending(void)
{
#if CONFIG_OTA_SOURCE_ONENET
    onenet_ota_pending_t pending = {0};
    esp_err_t ret = onenet_ota_provider_load_pending(&pending);
    if (ret == ESP_ERR_NVS_NOT_FOUND)
    {
        return;
    }
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "OneNET pending load failed: %s", esp_err_to_name(ret));
        return;
    }

    for (unsigned int attempt = 0U;
         attempt < 30U && !network_service_is_service_ready(); ++attempt)
    {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
    if (!network_service_is_service_ready() ||
        system_time_service_ensure_valid_for_tls(5000U) != ESP_OK)
    {
        ESP_LOGW(TAG, "OneNET pending deferred: network or TLS time unavailable");
        return;
    }

    const esp_app_desc_t *description = esp_app_get_description();
    if (description == NULL || description->version[0] == '\0')
    {
        return;
    }
    const bool booted_target = strcmp(description->version, pending.target) == 0;
    ret = onenet_ota_provider_report_version(description->version);
    if (ret == ESP_OK)
    {
        const ota_update_plan_t pending_plan = {
            .source = OTA_UPDATE_SOURCE_ONENET,
            .provider_task_id = pending.task_id,
        };
        ret = ota_provider_report_status(
            &pending_plan, booted_target ? OTA_PROVIDER_STATUS_SUCCESS
                                         : OTA_PROVIDER_STATUS_ACTIVATE_FAILURE);
    }
    /* 无论启动目标是否匹配，只要云端接受了终态，本地 pending 就已完成消费。
     * 不清除会让回滚或激活失败后的旧记录在每次启动重复上报。 */
    if (ret == ESP_OK)
    {
        ret = onenet_ota_provider_clear_pending();
    }
    ESP_LOGI(TAG, "OneNET pending report: tid=%u target=%s booted=%d result=%s",
             (unsigned int)pending.task_id, pending.target,
             booted_target ? 1 : 0, esp_err_to_name(ret));
#endif
}
