#include "services/power/sleep_coordinator.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

static const char *TAG = "sleep_coordinator";
static const TickType_t k_dry_run_period_ticks = pdMS_TO_TICKS(2000);

typedef struct
{
    bool initialized;
    bool started;
    sleep_coordinator_mode_t mode;
    power_policy_sleep_permission_t sleep_permission;
    uint32_t sleep_blockers;
    uint32_t sleep_interval_hint_ms;
    uint32_t budget_version;
    uint32_t dry_run_count;
    TaskHandle_t task_handle;
    portMUX_TYPE lock;
} sleep_coordinator_state_t;

static sleep_coordinator_state_t s_sleep_coordinator = {
    .initialized = false,
    .started = false,
    .mode = SLEEP_COORDINATOR_MODE_DRY_RUN,
    .sleep_permission = POWER_POLICY_SLEEP_NONE,
    .sleep_blockers = POWER_POLICY_SLEEP_BLOCKER_NONE,
    .sleep_interval_hint_ms = 0,
    .budget_version = 0,
    .dry_run_count = 0,
    .task_handle = NULL,
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static const char *sleep_coordinator_mode_text(sleep_coordinator_mode_t mode)
{
    switch (mode)
    {
    case SLEEP_COORDINATOR_MODE_DISABLED:
        return "DISABLED";
    case SLEEP_COORDINATOR_MODE_DRY_RUN:
    default:
        return "DRY_RUN";
    }
}

/**
 * @brief 执行一次 dry-run 采样。
 *
 * 本函数只读取 `power_policy` 发布的预算快照，不读取 UI、网络、音频或 PMIC
 * 内部状态，也不调用任何 ESP sleep API。
 */
static void sleep_coordinator_record_dry_run(void)
{
    sleep_coordinator_mode_t mode = SLEEP_COORDINATOR_MODE_DISABLED;

    taskENTER_CRITICAL(&s_sleep_coordinator.lock);
    mode = s_sleep_coordinator.mode;
    taskEXIT_CRITICAL(&s_sleep_coordinator.lock);

    if (mode != SLEEP_COORDINATOR_MODE_DRY_RUN)
    {
        return;
    }

    const power_policy_budget_t budget = power_policy_get_budget();
    uint32_t dry_run_count = 0;

    taskENTER_CRITICAL(&s_sleep_coordinator.lock);
    s_sleep_coordinator.sleep_permission = budget.sleep_permission;
    s_sleep_coordinator.sleep_blockers = budget.sleep_blockers;
    s_sleep_coordinator.sleep_interval_hint_ms = budget.sleep_interval_hint_ms;
    s_sleep_coordinator.budget_version = budget.budget_version;
    s_sleep_coordinator.dry_run_count++;
    dry_run_count = s_sleep_coordinator.dry_run_count;
    taskEXIT_CRITICAL(&s_sleep_coordinator.lock);

    char blocker_text[160];
    power_policy_format_sleep_blockers(budget.sleep_blockers, blocker_text,
                                       sizeof(blocker_text));
    ESP_LOGD(TAG,
             "dry_run: count=%u budget_version=%u permission=%s blockers=%s interval_ms=%u",
             (unsigned)dry_run_count,
             (unsigned)budget.budget_version,
             power_policy_sleep_permission_text(budget.sleep_permission),
             blocker_text,
             (unsigned)budget.sleep_interval_hint_ms);
}

static void sleep_coordinator_task(void *arg)
{
    (void)arg;

    while (1)
    {
        sleep_coordinator_record_dry_run();
        vTaskDelay(k_dry_run_period_ticks);
    }
}

esp_err_t sleep_coordinator_init(void)
{
    if (s_sleep_coordinator.initialized)
    {
        return ESP_OK;
    }

    esp_err_t ret = power_policy_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    taskENTER_CRITICAL(&s_sleep_coordinator.lock);
    s_sleep_coordinator.initialized = true;
    taskEXIT_CRITICAL(&s_sleep_coordinator.lock);
    return ESP_OK;
}

esp_err_t sleep_coordinator_start(void)
{
    esp_err_t ret = sleep_coordinator_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    taskENTER_CRITICAL(&s_sleep_coordinator.lock);
    const bool already_started = s_sleep_coordinator.started;
    taskEXIT_CRITICAL(&s_sleep_coordinator.lock);
    if (already_started)
    {
        return ESP_OK;
    }

    const BaseType_t ok =
        xTaskCreate(sleep_coordinator_task, "sleep_coord", 3072, NULL, 3,
                    &s_sleep_coordinator.task_handle);
    if (ok != pdPASS)
    {
        s_sleep_coordinator.task_handle = NULL;
        return ESP_FAIL;
    }

    taskENTER_CRITICAL(&s_sleep_coordinator.lock);
    s_sleep_coordinator.started = true;
    taskEXIT_CRITICAL(&s_sleep_coordinator.lock);

    ESP_LOGI(TAG, "started: mode=%s",
             sleep_coordinator_mode_text(SLEEP_COORDINATOR_MODE_DRY_RUN));
    return ESP_OK;
}

esp_err_t sleep_coordinator_set_mode(sleep_coordinator_mode_t mode)
{
    esp_err_t ret = sleep_coordinator_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    if (mode != SLEEP_COORDINATOR_MODE_DISABLED &&
        mode != SLEEP_COORDINATOR_MODE_DRY_RUN)
    {
        ESP_LOGW(TAG, "unsupported mode: %s",
                 sleep_coordinator_mode_text(mode));
        return ESP_ERR_NOT_SUPPORTED;
    }

    taskENTER_CRITICAL(&s_sleep_coordinator.lock);
    s_sleep_coordinator.mode = mode;
    taskEXIT_CRITICAL(&s_sleep_coordinator.lock);

    ESP_LOGI(TAG, "mode set: %s", sleep_coordinator_mode_text(mode));
    return ESP_OK;
}

sleep_coordinator_snapshot_t sleep_coordinator_get_snapshot(void)
{
    sleep_coordinator_snapshot_t snapshot = {0};

    taskENTER_CRITICAL(&s_sleep_coordinator.lock);
    snapshot.initialized = s_sleep_coordinator.initialized;
    snapshot.started = s_sleep_coordinator.started;
    snapshot.mode = s_sleep_coordinator.mode;
    snapshot.sleep_permission = s_sleep_coordinator.sleep_permission;
    snapshot.sleep_blockers = s_sleep_coordinator.sleep_blockers;
    snapshot.sleep_interval_hint_ms =
        s_sleep_coordinator.sleep_interval_hint_ms;
    snapshot.budget_version = s_sleep_coordinator.budget_version;
    snapshot.dry_run_count = s_sleep_coordinator.dry_run_count;
    taskEXIT_CRITICAL(&s_sleep_coordinator.lock);

    return snapshot;
}
