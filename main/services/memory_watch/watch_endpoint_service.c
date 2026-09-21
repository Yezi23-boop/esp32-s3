#include "services/memory_watch/watch_endpoint_service.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "services/network/network_service.h"
#include "services/memory_watch/memory_watch_service.h"
#include "services/memory_watch/memory_watch_voice_client.h"

static const char *TAG = "watch_endpoint";
static const UBaseType_t kAlertQueueLength = 1;
/* xTaskCreateWithCaps 的栈单位是 bytes；HTTPS/TLS 短请求预留 8KB PSRAM 栈。 */
static const uint32_t kAlertWorkerStackBytes = 8192U;
static const uint32_t kDangerAlertTimeoutMs = 8000U;

typedef struct
{
    memory_watch_service_endpoint_snapshot_t endpoint_config;
    char danger_type[WATCH_ENDPOINT_DANGER_TYPE_MAX_BYTES];
    char message[WATCH_ENDPOINT_DANGER_MESSAGE_MAX_BYTES];
    float danger_prob;
    uint32_t alert_sequence;
} watch_endpoint_alert_job_t;

static TaskHandle_t s_alert_worker_task_handle = NULL;
static QueueHandle_t s_alert_worker_queue = NULL;
static StaticQueue_t s_alert_worker_queue_buffer;
static uint8_t s_alert_worker_queue_storage[
    1 * sizeof(watch_endpoint_alert_job_t)];

static void watch_endpoint_service_copy_text(char *dst, size_t dst_len,
                                             const char *src)
{
    if (dst == NULL || dst_len == 0U)
    {
        return;
    }
    if (src == NULL)
    {
        dst[0] = '\0';
        return;
    }

    size_t index = 0;
    while (index + 1U < dst_len && src[index] != '\0')
    {
        dst[index] = src[index];
        ++index;
    }
    dst[index] = '\0';
}

static bool watch_endpoint_service_is_safe_alert_text(const char *text,
                                                      size_t max_len)
{
    if (text == NULL || text[0] == '\0' || max_len == 0U)
    {
        return false;
    }

    size_t len = 0;
    for (const char *p = text; *p != '\0'; ++p)
    {
        const unsigned char c = (unsigned char)*p;
        if (c < 0x20U || *p == '"' || *p == '\\')
        {
            return false;
        }
        ++len;
        if (len >= max_len)
        {
            return false;
        }
    }
    return true;
}

static const char *watch_endpoint_service_firmware_version(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    if (app_desc == NULL || app_desc->version[0] == '\0')
    {
        return NULL;
    }
    return app_desc->version;
}

static void watch_endpoint_service_alert_worker_task(void *arg)
{
    (void)arg;

    while (1)
    {
        watch_endpoint_alert_job_t job;
        if (xQueueReceive(s_alert_worker_queue, &job, portMAX_DELAY) !=
            pdTRUE)
        {
            continue;
        }

        const memory_watch_voice_client_config_t client_config = {
            .base_url = job.endpoint_config.base_url,
            .device_id = job.endpoint_config.device_id,
            .device_token = job.endpoint_config.device_token,
            .timeout_ms = kDangerAlertTimeoutMs,
            .allow_insecure_http = job.endpoint_config.allow_insecure_http,
        };
        const memory_watch_voice_client_danger_alert_request_t request = {
            .danger_type = job.danger_type,
            .danger_prob = job.danger_prob,
            .alert_sequence = job.alert_sequence,
            .message = job.message,
            .firmware_version = watch_endpoint_service_firmware_version(),
        };
        memory_watch_voice_client_danger_alert_response_t response = {0};
        const esp_err_t err = memory_watch_voice_client_post_danger_alert(
            &client_config, &request, &response);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "danger alert dispatch failed: status=%d err=%s",
                     response.http_status, esp_err_to_name(err));
        }
        else
        {
            ESP_LOGI(TAG,
                     "danger alert dispatched: type=%s prob=%.4f seq=%lu",
                     job.danger_type,
                     (double)job.danger_prob,
                     (unsigned long)job.alert_sequence);
        }
    }
}

esp_err_t watch_endpoint_service_init(void)
{
    if (s_alert_worker_task_handle != NULL)
    {
        return ESP_OK;
    }
    if (s_alert_worker_queue == NULL)
    {
        s_alert_worker_queue = xQueueCreateStatic(
            kAlertQueueLength,
            sizeof(watch_endpoint_alert_job_t),
            s_alert_worker_queue_storage,
            &s_alert_worker_queue_buffer);
    }
    if (s_alert_worker_queue == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t created = xTaskCreateWithCaps(
        watch_endpoint_service_alert_worker_task,
        "watch_alert",
        kAlertWorkerStackBytes,
        NULL,
        4,
        &s_alert_worker_task_handle,
        MALLOC_CAP_SPIRAM);
    if (created != pdPASS)
    {
        s_alert_worker_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t watch_endpoint_service_post_danger_alert(
    const watch_endpoint_danger_alert_t *alert)
{
    if (s_alert_worker_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (alert == NULL || alert->danger_prob < 0.0f ||
        alert->danger_prob > 1.0f ||
        !watch_endpoint_service_is_safe_alert_text(
            alert->danger_type, WATCH_ENDPOINT_DANGER_TYPE_MAX_BYTES) ||
        !watch_endpoint_service_is_safe_alert_text(
            alert->message, WATCH_ENDPOINT_DANGER_MESSAGE_MAX_BYTES))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!network_service_is_service_ready())
    {
        return ESP_ERR_INVALID_STATE;
    }

    watch_endpoint_alert_job_t job = {
        .danger_prob = alert->danger_prob,
        .alert_sequence = alert->alert_sequence,
    };
    esp_err_t err =
        memory_watch_service_copy_endpoint_config(&job.endpoint_config);
    if (err != ESP_OK)
    {
        return err;
    }
    watch_endpoint_service_copy_text(job.danger_type, sizeof(job.danger_type),
                                     alert->danger_type);
    watch_endpoint_service_copy_text(job.message, sizeof(job.message),
                                     alert->message);

    if (xQueueSend(s_alert_worker_queue, &job, 0) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}
