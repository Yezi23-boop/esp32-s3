#include "ui_refresh_policy.h"

#include "co5300_panel.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "network_manager.h"

#include <limits.h>
#include <stddef.h>

/*
 * 刷新策略实现说明：
 * 1. 该模块只维护“界面是否活跃”的事实，不直接管理触摸采样或 LVGL tick；
 * 2. 延时策略和亮度策略复用同一状态机，保证 STANDBY 降频和渐暗同步发生；
 * 3. 所有对外接口都允许被高层多次调用，初始化和状态切换需保持幂等。
 */

typedef enum
{
    UI_REFRESH_POLICY_STATE_ACTIVE = 0,
    UI_REFRESH_POLICY_STATE_STANDBY,
    UI_REFRESH_POLICY_STATE_FORCE_ACTIVE,
} ui_refresh_policy_state_t;

typedef enum
{
    UI_REFRESH_POLICY_THROTTLE_MODE_NORMAL = 0,
    UI_REFRESH_POLICY_THROTTLE_MODE_PROVISIONING,
} ui_refresh_policy_throttle_mode_internal_t;

static const char *TAG = "ui_refresh_policy";
static const uint32_t k_active_delay_ms = 16U;                 /* 活跃态最大循环延时，单位为毫秒。 */
static const uint32_t k_standby_delay_ms = 500U;               /* STANDBY 最小循环延时，单位为毫秒。 */
static const uint32_t k_provisioning_active_delay_ms = 80U;    /* BLE 配网活跃态最小循环延时，单位为毫秒。 */
static const uint32_t k_provisioning_standby_delay_ms = 500U;  /* BLE 配网待机态最小循环延时，单位为毫秒。 */
static const int64_t k_standby_timeout_us = 30000LL * 1000LL;  /* 无交互进入 STANDBY 的阈值，单位为微秒。 */
static const int64_t k_standby_fade_us = 5000LL * 1000LL;      /* STANDBY 渐暗时长，单位为微秒。 */
static const uint8_t k_default_user_brightness_percent = 100U; /* 默认用户亮度，单位为百分比。 */

static bool s_initialized = false;
static bool s_force_active = false;                                        /* 强制活跃标志，由上层场景控制。 */
static uint8_t s_user_brightness_percent = 100U;                           /* 用户配置的原始亮度百分比。 */
static uint8_t s_applied_brightness_percent = UCHAR_MAX;                   /* 最近一次成功下发给面板的亮度；`UCHAR_MAX` 表示尚未下发。 */
static int64_t s_last_touch_time_us = 0;                                   /* 最近一次用户活跃时间戳，单位为微秒。 */
static int64_t s_standby_enter_time_us = 0;                                /* 最近一次进入 STANDBY 的时间戳，单位为微秒。 */
static ui_refresh_policy_state_t s_state = UI_REFRESH_POLICY_STATE_ACTIVE;  /* 当前交互活跃状态。 */
static ui_refresh_policy_throttle_mode_internal_t s_throttle_mode =
    UI_REFRESH_POLICY_THROTTLE_MODE_NORMAL; /* 当前系统级刷新节流模式。 */

/**
 * @brief 限制亮度百分比到合法范围。
 * @param[in] percent 外部传入亮度值。
 * @return 0 到 100 之间的亮度百分比。
 */
static uint8_t ui_refresh_policy_clamp_percent(uint8_t percent)
{
    if (percent > 100U)
    {
        return 100U;
    }
    return percent;
}

/**
 * @brief 根据 STANDBY 渐暗策略计算目标亮度。
 *
 * STANDBY 第一版不让面板进入硬件 sleep，只通过多次亮度写入把屏幕
 * 从用户亮度逐步降到 0，降低恢复路径风险。
 *
 * @param[in] percent 用户配置的原始亮度百分比。
 * @return STANDBY 当前阶段应使用的亮度百分比。
 */
static uint8_t ui_refresh_policy_compute_standby_brightness(uint8_t percent)
{
    if (percent == 0U)
    {
        return 0U;
    }

    int64_t standby_elapsed_us = esp_timer_get_time() - s_standby_enter_time_us;
    if (standby_elapsed_us < 0)
    {
        standby_elapsed_us = 0;
    }
    if (standby_elapsed_us >= k_standby_fade_us)
    {
        return 0U;
    }

    const int64_t remaining_us = k_standby_fade_us - standby_elapsed_us;
    return (uint8_t)(((uint32_t)percent * (uint32_t)remaining_us) /
                     (uint32_t)k_standby_fade_us);
}

/**
 * @brief 判断某个刷新策略状态是否应保持用户配置的原始亮度。
 *
 * @param[in] state 当前最终刷新策略状态。
 * @return true 表示应保持用户亮度；false 表示应进入 STANDBY 渐暗亮度。
 */
static bool ui_refresh_policy_state_uses_full_brightness(
    ui_refresh_policy_state_t state)
{
    return state == UI_REFRESH_POLICY_STATE_ACTIVE ||
           state == UI_REFRESH_POLICY_STATE_FORCE_ACTIVE;
}

/**
 * @brief 根据当前状态机推导真正要下发给面板的亮度。
 * @return 应写入面板的亮度百分比。
 */
static uint8_t ui_refresh_policy_get_effective_brightness_percent(void)
{
    if (ui_refresh_policy_state_uses_full_brightness(s_state))
    {
        return s_user_brightness_percent;
    }

    return ui_refresh_policy_compute_standby_brightness(s_user_brightness_percent);
}

/**
 * @brief 将内部状态枚举转换成日志可读字符串。
 * @param[in] state 内部状态机状态。
 * @return 状态对应的短字符串。
 */
static const char *ui_refresh_policy_state_name(ui_refresh_policy_state_t state)
{
    switch (state)
    {
    case UI_REFRESH_POLICY_STATE_ACTIVE:
        return "active";
    case UI_REFRESH_POLICY_STATE_STANDBY:
        return "standby";
    case UI_REFRESH_POLICY_STATE_FORCE_ACTIVE:
        return "force_active";
    default:
        return "unknown";
    }
}

/**
 * @brief 将内部活跃状态转换成公开快照枚举。
 * @param[in] state 内部状态机状态。
 * @return 对外稳定的活跃状态。
 */
static ui_refresh_policy_activity_state_t ui_refresh_policy_public_activity_state(
    ui_refresh_policy_state_t state)
{
    switch (state)
    {
    case UI_REFRESH_POLICY_STATE_ACTIVE:
        return UI_REFRESH_POLICY_ACTIVITY_ACTIVE;
    case UI_REFRESH_POLICY_STATE_STANDBY:
        return UI_REFRESH_POLICY_ACTIVITY_STANDBY;
    case UI_REFRESH_POLICY_STATE_FORCE_ACTIVE:
        return UI_REFRESH_POLICY_ACTIVITY_FORCE_ACTIVE;
    default:
        return UI_REFRESH_POLICY_ACTIVITY_UNINITIALIZED;
    }
}

/**
 * @brief 将系统级节流模式转换成日志可读字符串。
 * @param[in] mode 当前节流模式。
 * @return 模式对应的短字符串。
 */
static const char *ui_refresh_policy_throttle_mode_name(
    ui_refresh_policy_throttle_mode_internal_t mode)
{
    switch (mode)
    {
    case UI_REFRESH_POLICY_THROTTLE_MODE_NORMAL:
        return "normal";
    case UI_REFRESH_POLICY_THROTTLE_MODE_PROVISIONING:
        return "provisioning_throttled";
    default:
        return "unknown";
    }
}

/**
 * @brief 将内部节流模式转换成公开快照枚举。
 * @param[in] mode 内部节流模式。
 * @return 对外稳定的节流模式。
 */
static ui_refresh_policy_throttle_mode_t ui_refresh_policy_public_throttle_mode(
    ui_refresh_policy_throttle_mode_internal_t mode)
{
    switch (mode)
    {
    case UI_REFRESH_POLICY_THROTTLE_MODE_PROVISIONING:
        return UI_REFRESH_POLICY_THROTTLE_PROVISIONING;
    case UI_REFRESH_POLICY_THROTTLE_MODE_NORMAL:
    default:
        return UI_REFRESH_POLICY_THROTTLE_NORMAL;
    }
}

/**
 * @brief 仅在亮度变化时向面板下发新的背光值。
 *
 * 空闲轮询会高频进入该路径；若每次都写面板寄存器，会增加无意义总线操作并干扰功耗观察。
 *
 * @return 无返回值。
 */
static void ui_refresh_policy_apply_brightness_if_needed(void)
{
    uint8_t target_percent = ui_refresh_policy_get_effective_brightness_percent();
    if (target_percent == s_applied_brightness_percent)
    {
        return;
    }

    esp_err_t ret = co5300_panel_set_brightness_percent(target_percent);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "设置亮度失败 target=%u%% err=%d", target_percent, ret);
        return;
    }

    ESP_LOGI(TAG, "apply brightness state=%s force=%d user=%u%% target=%u%%",
             ui_refresh_policy_state_name(s_state),
             s_force_active,
             s_user_brightness_percent,
             target_percent);
    s_applied_brightness_percent = target_percent;
}

/**
 * @brief 更新刷新策略状态并输出统一日志。
 * @param[in] next_state 目标状态。
 * @return 无返回值。
 */
static void ui_refresh_policy_set_state(ui_refresh_policy_state_t next_state)
{
    if (s_state == next_state)
    {
        return;
    }

    ESP_LOGI(TAG, "refresh state %s -> %s",
             ui_refresh_policy_state_name(s_state),
             ui_refresh_policy_state_name(next_state));
    s_state = next_state;
    if (next_state == UI_REFRESH_POLICY_STATE_STANDBY)
    {
        s_standby_enter_time_us = esp_timer_get_time();
    }
    else
    {
        s_standby_enter_time_us = 0;
    }
}

/**
 * @brief 更新系统级节流模式并输出统一日志。
 * @param[in] next_mode 目标节流模式。
 * @return 无返回值。
 */
static void ui_refresh_policy_set_throttle_mode(
    ui_refresh_policy_throttle_mode_internal_t next_mode)
{
    if (s_throttle_mode == next_mode)
    {
        return;
    }

    ESP_LOGI(TAG, "refresh throttle %s -> %s",
             ui_refresh_policy_throttle_mode_name(s_throttle_mode),
             ui_refresh_policy_throttle_mode_name(next_mode));
    s_throttle_mode = next_mode;
}

/**
 * @brief 查询当前是否处于官方 BLE provisioning 活跃期。
 *
 * 当前真机日志表明，BLE provisioning 建链和 Wi-Fi 扫描阶段会明显挤压片内 DMA
 * 可用内存。这里统一只认 `network_manager` 的主状态机，避免 UI 层自行猜 transport owner。
 *
 * @return true 表示当前正在走 BLE provisioning，会触发 UI 降载。
 */
static bool ui_refresh_policy_is_ble_provisioning_active(void)
{
    return network_manager_get_state_cached() ==
           NETWORK_MANAGER_STATE_PROVISIONING_BLE;
}

/**
 * @brief 根据当前交互输入条件计算活跃状态。
 *
 * 这里专门只处理“用户是否活跃/是否允许 STANDBY”这个维度：
 * - `FORCE_ACTIVE` 表示上层场景禁止自动待机；
 * - `ACTIVE` 表示最近 30 秒内仍有交互；
 * - `STANDBY` 表示已进入渐暗、低刷新运行态待机。
 *
 * BLE provisioning 的资源保护不在这里混算，而是单独走 `s_throttle_mode`。
 * 这样可以保留“配网期间仍然允许 30 秒后 STANDBY”的节电语义，避免一个状态同时承担
 * “是否活跃”和“是否要限流”两种职责。
 *
 * @return 当前交互活跃状态。
 */
static ui_refresh_policy_state_t ui_refresh_policy_compute_state(void)
{
    if (s_force_active)
    {
        return UI_REFRESH_POLICY_STATE_FORCE_ACTIVE;
    }

    int64_t idle_time_us = esp_timer_get_time() - s_last_touch_time_us;
    if (idle_time_us < 0)
    {
        idle_time_us = 0;
    }

    if (idle_time_us <= k_standby_timeout_us)
    {
        return UI_REFRESH_POLICY_STATE_ACTIVE;
    }

    return UI_REFRESH_POLICY_STATE_STANDBY;
}

/**
 * @brief 根据当前系统条件计算刷新节流模式。
 *
 * 节流模式只表达“当前系统是否需要为了资源稳定性而主动降载”。
 * 它不决定是否进入 STANDBY，也不覆盖 `ACTIVE / STANDBY / FORCE_ACTIVE` 的交互语义。
 *
 * @return 当前系统级节流模式。
 */
static ui_refresh_policy_throttle_mode_internal_t
ui_refresh_policy_compute_throttle_mode(void)
{
    return ui_refresh_policy_is_ble_provisioning_active()
               ? UI_REFRESH_POLICY_THROTTLE_MODE_PROVISIONING
               : UI_REFRESH_POLICY_THROTTLE_MODE_NORMAL;
}

/**
 * @brief 初始化刷新策略状态机。
 *
 * 初始化后默认进入活跃态，并立即把亮度同步到默认用户亮度，
 * 保证开机后不会沿用上一次缓存的 STANDBY 状态。
 */
void ui_refresh_policy_init(void)
{
    s_last_touch_time_us = esp_timer_get_time();
    s_standby_enter_time_us = 0;
    s_state = UI_REFRESH_POLICY_STATE_ACTIVE;
    s_throttle_mode = UI_REFRESH_POLICY_THROTTLE_MODE_NORMAL;
    s_force_active = false;
    s_user_brightness_percent = k_default_user_brightness_percent;
    s_applied_brightness_percent = UCHAR_MAX;
    s_initialized = true;

    ui_refresh_policy_apply_brightness_if_needed();
}

/**
 * @brief 通知策略层刚刚发生过一次用户活跃事件。
 *
 * 典型调用点是触摸驱动、按键输入或编码器旋转。
 * 该接口只刷新时间戳；真正的空闲态判断仍由 `ui_refresh_policy_poll()` 统一执行。
 */
void ui_refresh_policy_notify_activity(void)
{
    if (!s_initialized)
    {
        return;
    }

    s_last_touch_time_us = esp_timer_get_time();
    ui_refresh_policy_set_state(ui_refresh_policy_compute_state());
    ui_refresh_policy_apply_brightness_if_needed();
}

void ui_refresh_policy_notify_touch(void)
{
    ui_refresh_policy_notify_activity();
}

/**
 * @brief 打开或关闭强制活跃模式。
 * @param enabled true 表示禁止自动 STANDBY 和自动降频；false 表示恢复普通策略。
 */
void ui_refresh_policy_set_force_active(bool enabled)
{
    if (!s_initialized)
    {
        ui_refresh_policy_init();
    }

    s_force_active = enabled;
    if (enabled)
    {
        ui_refresh_policy_set_state(UI_REFRESH_POLICY_STATE_FORCE_ACTIVE);
    }

    ui_refresh_policy_poll();
}

/**
 * @brief 查询强制活跃模式是否开启。
 * @return true 表示当前不允许自动进入空闲态。
 */
bool ui_refresh_policy_is_force_active(void)
{
    return s_force_active;
}

/**
 * @brief 设置用户期望亮度百分比。
 * @param percent 用户设置的原始亮度值，超过 100 会被钳制。
 *
 * 该值不是最终输出亮度。若策略当前处于 STANDBY，最终下发值会按渐暗进度缩小。
 */
void ui_refresh_policy_set_user_brightness_percent(uint8_t percent)
{
    if (!s_initialized)
    {
        ui_refresh_policy_init();
    }

    s_user_brightness_percent = ui_refresh_policy_clamp_percent(percent);
    ui_refresh_policy_apply_brightness_if_needed();
}

/**
 * @brief 获取用户配置的原始亮度百分比。
 * @return 用户层设置值，而不是当前面板实际生效值。
 */
uint8_t ui_refresh_policy_get_user_brightness_percent(void)
{
    return s_user_brightness_percent;
}

/**
 * @brief 读取 UI 活跃度只读快照。
 *
 * 该接口只复制 `ui_refresh_policy` 已缓存的事实，不调用 `poll()`，
 * 不改变状态机，也不写面板亮度；后续 `power_policy` 接入前可先用它做低风险观测。
 *
 * @param[out] snapshot 输出快照。
 * @return true 表示读取成功；false 表示参数为空。
 */
bool ui_refresh_policy_get_activity_snapshot(
    ui_refresh_policy_activity_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return false;
    }

    const int64_t now_us = esp_timer_get_time();
    int64_t idle_time_us = now_us - s_last_touch_time_us;
    if (!s_initialized || idle_time_us < 0)
    {
        idle_time_us = 0;
    }

    snapshot->initialized = s_initialized;
    snapshot->active = s_state == UI_REFRESH_POLICY_STATE_ACTIVE ||
                       s_state == UI_REFRESH_POLICY_STATE_FORCE_ACTIVE;
    snapshot->standby = s_state == UI_REFRESH_POLICY_STATE_STANDBY;
    snapshot->force_active = s_force_active;
    snapshot->provisioning_throttled =
        s_throttle_mode == UI_REFRESH_POLICY_THROTTLE_MODE_PROVISIONING;
    snapshot->activity_state = s_initialized
                                   ? ui_refresh_policy_public_activity_state(s_state)
                                   : UI_REFRESH_POLICY_ACTIVITY_UNINITIALIZED;
    snapshot->throttle_mode =
        ui_refresh_policy_public_throttle_mode(s_throttle_mode);
    snapshot->user_brightness_percent = s_user_brightness_percent;
    snapshot->target_brightness_percent =
        ui_refresh_policy_get_effective_brightness_percent();
    snapshot->brightness_applied = s_applied_brightness_percent != UCHAR_MAX;
    snapshot->applied_brightness_percent =
        snapshot->brightness_applied ? s_applied_brightness_percent : 0U;
    snapshot->last_touch_time_us = s_last_touch_time_us;
    snapshot->idle_time_ms = idle_time_us / 1000LL;
    return true;
}

/**
 * @brief 按当前活跃状态修正 LVGL 主循环延时。
 * @param next_call_ms `lv_timer_handler()` 建议的下次唤醒时间。
 * @return 应实际传给 `vTaskDelay()` 的毫秒值。
 *
 * 活跃态优先保证流畅度，因此把最大延时压到 16ms；
 * STANDBY 优先省电，因此把最小延时抬高到 500ms。
 * 若当前正跑 BLE provisioning，则进一步把最小唤醒间隔抬高，
 * 优先降低显示 flush 与 NimBLE / Wi-Fi scan 对片内 DMA 内存的竞争。
 */
uint32_t ui_refresh_policy_adjust_delay(uint32_t next_call_ms)
{
    if (!s_initialized)
    {
        return next_call_ms;
    }

    if (s_throttle_mode == UI_REFRESH_POLICY_THROTTLE_MODE_PROVISIONING)
    {
        if (s_state == UI_REFRESH_POLICY_STATE_STANDBY)
        {
            return next_call_ms < k_provisioning_standby_delay_ms
                       ? k_provisioning_standby_delay_ms
                       : next_call_ms;
        }

        return next_call_ms < k_provisioning_active_delay_ms
                   ? k_provisioning_active_delay_ms
                   : next_call_ms;
    }

    if (s_state == UI_REFRESH_POLICY_STATE_FORCE_ACTIVE ||
        s_state == UI_REFRESH_POLICY_STATE_ACTIVE)
    {
        return next_call_ms > k_active_delay_ms ? k_active_delay_ms : next_call_ms;
    }

    return next_call_ms < k_standby_delay_ms ? k_standby_delay_ms : next_call_ms;
}

/**
 * @brief 周期轮询刷新策略状态机。
 *
 * 该函数应在 UI 主循环中高频调用。它会结合最近触摸时间和强制活跃标志，
 * 统一决定当前状态，并在状态变化时同步亮度。
 * 同时它会读取 `network_manager` 的无锁状态快照，单独驱动“配网期 UI 降载”节流模式，
 * 避免把主循环绑定到网络互斥锁上。
 */
void ui_refresh_policy_poll(void)
{
    if (!s_initialized)
    {
        return;
    }

    ui_refresh_policy_set_throttle_mode(ui_refresh_policy_compute_throttle_mode());
    ui_refresh_policy_set_state(ui_refresh_policy_compute_state());
    ui_refresh_policy_apply_brightness_if_needed();
}
