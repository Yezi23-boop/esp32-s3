#include "services/ota/ota_metrics.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* 复用 OTA_TRANSPORT_VERSION_MAX，保证条目版本字段与 manifest 版本同宽。 */
#include "services/ota/ota_transport.h"

#if CONFIG_OTA_METRICS_ENABLED

static const char *TAG = "ota_metrics";

static const char *kNvsNamespace = "ota_metrics";
static const char *kResultLogKey = "ota_result_log"; /* NVS key 上限 15 字符 */

/** 单条升级阶段结果；固定布局写入 NVS blob，字段不得随意增删。 */
typedef struct
{
    uint32_t uptime_s;      /* 记录时刻的启动后运行秒数，不依赖网络时间 */
    uint8_t stage;          /* ota_metrics_stage_t */
    uint8_t is_delta;       /* 0/1：是否差分路径 */
    uint8_t retry_count;    /* delta 失败重试次数（全量路径为 0） */
    uint8_t reserved;
    int32_t result;         /* 该阶段最终 esp_err_t（ESP_OK 表示成功） */
    uint32_t duration_ms;   /* 阶段耗时（ms） */
    char version[OTA_TRANSPORT_VERSION_MAX]; /* 目标版本；失败时可能为空串 */
} ota_metrics_entry_t;

/** NVS 中的环形结果日志；count>=MAX 后 next_index 循环覆盖最旧条目。 */
typedef struct
{
    uint32_t magic;
    uint32_t count; /* 已写入条目总数（含被覆盖的旧条目） */
    uint32_t next_index; /* 下一条写入位置，恒 < CONFIG_OTA_METRICS_LOG_COUNT */
    uint32_t reserved;
    ota_metrics_entry_t entries[CONFIG_OTA_METRICS_LOG_COUNT];
} ota_metrics_log_t;

static const uint32_t kLogMagic = 0x0A1A5044U; /* 魔数，防止读到脏/损坏数据 */

static nvs_handle_t s_nvs_handle;
static SemaphoreHandle_t s_mutex; /* 保护 s_nvs_handle 与持久化状态（可跨任务 dump） */
static bool s_initialized;
static uint64_t s_stage_begin_us[OTA_METRICS_STAGE_COUNT]; /* esp_timer 微秒起点 */

static const char *ota_metrics_stage_text(ota_metrics_stage_t stage)
{
    switch (stage)
    {
    case OTA_METRICS_STAGE_MANIFEST:
        return "manifest";
    case OTA_METRICS_STAGE_DOWNLOAD:
        return "download";
    case OTA_METRICS_STAGE_ACTIVATE:
        return "activate";
    default:
        return "unknown";
    }
}

esp_err_t ota_metrics_init(void)
{
    if (s_mutex == NULL)
    {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL)
        {
            ESP_LOGW(TAG, "mutex create failed");
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_initialized)
    {
        return ESP_OK;
    }
    const esp_err_t ret = nvs_open(kNvsNamespace, NVS_READWRITE, &s_nvs_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "nvs open failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_initialized = true;
    return ESP_OK;
}

void ota_metrics_stage_begin(ota_metrics_stage_t stage)
{
    if (stage >= OTA_METRICS_STAGE_COUNT)
    {
        return;
    }
    s_stage_begin_us[stage] = esp_timer_get_time();
}

uint32_t ota_metrics_stage_elapsed_ms(ota_metrics_stage_t stage)
{
    if (stage >= OTA_METRICS_STAGE_COUNT || s_stage_begin_us[stage] == 0U)
    {
        return 0U;
    }
    const uint64_t now = esp_timer_get_time();
    return (uint32_t)((now - s_stage_begin_us[stage]) / 1000U);
}

void ota_metrics_record_result(ota_metrics_stage_t stage,
                               const char *target_version, esp_err_t result,
                               uint32_t duration_ms, bool is_delta,
                               uint8_t retry_count)
{
    if (stage >= OTA_METRICS_STAGE_COUNT || s_mutex == NULL)
    {
        return;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE)
    {
        return;
    }

    ota_metrics_log_t log = {0};
    bool loaded = false;
    if (s_initialized)
    {
        size_t length = sizeof(log);
        if (nvs_get_blob(s_nvs_handle, kResultLogKey, &log, &length) == ESP_OK &&
            length == sizeof(log) && log.magic == kLogMagic)
        {
            loaded = true;
        }
    }
    if (!loaded)
    {
        /* 首次写入或旧数据损坏：以干净状态重建环形缓冲。 */
        log.magic = kLogMagic;
        log.count = 0U;
        log.next_index = 0U;
    }

    ota_metrics_entry_t *entry =
        &log.entries[log.next_index % CONFIG_OTA_METRICS_LOG_COUNT];
    memset(entry, 0, sizeof(*entry));
    entry->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    entry->stage = (uint8_t)stage;
    entry->is_delta = is_delta ? 1U : 0U;
    entry->retry_count = retry_count;
    entry->result = (int32_t)result;
    entry->duration_ms = duration_ms;
    if (target_version != NULL)
    {
        strncpy(entry->version, target_version, sizeof(entry->version) - 1U);
    }

    log.next_index = (log.next_index + 1U) % CONFIG_OTA_METRICS_LOG_COUNT;
    if (log.count < (uint32_t)CONFIG_OTA_METRICS_LOG_COUNT)
    {
        ++log.count;
    }

    if (s_initialized)
    {
        esp_err_t ret = nvs_set_blob(s_nvs_handle, kResultLogKey, &log,
                                     sizeof(log));
        if (ret == ESP_OK)
        {
            ret = nvs_commit(s_nvs_handle);
        }
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "result persist failed: %s", esp_err_to_name(ret));
        }
    }

    ESP_LOGI(TAG,
             "record stage=%s ver=%s result=%s dur=%ums delta=%d retry=%u "
             "uptime=%us",
             ota_metrics_stage_text(stage),
             entry->version[0] != '\0' ? entry->version : "-",
             esp_err_to_name(result), (unsigned int)duration_ms,
             is_delta ? 1 : 0, (unsigned int)retry_count,
             (unsigned int)entry->uptime_s);
    xSemaphoreGive(s_mutex);
}

esp_err_t ota_metrics_dump_recent(void)
{
    if (s_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    if (s_initialized)
    {
        ota_metrics_log_t log = {0};
        size_t length = sizeof(log);
        if (nvs_get_blob(s_nvs_handle, kResultLogKey, &log, &length) == ESP_OK &&
            length == sizeof(log) && log.magic == kLogMagic && log.count > 0U)
        {
            ESP_LOGI(TAG, "recent OTA results (%u entries):",
                     (unsigned int)log.count);
            /* 环形打印：未写满时从头开始；写满后从 next_index（最旧）开始。 */
            const uint32_t total = log.count;
            const uint32_t skip = total >= (uint32_t)CONFIG_OTA_METRICS_LOG_COUNT
                                      ? log.next_index
                                      : 0U;
            for (uint32_t index = 0U; index < total; ++index)
            {
                const ota_metrics_entry_t *entry =
                    &log.entries[(skip + index) % CONFIG_OTA_METRICS_LOG_COUNT];
                ESP_LOGI(TAG,
                         "  [%u] stage=%s ver=%s result=%s dur=%ums "
                         "delta=%d retry=%u uptime=%us",
                         index, ota_metrics_stage_text(
                                    (ota_metrics_stage_t)entry->stage),
                         entry->version[0] != '\0' ? entry->version : "-",
                         esp_err_to_name((esp_err_t)entry->result),
                         (unsigned int)entry->duration_ms, (int)entry->is_delta,
                         (unsigned int)entry->retry_count,
                         (unsigned int)entry->uptime_s);
            }
            ret = ESP_OK;
        }
    }
    xSemaphoreGive(s_mutex);
    return ret;
}

#else

esp_err_t ota_metrics_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void ota_metrics_stage_begin(ota_metrics_stage_t stage)
{
    (void)stage;
}

uint32_t ota_metrics_stage_elapsed_ms(ota_metrics_stage_t stage)
{
    (void)stage;
    return 0U;
}

void ota_metrics_record_result(ota_metrics_stage_t stage,
                               const char *target_version, esp_err_t result,
                               uint32_t duration_ms, bool is_delta,
                               uint8_t retry_count)
{
    (void)stage;
    (void)target_version;
    (void)result;
    (void)duration_ms;
    (void)is_delta;
    (void)retry_count;
}

esp_err_t ota_metrics_dump_recent(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
