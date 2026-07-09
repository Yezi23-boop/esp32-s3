#include "services/foreground_runtime_gate.h"

#include <inttypes.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static const char *TAG = "fg_runtime_gate";

static portMUX_TYPE s_gate_lock = portMUX_INITIALIZER_UNLOCKED;
static foreground_runtime_owner_t s_current_owner =
    FOREGROUND_RUNTIME_OWNER_NONE;
static int64_t s_quiet_until_us = 0;
static bool s_initialized = false;

static int64_t foreground_runtime_gate_now_us(void)
{
    return esp_timer_get_time();
}

static bool foreground_runtime_gate_is_quiet_locked(int64_t now_us)
{
    return s_quiet_until_us > now_us;
}

const char *foreground_runtime_gate_owner_text(
    foreground_runtime_owner_t owner)
{
    switch (owner)
    {
    case FOREGROUND_RUNTIME_OWNER_HERMES:
        return "HERMES";
    case FOREGROUND_RUNTIME_OWNER_OFFICIAL_CHAT:
        return "OFFICIAL_CHAT";
    case FOREGROUND_RUNTIME_OWNER_BLE_PROVISIONING:
        return "BLE_PROVISIONING";
    case FOREGROUND_RUNTIME_OWNER_OTA:
        return "OTA";
    case FOREGROUND_RUNTIME_OWNER_FUTURE_PAGE:
        return "FUTURE_PAGE";
    case FOREGROUND_RUNTIME_OWNER_NONE:
    default:
        return "NONE";
    }
}

esp_err_t foreground_runtime_gate_init(void)
{
    portENTER_CRITICAL(&s_gate_lock);
    s_initialized = true;
    portEXIT_CRITICAL(&s_gate_lock);
    return ESP_OK;
}

esp_err_t foreground_runtime_gate_acquire(foreground_runtime_owner_t owner,
                                          uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (owner == FOREGROUND_RUNTIME_OWNER_NONE)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;
    foreground_runtime_owner_t blocking_owner = FOREGROUND_RUNTIME_OWNER_NONE;
    const int64_t now_us = foreground_runtime_gate_now_us();
    int64_t quiet_remaining_ms = 0;

    portENTER_CRITICAL(&s_gate_lock);
    if (!s_initialized)
    {
        s_initialized = true;
    }

    if (foreground_runtime_gate_is_quiet_locked(now_us))
    {
        quiet_remaining_ms = (s_quiet_until_us - now_us) / 1000;
        ret = ESP_ERR_INVALID_STATE;
    }
    else if (s_current_owner != FOREGROUND_RUNTIME_OWNER_NONE &&
             s_current_owner != owner)
    {
        blocking_owner = s_current_owner;
        ret = ESP_ERR_INVALID_STATE;
    }
    else
    {
        s_current_owner = owner;
    }
    portEXIT_CRITICAL(&s_gate_lock);

    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "foreground acquired: owner=%s",
                 foreground_runtime_gate_owner_text(owner));
    }
    else if (quiet_remaining_ms > 0)
    {
        ESP_LOGW(TAG,
                 "foreground acquire denied: owner=%s quiet_remaining_ms=%" PRId64,
                 foreground_runtime_gate_owner_text(owner),
                 quiet_remaining_ms);
    }
    else
    {
        ESP_LOGW(TAG, "foreground acquire denied: owner=%s current=%s",
                 foreground_runtime_gate_owner_text(owner),
                 foreground_runtime_gate_owner_text(blocking_owner));
    }
    return ret;
}

esp_err_t foreground_runtime_gate_release(foreground_runtime_owner_t owner)
{
    if (owner == FOREGROUND_RUNTIME_OWNER_NONE)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;
    foreground_runtime_owner_t current = FOREGROUND_RUNTIME_OWNER_NONE;

    portENTER_CRITICAL(&s_gate_lock);
    current = s_current_owner;
    if (s_current_owner == owner)
    {
        s_current_owner = FOREGROUND_RUNTIME_OWNER_NONE;
    }
    else
    {
        ret = ESP_ERR_INVALID_STATE;
    }
    portEXIT_CRITICAL(&s_gate_lock);

    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "foreground released: owner=%s",
                 foreground_runtime_gate_owner_text(owner));
    }
    else
    {
        ESP_LOGW(TAG, "foreground release denied: owner=%s current=%s",
                 foreground_runtime_gate_owner_text(owner),
                 foreground_runtime_gate_owner_text(current));
    }
    return ret;
}

bool foreground_runtime_gate_is_active(void)
{
    bool active = false;
    portENTER_CRITICAL(&s_gate_lock);
    active = s_current_owner != FOREGROUND_RUNTIME_OWNER_NONE;
    portEXIT_CRITICAL(&s_gate_lock);
    return active;
}

foreground_runtime_owner_t foreground_runtime_gate_current_owner(void)
{
    foreground_runtime_owner_t owner = FOREGROUND_RUNTIME_OWNER_NONE;
    portENTER_CRITICAL(&s_gate_lock);
    owner = s_current_owner;
    portEXIT_CRITICAL(&s_gate_lock);
    return owner;
}

void foreground_runtime_gate_quiet_for(uint32_t duration_ms,
                                       const char *reason)
{
    const int64_t now_us = foreground_runtime_gate_now_us();
    const int64_t duration_us = (int64_t)duration_ms * 1000LL;
    int64_t quiet_until_us = 0;

    portENTER_CRITICAL(&s_gate_lock);
    if (!s_initialized)
    {
        s_initialized = true;
    }
    s_quiet_until_us = duration_ms == 0U ? 0 : now_us + duration_us;
    quiet_until_us = s_quiet_until_us;
    portEXIT_CRITICAL(&s_gate_lock);

    ESP_LOGI(TAG, "foreground quiet window: duration_ms=%" PRIu32
                  " until_us=%" PRId64 " reason=%s",
             duration_ms, quiet_until_us, reason != NULL ? reason : "none");
}

bool foreground_runtime_gate_is_quiet(void)
{
    bool quiet = false;
    const int64_t now_us = foreground_runtime_gate_now_us();
    portENTER_CRITICAL(&s_gate_lock);
    quiet = foreground_runtime_gate_is_quiet_locked(now_us);
    portEXIT_CRITICAL(&s_gate_lock);
    return quiet;
}
