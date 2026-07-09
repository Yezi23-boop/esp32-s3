#include "app/board_button.h"

#include <stdbool.h>

#include "button_gpio.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "iot_button.h"
#include "ui_refresh_policy.h"

static const char *TAG = "board_button";

/* 当前板型上 BOOT 键接在 GPIO10；高电平表示按下。 */
#define BOARD_BUTTON_GPIO_NUM GPIO_NUM_10
#define BOARD_BUTTON_EVENT_QUEUE_LENGTH 4

static button_handle_t s_button_handle;
static QueueHandle_t s_button_event_queue;
static bool s_button_initialized;

static void board_button_reset_driver_state(void)
{
    if (s_button_handle != NULL) {
        (void)iot_button_delete(s_button_handle);
        s_button_handle = NULL;
    }
    if (s_button_event_queue != NULL) {
        vQueueDelete(s_button_event_queue);
        s_button_event_queue = NULL;
    }
    s_button_initialized = false;
}

static void board_button_publish_event(board_button_event_t event)
{
    if (event == BOARD_BUTTON_EVENT_NONE) {
        return;
    }

    if (s_button_event_queue != NULL &&
        xQueueSendToBack(s_button_event_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Button event queue full, dropping event=%d", event);
    }
    ui_refresh_policy_notify_activity();
}

static void board_button_single_click_cb(void *arg, void *data)
{
    (void)arg;
    (void)data;

    board_button_publish_event(BOARD_BUTTON_EVENT_SINGLE_CLICK);
}

static void board_button_long_press_start_cb(void *arg, void *data)
{
    (void)arg;
    (void)data;

    ESP_LOGI(TAG, "BUTTON_LONG_PRESS_START");
    board_button_publish_event(BOARD_BUTTON_EVENT_LONG_PRESS);
}

esp_err_t board_button_init(void)
{
    if (s_button_initialized) {
        return ESP_OK;
    }

    if (s_button_event_queue == NULL) {
        s_button_event_queue =
            xQueueCreate(BOARD_BUTTON_EVENT_QUEUE_LENGTH,
                         sizeof(board_button_event_t));
        if (s_button_event_queue == NULL) {
            ESP_LOGE(TAG, "Button event queue create failed");
            return ESP_ERR_NO_MEM;
        }
    }

    /*
     * 阈值由 button 组件按毫秒解释。长按 1500ms 避免和 2048 快速暂停
     * 的短按冲突，同时保留误触退出保护。
     */
    button_config_t button_cfg = {
        .long_press_time = 1500,
        .short_press_time = 180,
    };
    button_gpio_config_t gpio_cfg = {
        .gpio_num = BOARD_BUTTON_GPIO_NUM,
        .active_level = 1,
        .enable_power_save = true,
        .disable_pull = false,
    };

    esp_err_t err =
        iot_button_new_gpio_device(&button_cfg, &gpio_cfg, &s_button_handle);
    if (err != ESP_OK) {
        s_button_handle = NULL;
        ESP_LOGE(TAG, "Button create failed: %s", esp_err_to_name(err));
        return err;
    }

    err = iot_button_register_cb(s_button_handle, BUTTON_SINGLE_CLICK, NULL,
                                 board_button_single_click_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Single click callback register failed: %s",
                 esp_err_to_name(err));
        board_button_reset_driver_state();
        return err;
    }

    err = iot_button_register_cb(s_button_handle, BUTTON_LONG_PRESS_START, NULL,
                                 board_button_long_press_start_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Long press callback register failed: %s",
                 esp_err_to_name(err));
        board_button_reset_driver_state();
        return err;
    }

    s_button_initialized = true;
    return ESP_OK;
}

board_button_event_t board_button_consume_event(void)
{
    board_button_event_t event = BOARD_BUTTON_EVENT_NONE;

    if (s_button_event_queue == NULL) {
        return BOARD_BUTTON_EVENT_NONE;
    }

    if (xQueueReceive(s_button_event_queue, &event, 0) != pdTRUE) {
        return BOARD_BUTTON_EVENT_NONE;
    }
    return event;
}

void board_button_clear_events(void)
{
    if (s_button_event_queue == NULL) {
        return;
    }

    board_button_event_t event = BOARD_BUTTON_EVENT_NONE;
    while (xQueueReceive(s_button_event_queue, &event, 0) == pdTRUE) {
    }
}
