#include "system_time_service.h"

#include <stdbool.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "system_time_srv";
static const uint32_t kNetworkReadySntpTimeoutMs = 15000U;

static TaskHandle_t s_sync_task_handle = NULL;
static bool s_started = false;
static bool s_network_sync_done = false;

/**
 * @brief 输出开机时间来源摘要。
 *
 * 该日志把 RTC 是否存在、OS 位、当前来源和系统时间可信性压缩成一行，
 * 方便板端验证时不用人工拼多条 RTC/SNTP 日志。
 *
 * @param[in] reason 额外原因，可为 NULL。
 * @return 无返回值。
 */
static void system_time_service_log_boot_summary(const char *reason)
{
    system_time_snapshot_t snapshot = {0};
    if (system_time_get_snapshot(&snapshot) != ESP_OK)
    {
        ESP_LOGW(TAG, "system_time_boot: snapshot_unavailable");
        return;
    }

    system_time_local_t local_time = {0};
    const esp_err_t local_ret = system_time_get_local_time(&local_time);
    ESP_LOGI(TAG,
             "system_time_boot: rtc_present=%d os=%d source=%s sys_valid=%d rtc=%s reason=%s",
             snapshot.rtc_present,
             snapshot.rtc_oscillator_stopped,
             system_time_source_text(snapshot.source),
             snapshot.system_time_valid,
             local_ret == ESP_OK ? local_time.time_str : "unknown",
             reason != NULL ? reason : "none");
}

static void system_time_service_sync_task(void *arg)
{
    (void)arg;

    const esp_err_t ret =
        system_time_sync_sntp_and_write_rtc(kNetworkReadySntpTimeoutMs);
    if (ret == ESP_OK)
    {
        s_network_sync_done = true;
        ESP_LOGI(TAG, "network time sync done");
    }
    else
    {
        ESP_LOGW(TAG, "network time sync failed: %s", esp_err_to_name(ret));
    }

    s_sync_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t system_time_service_start(void)
{
    if (s_started)
    {
        return ESP_OK;
    }

    esp_err_t ret = system_time_init();
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "system time core init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = system_time_bootstrap_from_rtc();
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "rtc bootstrap done");
        system_time_service_log_boot_summary("rtc_bootstrap_ok");
    }
    else
    {
        ESP_LOGW(TAG, "rtc bootstrap skipped: %s", esp_err_to_name(ret));
        system_time_service_log_boot_summary(esp_err_to_name(ret));
    }

    s_started = true;
    return ESP_OK;
}

esp_err_t system_time_service_note_network_ready(void)
{
    if (!s_started)
    {
        ESP_RETURN_ON_ERROR(system_time_service_start(), TAG,
                            "start before network sync failed");
    }

    if (s_network_sync_done || s_sync_task_handle != NULL)
    {
        return ESP_OK;
    }

    const BaseType_t result = xTaskCreateWithCaps(
        system_time_service_sync_task, "system_time_sync", 4096, NULL, 4,
        &s_sync_task_handle, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (result != pdPASS)
    {
        s_sync_task_handle = NULL;
        ESP_LOGW(TAG, "create network time sync task failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "network time sync scheduled");
    return ESP_OK;
}

esp_err_t system_time_service_ensure_valid_for_tls(uint32_t timeout_ms)
{
    if (!s_started)
    {
        ESP_RETURN_ON_ERROR(system_time_service_start(), TAG,
                            "start before tls ensure failed");
    }

    return system_time_ensure_valid_for_tls(timeout_ms);
}

esp_err_t system_time_service_apply_server_time(int64_t unix_seconds)
{
    if (!s_started)
    {
        ESP_RETURN_ON_ERROR(system_time_service_start(), TAG,
                            "start before server time apply failed");
    }

    return system_time_apply_unix_time(SYSTEM_TIME_SOURCE_SERVER, unix_seconds,
                                       true);
}

esp_err_t system_time_service_get_snapshot(system_time_snapshot_t *out)
{
    return system_time_get_snapshot(out);
}

esp_err_t system_time_service_get_local_time(system_time_local_t *out)
{
    return system_time_get_local_time(out);
}
