#ifndef STARTUP_READINESS_H
#define STARTUP_READINESS_H

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 初始化启动阶段 readiness 标志存储。
     *
     * 该模块只记录系统启动阶段的只读边界信号，供后台 manager 等待真实
     * ready 事件，而不是依赖固定延时猜测。
     *
     * @return `ESP_OK` 表示初始化成功或已初始化；其他错误表示 readiness
     *         标志不可用。
     */
    esp_err_t startup_readiness_init(void);

    /**
     * @brief 标记 UI 首帧已经完成。
     *
     * 只能由 UI/LVGL owner 在业务 UI 构建和首轮状态绑定完成后调用。
     */
    void startup_readiness_mark_ui_first_frame_ready(void);

    /**
     * @brief 查询 UI 首帧是否已经完成。
     *
     * @return true 表示 `ui_first_frame_ready` 已发布。
     */
    bool startup_readiness_is_ui_first_frame_ready(void);

    /**
     * @brief 等待 UI 首帧完成。
     *
     * @param[in] timeout_ticks 等待超时 tick；传 `portMAX_DELAY` 表示持续等待。
     * @return true 表示等待期间观察到 `ui_first_frame_ready`。
     */
    bool startup_readiness_wait_ui_first_frame(TickType_t timeout_ticks);

#ifdef __cplusplus
}
#endif

#endif // STARTUP_READINESS_H
