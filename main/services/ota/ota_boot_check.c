#include "services/ota/ota_boot_check.h"

#include <stdbool.h>
#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "ota_boot_check";

esp_err_t ota_boot_check_run(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t ret = esp_ota_get_state_partition(running, &state);
    if (ret == ESP_ERR_NOT_FOUND || ret == ESP_ERR_NOT_SUPPORTED)
    {
        return ESP_OK;
    }
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "read running OTA state failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "running slot=%s address=0x%08x ota_state=%d",
             running->label, (unsigned int)running->address, (int)state);
    if (state != ESP_OTA_IMG_PENDING_VERIFY)
    {
        return ESP_OK;
    }

    const esp_app_desc_t *description = esp_app_get_description();
    FILE *resource = fopen("/resources/README.md", "rb");
    const bool local_boot_check_ok = description != NULL &&
                                     description->version[0] != '\0' &&
                                     resource != NULL;
    if (resource != NULL)
    {
        fclose(resource);
    }

    if (!local_boot_check_ok)
    {
        ESP_LOGE(TAG, "PENDING_VERIFY local boot check failed, rolling back");
        return esp_ota_mark_app_invalid_rollback_and_reboot();
    }

#if CONFIG_OTA_BOOT_CHECK_TEST_FORCE_FAIL
    ESP_LOGW(TAG, "test hook: force PENDING_VERIFY failure");
    return esp_ota_mark_app_invalid_rollback_and_reboot();
#endif
#if CONFIG_OTA_BOOT_CHECK_TEST_HOLD_MS > 0
    ESP_LOGW(TAG, "test window: PENDING_VERIFY hold_ms=%u",
             (unsigned int)CONFIG_OTA_BOOT_CHECK_TEST_HOLD_MS);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_OTA_BOOT_CHECK_TEST_HOLD_MS));
#endif

    ret = esp_ota_mark_app_valid_cancel_rollback();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "mark app valid failed: %s, rolling back", esp_err_to_name(ret));
        return esp_ota_mark_app_invalid_rollback_and_reboot();
    }
    ESP_LOGI(TAG, "PENDING_VERIFY confirmed valid: version=%s",
             description->version);
    return ESP_OK;
}
