#include "network_service.h"

#include <lwip/netdb.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "network_manager.h"
#include "services/power/power_policy.h"
#include "services/music/music_service.h"
#include "services/official_chat_service.h"
#include "services/runtime/runtime_coordinator.h"
#include "services/time/system_time_service.h"
#include "wifi_control.h"

/*
 * `network_service` 当前是旧接口兼容层：
 * - 联网控制统一桥接给 `network_manager`
 * - 本文件只额外补“关键云端依赖是否真正可用”的探测
 * - 因此不再直接操作 `wifi_provision` 或底层 transport 生命周期
 */

static const char *TAG = "NETWORK_SERVICE";
static const char *kProbeHosts[] = {"api.tenclass.net", "mqtt.xiaozhi.me"}; // 关键云端依赖域名列表。
static const uint32_t kProbeAttemptMax = 15;                                // 单个域名的最大探测次数。
static const uint32_t kServicePollPeriodMs = 1000U;                         // 服务层轮询 network_manager 的周期。
static const uint32_t kBleRetryDelayMs = 800U;                              // BLE internal heap 不足后的唯一一次重试间隔。
static const uint32_t kBleTransitionStackBytes = 4096U;                     // BLE/NVS transition 的 internal task stack 字节数。
static const UBaseType_t kBleTransitionTaskPriority = 4U;                   // 低于 network owner，确保 handle 发布后 worker 才开始运行。

typedef enum
{
    NETWORK_SERVICE_BLE_OPERATION_NONE = 0,
    NETWORK_SERVICE_BLE_OPERATION_TOGGLE,
    NETWORK_SERVICE_BLE_OPERATION_START_BLE_PROVISIONING,
    NETWORK_SERVICE_BLE_OPERATION_START_SOFTAP_PROVISIONING,
    NETWORK_SERVICE_BLE_OPERATION_STOP_PROVISIONING,
} network_service_ble_operation_t;

static TaskHandle_t s_network_task_handle = NULL; // 网络服务后台任务句柄。
static TaskHandle_t s_ble_transition_task_handle = NULL; // 按需创建的 BLE/NVS transition worker。
static char s_network_ip[16] = {0}; // 当前缓存的 IPv4 字符串，仅由服务层更新。
static bool s_runtime_power_save_applied = false; // 最近一次按预算下发的 Wi-Fi 省电状态。
static bool s_runtime_power_save_known = false;   // false 表示尚未下发过 Wi-Fi 省电配置。
static bool s_ble_desired_enabled = false; // 仅在 snapshot lock 下更新的 BLE 目标态。
static bool s_ble_runtime_target_enabled = false; // coordinator 未阻塞时才跟随用户偏好。
static bool s_ble_presence_blocked = false; // 强前台占用期间暂停普通 BLE presence。
static bool s_ble_applied_enabled = false; // 最近一次 manager 成功应用的 BLE 状态。
static bool s_ble_transition_enabled = false; // 当前 worker 捕获的目标态。
static bool s_ble_transition_completed = false; // worker 已发布结果，等待 owner 回收 task。
static uint32_t s_ble_presence_quiesce_generation = 0U;
static uint32_t s_provision_request_generation = 0U;
static uint32_t s_provision_quiesce_generation = 0U;
static network_service_ble_operation_t s_provision_pending_operation =
    NETWORK_SERVICE_BLE_OPERATION_NONE;
static network_service_ble_operation_t s_ble_operation_request =
    NETWORK_SERVICE_BLE_OPERATION_NONE; // 最新待执行 BLE/provisioning 操作。
static network_service_ble_operation_t s_ble_operation_active =
    NETWORK_SERVICE_BLE_OPERATION_NONE; // 当前 worker 捕获的操作。
static uint32_t s_ble_request_generation = 0U; // 每次目标态提交递增。
static uint32_t s_ble_applied_generation = 0U; // 最近一次已完成尝试的目标代次。
static uint32_t s_ble_transition_generation = 0U; // 当前 worker 捕获的代次。
static uint32_t s_ble_provision_request_generation = 0U; // 每次配网入口请求递增。
static uint32_t s_ble_provision_applied_generation = 0U; // 最近一次已处理配网请求代次。
static uint32_t s_ble_provision_transition_generation = 0U; // worker 捕获的配网请求代次。
static uint32_t s_ble_provision_intent_generation = 0U; // UI 可见的配网意图代次。
static bool s_provision_stop_requested = false;
static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static network_service_wifi_status_t s_wifi_status_snapshot = {
    .service_state = NETWORK_SERVICE_STATE_OFFLINE,
    .wifi_connected = false,
    .has_credentials = false,
    .user_disconnect_latched = false,
    .provisioning_active = false,
    .ble_active = false,
    .ap_active = false,
    .default_transport = NETWORK_SERVICE_PROVISION_TRANSPORT_BLE,
    .ip = {0},
};
static network_service_snapshot_t s_snapshot = {
    .state = NETWORK_SERVICE_STATE_OFFLINE,
    .wifi_connected = false,
    .service_ready = false,
    .probe_active = false,
    .probe_paused_by_budget = false,
    .power_save_applied = false,
    .ble_desired_enabled = false,
    .ble_applied_enabled = false,
    .ble_runtime_ready = false,
    .ble_transition_pending = false,
    .ble_generation = 0U,
    .ble_last_error = ESP_OK,
    .provisioning_transition_pending = false,
    .provisioning_generation = 0U,
    .provisioning_last_error = ESP_OK,
    .last_error = ESP_OK,
    .last_probe_result = ESP_ERR_INVALID_STATE,
};

static const char *network_service_state_name(network_service_state_t state);
static void network_service_set_state(network_service_state_t state,
                                      const char *reason);
static void network_service_clear_cached_ip(void);
static bool network_service_has_saved_credentials(void);
static void network_service_sync_cached_ip(
    const network_manager_status_t *status);
static void network_service_publish_wifi_status(
    const network_manager_status_t *status);
static network_service_state_t network_service_map_manager_state(
    const network_manager_status_t *status);
static network_service_provision_transport_t
network_service_map_transport_from_manager(
    network_manager_provisioning_transport_t transport);
static network_manager_provisioning_transport_t
network_service_map_transport_to_manager(
    network_service_provision_transport_t transport);
static bool resolve_hostname_once(const char *hostname);
static esp_err_t probe_network_services_ready(void);
static void network_service_apply_power_budget(void);
static void network_service_reconcile_ble(void);
static void network_service_stop_completed_ble_provisioning_if_connected(
    const network_manager_status_t *status);
static void network_service_cancel_completed_ble_provisioning_request(
    const network_manager_status_t *status);
static void network_service_ble_transition_task(void *pv_parameter);
static void network_service_task(void *pv_parameter);
static esp_err_t network_service_request_provisioning(
    network_service_ble_operation_t operation);

/**
 * @brief 判断 worker 捕获的 BLE 请求是否仍为最新目标态。
 *
 * generation 保护快速 ON/OFF 反转，避免 800ms 重试把已过期的 enable 再次执行。
 */
static bool network_service_ble_request_is_current(uint32_t generation,
                                                   bool enabled)
{
    bool current = false;

    taskENTER_CRITICAL(&s_snapshot_lock);
    current = s_ble_request_generation == generation &&
              s_ble_runtime_target_enabled == enabled;
    taskEXIT_CRITICAL(&s_snapshot_lock);
    return current;
}

/**
 * @brief 在 provisioning 非正常或自动完成后恢复用户要求的普通 BLE presence。
 *
 * 该函数只能由 network service owner 调用；恢复动作放在同一 gate 生命周期内，
 * 避免 UI 或状态 getter 直接重新创建 BLE runtime。
 */
static esp_err_t network_service_restore_ble_presence_if_desired(void)
{
    if (!s_ble_desired_enabled || s_ble_presence_blocked ||
        network_manager_is_ble_enabled())
    {
        return ESP_OK;
    }
    return network_manager_set_ble_enabled(true);
}

/**
 * @brief 发布一次 BLE transition 的完成结果。
 */
static void network_service_finish_ble_transition(uint32_t generation,
                                                  bool enabled,
                                                  bool applied_enabled,
                                                  esp_err_t result)
{
    bool current = false;
    TaskHandle_t notify_handle = NULL;

    taskENTER_CRITICAL(&s_snapshot_lock);
    current = s_ble_request_generation == generation &&
              s_ble_runtime_target_enabled == enabled;
    s_ble_applied_generation = generation;
    s_ble_applied_enabled = applied_enabled;
    s_snapshot.ble_applied_enabled = applied_enabled;
    if (current)
    {
        s_snapshot.ble_transition_pending = false;
        s_snapshot.ble_runtime_ready = (result == ESP_OK);
        s_snapshot.ble_last_error = result;
    }
    taskEXIT_CRITICAL(&s_snapshot_lock);

    if (notify_handle != NULL)
    {
        xTaskNotifyGive(notify_handle);
    }
    else
    {
        ESP_LOGW(TAG, "BLE transition finished before network task handle ready");
    }
}

/**
 * @brief 发布一次 provisioning 入口请求的完成结果。
 */
static void network_service_finish_ble_provisioning(uint32_t generation,
                                                    esp_err_t result)
{
    bool current = false;
    TaskHandle_t notify_handle = NULL;

    taskENTER_CRITICAL(&s_snapshot_lock);
    current = s_ble_provision_request_generation == generation;
    s_ble_provision_applied_generation = generation;
    if (current)
    {
        s_snapshot.provisioning_generation = s_ble_provision_intent_generation;
        s_snapshot.provisioning_transition_pending = false;
        s_snapshot.provisioning_last_error = result;
    }
    s_ble_transition_completed = true;
    notify_handle = s_network_task_handle;
    taskEXIT_CRITICAL(&s_snapshot_lock);

    ESP_LOGI(TAG, "BLE provisioning request complete: generation=%u result=%s",
             (unsigned)generation, esp_err_to_name(result));

    if ((s_ble_operation_active ==
             NETWORK_SERVICE_BLE_OPERATION_START_BLE_PROVISIONING ||
         s_ble_operation_active ==
             NETWORK_SERVICE_BLE_OPERATION_START_SOFTAP_PROVISIONING) &&
        generation == s_ble_provision_request_generation &&
        s_provision_request_generation != 0U)
    {
        (void)runtime_coordinator_report_start_result(
            RUNTIME_COORDINATOR_PARTICIPANT_NETWORK_PROVISIONING,
            s_provision_request_generation, result);
        if (result != ESP_OK)
        {
            s_provision_request_generation = 0U;
        }
    }

    taskENTER_CRITICAL(&s_snapshot_lock);
    s_ble_transition_completed = true;
    notify_handle = s_network_task_handle;
    taskEXIT_CRITICAL(&s_snapshot_lock);

    if (notify_handle != NULL)
    {
        xTaskNotifyGive(notify_handle);
    }
    else
    {
        ESP_LOGW(TAG, "BLE provisioning finished before network task handle ready");
    }
}

/**
 * @brief 在 internal-stack worker 中执行 BLE 偏好持久化和真实启停。
 *
 * BLE 偏好持久化、presence 启停和官方 provisioning 启动都可能经过 NVS、
 * NimBLE 或 SoftAP manager；flash cache 关闭期间不能使用 PSRAM task stack，
 * 因此该 worker 必须由 `xTaskCreateWithCaps(...INTERNAL)` 创建。
 */
static void network_service_ble_transition_task(void *pv_parameter)
{
    (void)pv_parameter;
    /* creator 先发布 task handle，避免极速完成的 worker 留下不可回收旧句柄。 */
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const network_service_ble_operation_t operation = s_ble_operation_active;
    const bool enabled = s_ble_transition_enabled;
    const uint32_t generation = s_ble_transition_generation;
    const uint32_t provision_generation = s_ble_provision_transition_generation;
    bool applied_enabled = false;
    bool manager_called = false;
    esp_err_t ret = ESP_OK;

    taskENTER_CRITICAL(&s_snapshot_lock);
    applied_enabled = s_ble_applied_enabled;
    taskEXIT_CRITICAL(&s_snapshot_lock);

    if (operation == NETWORK_SERVICE_BLE_OPERATION_START_BLE_PROVISIONING)
    {
        ret = network_manager_start_ble_provisioning();
        network_service_finish_ble_provisioning(provision_generation, ret);
        vTaskSuspend(NULL);
        return;
    }

    if (operation == NETWORK_SERVICE_BLE_OPERATION_START_SOFTAP_PROVISIONING)
    {
        ret = network_manager_start_softap_provisioning();
        if (ret != ESP_OK)
        {
            (void)network_service_restore_ble_presence_if_desired();
        }
        network_service_finish_ble_provisioning(provision_generation, ret);
        vTaskSuspend(NULL);
        return;
    }

    if (operation == NETWORK_SERVICE_BLE_OPERATION_STOP_PROVISIONING)
    {
        ret = network_manager_stop_provisioning();
        uint32_t quiesce_generation = 0U;
        taskENTER_CRITICAL(&s_snapshot_lock);
        quiesce_generation = s_provision_quiesce_generation;
        taskEXIT_CRITICAL(&s_snapshot_lock);
        if (quiesce_generation != 0U)
        {
            const esp_err_t report_ret =
                runtime_coordinator_report_quiesce_result(
                RUNTIME_COORDINATOR_PARTICIPANT_NETWORK_PROVISIONING,
                quiesce_generation, ret);
            if (report_ret == ESP_OK)
            {
                taskENTER_CRITICAL(&s_snapshot_lock);
                if (s_provision_quiesce_generation == quiesce_generation)
                {
                    s_provision_quiesce_generation = 0U;
                }
                taskEXIT_CRITICAL(&s_snapshot_lock);
            }
            else
            {
                ESP_LOGW(TAG, "provisioning quiesce ACK failed: %s",
                         esp_err_to_name(report_ret));
            }
        }
        network_service_finish_ble_provisioning(provision_generation, ret);
        if (ret == ESP_OK)
        {
            s_provision_request_generation = 0U;
        }
        vTaskSuspend(NULL);
        return;
    }

    if (!network_service_ble_request_is_current(generation, enabled))
    {
        network_service_finish_ble_transition(
            generation, enabled, applied_enabled, ESP_ERR_INVALID_STATE);
        vTaskSuspend(NULL);
        return;
    }

    if (network_service_ble_request_is_current(generation, enabled))
    {
        manager_called = true;
        ret = network_manager_set_ble_enabled(enabled);
        if (ret == ESP_OK)
        {
            applied_enabled = enabled;
        }
        if (enabled && ret == ESP_ERR_NO_MEM &&
            network_service_ble_request_is_current(generation, enabled))
        {
            vTaskDelay(pdMS_TO_TICKS(kBleRetryDelayMs));
            if (network_service_ble_request_is_current(generation, enabled))
            {
                ret = network_manager_set_ble_enabled(true);
                if (ret == ESP_OK)
                {
                    applied_enabled = true;
                }
            }
        }
    }

    if (enabled && manager_called && ret != ESP_OK)
    {
        const esp_err_t cleanup_ret = network_manager_set_ble_enabled(false);
        if (cleanup_ret == ESP_OK)
        {
            applied_enabled = false;
        }
        else
        {
            ret = cleanup_ret;
        }
    }
    ESP_LOGI(TAG,
             "BLE transition complete: enabled=%d generation=%u result=%s",
             enabled ? 1 : 0, (unsigned)generation, esp_err_to_name(ret));
    uint32_t quiesce_generation = 0U;
    taskENTER_CRITICAL(&s_snapshot_lock);
    if (!enabled)
    {
        quiesce_generation = s_ble_presence_quiesce_generation;
    }
    taskEXIT_CRITICAL(&s_snapshot_lock);
    if (quiesce_generation != 0U)
    {
        const esp_err_t report_ret = runtime_coordinator_report_quiesce_result(
            RUNTIME_COORDINATOR_PARTICIPANT_BLE_PRESENCE,
            quiesce_generation, ret);
        if (report_ret == ESP_OK)
        {
            taskENTER_CRITICAL(&s_snapshot_lock);
            if (s_ble_presence_quiesce_generation == quiesce_generation)
            {
                s_ble_presence_quiesce_generation = 0U;
            }
            taskEXIT_CRITICAL(&s_snapshot_lock);
        }
        else
        {
            ESP_LOGW(TAG, "BLE presence quiesce ACK failed: %s",
                     esp_err_to_name(report_ret));
        }
    }
    network_service_finish_ble_transition(generation, enabled,
                                          applied_enabled, ret);
    vTaskSuspend(NULL);
}

/**
 * @brief 由 network owner 按最新 generation 创建一个 BLE transition worker。
 */
static void network_service_reconcile_ble(void)
{
    bool should_start = false;
    TaskHandle_t completed_worker = NULL;

    taskENTER_CRITICAL(&s_snapshot_lock);
    if (s_ble_transition_completed)
    {
        completed_worker = s_ble_transition_task_handle;
        s_ble_transition_task_handle = NULL;
        s_ble_transition_completed = false;
    }
    taskEXIT_CRITICAL(&s_snapshot_lock);

    if (completed_worker != NULL)
    {
        vTaskDeleteWithCaps(completed_worker);
    }

    taskENTER_CRITICAL(&s_snapshot_lock);
    should_start = s_ble_transition_task_handle == NULL &&
                   s_ble_request_generation != s_ble_applied_generation;
    if (!should_start &&
        s_ble_transition_task_handle == NULL &&
        s_ble_provision_request_generation !=
            s_ble_provision_applied_generation)
    {
        should_start = true;
        s_ble_operation_active = s_ble_operation_request;
        s_ble_provision_transition_generation =
            s_ble_provision_request_generation;
    }
    if (should_start)
    {
        if (s_ble_request_generation != s_ble_applied_generation)
        {
            s_ble_operation_active = NETWORK_SERVICE_BLE_OPERATION_TOGGLE;
            s_ble_transition_enabled = s_ble_runtime_target_enabled;
            s_ble_transition_generation = s_ble_request_generation;
            s_snapshot.ble_transition_pending = true;
            s_snapshot.ble_runtime_ready = false;
            s_snapshot.ble_last_error = ESP_OK;
        }
    }
    taskEXIT_CRITICAL(&s_snapshot_lock);

    if (!should_start)
    {
        return;
    }

    TaskHandle_t worker_handle = NULL;
    const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        network_service_ble_transition_task, "network_ble",
        kBleTransitionStackBytes, NULL, kBleTransitionTaskPriority,
        &worker_handle, 0, MALLOC_CAP_INTERNAL);

    taskENTER_CRITICAL(&s_snapshot_lock);
    if (created == pdPASS)
    {
        s_ble_transition_task_handle = worker_handle;
    }
    else
    {
        if (s_ble_operation_active == NETWORK_SERVICE_BLE_OPERATION_TOGGLE)
        {
            s_ble_applied_generation = s_ble_transition_generation;
            if (s_ble_request_generation == s_ble_transition_generation &&
                s_ble_runtime_target_enabled == s_ble_transition_enabled)
            {
                s_snapshot.ble_transition_pending = false;
                s_snapshot.ble_runtime_ready = false;
                s_snapshot.ble_last_error = ESP_ERR_NO_MEM;
            }
        }
        else
        {
            s_ble_provision_applied_generation =
                s_ble_provision_transition_generation;
            s_snapshot.provisioning_generation =
                s_ble_provision_transition_generation;
            s_snapshot.provisioning_transition_pending = false;
            s_snapshot.provisioning_last_error = ESP_ERR_NO_MEM;
        }
    }
    taskEXIT_CRITICAL(&s_snapshot_lock);

    if (created == pdPASS)
    {
        xTaskNotifyGive(worker_handle);
    }

    if (created != pdPASS)
    {
        ESP_LOGW(TAG, "BLE transition worker create failed");
    }
}

/**
 * @brief 提交 Wi-Fi 页面 provisioning 入口请求。
 */
static esp_err_t network_service_request_provisioning(
    network_service_ble_operation_t operation)
{
    if (s_network_task_handle == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (operation != NETWORK_SERVICE_BLE_OPERATION_START_BLE_PROVISIONING &&
        operation != NETWORK_SERVICE_BLE_OPERATION_START_SOFTAP_PROVISIONING &&
        operation != NETWORK_SERVICE_BLE_OPERATION_STOP_PROVISIONING)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (operation == NETWORK_SERVICE_BLE_OPERATION_STOP_PROVISIONING)
    {
        const uint32_t request_generation = s_provision_request_generation;
        if (request_generation != 0U)
        {
            s_provision_stop_requested = true;
            return runtime_coordinator_cancel_request(
                RUNTIME_COORDINATOR_PARTICIPANT_NETWORK_PROVISIONING,
                request_generation);
        }

        taskENTER_CRITICAL(&s_snapshot_lock);
        s_ble_operation_request = operation;
        ++s_ble_provision_request_generation;
        if (s_ble_provision_request_generation == 0U)
        {
            s_ble_provision_request_generation = 1U;
        }
        ++s_ble_provision_intent_generation;
        s_snapshot.provisioning_generation =
            s_ble_provision_intent_generation;
        s_snapshot.provisioning_transition_pending = true;
        s_snapshot.provisioning_last_error = ESP_OK;
        taskEXIT_CRITICAL(&s_snapshot_lock);
        xTaskNotifyGive(s_network_task_handle);
        return ESP_OK;
    }

    if (s_provision_request_generation != 0U)
    {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_snapshot_lock);
    s_provision_pending_operation = operation;
    ++s_ble_provision_intent_generation;
    if (s_ble_provision_intent_generation == 0U)
    {
        s_ble_provision_intent_generation = 1U;
    }
    s_snapshot.provisioning_generation = s_ble_provision_intent_generation;
    s_snapshot.provisioning_transition_pending = true;
    s_snapshot.provisioning_last_error = ESP_OK;
    taskEXIT_CRITICAL(&s_snapshot_lock);

    s_provision_request_generation = 0U;
    const esp_err_t ret = runtime_coordinator_request_foreground(
        RUNTIME_COORDINATOR_PARTICIPANT_NETWORK_PROVISIONING,
        &s_provision_request_generation);
    if (ret != ESP_OK)
    {
        s_provision_request_generation = 0U;
        taskENTER_CRITICAL(&s_snapshot_lock);
        s_snapshot.provisioning_transition_pending = false;
        s_snapshot.provisioning_last_error = ret;
        taskEXIT_CRITICAL(&s_snapshot_lock);
    }
    return ret;
}

static esp_err_t network_service_coordinator_grant_provisioning(
    uint32_t generation, void *user_ctx)
{
    (void)user_ctx;
    if (generation != s_provision_request_generation ||
        s_provision_pending_operation == NETWORK_SERVICE_BLE_OPERATION_NONE)
    {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_snapshot_lock);
    s_ble_operation_request = s_provision_pending_operation;
    ++s_ble_provision_request_generation;
    if (s_ble_provision_request_generation == 0U)
    {
        s_ble_provision_request_generation = 1U;
    }
    taskEXIT_CRITICAL(&s_snapshot_lock);
    xTaskNotifyGive(s_network_task_handle);
    return ESP_OK;
}

static esp_err_t network_service_coordinator_cancel_provisioning(
    uint32_t generation, esp_err_t reason, void *user_ctx)
{
    (void)user_ctx;
    if (generation != s_provision_request_generation)
    {
        return ESP_OK;
    }
    s_provision_request_generation = 0U;
    s_provision_pending_operation = NETWORK_SERVICE_BLE_OPERATION_NONE;
    s_provision_stop_requested = false;
    taskENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot.provisioning_transition_pending = false;
    s_snapshot.provisioning_last_error = reason;
    taskEXIT_CRITICAL(&s_snapshot_lock);
    xTaskNotifyGive(s_network_task_handle);
    return ESP_OK;
}

static esp_err_t network_service_coordinator_quiesce_provisioning(
    uint32_t generation, void *user_ctx)
{
    (void)user_ctx;
    taskENTER_CRITICAL(&s_snapshot_lock);
    s_provision_quiesce_generation = generation;
    s_provision_stop_requested = false;
    s_ble_operation_request = NETWORK_SERVICE_BLE_OPERATION_STOP_PROVISIONING;
    ++s_ble_provision_request_generation;
    if (s_ble_provision_request_generation == 0U)
    {
        s_ble_provision_request_generation = 1U;
    }
    taskEXIT_CRITICAL(&s_snapshot_lock);
    xTaskNotifyGive(s_network_task_handle);
    return ESP_OK;
}

static esp_err_t network_service_coordinator_quiesce_ble_presence(
    uint32_t generation, void *user_ctx)
{
    (void)user_ctx;
    bool already_quiesced = false;
    taskENTER_CRITICAL(&s_snapshot_lock);
    s_ble_presence_blocked = true;
    s_ble_runtime_target_enabled = false;
    s_ble_presence_quiesce_generation = generation;
    already_quiesced = !s_ble_applied_enabled;
    if (!already_quiesced)
    {
        ++s_ble_request_generation;
        if (s_ble_request_generation == 0U)
        {
            s_ble_request_generation = 1U;
        }
        s_snapshot.ble_transition_pending = true;
    }
    taskEXIT_CRITICAL(&s_snapshot_lock);

    if (already_quiesced)
    {
        const esp_err_t ret = runtime_coordinator_report_quiesce_result(
            RUNTIME_COORDINATOR_PARTICIPANT_BLE_PRESENCE,
            generation, ESP_OK);
        if (ret == ESP_OK)
        {
            taskENTER_CRITICAL(&s_snapshot_lock);
            if (s_ble_presence_quiesce_generation == generation)
            {
                s_ble_presence_quiesce_generation = 0U;
            }
            taskEXIT_CRITICAL(&s_snapshot_lock);
        }
        return ret;
    }
    xTaskNotifyGive(s_network_task_handle);
    return ESP_OK;
}

static esp_err_t network_service_coordinator_reevaluate_ble_presence(
    uint32_t generation, void *user_ctx)
{
    (void)generation;
    (void)user_ctx;
    taskENTER_CRITICAL(&s_snapshot_lock);
    s_ble_presence_blocked = false;
    s_ble_runtime_target_enabled = s_ble_desired_enabled;
    ++s_ble_request_generation;
    if (s_ble_request_generation == 0U)
    {
        s_ble_request_generation = 1U;
    }
    s_snapshot.ble_transition_pending = true;
    taskEXIT_CRITICAL(&s_snapshot_lock);
    xTaskNotifyGive(s_network_task_handle);
    return ESP_OK;
}

/**
 * @brief Wi-Fi 已连上后自动收口 BLE provisioning transport。
 *
 * 收到配网凭据后，真正的连接结果由 `network_manager` 根据 Wi-Fi runtime 判定；
 * 这里发现“BLE 配网仍 active 但 STA 已连接”时，只提交 stop 意图。真实
 * stop/deinit 仍由 internal-stack worker 执行，避免 PSRAM 栈的监控任务直接
 * 承担 NimBLE / provisioning manager 收尾。
 */
static void network_service_stop_completed_ble_provisioning_if_connected(
    const network_manager_status_t *status)
{
    if (status == NULL || !status->wifi_connected || !status->ble_active)
    {
        return;
    }

    if (s_provision_request_generation != 0U &&
        !s_provision_stop_requested)
    {
        s_provision_stop_requested = true;
        (void)runtime_coordinator_cancel_request(
            RUNTIME_COORDINATOR_PARTICIPANT_NETWORK_PROVISIONING,
            s_provision_request_generation);
    }
}

/**
 * @brief 在官方 provisioning 自动结束后取消 coordinator 前台请求。
 *
 * ESP-IDF provisioning manager 会在凭据成功后自动停止服务；该停止不是
 * `network_service` 发起的 worker 操作，因此 owner 需要在周期快照中补一次
 * coordinator 对账。这里只处理 provisioning 的前台请求，不影响普通 BLE
 * presence 的用户偏好。
 */
static void network_service_cancel_completed_ble_provisioning_request(
    const network_manager_status_t *status)
{
    if (status == NULL)
    {
        return;
    }

    if (s_provision_request_generation != 0U &&
        !s_provision_stop_requested && !status->ble_active &&
        status->state != NETWORK_MANAGER_STATE_PROVISIONING_BLE &&
        status->state != NETWORK_MANAGER_STATE_PROVISIONING_SOFTAP)
    {
        s_provision_stop_requested = true;
        (void)runtime_coordinator_cancel_request(
            RUNTIME_COORDINATOR_PARTICIPANT_NETWORK_PROVISIONING,
            s_provision_request_generation);
    }
}

/**
 * @brief 复制网络服务快照。
 *
 * 该函数只复制内存中的 owner facts，不执行 DNS、Wi-Fi 或 network_manager I/O。
 */
static network_service_snapshot_t network_service_copy_snapshot(void)
{
    network_service_snapshot_t snapshot;

    taskENTER_CRITICAL(&s_snapshot_lock);
    snapshot = s_snapshot;
    taskEXIT_CRITICAL(&s_snapshot_lock);

    return snapshot;
}

/**
 * @brief 原子更新最近错误码。
 * @param error 错误码。
 */
static void network_service_set_last_error(esp_err_t error)
{
    taskENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot.last_error = error;
    taskEXIT_CRITICAL(&s_snapshot_lock);
}

/**
 * @brief 原子更新 Wi-Fi 连接事实。
 * @param connected true 表示当前 Wi-Fi 已连接。
 */
static void network_service_set_wifi_connected(bool connected)
{
    taskENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot.wifi_connected = connected;
    taskEXIT_CRITICAL(&s_snapshot_lock);
}

/**
 * @brief 原子更新云端探测状态。
 * @param active true 表示探测正在进行。
 * @param paused_by_budget true 表示因预算暂停探测。
 * @param result 最近一次探测结果。
 */
static void network_service_set_probe_snapshot(bool active,
                                               bool paused_by_budget,
                                               esp_err_t result)
{
    taskENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot.probe_active = active;
    s_snapshot.probe_paused_by_budget = paused_by_budget;
    s_snapshot.last_probe_result = result;
    taskEXIT_CRITICAL(&s_snapshot_lock);
}

/**
 * @brief 清空服务层缓存的 IPv4 地址。
 * @return 无返回值。
 */
static void network_service_clear_cached_ip(void)
{
    s_network_ip[0] = '\0';
}

/**
 * @brief 返回网络服务状态的可读字符串。
 * @param[in] state 目标状态。
 * @return 状态名字符串。
 */
static const char *network_service_state_name(network_service_state_t state)
{
    switch (state)
    {
    case NETWORK_SERVICE_STATE_OFFLINE:
        return "OFFLINE";
    case NETWORK_SERVICE_STATE_BLE_PROVISIONING:
        return "BLE_PROVISIONING";
    case NETWORK_SERVICE_STATE_BLE_DISABLED:
        return "BLE_DISABLED";
    case NETWORK_SERVICE_STATE_CONNECTING:
        return "CONNECTING";
    case NETWORK_SERVICE_STATE_WIFI_READY:
        return "WIFI_READY";
    case NETWORK_SERVICE_STATE_SERVICE_READY:
        return "SERVICE_READY";
    case NETWORK_SERVICE_STATE_PORTAL_REQUIRED:
        return "PORTAL_REQUIRED";
    case NETWORK_SERVICE_STATE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

/**
 * @brief 更新网络服务状态，并在发生迁移时输出原因日志。
 * @param[in] state 目标状态。
 * @param[in] reason 迁移原因，可为 `NULL`。
 * @return 无返回值。
 */
static void network_service_set_state(network_service_state_t state,
                                      const char *reason)
{
    const network_service_state_t old_state =
        network_service_copy_snapshot().state;

    if (old_state != state)
    {
        ESP_LOGI(TAG, "network state: %s -> %s (%s)",
                 network_service_state_name(old_state),
                 network_service_state_name(state),
                 reason != NULL ? reason : "no-reason");
    }

    taskENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot.state = state;
    s_snapshot.service_ready =
        (state == NETWORK_SERVICE_STATE_SERVICE_READY);
    taskEXIT_CRITICAL(&s_snapshot_lock);
}

/**
 * @brief 判断当前是否存在 recent Wi-Fi 凭据。
 *
 * `network_manager` 当前已经把 recent Wi-Fi 列表作为“下次自动尝试”的
 * 统一凭据来源，因此兼容层以 recent 列表是否为空，来近似表达
 * “是否存在保存凭据”。
 *
 * @return true 表示至少存在 1 条 recent Wi-Fi 记录。
 */
static bool network_service_has_saved_credentials(void)
{
    size_t count = 0;

    if (network_manager_get_recent_networks(NULL, 0, &count) != ESP_OK)
    {
        return false;
    }

    return count > 0U;
}

/**
 * @brief 从 `network_manager` 状态快照同步缓存 IP。
 *
 * 只有在 Wi-Fi 已连接且 IP 发生变化时才输出日志，避免后台轮询刷屏。
 *
 * @param[in] status 当前 `network_manager` 状态快照。
 * @return 无返回值。
 */
static void network_service_sync_cached_ip(
    const network_manager_status_t *status)
{
    if (status == NULL || !status->wifi_connected || status->ip[0] == '\0')
    {
        network_service_clear_cached_ip();
        network_service_set_wifi_connected(false);
        return;
    }

    network_service_set_wifi_connected(true);

    if (strcmp(s_network_ip, status->ip) != 0)
    {
        strncpy(s_network_ip, status->ip, sizeof(s_network_ip) - 1U);
        s_network_ip[sizeof(s_network_ip) - 1U] = '\0';
        ESP_LOGI(TAG, "Wi-Fi connected, IP: %s", s_network_ip);
    }
}

/**
 * @brief 发布 Wi-Fi 管理页只读快照。
 *
 * 该函数只能由 network owner 在已拿到 `network_manager` 快照后调用；
 * UI getter 只复制这里发布的缓存，避免 LVGL timer 触发底层状态刷新。
 *
 * @param[in] status 当前 `network_manager` 状态；为 NULL 时发布离线兜底。
 */
static void network_service_publish_wifi_status(
    const network_manager_status_t *status)
{
    network_service_wifi_status_t next = {
        .service_state = NETWORK_SERVICE_STATE_ERROR,
        .wifi_connected = false,
        .has_credentials = network_service_has_saved_credentials(),
        .user_disconnect_latched = false,
        .provisioning_active = false,
        .ble_active = false,
        .ap_active = false,
        .default_transport = NETWORK_SERVICE_PROVISION_TRANSPORT_BLE,
        .ip = {0},
    };

    if (status != NULL)
    {
        next.service_state = network_service_map_manager_state(status);
        next.wifi_connected = status->wifi_connected;
        next.user_disconnect_latched =
            (status->state == NETWORK_MANAGER_STATE_DISCONNECTED_BY_USER);
        next.ble_active = status->ble_active;
        next.ap_active =
            (status->state == NETWORK_MANAGER_STATE_PROVISIONING_SOFTAP);
        next.provisioning_active =
            (status->state == NETWORK_MANAGER_STATE_PROVISIONING_BLE) ||
            (status->state == NETWORK_MANAGER_STATE_PROVISIONING_SOFTAP);
        next.default_transport =
            network_service_map_transport_from_manager(
                status->default_transport);

        if (status->ip[0] != '\0')
        {
            strncpy(next.ip, status->ip, sizeof(next.ip) - 1U);
            next.ip[sizeof(next.ip) - 1U] = '\0';
        }
    }

    taskENTER_CRITICAL(&s_snapshot_lock);
    s_wifi_status_snapshot = next;
    taskEXIT_CRITICAL(&s_snapshot_lock);
}

/**
 * @brief 将 `network_manager` 的主状态映射为兼容层状态。
 *
 * @param[in] status 当前 `network_manager` 状态快照。
 * @return 对应的 `network_service` 兼容状态。
 */
static network_service_state_t network_service_map_manager_state(
    const network_manager_status_t *status)
{
    const bool has_credentials = network_service_has_saved_credentials();

    if (status == NULL)
    {
        return NETWORK_SERVICE_STATE_ERROR;
    }

    switch (status->state)
    {
    case NETWORK_MANAGER_STATE_CONNECTING_LATEST:
        return NETWORK_SERVICE_STATE_CONNECTING;
    case NETWORK_MANAGER_STATE_CONNECTED:
        return NETWORK_SERVICE_STATE_WIFI_READY;
    case NETWORK_MANAGER_STATE_PROVISIONING_BLE:
        return NETWORK_SERVICE_STATE_BLE_PROVISIONING;
    case NETWORK_MANAGER_STATE_PROVISIONING_SOFTAP:
        return NETWORK_SERVICE_STATE_PORTAL_REQUIRED;
    case NETWORK_MANAGER_STATE_DISCONNECTED_BY_USER:
        return NETWORK_SERVICE_STATE_OFFLINE;
    case NETWORK_MANAGER_STATE_ERROR:
        return NETWORK_SERVICE_STATE_ERROR;
    case NETWORK_MANAGER_STATE_IDLE:
    default:
        if (!status->ble_enabled && !has_credentials)
        {
            return NETWORK_SERVICE_STATE_BLE_DISABLED;
        }
        return NETWORK_SERVICE_STATE_OFFLINE;
    }
}

/**
 * @brief 将新架构 transport 枚举映射为兼容层枚举。
 * @param[in] transport `network_manager` transport。
 * @return 对应的 `network_service` transport。
 */
static network_service_provision_transport_t
network_service_map_transport_from_manager(
    network_manager_provisioning_transport_t transport)
{
    switch (transport)
    {
    case NETWORK_MANAGER_PROVISIONING_TRANSPORT_SOFTAP:
        return NETWORK_SERVICE_PROVISION_TRANSPORT_AP;
    case NETWORK_MANAGER_PROVISIONING_TRANSPORT_BLE:
    default:
        return NETWORK_SERVICE_PROVISION_TRANSPORT_BLE;
    }
}

/**
 * @brief 将兼容层 transport 枚举映射为新架构 transport。
 *
 * 旧的 `AUTO` 只为兼容保留，当前内部直接退化为 `BLE`，避免继续保留
 * 两套自动选择语义。
 *
 * @param[in] transport `network_service` transport。
 * @return 对应的 `network_manager` transport。
 */
static network_manager_provisioning_transport_t
network_service_map_transport_to_manager(
    network_service_provision_transport_t transport)
{
    switch (transport)
    {
    case NETWORK_SERVICE_PROVISION_TRANSPORT_AP:
        return NETWORK_MANAGER_PROVISIONING_TRANSPORT_SOFTAP;
    case NETWORK_SERVICE_PROVISION_TRANSPORT_AUTO:
        ESP_LOGW(TAG, "AUTO transport is deprecated, fallback to BLE");
        return NETWORK_MANAGER_PROVISIONING_TRANSPORT_BLE;
    case NETWORK_SERVICE_PROVISION_TRANSPORT_BLE:
    default:
        return NETWORK_MANAGER_PROVISIONING_TRANSPORT_BLE;
    }
}

/**
 * @brief 对单个域名执行一次解析探测。
 *
 * 这里继续选择 DNS 解析而不是业务请求，是为了用更轻量的方式同时验证
 * DNS 基本可用性和外网访问链路是否已经打通。
 *
 * @param[in] hostname 待探测域名。
 * @return true 表示本次探测成功。
 */
static bool resolve_hostname_once(const char *hostname)
{
    struct addrinfo hints = {0};
    struct addrinfo *result = NULL;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    const int err = getaddrinfo(hostname, "443", &hints, &result);
    if (err == 0 && result != NULL)
    {
        freeaddrinfo(result);
        ESP_LOGI(TAG, "network service ready: %s", hostname);
        return true;
    }

    if (result != NULL)
    {
        freeaddrinfo(result);
    }

    ESP_LOGW(TAG, "network service not ready yet: host=%s err=%d", hostname,
             err);
    return false;
}

/**
 * @brief 探测关键业务依赖是否已就绪。
 *
 * 当前策略要求所有关键域名都能成功解析，才把状态提升到
 * `SERVICE_READY`，以避免页面在“仅连上路由器但云端仍不可用”时误判。
 *
 * @return `ESP_OK` 表示所有关键依赖都已就绪；其他错误表示探测超时或失败。
 */
static esp_err_t probe_network_services_ready(void)
{
    network_service_set_probe_snapshot(true, false, ESP_ERR_INVALID_STATE);

    for (size_t host_index = 0;
         host_index < (sizeof(kProbeHosts) / sizeof(kProbeHosts[0]));
         ++host_index)
    {
        const char *hostname = kProbeHosts[host_index];
        for (uint32_t attempt = 1; attempt <= kProbeAttemptMax; ++attempt)
        {
            const power_policy_budget_t budget = power_policy_get_budget();
            if (!budget.network_sync_allowed)
            {
                network_service_set_probe_snapshot(
                    false, true, ESP_ERR_INVALID_STATE);
                ESP_LOGI(TAG, "network service probe paused by power budget");
                return ESP_ERR_INVALID_STATE;
            }

            if (resolve_hostname_once(hostname))
            {
                break;
            }

            if (attempt == kProbeAttemptMax)
            {
                ESP_LOGW(TAG,
                         "network service probe timed out: host=%s attempts=%u",
                         hostname, (unsigned)attempt);
                network_service_set_probe_snapshot(
                    false, false, ESP_ERR_TIMEOUT);
                return ESP_ERR_TIMEOUT;
            }

            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) > 0U)
            {
                network_service_set_probe_snapshot(
                    false, false, ESP_ERR_INVALID_STATE);
                ESP_LOGI(TAG, "network probe yielded to owner command");
                return ESP_ERR_INVALID_STATE;
            }
        }
    }

    network_service_set_probe_snapshot(false, false, ESP_OK);
    return ESP_OK;
}

/**
 * @brief 按整机预算同步 Wi-Fi runtime 省电配置。
 *
 * 第一版 STANDBY 不主动断开 AP，也不销毁 IP/MQTT/HTTP 状态；这里只切换
 * `wifi_control` 的 `esp_wifi_set_ps()` 配置，并让本服务层暂停非关键探测。
 *
 * @return 无返回值。
 */
static void network_service_apply_power_budget(void)
{
    const power_policy_budget_t budget = power_policy_get_budget();
    music_service_snapshot_t music_snapshot = {0};
    const bool music_stream_active =
        music_service_get_snapshot(&music_snapshot) == ESP_OK &&
        (music_snapshot.state == MUSIC_SERVICE_STATE_BUFFERING ||
         music_snapshot.state == MUSIC_SERVICE_STATE_PLAYING);
    official_chat_service_snapshot_t chat_snapshot = {0};
    const bool official_chat_audio_active =
        official_chat_service_get_snapshot(&chat_snapshot) == ESP_OK &&
        (chat_snapshot.state == OFFICIAL_CHAT_SERVICE_STATE_CONNECTING ||
         chat_snapshot.state == OFFICIAL_CHAT_SERVICE_STATE_LISTENING ||
         chat_snapshot.state == OFFICIAL_CHAT_SERVICE_STATE_SPEAKING);
    /* maintenance 仍会暂停后台同步，但 OTA 等前台 HTTPS 传输必须保持 STA
     * 唤醒；否则 DTIM 省电会把长镜像下载误当成可延后的后台请求。 */
    const bool maintenance_keeps_wifi_awake =
        (budget.flags & POWER_POLICY_FLAG_MAINTENANCE) != 0U;
    /* 音乐和 Hermes 下行都属于持续媒体传输，允许屏幕保持 STANDBY，但不能让
     * modem sleep 延迟或重置音频连接；network owner 统一消费两个服务快照。 */
    const bool power_save = !music_stream_active &&
                            !official_chat_audio_active &&
                            !maintenance_keeps_wifi_awake &&
                            (!budget.network_sync_allowed ||
                             budget.state == POWER_POLICY_STATE_STANDBY);

    if (s_runtime_power_save_known &&
        s_runtime_power_save_applied == power_save)
    {
        return;
    }

    esp_err_t ret = wifi_control_set_power_save(power_save);
    if (ret != ESP_OK)
    {
        network_service_set_last_error(ret);
        ESP_LOGW(TAG, "Wi-Fi power save apply failed: standby=%d err=%s",
                 power_save ? 1 : 0, esp_err_to_name(ret));
        return;
    }

    s_runtime_power_save_applied = power_save;
    s_runtime_power_save_known = true;
    taskENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot.power_save_applied = power_save;
    taskEXIT_CRITICAL(&s_snapshot_lock);
    const char *active_reason =
        music_stream_active ? " (music stream active)" :
        (official_chat_audio_active ? " (official chat audio active)" : "");
    ESP_LOGI(TAG, "Wi-Fi power save %s by power budget%s",
             power_save ? "enabled" : "disabled",
             active_reason);
}

/**
 * @brief 网络服务后台任务。
 * @param[in] pv_parameter 未使用，保留任务签名。
 * @return 无返回值。
 *
 * 该任务不再推进联网动作，只做两件事：
 * 1. 轮询 `network_manager` 并把状态映射为兼容层语义；
 * 2. 在 Wi-Fi 连通后继续执行关键云端依赖探测。
 */
static void network_service_task(void *pv_parameter)
{
    (void)pv_parameter;
    s_network_task_handle = xTaskGetCurrentTaskHandle();

    while (1)
    {
        network_service_reconcile_ble();
        network_service_apply_power_budget();

        network_manager_status_t status = {0};
        const esp_err_t ret = network_manager_get_status(&status);

        if (ret != ESP_OK)
        {
            network_service_clear_cached_ip();
            network_service_set_wifi_connected(false);
            network_service_publish_wifi_status(NULL);
            network_service_set_last_error(ret);
            network_service_set_probe_snapshot(false, false, ESP_FAIL);
            network_service_set_state(NETWORK_SERVICE_STATE_ERROR,
                                      "network manager status unavailable");
            (void)ulTaskNotifyTake(pdTRUE,
                                   pdMS_TO_TICKS(kServicePollPeriodMs));
            continue;
        }

        network_service_stop_completed_ble_provisioning_if_connected(&status);
        network_service_cancel_completed_ble_provisioning_request(&status);
        network_service_sync_cached_ip(&status);
        network_service_publish_wifi_status(&status);
        const power_policy_budget_t budget = power_policy_get_budget();

        if (status.wifi_connected && budget.network_sync_allowed)
        {
            if (network_service_copy_snapshot().state !=
                NETWORK_SERVICE_STATE_SERVICE_READY)
            {
                network_service_set_state(
                    NETWORK_SERVICE_STATE_WIFI_READY,
                    "STA connected, probe cloud dependencies");
                if (probe_network_services_ready() == ESP_OK)
                {
                    network_service_set_state(
                        NETWORK_SERVICE_STATE_SERVICE_READY,
                        "critical hosts resolved");
                    if (system_time_service_note_network_ready() != ESP_OK)
                    {
                        ESP_LOGW(TAG, "system time network-ready notify failed");
                    }
                }
                else
                {
                    network_service_set_state(
                        NETWORK_SERVICE_STATE_WIFI_READY,
                        "cloud probe still pending");
                }
            }
        }
        else if (status.wifi_connected)
        {
            network_service_set_probe_snapshot(
                false, true, ESP_ERR_INVALID_STATE);
            network_service_set_state(
                NETWORK_SERVICE_STATE_WIFI_READY,
                "network sync paused by power budget");
        }
        else
        {
            network_service_clear_cached_ip();
            network_service_set_wifi_connected(false);
            network_service_set_probe_snapshot(
                false, false, ESP_ERR_INVALID_STATE);
            network_service_set_state(
                network_service_map_manager_state(&status),
                "mirrored from network manager");
        }

        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kServicePollPeriodMs));
    }
}

/**
 * @brief 启动网络服务后台任务。
 *
 * 当前会先确保 `network_manager` 已启动，再创建自己的轻量监控任务。
 *
 * @return `ESP_OK` 表示任务已启动或之前已启动；失败表示无法创建后台任务。
 */
esp_err_t network_service_start(void)
{
    esp_err_t ret = ESP_OK;

    if (s_network_task_handle != NULL)
    {
        return ESP_OK;
    }

    ret = network_manager_start();
    if (ret != ESP_OK)
    {
        network_service_set_last_error(ret);
        network_service_set_state(NETWORK_SERVICE_STATE_ERROR,
                                  "network manager start failed");
        return ret;
    }

    const runtime_coordinator_participant_config_t provisioning = {
        .id = RUNTIME_COORDINATOR_PARTICIPANT_NETWORK_PROVISIONING,
        .name = "network_provisioning",
        .capabilities = RUNTIME_COORDINATOR_CAPABILITY_FOREGROUND_EXCLUSIVE,
        .request_quiesce =
            network_service_coordinator_quiesce_provisioning,
        .grant_foreground =
            network_service_coordinator_grant_provisioning,
        .cancel_pending_request =
            network_service_coordinator_cancel_provisioning,
    };
    ret = runtime_coordinator_register(&provisioning);
    if (ret != ESP_OK)
    {
        return ret;
    }
    const runtime_coordinator_participant_config_t presence = {
        .id = RUNTIME_COORDINATOR_PARTICIPANT_BLE_PRESENCE,
        .name = "ble_presence",
        .capabilities =
            RUNTIME_COORDINATOR_CAPABILITY_BACKGROUND_PREEMPTIBLE,
        .request_quiesce =
            network_service_coordinator_quiesce_ble_presence,
        .request_reevaluate =
            network_service_coordinator_reevaluate_ble_presence,
    };
    ret = runtime_coordinator_register(&presence);
    if (ret != ESP_OK)
    {
        return ret;
    }

    const bool initial_ble_enabled = network_manager_is_ble_enabled();
    taskENTER_CRITICAL(&s_snapshot_lock);
    s_ble_desired_enabled = initial_ble_enabled;
    s_ble_runtime_target_enabled = initial_ble_enabled;
    s_ble_applied_enabled = initial_ble_enabled;
    s_ble_request_generation = 1U;
    s_ble_applied_generation = 0U;
    s_snapshot.ble_desired_enabled = s_ble_desired_enabled;
    s_snapshot.ble_applied_enabled = s_ble_applied_enabled;
    s_snapshot.ble_runtime_ready = false;
    s_snapshot.ble_transition_pending = true;
    s_snapshot.ble_generation = s_ble_request_generation;
    s_snapshot.ble_last_error = ESP_OK;
    taskEXIT_CRITICAL(&s_snapshot_lock);

    /* 栈缩为 4096B：高压实测 free=3804B（62% 空闲），缩 2KB 仍有余量。
     * 栈迁 PSRAM：省 internal RAM，该任务不直接做 NVS/flash 写操作。 */
    const BaseType_t result =
        xTaskCreatePinnedToCoreWithCaps(network_service_task, "network_service",
                                        1024 * 4, NULL, 5, &s_network_task_handle, 0,
                                        MALLOC_CAP_SPIRAM);
    if (result != pdPASS)
    {
        s_network_task_handle = NULL;
        taskENTER_CRITICAL(&s_snapshot_lock);
        s_ble_applied_generation = s_ble_request_generation;
        s_snapshot.ble_transition_pending = false;
        s_snapshot.ble_runtime_ready = false;
        s_snapshot.ble_last_error = ESP_FAIL;
        taskEXIT_CRITICAL(&s_snapshot_lock);
        network_service_set_last_error(ESP_FAIL);
        network_service_set_state(NETWORK_SERVICE_STATE_ERROR,
                                  "network service task create failed");
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief 获取当前网络服务状态。
 * @return 当前兼容层状态。
 */
network_service_state_t network_service_get_state(void)
{
    return network_service_copy_snapshot().state;
}

/**
 * @brief 获取网络服务生命周期快照。
 * @param snapshot 输出快照，不能为空。
 * @return `ESP_OK` 表示成功复制。
 */
esp_err_t network_service_get_snapshot(network_service_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *snapshot = network_service_copy_snapshot();
    return ESP_OK;
}

/**
 * @brief 提交 BLE 总开关 desired state。
 * @param[in] enabled 目标开关值。
 * @return `ESP_OK` 表示 owner 已收到目标态；owner 尚未启动时返回
 *         `ESP_ERR_INVALID_STATE`。
 */
esp_err_t network_service_set_ble_enabled(bool enabled)
{
    uint32_t generation = 0U;

    if (s_network_task_handle == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_snapshot_lock);
    s_ble_desired_enabled = enabled;
    if (!s_ble_presence_blocked)
    {
        s_ble_runtime_target_enabled = enabled;
    }
    s_ble_request_generation++;
    if (s_ble_request_generation == 0U)
    {
        s_ble_request_generation = 1U;
    }
    s_snapshot.ble_desired_enabled = enabled;
    s_snapshot.ble_runtime_ready = false;
    s_snapshot.ble_transition_pending = true;
    s_snapshot.ble_generation = s_ble_request_generation;
    s_snapshot.ble_last_error = ESP_OK;
    generation = s_ble_request_generation;
    taskEXIT_CRITICAL(&s_snapshot_lock);

    ESP_LOGI(TAG, "queue BLE enable request: enabled=%d generation=%u",
             enabled ? 1 : 0, (unsigned)generation);
    xTaskNotifyGive(s_network_task_handle);
    return ESP_OK;
}

/**
 * @brief 查询 BLE 偏好是否开启。
 * @return true 表示当前允许 BLE。
 */
bool network_service_is_ble_enabled(void)
{
    return network_manager_is_ble_enabled();
}

/**
 * @brief 查询 BLE transport 当前是否活动。
 * @return true 表示 BLE transport 当前活跃。
 */
bool network_service_is_ble_active(void)
{
    return network_manager_is_ble_active();
}

/**
 * @brief 查询当前 Wi-Fi 是否已连接。
 * @return true 表示当前已经拿到有效 Wi-Fi 连接。
 */
bool network_service_is_wifi_connected(void)
{
    network_service_wifi_status_t status = {0};

    if (network_service_get_wifi_status(&status) != ESP_OK)
    {
        return false;
    }

    return status.wifi_connected;
}

/**
 * @brief 获取 Wi-Fi 管理页兼容状态快照。
 *
 * 该结构只为旧调用方保留，真实状态来源已经收敛到 `network_manager`。
 *
 * @param[out] status 输出结构。
 * @return `ESP_OK` 表示成功；`ESP_ERR_INVALID_ARG` 表示参数非法。
 */
esp_err_t network_service_get_wifi_status(network_service_wifi_status_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_snapshot_lock);
    *status = s_wifi_status_snapshot;
    taskEXIT_CRITICAL(&s_snapshot_lock);

    return ESP_OK;
}

/**
 * @brief 再次使用最近一次成功连接的 Wi-Fi。
 * @return `network_manager` 的执行结果。
 */
esp_err_t network_service_request_connect_with_saved_credentials(void)
{
    ESP_LOGI(TAG, "bridge saved Wi-Fi retry request to network_manager");
    return network_manager_use_latest_wifi();
}

/**
 * @brief 主动断开当前网络连接，并暂停自动重连。
 * @return `network_manager` 的执行结果。
 */
esp_err_t network_service_request_disconnect(void)
{
    ESP_LOGI(TAG, "bridge disconnect request to network_manager");
    return network_manager_disconnect();
}

/**
 * @brief 重新进入当前默认 provisioning transport。
 * @return `network_manager` 的执行结果。
 */
esp_err_t network_service_request_reprovision(void)
{
    ESP_LOGI(TAG, "bridge reprovision request to network_manager");
    return network_manager_reprovision();
}

/**
 * @brief 设置默认配网 transport。
 *
 * 兼容层仍接受旧枚举，但真实持久化与调度已经交给 `network_manager`。
 *
 * @param[in] transport 兼容层 transport 枚举。
 * @return `network_manager` 的执行结果。
 */
esp_err_t network_service_set_default_provision_transport(
    network_service_provision_transport_t transport)
{
    const network_manager_provisioning_transport_t manager_transport =
        network_service_map_transport_to_manager(transport);

    ESP_LOGI(TAG, "bridge transport set request to network_manager: transport=%d",
             (int)transport);
    return network_manager_set_default_transport(manager_transport);
}

/**
 * @brief 获取当前默认配网 transport。
 * @return 兼容层 transport 枚举。
 */
network_service_provision_transport_t
network_service_get_default_provision_transport(void)
{
    return network_service_map_transport_from_manager(
        network_manager_get_default_transport());
}

/**
 * @brief 判断云端业务依赖是否已经可用。
 * @return true 表示 Wi-Fi 已连通且关键域名探测通过。
 */
bool network_service_is_service_ready(void)
{
    return network_service_copy_snapshot().service_ready;
}

/**
 * @brief 获取当前缓存的 IPv4 地址字符串。
 *
 * 若缓存为空，会尝试从 `network_manager` 再同步一遍状态，以兼容
 * “调用方先查 IP，再等服务层下一轮轮询”的时序。
 *
 * @param[out] ip_str 输出缓冲区。
 * @param[in] ip_str_len 输出缓冲区长度，单位为字节。
 * @return `ESP_OK` 表示成功复制；
 *         `ESP_ERR_INVALID_ARG` 表示参数非法；
 *         `ESP_ERR_INVALID_STATE` 表示当前尚未拿到有效 IP。
 */
esp_err_t network_service_get_ip(char *ip_str, size_t ip_str_len)
{
    if (ip_str == NULL || ip_str_len == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_network_ip[0] == '\0')
    {
        network_manager_status_t status = {0};
        if (network_manager_get_status(&status) == ESP_OK)
        {
            network_service_sync_cached_ip(&status);
        }
    }

    if (s_network_ip[0] == '\0')
    {
        return ESP_ERR_INVALID_STATE;
    }

    strncpy(ip_str, s_network_ip, ip_str_len - 1U);
    ip_str[ip_str_len - 1U] = '\0';
    return ESP_OK;
}

/**
 * @brief 主动请求切换到 SoftAP 配网。
 *
 * 兼容层直接桥接到 `network_manager` 的显式 SoftAP 入口，避免把
 * “切默认 transport + reprovision”的两步旧 UI 语义继续扩散。
 *
 * @return `ESP_OK` 表示 owner 已收到目标态；`ESP_ERR_INVALID_STATE` 表示
 *         owner 尚未启动。
 */
esp_err_t network_service_request_portal(void)
{
    return network_service_request_provisioning(
        NETWORK_SERVICE_BLE_OPERATION_START_SOFTAP_PROVISIONING);
}

/**
 * @brief 主动请求切换到 BLE 配网。
 *
 * 兼容层只桥接到显式 BLE 配网入口；是否允许 BLE 由主界面蓝牙总开关控制，
 * 本函数不会偷偷打开蓝牙，避免把“蓝牙开关”和“小程序配网”再次耦合。
 *
 * @return `ESP_OK` 表示 owner 已收到目标态；`ESP_ERR_INVALID_STATE` 表示
 *         owner 尚未启动。
 */
esp_err_t network_service_request_ble(void)
{
    return network_service_request_provisioning(
        NETWORK_SERVICE_BLE_OPERATION_START_BLE_PROVISIONING);
}

/**
 * @brief 异步停止当前 provisioning 会话。
 *
 * @return `ESP_OK` 表示 owner 已收到目标态；`ESP_ERR_INVALID_STATE` 表示
 *         owner 尚未启动。
 */
esp_err_t network_service_request_stop_provisioning(void)
{
    return network_service_request_provisioning(
        NETWORK_SERVICE_BLE_OPERATION_STOP_PROVISIONING);
}
