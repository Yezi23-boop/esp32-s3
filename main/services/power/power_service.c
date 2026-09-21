#include "services/power/power_service.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "services/power/power_policy.h"

/*
 * 电源服务实现说明：
 * - 对底层电源采样结果做周期刷新、抖动抑制和失败降级；
 * - 采用双缓冲发布状态，读取方永远只能看到一份完整快照；
 * - 该模块本身不直接决定 UI 表现，只输出“当前电源状态是否可信”这一事实。
 */

static const char *TAG = "POWER_SERVICE";
static const TickType_t k_failure_log_throttle_ticks = pdMS_TO_TICKS(5000); /* 失败日志节流窗口，单位为 tick。 */
static const uint16_t k_voltage_jitter_threshold_mv = 20; /* 电压抖动忽略阈值，单位为毫伏。 */

static TaskHandle_t s_task_handle = NULL; /* 后台采样任务句柄，仅启动阶段写入。 */
static board_power_state_t s_state_buffers[2] = {
    {.battery_percent = UINT8_MAX},
    {.battery_percent = UINT8_MAX},
};
static power_state_changed_cb_t s_callback = NULL; /* 状态变化回调，在采样任务上下文调用。 */
static bool s_initialized = false;
static bool s_started = false;
static uint32_t s_failure_count = 0;         /* 连续刷新失败次数，用于退避。 */
static TickType_t s_last_failure_log_tick = 0; /* 最近一次打印失败日志的 tick。 */
static uint8_t s_active_state_index = 0;     /* 当前发布缓冲索引，只能为 0 或 1。 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED; /* 保护双缓冲切换与回调指针。 */

/**
 * @brief 构造一份“尚未采样成功”的默认电源状态。
 *
 * 该状态用于初始化阶段和彻底失败路径，目的是让上层能明确区分“未知”与“真实的 0% 电量”。
 *
 * @return 默认电源状态快照。
 */
static board_power_state_t power_service_make_unsampled_state(void)
{
    board_power_state_t state = {0};
    /* UINT8_MAX 作为“未知电量”的哨兵值，避免把 0% 误判成有效结果。 */
    state.battery_percent = UINT8_MAX;
    return state;
}

/**
 * @brief 判断缓存状态是否代表一次历史上的成功采样。
 * @param[in] cached_state 待判断的缓存快照。
 * @return true 表示当前缓存可被视为“上次已知状态”。
 */
static bool power_service_cached_state_has_history(
    const board_power_state_t *cached_state)
{
    return cached_state != NULL && cached_state->available;
}

/**
 * @brief 用当前板级缓存初始化双缓冲状态。
 *
 * 启动服务前先把两份缓冲同步成同一份快照，可以避免首次读取时因为活动缓冲未初始化而读到不一致内容。
 *
 * @return 无返回值。
 */
static void power_service_copy_cached_state(void)
{
    const board_power_state_t *cached_state = board_power_get_cached_state();
    board_power_state_t snapshot = power_service_make_unsampled_state();

    if (cached_state != NULL)
    {
        snapshot = *cached_state;
    }

    /* 初始化时同步两份缓冲，让首次读取就能拿到一致视图。 */
    taskENTER_CRITICAL(&s_lock);
    s_state_buffers[0] = snapshot;
    s_state_buffers[1] = snapshot;
    s_active_state_index = 0;
    taskEXIT_CRITICAL(&s_lock);
}

/**
 * @brief 判断两次电压采样差值是否值得视为状态变化。
 * @param[in] lhs 第一个电压值，单位为毫伏。
 * @param[in] rhs 第二个电压值，单位为毫伏。
 * @return true 表示差值超过抖动阈值。
 */
static bool power_service_mv_changed(uint16_t lhs, uint16_t rhs)
{
    uint16_t delta = lhs > rhs ? (lhs - rhs) : (rhs - lhs);
    return delta >= k_voltage_jitter_threshold_mv;
}

/**
 * @brief 比较两份电源状态是否存在“有效变化”。
 *
 * 该比较会忽略小于抖动阈值的电压变化，避免 UI 因轻微 ADC 波动频繁刷新。
 *
 * @param[in] lhs 第一份状态。
 * @param[in] rhs 第二份状态。
 * @return true 表示两份状态对上层而言等价。
 */
static bool power_service_state_equal(const board_power_state_t *lhs,
                                      const board_power_state_t *rhs)
{
    return lhs->available == rhs->available &&
           lhs->battery_data_valid == rhs->battery_data_valid &&
           lhs->snapshot_stale == rhs->snapshot_stale &&
           lhs->charging == rhs->charging &&
           lhs->discharging == rhs->discharging &&
           lhs->external_power_present == rhs->external_power_present &&
           lhs->battery_present == rhs->battery_present &&
           !power_service_mv_changed(lhs->battery_mv, rhs->battery_mv) &&
           !power_service_mv_changed(lhs->system_mv, rhs->system_mv) &&
           lhs->battery_percent == rhs->battery_percent;
}

/* 统一格式化电源状态变化日志，便于追踪快照质量与电量字段可信度。 */
static void power_service_log_state_change(const board_power_state_t *state)
{
    if (state == NULL)
    {
        ESP_LOGI(TAG, "power state changed: unavailable");
        return;
    }

    if (state->battery_data_valid)
    {
        ESP_LOGI(TAG,
                 "power state changed: available=%d stale=%d ext=%d bat=%d chg=%d dchg=%d vbat=%umV vsys=%umV soc=%u%%",
                 state->available, state->snapshot_stale,
                 state->external_power_present, state->battery_present,
                 state->charging, state->discharging, state->battery_mv,
                 state->system_mv, state->battery_percent);
        return;
    }

    ESP_LOGI(TAG,
             "power state changed: available=%d stale=%d ext=%d bat=%d chg=%d dchg=%d vbat=%umV vsys=%umV soc=unknown",
             state->available, state->snapshot_stale,
             state->external_power_present, state->battery_present,
             state->charging, state->discharging, state->battery_mv,
             state->system_mv);
}

/**
 * @brief 将新状态发布到双缓冲，并判断是否需要通知观察者。
 *
 * 先写入非活动缓冲，再原子切换活动索引，这样读取方即使和采样任务并发执行，也只会看到完整快照。
 *
 * @param[in] next_state 候选的新状态。
 * @param[out] state_changed 输出是否发生有效变化。
 * @param[out] callback 输出当前应调用的回调；无变化时返回 NULL。
 * @param[out] published_state 输出最终发布的状态指针。
 * @return 无返回值。
 */
static void power_service_store_state(const board_power_state_t *next_state,
                                      bool *state_changed,
                                      power_state_changed_cb_t *callback,
                                      const board_power_state_t **published_state)
{
    taskENTER_CRITICAL(&s_lock);
    uint8_t active_index = s_active_state_index;
    uint8_t inactive_index = active_index ^ 1U;

    *state_changed =
        !power_service_state_equal(&s_state_buffers[active_index], next_state);
    if (*state_changed)
    {
        /* 先写入非活动缓冲，再切换活动索引，避免读到半更新状态。 */
        s_state_buffers[inactive_index] = *next_state;
        s_active_state_index = inactive_index;
        *published_state = &s_state_buffers[inactive_index];
        *callback = s_callback;
    }
    else
    {
        *published_state = &s_state_buffers[active_index];
        *callback = NULL;
    }
    taskEXIT_CRITICAL(&s_lock);
}

/**
 * @brief 受节流控制地打印采样失败日志。
 * @param[in] ret 本次失败的错误码。
 * @return 无返回值。
 */
static void power_service_log_failure(esp_err_t ret)
{
    TickType_t now = xTaskGetTickCount();
    bool should_log = (s_last_failure_log_tick == 0) ||
                      ((now - s_last_failure_log_tick) >=
                       k_failure_log_throttle_ticks);

    if (should_log)
    {
        s_last_failure_log_tick = now;
        ESP_LOGW(TAG, "power refresh failed: %s", esp_err_to_name(ret));
    }
}

/**
 * @brief 为采样失败路径构造降级状态。
 *
 * 若历史上存在成功采样，则优先复用最近一次快照并标记为 stale；
 * 若从未成功采样，则回退到统一的 unsampled 状态。
 *
 * @param[out] state 输出降级后的状态。
 * @return 无返回值。
 */
static void power_service_prepare_failure_state(board_power_state_t *state)
{
    const board_power_state_t *cached_state = board_power_get_cached_state();

    if (cached_state == NULL)
    {
        *state = power_service_make_unsampled_state();
        return;
    }

    /* 采样失败时尽量复用最近快照，让 UI 还能显示“上次已知状态”。 */
    *state = *cached_state;
    if (power_service_cached_state_has_history(cached_state))
    {
        state->snapshot_stale = true;
        state->available = false;
    }
}

/**
 * @brief 电源服务后台轮询任务。
 * @param pv_parameter 未使用，保留任务签名。
 *
 * 该任务周期刷新板级电源快照：
 * - 成功时按需发布状态变化；
 * - 失败时退避重试，并尽量保留历史快照。
 */
static void power_service_task(void *pv_parameter)
{
    (void)pv_parameter;

    while (1)
    {
        board_power_state_t next_state = {0};
        TickType_t delay_ticks = pdMS_TO_TICKS(1000);
        esp_err_t ret = board_power_refresh(&next_state);

        if (ret == ESP_OK)
        {
            /* 采样成功时恢复正常轮询节奏，并按需通知观察者。 */
            bool state_changed = false;
            power_state_changed_cb_t callback = NULL;
            const board_power_state_t *published_state = NULL;

            s_failure_count = 0;
            s_last_failure_log_tick = 0;
            power_service_store_state(&next_state, &state_changed, &callback,
                                      &published_state);
            if (state_changed)
            {
                power_service_log_state_change(published_state);
                (void)power_policy_notify(POWER_POLICY_NOTIFY_POWER_STATE);
                if (callback != NULL)
                {
                    callback(published_state);
                }
            }
        }
        else
        {
            /* 连续失败后逐步退避，减轻总线异常或设备缺失时的系统负担。 */
            power_service_prepare_failure_state(&next_state);

            bool state_changed = false;
            power_state_changed_cb_t callback = NULL;
            const board_power_state_t *published_state = NULL;
            power_service_store_state(&next_state, &state_changed, &callback,
                                      &published_state);
            if (state_changed)
            {
                power_service_log_state_change(published_state);
                (void)power_policy_notify(POWER_POLICY_NOTIFY_POWER_STATE);
                if (callback != NULL)
                {
                    callback(published_state);
                }
            }

            ++s_failure_count;
            delay_ticks =
                s_failure_count >= 3 ? pdMS_TO_TICKS(5000)
                                     : pdMS_TO_TICKS(2000);
            power_service_log_failure(ret);
        }

        vTaskDelay(delay_ticks);
    }
}

/**
 * @brief 初始化电源服务内部状态。
 * @return ESP_OK 表示初始化成功或已初始化。
 */
esp_err_t power_service_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    power_service_copy_cached_state();
    s_failure_count = 0;
    s_last_failure_log_tick = 0;
    s_started = false;
    s_initialized = true;
    return ESP_OK;
}

/**
 * @brief 启动电源服务后台任务。
 * @return ESP_OK 表示任务已启动或之前已启动。
 */
esp_err_t power_service_start(void)
{
    esp_err_t ret = power_service_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    if (s_started)
    {
        return ESP_OK;
    }

    /* 电源状态变化不要求极低延迟，普通优先级后台任务即可。 */
    /* 该任务每秒通过 I2C 读取 AXP2101。IDF 新 I2C 驱动的 ISR 在 Flash
       cache-disabled 窗口仍会访问调用任务栈里的事务描述符；若栈在 PSRAM，
       OTA 写 Flash 时会触发 cache error。因此此 I2C owner 必须使用 internal
       RAM 栈，不能按普通后台任务迁到 PSRAM。 */
    BaseType_t ok =
        xTaskCreateWithCaps(power_service_task, "power_service", 3072, NULL, 5,
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

/**
 * @brief 注册电源状态变化回调。
 * @param cb 回调函数，可为 NULL 表示取消监听。
 */
void power_service_register_callback(power_state_changed_cb_t cb)
{
    taskENTER_CRITICAL(&s_lock);
    s_callback = cb;
    taskEXIT_CRITICAL(&s_lock);
}

/**
 * @brief 复制当前发布中的电源状态快照。
 *
 * 该接口只复制双缓冲中的活动快照，不执行 I2C/PMIC 访问，也不推进采样状态机。
 *
 * @param[out] out_state 输出快照，不能为空。
 * @return `ESP_OK` 表示成功复制。
 */
esp_err_t power_service_get_snapshot(board_power_state_t *out_state)
{
    if (out_state == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_lock);
    *out_state = s_state_buffers[s_active_state_index];
    taskEXIT_CRITICAL(&s_lock);

    return ESP_OK;
}

/**
 * @brief 获取当前发布中的电源状态快照视图。
 * @return 服务层拥有的只读快照指针；新增代码优先使用 `power_service_get_snapshot(out)`。
 */
const board_power_state_t *power_service_get_state(void)
{
    const board_power_state_t *state = NULL;

    taskENTER_CRITICAL(&s_lock);
    state = &s_state_buffers[s_active_state_index];
    taskEXIT_CRITICAL(&s_lock);
    return state;
}
