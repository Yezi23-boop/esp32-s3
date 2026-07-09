#ifndef FOREGROUND_RUNTIME_GATE_H
#define FOREGROUND_RUNTIME_GATE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 强前台资源 owner。
     *
     * 这里只记录用户显式前台重任务的互斥 owner。ESP-DL / 安全检测属于可抢占
     * 增强任务，不在这里注册为强 owner，而是在启动或推理窗口前观察 gate 状态并让路。
     */
    typedef enum
    {
        FOREGROUND_RUNTIME_OWNER_NONE = 0, /**< 当前没有强前台 owner。 */
        FOREGROUND_RUNTIME_OWNER_HERMES, /**< Hermes 页面录音、WebSocket 或前台交互。 */
        FOREGROUND_RUNTIME_OWNER_OFFICIAL_CHAT, /**< 小智 official_chat 页面 WebSocket 或前台语音交互。 */
        FOREGROUND_RUNTIME_OWNER_BLE_PROVISIONING, /**< BLE 配网或 BLE 启动敏感窗口。 */
        FOREGROUND_RUNTIME_OWNER_OTA, /**< OTA 或系统维护重任务。 */
        FOREGROUND_RUNTIME_OWNER_FUTURE_PAGE, /**< 后续前台重交互页面的预留 owner。 */
    } foreground_runtime_owner_t;

    /**
     * @brief 初始化强前台运行时 gate。
     *
     * 该 gate 是轻量状态事实，不创建任务、不分配堆内存、不直接暂停任何 owner。
     *
     * @return `ESP_OK` 表示初始化成功或已初始化。
     */
    esp_err_t foreground_runtime_gate_init(void);

    /**
     * @brief 申请强前台独占窗口。
     *
     * @param[in] owner 申请窗口的 owner，不能为 `FOREGROUND_RUNTIME_OWNER_NONE`。
     * @param[in] timeout_ms 当前版本不阻塞等待，只保留参数用于后续兼容；传 0 即可。
     * @return `ESP_OK` 表示申请成功；`ESP_ERR_INVALID_ARG` 表示 owner 无效；
     *         `ESP_ERR_INVALID_STATE` 表示已有其他强前台 owner 或 quiet window 未结束。
     */
    esp_err_t foreground_runtime_gate_acquire(foreground_runtime_owner_t owner,
                                              uint32_t timeout_ms);

    /**
     * @brief 释放强前台独占窗口。
     *
     * 只有当前持有者可以释放；其他 owner 调用会被拒绝，避免非 owner 假释放。
     *
     * @param[in] owner 当前持有者。
     * @return `ESP_OK` 表示释放成功；`ESP_ERR_INVALID_STATE` 表示 owner 不匹配。
     */
    esp_err_t foreground_runtime_gate_release(foreground_runtime_owner_t owner);

    /**
     * @brief 查询是否存在强前台 owner。
     *
     * @return true 表示 Hermes / BLE / OTA / 未来前台重任务正在占用强前台窗口。
     */
    bool foreground_runtime_gate_is_active(void);

    /**
     * @brief 查询当前强前台 owner。
     *
     * @return 当前 owner；无 owner 时返回 `FOREGROUND_RUNTIME_OWNER_NONE`。
     */
    foreground_runtime_owner_t foreground_runtime_gate_current_owner(void);

    /**
     * @brief 打开短暂 quiet window，阻止新的强前台 owner 进入。
     *
     * quiet window 用于 BLE 启动前、低内存重试前等短互斥窗口。它不停止已有 owner，
     * 也不直接控制 Wi-Fi / ESP-DL / UI。
     *
     * @param[in] duration_ms quiet window 持续时间，0 表示立即清空。
     * @param[in] reason 日志原因，可为 NULL，不能包含密钥或 token。
     */
    void foreground_runtime_gate_quiet_for(uint32_t duration_ms,
                                           const char *reason);

    /**
     * @brief 查询当前是否处于 quiet window。
     *
     * @return true 表示 quiet window 尚未结束。
     */
    bool foreground_runtime_gate_is_quiet(void);

    /**
     * @brief 将 owner 枚举转为日志文本。
     *
     * @param[in] owner owner 枚举。
     * @return 常量字符串，调用方不得释放。
     */
    const char *foreground_runtime_gate_owner_text(
        foreground_runtime_owner_t owner);

#ifdef __cplusplus
}
#endif

#endif // FOREGROUND_RUNTIME_GATE_H
