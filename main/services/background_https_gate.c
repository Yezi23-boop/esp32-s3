#include "services/background_https_gate.h"

#include <inttypes.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"

static const char *TAG = "bg_https_gate";

static StaticSemaphore_t s_token_buffer;
static SemaphoreHandle_t s_token = NULL;
static portMUX_TYPE s_gate_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_quiet_until_us = 0;

static int64_t background_https_gate_now_us(void)
{
    return esp_timer_get_time();
}

static bool background_https_gate_is_quiet_locked(int64_t now_us)
{
    return s_quiet_until_us > now_us;
}

const char *background_https_gate_reason_text(
    background_https_gate_reason_t reason)
{
    switch (reason)
    {
    case BACKGROUND_HTTPS_GATE_REASON_MEMORY_WATCH_HEALTH:
        return "memory_watch_health";
    case BACKGROUND_HTTPS_GATE_REASON_MEMORY_WATCH_SYNC:
        return "memory_watch_sync";
    case BACKGROUND_HTTPS_GATE_REASON_MEMORY_WATCH_INBOX:
        return "memory_watch_inbox";
    case BACKGROUND_HTTPS_GATE_REASON_MEMORY_WATCH_MARK_READ:
        return "memory_watch_mark_read";
    case BACKGROUND_HTTPS_GATE_REASON_MEMORY_WATCH_ALERT:
        return "memory_watch_alert";
    case BACKGROUND_HTTPS_GATE_REASON_WEATHER:
        return "weather";
    default:
        return "unknown";
    }
}

esp_err_t background_https_gate_init(void)
{
    if (s_token != NULL)
    {
        return ESP_OK;
    }

    portENTER_CRITICAL(&s_gate_lock);
    if (s_token == NULL)
    {
        s_token = xSemaphoreCreateBinaryStatic(&s_token_buffer);
        if (s_token != NULL)
        {
            xSemaphoreGive(s_token);
        }
    }
    portEXIT_CRITICAL(&s_gate_lock);

    return s_token != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t background_https_gate_acquire(
    background_https_gate_reason_t reason,
    TickType_t wait_ticks)
{
    esp_err_t ret = background_https_gate_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    const int64_t now_us = background_https_gate_now_us();
    int64_t quiet_remaining_ms = 0;
    portENTER_CRITICAL(&s_gate_lock);
    if (background_https_gate_is_quiet_locked(now_us))
    {
        quiet_remaining_ms = (s_quiet_until_us - now_us) / 1000;
        ret = ESP_ERR_INVALID_STATE;
    }
    portEXIT_CRITICAL(&s_gate_lock);

    if (ret != ESP_OK)
    {
        ESP_LOGI(TAG,
                 "background https denied: reason=%s quiet_remaining_ms=%" PRId64,
                 background_https_gate_reason_text(reason),
                 quiet_remaining_ms);
        return ret;
    }

    if (xSemaphoreTake(s_token, wait_ticks) != pdTRUE)
    {
        ESP_LOGI(TAG, "background https busy: reason=%s",
                 background_https_gate_reason_text(reason));
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGD(TAG, "background https acquired: reason=%s",
             background_https_gate_reason_text(reason));
    return ESP_OK;
}

void background_https_gate_release(background_https_gate_reason_t reason)
{
    if (s_token == NULL)
    {
        return;
    }

    xSemaphoreGive(s_token);
    ESP_LOGD(TAG, "background https released: reason=%s",
             background_https_gate_reason_text(reason));
}

void background_https_gate_quiet_for(uint32_t duration_ms,
                                     const char *reason)
{
    const int64_t now_us = background_https_gate_now_us();
    const int64_t duration_us = (int64_t)duration_ms * 1000LL;
    int64_t quiet_until_us = 0;

    portENTER_CRITICAL(&s_gate_lock);
    s_quiet_until_us = duration_ms == 0U ? 0 : now_us + duration_us;
    quiet_until_us = s_quiet_until_us;
    portEXIT_CRITICAL(&s_gate_lock);

    ESP_LOGI(TAG, "background https quiet window: duration_ms=%" PRIu32
                  " until_us=%" PRId64 " reason=%s",
             duration_ms, quiet_until_us, reason != NULL ? reason : "none");
}

bool background_https_gate_is_quiet(void)
{
    bool quiet = false;
    const int64_t now_us = background_https_gate_now_us();
    portENTER_CRITICAL(&s_gate_lock);
    quiet = background_https_gate_is_quiet_locked(now_us);
    portEXIT_CRITICAL(&s_gate_lock);
    return quiet;
}
