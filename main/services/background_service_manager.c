#include "services/background_service_manager.h"

#include "audio_codec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "services/foreground_runtime_gate.h"
#include "services/power_policy.h"
#include "services/safety_monitor_session.h"
#include "services/startup_readiness.h"

static const char *TAG = "background_mgr";
static const TickType_t k_policy_poll_ticks = pdMS_TO_TICKS(1000);

typedef enum
{
    BACKGROUND_SERVICE_MANAGER_NOTIFY_NONE = 0,
    BACKGROUND_SERVICE_MANAGER_NOTIFY_USER_SWITCH = 1u << 0,
    BACKGROUND_SERVICE_MANAGER_NOTIFY_FOREGROUND_AUDIO = 1u << 1,
    BACKGROUND_SERVICE_MANAGER_NOTIFY_POWER_BUDGET = 1u << 2,
    BACKGROUND_SERVICE_MANAGER_NOTIFY_STARTUP = 1u << 3,
    BACKGROUND_SERVICE_MANAGER_NOTIFY_PERIODIC = 1u << 4,
    BACKGROUND_SERVICE_MANAGER_NOTIFY_FOREGROUND_RUNTIME = 1u << 5,
} background_service_manager_notify_reason_t;

typedef struct
{
    bool initialized;                  /**< 是否已初始化依赖模块。 */
    bool started;                      /**< 后台任务是否已启动。 */
    bool danger_enabled_by_user;       /**< 用户是否允许危险识别后台运行。 */
    bool danger_allowed_by_policy;     /**< 最近一次策略是否允许危险识别。 */
    bool danger_should_run;            /**< 最近一次合成出的 Safety Monitor 目标态。 */
    bool danger_runtime_running;       /**< 最近一次命令确认的运行状态。 */
    background_service_manager_danger_block_reason_t danger_block_reason; /**< 目标态未运行的主原因。 */
    bool foreground_audio_active;      /**< 前台录音/语音是否正在占用麦克风。 */
    bool danger_blocked_by_foreground_audio; /**< 当前麦克风 owner 是否阻塞危险识别。 */
    bool danger_blocked_by_foreground_runtime; /**< 强前台重任务是否要求 ESP-DL 让路。 */
    power_policy_state_t policy_state; /**< 最近一次策略状态。 */
    uint32_t policy_flags;             /**< 最近一次预算 flag。 */
    esp_err_t last_error;              /**< 最近一次 session 启动、停止或恢复错误码。 */
    TaskHandle_t task_handle;          /**< 后台策略轮询任务。 */
    portMUX_TYPE lock;                 /**< 保护管理器快照。 */
} background_service_manager_state_t;

static background_service_manager_state_t s_manager = {
    .initialized = false,
    .started = false,
    .danger_enabled_by_user = false,
    .danger_allowed_by_policy = true,
    .danger_should_run = false,
    .danger_runtime_running = false,
    .danger_block_reason =
        BACKGROUND_SERVICE_MANAGER_DANGER_BLOCK_MANAGER_NOT_READY,
    .foreground_audio_active = false,
    .danger_blocked_by_foreground_audio = false,
    .danger_blocked_by_foreground_runtime = false,
    .policy_state = POWER_POLICY_STATE_ACTIVE,
    .policy_flags = POWER_POLICY_FLAG_NONE,
    .last_error = ESP_OK,
    .task_handle = NULL,
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static const char *background_service_manager_notify_reason_text(
    uint32_t reasons)
{
    if ((reasons & BACKGROUND_SERVICE_MANAGER_NOTIFY_USER_SWITCH) != 0U)
    {
        return "user_switch";
    }
    if ((reasons & BACKGROUND_SERVICE_MANAGER_NOTIFY_FOREGROUND_AUDIO) != 0U)
    {
        return "foreground_audio";
    }
    if ((reasons & BACKGROUND_SERVICE_MANAGER_NOTIFY_FOREGROUND_RUNTIME) != 0U)
    {
        return "foreground_runtime";
    }
    if ((reasons & BACKGROUND_SERVICE_MANAGER_NOTIFY_POWER_BUDGET) != 0U)
    {
        return "power_budget";
    }
    if ((reasons & BACKGROUND_SERVICE_MANAGER_NOTIFY_STARTUP) != 0U)
    {
        return "startup";
    }
    if ((reasons & BACKGROUND_SERVICE_MANAGER_NOTIFY_PERIODIC) != 0U)
    {
        return "periodic";
    }
    return "manual";
}

static const char *background_service_manager_block_reason_text(
    background_service_manager_danger_block_reason_t reason)
{
    switch (reason)
    {
    case BACKGROUND_SERVICE_MANAGER_DANGER_BLOCK_NONE:
        return "none";
    case BACKGROUND_SERVICE_MANAGER_DANGER_BLOCK_MANAGER_NOT_READY:
        return "manager_not_ready";
    case BACKGROUND_SERVICE_MANAGER_DANGER_BLOCK_USER_DISABLED:
        return "user_disabled";
    case BACKGROUND_SERVICE_MANAGER_DANGER_BLOCK_POLICY:
        return "policy";
    case BACKGROUND_SERVICE_MANAGER_DANGER_BLOCK_FOREGROUND_AUDIO:
        return "foreground_audio";
    case BACKGROUND_SERVICE_MANAGER_DANGER_BLOCK_FOREGROUND_RUNTIME:
        return "foreground_runtime";
    default:
        return "unknown";
    }
}

static background_service_manager_danger_block_reason_t
background_service_manager_resolve_danger_block_reason(
    bool manager_started,
    bool user_enabled,
    bool policy_allowed,
    bool foreground_audio_blocked,
    bool foreground_runtime_blocked)
{
    if (!manager_started)
    {
        return BACKGROUND_SERVICE_MANAGER_DANGER_BLOCK_MANAGER_NOT_READY;
    }
    if (!user_enabled)
    {
        return BACKGROUND_SERVICE_MANAGER_DANGER_BLOCK_USER_DISABLED;
    }
    if (foreground_audio_blocked)
    {
        return BACKGROUND_SERVICE_MANAGER_DANGER_BLOCK_FOREGROUND_AUDIO;
    }
    if (foreground_runtime_blocked)
    {
        return BACKGROUND_SERVICE_MANAGER_DANGER_BLOCK_FOREGROUND_RUNTIME;
    }
    if (!policy_allowed)
    {
        return BACKGROUND_SERVICE_MANAGER_DANGER_BLOCK_POLICY;
    }
    return BACKGROUND_SERVICE_MANAGER_DANGER_BLOCK_NONE;
}

static bool background_service_manager_input_owner_blocks_danger(
    audio_codec_owner_t owner)
{
    switch (owner)
    {
    case AUDIO_CODEC_OWNER_SYSTEM:
    case AUDIO_CODEC_OWNER_ESPDL_INFERENCE:
        return false;
    case AUDIO_CODEC_OWNER_TRAFFIC_INFERENCE:
    case AUDIO_CODEC_OWNER_AUDIO_PLAYER:
    case AUDIO_CODEC_OWNER_AUDIO_RECORDER:
    case AUDIO_CODEC_OWNER_OFFICIAL_CHAT:
    case AUDIO_CODEC_OWNER_HERMES:
    default:
        return true;
    }
}

static bool background_service_manager_audio_blocks_danger(
    bool explicit_foreground_audio, const char **owner_text)
{
    if (owner_text != NULL)
    {
        *owner_text = NULL;
    }

    if (explicit_foreground_audio)
    {
        if (owner_text != NULL)
        {
            *owner_text = "foreground_audio";
        }
        return true;
    }

    audio_codec_session_snapshot_t audio_snapshot = {0};
    esp_err_t ret = audio_codec_get_session_snapshot(&audio_snapshot);
    if (ret != ESP_OK || !audio_snapshot.input_active)
    {
        return false;
    }

    if (!background_service_manager_input_owner_blocks_danger(
            audio_snapshot.input_owner))
    {
        return false;
    }

    if (owner_text != NULL)
    {
        *owner_text = audio_codec_owner_to_text(audio_snapshot.input_owner);
    }
    return true;
}

/**
 * @brief 唤醒后台管理器 task 重新合成 Safety Monitor 目标态。
 *
 * notify 只是轻量事件信号，最终事实仍由 manager task 读取自身 snapshot、
 * `power_policy_get_budget()` 和 `audio_codec_get_session_snapshot()` 得到。
 */
static esp_err_t background_service_manager_notify(uint32_t reasons)
{
    if (reasons == BACKGROUND_SERVICE_MANAGER_NOTIFY_NONE)
    {
        reasons = BACKGROUND_SERVICE_MANAGER_NOTIFY_PERIODIC;
    }

    TaskHandle_t task_handle = NULL;
    bool started = false;

    taskENTER_CRITICAL(&s_manager.lock);
    task_handle = s_manager.task_handle;
    started = s_manager.started;
    taskEXIT_CRITICAL(&s_manager.lock);

    if (!started || task_handle == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xTaskNotify(task_handle, reasons, eSetBits);
    return ESP_OK;
}

/**
 * @brief 根据最新策略和用户开关同步危险识别后台运行状态。
 *
 * 该函数是第一阶段的核心闭环：页面不再拥有 start/stop 生命周期，
 * 后台管理器根据用户开关和资源预算决定是否运行危险识别。
 */
static esp_err_t background_service_manager_apply_policy(const char *reason)
{
    const power_policy_budget_t budget = power_policy_get_budget();
    const safety_monitor_session_snapshot_t before_session =
        safety_monitor_session_get_snapshot();

    bool user_enabled = true;
    bool explicit_foreground_audio = false;
    bool previous_allowed = true;
    bool previous_should_run = false;
    background_service_manager_danger_block_reason_t previous_block_reason =
        BACKGROUND_SERVICE_MANAGER_DANGER_BLOCK_MANAGER_NOT_READY;
    bool previous_running = false;
    bool previous_audio_blocked = false;
    bool previous_runtime_blocked = false;

    taskENTER_CRITICAL(&s_manager.lock);
    user_enabled = s_manager.danger_enabled_by_user;
    explicit_foreground_audio = s_manager.foreground_audio_active;
    previous_allowed = s_manager.danger_allowed_by_policy;
    previous_should_run = s_manager.danger_should_run;
    previous_block_reason = s_manager.danger_block_reason;
    previous_running = s_manager.danger_runtime_running;
    previous_audio_blocked = s_manager.danger_blocked_by_foreground_audio;
    previous_runtime_blocked =
        s_manager.danger_blocked_by_foreground_runtime;
    s_manager.policy_state = budget.state;
    s_manager.policy_flags = budget.flags;
    s_manager.danger_allowed_by_policy = budget.danger_detection_allowed;
    s_manager.danger_runtime_running = before_session.runtime_running;
    s_manager.last_error = before_session.last_error;
    taskEXIT_CRITICAL(&s_manager.lock);

    const char *blocking_audio_owner = NULL;
    const bool foreground_audio_blocked =
        background_service_manager_audio_blocks_danger(
            explicit_foreground_audio, &blocking_audio_owner);
    const bool foreground_runtime_blocked =
        foreground_runtime_gate_is_active();
    const foreground_runtime_owner_t foreground_owner =
        foreground_runtime_gate_current_owner();
    const background_service_manager_danger_block_reason_t block_reason =
        background_service_manager_resolve_danger_block_reason(
            true,
            user_enabled,
            budget.danger_detection_allowed,
            foreground_audio_blocked,
            foreground_runtime_blocked);
    const bool should_run =
        block_reason == BACKGROUND_SERVICE_MANAGER_DANGER_BLOCK_NONE;

    taskENTER_CRITICAL(&s_manager.lock);
    s_manager.danger_blocked_by_foreground_audio = foreground_audio_blocked;
    s_manager.danger_blocked_by_foreground_runtime =
        foreground_runtime_blocked;
    s_manager.danger_should_run = should_run;
    s_manager.danger_block_reason = block_reason;
    taskEXIT_CRITICAL(&s_manager.lock);

    if (previous_allowed != budget.danger_detection_allowed)
    {
        ESP_LOGI(TAG,
                 "background_allowed_change: danger=%d policy=%s reason=%s",
                 budget.danger_detection_allowed,
                 power_policy_state_text(budget.state),
                 reason != NULL ? reason : "unknown");
    }

    if (previous_audio_blocked != foreground_audio_blocked)
    {
        ESP_LOGI(TAG,
                 "resource_blocked_change: resource=mic danger=%d owner=%s reason=%s",
                 foreground_audio_blocked ? 1 : 0,
                 blocking_audio_owner != NULL ? blocking_audio_owner : "none",
                 reason != NULL ? reason : "unknown");
    }

    if (previous_runtime_blocked != foreground_runtime_blocked)
    {
        ESP_LOGI(TAG,
                 "resource_blocked_change: resource=foreground_runtime danger=%d owner=%s reason=%s",
                 foreground_runtime_blocked ? 1 : 0,
                 foreground_runtime_gate_owner_text(foreground_owner),
                 reason != NULL ? reason : "unknown");
    }

    if (previous_should_run != should_run ||
        previous_block_reason != block_reason)
    {
        ESP_LOGI(TAG,
                 "background_target_change: danger_should_run=%d block_reason=%s reason=%s",
                 should_run ? 1 : 0,
                 background_service_manager_block_reason_text(block_reason),
                 reason != NULL ? reason : "unknown");
    }

    esp_err_t ret = safety_monitor_session_apply(should_run, reason);
    const safety_monitor_session_snapshot_t after_session =
        safety_monitor_session_get_snapshot();

    taskENTER_CRITICAL(&s_manager.lock);
    s_manager.last_error = after_session.last_error;
    s_manager.danger_runtime_running = after_session.runtime_running;
    taskEXIT_CRITICAL(&s_manager.lock);

    if (previous_running != after_session.runtime_running)
    {
        ESP_LOGI(TAG, "background danger runtime observed: running=%d",
                 after_session.runtime_running);
    }

    return ret;
}

static void background_service_manager_task(void *arg)
{
    (void)arg;

    /*
     * 后台 Safety Monitor session 可能加载模型并占用麦克风、PSRAM 和
     * internal/DMA 相关资源；必须等 UI 首帧真实完成后再进入策略循环，
     * 避免用固定延时猜测 Display Foundation / UI First Frame 边界。
     */
    ESP_LOGI(TAG, "background_gate_wait: ui_first_frame_ready");
    (void)startup_readiness_wait_ui_first_frame(portMAX_DELAY);
    ESP_LOGI(TAG, "background_gate_ready: ui_first_frame_ready");

    (void)background_service_manager_apply_policy("startup");

    while (1)
    {
        uint32_t notify_reasons = BACKGROUND_SERVICE_MANAGER_NOTIFY_NONE;
        const BaseType_t notified =
            xTaskNotifyWait(0, UINT32_MAX, &notify_reasons,
                            k_policy_poll_ticks);
        if (notified != pdTRUE ||
            notify_reasons == BACKGROUND_SERVICE_MANAGER_NOTIFY_NONE)
        {
            notify_reasons = BACKGROUND_SERVICE_MANAGER_NOTIFY_PERIODIC;
        }
        (void)background_service_manager_apply_policy(
            background_service_manager_notify_reason_text(notify_reasons));
    }
}

esp_err_t background_service_manager_init(void)
{
    if (s_manager.initialized)
    {
        return ESP_OK;
    }

    esp_err_t ret = power_policy_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = startup_readiness_init();
    if (ret != ESP_OK)
    {
        taskENTER_CRITICAL(&s_manager.lock);
        s_manager.last_error = ret;
        taskEXIT_CRITICAL(&s_manager.lock);
        return ret;
    }

    ret = safety_monitor_session_init();
    if (ret != ESP_OK)
    {
        taskENTER_CRITICAL(&s_manager.lock);
        s_manager.last_error = ret;
        taskEXIT_CRITICAL(&s_manager.lock);
        return ret;
    }

    taskENTER_CRITICAL(&s_manager.lock);
    s_manager.initialized = true;
    s_manager.last_error = ESP_OK;
    taskEXIT_CRITICAL(&s_manager.lock);
    return ESP_OK;
}

esp_err_t background_service_manager_start(void)
{
    esp_err_t ret = background_service_manager_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    taskENTER_CRITICAL(&s_manager.lock);
    const bool already_started = s_manager.started;
    taskEXIT_CRITICAL(&s_manager.lock);
    if (already_started)
    {
        return ESP_OK;
    }

    /* 栈迁 PSRAM：省 internal RAM，该任务只做调度/协调，不直接操作 flash/NVS。 */
    BaseType_t ok = xTaskCreateWithCaps(background_service_manager_task,
                                        "background_mgr",
                                        4096,
                                        NULL,
                                        4,
                                        &s_manager.task_handle,
                                        MALLOC_CAP_SPIRAM);
    if (ok != pdPASS)
    {
        s_manager.task_handle = NULL;
        /*
         * 管理器任务没起来时不能留下无人托管的后台监听，
         * 否则后续用户开关和策略预算都无法继续生效。
         */
        (void)safety_monitor_session_apply(false, "manager_start_failed");
        taskENTER_CRITICAL(&s_manager.lock);
        s_manager.danger_should_run = false;
        s_manager.danger_block_reason =
            BACKGROUND_SERVICE_MANAGER_DANGER_BLOCK_MANAGER_NOT_READY;
        s_manager.danger_runtime_running = false;
        taskEXIT_CRITICAL(&s_manager.lock);
        return ESP_FAIL;
    }

    taskENTER_CRITICAL(&s_manager.lock);
    s_manager.started = true;
    taskEXIT_CRITICAL(&s_manager.lock);

    ESP_LOGI(TAG, "background service manager started");
    return ESP_OK;
}

esp_err_t background_service_manager_set_danger_detection_enabled(bool enabled)
{
    esp_err_t ret = background_service_manager_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    taskENTER_CRITICAL(&s_manager.lock);
    const bool changed = s_manager.danger_enabled_by_user != enabled;
    s_manager.danger_enabled_by_user = enabled;
    taskEXIT_CRITICAL(&s_manager.lock);

    if (changed)
    {
        ESP_LOGI(TAG, "safety_monitor_session: enabled_by_user=%d", enabled);
    }

    taskENTER_CRITICAL(&s_manager.lock);
    const bool manager_started = s_manager.started;
    taskEXIT_CRITICAL(&s_manager.lock);
    if (!manager_started)
    {
        /*
         * 页面可以更新用户开关，但不能在管理器任务未启动时拉起无人托管的
         * 后台监听。app_main 会负责启动真正的周期 owner。
         */
        taskENTER_CRITICAL(&s_manager.lock);
        s_manager.danger_should_run = false;
        s_manager.danger_block_reason =
            background_service_manager_resolve_danger_block_reason(
                false,
                s_manager.danger_enabled_by_user,
                s_manager.danger_allowed_by_policy,
                s_manager.danger_blocked_by_foreground_audio,
                s_manager.danger_blocked_by_foreground_runtime);
        taskEXIT_CRITICAL(&s_manager.lock);
        return ESP_ERR_INVALID_STATE;
    }

    return background_service_manager_notify(
        BACKGROUND_SERVICE_MANAGER_NOTIFY_USER_SWITCH);
}

esp_err_t background_service_manager_set_foreground_audio_active(
    bool active, const char *reason)
{
    esp_err_t ret = background_service_manager_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    taskENTER_CRITICAL(&s_manager.lock);
    const bool changed = s_manager.foreground_audio_active != active;
    s_manager.foreground_audio_active = active;
    const bool manager_started = s_manager.started;
    taskEXIT_CRITICAL(&s_manager.lock);

    if (changed)
    {
        ESP_LOGI(TAG, "foreground_audio_active: active=%d reason=%s",
                 active, reason != NULL ? reason : "unknown");
    }

    if (!manager_started)
    {
        return ESP_OK;
    }

    return background_service_manager_notify(
        BACKGROUND_SERVICE_MANAGER_NOTIFY_FOREGROUND_AUDIO);
}

esp_err_t background_service_manager_notify_policy_changed(void)
{
    const esp_err_t ret = background_service_manager_notify(
        BACKGROUND_SERVICE_MANAGER_NOTIFY_POWER_BUDGET);
    return ret == ESP_ERR_INVALID_STATE ? ESP_OK : ret;
}

esp_err_t background_service_manager_notify_foreground_runtime_changed(void)
{
    const esp_err_t ret = background_service_manager_notify(
        BACKGROUND_SERVICE_MANAGER_NOTIFY_FOREGROUND_RUNTIME);
    return ret == ESP_ERR_INVALID_STATE ? ESP_OK : ret;
}

background_service_manager_snapshot_t
background_service_manager_get_snapshot(void)
{
    background_service_manager_snapshot_t snapshot;

    taskENTER_CRITICAL(&s_manager.lock);
    snapshot.started = s_manager.started;
    snapshot.danger_enabled_by_user = s_manager.danger_enabled_by_user;
    snapshot.danger_allowed_by_policy = s_manager.danger_allowed_by_policy;
    snapshot.danger_should_run = s_manager.danger_should_run;
    snapshot.danger_runtime_running = s_manager.danger_runtime_running;
    snapshot.danger_block_reason = s_manager.danger_block_reason;
    snapshot.danger_blocked_by_foreground_audio =
        s_manager.danger_blocked_by_foreground_audio;
    snapshot.danger_blocked_by_foreground_runtime =
        s_manager.danger_blocked_by_foreground_runtime;
    snapshot.policy_state = s_manager.policy_state;
    snapshot.policy_flags = s_manager.policy_flags;
    snapshot.last_error = s_manager.last_error;
    taskEXIT_CRITICAL(&s_manager.lock);

    return snapshot;
}
