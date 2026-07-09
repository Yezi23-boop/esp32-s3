#include "system_time.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pcf85063atl.h"
#include "sdkconfig.h"

static const char *TAG = "system_time";

#define SYSTEM_TIME_TLS_VALID_EPOCH 1704067200LL
#define SYSTEM_TIME_SNTP_POLL_INTERVAL_MS 250U

static const char *const k_sntp_servers[] = {
    "ntp1.aliyun.com",
    "cn.pool.ntp.org",
    "ntp2.aliyun.com",
};

static bool s_initialized = false;
static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static system_time_snapshot_t s_snapshot = {
    .system_time_valid = false,
    .rtc_present = false,
    .rtc_oscillator_stopped = false,
    .rtc_writeback_ok = false,
    .source = SYSTEM_TIME_SOURCE_NONE,
    .unix_seconds = 0,
};

static void system_time_configure_timezone(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();
}

static bool system_time_is_unix_time_valid(int64_t unix_seconds)
{
    return unix_seconds >= SYSTEM_TIME_TLS_VALID_EPOCH;
}

static void system_time_store_snapshot(const system_time_snapshot_t *snapshot)
{
    portENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot = *snapshot;
    portEXIT_CRITICAL(&s_snapshot_lock);
}

static void system_time_load_snapshot(system_time_snapshot_t *snapshot)
{
    portENTER_CRITICAL(&s_snapshot_lock);
    *snapshot = s_snapshot;
    portEXIT_CRITICAL(&s_snapshot_lock);
}

static esp_err_t system_time_rtc_to_epoch(const pcf85063atl_time_t *rtc_time,
                                          int64_t *unix_seconds)
{
    ESP_RETURN_ON_FALSE(rtc_time != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "rtc_time is null");
    ESP_RETURN_ON_FALSE(unix_seconds != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "unix_seconds is null");
    ESP_RETURN_ON_FALSE(rtc_time->seconds <= 59 && rtc_time->minutes <= 59 &&
                            rtc_time->hours <= 23 && rtc_time->days >= 1 &&
                            rtc_time->days <= 31 && rtc_time->months >= 1 &&
                            rtc_time->months <= 12 && rtc_time->years <= 99,
                        ESP_ERR_INVALID_ARG, TAG, "rtc fields invalid");

    system_time_configure_timezone();

    struct tm local_time = {
        .tm_sec = rtc_time->seconds,
        .tm_min = rtc_time->minutes,
        .tm_hour = rtc_time->hours,
        .tm_mday = rtc_time->days,
        .tm_mon = rtc_time->months - 1,
        .tm_year = 100 + rtc_time->years,
        .tm_isdst = -1,
    };

    time_t epoch = mktime(&local_time);
    ESP_RETURN_ON_FALSE(epoch >= 0, ESP_ERR_INVALID_ARG, TAG,
                        "rtc time cannot convert to epoch");

    *unix_seconds = (int64_t)epoch;
    return ESP_OK;
}

static esp_err_t system_time_epoch_to_rtc(int64_t unix_seconds,
                                          pcf85063atl_time_t *rtc_time)
{
    ESP_RETURN_ON_FALSE(rtc_time != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "rtc_time is null");

    system_time_configure_timezone();

    time_t epoch = (time_t)unix_seconds;
    struct tm local_time = {0};
    ESP_RETURN_ON_FALSE(localtime_r(&epoch, &local_time) != NULL,
                        ESP_ERR_INVALID_ARG, TAG,
                        "epoch cannot convert to local time");

    const int full_year = local_time.tm_year + 1900;
    ESP_RETURN_ON_FALSE(full_year >= 2000 && full_year <= 2099,
                        ESP_ERR_INVALID_ARG, TAG,
                        "rtc year supports 2000-2099 only");

    rtc_time->oscillator_stopped = false;
    rtc_time->seconds = (uint8_t)local_time.tm_sec;
    rtc_time->minutes = (uint8_t)local_time.tm_min;
    rtc_time->hours = (uint8_t)local_time.tm_hour;
    rtc_time->days = (uint8_t)local_time.tm_mday;
    rtc_time->weekdays = (uint8_t)local_time.tm_wday;
    rtc_time->months = (uint8_t)(local_time.tm_mon + 1);
    rtc_time->years = (uint8_t)(full_year - 2000);
    return ESP_OK;
}

static esp_err_t system_time_write_rtc_from_epoch(int64_t unix_seconds)
{
    pcf85063atl_time_t rtc_time = {0};
    ESP_RETURN_ON_ERROR(system_time_epoch_to_rtc(unix_seconds, &rtc_time), TAG,
                        "convert epoch to rtc failed");
    return pcf85063atl_set_time(&rtc_time);
}

static void system_time_configure_sntp(void)
{
    const size_t configured_servers =
        sizeof(k_sntp_servers) / sizeof(k_sntp_servers[0]);
    size_t max_servers = CONFIG_LWIP_SNTP_MAX_SERVERS;
    if (max_servers > configured_servers)
    {
        max_servers = configured_servers;
    }

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    for (size_t index = 0; index < max_servers; ++index)
    {
        esp_sntp_setservername((u8_t)index, k_sntp_servers[index]);
    }
}

static void system_time_start_or_restart_sntp(void)
{
    if (esp_sntp_enabled())
    {
        ESP_LOGI(TAG, "SNTP already enabled, restart");
        esp_sntp_restart();
        return;
    }

    system_time_configure_sntp();
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started server0=%s", k_sntp_servers[0]);
}

const char *system_time_source_text(system_time_source_t source)
{
    switch (source)
    {
    case SYSTEM_TIME_SOURCE_RTC:
        return "RTC";
    case SYSTEM_TIME_SOURCE_SNTP:
        return "SNTP";
    case SYSTEM_TIME_SOURCE_SERVER:
        return "SERVER";
    case SYSTEM_TIME_SOURCE_NONE:
    default:
        return "NONE";
    }
}

esp_err_t system_time_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    system_time_configure_timezone();
    ESP_RETURN_ON_ERROR(pcf85063atl_init(), TAG, "rtc init failed");
    s_initialized = true;
    return ESP_OK;
}

esp_err_t system_time_bootstrap_from_rtc(void)
{
    ESP_RETURN_ON_ERROR(system_time_init(), TAG,
                        "init failed before rtc bootstrap");

    system_time_snapshot_t snapshot = {0};
    system_time_load_snapshot(&snapshot);

    bool present = false;
    ESP_RETURN_ON_ERROR(pcf85063atl_probe(&present), TAG, "rtc probe failed");
    snapshot.rtc_present = present;
    if (!present)
    {
        snapshot.rtc_oscillator_stopped = false;
        system_time_store_snapshot(&snapshot);
        ESP_LOGW(TAG, "rtc bootstrap skipped: rtc not present");
        return ESP_ERR_NOT_FOUND;
    }

    pcf85063atl_status_t status = {0};
    ESP_RETURN_ON_ERROR(pcf85063atl_read_status(&status), TAG,
                        "rtc status read failed");
    snapshot.rtc_oscillator_stopped = status.oscillator_stopped;
    if (status.oscillator_stopped)
    {
        system_time_store_snapshot(&snapshot);
        ESP_LOGW(TAG, "rtc bootstrap skipped: oscillator_stopped=1");
        return ESP_ERR_INVALID_STATE;
    }

    pcf85063atl_time_t rtc_time = {0};
    ESP_RETURN_ON_ERROR(pcf85063atl_read_time(&rtc_time), TAG,
                        "rtc time read failed");

    int64_t unix_seconds = 0;
    ESP_RETURN_ON_ERROR(system_time_rtc_to_epoch(&rtc_time, &unix_seconds),
                        TAG, "rtc time invalid");

    struct timeval tv = {
        .tv_sec = (time_t)unix_seconds,
        .tv_usec = 0,
    };
    ESP_RETURN_ON_FALSE(settimeofday(&tv, NULL) == 0, ESP_FAIL, TAG,
                        "settimeofday from rtc failed");

    snapshot.system_time_valid = system_time_is_unix_time_valid(unix_seconds);
    snapshot.source = snapshot.system_time_valid ? SYSTEM_TIME_SOURCE_RTC
                                                 : SYSTEM_TIME_SOURCE_NONE;
    snapshot.unix_seconds = unix_seconds;
    snapshot.rtc_writeback_ok = false;
    system_time_store_snapshot(&snapshot);

    ESP_LOGI(TAG, "rtc bootstrap %s epoch=%lld",
             snapshot.system_time_valid ? "ok" : "invalid",
             (long long)unix_seconds);
    return snapshot.system_time_valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t system_time_apply_unix_time(system_time_source_t source,
                                      int64_t unix_seconds,
                                      bool write_rtc)
{
    ESP_RETURN_ON_FALSE(source != SYSTEM_TIME_SOURCE_NONE,
                        ESP_ERR_INVALID_ARG, TAG, "source invalid");
    ESP_RETURN_ON_FALSE(system_time_is_unix_time_valid(unix_seconds),
                        ESP_ERR_INVALID_ARG, TAG, "unix time invalid");
    ESP_RETURN_ON_ERROR(system_time_init(), TAG,
                        "init failed before apply unix time");

    struct timeval tv = {
        .tv_sec = (time_t)unix_seconds,
        .tv_usec = 0,
    };
    ESP_RETURN_ON_FALSE(settimeofday(&tv, NULL) == 0, ESP_FAIL, TAG,
                        "settimeofday failed");

    system_time_snapshot_t snapshot = {0};
    system_time_load_snapshot(&snapshot);
    snapshot.system_time_valid = true;
    snapshot.source = source;
    snapshot.unix_seconds = unix_seconds;

    if (write_rtc)
    {
        const esp_err_t rtc_ret = system_time_write_rtc_from_epoch(unix_seconds);
        snapshot.rtc_present = rtc_ret != ESP_ERR_NOT_FOUND;
        snapshot.rtc_writeback_ok = rtc_ret == ESP_OK;
        snapshot.rtc_oscillator_stopped = rtc_ret == ESP_OK ? false
                                                            : snapshot.rtc_oscillator_stopped;
        if (rtc_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "rtc writeback failed: %s", esp_err_to_name(rtc_ret));
        }
    }

    system_time_store_snapshot(&snapshot);
    ESP_LOGI(TAG, "system time applied source=%d epoch=%lld rtc_writeback=%d",
             source, (long long)unix_seconds, snapshot.rtc_writeback_ok);
    return ESP_OK;
}

esp_err_t system_time_sync_sntp_and_write_rtc(uint32_t timeout_ms)
{
    ESP_RETURN_ON_ERROR(system_time_init(), TAG,
                        "init failed before sntp sync");
    ESP_RETURN_ON_FALSE(timeout_ms > 0, ESP_ERR_INVALID_ARG, TAG,
                        "timeout must be > 0");

    system_time_start_or_restart_sntp();

    system_time_snapshot_t before_snapshot = {0};
    system_time_load_snapshot(&before_snapshot);

    const TickType_t deadline_ticks =
        xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline_ticks)
    {
        time_t now = 0;
        time(&now);
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED &&
            system_time_is_unix_time_valid((int64_t)now))
        {
            ESP_RETURN_ON_ERROR(
                system_time_apply_unix_time(SYSTEM_TIME_SOURCE_SNTP,
                                            (int64_t)now, true),
                TAG, "apply sntp time failed");
            system_time_snapshot_t after_snapshot = {0};
            system_time_load_snapshot(&after_snapshot);
            const bool drift_known = before_snapshot.system_time_valid &&
                                     before_snapshot.source == SYSTEM_TIME_SOURCE_RTC;
            const int64_t drift_sec =
                drift_known ? ((int64_t)now - before_snapshot.unix_seconds) : 0;
            ESP_LOGI(TAG,
                     "system_time_sync: source=%s rtc_writeback=%d drift_sec=%lld drift_known=%d",
                     system_time_source_text(after_snapshot.source),
                     after_snapshot.rtc_writeback_ok,
                     (long long)drift_sec,
                     drift_known ? 1 : 0);
            ESP_LOGI(TAG, "sntp sync ok source=SNTP");
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(SYSTEM_TIME_SNTP_POLL_INTERVAL_MS));
    }

    system_time_snapshot_t snapshot = {0};
    system_time_load_snapshot(&snapshot);
    time_t now = 0;
    time(&now);
    snapshot.system_time_valid = system_time_is_unix_time_valid((int64_t)now);
    snapshot.unix_seconds = (int64_t)now;
    system_time_store_snapshot(&snapshot);

    ESP_LOGW(TAG,
             "sntp sync timeout status=%d reachability0=%u time_valid=%d",
             esp_sntp_get_sync_status(), esp_sntp_getreachability(0),
             snapshot.system_time_valid);
    return ESP_ERR_TIMEOUT;
}

esp_err_t system_time_ensure_valid_for_tls(uint32_t timeout_ms)
{
    time_t now = 0;
    time(&now);
    if (system_time_is_unix_time_valid((int64_t)now))
    {
        system_time_snapshot_t snapshot = {0};
        system_time_load_snapshot(&snapshot);
        snapshot.system_time_valid = true;
        snapshot.unix_seconds = (int64_t)now;
        if (snapshot.source == SYSTEM_TIME_SOURCE_NONE)
        {
            snapshot.source = SYSTEM_TIME_SOURCE_SERVER;
        }
        system_time_store_snapshot(&snapshot);
        return ESP_OK;
    }

    return system_time_sync_sntp_and_write_rtc(timeout_ms);
}

esp_err_t system_time_get_snapshot(system_time_snapshot_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is null");
    system_time_load_snapshot(out);
    return ESP_OK;
}

esp_err_t system_time_get_local_time(system_time_local_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is null");

    time_t now = 0;
    time(&now);
    ESP_RETURN_ON_FALSE(system_time_is_unix_time_valid((int64_t)now),
                        ESP_ERR_INVALID_STATE, TAG,
                        "system time is not valid");

    system_time_configure_timezone();

    struct tm local_time = {0};
    ESP_RETURN_ON_FALSE(localtime_r(&now, &local_time) != NULL, ESP_FAIL, TAG,
                        "localtime failed");

    out->year = local_time.tm_year + 1900;
    out->month = local_time.tm_mon + 1;
    out->day = local_time.tm_mday;
    out->hour = local_time.tm_hour;
    out->min = local_time.tm_min;
    out->sec = local_time.tm_sec;
    snprintf(out->time_str, sizeof(out->time_str),
             "%04d-%02d-%02d %02d:%02d:%02d", out->year, out->month,
             out->day, out->hour, out->min, out->sec);
    return ESP_OK;
}
