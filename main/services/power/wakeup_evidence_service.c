#include "services/power/wakeup_evidence_service.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "axp2101.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pcf85063atl.h"

static const char *TAG = "wakeup_evidence";

static const gpio_num_t k_rtc_int_gpio = GPIO_NUM_39;       /* 原理图确认 RTC_INT -> GPIO39。 */
static const uint8_t k_rtc_timer_seconds = 8U;              /* 上板观察窗口，单位秒。 */
static const TickType_t k_poll_interval_ticks = pdMS_TO_TICKS(1000);
static const TickType_t k_clear_settle_ticks = pdMS_TO_TICKS(20);

#define PCF85063ATL_CONTROL2_TF (1u << 3)

static bool s_initialized = false;
static bool s_started = false;
static bool s_rtc_present = false;
static bool s_rtc_runtime_evidence_ready = false;
static bool s_rtc_timer_stopped_after_evidence = false;
static TaskHandle_t s_task_handle = NULL;

static esp_err_t wakeup_evidence_configure_rtc_int_gpio(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << k_rtc_int_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

static void wakeup_evidence_log_rtc_time_once(void)
{
    pcf85063atl_time_t time = {0};
    esp_err_t ret = pcf85063atl_read_time(&time);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "rtc_time_read_failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG,
             "rtc_time_snapshot: os=%d %02u-%02u-%02u weekday=%u %02u:%02u:%02u",
             time.oscillator_stopped,
             time.years,
             time.months,
             time.days,
             time.weekdays,
             time.hours,
             time.minutes,
             time.seconds);
}

static void wakeup_evidence_log_axp_irq_if_any(void)
{
    axp2101_irq_status_t irq = {0};
    esp_err_t ret = axp2101_read_irq_status(&irq);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "axp_irq_read_failed: %s", esp_err_to_name(ret));
        return;
    }

    if (irq.irq0 == 0 && irq.irq1 == 0 && irq.irq2 == 0)
    {
        return;
    }

    ESP_LOGI(TAG, "axp_irq_snapshot: irq0=0x%02x irq1=0x%02x irq2=0x%02x",
             irq.irq0, irq.irq1, irq.irq2);

    ret = axp2101_clear_irq_status(&irq);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "axp_irq_clear_failed: %s", esp_err_to_name(ret));
    }
}

static bool wakeup_evidence_clear_rtc_timer_flag(int level_before_clear)
{
    ESP_LOGI(TAG, "rtc_timer_flag_observed: clearing_tf");
    esp_err_t ret = pcf85063atl_clear_interrupt_flags();
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "rtc_timer_flag_clear_failed: %s", esp_err_to_name(ret));
        return false;
    }

    vTaskDelay(k_clear_settle_ticks);

    uint8_t control2_after = 0;
    ret = pcf85063atl_read_control2(&control2_after);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "rtc_control2_after_clear_read_failed: %s",
                 esp_err_to_name(ret));
        return false;
    }

    int level_after = gpio_get_level(k_rtc_int_gpio);
    ESP_LOGI(TAG,
             "rtc_timer_flag_cleared: level_before=%d level_after=%d control2_after=0x%02x",
             level_before_clear,
             level_after,
             control2_after);
    return level_after == 1 && (control2_after & PCF85063ATL_CONTROL2_TF) == 0;
}

static void wakeup_evidence_stop_runtime_timer_after_evidence(void)
{
    if (s_rtc_timer_stopped_after_evidence)
    {
        return;
    }

    esp_err_t ret = pcf85063atl_stop_countdown_timer();
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "rtc_timer_stop_after_evidence_failed: %s",
                 esp_err_to_name(ret));
        return;
    }

    s_rtc_timer_stopped_after_evidence = true;
    ESP_LOGI(TAG, "rtc_timer_stopped_after_evidence");
}

static void wakeup_evidence_task(void *arg)
{
    (void)arg;

    if (s_rtc_present)
    {
        wakeup_evidence_log_rtc_time_once();

        esp_err_t ret = pcf85063atl_arm_countdown_timer(k_rtc_timer_seconds);
        if (ret == ESP_OK)
        {
            ESP_LOGI(TAG,
                     "rtc_timer_armed: seconds=%u rtc_int_gpio=%d initial_level=%d",
                     k_rtc_timer_seconds,
                     k_rtc_int_gpio,
                     gpio_get_level(k_rtc_int_gpio));
        }
        else
        {
            ESP_LOGW(TAG, "rtc_timer_arm_failed: %s", esp_err_to_name(ret));
        }
    }

    while (1)
    {
        if (s_rtc_present)
        {
            uint8_t control2 = 0;
            esp_err_t ret = pcf85063atl_read_control2(&control2);
            if (ret == ESP_OK)
            {
                int rtc_int_level = gpio_get_level(k_rtc_int_gpio);
                ESP_LOGD(TAG, "rtc_int_sample: gpio=%d level=%d control2=0x%02x",
                         k_rtc_int_gpio, rtc_int_level, control2);
                if ((control2 & PCF85063ATL_CONTROL2_TF) != 0)
                {
                    if (wakeup_evidence_clear_rtc_timer_flag(rtc_int_level))
                    {
                        s_rtc_runtime_evidence_ready = true;
                        wakeup_evidence_stop_runtime_timer_after_evidence();
                        ESP_LOGI(TAG,
                                 "rtc_runtime_evidence_ready: runtime_evidence_only");
                    }
                }
            }
            else
            {
                ESP_LOGW(TAG, "rtc_control2_read_failed: %s",
                         esp_err_to_name(ret));
            }
        }

        wakeup_evidence_log_axp_irq_if_any();
        vTaskDelay(k_poll_interval_ticks);
    }
}

esp_err_t wakeup_evidence_service_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    esp_err_t ret = wakeup_evidence_configure_rtc_int_gpio();
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = pcf85063atl_probe(&s_rtc_present);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ESP_LOGI(TAG, "wakeup evidence init: rtc_present=%d rtc_int_gpio=%d level=%d",
             s_rtc_present,
             k_rtc_int_gpio,
             gpio_get_level(k_rtc_int_gpio));
    s_initialized = true;
    return ESP_OK;
}

esp_err_t wakeup_evidence_service_start(void)
{
    esp_err_t ret = wakeup_evidence_service_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    if (s_started)
    {
        return ESP_OK;
    }

    /* 此轮询任务会直接访问 RTC 与 AXP2101。I2C 新驱动的 ISR 会在 Flash
       cache-disabled 窗口读取调用任务栈的事务描述符，所以必须留在 internal
       RAM；这不是 OTA 生命周期控制，I2C 在 OTA 期间仍可正常工作。 */
    BaseType_t ok = xTaskCreateWithCaps(wakeup_evidence_task,
                                        "wakeup_evidence",
                                        4096,
                                        NULL,
                                        4,
                                        &s_task_handle,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ok != pdPASS)
    {
        s_task_handle = NULL;
        return ESP_FAIL;
    }

    s_started = true;
    return ESP_OK;
}
