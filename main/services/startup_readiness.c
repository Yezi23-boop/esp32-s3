#include "services/startup_readiness.h"

#include "freertos/event_groups.h"

#define STARTUP_READINESS_UI_FIRST_FRAME_BIT BIT0

static StaticEventGroup_t s_startup_readiness_events_buffer;
static EventGroupHandle_t s_startup_readiness_events = NULL;
static portMUX_TYPE s_startup_readiness_lock = portMUX_INITIALIZER_UNLOCKED;

static EventGroupHandle_t startup_readiness_get_events(void)
{
    EventGroupHandle_t events = NULL;

    taskENTER_CRITICAL(&s_startup_readiness_lock);
    if (s_startup_readiness_events == NULL)
    {
        s_startup_readiness_events =
            xEventGroupCreateStatic(&s_startup_readiness_events_buffer);
    }
    events = s_startup_readiness_events;
    taskEXIT_CRITICAL(&s_startup_readiness_lock);

    return events;
}

esp_err_t startup_readiness_init(void)
{
    return startup_readiness_get_events() != NULL ? ESP_OK : ESP_FAIL;
}

void startup_readiness_mark_ui_first_frame_ready(void)
{
    EventGroupHandle_t events = startup_readiness_get_events();
    if (events == NULL)
    {
        return;
    }

    (void)xEventGroupSetBits(events, STARTUP_READINESS_UI_FIRST_FRAME_BIT);
}

bool startup_readiness_is_ui_first_frame_ready(void)
{
    EventGroupHandle_t events = startup_readiness_get_events();
    if (events == NULL)
    {
        return false;
    }

    EventBits_t bits = xEventGroupGetBits(events);
    return (bits & STARTUP_READINESS_UI_FIRST_FRAME_BIT) != 0;
}

bool startup_readiness_wait_ui_first_frame(TickType_t timeout_ticks)
{
    EventGroupHandle_t events = startup_readiness_get_events();
    if (events == NULL)
    {
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(
        events,
        STARTUP_READINESS_UI_FIRST_FRAME_BIT,
        pdFALSE,
        pdTRUE,
        timeout_ticks);
    return (bits & STARTUP_READINESS_UI_FIRST_FRAME_BIT) != 0;
}
