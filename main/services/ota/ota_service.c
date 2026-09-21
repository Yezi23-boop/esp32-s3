#include "services/ota/ota_service.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "services/ota/ota_metrics.h"
#include "services/ota/ota_provider.h"
#include "services/network/network_service.h"
#include "services/power/power_policy.h"
#include "services/runtime/runtime_coordinator.h"
#include "services/runtime/startup_readiness.h"

static const char *TAG = "ota_service";

enum
{
    kCommandQueueLength = 4,
    /* Flash 写入期间 cache 可能被冻结，OTA owner 的栈必须留在片内 RAM。 */
    kTaskStackBytes = 16384,
};

typedef enum
{
    OTA_SERVICE_COMMAND_PREPARE = 0,
    OTA_SERVICE_COMMAND_CANCEL,
    OTA_SERVICE_COMMAND_CHECK_UPDATE,
    OTA_SERVICE_COMMAND_START_DOWNLOAD,
    OTA_SERVICE_COMMAND_ACTIVATE,
    OTA_SERVICE_COMMAND_COORDINATOR_GRANTED,
    OTA_SERVICE_COMMAND_COORDINATOR_QUIESCE,
    OTA_SERVICE_COMMAND_COORDINATOR_CANCELLED,
} ota_service_command_t;

typedef struct
{
    ota_service_command_t type;
    uint32_t generation;
    esp_err_t result;
} ota_service_command_message_t;

typedef struct
{
    StaticQueue_t queue_buffer;
    uint8_t queue_storage[sizeof(ota_service_command_message_t) *
                          kCommandQueueLength];
    QueueHandle_t queue;
    TaskHandle_t task;
    portMUX_TYPE lock;
    ota_service_snapshot_t snapshot;
    ota_update_plan_t plan;
    bool plan_valid;
    bool coordinator_granted;
    bool start_after_grant;
    bool power_maintenance_active;
    bool staged;
    bool cancel_requested;
    ota_transport_fault_mode_t fault_mode;
    uint32_t restart_hold_ms;
    uint32_t coordinator_request_generation;
    uint32_t coordinator_quiesce_generation;
    bool initialized;
} ota_service_context_t;

static ota_service_context_t s_ota = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

const char *ota_service_state_text(ota_service_state_t state)
{
    switch (state)
    {
    case OTA_SERVICE_STATE_IDLE:
        return "IDLE";
    case OTA_SERVICE_STATE_PREPARING:
        return "PREPARING";
    case OTA_SERVICE_STATE_QUIESCING:
        return "QUIESCING";
    case OTA_SERVICE_STATE_QUIESCED:
        return "QUIESCED";
    case OTA_SERVICE_STATE_READY:
        return "READY";
    case OTA_SERVICE_STATE_NO_UPDATE:
        return "NO_UPDATE";
    case OTA_SERVICE_STATE_DOWNLOADING:
        return "DOWNLOADING";
    case OTA_SERVICE_STATE_STAGED:
        return "STAGED";
    case OTA_SERVICE_STATE_VERIFYING:
        return "VERIFYING";
    case OTA_SERVICE_STATE_RESTARTING:
        return "RESTARTING";
    case OTA_SERVICE_STATE_FAILED:
        return "FAILED";
    case OTA_SERVICE_STATE_PENDING_VERIFY:
        return "PENDING_VERIFY";
    case OTA_SERVICE_STATE_VALID:
        return "VALID";
    case OTA_SERVICE_STATE_ROLLED_BACK:
        return "ROLLED_BACK";
    default:
        return "UNKNOWN";
    }
}

static void ota_service_publish_state(ota_service_state_t state,
                                      esp_err_t error)
{
    taskENTER_CRITICAL(&s_ota.lock);
    s_ota.snapshot.state = state;
    s_ota.snapshot.last_error = error;
    s_ota.snapshot.maintenance_active = s_ota.coordinator_granted;
    taskEXIT_CRITICAL(&s_ota.lock);
    ESP_LOGI(TAG, "state=%s err=%s maintenance=%d",
             ota_service_state_text(state), esp_err_to_name(error),
             s_ota.coordinator_granted ? 1 : 0);
}

static void ota_service_cleanup_maintenance(void)
{
    if (s_ota.staged || ota_transport_has_staged_image())
    {
        const esp_err_t abort_ret = ota_transport_abort_staging();
        if (abort_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "staged OTA abort failed: %s",
                     esp_err_to_name(abort_ret));
        }
        s_ota.staged = false;
    }

    if (s_ota.power_maintenance_active)
    {
        const esp_err_t power_ret =
            power_policy_set_maintenance_window(false, "ota");
        if (power_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "power maintenance cleanup failed: err=%s",
                     esp_err_to_name(power_ret));
        }
        s_ota.power_maintenance_active = false;
    }

    taskENTER_CRITICAL(&s_ota.lock);
    s_ota.snapshot.maintenance_active = false;
    taskEXIT_CRITICAL(&s_ota.lock);
}

static esp_err_t ota_service_prepare(void)
{
    ota_service_publish_state(OTA_SERVICE_STATE_PREPARING, ESP_OK);
    const esp_err_t ret = power_policy_set_maintenance_window(true, "ota");
    if (ret != ESP_OK)
    {
        ota_service_cleanup_maintenance();
        ota_service_publish_state(OTA_SERVICE_STATE_FAILED, ret);
        return ret;
    }
    s_ota.power_maintenance_active = true;
    ota_service_publish_state(OTA_SERVICE_STATE_QUIESCED, ESP_OK);
    /* OTA 只发布强前台 owner 事实；业务 owner 观测该事实后自行让路。 */
    ota_service_publish_state(OTA_SERVICE_STATE_READY, ESP_OK);
    return ESP_OK;
}

static void ota_service_progress_cb(size_t received, size_t total,
                                    void *user_ctx)
{
    (void)user_ctx;
    taskENTER_CRITICAL(&s_ota.lock);
    s_ota.snapshot.bytes_received = received;
    s_ota.snapshot.image_size = total;
    s_ota.snapshot.progress_percent =
        total == 0U ? 0U : (uint8_t)((received * 100U) / total);
    taskEXIT_CRITICAL(&s_ota.lock);
}

static bool ota_service_cancel_cb(void *user_ctx)
{
    (void)user_ctx;
    ota_service_command_message_t command = {
        .type = OTA_SERVICE_COMMAND_CANCEL,
    };
    while (xQueueReceive(s_ota.queue, &command, 0) == pdTRUE)
    {
        if (command.type == OTA_SERVICE_COMMAND_CANCEL)
        {
            s_ota.cancel_requested = true;
        }
    }
    return s_ota.cancel_requested;
}

static esp_err_t ota_service_check_update(void)
{
    ota_service_state_t state = OTA_SERVICE_STATE_IDLE;
    taskENTER_CRITICAL(&s_ota.lock);
    state = s_ota.snapshot.state;
    taskEXIT_CRITICAL(&s_ota.lock);
    if (state == OTA_SERVICE_STATE_PREPARING ||
        state == OTA_SERVICE_STATE_QUIESCING ||
        state == OTA_SERVICE_STATE_QUIESCED ||
        state == OTA_SERVICE_STATE_DOWNLOADING ||
        state == OTA_SERVICE_STATE_STAGED ||
        state == OTA_SERVICE_STATE_VERIFYING ||
        state == OTA_SERVICE_STATE_RESTARTING)
    {
        ESP_LOGW(TAG, "check ignored while state=%s",
                 ota_service_state_text(state));
        return ESP_ERR_INVALID_STATE;
    }

    const esp_app_desc_t *description = esp_app_get_description();
    if (description == NULL || description->version[0] == '\0')
    {
        ota_service_publish_state(OTA_SERVICE_STATE_FAILED,
                                  ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_ota.lock);
    memset(&s_ota.plan, 0, sizeof(s_ota.plan));
    s_ota.plan_valid = false;
    s_ota.snapshot.update_available = false;
    s_ota.snapshot.delta_available = false;
    s_ota.snapshot.target_version[0] = '\0';
    s_ota.snapshot.image_size = 0U;
    taskEXIT_CRITICAL(&s_ota.lock);

    ota_service_publish_state(OTA_SERVICE_STATE_PREPARING, ESP_OK);
    /* 网络恢复后再次进入检查页时，顺便重试上次启动未完成的终态上报。 */
    if (network_service_is_service_ready())
    {
        ota_provider_report_pending();
    }
    ota_metrics_stage_begin(OTA_METRICS_STAGE_MANIFEST);
    ota_update_plan_t plan = {0};
    esp_err_t ret = ota_provider_check(description->version, &plan);
    if (ret == ESP_OK)
    {
        ret = ota_provider_prepare_download(&plan);
    }
    if (ret == ESP_ERR_NOT_FOUND)
    {
        ota_metrics_record_result(OTA_METRICS_STAGE_MANIFEST, NULL, ret,
                                  ota_metrics_stage_elapsed_ms(
                                      OTA_METRICS_STAGE_MANIFEST),
                                  false, 0U);
        ota_service_publish_state(OTA_SERVICE_STATE_NO_UPDATE, ret);
        return ret;
    }
    if (ret != ESP_OK)
    {
        ota_metrics_record_result(OTA_METRICS_STAGE_MANIFEST, NULL, ret,
                                  ota_metrics_stage_elapsed_ms(
                                      OTA_METRICS_STAGE_MANIFEST),
                                  false, 0U);
        ota_service_publish_state(OTA_SERVICE_STATE_FAILED, ret);
        return ret;
    }

    taskENTER_CRITICAL(&s_ota.lock);
    s_ota.plan = plan;
    s_ota.plan_valid = true;
    s_ota.snapshot.source = plan.source;
    s_ota.snapshot.update_available = true;
    s_ota.snapshot.delta_available = plan.has_delta;
    s_ota.snapshot.image_size = plan.size;
    strncpy(s_ota.snapshot.target_version, plan.version,
            sizeof(s_ota.snapshot.target_version) - 1U);
    taskEXIT_CRITICAL(&s_ota.lock);

    ota_metrics_record_result(OTA_METRICS_STAGE_MANIFEST, plan.version,
                              ESP_OK,
                              ota_metrics_stage_elapsed_ms(
                                  OTA_METRICS_STAGE_MANIFEST),
                              plan.has_delta, 0U);
    ota_service_publish_state(OTA_SERVICE_STATE_READY, ESP_OK);
    ESP_LOGI(TAG, "update ready: source=%d target=%s size=%u delta=%d",
             (int)plan.source, plan.version, (unsigned int)plan.size,
             plan.has_delta ? 1 : 0);
    return ESP_OK;
}

static esp_err_t ota_service_download_delta_to_staging(void)
{
    if (!s_ota.plan_valid || !s_ota.plan.has_delta)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_app_desc_t *description = esp_app_get_description();
    if (description == NULL || description->version[0] == '\0')
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (strcmp(description->version, s_ota.plan.baseline_version) != 0)
    {
        ESP_LOGW(TAG, "delta baseline mismatch: current=%s baseline=%s",
                 description->version, s_ota.plan.baseline_version);
        return ESP_ERR_INVALID_VERSION;
    }
    const ota_transport_download_config_t config = {
        .root_ca_pem = s_ota.plan.root_ca_pem,
        .use_cert_bundle = s_ota.plan.use_cert_bundle,
        .authorization = s_ota.plan.authorization[0] == '\0'
                             ? NULL
                             : s_ota.plan.authorization,
        .progress_cb = ota_service_progress_cb,
        .cancel_cb = ota_service_cancel_cb,
        .user_ctx = NULL,
        .fault_mode = s_ota.fault_mode,
    };
    return ota_transport_download_delta_to_staging(&s_ota.plan, &config);
}

static esp_err_t ota_service_download_to_staging(void)
{
    if (!s_ota.plan_valid)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_ota.cancel_requested = false;
    ota_service_publish_state(OTA_SERVICE_STATE_DOWNLOADING, ESP_OK);
    ota_metrics_stage_begin(OTA_METRICS_STAGE_DOWNLOAD);
    const ota_transport_download_config_t config = {
        .root_ca_pem = s_ota.plan.root_ca_pem,
        .use_cert_bundle = s_ota.plan.use_cert_bundle,
        .authorization = s_ota.plan.authorization[0] == '\0'
                             ? NULL
                             : s_ota.plan.authorization,
        .progress_cb = ota_service_progress_cb,
        .cancel_cb = ota_service_cancel_cb,
        .user_ctx = NULL,
        .fault_mode = s_ota.fault_mode,
    };

    esp_err_t ret = ESP_OK;
    bool used_delta = false;
    uint8_t delta_retry = 0U;
#if CONFIG_OTA_DELTA_ENABLED
    if (s_ota.plan.has_delta)
    {
        used_delta = true;
        for (uint32_t attempt = 0U;
             attempt <= (uint32_t)CONFIG_OTA_DELTA_MAX_RETRY; ++attempt)
        {
            ret = ota_service_download_delta_to_staging();
            if (ret == ESP_OK || s_ota.cancel_requested)
            {
                break;
            }
            delta_retry = (uint8_t)attempt + 1U;
            ESP_LOGW(TAG, "delta attempt %u failed: %s, retry or fallback",
                     attempt + 1U, esp_err_to_name(ret));
        }
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "delta exhausted, falling back to full image");
            /* plan 保持原样，fallback 只改变本次执行路径。 */
            ret = ota_transport_download_to_staging(&s_ota.plan, &config);
        }
    }
    else
#endif
    {
        ret = ota_transport_download_to_staging(&s_ota.plan, &config);
    }

    const uint32_t download_ms =
        ota_metrics_stage_elapsed_ms(OTA_METRICS_STAGE_DOWNLOAD);
    if (ret != ESP_OK)
    {
        if (!s_ota.cancel_requested)
        {
            const esp_err_t report_ret =
                ota_provider_report_status(&s_ota.plan,
                                            OTA_PROVIDER_STATUS_DOWNLOAD_FAILURE);
            if (report_ret != ESP_OK)
            {
                ESP_LOGW(TAG, "provider download failure report failed: %s",
                         esp_err_to_name(report_ret));
            }
        }
        ota_metrics_record_result(OTA_METRICS_STAGE_DOWNLOAD,
                                  s_ota.plan.version, ret, download_ms,
                                  used_delta, delta_retry);
        ota_service_cleanup_maintenance();
        ota_service_publish_state(
            s_ota.cancel_requested ? OTA_SERVICE_STATE_IDLE
                                    : OTA_SERVICE_STATE_FAILED,
            ret);
        return ret;
    }

    s_ota.staged = true;
    ota_metrics_record_result(OTA_METRICS_STAGE_DOWNLOAD,
                              s_ota.plan.version, ESP_OK, download_ms,
                              used_delta, delta_retry);
    ota_service_publish_state(OTA_SERVICE_STATE_STAGED, ESP_OK);
    return ESP_OK;
}

static esp_err_t ota_service_activate_staging(void)
{
    if (!s_ota.staged || !ota_transport_has_staged_image())
    {
        ota_service_publish_state(OTA_SERVICE_STATE_FAILED, ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }

    ota_service_publish_state(OTA_SERVICE_STATE_VERIFYING, ESP_OK);
    ota_metrics_stage_begin(OTA_METRICS_STAGE_ACTIVATE);
    bool pending_stored = false;
    esp_err_t ret = ota_provider_store_pending(&s_ota.plan);
    if (ret == ESP_OK)
    {
        pending_stored = true;
        ret = ota_transport_activate_staging();
    }
    s_ota.staged = false;
    if (ret != ESP_OK)
    {
        const esp_err_t report_ret =
            ota_provider_report_status(&s_ota.plan,
                                        OTA_PROVIDER_STATUS_ACTIVATE_FAILURE);
        if (report_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "provider activation failure report failed: %s",
                     esp_err_to_name(report_ret));
        }
        if (pending_stored)
        {
            const esp_err_t clear_ret = ota_provider_clear_pending();
            if (clear_ret != ESP_OK)
            {
                ESP_LOGW(TAG, "provider pending cleanup failed: %s",
                         esp_err_to_name(clear_ret));
            }
        }
        ota_metrics_record_result(OTA_METRICS_STAGE_ACTIVATE,
                                  s_ota.plan.version, ret,
                                  ota_metrics_stage_elapsed_ms(
                                      OTA_METRICS_STAGE_ACTIVATE),
                                  s_ota.plan.has_delta, 0U);
        ota_service_cleanup_maintenance();
        ota_service_publish_state(OTA_SERVICE_STATE_FAILED, ret);
        return ret;
    }

    ota_metrics_record_result(OTA_METRICS_STAGE_ACTIVATE, s_ota.plan.version,
                              ESP_OK,
                              ota_metrics_stage_elapsed_ms(
                                  OTA_METRICS_STAGE_ACTIVATE),
                              s_ota.plan.has_delta, 0U);
    ota_service_publish_state(OTA_SERVICE_STATE_RESTARTING, ESP_OK);
    ESP_LOGW(TAG, "fault_window: finish_succeeded_before_restart hold_ms=%u",
             (unsigned int)s_ota.restart_hold_ms);
    if (s_ota.restart_hold_ms > 0U)
    {
        vTaskDelay(pdMS_TO_TICKS(s_ota.restart_hold_ms));
    }
    esp_restart();
    return ESP_FAIL;
}

static esp_err_t ota_service_send_coordinator_command(
    ota_service_command_t type, uint32_t generation, esp_err_t result)
{
    if (s_ota.queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const ota_service_command_message_t command = {
        .type = type,
        .generation = generation,
        .result = result,
    };
    return xQueueSend(s_ota.queue, &command, 0) == pdTRUE ? ESP_OK
                                                           : ESP_ERR_TIMEOUT;
}

static esp_err_t ota_service_coordinator_grant(uint32_t generation,
                                                void *user_ctx)
{
    (void)user_ctx;
    return ota_service_send_coordinator_command(
        OTA_SERVICE_COMMAND_COORDINATOR_GRANTED, generation, ESP_OK);
}

static esp_err_t ota_service_coordinator_cancel(uint32_t generation,
                                                 esp_err_t reason,
                                                 void *user_ctx)
{
    (void)user_ctx;
    return ota_service_send_coordinator_command(
        OTA_SERVICE_COMMAND_COORDINATOR_CANCELLED, generation, reason);
}

static esp_err_t ota_service_coordinator_quiesce(uint32_t generation,
                                                  void *user_ctx)
{
    (void)user_ctx;
    ota_service_state_t state = OTA_SERVICE_STATE_IDLE;
    taskENTER_CRITICAL(&s_ota.lock);
    state = s_ota.snapshot.state;
    taskEXIT_CRITICAL(&s_ota.lock);
    if (state == OTA_SERVICE_STATE_DOWNLOADING ||
        state == OTA_SERVICE_STATE_VERIFYING ||
        state == OTA_SERVICE_STATE_RESTARTING)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return ota_service_send_coordinator_command(
        OTA_SERVICE_COMMAND_COORDINATOR_QUIESCE, generation, ESP_OK);
}

static void ota_service_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "wait: ui_first_frame_ready");
    (void)startup_readiness_wait_ui_first_frame(portMAX_DELAY);
    ESP_LOGI(TAG, "ready: ui_first_frame_ready");

    if (ota_metrics_init() == ESP_OK)
    {
        (void)ota_metrics_dump_recent();
    }
    ota_provider_report_pending();

    while (true)
    {
        ota_service_command_message_t command = {
            .type = OTA_SERVICE_COMMAND_CANCEL,
        };
        if (xQueueReceive(s_ota.queue, &command, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        if (command.type == OTA_SERVICE_COMMAND_PREPARE ||
            command.type == OTA_SERVICE_COMMAND_START_DOWNLOAD)
        {
            if (s_ota.coordinator_granted ||
                s_ota.coordinator_request_generation != 0U)
            {
                ESP_LOGW(TAG, "prepare ignored: maintenance already active");
                continue;
            }
            if (command.type == OTA_SERVICE_COMMAND_START_DOWNLOAD &&
                !s_ota.plan_valid)
            {
                ota_service_publish_state(OTA_SERVICE_STATE_FAILED,
                                          ESP_ERR_INVALID_STATE);
                continue;
            }
            s_ota.start_after_grant =
                command.type == OTA_SERVICE_COMMAND_START_DOWNLOAD;
            uint32_t request_generation = 0U;
            const esp_err_t request_ret = runtime_coordinator_request_foreground(
                RUNTIME_COORDINATOR_PARTICIPANT_OTA, &request_generation);
            if (request_ret != ESP_OK)
            {
                ota_service_publish_state(OTA_SERVICE_STATE_FAILED,
                                          request_ret);
                continue;
            }
            s_ota.coordinator_request_generation = request_generation;
            ota_service_publish_state(OTA_SERVICE_STATE_QUIESCING, ESP_OK);
        }
        else if (command.type == OTA_SERVICE_COMMAND_COORDINATOR_GRANTED)
        {
            if (command.generation != s_ota.coordinator_request_generation)
            {
                (void)runtime_coordinator_report_start_result(
                    RUNTIME_COORDINATOR_PARTICIPANT_OTA, command.generation,
                    ESP_ERR_INVALID_STATE);
                continue;
            }
            s_ota.coordinator_granted = true;
            const esp_err_t prepare_ret = ota_service_prepare();
            (void)runtime_coordinator_report_start_result(
                RUNTIME_COORDINATOR_PARTICIPANT_OTA, command.generation,
                prepare_ret);
            if (prepare_ret == ESP_OK && s_ota.start_after_grant)
            {
                (void)ota_service_download_to_staging();
                if (!s_ota.staged)
                {
                    (void)runtime_coordinator_cancel_request(
                        RUNTIME_COORDINATOR_PARTICIPANT_OTA,
                        s_ota.coordinator_request_generation);
                }
            }
        }
        else if (command.type == OTA_SERVICE_COMMAND_COORDINATOR_QUIESCE)
        {
            s_ota.coordinator_quiesce_generation = command.generation;
            ota_service_cleanup_maintenance();
            s_ota.coordinator_granted = false;
            s_ota.coordinator_request_generation = 0U;
            s_ota.start_after_grant = false;
            ota_service_publish_state(OTA_SERVICE_STATE_IDLE, ESP_OK);
            (void)runtime_coordinator_report_quiesce_result(
                RUNTIME_COORDINATOR_PARTICIPANT_OTA,
                s_ota.coordinator_quiesce_generation, ESP_OK);
            s_ota.coordinator_quiesce_generation = 0U;
        }
        else if (command.type == OTA_SERVICE_COMMAND_COORDINATOR_CANCELLED)
        {
            if (command.generation == s_ota.coordinator_request_generation)
            {
                s_ota.coordinator_request_generation = 0U;
                s_ota.start_after_grant = false;
                ota_service_publish_state(OTA_SERVICE_STATE_IDLE,
                                          command.result);
            }
        }
        else if (command.type == OTA_SERVICE_COMMAND_CHECK_UPDATE)
        {
            (void)ota_service_check_update();
        }
        else if (command.type == OTA_SERVICE_COMMAND_ACTIVATE)
        {
            (void)ota_service_activate_staging();
        }
        else if (command.type == OTA_SERVICE_COMMAND_CANCEL)
        {
            if (s_ota.coordinator_request_generation != 0U)
            {
                (void)runtime_coordinator_cancel_request(
                    RUNTIME_COORDINATOR_PARTICIPANT_OTA,
                    s_ota.coordinator_request_generation);
            }
            else if (s_ota.coordinator_granted ||
                     s_ota.power_maintenance_active)
            {
                ota_service_cleanup_maintenance();
                s_ota.coordinator_granted = false;
                ota_service_publish_state(OTA_SERVICE_STATE_IDLE, ESP_OK);
            }
        }
    }
}

esp_err_t ota_service_start(void)
{
    taskENTER_CRITICAL(&s_ota.lock);
    if (s_ota.initialized)
    {
        taskEXIT_CRITICAL(&s_ota.lock);
        return ESP_OK;
    }
    taskEXIT_CRITICAL(&s_ota.lock);

    if (startup_readiness_init() != ESP_OK)
    {
        return ESP_FAIL;
    }
    s_ota.queue = xQueueCreateStatic(
        kCommandQueueLength, sizeof(ota_service_command_message_t),
        s_ota.queue_storage, &s_ota.queue_buffer);
    if (s_ota.queue == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    const runtime_coordinator_participant_config_t participant = {
        .id = RUNTIME_COORDINATOR_PARTICIPANT_OTA,
        .name = "ota",
        .capabilities = RUNTIME_COORDINATOR_CAPABILITY_FOREGROUND_EXCLUSIVE,
        .request_quiesce = ota_service_coordinator_quiesce,
        .grant_foreground = ota_service_coordinator_grant,
        .cancel_pending_request = ota_service_coordinator_cancel,
    };
    const esp_err_t register_ret = runtime_coordinator_register(&participant);
    if (register_ret != ESP_OK)
    {
        return register_ret;
    }

    const BaseType_t task_ret = xTaskCreateWithCaps(
        ota_service_task, "ota_service", kTaskStackBytes, NULL, 5,
        &s_ota.task, MALLOC_CAP_INTERNAL);
    if (task_ret != pdPASS)
    {
        s_ota.queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    taskENTER_CRITICAL(&s_ota.lock);
    s_ota.initialized = true;
    s_ota.snapshot.state = OTA_SERVICE_STATE_IDLE;
    s_ota.snapshot.last_error = ESP_OK;
    s_ota.snapshot.task_started = true;
    taskEXIT_CRITICAL(&s_ota.lock);
#if CONFIG_OTA_SERVICE_TEST_FAULT_MODE > 0
    s_ota.fault_mode = (ota_transport_fault_mode_t)
        CONFIG_OTA_SERVICE_TEST_FAULT_MODE;
#endif
#if CONFIG_OTA_SERVICE_TEST_RESTART_HOLD_MS > 0
    s_ota.restart_hold_ms = CONFIG_OTA_SERVICE_TEST_RESTART_HOLD_MS;
#endif
    ESP_LOGI(TAG, "started");
    return ESP_OK;
}

static esp_err_t ota_service_send_command(ota_service_command_t command)
{
    if (s_ota.queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const ota_service_command_message_t message = {.type = command};
    return xQueueSend(s_ota.queue, &message, 0) == pdTRUE ? ESP_OK
                                                           : ESP_ERR_TIMEOUT;
}

esp_err_t ota_service_request_prepare(void)
{
    return ota_service_send_command(OTA_SERVICE_COMMAND_PREPARE);
}

esp_err_t ota_service_request_check(void)
{
    return ota_service_send_command(OTA_SERVICE_COMMAND_CHECK_UPDATE);
}

esp_err_t ota_service_request_download(void)
{
    return ota_service_send_command(OTA_SERVICE_COMMAND_START_DOWNLOAD);
}

esp_err_t ota_service_request_activate(void)
{
    return ota_service_send_command(OTA_SERVICE_COMMAND_ACTIVATE);
}

esp_err_t ota_service_set_fault_mode(ota_transport_fault_mode_t mode)
{
    if (mode > OTA_TRANSPORT_FAULT_ABORT_AT_90_PERCENT ||
        s_ota.coordinator_granted)
    {
        return ESP_ERR_INVALID_ARG;
    }
    s_ota.fault_mode = mode;
    return ESP_OK;
}

esp_err_t ota_service_set_restart_hold_ms(uint32_t hold_ms)
{
    if (hold_ms > 30000U || s_ota.coordinator_granted)
    {
        return ESP_ERR_INVALID_ARG;
    }
    s_ota.restart_hold_ms = hold_ms;
    return ESP_OK;
}

esp_err_t ota_service_request_cancel(void)
{
    return ota_service_send_command(OTA_SERVICE_COMMAND_CANCEL);
}

esp_err_t ota_service_get_snapshot(ota_service_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_ota.lock);
    *out_snapshot = s_ota.snapshot;
    out_snapshot->maintenance_active = s_ota.coordinator_granted;
    taskEXIT_CRITICAL(&s_ota.lock);
    return ESP_OK;
}
