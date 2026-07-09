#ifndef BACKGROUND_HTTPS_GATE_H
#define BACKGROUND_HTTPS_GATE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 低优先级后台 HTTPS 请求来源。
     *
     * 该 gate 只用于错峰后台 health/sync/inbox/weather 等 HTTPS 请求，前台
     * Hermes WebSocket、用户主动语音上传和取消请求不走这里。
     */
    typedef enum
    {
        BACKGROUND_HTTPS_GATE_REASON_MEMORY_WATCH_HEALTH = 0,
        BACKGROUND_HTTPS_GATE_REASON_MEMORY_WATCH_SYNC,
        BACKGROUND_HTTPS_GATE_REASON_MEMORY_WATCH_INBOX,
        BACKGROUND_HTTPS_GATE_REASON_MEMORY_WATCH_MARK_READ,
        BACKGROUND_HTTPS_GATE_REASON_MEMORY_WATCH_ALERT,
        BACKGROUND_HTTPS_GATE_REASON_WEATHER,
    } background_https_gate_reason_t;

    /**
     * @brief 初始化后台 HTTPS gate。
     *
     * @return `ESP_OK` 表示初始化成功或已经初始化。
     */
    esp_err_t background_https_gate_init(void);

    /**
     * @brief 申请后台 HTTPS 令牌。
     *
     * quiet window 未结束时直接拒绝；已有后台请求在途时按 wait_ticks 等待。
     *
     * @param[in] reason 请求来源。
     * @param[in] wait_ticks 等待令牌的 FreeRTOS tick 数。
     * @return `ESP_OK` 表示拿到令牌；`ESP_ERR_TIMEOUT` 表示忙；`ESP_ERR_INVALID_STATE` 表示 quiet window。
     */
    esp_err_t background_https_gate_acquire(
        background_https_gate_reason_t reason,
        TickType_t wait_ticks);

    /**
     * @brief 释放后台 HTTPS 令牌。
     *
     * @param[in] reason 请求来源，仅用于日志。
     */
    void background_https_gate_release(background_https_gate_reason_t reason);

    /**
     * @brief 打开短 quiet window，阻止新的后台 HTTPS 请求进入。
     *
     * @param[in] duration_ms 持续时间，0 表示清空。
     * @param[in] reason 日志原因，可为 NULL。
     */
    void background_https_gate_quiet_for(uint32_t duration_ms,
                                         const char *reason);

    /**
     * @brief 查询是否处于后台 HTTPS quiet window。
     *
     * @return true 表示 quiet window 尚未结束。
     */
    bool background_https_gate_is_quiet(void);

    /**
     * @brief 将 reason 枚举转为日志文本。
     *
     * @param[in] reason 请求来源。
     * @return 常量字符串，调用方不得释放。
     */
    const char *background_https_gate_reason_text(
        background_https_gate_reason_t reason);

#ifdef __cplusplus
}
#endif

#endif // BACKGROUND_HTTPS_GATE_H
