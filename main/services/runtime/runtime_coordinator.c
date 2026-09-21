#include "services/runtime/runtime_coordinator.h"

#include <limits.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

enum
{
    RUNTIME_COORDINATOR_QUEUE_LENGTH = 16,
    RUNTIME_COORDINATOR_TASK_STACK_BYTES = 4096,
    RUNTIME_COORDINATOR_TASK_PRIORITY = 5,
};

static const uint32_t kForegroundTimeoutMs = 5000U;
static const uint32_t kBackgroundTimeoutMs = 2500U;
static const char *TAG = "runtime_coord";

typedef enum
{
    RUNTIME_COORDINATOR_COMMAND_REQUEST = 0,
    RUNTIME_COORDINATOR_COMMAND_CANCEL,
    RUNTIME_COORDINATOR_COMMAND_QUIESCE_RESULT,
    RUNTIME_COORDINATOR_COMMAND_START_RESULT,
    RUNTIME_COORDINATOR_COMMAND_REPORT_ACTIVE,
} runtime_coordinator_command_type_t;

typedef struct
{
    runtime_coordinator_command_type_t type;
    runtime_coordinator_participant_id_t id;
    uint32_t generation;
    esp_err_t result;
} runtime_coordinator_command_t;

typedef struct
{
    bool initialized;
    bool started;
    runtime_coordinator_participant_config_t
        participants[RUNTIME_COORDINATOR_PARTICIPANT_COUNT];
    bool registered[RUNTIME_COORDINATOR_PARTICIPANT_COUNT];
    runtime_coordinator_snapshot_t snapshot;
    runtime_coordinator_snapshot_t published_snapshot;
    uint32_t request_counter;
    uint32_t transition_counter;
    uint32_t provisional_request_generation;
    uint32_t background_waiting_mask;
    uint32_t foreground_waiting_mask;
    uint32_t quiesced_background_mask;
    TickType_t background_deadline;
    TickType_t foreground_deadline;
    QueueHandle_t queue;
    TaskHandle_t task_handle;
    StaticQueue_t queue_buffer;
    uint8_t queue_storage[RUNTIME_COORDINATOR_QUEUE_LENGTH *
                          sizeof(runtime_coordinator_command_t)];
    portMUX_TYPE lock;
} runtime_coordinator_context_t;

static runtime_coordinator_context_t s_coordinator = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static uint32_t runtime_coordinator_bit(runtime_coordinator_participant_id_t id)
{
    return (id > RUNTIME_COORDINATOR_PARTICIPANT_NONE &&
            id < RUNTIME_COORDINATOR_PARTICIPANT_COUNT)
               ? (1U << (uint32_t)id)
               : 0U;
}

static bool runtime_coordinator_is_registered(
    runtime_coordinator_participant_id_t id)
{
    return id > RUNTIME_COORDINATOR_PARTICIPANT_NONE &&
           id < RUNTIME_COORDINATOR_PARTICIPANT_COUNT &&
           s_coordinator.registered[id];
}

static uint32_t runtime_coordinator_next_generation(uint32_t *counter)
{
    (*counter)++;
    if (*counter == 0U)
    {
        *counter = 1U;
    }
    return *counter;
}

static void runtime_coordinator_store_snapshot(void)
{
    s_coordinator.snapshot.waiting_mask =
        s_coordinator.background_waiting_mask |
        s_coordinator.foreground_waiting_mask;
    taskENTER_CRITICAL(&s_coordinator.lock);
    s_coordinator.published_snapshot = s_coordinator.snapshot;
    taskEXIT_CRITICAL(&s_coordinator.lock);
}

static void runtime_coordinator_set_failure(
    runtime_coordinator_participant_id_t id, esp_err_t error)
{
    s_coordinator.snapshot.error_participant = id;
    s_coordinator.snapshot.last_error = error;
}

static void runtime_coordinator_cancel_target(esp_err_t reason)
{
    const runtime_coordinator_participant_id_t target =
        s_coordinator.snapshot.target_owner;
    const uint32_t request_generation =
        s_coordinator.snapshot.request_generation;

    if (runtime_coordinator_is_registered(target))
    {
        const runtime_coordinator_participant_config_t *participant =
            &s_coordinator.participants[target];
        if (participant->cancel_pending_request != NULL)
        {
            (void)participant->cancel_pending_request(
                request_generation, reason, participant->user_ctx);
        }
    }
    s_coordinator.snapshot.target_owner =
        RUNTIME_COORDINATOR_PARTICIPANT_NONE;
}

static void runtime_coordinator_reevaluate_background(void)
{
    const uint32_t generation = s_coordinator.snapshot.transition_generation;
    const uint32_t mask = s_coordinator.quiesced_background_mask;
    s_coordinator.quiesced_background_mask = 0U;

    for (int id = RUNTIME_COORDINATOR_PARTICIPANT_NONE + 1;
         id < RUNTIME_COORDINATOR_PARTICIPANT_COUNT; ++id)
    {
        if ((mask & runtime_coordinator_bit(id)) == 0U ||
            !runtime_coordinator_is_registered(id))
        {
            continue;
        }
        const runtime_coordinator_participant_config_t *participant =
            &s_coordinator.participants[id];
        if (participant->request_reevaluate != NULL)
        {
            (void)participant->request_reevaluate(
                generation, participant->user_ctx);
        }
    }
}

static void runtime_coordinator_enter_idle(void)
{
    s_coordinator.snapshot.state = RUNTIME_COORDINATOR_STATE_IDLE;
    s_coordinator.snapshot.current_owner =
        RUNTIME_COORDINATOR_PARTICIPANT_NONE;
    s_coordinator.snapshot.provisional_owner =
        RUNTIME_COORDINATOR_PARTICIPANT_NONE;
    s_coordinator.provisional_request_generation = 0U;
    s_coordinator.snapshot.active_request_generation = 0U;
    s_coordinator.background_waiting_mask = 0U;
    s_coordinator.foreground_waiting_mask = 0U;
    runtime_coordinator_store_snapshot();
    runtime_coordinator_reevaluate_background();
}

static void runtime_coordinator_grant_target(void)
{
    const runtime_coordinator_participant_id_t target =
        s_coordinator.snapshot.target_owner;
    if (target == RUNTIME_COORDINATOR_PARTICIPANT_NONE)
    {
        runtime_coordinator_enter_idle();
        return;
    }

    const runtime_coordinator_participant_config_t *participant =
        &s_coordinator.participants[target];
    const esp_err_t ret = participant->grant_foreground(
        s_coordinator.snapshot.request_generation, participant->user_ctx);
    if (ret != ESP_OK)
    {
        runtime_coordinator_set_failure(target, ret);
        runtime_coordinator_cancel_target(ret);
        runtime_coordinator_enter_idle();
        return;
    }

    s_coordinator.snapshot.provisional_owner = target;
    s_coordinator.provisional_request_generation =
        s_coordinator.snapshot.request_generation;
    s_coordinator.snapshot.state = RUNTIME_COORDINATOR_STATE_GRANTING;
    s_coordinator.foreground_deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS(kForegroundTimeoutMs);
    runtime_coordinator_store_snapshot();
    ESP_LOGI(TAG, "grant: target=%s request=%u",
             runtime_coordinator_participant_text(target),
             (unsigned)s_coordinator.snapshot.request_generation);
}

static void runtime_coordinator_finish_quiesce_if_ready(void)
{
    if (s_coordinator.background_waiting_mask != 0U ||
        s_coordinator.foreground_waiting_mask != 0U)
    {
        runtime_coordinator_store_snapshot();
        return;
    }

    if (s_coordinator.snapshot.state == RUNTIME_COORDINATOR_STATE_ROLLING_BACK)
    {
        const bool has_newer_target =
            s_coordinator.snapshot.target_owner !=
                RUNTIME_COORDINATOR_PARTICIPANT_NONE &&
            (s_coordinator.snapshot.target_owner !=
                 s_coordinator.snapshot.provisional_owner ||
             s_coordinator.snapshot.request_generation !=
                 s_coordinator.provisional_request_generation);
        s_coordinator.snapshot.provisional_owner =
            RUNTIME_COORDINATOR_PARTICIPANT_NONE;
        s_coordinator.provisional_request_generation = 0U;
        if (has_newer_target)
        {
            runtime_coordinator_grant_target();
        }
        else
        {
            runtime_coordinator_cancel_target(ESP_ERR_TIMEOUT);
            runtime_coordinator_enter_idle();
        }
        return;
    }
    runtime_coordinator_grant_target();
}

static esp_err_t runtime_coordinator_request_participant_quiesce(
    runtime_coordinator_participant_id_t id, uint32_t generation)
{
    const runtime_coordinator_participant_config_t *participant =
        &s_coordinator.participants[id];
    return participant->request_quiesce(generation, participant->user_ctx);
}

static void runtime_coordinator_fail_quiesce(
    runtime_coordinator_participant_id_t id, esp_err_t error, bool degraded)
{
    const bool background_failure =
        (s_coordinator.background_waiting_mask &
         runtime_coordinator_bit(id)) != 0U;
    runtime_coordinator_set_failure(id, error);
    runtime_coordinator_cancel_target(error);

    if (degraded)
    {
        s_coordinator.snapshot.state =
            s_coordinator.snapshot.provisional_owner == id
                ? RUNTIME_COORDINATOR_STATE_DEGRADED_TARGET
                : RUNTIME_COORDINATOR_STATE_DEGRADED_CURRENT;
        runtime_coordinator_store_snapshot();
        return;
    }

    if (background_failure &&
        s_coordinator.foreground_waiting_mask != 0U)
    {
        /* 旧 owner 已接受 stop 时继续等它的事实 ACK，再决定 IDLE/ACTIVE。 */
        s_coordinator.background_waiting_mask = 0U;
        s_coordinator.snapshot.state = RUNTIME_COORDINATOR_STATE_QUIESCING;
        runtime_coordinator_store_snapshot();
        return;
    }

    if (s_coordinator.snapshot.current_owner !=
        RUNTIME_COORDINATOR_PARTICIPANT_NONE)
    {
        s_coordinator.snapshot.state = RUNTIME_COORDINATOR_STATE_ACTIVE;
        s_coordinator.background_waiting_mask = 0U;
        s_coordinator.foreground_waiting_mask = 0U;
        runtime_coordinator_store_snapshot();
        return;
    }
    runtime_coordinator_enter_idle();
}

static void runtime_coordinator_begin_quiesce(bool rollback)
{
    const uint32_t generation = runtime_coordinator_next_generation(
        &s_coordinator.transition_counter);
    const TickType_t now = xTaskGetTickCount();
    s_coordinator.snapshot.transition_generation = generation;
    s_coordinator.snapshot.state = rollback
                                       ? RUNTIME_COORDINATOR_STATE_ROLLING_BACK
                                       : RUNTIME_COORDINATOR_STATE_QUIESCING;
    s_coordinator.background_waiting_mask = 0U;
    s_coordinator.foreground_waiting_mask = 0U;

    runtime_coordinator_participant_id_t foreground =
        rollback ? s_coordinator.snapshot.provisional_owner
                 : s_coordinator.snapshot.current_owner;
    if (foreground != RUNTIME_COORDINATOR_PARTICIPANT_NONE)
    {
        const esp_err_t ret = runtime_coordinator_request_participant_quiesce(
            foreground, generation);
        if (ret != ESP_OK)
        {
            runtime_coordinator_fail_quiesce(foreground, ret, rollback);
            return;
        }
        s_coordinator.foreground_waiting_mask |=
            runtime_coordinator_bit(foreground);
        s_coordinator.foreground_deadline =
            now + pdMS_TO_TICKS(kForegroundTimeoutMs);
    }

    if (!rollback)
    {
        for (int id = RUNTIME_COORDINATOR_PARTICIPANT_NONE + 1;
             id < RUNTIME_COORDINATOR_PARTICIPANT_COUNT; ++id)
        {
            if (!runtime_coordinator_is_registered(id) || id == foreground)
            {
                continue;
            }
            const runtime_coordinator_participant_config_t *participant =
                &s_coordinator.participants[id];
            if ((participant->capabilities &
                 RUNTIME_COORDINATOR_CAPABILITY_BACKGROUND_PREEMPTIBLE) == 0U)
            {
                continue;
            }
            if ((s_coordinator.quiesced_background_mask &
                 runtime_coordinator_bit(id)) != 0U)
            {
                continue;
            }
            const esp_err_t ret = runtime_coordinator_request_participant_quiesce(
                id, generation);
            if (ret != ESP_OK)
            {
                runtime_coordinator_fail_quiesce(id, ret, false);
                return;
            }
            const uint32_t bit = runtime_coordinator_bit(id);
            s_coordinator.background_waiting_mask |= bit;
            s_coordinator.quiesced_background_mask |= bit;
        }
        s_coordinator.background_deadline =
            now + pdMS_TO_TICKS(kBackgroundTimeoutMs);
    }

    runtime_coordinator_store_snapshot();
    runtime_coordinator_finish_quiesce_if_ready();
}

static void runtime_coordinator_handle_request(
    const runtime_coordinator_command_t *command)
{
    if (s_coordinator.snapshot.state ==
            RUNTIME_COORDINATOR_STATE_DEGRADED_CURRENT ||
        s_coordinator.snapshot.state ==
            RUNTIME_COORDINATOR_STATE_DEGRADED_TARGET)
    {
        const runtime_coordinator_participant_config_t *participant =
            &s_coordinator.participants[command->id];
        if (participant->cancel_pending_request != NULL)
        {
            (void)participant->cancel_pending_request(
                command->generation, ESP_ERR_INVALID_STATE,
                participant->user_ctx);
        }
        return;
    }

    if (s_coordinator.snapshot.target_owner !=
        RUNTIME_COORDINATOR_PARTICIPANT_NONE)
    {
        runtime_coordinator_cancel_target(ESP_ERR_INVALID_STATE);
    }
    s_coordinator.snapshot.target_owner = command->id;
    s_coordinator.snapshot.request_generation = command->generation;
    s_coordinator.snapshot.last_error = ESP_OK;
    s_coordinator.snapshot.error_participant =
        RUNTIME_COORDINATOR_PARTICIPANT_NONE;

    if (s_coordinator.snapshot.state == RUNTIME_COORDINATOR_STATE_QUIESCING)
    {
        runtime_coordinator_store_snapshot();
        return;
    }
    if (s_coordinator.snapshot.state == RUNTIME_COORDINATOR_STATE_GRANTING)
    {
        runtime_coordinator_begin_quiesce(true);
        return;
    }
    if (s_coordinator.snapshot.state == RUNTIME_COORDINATOR_STATE_ROLLING_BACK)
    {
        runtime_coordinator_store_snapshot();
        return;
    }
    runtime_coordinator_begin_quiesce(false);
}

static void runtime_coordinator_handle_cancel(
    const runtime_coordinator_command_t *command)
{
    if (s_coordinator.snapshot.target_owner == command->id &&
        s_coordinator.snapshot.request_generation == command->generation)
    {
        runtime_coordinator_cancel_target(ESP_ERR_INVALID_STATE);
        if (s_coordinator.snapshot.state == RUNTIME_COORDINATOR_STATE_GRANTING)
        {
            runtime_coordinator_begin_quiesce(true);
        }
        else
        {
            runtime_coordinator_finish_quiesce_if_ready();
        }
        return;
    }

    if (s_coordinator.snapshot.current_owner == command->id &&
        s_coordinator.snapshot.active_request_generation == command->generation)
    {
        s_coordinator.snapshot.target_owner =
            RUNTIME_COORDINATOR_PARTICIPANT_NONE;
        runtime_coordinator_begin_quiesce(false);
    }
}

static void runtime_coordinator_handle_quiesce_result(
    const runtime_coordinator_command_t *command)
{
    if (command->generation != s_coordinator.snapshot.transition_generation)
    {
        return;
    }

    const uint32_t bit = runtime_coordinator_bit(command->id);
    const bool foreground_waiting =
        (s_coordinator.foreground_waiting_mask & bit) != 0U;
    const bool background_waiting =
        (s_coordinator.background_waiting_mask & bit) != 0U;
    if (!foreground_waiting && !background_waiting)
    {
        return;
    }

    if (command->result != ESP_OK)
    {
        runtime_coordinator_fail_quiesce(
            command->id, command->result,
            s_coordinator.snapshot.state ==
                RUNTIME_COORDINATOR_STATE_ROLLING_BACK);
        return;
    }

    s_coordinator.foreground_waiting_mask &= ~bit;
    s_coordinator.background_waiting_mask &= ~bit;
    if (foreground_waiting)
    {
        if (s_coordinator.snapshot.current_owner == command->id)
        {
            s_coordinator.snapshot.current_owner =
                RUNTIME_COORDINATOR_PARTICIPANT_NONE;
            s_coordinator.snapshot.active_request_generation = 0U;
        }
        if (s_coordinator.snapshot.provisional_owner == command->id &&
            s_coordinator.snapshot.state !=
                RUNTIME_COORDINATOR_STATE_ROLLING_BACK)
        {
            s_coordinator.snapshot.provisional_owner =
                RUNTIME_COORDINATOR_PARTICIPANT_NONE;
            s_coordinator.provisional_request_generation = 0U;
        }
    }
    runtime_coordinator_finish_quiesce_if_ready();
}

static void runtime_coordinator_handle_start_result(
    const runtime_coordinator_command_t *command)
{
    if (s_coordinator.snapshot.state != RUNTIME_COORDINATOR_STATE_GRANTING ||
        command->id != s_coordinator.snapshot.provisional_owner ||
        command->generation != s_coordinator.provisional_request_generation)
    {
        return;
    }

    if (command->result != ESP_OK)
    {
        runtime_coordinator_set_failure(command->id, command->result);
        runtime_coordinator_cancel_target(command->result);
        s_coordinator.snapshot.provisional_owner =
            RUNTIME_COORDINATOR_PARTICIPANT_NONE;
        s_coordinator.provisional_request_generation = 0U;
        runtime_coordinator_enter_idle();
        return;
    }

    s_coordinator.snapshot.current_owner = command->id;
    s_coordinator.snapshot.active_request_generation = command->generation;
    s_coordinator.snapshot.provisional_owner =
        RUNTIME_COORDINATOR_PARTICIPANT_NONE;
    s_coordinator.provisional_request_generation = 0U;
    s_coordinator.snapshot.target_owner =
        RUNTIME_COORDINATOR_PARTICIPANT_NONE;
    s_coordinator.snapshot.state = RUNTIME_COORDINATOR_STATE_ACTIVE;
    runtime_coordinator_store_snapshot();
    ESP_LOGI(TAG, "active: owner=%s request=%u",
             runtime_coordinator_participant_text(command->id),
             (unsigned)command->generation);
}

static void runtime_coordinator_handle_report_active(
    const runtime_coordinator_command_t *command)
{
    if (s_coordinator.snapshot.state !=
            RUNTIME_COORDINATOR_STATE_DEGRADED_CURRENT &&
        s_coordinator.snapshot.state !=
            RUNTIME_COORDINATOR_STATE_DEGRADED_TARGET)
    {
        return;
    }
    const runtime_coordinator_participant_id_t expected =
        s_coordinator.snapshot.state ==
                RUNTIME_COORDINATOR_STATE_DEGRADED_TARGET
            ? s_coordinator.snapshot.provisional_owner
            : s_coordinator.snapshot.current_owner;
    if (command->id != expected)
    {
        return;
    }
    s_coordinator.snapshot.current_owner = command->id;
    if (s_coordinator.snapshot.state ==
        RUNTIME_COORDINATOR_STATE_DEGRADED_TARGET)
    {
        s_coordinator.snapshot.active_request_generation =
            s_coordinator.provisional_request_generation;
    }
    s_coordinator.snapshot.provisional_owner =
        RUNTIME_COORDINATOR_PARTICIPANT_NONE;
    s_coordinator.provisional_request_generation = 0U;
    s_coordinator.snapshot.state = RUNTIME_COORDINATOR_STATE_ACTIVE;
    s_coordinator.foreground_waiting_mask = 0U;
    s_coordinator.background_waiting_mask = 0U;
    runtime_coordinator_store_snapshot();
}

static void runtime_coordinator_handle_timeout(void)
{
    const TickType_t now = xTaskGetTickCount();
    if (s_coordinator.snapshot.state == RUNTIME_COORDINATOR_STATE_GRANTING &&
        (int32_t)(now - s_coordinator.foreground_deadline) >= 0)
    {
        runtime_coordinator_set_failure(
            s_coordinator.snapshot.provisional_owner, ESP_ERR_TIMEOUT);
        runtime_coordinator_begin_quiesce(true);
        return;
    }

    if ((s_coordinator.snapshot.state == RUNTIME_COORDINATOR_STATE_QUIESCING ||
         s_coordinator.snapshot.state ==
             RUNTIME_COORDINATOR_STATE_ROLLING_BACK) &&
        s_coordinator.foreground_waiting_mask != 0U &&
        (int32_t)(now - s_coordinator.foreground_deadline) >= 0)
    {
        runtime_coordinator_participant_id_t id =
            s_coordinator.snapshot.state ==
                    RUNTIME_COORDINATOR_STATE_ROLLING_BACK
                ? s_coordinator.snapshot.provisional_owner
                : s_coordinator.snapshot.current_owner;
        runtime_coordinator_fail_quiesce(id, ESP_ERR_TIMEOUT, true);
        return;
    }

    if (s_coordinator.snapshot.state == RUNTIME_COORDINATOR_STATE_QUIESCING &&
        s_coordinator.background_waiting_mask != 0U &&
        (int32_t)(now - s_coordinator.background_deadline) >= 0)
    {
        runtime_coordinator_participant_id_t id =
            RUNTIME_COORDINATOR_PARTICIPANT_NONE;
        for (int candidate = RUNTIME_COORDINATOR_PARTICIPANT_NONE + 1;
             candidate < RUNTIME_COORDINATOR_PARTICIPANT_COUNT; ++candidate)
        {
            if ((s_coordinator.background_waiting_mask &
                 runtime_coordinator_bit(candidate)) != 0U)
            {
                id = candidate;
                break;
            }
        }
        runtime_coordinator_fail_quiesce(id, ESP_ERR_TIMEOUT, false);
    }
}

static TickType_t runtime_coordinator_wait_ticks(void)
{
    const TickType_t now = xTaskGetTickCount();
    TickType_t deadline = 0U;

    if (s_coordinator.snapshot.state == RUNTIME_COORDINATOR_STATE_GRANTING ||
        ((s_coordinator.snapshot.state ==
              RUNTIME_COORDINATOR_STATE_QUIESCING ||
          s_coordinator.snapshot.state ==
              RUNTIME_COORDINATOR_STATE_ROLLING_BACK) &&
         s_coordinator.foreground_waiting_mask != 0U))
    {
        deadline = s_coordinator.foreground_deadline;
    }
    if (s_coordinator.snapshot.state == RUNTIME_COORDINATOR_STATE_QUIESCING &&
        s_coordinator.background_waiting_mask != 0U &&
        (deadline == 0U ||
         (int32_t)(s_coordinator.background_deadline - deadline) < 0))
    {
        deadline = s_coordinator.background_deadline;
    }
    if (deadline == 0U)
    {
        return portMAX_DELAY;
    }
    return (int32_t)(deadline - now) <= 0 ? 0U : deadline - now;
}

static void runtime_coordinator_task(void *arg)
{
    (void)arg;
    runtime_coordinator_command_t command;

    while (true)
    {
        const TickType_t wait_ticks = runtime_coordinator_wait_ticks();
        if (xQueueReceive(s_coordinator.queue, &command, wait_ticks) != pdTRUE)
        {
            runtime_coordinator_handle_timeout();
            continue;
        }

        switch (command.type)
        {
        case RUNTIME_COORDINATOR_COMMAND_REQUEST:
            runtime_coordinator_handle_request(&command);
            break;
        case RUNTIME_COORDINATOR_COMMAND_CANCEL:
            runtime_coordinator_handle_cancel(&command);
            break;
        case RUNTIME_COORDINATOR_COMMAND_QUIESCE_RESULT:
            runtime_coordinator_handle_quiesce_result(&command);
            break;
        case RUNTIME_COORDINATOR_COMMAND_START_RESULT:
            runtime_coordinator_handle_start_result(&command);
            break;
        case RUNTIME_COORDINATOR_COMMAND_REPORT_ACTIVE:
            runtime_coordinator_handle_report_active(&command);
            break;
        default:
            break;
        }
    }
}

static esp_err_t runtime_coordinator_send(
    const runtime_coordinator_command_t *command)
{
    if (command == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_coordinator.started || s_coordinator.queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(s_coordinator.queue, command, 0) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

esp_err_t runtime_coordinator_init(void)
{
    taskENTER_CRITICAL(&s_coordinator.lock);
    if (s_coordinator.initialized)
    {
        taskEXIT_CRITICAL(&s_coordinator.lock);
        return ESP_OK;
    }
    s_coordinator.queue = xQueueCreateStatic(
        RUNTIME_COORDINATOR_QUEUE_LENGTH,
        sizeof(runtime_coordinator_command_t),
        s_coordinator.queue_storage,
        &s_coordinator.queue_buffer);
    if (s_coordinator.queue == NULL)
    {
        taskEXIT_CRITICAL(&s_coordinator.lock);
        return ESP_ERR_NO_MEM;
    }
    s_coordinator.snapshot.state = RUNTIME_COORDINATOR_STATE_IDLE;
    s_coordinator.snapshot.last_error = ESP_OK;
    s_coordinator.published_snapshot = s_coordinator.snapshot;
    s_coordinator.initialized = true;
    taskEXIT_CRITICAL(&s_coordinator.lock);
    return ESP_OK;
}

esp_err_t runtime_coordinator_start(void)
{
    esp_err_t ret = runtime_coordinator_init();
    if (ret != ESP_OK)
    {
        return ret;
    }
    if (s_coordinator.started)
    {
        return ESP_OK;
    }

    const BaseType_t created = xTaskCreateWithCaps(
        runtime_coordinator_task, "runtime_coord",
        RUNTIME_COORDINATOR_TASK_STACK_BYTES, NULL,
        RUNTIME_COORDINATOR_TASK_PRIORITY,
        &s_coordinator.task_handle, MALLOC_CAP_INTERNAL);
    if (created != pdPASS)
    {
        s_coordinator.task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_coordinator.started = true;
    s_coordinator.snapshot.started = true;
    runtime_coordinator_store_snapshot();
    return ESP_OK;
}

esp_err_t runtime_coordinator_register(
    const runtime_coordinator_participant_config_t *config)
{
    if (config == NULL || config->id <= RUNTIME_COORDINATOR_PARTICIPANT_NONE ||
        config->id >= RUNTIME_COORDINATOR_PARTICIPANT_COUNT ||
        config->name == NULL || config->request_quiesce == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const bool foreground =
        (config->capabilities &
         RUNTIME_COORDINATOR_CAPABILITY_FOREGROUND_EXCLUSIVE) != 0U;
    const bool background =
        (config->capabilities &
         RUNTIME_COORDINATOR_CAPABILITY_BACKGROUND_PREEMPTIBLE) != 0U;
    if (foreground == background ||
        (foreground && (config->grant_foreground == NULL ||
                        config->cancel_pending_request == NULL)) ||
        (background && config->request_reevaluate == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_coordinator.lock);
    if (s_coordinator.registered[config->id])
    {
        taskEXIT_CRITICAL(&s_coordinator.lock);
        return ESP_OK;
    }
    s_coordinator.participants[config->id] = *config;
    s_coordinator.registered[config->id] = true;
    taskEXIT_CRITICAL(&s_coordinator.lock);
    return ESP_OK;
}

esp_err_t runtime_coordinator_request_foreground(
    runtime_coordinator_participant_id_t id,
    uint32_t *out_request_generation)
{
    if (out_request_generation == NULL ||
        !runtime_coordinator_is_registered(id) ||
        (s_coordinator.participants[id].capabilities &
         RUNTIME_COORDINATOR_CAPABILITY_FOREGROUND_EXCLUSIVE) == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_coordinator.lock);
    const uint32_t generation = runtime_coordinator_next_generation(
        &s_coordinator.request_counter);
    taskEXIT_CRITICAL(&s_coordinator.lock);
    *out_request_generation = generation;
    const runtime_coordinator_command_t command = {
        .type = RUNTIME_COORDINATOR_COMMAND_REQUEST,
        .id = id,
        .generation = generation,
        .result = ESP_OK,
    };
    const esp_err_t ret = runtime_coordinator_send(&command);
    if (ret != ESP_OK)
    {
        *out_request_generation = 0U;
    }
    return ret;
}

esp_err_t runtime_coordinator_cancel_request(
    runtime_coordinator_participant_id_t id,
    uint32_t request_generation)
{
    if (request_generation == 0U || !runtime_coordinator_is_registered(id) ||
        (s_coordinator.participants[id].capabilities &
         RUNTIME_COORDINATOR_CAPABILITY_FOREGROUND_EXCLUSIVE) == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const runtime_coordinator_command_t command = {
        .type = RUNTIME_COORDINATOR_COMMAND_CANCEL,
        .id = id,
        .generation = request_generation,
    };
    return runtime_coordinator_send(&command);
}

esp_err_t runtime_coordinator_report_quiesce_result(
    runtime_coordinator_participant_id_t id,
    uint32_t transition_generation,
    esp_err_t result)
{
    if (transition_generation == 0U ||
        !runtime_coordinator_is_registered(id))
    {
        return ESP_ERR_INVALID_ARG;
    }
    const runtime_coordinator_command_t command = {
        .type = RUNTIME_COORDINATOR_COMMAND_QUIESCE_RESULT,
        .id = id,
        .generation = transition_generation,
        .result = result,
    };
    return runtime_coordinator_send(&command);
}

esp_err_t runtime_coordinator_report_start_result(
    runtime_coordinator_participant_id_t id,
    uint32_t request_generation,
    esp_err_t result)
{
    if (request_generation == 0U || !runtime_coordinator_is_registered(id) ||
        (s_coordinator.participants[id].capabilities &
         RUNTIME_COORDINATOR_CAPABILITY_FOREGROUND_EXCLUSIVE) == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const runtime_coordinator_command_t command = {
        .type = RUNTIME_COORDINATOR_COMMAND_START_RESULT,
        .id = id,
        .generation = request_generation,
        .result = result,
    };
    return runtime_coordinator_send(&command);
}

esp_err_t runtime_coordinator_report_active(
    runtime_coordinator_participant_id_t id)
{
    if (!runtime_coordinator_is_registered(id) ||
        (s_coordinator.participants[id].capabilities &
         RUNTIME_COORDINATOR_CAPABILITY_FOREGROUND_EXCLUSIVE) == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const runtime_coordinator_command_t command = {
        .type = RUNTIME_COORDINATOR_COMMAND_REPORT_ACTIVE,
        .id = id,
    };
    return runtime_coordinator_send(&command);
}

runtime_coordinator_snapshot_t runtime_coordinator_get_snapshot(void)
{
    runtime_coordinator_snapshot_t snapshot;
    taskENTER_CRITICAL(&s_coordinator.lock);
    snapshot = s_coordinator.published_snapshot;
    taskEXIT_CRITICAL(&s_coordinator.lock);
    return snapshot;
}

const char *runtime_coordinator_participant_text(
    runtime_coordinator_participant_id_t id)
{
    switch (id)
    {
    case RUNTIME_COORDINATOR_PARTICIPANT_HERMES:
        return "HERMES";
    case RUNTIME_COORDINATOR_PARTICIPANT_OFFICIAL_CHAT:
        return "OFFICIAL_CHAT";
    case RUNTIME_COORDINATOR_PARTICIPANT_NETWORK_PROVISIONING:
        return "NETWORK_PROVISIONING";
    case RUNTIME_COORDINATOR_PARTICIPANT_OTA:
        return "OTA";
    case RUNTIME_COORDINATOR_PARTICIPANT_SAFETY_MONITOR:
        return "SAFETY_MONITOR";
    case RUNTIME_COORDINATOR_PARTICIPANT_BLE_PRESENCE:
        return "BLE_PRESENCE";
    case RUNTIME_COORDINATOR_PARTICIPANT_TEST_OWNER:
        return "TEST_OWNER";
    case RUNTIME_COORDINATOR_PARTICIPANT_TEST_BACKGROUND:
        return "TEST_BACKGROUND";
    case RUNTIME_COORDINATOR_PARTICIPANT_NONE:
    default:
        return "NONE";
    }
}

const char *runtime_coordinator_state_text(runtime_coordinator_state_t state)
{
    switch (state)
    {
    case RUNTIME_COORDINATOR_STATE_IDLE:
        return "IDLE";
    case RUNTIME_COORDINATOR_STATE_QUIESCING:
        return "QUIESCING";
    case RUNTIME_COORDINATOR_STATE_GRANTING:
        return "GRANTING";
    case RUNTIME_COORDINATOR_STATE_ACTIVE:
        return "ACTIVE";
    case RUNTIME_COORDINATOR_STATE_ROLLING_BACK:
        return "ROLLING_BACK";
    case RUNTIME_COORDINATOR_STATE_DEGRADED_CURRENT:
        return "DEGRADED_CURRENT";
    case RUNTIME_COORDINATOR_STATE_DEGRADED_TARGET:
        return "DEGRADED_TARGET";
    default:
        return "UNKNOWN";
    }
}
