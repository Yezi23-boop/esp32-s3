#include "services/power/power_policy.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "app/board_power.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "services/power/power_service.h"
#include "services/runtime/runtime_coordinator.h"
#include "ui_refresh_policy.h"

static const char *TAG = "power_policy";
static const uint8_t k_low_battery_warn_percent = 20U; /* 低电量预警起点，单位为百分比。 */
static const uint32_t k_standby_sleep_interval_hint_ms = 8000U; /* dry-run 建议 Light Sleep 间隔。 */
static const int64_t k_light_allowed_idle_time_ms = 5LL * 60LL * 1000LL; /* 屏幕无交互满 5 分钟后才允许发布 LIGHT_ALLOWED。 */
static const TickType_t k_policy_task_period_ticks = pdMS_TO_TICKS(1000); /* 周期兜底重算，避免 notify 丢失后预算长时间陈旧。 */

/* 省电参与者静态表：容量固定，不提供运行期卸载。
 * 写入只发生在 policy task 启动前（service 初始化阶段），task 启动后只读，
 * 因此遍历无需持锁；provider 回调在锁外执行，避免回调重入和锁反转。 */
#define POWER_POLICY_MAX_PARTICIPANTS (8U)
typedef struct
{
    power_policy_participant_config_t config;
} power_policy_participant_t;
static power_policy_participant_t s_participants[POWER_POLICY_MAX_PARTICIPANTS];
static size_t s_participant_count = 0;

/* 前向声明：注册写入发生在 policy task 启动前，定义在文件末尾的注册区。 */
static esp_err_t power_policy_add_participant(
    const power_policy_participant_config_t *config);

static bool s_initialized = false;
static bool s_started = false;
static bool s_maintenance_window_active = false;
static TaskHandle_t s_task_handle = NULL;
static uint32_t s_budget_version = 0;
static power_policy_budget_t s_last_budget = {
    .state = POWER_POLICY_STATE_ACTIVE,
    .standby_reason = POWER_POLICY_STANDBY_REASON_NONE,
    .display_budget = POWER_POLICY_DISPLAY_FULL,
    .ui_budget = POWER_POLICY_UI_HIGH_REFRESH,
    .network_budget = POWER_POLICY_NETWORK_FULL,
    .background_budget = POWER_POLICY_BACKGROUND_FULL,
    .cpu_budget = POWER_POLICY_CPU_PERFORMANCE,
    .power_poll_budget = POWER_POLICY_POWER_POLL_NORMAL,
    .sleep_permission = POWER_POLICY_SLEEP_NONE,
    .sleep_blockers = POWER_POLICY_SLEEP_BLOCKER_NONE,
    .flags = POWER_POLICY_FLAG_NONE,
    .sleep_interval_hint_ms = 0,
    .danger_detection_allowed = true,
    .network_sync_allowed = true,
    .maintenance_allowed = false,
    .ui_high_refresh_allowed = true,
    .haptic_alert_allowed = true,
    .low_battery_warn = false,
    .external_power_present = false,
    .battery_data_valid = false,
    .battery_percent = UINT8_MAX,
    .battery_mv = 0,
    .budget_version = 0,
    .last_notify_reasons = POWER_POLICY_NOTIFY_NONE,
};
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

const char *power_policy_state_text(power_policy_state_t state)
{
    switch (state)
    {
    case POWER_POLICY_STATE_STANDBY:
        return "STANDBY";
    case POWER_POLICY_STATE_ACTIVE:
    default:
        return "ACTIVE";
    }
}

const char *power_policy_sleep_permission_text(
    power_policy_sleep_permission_t permission)
{
    switch (permission)
    {
    case POWER_POLICY_SLEEP_LIGHT_ALLOWED:
        return "LIGHT_ALLOWED";
    case POWER_POLICY_SLEEP_DEEP_ALLOWED:
        return "DEEP_ALLOWED";
    case POWER_POLICY_SLEEP_NONE:
    default:
        return "NONE";
    }
}

static void power_policy_append_blocker(char *buffer, size_t buffer_size,
                                        const char *name)
{
    if (buffer == NULL || buffer_size == 0U || name == NULL)
    {
        return;
    }

    size_t used = strlen(buffer);
    if (used >= buffer_size - 1U)
    {
        return;
    }

    if (buffer[0] != '\0')
    {
        strncat(buffer, "|", buffer_size - used - 1U);
        used = strlen(buffer);
        if (used >= buffer_size - 1U)
        {
            return;
        }
    }
    strncat(buffer, name, buffer_size - used - 1U);
}

void power_policy_format_sleep_blockers(uint32_t blockers, char *buffer,
                                        size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0U)
    {
        return;
    }

    buffer[0] = '\0';
    if (blockers == POWER_POLICY_SLEEP_BLOCKER_NONE)
    {
        snprintf(buffer, buffer_size, "none");
        return;
    }

    if ((blockers & POWER_POLICY_SLEEP_BLOCKER_UI_FORCE_ACTIVE) != 0U)
    {
        power_policy_append_blocker(buffer, buffer_size, "UI_FORCE_ACTIVE");
    }
    if ((blockers & POWER_POLICY_SLEEP_BLOCKER_AUDIO_ACTIVE) != 0U)
    {
        power_policy_append_blocker(buffer, buffer_size, "AUDIO_ACTIVE");
    }
    if ((blockers & POWER_POLICY_SLEEP_BLOCKER_NETWORK_CRITICAL) != 0U)
    {
        power_policy_append_blocker(buffer, buffer_size, "NETWORK_CRITICAL");
    }
    if ((blockers & POWER_POLICY_SLEEP_BLOCKER_BACKGROUND_CRITICAL) != 0U)
    {
        power_policy_append_blocker(buffer, buffer_size, "BACKGROUND_CRITICAL");
    }
    if ((blockers & POWER_POLICY_SLEEP_BLOCKER_OTA_ACTIVE) != 0U)
    {
        power_policy_append_blocker(buffer, buffer_size, "OTA_ACTIVE");
    }
    if ((blockers & POWER_POLICY_SLEEP_BLOCKER_PROVISIONING_ACTIVE) != 0U)
    {
        power_policy_append_blocker(buffer, buffer_size, "PROVISIONING_ACTIVE");
    }
    if ((blockers & POWER_POLICY_SLEEP_BLOCKER_ALERT_ACTIVE) != 0U)
    {
        power_policy_append_blocker(buffer, buffer_size, "ALERT_ACTIVE");
    }
    if ((blockers & POWER_POLICY_SLEEP_BLOCKER_DEBUG_LOCK) != 0U)
    {
        power_policy_append_blocker(buffer, buffer_size, "DEBUG_LOCK");
    }
}

/**
 * @brief 按 UI 活跃度快照收窄普通运行态预算。
 *
 * `ui_refresh_policy` 仍然是亮度和 LVGL 延时 owner；这里仅消费它已经
 * 缓存的只读事实，用于把普通 `ACTIVE` 预算收窄为运行态 `STANDBY`。
 *
 * @param[in,out] budget 当前待发布的资源预算。
 */
static void power_policy_apply_ui_activity_budget(power_policy_budget_t *budget)
{
    if (budget == NULL)
    {
        return;
    }

    ui_refresh_policy_activity_snapshot_t activity_snapshot;
    if (!ui_refresh_policy_get_activity_snapshot(&activity_snapshot) ||
        !activity_snapshot.initialized)
    {
        return;
    }

    if (activity_snapshot.activity_state ==
        UI_REFRESH_POLICY_ACTIVITY_STANDBY)
    {
        if (budget->state == POWER_POLICY_STATE_ACTIVE)
        {
            budget->state = POWER_POLICY_STATE_STANDBY;
            budget->standby_reason = POWER_POLICY_STANDBY_REASON_AUTO_IDLE;
        }
        budget->display_budget = POWER_POLICY_DISPLAY_OFF;
        budget->ui_budget = POWER_POLICY_UI_LOW_REFRESH;
        budget->network_budget = POWER_POLICY_NETWORK_SYNC_PAUSED;
        budget->background_budget = POWER_POLICY_BACKGROUND_PAUSE_OPTIONAL;
        budget->cpu_budget = POWER_POLICY_CPU_LOW;
        budget->power_poll_budget = POWER_POLICY_POWER_POLL_SLOW;
        if (activity_snapshot.idle_time_ms >= k_light_allowed_idle_time_ms)
        {
            budget->sleep_interval_hint_ms = k_standby_sleep_interval_hint_ms;
        }
        budget->network_sync_allowed = false;
        budget->maintenance_allowed = false;
        budget->ui_high_refresh_allowed = false;
        return;
    }

    if (activity_snapshot.force_active)
    {
        budget->sleep_blockers |= POWER_POLICY_SLEEP_BLOCKER_UI_FORCE_ACTIVE;
    }
}

/**
 * @brief 根据电源快照和只读 UI 活跃度事实计算第一阶段资源预算。
 *
 * 第一阶段只把已可观测的充电、低电量、维护窗口和 UI STANDBY 信号纳入预算。
 * `power_policy` 只发布预算，不直接操作屏幕、Wi-Fi 或后台任务。
 */
static power_policy_budget_t power_policy_build_budget(
    const board_power_state_t *power_state, uint32_t notify_reasons)
{
    power_policy_budget_t budget = {
        .state = POWER_POLICY_STATE_ACTIVE,
        .standby_reason = POWER_POLICY_STANDBY_REASON_NONE,
        .display_budget = POWER_POLICY_DISPLAY_FULL,
        .ui_budget = POWER_POLICY_UI_HIGH_REFRESH,
        .network_budget = POWER_POLICY_NETWORK_FULL,
        .background_budget = POWER_POLICY_BACKGROUND_FULL,
        .cpu_budget = POWER_POLICY_CPU_PERFORMANCE,
        .power_poll_budget = POWER_POLICY_POWER_POLL_NORMAL,
        .sleep_permission = POWER_POLICY_SLEEP_NONE,
        .sleep_blockers = POWER_POLICY_SLEEP_BLOCKER_NONE,
        .flags = POWER_POLICY_FLAG_NONE,
        .sleep_interval_hint_ms = 0,
        .danger_detection_allowed = true,
        .network_sync_allowed = true,
        .maintenance_allowed = false,
        .ui_high_refresh_allowed = true,
        .haptic_alert_allowed = true,
        .low_battery_warn = false,
        .external_power_present = false,
        .battery_data_valid = false,
        .battery_percent = UINT8_MAX,
        .battery_mv = 0,
        .budget_version = 0,
        .last_notify_reasons = notify_reasons,
    };

    taskENTER_CRITICAL(&s_lock);
    const bool maintenance_window_active = s_maintenance_window_active;
    taskEXIT_CRITICAL(&s_lock);

    if (power_state == NULL || !power_state->available)
    {
        if (maintenance_window_active)
        {
            budget.flags |= POWER_POLICY_FLAG_MAINTENANCE;
            budget.danger_detection_allowed = false;
            budget.network_sync_allowed = false;
            budget.maintenance_allowed = true;
            budget.ui_high_refresh_allowed = false;
            budget.network_budget = POWER_POLICY_NETWORK_SYNC_PAUSED;
            budget.background_budget = POWER_POLICY_BACKGROUND_PAUSE_OPTIONAL;
            budget.sleep_blockers |= POWER_POLICY_SLEEP_BLOCKER_BACKGROUND_CRITICAL;
        }
        return budget;
    }

    budget.external_power_present =
        power_state->external_power_present || power_state->charging;
    if (budget.external_power_present)
    {
        budget.flags |= POWER_POLICY_FLAG_EXTERNAL_POWER;
    }
    if (power_state->charging)
    {
        budget.flags |= POWER_POLICY_FLAG_CHARGING;
    }
    budget.battery_data_valid = power_state->battery_data_valid;
    budget.battery_percent = power_state->battery_data_valid
                                 ? power_state->battery_percent
                                 : UINT8_MAX;
    budget.battery_mv = power_state->battery_mv;
    budget.low_battery_warn =
        power_state->battery_data_valid &&
        !budget.external_power_present &&
        power_state->battery_percent <= k_low_battery_warn_percent;
    if (budget.low_battery_warn)
    {
        budget.flags |= POWER_POLICY_FLAG_LOW_BATTERY_WARN;
    }

    if (maintenance_window_active)
    {
        budget.flags |= POWER_POLICY_FLAG_MAINTENANCE;
        budget.danger_detection_allowed = false;
        budget.network_sync_allowed = false;
        budget.maintenance_allowed = true;
        budget.ui_high_refresh_allowed = false;
        budget.network_budget = POWER_POLICY_NETWORK_SYNC_PAUSED;
        budget.background_budget = POWER_POLICY_BACKGROUND_PAUSE_OPTIONAL;
        budget.sleep_blockers |= POWER_POLICY_SLEEP_BLOCKER_BACKGROUND_CRITICAL;
    }

    power_policy_apply_ui_activity_budget(&budget);

    if (budget.external_power_present && budget.state == POWER_POLICY_STATE_ACTIVE)
    {
        budget.maintenance_allowed = true;
    }

    if (budget.state == POWER_POLICY_STATE_STANDBY &&
        budget.sleep_blockers == POWER_POLICY_SLEEP_BLOCKER_NONE &&
        budget.sleep_interval_hint_ms > 0U)
    {
        budget.sleep_permission = POWER_POLICY_SLEEP_LIGHT_ALLOWED;
    }

    return budget;
}

static bool power_policy_budget_equal(const power_policy_budget_t *lhs,
                                      const power_policy_budget_t *rhs)
{
    return lhs->state == rhs->state &&
           lhs->standby_reason == rhs->standby_reason &&
           lhs->display_budget == rhs->display_budget &&
           lhs->ui_budget == rhs->ui_budget &&
           lhs->network_budget == rhs->network_budget &&
           lhs->background_budget == rhs->background_budget &&
           lhs->cpu_budget == rhs->cpu_budget &&
           lhs->power_poll_budget == rhs->power_poll_budget &&
           lhs->sleep_permission == rhs->sleep_permission &&
           lhs->sleep_blockers == rhs->sleep_blockers &&
           lhs->flags == rhs->flags &&
           lhs->sleep_interval_hint_ms == rhs->sleep_interval_hint_ms &&
           lhs->danger_detection_allowed == rhs->danger_detection_allowed &&
           lhs->network_sync_allowed == rhs->network_sync_allowed &&
           lhs->maintenance_allowed == rhs->maintenance_allowed &&
           lhs->ui_high_refresh_allowed == rhs->ui_high_refresh_allowed &&
           lhs->haptic_alert_allowed == rhs->haptic_alert_allowed &&
           lhs->low_battery_warn == rhs->low_battery_warn &&
           lhs->external_power_present == rhs->external_power_present &&
           lhs->battery_data_valid == rhs->battery_data_valid &&
           lhs->battery_percent == rhs->battery_percent &&
           lhs->battery_mv == rhs->battery_mv;
}

static power_policy_budget_t power_policy_load_budget(void)
{
    power_policy_budget_t budget;

    taskENTER_CRITICAL(&s_lock);
    budget = s_last_budget;
    taskEXIT_CRITICAL(&s_lock);
    return budget;
}

/**
 * @brief 发布预算变化日志。
 *
 * 只在预算发生有效变化时打印，避免后台服务周期读取造成日志噪声。
 */
static void power_policy_store_budget(const power_policy_budget_t *budget)
{
    bool changed = false;
    power_policy_budget_t stored_budget = *budget;

    taskENTER_CRITICAL(&s_lock);
    stored_budget.budget_version = s_budget_version;
    changed = !power_policy_budget_equal(&s_last_budget, &stored_budget);
    if (changed)
    {
        s_budget_version++;
        stored_budget.budget_version = s_budget_version;
        s_last_budget = stored_budget;
    }
    taskEXIT_CRITICAL(&s_lock);

    if (changed)
    {
        /* 预算变化只广播给注册了 on_budget_changed 的参与者；Safety 等 consumer
         * 通过同一张表收到唤醒，真实动作仍由其 owner task 执行。 */
        (void)power_policy_budget_changed_notify();

        char blocker_text[160];
        power_policy_format_sleep_blockers(budget->sleep_blockers,
                                           blocker_text,
                                           sizeof(blocker_text));
        ESP_LOGI(TAG,
                 "power_budget_change: version=%u reasons=0x%08" PRIx32 " state=%s standby_reason=%d display=%d ui=%d network=%d background=%d cpu=%d poll=%d sleep=%s blockers=%s interval_ms=%u flags=0x%08" PRIx32 " danger=%d net_sync=%d maintenance=%d ui_high_refresh=%d low_battery=%d external_power=%d bat_valid=%d soc=%u vbat=%umV",
                 (unsigned)stored_budget.budget_version,
                 stored_budget.last_notify_reasons,
                 power_policy_state_text(stored_budget.state),
                 stored_budget.standby_reason,
                 stored_budget.display_budget,
                 stored_budget.ui_budget,
                 stored_budget.network_budget,
                 stored_budget.background_budget,
                 stored_budget.cpu_budget,
                 stored_budget.power_poll_budget,
                 power_policy_sleep_permission_text(stored_budget.sleep_permission),
                 blocker_text,
                 (unsigned)stored_budget.sleep_interval_hint_ms,
                 stored_budget.flags,
                 stored_budget.danger_detection_allowed,
                 stored_budget.network_sync_allowed,
                 stored_budget.maintenance_allowed,
                 stored_budget.ui_high_refresh_allowed,
                 stored_budget.low_battery_warn,
                 stored_budget.external_power_present,
                 stored_budget.battery_data_valid,
                 stored_budget.battery_data_valid ? stored_budget.battery_percent : 0U,
                 stored_budget.battery_mv);
    }
}

/**
 * @brief runtime_coordinator 内置事实 provider。
 *
 * OTA / provisioning 是跨 owner 强前台交接，真实状态由 coordinator 单一快照
 * 持有；这里只把 current/target/provisional owner 映射为 sleep blocker，
 * 避免同一事实被 coordinator participant 和 power provider 重复上报。
 * 该回调只读无锁快照，不做任何交接动作。
 */
static esp_err_t power_policy_coordinator_facts(
    power_policy_provider_facts_t *facts, void *context)
{
    (void)context;
    if (facts == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(facts, 0, sizeof(*facts));
    const runtime_coordinator_snapshot_t snapshot =
        runtime_coordinator_get_snapshot();
    if (!snapshot.started)
    {
        return ESP_OK;
    }

    if (snapshot.current_owner == RUNTIME_COORDINATOR_PARTICIPANT_OTA ||
        snapshot.target_owner == RUNTIME_COORDINATOR_PARTICIPANT_OTA ||
        snapshot.provisional_owner == RUNTIME_COORDINATOR_PARTICIPANT_OTA)
    {
        facts->sleep_blockers |= POWER_POLICY_SLEEP_BLOCKER_OTA_ACTIVE;
    }
    if (snapshot.current_owner ==
            RUNTIME_COORDINATOR_PARTICIPANT_NETWORK_PROVISIONING ||
        snapshot.target_owner ==
            RUNTIME_COORDINATOR_PARTICIPANT_NETWORK_PROVISIONING ||
        snapshot.provisional_owner ==
            RUNTIME_COORDINATOR_PARTICIPANT_NETWORK_PROVISIONING)
    {
        facts->sleep_blockers |= POWER_POLICY_SLEEP_BLOCKER_PROVISIONING_ACTIVE;
    }
    return ESP_OK;
}

/**
 * @brief 遍历省电参与者表，把各 provider 快照聚合进预算。
 *
 * 在 policy task（或同步兜底）上下文中执行，且不在 policy 锁内；任一 provider
 * 读取失败都返回 false，由调用方按 fail-closed 规则收紧睡眠许可，同时保留
 * 已成功聚合的 blocker，避免错误来源悄悄清掉其他 blocker。
 *
 * 注意：blocker 位图随每次预算重建，失败 provider 的上一轮 blocker 不会被保留；
 * 睡眠闸门由 fail-closed `sleep_permission=NONE` 兜底，不依赖 blocker 位图。
 *
 * @param[in,out] budget 当前待发布的资源预算。
 * @return true 表示所有 provider 读取成功；false 表示至少一个 provider 失败。
 */
static bool power_policy_apply_provider_facts(power_policy_budget_t *budget)
{
    if (budget == NULL)
    {
        return true;
    }

    bool all_ok = true;
    for (size_t i = 0; i < s_participant_count; i++)
    {
        const power_policy_participant_t *participant = &s_participants[i];
        if (participant->config.get_facts == NULL)
        {
            continue;
        }

        power_policy_provider_facts_t facts = {0};
        const esp_err_t ret = participant->config.get_facts(
            &facts, participant->config.context);
        if (ret != ESP_OK)
        {
            /* 记录来源和错误；失败不能静默清除 blocker。 */
            ESP_LOGW(TAG, "provider facts failed: id=%d name=%s err=%s",
                     participant->config.id,
                     participant->config.name != NULL
                         ? participant->config.name
                         : "unknown",
                     esp_err_to_name(ret));
            all_ok = false;
            continue;
        }

        if (facts.sleep_blockers != POWER_POLICY_SLEEP_BLOCKER_NONE)
        {
            budget->sleep_blockers |= facts.sleep_blockers;
        }
        if (facts.last_error != ESP_OK)
        {
            ESP_LOGW(TAG, "provider reported error: id=%d err=%s",
                     participant->config.id,
                     esp_err_to_name(facts.last_error));
        }
    }
    return all_ok;
}

static void power_policy_recalculate(uint32_t notify_reasons)
{
    board_power_state_t power_snapshot = {0};
    const board_power_state_t *power_state = NULL;

    if (power_service_get_snapshot(&power_snapshot) == ESP_OK)
    {
        power_state = &power_snapshot;
    }

    power_policy_budget_t budget =
        power_policy_build_budget(power_state, notify_reasons);
    const bool provider_ok = power_policy_apply_provider_facts(&budget);
    if (!provider_ok)
    {
        /* fail-closed：任一 provider 事实读取失败时不允许发布 sleep 许可，
         * 避免未知状态被静默放行；STANDBY 的 UI/Wi-Fi 基础节能不受影响。 */
        budget.sleep_permission = POWER_POLICY_SLEEP_NONE;
    }
    /* provider blocker 聚合之后统一收紧：任何 blocker（音频、后台关键、OTA、
     * provisioning、UI force active 等）存在时都不允许发布 LIGHT_ALLOWED，
     * 与架构卡的“sleep_blockers=none”门槛保持一致。 */
    if (budget.sleep_blockers != POWER_POLICY_SLEEP_BLOCKER_NONE &&
        budget.sleep_permission == POWER_POLICY_SLEEP_LIGHT_ALLOWED)
    {
        budget.sleep_permission = POWER_POLICY_SLEEP_NONE;
    }
    power_policy_store_budget(&budget);
}

static void power_policy_task(void *arg)
{
    (void)arg;

    power_policy_recalculate(POWER_POLICY_NOTIFY_MANUAL);

    while (1)
    {
        uint32_t notify_reasons = POWER_POLICY_NOTIFY_NONE;
        const BaseType_t notified =
            xTaskNotifyWait(0, UINT32_MAX, &notify_reasons,
                            k_policy_task_period_ticks);
        if (notified != pdTRUE || notify_reasons == POWER_POLICY_NOTIFY_NONE)
        {
            notify_reasons = POWER_POLICY_NOTIFY_PERIODIC;
        }
        power_policy_recalculate(notify_reasons);
    }
}

esp_err_t power_policy_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    /* 内置 coordinator 事实 provider：OTA/provisioning blocker 由单一交接快照
     * 派生，不依赖任何业务模块反向注册。 */
    const power_policy_participant_config_t coordinator = {
        .id = POWER_POLICY_PROVIDER_RUNTIME_COORDINATOR,
        .name = "runtime_coordinator",
        .capabilities = POWER_POLICY_PARTICIPANT_FACTS_ONLY,
        .get_facts = power_policy_coordinator_facts,
        .on_budget_changed = NULL,
        .context = NULL,
    };
    const esp_err_t ret = power_policy_add_participant(&coordinator);
    if (ret != ESP_OK)
    {
        return ret;
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t power_policy_start(void)
{
    esp_err_t ret = power_policy_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    if (s_started)
    {
        return ESP_OK;
    }

    /* 栈迁 PSRAM：省 internal RAM，该任务不直接操作 flash/NVS/sleep 入口。 */
    const BaseType_t ok =
        xTaskCreateWithCaps(power_policy_task, "power_policy", 4096, NULL, 4,
                            &s_task_handle, MALLOC_CAP_SPIRAM);
    if (ok != pdPASS)
    {
        s_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* 与读取方一致持锁发布启动态：task 创建后立即运行并遍历参与者表，注册
     * 检查 s_started 的 check-then-act 必须能看到同一个原子值。 */
    taskENTER_CRITICAL(&s_lock);
    s_started = true;
    taskEXIT_CRITICAL(&s_lock);
    power_policy_budget_t budget = power_policy_get_budget();
    ESP_LOGI(TAG, "policy task started: state=%s version=%u",
             power_policy_state_text(budget.state),
             (unsigned)budget.budget_version);
    return ESP_OK;
}

esp_err_t power_policy_set_maintenance_window(bool active, const char *reason)
{
    esp_err_t ret = power_policy_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    bool changed = false;
    taskENTER_CRITICAL(&s_lock);
    changed = s_maintenance_window_active != active;
    s_maintenance_window_active = active;
    taskEXIT_CRITICAL(&s_lock);

    if (changed)
    {
        ESP_LOGI(TAG, "maintenance_window_%s: reason=%s",
                 active ? "enter" : "exit",
                 reason != NULL ? reason : "unknown");
    }

    (void)power_policy_notify(POWER_POLICY_NOTIFY_MAINTENANCE);
    return ESP_OK;
}

esp_err_t power_policy_notify(uint32_t reason)
{
    if (reason == POWER_POLICY_NOTIFY_NONE)
    {
        reason = POWER_POLICY_NOTIFY_MANUAL;
    }

    TaskHandle_t task_handle = NULL;
    bool started = false;

    taskENTER_CRITICAL(&s_lock);
    task_handle = s_task_handle;
    started = s_started;
    taskEXIT_CRITICAL(&s_lock);

    if (started && task_handle != NULL)
    {
        xTaskNotify(task_handle, reason, eSetBits);
        return ESP_OK;
    }

    power_policy_recalculate(reason);
    return ESP_OK;
}

power_policy_budget_t power_policy_get_budget(void)
{
    bool started = false;

    taskENTER_CRITICAL(&s_lock);
    started = s_started;
    taskEXIT_CRITICAL(&s_lock);

    if (!started)
    {
        power_policy_recalculate(POWER_POLICY_NOTIFY_MANUAL);
    }
    return power_policy_load_budget();
}

/**
 * @brief 向静态参与者表写入一个登记项。
 *
 * 只在 service 初始化阶段（policy task 启动前）调用；不提供运行期卸载，
 * 因此表写入完成后对 task 而言是只读的。
 *
 * @param[in] config 参与者配置。
 * @return `ESP_OK` 表示写入或幂等成功；`ESP_ERR_INVALID_ARG` 表示参数非法或
 *         同 id 冲突配置；`ESP_ERR_NO_MEM` 表示表已满。
 */
static esp_err_t power_policy_add_participant(
    const power_policy_participant_config_t *config)
{
    if (config == NULL || config->id <= POWER_POLICY_PROVIDER_NONE ||
        config->id >= POWER_POLICY_PROVIDER_COUNT)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->get_facts == NULL && config->on_budget_changed == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_lock);
    const bool started = s_started;
    taskEXIT_CRITICAL(&s_lock);
    if (started)
    {
        ESP_LOGE(TAG, "register participant after policy task started: id=%d",
                 config->id);
        return ESP_ERR_INVALID_STATE;
    }

    for (size_t i = 0; i < s_participant_count; i++)
    {
        if (s_participants[i].config.id != config->id)
        {
            continue;
        }
        /* 幂等：同 id 且回调/上下文一致视为重复注册成功；冲突配置明确报错。 */
        if (s_participants[i].config.get_facts != config->get_facts ||
            s_participants[i].config.on_budget_changed !=
                config->on_budget_changed ||
            s_participants[i].config.context != config->context)
        {
            ESP_LOGE(TAG, "register participant conflict: id=%d", config->id);
            return ESP_ERR_INVALID_ARG;
        }
        ESP_LOGI(TAG, "register participant idempotent: id=%d name=%s",
                 config->id, config->name != NULL ? config->name : "unknown");
        return ESP_OK;
    }

    if (s_participant_count >= POWER_POLICY_MAX_PARTICIPANTS)
    {
        ESP_LOGE(TAG, "register participant table full: count=%u",
                 (unsigned)s_participant_count);
        return ESP_ERR_NO_MEM;
    }

    s_participants[s_participant_count].config = *config;
    s_participant_count++;
    ESP_LOGI(TAG, "register participant: id=%d name=%s facts=%d consumer=%d",
             config->id, config->name != NULL ? config->name : "unknown",
             config->get_facts != NULL ? 1 : 0,
             config->on_budget_changed != NULL ? 1 : 0);
    return ESP_OK;
}

esp_err_t power_policy_register_participant(
    const power_policy_participant_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return power_policy_add_participant(config);
}

uint32_t power_policy_budget_version(void)
{
    uint32_t version = 0;
    taskENTER_CRITICAL(&s_lock);
    version = s_budget_version;
    taskEXIT_CRITICAL(&s_lock);
    return version;
}

esp_err_t power_policy_budget_changed_notify(void)
{
    const uint32_t version = power_policy_budget_version();
    for (size_t i = 0; i < s_participant_count; i++)
    {
        const power_policy_participant_t *participant = &s_participants[i];
        if (participant->config.on_budget_changed == NULL)
        {
            continue;
        }
        /* 回调不在 policy 锁内执行：只允许唤醒 owner task，不允许在此上下文
         * 执行业务动作，避免回调重入和锁反转。 */
        if (participant->config.on_budget_changed(
                version, participant->config.context) != ESP_OK)
        {
            ESP_LOGW(TAG, "budget changed notify failed: id=%d",
                     participant->config.id);
        }
    }
    return ESP_OK;
}
