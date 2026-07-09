#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "button_gpio.h"
#include "driver/gpio.h"
#include "esp_freertos_hooks.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "events_init.h"
#include "features/alerts/display_alert_adapter.h"
#include "features/weather/hptts.h"
#include "features/weather/time_weather.h"
#include "gui_guider.h"
#include "iot_button.h"
#include "lv_demos.h"
#include "lv_port.h"
#include "lvgl.h"
#include "lvgl_task.h"
#include "nvs_flash.h"
#include "printf_esp32.h"
#include "services/startup_readiness.h"
#include "ui/custom/ai_ui_controller.h"
#include "ui/custom/custom.h"
#include "ui/custom/danger_detection_controller.h"
#include "ui/custom/memory_watch_controller.h"
#include "ui/custom/mini_games_controller.h"
#include "ui/custom/wifi_management_controller.h"
#include "ui_refresh_policy.h"

/*
 * UI 主任务实现说明：
 * 1. 该任务是 LVGL 前台线程，所有需要直接操作 LVGL 对象树的逻辑都应尽量汇聚到这里；
 * 2. 告警覆盖层、危险检测轮询和刷新节流都在主循环串行执行，避免跨线程直接改 UI；
 */

static const char *TAG = "lvgl_task";
int next_call = 0; /* 最近一次 `lv_timer_handler()` 返回值，单位为毫秒。 */
lv_ui guider_ui;   /* GUI Guider 生成的全局 UI 树实例，仅 UI 线程负责初始化。 */
static TaskHandle_t cpu_monitor_task_handle = NULL; /* 低频调试任务句柄。 */

/**
 * @brief 低频 CPU 监视任务。
 * @param[in] arg 未使用，保留为 FreeRTOS 任务签名。
 * @return 无返回值。
 *
 * 当前只保留一个低频挂点，方便后续按需打印内存或调度统计，
 * 避免把调试日志直接塞进高频 UI 主循环。
 */
static void cpu_monitor_task(void *arg)
{
    (void)arg;

    /*
     * 冷启动稳态采样：首次循环打印一次内存全景与全任务栈高水位，
     * 供资源实测阶段建立基线。之后保持静默，避免高频刷屏。
     * 延迟 8 秒让所有 deferred service 完成初始化后再采样，数据更准。
     */
    vTaskDelay(pdMS_TO_TICKS(8000));
    printf_esp32_memory_stats();
    printf_esp32_all_task_stack_stats();
    ESP_LOGI(TAG, "boot_stage: cold_boot_resource_snapshot_done");

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(5000));
        // printf_esp32_memory_stats();
        // ESP_LOGI(TAG, "next_call:%d", next_call);
    }
}

/**
 * @brief LVGL 前台主任务入口。
 * @param[in] pvParameter 未使用，保留为系统任务签名。
 * @return 无返回值。
 *
 * 启动顺序上先初始化显示端口，再创建 UI、控制器和事件绑定。
 * 之后主循环固定做三类事情：
 * 1. 处理跨线程投递到 UI 线程的请求；
 * 2. 驱动 LVGL timer；
 * 3. 结合刷新策略决定下一轮延时，平衡流畅度与空闲功耗。
 *
 * @note 该任务是本模块唯一允许直接操作 LVGL 对象树的上下文。
 */
void lvgl_task(void *pvParameter)
{
    (void)pvParameter;

    ESP_LOGI(TAG, "Starting application");
    lv_port_init_small();
    ESP_LOGI(TAG, "boot_stage: display_foundation_done");
    // lv_demo_benchmark();
    // lv_demo_stress();

    setup_ui(&guider_ui);
    ai_ui_controller_init(&guider_ui);
    danger_detection_controller_init(&guider_ui);
    memory_watch_controller_init(&guider_ui);
    mini_games_controller_init(&guider_ui);
    wifi_management_controller_init(&guider_ui);
    events_init(&guider_ui);
    custom_init(&guider_ui);
    ui_refresh_policy_init();
    startup_readiness_mark_ui_first_frame_ready();
    ESP_LOGI(TAG, "boot_stage: ui_first_frame_ready");

    xTaskCreatePinnedToCore(
        cpu_monitor_task,
        "cpu_monitor",
        4096,
        NULL,
        1,
        &cpu_monitor_task_handle,
        0);

    while (1)
    {
        display_alert_adapter_process_ui();
        danger_detection_controller_poll_ui();
        memory_watch_controller_poll_ui();
        mini_games_controller_poll_ui();

        next_call = lv_timer_handler();
        ui_refresh_policy_poll();

        uint32_t next_call_ms = next_call > 0 ? (uint32_t)next_call : 0U;
        uint32_t delay_ms = ui_refresh_policy_adjust_delay(next_call_ms);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}
