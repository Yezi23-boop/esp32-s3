/**
 * @file lv_port_tick.c
 * @brief LVGL tick 定时器实现
 */

#include "esp_timer.h"
#include "lv_port_internal.h"

/**
 * @brief `esp_timer` 回调，用于推进 LVGL 逻辑时钟。
 * @param[in] arg 传入 `tick_interval` 指针，单位为毫秒。
 * @return 无返回值。
 */
static void lv_port_tick_cb(void *arg)
{
    uint32_t tick_interval = *((uint32_t *)arg);
    lv_tick_inc(tick_interval);
}

/**
 * @brief 初始化 LVGL tick 定时器。
 * @return 无返回值。
 *
 * @note 当前使用 `ESP_TIMER_TASK` 分发方式，避免在 ISR 中执行 LVGL 相关逻辑。
 */
void lv_port_tick_init(void)
{
    // LVGL 逻辑时钟步进间隔，单位为毫秒；间隔越小响应越细，但 CPU 唤醒更频繁。
    static uint32_t tick_interval = 5;

    // 创建周期定时器，使用任务上下文回调，避免 ISR 中执行复杂逻辑。
    const esp_timer_create_args_t arg = {
        .arg = &tick_interval,
        .callback = lv_port_tick_cb,
        .name = "lvgl",
        .dispatch_method = ESP_TIMER_TASK,
        .skip_unhandled_events = true,
    };

    esp_timer_handle_t timer_handle;
    // `esp_timer_start_periodic()` 的周期单位是微秒，因此这里需要乘以 1000。
    esp_timer_create(&arg, &timer_handle);
    esp_timer_start_periodic(timer_handle, tick_interval * 1000);
}
