#include "time_weather.h"
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "network_manager.h"
#include "features/weather/hptts.h"
#include "services/power_policy.h"
#include "esp_log.h"

static const char *TAG = "WEATHER_SERVICE";
static const uint32_t kWeatherTaskPeriodSeconds = 10;
static const uint32_t kWeatherRefreshNormalSeconds = 60 * 60;
static const uint32_t kWeatherRefreshLowPowerSeconds = 2 * 60 * 60;
static const uint32_t kWeatherRetryAfterFailureSeconds = 5 * 60;

// 静态全局天气信息与保护互斥锁
static weather_info_t s_weather_info = {
    .temp = 0,
    .weather_text = "",
    .icon_path = "",
    .is_valid = false
};
static SemaphoreHandle_t s_weather_mutex = NULL;

static const char *get_weather_icon_by_code(const char *code)
{
    if (code == NULL) return "A:/weather/duoyun.png";
    int code_val = atoi(code);
    if (code_val >= 0 && code_val <= 3) {
        return "A:/weather/sunny.png";
    } else if (code_val >= 4 && code_val <= 8) {
        return "A:/weather/duoyun.png";
    } else if (code_val == 9) {
        return "A:/weather/yintian.png";
    } else if (code_val >= 10 && code_val <= 18) {
        return "A:/weather/rain.png";
    } else if (code_val >= 19 && code_val <= 25) {
        return "A:/weather/snow.png";
    } else if (code_val == 32) {
        return "A:/weather/wu.png";
    } else if (code_val == 33) {
        return "A:/weather/mai.png";
    } else if (code_val == 36) {
        return "A:/weather/wind.png";
    }
    return "A:/weather/duoyun.png";
}

static uint32_t get_weather_refresh_interval_seconds(void)
{
    power_policy_budget_t budget = power_policy_get_budget();

    if (budget.state == POWER_POLICY_STATE_STANDBY ||
        budget.display_budget == POWER_POLICY_DISPLAY_OFF ||
        budget.background_budget == POWER_POLICY_BACKGROUND_PAUSE_OPTIONAL) {
        return kWeatherRefreshLowPowerSeconds;
    }

    return kWeatherRefreshNormalSeconds;
}

static uint32_t get_weather_retry_elapsed_seconds(uint32_t refresh_interval_seconds)
{
    if (refresh_interval_seconds <= kWeatherRetryAfterFailureSeconds) {
        return 0;
    }

    return refresh_interval_seconds - kWeatherRetryAfterFailureSeconds;
}

void weather_service_update_info(int temp, const char *text, const char *code)
{
    if (s_weather_mutex == NULL) {
        s_weather_mutex = xSemaphoreCreateMutex();
    }

    if (xSemaphoreTake(s_weather_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        s_weather_info.temp = temp;
        if (text != NULL) {
            strncpy(s_weather_info.weather_text, text, sizeof(s_weather_info.weather_text) - 1);
            s_weather_info.weather_text[sizeof(s_weather_info.weather_text) - 1] = '\0';
        }
        const char *icon = get_weather_icon_by_code(code);
        strncpy(s_weather_info.icon_path, icon, sizeof(s_weather_info.icon_path) - 1);
        s_weather_info.icon_path[sizeof(s_weather_info.icon_path) - 1] = '\0';
        s_weather_info.is_valid = true;
        xSemaphoreGive(s_weather_mutex);
        ESP_LOGI(TAG, "Weather service state updated: temp=%d, text=%s, icon=%s", temp, text, icon);
    }
}

esp_err_t weather_service_get_info(weather_info_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (s_weather_mutex == NULL) {
        out->is_valid = false;
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_FAIL;
    if (xSemaphoreTake(s_weather_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        memcpy(out, &s_weather_info, sizeof(weather_info_t));
        xSemaphoreGive(s_weather_mutex);
        ret = ESP_OK;
    }
    return ret;
}

void time_and_weather(void *pvParameters)
{
    (void)pvParameters;

    ESP_LOGI(TAG, "Time and weather background service task started.");

    if (s_weather_mutex == NULL) {
        s_weather_mutex = xSemaphoreCreateMutex();
    }

    uint32_t seconds_since_last_fetch = kWeatherRefreshNormalSeconds; // 首次联网后立刻拉取一次。
    while (1)
    {
        network_manager_state_t net_state = network_manager_get_state();
        uint32_t refresh_interval_seconds = get_weather_refresh_interval_seconds();
        if (net_state == NETWORK_MANAGER_STATE_CONNECTED)
        {
            if (seconds_since_last_fetch >= refresh_interval_seconds)
            {
                ESP_LOGI(TAG, "Wi-Fi Connected. Fetching weather from Seniverse API interval=%lu seconds...",
                         (unsigned long)refresh_interval_seconds);
                esp_err_t err = http_rest_with_url();
                if (err == ESP_OK) {
                    seconds_since_last_fetch = 0;
                } else {
                    seconds_since_last_fetch =
                        get_weather_retry_elapsed_seconds(refresh_interval_seconds);
                    ESP_LOGW(TAG, "Weather fetch failed: %s, retry in %lu seconds",
                             esp_err_to_name(err),
                             (unsigned long)kWeatherRetryAfterFailureSeconds);
                }
            }
        }
        else
        {
            if (seconds_since_last_fetch < refresh_interval_seconds) {
                seconds_since_last_fetch = refresh_interval_seconds;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(kWeatherTaskPeriodSeconds * 1000));
        if (seconds_since_last_fetch <= UINT32_MAX - kWeatherTaskPeriodSeconds) {
            seconds_since_last_fetch += kWeatherTaskPeriodSeconds;
        }
    }
}
