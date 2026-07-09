#ifndef UI_REFRESH_POLICY_H

#define UI_REFRESH_POLICY_H

#include <stdbool.h>

#include <stdint.h>

/*
 * 文件作用：
 * - 封装 UI 刷新频率与背光亮度之间的联动策略；
 * - 对外隐藏活跃态 / STANDBY / 强制活跃态切换细节；
 * - 提供给 LVGL 主循环、输入驱动和亮度设置控件一个稳定接口。
 *
 * UI 刷新策略模块：
 * 1. 由 `lvgl_task()` 周期性调用，决定当前界面应保持活跃刷新还是进入空闲降频。
 * 2. 降频时同时配合 `co5300_panel` 调低背光，降低整机空闲功耗。
 * 3. 当 `network_manager` 进入 BLE provisioning 时，会叠加专门的受限刷新节流模式，
 *    优先降低显示 flush 与 BLE / Wi-Fi scan 对片内 DMA 的竞争，但不会破坏原有
 *    `30s` 无活跃后进入 STANDBY 的交互语义。
 * 4. 对上层暴露的接口尽量简单，只提供“触摸通知 / 强制常亮 / 用户亮度 / 延时调整 / 只读快照”五类能力，
 *    这样 UI 控制器、输入设备和屏幕驱动可以解耦。
 */

#ifdef __cplusplus

extern "C"
{

#endif

    /**
     * @brief UI 刷新策略当前交互活跃状态。
     */
    typedef enum
    {
        UI_REFRESH_POLICY_ACTIVITY_ACTIVE = 0,      /**< 最近有交互，保持全亮和高刷新。 */
        UI_REFRESH_POLICY_ACTIVITY_STANDBY,         /**< 已进入运行态待机，允许渐进变暗和刷新降频。 */
        UI_REFRESH_POLICY_ACTIVITY_FORCE_ACTIVE,    /**< 上层场景要求禁止自动待机。 */
        UI_REFRESH_POLICY_ACTIVITY_UNINITIALIZED,   /**< 策略尚未初始化。 */
    } ui_refresh_policy_activity_state_t;

    /**
     * @brief UI 刷新策略当前系统级节流模式。
     */
    typedef enum
    {
        UI_REFRESH_POLICY_THROTTLE_NORMAL = 0,       /**< 普通刷新策略。 */
        UI_REFRESH_POLICY_THROTTLE_PROVISIONING,     /**< BLE 配网期间主动降低刷新压力。 */
    } ui_refresh_policy_throttle_mode_t;

    /**
     * @brief UI 活跃度只读快照。
     *
     * 该结构只表达 `ui_refresh_policy` 已经持有的事实，供后续 `power_policy`
     * 或诊断代码读取；读取快照不推进状态机、不写面板亮度、不触发 LVGL 操作。
     */
    typedef struct
    {
        bool initialized;                                      /**< true 表示策略已初始化。 */
        bool active;                                           /**< true 表示当前按活跃态处理。 */
        bool standby;                                          /**< true 表示当前已进入运行态待机。 */
        bool force_active;                                     /**< true 表示上层禁止自动待机。 */
        bool provisioning_throttled;                           /**< true 表示 BLE 配网刷新节流生效。 */
        ui_refresh_policy_activity_state_t activity_state;     /**< 当前交互活跃状态。 */
        ui_refresh_policy_throttle_mode_t throttle_mode;       /**< 当前系统级节流模式。 */
        uint8_t user_brightness_percent;                       /**< 用户配置的原始亮度百分比。 */
        uint8_t target_brightness_percent;                     /**< 当前状态下希望下发的目标亮度百分比。 */
        bool brightness_applied;                               /**< true 表示已有成功下发过的亮度值。 */
        uint8_t applied_brightness_percent;                    /**< 最近一次成功下发的亮度百分比，未下发时为 0。 */
        int64_t last_touch_time_us;                            /**< 最近一次用户活跃时间戳，单位微秒。 */
        int64_t idle_time_ms;                                  /**< 当前距离最近一次用户活跃的时间，单位毫秒。 */
    } ui_refresh_policy_activity_snapshot_t;

    /* 初始化内部状态并立刻把面板亮度同步到策略默认值。 */
    void ui_refresh_policy_init(void);

    /* 在触摸、编码器、按键等任意“用户活跃”事件后调用，用来刷新活跃超时计时器。 */
    void ui_refresh_policy_notify_touch(void);

    /* 在高优先级提示等非触摸场景需要唤醒 UI 时调用。 */
    void ui_refresh_policy_notify_activity(void);

    /* 强制保持活跃模式，通常用于播放动画、重要提示页或不希望被自动调暗的场景。 */
    void ui_refresh_policy_set_force_active(bool enabled);

    /* 查询当前是否处于强制活跃模式。 */
    bool ui_refresh_policy_is_force_active(void);

    /* 设置用户期望亮度百分比，策略层会在活跃/STANDBY 状态下换算出最终背光值。 */
    void ui_refresh_policy_set_user_brightness_percent(uint8_t percent);

    /* 获取用户配置的原始亮度百分比，而不是当前实际输出到屏幕的亮度。 */
    uint8_t ui_refresh_policy_get_user_brightness_percent(void);

    /**
     * @brief 读取 UI 活跃度只读快照。
     *
     * 该接口不调用 `ui_refresh_policy_poll()`，不修改内部状态，也不会写屏幕亮度。
     *
     * @param[out] snapshot 输出快照，不能为空。
     * @return true 表示读取成功；false 表示参数为空。
     */
    bool ui_refresh_policy_get_activity_snapshot(
        ui_refresh_policy_activity_snapshot_t *snapshot);

    /*
     * 根据当前策略修正 LVGL 主循环的下一次唤醒时间：
     * - 活跃态：限制最大延时，保证交互和动画流畅。
     * - STANDBY：拉长最小延时，减少无意义刷新。
     */
    uint32_t ui_refresh_policy_adjust_delay(uint32_t next_call_ms);

    /* 周期轮询状态机，通常在 UI 主循环里调用一次即可。 */
    void ui_refresh_policy_poll(void);

#ifdef __cplusplus
}

#endif

#endif // UI_REFRESH_POLICY_H
