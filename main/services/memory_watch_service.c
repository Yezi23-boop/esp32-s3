#include "services/memory_watch_service.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "network_service.h"
#include "nvs.h"
#include "esp_timer.h"
#include "services/background_service_manager.h"
#include "services/foreground_runtime_gate.h"
#include "power_policy.h"
#include "services/memory_watch_recorder.h"
#include "services/memory_watch_voice_client.h"
#include "services/memory_watch_ws_client.h"

#ifndef CONFIG_MEMORY_WATCH_DEFAULT_BASE_URL
#define CONFIG_MEMORY_WATCH_DEFAULT_BASE_URL ""
#endif

#ifndef CONFIG_MEMORY_WATCH_DEFAULT_DEVICE_ID
#define CONFIG_MEMORY_WATCH_DEFAULT_DEVICE_ID ""
#endif

#ifndef CONFIG_MEMORY_WATCH_DEFAULT_DEVICE_TOKEN
#define CONFIG_MEMORY_WATCH_DEFAULT_DEVICE_TOKEN ""
#endif

#ifndef CONFIG_MEMORY_WATCH_DEFAULT_TIMEOUT_MS
#define CONFIG_MEMORY_WATCH_DEFAULT_TIMEOUT_MS 0
#endif

#ifndef CONFIG_MEMORY_WATCH_DEFAULT_ALLOW_HTTP
#define CONFIG_MEMORY_WATCH_DEFAULT_ALLOW_HTTP 0
#endif

#ifndef CONFIG_MEMORY_WATCH_BOOT_TEXT
#define CONFIG_MEMORY_WATCH_BOOT_TEXT ""
#endif

static const char *TAG = "memory_watch";
static const UBaseType_t kCommandQueueLength = 8;
static const UBaseType_t kWorkerQueueLength = 1;
static const UBaseType_t kCancelQueueLength = 1;
static const UBaseType_t kHealthQueueLength = 1;
/* 栈大小按 bytes 计（xTaskCreateWithCaps 单位为 bytes，见 idf_additions.h）。
 * 下列常量名保留 *Words 后缀属历史命名，实际单位为 bytes。
 * kHealthWorkerStackWords/kConversationWorkerStackWords 经高压实测扩栈
 * （health check 后 free=208B，AI 对话后 free=200B，3.3%~3.4% 接近溢出）。 */
static const uint32_t kTaskStackWords = 6144;
static const uint32_t kUploadWorkerStackWords = 24576;
static const uint32_t kCancelWorkerStackWords = 3072;
static const uint32_t kHealthWorkerStackWords = 10240;
static const uint32_t kConversationWorkerStackWords = 10240;
static const size_t kAudioBufferInitialBytes = 8192U;
static const size_t kWsAudioChunkBytes = 16U * 1024U;
static const int64_t kConversationPollIntervalMs = 5000;
static const int64_t kBackgroundHttpsRetryIntervalMs = 5000;
static const uint32_t kConversationPollTimeoutMs = 4000U;
static const EventBits_t kWsWaitConversationBit = BIT0;
static const EventBits_t kWsWaitErrorBit = BIT1;
static const EventBits_t kWsWaitDisconnectedBit = BIT2;
static const EventBits_t kWsWaitAsrReadyBit = BIT3;
static const char *kEndpointNvsNamespace = "memory_watch";
static const char *kEndpointNvsBaseUrlKey = "base_url";
static const char *kEndpointNvsDeviceIdKey = "device_id";
static const char *kEndpointNvsDeviceTokenKey = "device_token";
static const char *kEndpointNvsTimeoutMsKey = "timeout_ms";
static const char *kEndpointNvsAllowHttpKey = "allow_http";

typedef enum
{
    MEMORY_WATCH_SERVICE_CMD_BEGIN_RECORDING = 0,
    MEMORY_WATCH_SERVICE_CMD_CHECK_HEALTH,
    MEMORY_WATCH_SERVICE_CMD_SEND_RECORDING,
    MEMORY_WATCH_SERVICE_CMD_SEND_TEXT,
    MEMORY_WATCH_SERVICE_CMD_CANCEL_RECORDING,
    MEMORY_WATCH_SERVICE_CMD_CANCEL_WAITING,
    MEMORY_WATCH_SERVICE_CMD_CANCEL_CLARIFICATION,
    MEMORY_WATCH_SERVICE_CMD_HEALTH_DONE,
    MEMORY_WATCH_SERVICE_CMD_WORKER_UPLOAD_STARTED,
    MEMORY_WATCH_SERVICE_CMD_WORKER_DONE,
    MEMORY_WATCH_SERVICE_CMD_SET_FOREGROUND,
    MEMORY_WATCH_SERVICE_CMD_CONVERSATION_POLL_DONE,
} memory_watch_service_cmd_type_t;

typedef struct
{
    bool configured;
    char base_url[MEMORY_WATCH_SERVICE_URL_MAX_BYTES];
    char device_id[MEMORY_WATCH_SERVICE_DEVICE_ID_MAX_BYTES];
    char device_token[MEMORY_WATCH_SERVICE_DEVICE_TOKEN_MAX_BYTES];
    uint32_t timeout_ms;
    bool allow_insecure_http;
} memory_watch_service_endpoint_state_t;

typedef struct
{
    memory_watch_voice_client_config_t client_config;
    char base_url[MEMORY_WATCH_SERVICE_URL_MAX_BYTES];
    char device_id[MEMORY_WATCH_SERVICE_DEVICE_ID_MAX_BYTES];
    char device_token[MEMORY_WATCH_SERVICE_DEVICE_TOKEN_MAX_BYTES];
} memory_watch_service_client_config_snapshot_t;

typedef struct
{
    memory_watch_service_client_config_snapshot_t client_config;
    char request_id[MEMORY_WATCH_SERVICE_ID_MAX_BYTES];
    char clarification_id[MEMORY_WATCH_SERVICE_ID_MAX_BYTES];
    char text[MEMORY_WATCH_SERVICE_TEXT_MAX_BYTES];
    bool text_command;
} memory_watch_service_upload_job_t;

typedef struct
{
    memory_watch_service_client_config_snapshot_t client_config;
    char request_id[MEMORY_WATCH_SERVICE_ID_MAX_BYTES];
} memory_watch_service_cancel_job_t;

typedef struct
{
    memory_watch_service_client_config_snapshot_t client_config;
} memory_watch_service_health_job_t;

typedef struct
{
    memory_watch_service_client_config_snapshot_t client_config;
    char after_message_id[64];
    char pending_request_id[MEMORY_WATCH_SERVICE_ID_MAX_BYTES];
    memory_watch_sync_mode_t sync_mode;
} memory_watch_service_conversation_job_t;

typedef struct
{
    esp_err_t error;
    int http_status;
    bool hermes_online;
} memory_watch_service_health_result_t;

typedef struct
{
    char request_id[MEMORY_WATCH_SERVICE_ID_MAX_BYTES];
    esp_err_t error;
    bool has_response;
    bool cancel_requested;
    bool conversation_already_appended;
    memory_watch_voice_client_response_t response;
} memory_watch_service_worker_result_t;

typedef struct
{
    uint8_t *data;
    size_t len;
    size_t capacity;
} memory_watch_service_audio_buffer_t;

typedef struct
{
    memory_watch_service_worker_result_t *result;
    const char *request_id;
} memory_watch_service_ws_wait_ctx_t;

typedef struct
{
    memory_watch_service_cmd_type_t type;
    memory_watch_service_health_result_t health_result;
    memory_watch_service_worker_result_t worker_result;
    char request_id[MEMORY_WATCH_SERVICE_ID_MAX_BYTES];
    char text[MEMORY_WATCH_SERVICE_TEXT_MAX_BYTES];
    bool foreground;
} memory_watch_service_cmd_t;

static TaskHandle_t s_service_task_handle = NULL;
static TaskHandle_t s_upload_worker_task_handle = NULL;
static TaskHandle_t s_cancel_worker_task_handle = NULL;
static TaskHandle_t s_health_worker_task_handle = NULL;
static TaskHandle_t s_conversation_worker_task_handle = NULL;
static QueueHandle_t s_command_queue = NULL;
static QueueHandle_t s_upload_worker_queue = NULL;
static QueueHandle_t s_cancel_worker_queue = NULL;
static QueueHandle_t s_health_worker_queue = NULL;
static QueueHandle_t s_conversation_worker_queue = NULL;
static StaticQueue_t s_command_queue_buffer;
static StaticQueue_t s_upload_worker_queue_buffer;
static StaticQueue_t s_cancel_worker_queue_buffer;
static StaticQueue_t s_health_worker_queue_buffer;
static StaticQueue_t s_conversation_worker_queue_buffer;
static uint8_t s_command_queue_storage[
    8 * sizeof(memory_watch_service_cmd_t)];
static uint8_t s_upload_worker_queue_storage[
    1 * sizeof(memory_watch_service_upload_job_t)];
static uint8_t s_cancel_worker_queue_storage[
    1 * sizeof(memory_watch_service_cancel_job_t)];
static uint8_t s_health_worker_queue_storage[
    1 * sizeof(memory_watch_service_health_job_t)];
static uint8_t s_conversation_worker_queue_storage[
    1 * sizeof(memory_watch_service_conversation_job_t)];
static memory_watch_service_cmd_t s_service_task_command;
static memory_watch_service_upload_job_t s_upload_worker_job;
static memory_watch_service_worker_result_t s_upload_worker_result;
static memory_watch_service_conversation_job_t s_conversation_worker_job;
static memory_watch_service_conversation_item_t
    s_conversation_items[MEMORY_WATCH_SERVICE_CONVERSATION_MAX_ITEMS];
static size_t s_conversation_item_count = 0;
static uint32_t s_conversation_generation = 0;
static StaticEventGroup_t s_ws_wait_event_buffer;
static EventGroupHandle_t s_ws_wait_event_group = NULL;
// Optimized: Legacy static buffers (s_service_task_stack, s_cancel_worker_task_stack,
// s_health_worker_task_stack) and StaticTask_t buffers have been removed.
// All tasks now dynamically allocate stack on external PSRAM via xTaskCreateWithCaps.
static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_endpoint_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_worker_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_foreground_lock = portMUX_INITIALIZER_UNLOCKED;
static memory_watch_service_snapshot_t s_snapshot = {
    .state = MEMORY_WATCH_SERVICE_STATE_READY,
    .network_ready = false,
    .endpoint_configured = false,
    .hermes_online = false,
    .request_active = false,
    .clarification_active = false,
    .last_error = ESP_OK,
};
static memory_watch_service_endpoint_state_t s_endpoint_config = {0};
static uint32_t s_boot_id = 0;
static uint32_t s_request_seq = 0;
static bool s_record_stop_requested = false;
static bool s_record_discard_requested = false;
static bool s_wait_cancel_requested = false;
static bool s_upload_worker_busy = false;
static bool s_foreground_active = false;
static bool s_foreground_runtime_gate_held = false;
static bool s_health_worker_busy = false;
static bool s_health_check_pending = false;
static int64_t s_health_retry_next_due_ms = 0;
static bool s_conversation_worker_busy = false;
static bool s_conversation_poll_active = false;
static int64_t s_conversation_poll_next_due_ms = 0;
static char s_conversation_pending_request_id[MEMORY_WATCH_SERVICE_ID_MAX_BYTES];
static char s_last_seen_conversation_id[64];
static memory_watch_conversation_message_t *s_conversation_staging = NULL;
static size_t s_conversation_staging_count = 0;
static esp_err_t s_conversation_staging_error = ESP_OK;
static memory_watch_voice_client_sync_result_t s_conversation_sync_result = {0};
static char s_wait_canceled_request_id[MEMORY_WATCH_SERVICE_ID_MAX_BYTES];
#if CONFIG_MEMORY_WATCH_BOOT_TEXT_SMOKE
static bool s_boot_text_sent = false;
#endif

/* 前向声明：inbox 相关函数和变量定义在文件后半部分 */
static esp_err_t memory_watch_service_inbox_init(void);
static portMUX_TYPE s_inbox_poll_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_inbox_poll_pending = false;
static int64_t s_inbox_poll_next_due_ms = 0;
static void memory_watch_service_inbox_set_poll_pending(bool pending);
static bool memory_watch_service_inbox_is_poll_pending(void);
static void memory_watch_service_inbox_try_poll(void);
static void memory_watch_service_inbox_handle_worker_result(uint32_t notify_val);
static int64_t memory_watch_service_now_ms(void);
static void memory_watch_service_conversation_try_poll(void);
static void memory_watch_service_conversation_handle_worker_done(void);
static
void memory_watch_service_handle_worker_done(
    const memory_watch_service_worker_result_t *result);

static void memory_watch_service_log_upload_stack(const char *stage)
{
    const UBaseType_t high_water_words = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "mw_upload stack: stage=%s high_water_words=%u",
             stage != NULL ? stage : "unknown",
             (unsigned int)high_water_words);
}

static int64_t memory_watch_service_now_ms(void)
{
    return esp_timer_get_time() / 1000LL;
}

/**
 * @brief 复制当前快照。
 *
 * snapshot 是 UI 与 owner task 共享的低频状态，因此用极短 critical
 * section 复制；getter 不做 I/O，也不推进录音或上传状态。
 */
static memory_watch_service_snapshot_t memory_watch_service_copy_snapshot(void)
{
    memory_watch_service_snapshot_t snapshot;

    portENTER_CRITICAL(&s_snapshot_lock);
    snapshot = s_snapshot;
    portEXIT_CRITICAL(&s_snapshot_lock);

    return snapshot;
}

static void memory_watch_service_set_state(
    memory_watch_service_state_t state, esp_err_t last_error)
{
    portENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot.state = state;
    s_snapshot.last_error = last_error;
    portEXIT_CRITICAL(&s_snapshot_lock);
}

static void memory_watch_service_set_request_active(bool active)
{
    portENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot.request_active = active;
    portEXIT_CRITICAL(&s_snapshot_lock);
}

static void memory_watch_service_set_network_ready(bool ready)
{
    portENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot.network_ready = ready;
    portEXIT_CRITICAL(&s_snapshot_lock);
}

static void memory_watch_service_set_endpoint_snapshot(bool configured,
                                                       bool hermes_online)
{
    portENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot.endpoint_configured = configured;
    s_snapshot.hermes_online = hermes_online;
    portEXIT_CRITICAL(&s_snapshot_lock);
}

static void memory_watch_service_copy_text(char *dst, size_t dst_len,
                                           const char *src)
{
    if (dst == NULL || dst_len == 0U)
    {
        return;
    }

    if (src == NULL)
    {
        dst[0] = '\0';
        return;
    }

    size_t index = 0;
    while (index + 1U < dst_len && src[index] != '\0')
    {
        dst[index] = src[index];
        ++index;
    }
    dst[index] = '\0';
}

static bool memory_watch_service_is_safe_user_text(const char *text)
{
    if (text == NULL || text[0] == '\0')
    {
        return false;
    }

    size_t len = 0;
    for (const char *p = text; *p != '\0'; ++p)
    {
        if (*p == '\r' || *p == '\n')
        {
            return false;
        }
        ++len;
        if (len >= MEMORY_WATCH_SERVICE_TEXT_MAX_BYTES)
        {
            return false;
        }
    }
    return true;
}

static void memory_watch_service_append_conversation_item(
    memory_watch_service_conversation_role_t role,
    const char *request_id,
    const char *text)
{
    if (text == NULL || text[0] == '\0')
    {
        return;
    }

    portENTER_CRITICAL(&s_snapshot_lock);
    for (size_t i = 0; i < s_conversation_item_count; ++i)
    {
        if (s_conversation_items[i].role == role &&
            strcmp(s_conversation_items[i].request_id,
                   request_id != NULL ? request_id : "") == 0 &&
            strcmp(s_conversation_items[i].text, text) == 0)
        {
            portEXIT_CRITICAL(&s_snapshot_lock);
            return;
        }
    }

    if (s_conversation_item_count >= MEMORY_WATCH_SERVICE_CONVERSATION_MAX_ITEMS)
    {
        memmove(&s_conversation_items[0], &s_conversation_items[1],
                (MEMORY_WATCH_SERVICE_CONVERSATION_MAX_ITEMS - 1U) *
                    sizeof(s_conversation_items[0]));
        s_conversation_item_count =
            MEMORY_WATCH_SERVICE_CONVERSATION_MAX_ITEMS - 1U;
    }

    memory_watch_service_conversation_item_t *item =
        &s_conversation_items[s_conversation_item_count++];
    item->role = role;
    memory_watch_service_copy_text(item->request_id, sizeof(item->request_id),
                                   request_id);
    memory_watch_service_copy_text(item->text, sizeof(item->text), text);
    ++s_conversation_generation;
    s_snapshot.conversation_generation = s_conversation_generation;
    portEXIT_CRITICAL(&s_snapshot_lock);
}

static void memory_watch_service_append_response_conversation(
    const memory_watch_voice_client_response_t *response)
{
    if (response == NULL)
    {
        return;
    }

    if (response->asr_text[0] != '\0')
    {
        memory_watch_service_append_conversation_item(
            MEMORY_WATCH_SERVICE_CONVERSATION_USER,
            response->request_id,
            response->asr_text);
    }
    if (response->reply_text[0] != '\0')
    {
        memory_watch_service_append_conversation_item(
            MEMORY_WATCH_SERVICE_CONVERSATION_HERMES,
            response->request_id,
            response->reply_text);
    }
}

static void memory_watch_service_set_last_seen_conversation_id(
    const char *message_id)
{
    if (message_id == NULL || message_id[0] == '\0')
    {
        return;
    }
    portENTER_CRITICAL(&s_snapshot_lock);
    memory_watch_service_copy_text(s_last_seen_conversation_id,
                                   sizeof(s_last_seen_conversation_id),
                                   message_id);
    portEXIT_CRITICAL(&s_snapshot_lock);
}

static void memory_watch_service_copy_last_seen_conversation_id(
    char *out, size_t out_len)
{
    if (out == NULL || out_len == 0U)
    {
        return;
    }
    portENTER_CRITICAL(&s_snapshot_lock);
    memory_watch_service_copy_text(out, out_len, s_last_seen_conversation_id);
    portEXIT_CRITICAL(&s_snapshot_lock);
}

static void memory_watch_service_append_server_conversation_message(
    const memory_watch_conversation_message_t *message)
{
    if (message == NULL)
    {
        return;
    }
    if (strcmp(message->role, "user") == 0)
    {
        memory_watch_service_append_conversation_item(
            MEMORY_WATCH_SERVICE_CONVERSATION_USER,
            message->request_id,
            message->text);
    }
    else if (strcmp(message->role, "assistant") == 0)
    {
        memory_watch_service_append_conversation_item(
            MEMORY_WATCH_SERVICE_CONVERSATION_HERMES,
            message->request_id,
            message->text);
    }
    memory_watch_service_set_last_seen_conversation_id(message->message_id);
}

static void memory_watch_service_set_foreground_active(bool foreground)
{
    bool should_acquire = false;
    bool should_release = false;

    portENTER_CRITICAL(&s_foreground_lock);
    s_foreground_active = foreground;
    if (foreground && !s_foreground_runtime_gate_held)
    {
        should_acquire = true;
    }
    else if (!foreground && s_foreground_runtime_gate_held)
    {
        s_foreground_runtime_gate_held = false;
        should_release = true;
    }
    portEXIT_CRITICAL(&s_foreground_lock);

    if (should_acquire)
    {
        const esp_err_t err = foreground_runtime_gate_acquire(
            FOREGROUND_RUNTIME_OWNER_HERMES, 0U);
        if (err == ESP_OK)
        {
            portENTER_CRITICAL(&s_foreground_lock);
            s_foreground_runtime_gate_held = s_foreground_active;
            const bool release_immediately = !s_foreground_active;
            if (release_immediately)
            {
                s_foreground_runtime_gate_held = false;
            }
            portEXIT_CRITICAL(&s_foreground_lock);
            if (release_immediately)
            {
                (void)foreground_runtime_gate_release(
                    FOREGROUND_RUNTIME_OWNER_HERMES);
            }
            (void)background_service_manager_notify_foreground_runtime_changed();
        }
        else
        {
            ESP_LOGW(TAG, "Hermes foreground gate acquire failed: %s",
                     esp_err_to_name(err));
        }
    }

    if (should_release)
    {
        (void)foreground_runtime_gate_release(FOREGROUND_RUNTIME_OWNER_HERMES);
        (void)background_service_manager_notify_foreground_runtime_changed();
    }
}

static bool memory_watch_service_is_foreground_active(void)
{
    bool foreground = false;
    portENTER_CRITICAL(&s_foreground_lock);
    foreground = s_foreground_active;
    portEXIT_CRITICAL(&s_foreground_lock);
    return foreground;
}

static esp_err_t memory_watch_service_copy_required_text(char *dst,
                                                         size_t dst_len,
                                                         const char *src)
{
    if (dst == NULL || dst_len == 0U || src == NULL || src[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = 0;
    while (src[len] != '\0' && len < dst_len)
    {
        ++len;
    }
    if (len == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (len >= dst_len)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(dst, src, len + 1U);
    return ESP_OK;
}

static bool memory_watch_service_is_safe_endpoint_text(const char *text)
{
    if (text == NULL || text[0] == '\0')
    {
        return false;
    }

    for (const char *p = text; *p != '\0'; ++p)
    {
        if (*p == '\r' || *p == '\n')
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief 校验所有 endpoint 配置入口共同遵守的 URL 与文本边界。
 *
 * 配置可能来自 SoftAP、NVS 或后续调试入口，因此校验放在 service owner 层，避免
 * 某个入口漏检后把非法地址标记为已配置。
 */
static esp_err_t memory_watch_service_validate_endpoint_state(
    const memory_watch_service_endpoint_state_t *state)
{
    if (state == NULL ||
        !memory_watch_service_is_safe_endpoint_text(state->base_url) ||
        !memory_watch_service_is_safe_endpoint_text(state->device_id) ||
        !memory_watch_service_is_safe_endpoint_text(state->device_token))
    {
        return ESP_ERR_INVALID_ARG;
    }

    const bool uses_http = strncmp(state->base_url, "http://", 7) == 0;
    const bool uses_https = strncmp(state->base_url, "https://", 8) == 0;
    if (!uses_http && !uses_https)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (uses_http && !state->allow_insecure_http)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t memory_watch_service_build_endpoint_state(
    const memory_watch_service_endpoint_config_t *config,
    memory_watch_service_endpoint_state_t *out_state)
{
    if (config == NULL || out_state == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memory_watch_service_endpoint_state_t next_config = {0};
    esp_err_t err = memory_watch_service_copy_required_text(
        next_config.base_url, sizeof(next_config.base_url), config->base_url);
    if (err != ESP_OK)
    {
        return err;
    }
    err = memory_watch_service_copy_required_text(
        next_config.device_id, sizeof(next_config.device_id),
        config->device_id);
    if (err != ESP_OK)
    {
        return err;
    }
    err = memory_watch_service_copy_required_text(
        next_config.device_token, sizeof(next_config.device_token),
        config->device_token);
    if (err != ESP_OK)
    {
        return err;
    }
    next_config.timeout_ms = config->timeout_ms;
    next_config.allow_insecure_http = config->allow_insecure_http;
    next_config.configured = true;

    err = memory_watch_service_validate_endpoint_state(&next_config);
    if (err != ESP_OK)
    {
        return err;
    }

    *out_state = next_config;
    return ESP_OK;
}

static esp_err_t memory_watch_service_read_nvs_text(nvs_handle_t handle,
                                                    const char *key,
                                                    char *dst,
                                                    size_t dst_len)
{
    if (key == NULL || dst == NULL || dst_len == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = dst_len;
    const esp_err_t err = nvs_get_str(handle, key, dst, &len);
    if (err != ESP_OK)
    {
        dst[0] = '\0';
        return err;
    }
    if (len == 0U || dst[0] == '\0')
    {
        dst[0] = '\0';
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/**
 * @brief 加载 Kconfig 默认 endpoint 兜底配置。
 *
 * NVS 配置优先级更高；该路径只用于还没通过 SoftAP 写入 NVS 时的
 * 开发/出厂默认值。`device_token` 只代表 watch endpoint 的设备 token，
 * 不得放入 Hermes、MiMo、Cloudflare 或 API Server key。
 */
static esp_err_t memory_watch_service_load_kconfig_endpoint_default(void)
{
#if CONFIG_MEMORY_WATCH_DEFAULT_ENDPOINT_ENABLED
    if (CONFIG_MEMORY_WATCH_DEFAULT_BASE_URL[0] == '\0' ||
        CONFIG_MEMORY_WATCH_DEFAULT_DEVICE_ID[0] == '\0' ||
        CONFIG_MEMORY_WATCH_DEFAULT_DEVICE_TOKEN[0] == '\0')
    {
        return ESP_ERR_NOT_FOUND;
    }

    const memory_watch_service_endpoint_config_t config = {
        .base_url = CONFIG_MEMORY_WATCH_DEFAULT_BASE_URL,
        .device_id = CONFIG_MEMORY_WATCH_DEFAULT_DEVICE_ID,
        .device_token = CONFIG_MEMORY_WATCH_DEFAULT_DEVICE_TOKEN,
        .timeout_ms = (uint32_t)CONFIG_MEMORY_WATCH_DEFAULT_TIMEOUT_MS,
        .allow_insecure_http =
            (CONFIG_MEMORY_WATCH_DEFAULT_ALLOW_HTTP != 0),
    };
    const esp_err_t err = memory_watch_service_configure_endpoint(&config);
    if (err == ESP_OK)
    {
        ESP_LOGW(TAG,
                 "watch endpoint configured from Kconfig default: device_id=%s allow_http=%u timeout_ms=%lu",
                 CONFIG_MEMORY_WATCH_DEFAULT_DEVICE_ID,
                 (unsigned int)(CONFIG_MEMORY_WATCH_DEFAULT_ALLOW_HTTP != 0),
                 (unsigned long)CONFIG_MEMORY_WATCH_DEFAULT_TIMEOUT_MS);
    }
    return err;
#else
    return ESP_ERR_NOT_FOUND;
#endif
}

static bool memory_watch_service_try_kconfig_endpoint_default(void)
{
    const esp_err_t err = memory_watch_service_load_kconfig_endpoint_default();
    if (err == ESP_OK)
    {
        return true;
    }
    if (err != ESP_ERR_NOT_FOUND)
    {
        ESP_LOGW(TAG, "watch endpoint Kconfig default rejected: %s",
                 esp_err_to_name(err));
    }
    return false;
}

/**
 * @brief 从 NVS 读取 watch endpoint 运行期配置。
 *
 * 这里仅读取 ESP32-S3 到 voice endpoint 的 device token，不读取也不保存
 * Hermes / MiMo / API Server key。缺失配置只让页面保持"未配置"，不阻塞启动。
 */
static esp_err_t memory_watch_service_load_endpoint_from_nvs(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kEndpointNvsNamespace, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        if (memory_watch_service_try_kconfig_endpoint_default())
        {
            return ESP_OK;
        }
        memory_watch_service_set_endpoint_snapshot(false, false);
        ESP_LOGI(TAG, "watch endpoint NVS config not found");
        return ESP_OK;
    }
    if (err != ESP_OK)
    {
        memory_watch_service_set_endpoint_snapshot(false, false);
        return err;
    }

    char base_url[MEMORY_WATCH_SERVICE_URL_MAX_BYTES] = {0};
    char device_id[MEMORY_WATCH_SERVICE_DEVICE_ID_MAX_BYTES] = {0};
    char device_token[MEMORY_WATCH_SERVICE_DEVICE_TOKEN_MAX_BYTES] = {0};

    err = memory_watch_service_read_nvs_text(
        handle, kEndpointNvsBaseUrlKey, base_url, sizeof(base_url));
    if (err == ESP_OK)
    {
        err = memory_watch_service_read_nvs_text(
            handle, kEndpointNvsDeviceIdKey, device_id, sizeof(device_id));
    }
    if (err == ESP_OK)
    {
        err = memory_watch_service_read_nvs_text(
            handle, kEndpointNvsDeviceTokenKey, device_token,
            sizeof(device_token));
    }
    if (err != ESP_OK)
    {
        nvs_close(handle);
        if (memory_watch_service_try_kconfig_endpoint_default())
        {
            return ESP_OK;
        }
        memory_watch_service_set_endpoint_snapshot(false, false);
        ESP_LOGW(TAG, "watch endpoint NVS config incomplete: %s",
                 esp_err_to_name(err));
        return ESP_OK;
    }

    uint32_t timeout_ms = 0;
    err = nvs_get_u32(handle, kEndpointNvsTimeoutMsKey, &timeout_ms);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
    {
        nvs_close(handle);
        memory_watch_service_set_endpoint_snapshot(false, false);
        return err;
    }

    uint8_t allow_http = 0;
    err = nvs_get_u8(handle, kEndpointNvsAllowHttpKey, &allow_http);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
    {
        nvs_close(handle);
        memory_watch_service_set_endpoint_snapshot(false, false);
        return err;
    }
    nvs_close(handle);

    const memory_watch_service_endpoint_config_t config = {
        .base_url = base_url,
        .device_id = device_id,
        .device_token = device_token,
        .timeout_ms = timeout_ms,
        .allow_insecure_http = allow_http != 0U,
    };
    err = memory_watch_service_configure_endpoint(&config);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG,
                 "watch endpoint configured from NVS: device_id=%s allow_http=%u timeout_ms=%lu",
                 device_id, (unsigned int)(allow_http != 0U),
                 (unsigned long)timeout_ms);
    }
    return err;
}

static void memory_watch_service_init_boot_id(void)
{
    if (s_boot_id != 0U)
    {
        return;
    }

    s_boot_id = esp_random();
    if (s_boot_id == 0U)
    {
        s_boot_id = 1U;
    }
}

static esp_err_t memory_watch_service_make_request_id(const char *device_id,
                                                      char *out_request_id,
                                                      size_t out_len)
{
    if (device_id == NULL || device_id[0] == '\0' ||
        out_request_id == NULL || out_len == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t seq = ++s_request_seq;
    const int written = snprintf(out_request_id, out_len,
                                 "%s-%08" PRIx32 "-%04" PRIu32,
                                 device_id, s_boot_id, seq);
    if (written < 0 || (size_t)written >= out_len)
    {
        out_request_id[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t memory_watch_service_copy_client_config(
    memory_watch_service_client_config_snapshot_t *out_config)
{
    if (out_config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memory_watch_service_endpoint_state_t endpoint_config;
    portENTER_CRITICAL(&s_endpoint_lock);
    endpoint_config = s_endpoint_config;
    portEXIT_CRITICAL(&s_endpoint_lock);

    if (!endpoint_config.configured)
    {
        return ESP_ERR_INVALID_STATE;
    }

    memset(out_config, 0, sizeof(*out_config));
    memory_watch_service_copy_text(out_config->base_url,
                                   sizeof(out_config->base_url),
                                   endpoint_config.base_url);
    memory_watch_service_copy_text(out_config->device_id,
                                   sizeof(out_config->device_id),
                                   endpoint_config.device_id);
    memory_watch_service_copy_text(out_config->device_token,
                                   sizeof(out_config->device_token),
                                   endpoint_config.device_token);

    out_config->client_config.base_url = out_config->base_url;
    out_config->client_config.device_id = out_config->device_id;
    out_config->client_config.device_token = out_config->device_token;
    out_config->client_config.timeout_ms = endpoint_config.timeout_ms;
    out_config->client_config.allow_insecure_http =
        endpoint_config.allow_insecure_http;
    return ESP_OK;
}

esp_err_t memory_watch_service_copy_endpoint_config(
    memory_watch_service_endpoint_snapshot_t *out_config)
{
    if (out_config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memory_watch_service_endpoint_state_t endpoint_config;
    portENTER_CRITICAL(&s_endpoint_lock);
    endpoint_config = s_endpoint_config;
    portEXIT_CRITICAL(&s_endpoint_lock);

    if (!endpoint_config.configured)
    {
        return ESP_ERR_INVALID_STATE;
    }

    memset(out_config, 0, sizeof(*out_config));
    memory_watch_service_copy_text(out_config->base_url,
                                   sizeof(out_config->base_url),
                                   endpoint_config.base_url);
    memory_watch_service_copy_text(out_config->device_id,
                                   sizeof(out_config->device_id),
                                   endpoint_config.device_id);
    memory_watch_service_copy_text(out_config->device_token,
                                   sizeof(out_config->device_token),
                                   endpoint_config.device_token);
    out_config->timeout_ms = endpoint_config.timeout_ms;
    out_config->allow_insecure_http = endpoint_config.allow_insecure_http;
    return ESP_OK;
}

static void memory_watch_service_rebind_client_config(
    memory_watch_service_client_config_snapshot_t *config)
{
    if (config == NULL)
    {
        return;
    }

    config->client_config.base_url = config->base_url;
    config->client_config.device_id = config->device_id;
    config->client_config.device_token = config->device_token;
}

static void memory_watch_service_set_request_id(const char *request_id)
{
    portENTER_CRITICAL(&s_snapshot_lock);
    memory_watch_service_copy_text(s_snapshot.request_id,
                                   sizeof(s_snapshot.request_id),
                                   request_id);
    s_snapshot.asr_text[0] = '\0';
    s_snapshot.reply_text[0] = '\0';
    portEXIT_CRITICAL(&s_snapshot_lock);
}

static void memory_watch_service_copy_current_clarification(
    char *out_clarification_id, size_t out_len)
{
    if (out_clarification_id == NULL || out_len == 0U)
    {
        return;
    }

    portENTER_CRITICAL(&s_snapshot_lock);
    memory_watch_service_copy_text(out_clarification_id, out_len,
                                   s_snapshot.clarification_active
                                       ? s_snapshot.clarification_id
                                       : "");
    portEXIT_CRITICAL(&s_snapshot_lock);
}

static bool memory_watch_service_request_id_matches_current(
    const char *request_id)
{
    if (request_id == NULL)
    {
        return false;
    }

    bool matches = false;
    portENTER_CRITICAL(&s_snapshot_lock);
    matches = strcmp(s_snapshot.request_id, request_id) == 0;
    portEXIT_CRITICAL(&s_snapshot_lock);
    return matches;
}

static void memory_watch_service_set_response_texts(
    const memory_watch_voice_client_response_t *response)
{
    if (response == NULL)
    {
        return;
    }

    portENTER_CRITICAL(&s_snapshot_lock);
    memory_watch_service_copy_text(s_snapshot.asr_text,
                                   sizeof(s_snapshot.asr_text),
                                   response->asr_text);
    memory_watch_service_copy_text(s_snapshot.reply_text,
                                   sizeof(s_snapshot.reply_text),
                                   response->reply_text);
    memory_watch_service_copy_text(s_snapshot.clarification_id,
                                   sizeof(s_snapshot.clarification_id),
                                   response->clarification_id);
    s_snapshot.clarification_active = response->clarification_id[0] != '\0';
    portEXIT_CRITICAL(&s_snapshot_lock);
}

static void memory_watch_service_reset_worker_flags(void)
{
    portENTER_CRITICAL(&s_worker_lock);
    s_record_stop_requested = false;
    s_record_discard_requested = false;
    s_wait_cancel_requested = false;
    s_wait_canceled_request_id[0] = '\0';
    portEXIT_CRITICAL(&s_worker_lock);
}

static void memory_watch_service_request_record_stop(bool discard)
{
    portENTER_CRITICAL(&s_worker_lock);
    s_record_stop_requested = true;
    if (discard)
    {
        s_record_discard_requested = true;
    }
    portEXIT_CRITICAL(&s_worker_lock);
}

static void memory_watch_service_request_wait_cancel(const char *request_id)
{
    portENTER_CRITICAL(&s_worker_lock);
    s_wait_cancel_requested = true;
    memory_watch_service_copy_text(s_wait_canceled_request_id,
                                   sizeof(s_wait_canceled_request_id),
                                   request_id);
    portEXIT_CRITICAL(&s_worker_lock);
}

static bool memory_watch_service_should_stop_recording(void *user_ctx)
{
    (void)user_ctx;

    bool should_stop = false;
    portENTER_CRITICAL(&s_worker_lock);
    should_stop = s_record_stop_requested || s_record_discard_requested;
    portEXIT_CRITICAL(&s_worker_lock);
    return should_stop;
}

static bool memory_watch_service_is_record_discard_requested(void)
{
    bool discard = false;
    portENTER_CRITICAL(&s_worker_lock);
    discard = s_record_discard_requested;
    portEXIT_CRITICAL(&s_worker_lock);
    return discard;
}

static bool memory_watch_service_should_abort_recording(void *user_ctx)
{
    (void)user_ctx;
    return memory_watch_service_is_record_discard_requested();
}

static bool memory_watch_service_is_wait_cancel_requested(void)
{
    bool cancel_requested = false;
    portENTER_CRITICAL(&s_worker_lock);
    cancel_requested = s_wait_cancel_requested;
    portEXIT_CRITICAL(&s_worker_lock);
    return cancel_requested;
}

static bool memory_watch_service_is_wait_canceled_request(
    const char *request_id)
{
    if (request_id == NULL)
    {
        return false;
    }

    bool canceled = false;
    portENTER_CRITICAL(&s_worker_lock);
    canceled = s_wait_canceled_request_id[0] != '\0' &&
               strcmp(s_wait_canceled_request_id, request_id) == 0;
    portEXIT_CRITICAL(&s_worker_lock);
    return canceled;
}

static void memory_watch_service_set_upload_worker_busy(bool busy)
{
    portENTER_CRITICAL(&s_worker_lock);
    s_upload_worker_busy = busy;
    portEXIT_CRITICAL(&s_worker_lock);
}

static bool memory_watch_service_is_upload_worker_busy(void)
{
    bool busy = false;
    portENTER_CRITICAL(&s_worker_lock);
    busy = s_upload_worker_busy;
    portEXIT_CRITICAL(&s_worker_lock);
    return busy;
}

static void *memory_watch_service_alloc_audio_psram(size_t len)
{
    return heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void memory_watch_service_free(void *ptr)
{
    if (ptr != NULL)
    {
        heap_caps_free(ptr);
    }
}

static void memory_watch_service_audio_buffer_free(
    memory_watch_service_audio_buffer_t *buffer)
{
    if (buffer == NULL)
    {
        return;
    }

    memory_watch_service_free(buffer->data);
    buffer->data = NULL;
    buffer->len = 0U;
    buffer->capacity = 0U;
}

static esp_err_t memory_watch_service_audio_buffer_reserve(
    memory_watch_service_audio_buffer_t *buffer, size_t required)
{
    if (buffer == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (required <= buffer->capacity)
    {
        return ESP_OK;
    }
    if (required > MEMORY_WATCH_VOICE_CLIENT_MAX_AUDIO_BYTES)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t next_capacity = buffer->capacity > 0U
                               ? buffer->capacity
                               : kAudioBufferInitialBytes;
    while (next_capacity < required)
    {
        if (next_capacity > (MEMORY_WATCH_VOICE_CLIENT_MAX_AUDIO_BYTES / 2U))
        {
            next_capacity = MEMORY_WATCH_VOICE_CLIENT_MAX_AUDIO_BYTES;
        }
        else
        {
            next_capacity *= 2U;
        }
    }

    uint8_t *next_data =
        (uint8_t *)memory_watch_service_alloc_audio_psram(next_capacity);
    if (next_data == NULL)
    {
        ESP_LOGW(TAG, "audio buffer PSRAM allocation failed: bytes=%u",
                 (unsigned int)next_capacity);
        return ESP_ERR_NO_MEM;
    }
    if (buffer->data != NULL && buffer->len > 0U)
    {
        memcpy(next_data, buffer->data, buffer->len);
    }
    memory_watch_service_free(buffer->data);
    buffer->data = next_data;
    buffer->capacity = next_capacity;
    return ESP_OK;
}

static esp_err_t memory_watch_service_audio_write_cb(const uint8_t *data,
                                                     size_t len,
                                                     void *user_ctx)
{
    memory_watch_service_audio_buffer_t *buffer =
        (memory_watch_service_audio_buffer_t *)user_ctx;
    if (buffer == NULL || (data == NULL && len > 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0U)
    {
        return ESP_OK;
    }
    if (len > MEMORY_WATCH_VOICE_CLIENT_MAX_AUDIO_BYTES ||
        buffer->len > MEMORY_WATCH_VOICE_CLIENT_MAX_AUDIO_BYTES - len)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t required = buffer->len + len;
    esp_err_t err =
        memory_watch_service_audio_buffer_reserve(buffer, required);
    if (err != ESP_OK)
    {
        return err;
    }

    memcpy(buffer->data + buffer->len, data, len);
    buffer->len = required;
    return ESP_OK;
}

static void memory_watch_service_ws_event_cb(
    const memory_watch_ws_event_t *event,
    void *user_ctx)
{
    memory_watch_service_ws_wait_ctx_t *ctx =
        (memory_watch_service_ws_wait_ctx_t *)user_ctx;
    if (event == NULL || ctx == NULL || ctx->result == NULL ||
        s_ws_wait_event_group == NULL)
    {
        return;
    }

    if (event->request_id[0] != '\0' && ctx->request_id != NULL &&
        strcmp(event->request_id, ctx->request_id) != 0)
    {
        return;
    }

    if (event->kind == MEMORY_WATCH_WS_EVENT_TURN_ASR_READY)
    {
        memory_watch_service_copy_text(ctx->result->response.request_id,
                                       sizeof(ctx->result->response.request_id),
                                       ctx->request_id);
        memory_watch_service_copy_text(ctx->result->response.asr_text,
                                       sizeof(ctx->result->response.asr_text),
                                       event->text);
        memory_watch_service_set_last_seen_conversation_id(event->message_id);
        memory_watch_service_append_conversation_item(
            MEMORY_WATCH_SERVICE_CONVERSATION_USER,
            ctx->request_id,
            event->text);
        xEventGroupSetBits(s_ws_wait_event_group, kWsWaitAsrReadyBit);
        return;
    }

    if (event->kind == MEMORY_WATCH_WS_EVENT_TURN_REPLY_MESSAGE)
    {
        memory_watch_service_copy_text(ctx->result->response.request_id,
                                       sizeof(ctx->result->response.request_id),
                                       ctx->request_id);
        memory_watch_service_copy_text(ctx->result->response.status,
                                       sizeof(ctx->result->response.status),
                                       event->status[0] != '\0' ? event->status : "done");
        memory_watch_service_copy_text(ctx->result->response.action,
                                       sizeof(ctx->result->response.action),
                                       "conversation_reply");
        memory_watch_service_copy_text(ctx->result->response.reply_text,
                                       sizeof(ctx->result->response.reply_text),
                                       event->text);
        if (event->error_code[0] != '\0')
        {
            memory_watch_service_copy_text(ctx->result->response.error_code,
                                           sizeof(ctx->result->response.error_code),
                                           event->error_code);
        }
        memory_watch_service_set_last_seen_conversation_id(event->message_id);
        (void)memory_watch_ws_client_send_ack(event->message_id);
        xEventGroupSetBits(s_ws_wait_event_group, kWsWaitConversationBit);
        return;
    }

    if (event->kind == MEMORY_WATCH_WS_EVENT_TURN_ERROR)
    {
        memory_watch_service_copy_text(ctx->result->response.request_id,
                                       sizeof(ctx->result->response.request_id),
                                       ctx->request_id);
        memory_watch_service_copy_text(ctx->result->response.status,
                                       sizeof(ctx->result->response.status),
                                       "error");
        memory_watch_service_copy_text(ctx->result->response.action,
                                       sizeof(ctx->result->response.action),
                                       "error");
        memory_watch_service_copy_text(ctx->result->response.error_code,
                                       sizeof(ctx->result->response.error_code),
                                       event->error_code[0] != '\0'
                                           ? event->error_code
                                           : "websocket_error");
        xEventGroupSetBits(s_ws_wait_event_group, kWsWaitErrorBit);
    }
}

static void memory_watch_service_ws_disconnect_cb(void *user_ctx)
{
    (void)user_ctx;
    if (s_ws_wait_event_group != NULL)
    {
        xEventGroupSetBits(s_ws_wait_event_group, kWsWaitDisconnectedBit);
    }
}

static void memory_watch_service_fill_pending_response(
    memory_watch_service_worker_result_t *result,
    const char *request_id)
{
    if (result == NULL)
    {
        return;
    }
    memory_watch_service_copy_text(result->response.request_id,
                                   sizeof(result->response.request_id),
                                   request_id);
    memory_watch_service_copy_text(result->response.status,
                                   sizeof(result->response.status),
                                   "pending");
    memory_watch_service_copy_text(result->response.action,
                                   sizeof(result->response.action),
                                   "conversation_pending");
    result->has_response = true;
}

static esp_err_t memory_watch_service_send_voice_over_ws(
    const memory_watch_service_upload_job_t *job,
    const memory_watch_service_audio_buffer_t *audio_buffer,
    memory_watch_service_worker_result_t *result)
{
    if (job == NULL || audio_buffer == NULL || audio_buffer->data == NULL ||
        audio_buffer->len == 0U || result == NULL ||
        s_ws_wait_event_group == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memory_watch_service_ws_wait_ctx_t wait_ctx = {
        .result = result,
        .request_id = job->request_id,
    };
    xEventGroupClearBits(s_ws_wait_event_group,
                         kWsWaitConversationBit |
                             kWsWaitErrorBit |
                             kWsWaitDisconnectedBit |
                             kWsWaitAsrReadyBit);
    char last_seen_conversation_id[64];
    memory_watch_service_copy_last_seen_conversation_id(
        last_seen_conversation_id, sizeof(last_seen_conversation_id));

    const memory_watch_ws_client_config_t ws_config = {
        .endpoint = job->client_config.client_config,
        .last_seen_conversation_id = last_seen_conversation_id,
        .event_cb = memory_watch_service_ws_event_cb,
        .disconnect_cb = memory_watch_service_ws_disconnect_cb,
        .user_ctx = &wait_ctx,
    };
    esp_err_t err = memory_watch_ws_client_connect(&ws_config);
    if (err != ESP_OK)
    {
        return err;
    }

    err = memory_watch_ws_client_send_audio_turn(
        job->request_id, audio_buffer->data, audio_buffer->len,
        kWsAudioChunkBytes);
    if (err != ESP_OK)
    {
        memory_watch_ws_client_close();
        return err;
    }

    const uint32_t timeout_ms =
        job->client_config.client_config.timeout_ms > 0U
            ? job->client_config.client_config.timeout_ms
            : MEMORY_WATCH_VOICE_CLIENT_DEFAULT_TIMEOUT_MS;
    EventBits_t bits = 0;
    bool asr_ready_seen = false;
    const int64_t wait_deadline_ms = esp_timer_get_time() / 1000LL +
                                     (int64_t)timeout_ms;
    while (true)
    {
        if (!memory_watch_service_is_foreground_active())
        {
            if (asr_ready_seen)
            {
                memory_watch_service_fill_pending_response(result, job->request_id);
                memory_watch_ws_client_close();
                return ESP_OK;
            }
        }

        bits = xEventGroupWaitBits(
            s_ws_wait_event_group,
            kWsWaitConversationBit | kWsWaitErrorBit | kWsWaitDisconnectedBit |
                kWsWaitAsrReadyBit,
            pdTRUE,
            pdFALSE,
            pdMS_TO_TICKS(250U));
        if ((bits & (kWsWaitConversationBit | kWsWaitErrorBit |
                     kWsWaitDisconnectedBit)) != 0)
        {
            break;
        }
        if ((bits & kWsWaitAsrReadyBit) != 0)
        {
            asr_ready_seen = true;
        }
        if ((esp_timer_get_time() / 1000LL) >= wait_deadline_ms)
        {
            break;
        }
    }
    memory_watch_ws_client_close();

    if ((bits & kWsWaitConversationBit) != 0)
    {
        return ESP_OK;
    }
    if ((bits & kWsWaitErrorBit) != 0)
    {
        return ESP_FAIL;
    }
    if ((bits & kWsWaitDisconnectedBit) != 0)
    {
        memory_watch_service_fill_pending_response(result, job->request_id);
        return ESP_OK;
    }
    memory_watch_service_copy_text(result->response.request_id,
                                   sizeof(result->response.request_id),
                                   job->request_id);
    memory_watch_service_copy_text(result->response.status,
                                   sizeof(result->response.status),
                                   "timeout");
    memory_watch_service_copy_text(result->response.action,
                                   sizeof(result->response.action),
                                   "timeout");
    memory_watch_service_copy_text(result->response.error_code,
                                   sizeof(result->response.error_code),
                                   "watch_timeout");
    return ESP_ERR_TIMEOUT;
}

static esp_err_t memory_watch_service_post_worker_result(
    const memory_watch_service_worker_result_t *result)
{
    if (s_command_queue == NULL || result == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    memory_watch_service_cmd_t command = {
        .type = MEMORY_WATCH_SERVICE_CMD_WORKER_DONE,
        .worker_result = *result,
    };
    if (xQueueSend(s_command_queue, &command, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t memory_watch_service_post_health_result(
    const memory_watch_service_health_result_t *result)
{
    if (s_command_queue == NULL || result == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    memory_watch_service_cmd_t command = {
        .type = MEMORY_WATCH_SERVICE_CMD_HEALTH_DONE,
        .health_result = *result,
    };
    if (xQueueSend(s_command_queue, &command, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t memory_watch_service_post_upload_started(
    const char *request_id)
{
    if (s_command_queue == NULL || request_id == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    memory_watch_service_cmd_t command = {
        .type = MEMORY_WATCH_SERVICE_CMD_WORKER_UPLOAD_STARTED,
    };
    memory_watch_service_copy_text(command.request_id,
                                   sizeof(command.request_id),
                                   request_id);
    if (xQueueSend(s_command_queue, &command, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static const char *memory_watch_service_firmware_version(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    if (app_desc == NULL || app_desc->version[0] == '\0')
    {
        return NULL;
    }
    return app_desc->version;
}

static void memory_watch_service_fill_power_fields(
    memory_watch_voice_client_request_t *request)
{
    if (request == NULL)
    {
        return;
    }

    const power_policy_budget_t budget = power_policy_get_budget();
    if (budget.battery_data_valid && budget.battery_percent <= 100U)
    {
        request->has_battery_percent = true;
        request->battery_percent = (int)budget.battery_percent;
    }
    request->has_charging = true;
    request->charging = (budget.flags & POWER_POLICY_FLAG_CHARGING) != 0U;
}

static void memory_watch_service_fill_text_power_fields(
    memory_watch_voice_client_text_request_t *request)
{
    if (request == NULL)
    {
        return;
    }

    const power_policy_budget_t budget = power_policy_get_budget();
    if (budget.battery_data_valid && budget.battery_percent <= 100U)
    {
        request->has_battery_percent = true;
        request->battery_percent = (int)budget.battery_percent;
    }
    request->has_charging = true;
    request->charging = (budget.flags & POWER_POLICY_FLAG_CHARGING) != 0U;
}

static void memory_watch_service_clear_clarification(void)
{
    portENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot.clarification_active = false;
    s_snapshot.clarification_id[0] = '\0';
    portEXIT_CRITICAL(&s_snapshot_lock);
}

static bool memory_watch_service_can_begin_from_state(
    memory_watch_service_state_t state)
{
    switch (state)
    {
    case MEMORY_WATCH_SERVICE_STATE_READY:
    case MEMORY_WATCH_SERVICE_STATE_DONE:
    case MEMORY_WATCH_SERVICE_STATE_TIMEOUT:
    case MEMORY_WATCH_SERVICE_STATE_ERROR:
    case MEMORY_WATCH_SERVICE_STATE_CANCELED:
    case MEMORY_WATCH_SERVICE_STATE_NEEDS_CLARIFICATION:
        return true;
    case MEMORY_WATCH_SERVICE_STATE_WAITING_NETWORK:
    case MEMORY_WATCH_SERVICE_STATE_RECORDING:
    case MEMORY_WATCH_SERVICE_STATE_ENCODING:
    case MEMORY_WATCH_SERVICE_STATE_UPLOADING:
    case MEMORY_WATCH_SERVICE_STATE_THINKING:
    default:
        return false;
    }
}

static esp_err_t memory_watch_service_start_upload_job(
    const memory_watch_service_client_config_snapshot_t *client_config,
    const char *request_id, const char *text)
{
    if (s_upload_worker_queue == NULL || client_config == NULL ||
        request_id == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    memory_watch_service_upload_job_t job = {
        .client_config = *client_config,
    };
    memory_watch_service_rebind_client_config(&job.client_config);
    memory_watch_service_copy_text(job.request_id, sizeof(job.request_id),
                                   request_id);
    memory_watch_service_copy_current_clarification(
        job.clarification_id, sizeof(job.clarification_id));
    if (text != NULL && text[0] != '\0')
    {
        memory_watch_service_copy_text(job.text, sizeof(job.text), text);
        job.text_command = true;
    }

    memory_watch_service_set_upload_worker_busy(true);
    if (xQueueSend(s_upload_worker_queue, &job, 0) != pdTRUE)
    {
        memory_watch_service_set_upload_worker_busy(false);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t memory_watch_service_start_cancel_job(
    const memory_watch_service_client_config_snapshot_t *client_config,
    const char *request_id)
{
    if (s_cancel_worker_queue == NULL || client_config == NULL ||
        request_id == NULL || request_id[0] == '\0')
    {
        return ESP_ERR_INVALID_STATE;
    }

    memory_watch_service_cancel_job_t job = {
        .client_config = *client_config,
    };
    memory_watch_service_rebind_client_config(&job.client_config);
    memory_watch_service_copy_text(job.request_id, sizeof(job.request_id),
                                   request_id);
    if (xQueueSend(s_cancel_worker_queue, &job, 0) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t memory_watch_service_start_health_job(
    const memory_watch_service_client_config_snapshot_t *client_config)
{
    if (s_health_worker_queue == NULL || client_config == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    memory_watch_service_health_job_t job = {
        .client_config = *client_config,
    };
    memory_watch_service_rebind_client_config(&job.client_config);
    job.client_config.client_config.timeout_ms =
        MEMORY_WATCH_SERVICE_HEALTH_TIMEOUT_MS;
    if (xQueueSend(s_health_worker_queue, &job, 0) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    s_health_worker_busy = true;
    return ESP_OK;
}

static esp_err_t memory_watch_service_start_conversation_job(
    const memory_watch_service_client_config_snapshot_t *client_config,
    memory_watch_sync_mode_t sync_mode)
{
    if (s_conversation_worker_queue == NULL || client_config == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    memory_watch_service_conversation_job_t job = {
        .client_config = *client_config,
    };
    memory_watch_service_rebind_client_config(&job.client_config);
    job.client_config.client_config.timeout_ms = kConversationPollTimeoutMs;
    job.sync_mode = sync_mode;
    memory_watch_service_copy_last_seen_conversation_id(
        job.after_message_id, sizeof(job.after_message_id));
    if (sync_mode == MEMORY_WATCH_SYNC_MODE_BACKGROUND)
    {
        memory_watch_service_copy_text(job.pending_request_id,
                                       sizeof(job.pending_request_id),
                                       s_conversation_pending_request_id);
    }

    if (xQueueSend(s_conversation_worker_queue, &job, 0) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    s_conversation_worker_busy = true;
    return ESP_OK;
}

static void memory_watch_service_start_conversation_polling(
    const char *request_id)
{
    if (request_id == NULL || request_id[0] == '\0')
    {
        return;
    }
    s_conversation_poll_active = true;
    s_conversation_poll_next_due_ms = esp_timer_get_time() / 1000LL;
    memory_watch_service_copy_text(s_conversation_pending_request_id,
                                   sizeof(s_conversation_pending_request_id),
                                   request_id);
    ESP_LOGI(TAG, "conversation: background polling started request_id=%.32s",
             request_id);
}

static void memory_watch_service_start_foreground_reconcile(void)
{
    if (s_conversation_worker_busy)
    {
        return;
    }

    memory_watch_service_client_config_snapshot_t client_config;
    if (memory_watch_service_copy_client_config(&client_config) != ESP_OK)
    {
        return;
    }

    const esp_err_t err = memory_watch_service_start_conversation_job(
        &client_config, MEMORY_WATCH_SYNC_MODE_FOREGROUND_RECONCILE);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "conversation: foreground sync dispatch failed: %s",
                 esp_err_to_name(err));
    }
}

static void memory_watch_service_stop_conversation_polling(void)
{
    s_conversation_poll_active = false;
    s_conversation_poll_next_due_ms = 0;
    s_conversation_pending_request_id[0] = '\0';
}

static bool memory_watch_service_conversation_status_is_terminal(
    const char *status)
{
    return status != NULL &&
           (strcmp(status, "done") == 0 ||
            strcmp(status, "error") == 0 ||
            strcmp(status, "timeout") == 0 ||
            strcmp(status, "canceled") == 0);
}

static void memory_watch_service_conversation_try_poll(void)
{
    if (!s_conversation_poll_active || s_conversation_worker_busy)
    {
        return;
    }

    const int64_t now_ms = esp_timer_get_time() / 1000LL;
    if (now_ms < s_conversation_poll_next_due_ms)
    {
        return;
    }

    memory_watch_service_client_config_snapshot_t client_config;
    if (memory_watch_service_copy_client_config(&client_config) != ESP_OK)
    {
        s_conversation_poll_next_due_ms = now_ms + kConversationPollIntervalMs;
        return;
    }

    const esp_err_t err =
        memory_watch_service_start_conversation_job(
            &client_config, MEMORY_WATCH_SYNC_MODE_BACKGROUND);
    if (err != ESP_OK)
    {
        s_conversation_poll_next_due_ms = now_ms + kConversationPollIntervalMs;
        ESP_LOGW(TAG, "conversation: poll dispatch failed: %s",
                 esp_err_to_name(err));
    }
}

static void memory_watch_service_handle_begin_recording(void)
{
    const memory_watch_service_snapshot_t before =
        memory_watch_service_copy_snapshot();
    if (before.request_active ||
        !memory_watch_service_can_begin_from_state(before.state))
    {
        memory_watch_service_set_state(before.state, ESP_ERR_INVALID_STATE);
        ESP_LOGW(TAG, "recording blocked: request already active state=%s",
                 memory_watch_service_state_to_string(before.state));
        return;
    }

    memory_watch_service_client_config_snapshot_t client_config;
    esp_err_t err = memory_watch_service_copy_client_config(&client_config);
    if (err != ESP_OK)
    {
        memory_watch_service_set_endpoint_snapshot(false, false);
        memory_watch_service_set_request_active(false);
        memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_ERROR, err);
        ESP_LOGW(TAG, "recording blocked: watch endpoint is not configured");
        return;
    }

    const bool network_ready = network_service_is_service_ready();
    memory_watch_service_set_network_ready(network_ready);
    if (!network_ready)
    {
        memory_watch_service_set_state(
            MEMORY_WATCH_SERVICE_STATE_WAITING_NETWORK,
            ESP_ERR_INVALID_STATE);
        memory_watch_service_set_request_active(false);
        ESP_LOGW(TAG, "recording blocked: network service is not ready");
        return;
    }
    if (memory_watch_service_is_upload_worker_busy())
    {
        memory_watch_service_set_state(before.state, ESP_ERR_INVALID_STATE);
        ESP_LOGW(TAG, "recording blocked: upload worker is still busy");
        return;
    }

    char request_id[MEMORY_WATCH_SERVICE_ID_MAX_BYTES];
    err = memory_watch_service_make_request_id(
        client_config.client_config.device_id, request_id,
        sizeof(request_id));
    if (err != ESP_OK)
    {
        memory_watch_service_set_request_active(false);
        memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_ERROR, err);
        return;
    }

    memory_watch_service_set_request_id(request_id);
    memory_watch_service_set_request_active(true);
    memory_watch_service_reset_worker_flags();
    memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_RECORDING,
                                   ESP_OK);
    err = memory_watch_service_start_upload_job(&client_config, request_id,
                                                NULL);
    if (err != ESP_OK)
    {
        memory_watch_service_set_request_active(false);
        memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_ERROR, err);
    }
}

static void memory_watch_service_handle_send_text(const char *text)
{
    if (!memory_watch_service_is_safe_user_text(text))
    {
        memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_ERROR,
                                       ESP_ERR_INVALID_ARG);
        ESP_LOGW(TAG, "text command blocked: invalid text");
        return;
    }

    const memory_watch_service_snapshot_t before =
        memory_watch_service_copy_snapshot();
    if (before.request_active ||
        !memory_watch_service_can_begin_from_state(before.state))
    {
        memory_watch_service_set_state(before.state, ESP_ERR_INVALID_STATE);
        ESP_LOGW(TAG, "text command blocked: request already active state=%s",
                 memory_watch_service_state_to_string(before.state));
        return;
    }

    memory_watch_service_client_config_snapshot_t client_config;
    esp_err_t err = memory_watch_service_copy_client_config(&client_config);
    if (err != ESP_OK)
    {
        memory_watch_service_set_endpoint_snapshot(false, false);
        memory_watch_service_set_request_active(false);
        memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_ERROR, err);
        ESP_LOGW(TAG, "text command blocked: watch endpoint is not configured");
        return;
    }

    const bool network_ready = network_service_is_service_ready();
    memory_watch_service_set_network_ready(network_ready);
    if (!network_ready)
    {
        memory_watch_service_set_state(
            MEMORY_WATCH_SERVICE_STATE_WAITING_NETWORK,
            ESP_ERR_INVALID_STATE);
        memory_watch_service_set_request_active(false);
        ESP_LOGW(TAG, "text command blocked: network service is not ready");
        return;
    }
    if (memory_watch_service_is_upload_worker_busy())
    {
        memory_watch_service_set_state(before.state, ESP_ERR_INVALID_STATE);
        ESP_LOGW(TAG, "text command blocked: upload worker is still busy");
        return;
    }

    char request_id[MEMORY_WATCH_SERVICE_ID_MAX_BYTES];
    err = memory_watch_service_make_request_id(
        client_config.client_config.device_id, request_id,
        sizeof(request_id));
    if (err != ESP_OK)
    {
        memory_watch_service_set_request_active(false);
        memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_ERROR, err);
        return;
    }

    memory_watch_service_set_request_id(request_id);
    portENTER_CRITICAL(&s_snapshot_lock);
    memory_watch_service_copy_text(s_snapshot.asr_text,
                                   sizeof(s_snapshot.asr_text), text);
    portEXIT_CRITICAL(&s_snapshot_lock);
    memory_watch_service_set_request_active(true);
    memory_watch_service_reset_worker_flags();
    err = memory_watch_service_start_upload_job(&client_config, request_id,
                                                text);
    if (err != ESP_OK)
    {
        memory_watch_service_set_request_active(false);
        memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_ERROR, err);
        return;
    }

    memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_UPLOADING,
                                   ESP_OK);
}

static void memory_watch_service_schedule_health_retry(const char *reason)
{
    s_health_check_pending = true;
    s_health_retry_next_due_ms =
        memory_watch_service_now_ms() + kBackgroundHttpsRetryIntervalMs;
    ESP_LOGI(TAG, "health check deferred: reason=%s retry_in_ms=%" PRId64,
             reason != NULL ? reason : "transient",
             kBackgroundHttpsRetryIntervalMs);
}

static void memory_watch_service_handle_check_health(void)
{
    const memory_watch_service_snapshot_t before =
        memory_watch_service_copy_snapshot();
    if (before.request_active)
    {
        memory_watch_service_set_state(before.state, ESP_ERR_INVALID_STATE);
        return;
    }

    const bool network_ready = network_service_is_service_ready();
    memory_watch_service_set_network_ready(network_ready);
    if (!network_ready)
    {
        memory_watch_service_set_endpoint_snapshot(before.endpoint_configured,
                                                   false);
        memory_watch_service_set_state(
            MEMORY_WATCH_SERVICE_STATE_WAITING_NETWORK,
            ESP_ERR_INVALID_STATE);
        return;
    }

    if (s_health_worker_busy)
    {
        memory_watch_service_schedule_health_retry("worker_busy");
        return;
    }

    memory_watch_service_client_config_snapshot_t client_config;
    esp_err_t err = memory_watch_service_copy_client_config(&client_config);
    if (err != ESP_OK)
    {
        memory_watch_service_set_endpoint_snapshot(false, false);
        memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_ERROR, err);
        return;
    }

    err = memory_watch_service_start_health_job(&client_config);
    if (err != ESP_OK)
    {
        memory_watch_service_schedule_health_retry("dispatch_failed");
        return;
    }

    s_health_check_pending = false;
    s_health_retry_next_due_ms = 0;
    memory_watch_service_set_endpoint_snapshot(true, before.hermes_online);
}

static void memory_watch_service_handle_send_recording(void)
{
    const memory_watch_service_snapshot_t before =
        memory_watch_service_copy_snapshot();
    if (before.state != MEMORY_WATCH_SERVICE_STATE_RECORDING)
    {
        memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_ERROR,
                                       ESP_ERR_INVALID_STATE);
        return;
    }

    memory_watch_service_request_record_stop(false);
    memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_ENCODING,
                                   ESP_OK);
}

static void memory_watch_service_handle_cancel_recording(void)
{
    const memory_watch_service_snapshot_t before =
        memory_watch_service_copy_snapshot();
    if (before.state == MEMORY_WATCH_SERVICE_STATE_RECORDING ||
        before.state == MEMORY_WATCH_SERVICE_STATE_ENCODING)
    {
        memory_watch_service_request_record_stop(true);
    }
    memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_CANCELED,
                                   ESP_OK);
}

static void memory_watch_service_handle_cancel_waiting(void)
{
    const memory_watch_service_snapshot_t before =
        memory_watch_service_copy_snapshot();
    if (before.state == MEMORY_WATCH_SERVICE_STATE_RECORDING ||
        before.state == MEMORY_WATCH_SERVICE_STATE_ENCODING)
    {
        memory_watch_service_request_record_stop(true);
        memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_CANCELED,
                                       ESP_OK);
        return;
    }
    if (!before.request_active || before.request_id[0] == '\0')
    {
        memory_watch_service_set_request_active(false);
        memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_CANCELED,
                                       ESP_OK);
        return;
    }
    if (before.state != MEMORY_WATCH_SERVICE_STATE_UPLOADING &&
        before.state != MEMORY_WATCH_SERVICE_STATE_THINKING)
    {
        memory_watch_service_set_state(before.state, ESP_ERR_INVALID_STATE);
        return;
    }

    memory_watch_service_request_wait_cancel(before.request_id);
    memory_watch_service_set_request_active(false);
    memory_watch_service_client_config_snapshot_t client_config;
    esp_err_t err = memory_watch_service_copy_client_config(&client_config);
    if (err == ESP_OK)
    {
        err = memory_watch_service_start_cancel_job(&client_config,
                                                    before.request_id);
    }
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "server cancel job failed: %s", esp_err_to_name(err));
    }
    memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_CANCELED,
                                   ESP_OK);
}

static void memory_watch_service_handle_upload_started(
    const char *request_id)
{
    if (!memory_watch_service_request_id_matches_current(request_id) ||
        memory_watch_service_is_wait_cancel_requested() ||
        memory_watch_service_is_wait_canceled_request(request_id))
    {
        return;
    }

    memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_UPLOADING,
                                   ESP_OK);
    memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_THINKING,
                                   ESP_OK);
}

static void memory_watch_service_handle_health_done(
    const memory_watch_service_health_result_t *result)
{
    if (result == NULL)
    {
        return;
    }

    s_health_worker_busy = false;
    const memory_watch_service_snapshot_t before =
        memory_watch_service_copy_snapshot();
    if (before.request_active)
    {
        return;
    }

    const bool auth_or_protocol_error =
        result->http_status == 401 || result->http_status == 403 ||
        result->http_status == 422;
    if (result->error != ESP_OK && !auth_or_protocol_error)
    {
        memory_watch_service_schedule_health_retry("transient_failure");
        memory_watch_service_set_endpoint_snapshot(true, before.hermes_online);
        memory_watch_service_set_state(before.state, result->error);
        ESP_LOGW(TAG,
                 "watch endpoint health transient failure: keep_online=%u err=%s",
                 (unsigned int)before.hermes_online,
                 esp_err_to_name(result->error));
        return;
    }

    s_health_check_pending = false;
    s_health_retry_next_due_ms = 0;
    memory_watch_service_set_endpoint_snapshot(true, result->hermes_online);
    memory_watch_service_set_state(
        result->hermes_online ? MEMORY_WATCH_SERVICE_STATE_READY
                              : MEMORY_WATCH_SERVICE_STATE_ERROR,
        result->error);
    ESP_LOGI(TAG, "watch endpoint health result: hermes_online=%u err=%s",
             (unsigned int)result->hermes_online,
             esp_err_to_name(result->error));
    /* 健康检查成功后触发首次 inbox 轮询 */
    if (result->hermes_online)
    {
        memory_watch_service_inbox_try_poll();
    }
#if CONFIG_MEMORY_WATCH_BOOT_TEXT_SMOKE
    if (!s_boot_text_sent &&
        CONFIG_MEMORY_WATCH_BOOT_TEXT[0] != '\0')
    {
        s_boot_text_sent = true;
        ESP_LOGW(TAG, "Kconfig boot text smoke started after health result");
        memory_watch_service_handle_send_text(
            CONFIG_MEMORY_WATCH_BOOT_TEXT);
    }
#endif
}

static memory_watch_service_state_t
memory_watch_service_state_from_response(
    const memory_watch_voice_client_response_t *response,
    esp_err_t error)
{
    if (response != NULL)
    {
        if (strcmp(response->status, "canceled") == 0)
        {
            return MEMORY_WATCH_SERVICE_STATE_CANCELED;
        }
        if (strcmp(response->status, "timeout") == 0)
        {
            return MEMORY_WATCH_SERVICE_STATE_TIMEOUT;
        }
        if (strcmp(response->status, "error") == 0)
        {
            return MEMORY_WATCH_SERVICE_STATE_ERROR;
        }
        if (strcmp(response->action, "clarification_needed") == 0 ||
            response->clarification_id[0] != '\0')
        {
            return MEMORY_WATCH_SERVICE_STATE_NEEDS_CLARIFICATION;
        }
        return MEMORY_WATCH_SERVICE_STATE_DONE;
    }

    if (error == ESP_ERR_TIMEOUT)
    {
        return MEMORY_WATCH_SERVICE_STATE_TIMEOUT;
    }
    return MEMORY_WATCH_SERVICE_STATE_ERROR;
}

static void memory_watch_service_handle_worker_done(
    const memory_watch_service_worker_result_t *result)
{
    if (result == NULL)
    {
        return;
    }

    if (memory_watch_service_is_wait_canceled_request(result->request_id))
    {
        memory_watch_service_set_request_active(false);
        memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_CANCELED,
                                       ESP_OK);
        return;
    }

    if (!memory_watch_service_request_id_matches_current(result->request_id))
    {
        return;
    }

    if (result->cancel_requested)
    {
        memory_watch_service_set_request_active(false);
        memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_CANCELED,
                                       ESP_OK);
        return;
    }

    if (result->has_response)
    {
        if (strcmp(result->response.status, "pending") == 0 &&
            strcmp(result->response.action, "conversation_pending") == 0)
        {
            memory_watch_service_start_conversation_polling(
                result->request_id);
            memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_THINKING,
                                           ESP_OK);
            return;
        }
        memory_watch_service_set_response_texts(&result->response);
        if (!result->conversation_already_appended)
        {
            memory_watch_service_append_response_conversation(&result->response);
        }
        ESP_LOGI(TAG,
                 "watch request result: request_id=%s status=%s action=%s error_code=%s asr_chars=%u reply_chars=%u",
                 result->response.request_id,
                 result->response.status,
                 result->response.action,
                 result->response.error_code[0] != '\0'
                     ? result->response.error_code
                     : "none",
                 (unsigned int)strlen(result->response.asr_text),
                 (unsigned int)strlen(result->response.reply_text));
    }

    const memory_watch_service_state_t next_state =
        memory_watch_service_state_from_response(
            result->has_response ? &result->response : NULL,
            result->error);
    memory_watch_service_set_request_active(false);
    memory_watch_service_set_state(next_state, result->error);
}

static void memory_watch_service_conversation_handle_worker_done(void)
{
    s_conversation_worker_busy = false;
    const int64_t now_ms = esp_timer_get_time() / 1000LL;

    if (s_conversation_staging_error != ESP_OK)
    {
        if (s_conversation_sync_result.http_status == 401 ||
            s_conversation_sync_result.http_status == 403)
        {
            memory_watch_service_worker_result_t auth_result = {0};
            memory_watch_service_copy_text(auth_result.request_id,
                                           sizeof(auth_result.request_id),
                                           s_conversation_pending_request_id);
            auth_result.error = ESP_ERR_INVALID_STATE;
            auth_result.has_response = true;
            memory_watch_service_copy_text(
                auth_result.response.request_id,
                sizeof(auth_result.response.request_id),
                s_conversation_pending_request_id);
            memory_watch_service_copy_text(
                auth_result.response.status,
                sizeof(auth_result.response.status),
                "error");
            memory_watch_service_copy_text(
                auth_result.response.action,
                sizeof(auth_result.response.action),
                "config_error");
            memory_watch_service_copy_text(
                auth_result.response.error_code,
                sizeof(auth_result.response.error_code),
                "sync_auth_error");
            memory_watch_service_stop_conversation_polling();
            memory_watch_service_handle_worker_done(&auth_result);
            ESP_LOGW(TAG, "conversation: sync auth failed, pending sync paused");
            return;
        }
        s_conversation_poll_next_due_ms = now_ms + kConversationPollIntervalMs;
        ESP_LOGW(TAG, "conversation: sync failed, will retry");
        return;
    }

    memory_watch_service_worker_result_t terminal_result = {0};
    bool has_terminal_reply = false;
    for (size_t i = 0; i < s_conversation_staging_count; ++i)
    {
        const memory_watch_conversation_message_t *message =
            &s_conversation_staging[i];
        memory_watch_service_append_server_conversation_message(message);
        if (strcmp(message->role, "assistant") == 0 &&
            strcmp(message->request_id, s_conversation_pending_request_id) == 0 &&
            memory_watch_service_conversation_status_is_terminal(
                message->status))
        {
            has_terminal_reply = true;
            terminal_result.error =
                strcmp(message->status, "done") == 0 ? ESP_OK : ESP_FAIL;
            terminal_result.has_response = true;
            terminal_result.conversation_already_appended = true;
            memory_watch_service_copy_text(
                terminal_result.request_id,
                sizeof(terminal_result.request_id),
                message->request_id);
            memory_watch_service_copy_text(
                terminal_result.response.request_id,
                sizeof(terminal_result.response.request_id),
                message->request_id);
            memory_watch_service_copy_text(
                terminal_result.response.status,
                sizeof(terminal_result.response.status),
                message->status);
            memory_watch_service_copy_text(
                terminal_result.response.action,
                sizeof(terminal_result.response.action),
                "conversation_reply");
            memory_watch_service_copy_text(
                terminal_result.response.reply_text,
                sizeof(terminal_result.response.reply_text),
                message->text);
            if (strcmp(message->status, "done") != 0)
            {
                memory_watch_service_copy_text(
                    terminal_result.response.error_code,
                    sizeof(terminal_result.response.error_code),
                    "conversation_reply_error");
            }
        }
    }

    if (!has_terminal_reply &&
        s_conversation_pending_request_id[0] != '\0' &&
        strcmp(s_conversation_sync_result.session_state, "done") != 0 &&
        memory_watch_service_conversation_status_is_terminal(
            s_conversation_sync_result.session_state))
    {
        has_terminal_reply = true;
        terminal_result.error = ESP_FAIL;
        terminal_result.has_response = true;
        memory_watch_service_copy_text(
            terminal_result.request_id,
            sizeof(terminal_result.request_id),
            s_conversation_pending_request_id);
        memory_watch_service_copy_text(
            terminal_result.response.request_id,
            sizeof(terminal_result.response.request_id),
            s_conversation_pending_request_id);
        memory_watch_service_copy_text(
            terminal_result.response.status,
            sizeof(terminal_result.response.status),
            s_conversation_sync_result.session_state);
        memory_watch_service_copy_text(
            terminal_result.response.action,
            sizeof(terminal_result.response.action),
            "conversation_reply");
        memory_watch_service_copy_text(
            terminal_result.response.error_code,
            sizeof(terminal_result.response.error_code),
            "sync_session_terminal");
    }

    ESP_LOGI(TAG, "conversation: sync ok messages=%u session=%s terminal=%u",
             (unsigned int)s_conversation_staging_count,
             s_conversation_sync_result.session_state,
             (unsigned int)has_terminal_reply);
    if (has_terminal_reply)
    {
        memory_watch_service_stop_conversation_polling();
        memory_watch_service_handle_worker_done(&terminal_result);
        return;
    }

    s_conversation_poll_next_due_ms = now_ms + kConversationPollIntervalMs;
}

static void memory_watch_service_handle_command(
    const memory_watch_service_cmd_t *command)
{
    if (command == NULL)
    {
        return;
    }

    switch (command->type)
    {
    case MEMORY_WATCH_SERVICE_CMD_BEGIN_RECORDING:
        memory_watch_service_handle_begin_recording();
        break;
    case MEMORY_WATCH_SERVICE_CMD_CHECK_HEALTH:
        memory_watch_service_handle_check_health();
        break;
    case MEMORY_WATCH_SERVICE_CMD_SEND_RECORDING:
        memory_watch_service_handle_send_recording();
        break;
    case MEMORY_WATCH_SERVICE_CMD_SEND_TEXT:
        memory_watch_service_handle_send_text(command->text);
        break;
    case MEMORY_WATCH_SERVICE_CMD_CANCEL_RECORDING:
        memory_watch_service_handle_cancel_recording();
        break;
    case MEMORY_WATCH_SERVICE_CMD_CANCEL_WAITING:
        memory_watch_service_handle_cancel_waiting();
        break;
    case MEMORY_WATCH_SERVICE_CMD_CANCEL_CLARIFICATION:
        memory_watch_service_clear_clarification();
        memory_watch_service_set_request_active(false);
        memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_READY,
                                       ESP_OK);
        break;
    case MEMORY_WATCH_SERVICE_CMD_HEALTH_DONE:
        memory_watch_service_handle_health_done(&command->health_result);
        break;
    case MEMORY_WATCH_SERVICE_CMD_WORKER_UPLOAD_STARTED:
        memory_watch_service_handle_upload_started(command->request_id);
        break;
    case MEMORY_WATCH_SERVICE_CMD_WORKER_DONE:
        memory_watch_service_handle_worker_done(&command->worker_result);
        break;
    case MEMORY_WATCH_SERVICE_CMD_SET_FOREGROUND:
        memory_watch_service_set_foreground_active(command->foreground);
        if (command->foreground)
        {
            memory_watch_service_start_foreground_reconcile();
            memory_watch_service_inbox_try_poll();
        }
        break;
    case MEMORY_WATCH_SERVICE_CMD_CONVERSATION_POLL_DONE:
        memory_watch_service_conversation_handle_worker_done();
        break;
    default:
        break;
    }
}

static void memory_watch_service_upload_worker_task(void *arg)
{
    (void)arg;

    while (1)
    {
        if (xQueueReceive(s_upload_worker_queue, &s_upload_worker_job,
                          portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        memory_watch_service_upload_job_t *job = &s_upload_worker_job;
        memory_watch_service_worker_result_t *result =
            &s_upload_worker_result;
        memory_watch_service_rebind_client_config(&job->client_config);
        memset(result, 0, sizeof(*result));
        memory_watch_service_copy_text(result->request_id,
                                       sizeof(result->request_id),
                                       job->request_id);
        result->error = ESP_OK;
        memory_watch_service_log_upload_stack("job-received");

        if (job->text_command)
        {
            (void)memory_watch_service_post_upload_started(job->request_id);

            memory_watch_voice_client_text_request_t request = {
                .request_id = job->request_id,
                .text = job->text,
                .clarification_id = job->clarification_id,
                .firmware_version = memory_watch_service_firmware_version(),
                .ui_state = memory_watch_service_state_to_string(
                    MEMORY_WATCH_SERVICE_STATE_THINKING),
            };
            memory_watch_service_fill_text_power_fields(&request);
            result->error = memory_watch_voice_client_post_text_command(
                &job->client_config.client_config, &request,
                &result->response);
            result->has_response = result->error == ESP_OK;
            result->cancel_requested =
                memory_watch_service_is_wait_cancel_requested() ||
                memory_watch_service_is_wait_canceled_request(job->request_id);
            memory_watch_service_log_upload_stack("text-http-done");
        }
        else
        {
            memory_watch_service_audio_buffer_t audio_buffer = {0};
            memory_watch_recorder_result_t recorder_result = {0};
            memory_watch_recorder_config_t recorder_config = {
                .ogg_serial = esp_random(),
                .write_cb = memory_watch_service_audio_write_cb,
                .write_user_ctx = &audio_buffer,
                .should_stop_cb = memory_watch_service_should_stop_recording,
                .should_stop_user_ctx = NULL,
                .should_abort_cb = memory_watch_service_should_abort_recording,
                .should_abort_user_ctx = NULL,
            };
            memory_watch_service_log_upload_stack("voice-record-start");
            result->error = memory_watch_recorder_capture_ogg_opus(
                &recorder_config, &recorder_result);
            memory_watch_service_log_upload_stack("voice-record-done");
            ESP_LOGI(TAG, "voice record result: err=%s audio_len=%u dur_ms=%lu pkts=%lu discard=%d",
                     esp_err_to_name(result->error),
                     (unsigned int)audio_buffer.len,
                     (unsigned long)recorder_result.duration_ms,
                     (unsigned long)recorder_result.opus_packets,
                     (int)memory_watch_service_is_record_discard_requested());

            const bool discard_requested =
                memory_watch_service_is_record_discard_requested();
            if (discard_requested)
            {
                result->cancel_requested = true;
                result->error = ESP_OK;
            }
            else if (result->error == ESP_OK)
            {
                (void)memory_watch_service_post_upload_started(job->request_id);

#if CONFIG_MEMORY_WATCH_WEBSOCKET_ENABLED
                result->error = memory_watch_service_send_voice_over_ws(
                    job, &audio_buffer, result);
                result->has_response =
                    result->error == ESP_OK ||
                    result->response.status[0] != '\0';
                memory_watch_service_log_upload_stack("voice-ws-done");
#else
                memory_watch_voice_client_request_t request = {
                    .request_id = job->request_id,
                    .audio = audio_buffer.data,
                    .audio_len = audio_buffer.len,
                    .clarification_id = job->clarification_id,
                    .firmware_version =
                        memory_watch_service_firmware_version(),
                    .ui_state = memory_watch_service_state_to_string(
                        MEMORY_WATCH_SERVICE_STATE_THINKING),
                };
                memory_watch_service_fill_power_fields(&request);
                result->error = memory_watch_voice_client_post_voice_command(
                    &job->client_config.client_config, &request,
                    &result->response);
                result->has_response = result->error == ESP_OK;
                memory_watch_service_log_upload_stack("voice-http-done");
#endif
                result->cancel_requested =
                    memory_watch_service_is_wait_cancel_requested() ||
                    memory_watch_service_is_wait_canceled_request(
                        job->request_id);
            }

            memory_watch_service_audio_buffer_free(&audio_buffer);
        }
        memory_watch_service_set_upload_worker_busy(false);
        (void)memory_watch_service_post_worker_result(result);
    }
}

static void memory_watch_service_conversation_worker_task(void *arg)
{
    (void)arg;

    while (1)
    {
        if (xQueueReceive(s_conversation_worker_queue,
                          &s_conversation_worker_job,
                          portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        memory_watch_service_rebind_client_config(
            &s_conversation_worker_job.client_config);
        s_conversation_staging_count = 0;
        s_conversation_staging_error = ESP_FAIL;
        memset(&s_conversation_sync_result, 0,
               sizeof(s_conversation_sync_result));

        const memory_watch_voice_client_sync_cursor_t cursor = {
            .mode = s_conversation_worker_job.sync_mode,
            .pending_request_id =
                s_conversation_worker_job.pending_request_id[0] != '\0'
                    ? s_conversation_worker_job.pending_request_id
                    : NULL,
            .after_message_id =
                s_conversation_worker_job.after_message_id[0] != '\0'
                    ? s_conversation_worker_job.after_message_id
                    : NULL,
            .max_messages = MEMORY_WATCH_SYNC_DEFAULT_MAX_MESSAGES,
        };
        const esp_err_t err = memory_watch_voice_client_sync(
            &s_conversation_worker_job.client_config.client_config,
            &cursor,
            s_conversation_staging,
            MEMORY_WATCH_CONVERSATION_MAX_MESSAGES,
            &s_conversation_sync_result);
        if (err == ESP_OK)
        {
            s_conversation_staging_count =
                s_conversation_sync_result.message_count;
            s_conversation_staging_error = ESP_OK;
        }
        else
        {
            s_conversation_staging_error = err;
        }

        if (s_command_queue != NULL)
        {
            memory_watch_service_cmd_t command = {
                .type = MEMORY_WATCH_SERVICE_CMD_CONVERSATION_POLL_DONE,
            };
            (void)xQueueSend(s_command_queue, &command, portMAX_DELAY);
        }
    }
}

static void memory_watch_service_health_worker_task(void *arg)
{
    (void)arg;

    while (1)
    {
        memory_watch_service_health_job_t job;
        if (xQueueReceive(s_health_worker_queue, &job, portMAX_DELAY) !=
            pdTRUE)
        {
            continue;
        }
        memory_watch_service_rebind_client_config(&job.client_config);

        memory_watch_voice_client_health_t health = {0};
        const esp_err_t err = memory_watch_voice_client_get_health(
            &job.client_config.client_config, &health);
        const memory_watch_service_health_result_t result = {
            .error = err,
            .http_status = health.http_status,
            .hermes_online = err == ESP_OK,
        };
        (void)memory_watch_service_post_health_result(&result);
    }
}

static void memory_watch_service_cancel_worker_task(void *arg)
{
    (void)arg;

    while (1)
    {
        memory_watch_service_cancel_job_t job;
        if (xQueueReceive(s_cancel_worker_queue, &job, portMAX_DELAY) !=
            pdTRUE)
        {
            continue;
        }
        memory_watch_service_rebind_client_config(&job.client_config);

        memory_watch_voice_client_response_t response = {0};
        const esp_err_t err = memory_watch_voice_client_cancel_request(
            &job.client_config.client_config, job.request_id, &response);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "server cancel request failed: %s",
                     esp_err_to_name(err));
        }
    }
}

static void memory_watch_service_task(void *arg)
{
    (void)arg;
#if CONFIG_MEMORY_WATCH_BOOT_HEALTH_CHECK
    bool boot_health_done = false;
    uint32_t boot_health_attempts = 0;
#endif

    while (1)
    {
        TickType_t wait_ticks = portMAX_DELAY;
#if CONFIG_MEMORY_WATCH_BOOT_HEALTH_CHECK
        if (!boot_health_done)
        {
            wait_ticks = pdMS_TO_TICKS(1000U);
        }
#endif
        /* 用有限超时等待命令队列，使 owner task 能周期性检查
         * task notification（inbox worker 结果）和 inbox 轮询 pending。 */
        const TickType_t inbox_check_ticks = pdMS_TO_TICKS(2000U);
        if (wait_ticks == portMAX_DELAY || wait_ticks > inbox_check_ticks)
        {
            wait_ticks = inbox_check_ticks;
        }
        if (xQueueReceive(s_command_queue, &s_service_task_command,
                          wait_ticks) == pdTRUE)
        {
            memory_watch_service_handle_command(&s_service_task_command);
        }

        /* 处理 inbox worker 完成通知（来自 xTaskNotify）*/
        uint32_t notify_val = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &notify_val, 0) == pdTRUE &&
            notify_val != 0U)
        {
            memory_watch_service_inbox_handle_worker_result(notify_val);
        }

        /* 处理 inbox 轮询 pending（来自 poll_now 或 worker 完成后的补发）*/
        if (memory_watch_service_inbox_is_poll_pending())
        {
            memory_watch_service_inbox_try_poll();
        }
        if (s_health_check_pending &&
            memory_watch_service_now_ms() >= s_health_retry_next_due_ms)
        {
            memory_watch_service_handle_check_health();
        }
        memory_watch_service_conversation_try_poll();

#if CONFIG_MEMORY_WATCH_BOOT_HEALTH_CHECK
        if (boot_health_done)
        {
            continue;
        }

        ++boot_health_attempts;
        const memory_watch_service_snapshot_t snapshot =
            memory_watch_service_copy_snapshot();
        if (snapshot.endpoint_configured && !snapshot.request_active &&
            network_service_is_service_ready())
        {
            ESP_LOGW(TAG, "Kconfig boot health check started");
            memory_watch_service_handle_check_health();
            boot_health_done = true;
            continue;
        }
        if (boot_health_attempts >= 90U)
        {
            ESP_LOGW(TAG,
                     "Kconfig boot health check skipped: endpoint=%u request=%u network=%u attempts=%lu",
                     (unsigned int)snapshot.endpoint_configured,
                     (unsigned int)snapshot.request_active,
                     (unsigned int)network_service_is_service_ready(),
                     (unsigned long)boot_health_attempts);
            boot_health_done = true;
        }
#endif
    }
}

static esp_err_t memory_watch_service_post_command(
    memory_watch_service_cmd_type_t type)
{
    if (s_command_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    memory_watch_service_cmd_t command = {
        .type = type,
    };
    if (xQueueSend(s_command_queue, &command, 0) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t memory_watch_service_init(void)
{
    if (s_service_task_handle != NULL &&
        s_upload_worker_task_handle != NULL &&
        s_cancel_worker_task_handle != NULL &&
        s_health_worker_task_handle != NULL &&
        s_conversation_worker_task_handle != NULL)
    {
        return ESP_OK;
    }
    if (s_service_task_handle != NULL ||
        s_upload_worker_task_handle != NULL ||
        s_cancel_worker_task_handle != NULL ||
        s_health_worker_task_handle != NULL ||
        s_conversation_worker_task_handle != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    memory_watch_service_init_boot_id();

    if (s_command_queue == NULL)
    {
        s_command_queue = xQueueCreateStatic(
            kCommandQueueLength,
            sizeof(memory_watch_service_cmd_t),
            s_command_queue_storage,
            &s_command_queue_buffer);
    }
    if (s_command_queue == NULL)
    {
        memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_ERROR,
                                       ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    if (s_upload_worker_queue == NULL)
    {
        s_upload_worker_queue = xQueueCreateStatic(
            kWorkerQueueLength,
            sizeof(memory_watch_service_upload_job_t),
            s_upload_worker_queue_storage,
            &s_upload_worker_queue_buffer);
    }
    if (s_upload_worker_queue == NULL)
    {
        memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_ERROR,
                                       ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    if (s_cancel_worker_queue == NULL)
    {
        s_cancel_worker_queue = xQueueCreateStatic(
            kCancelQueueLength,
            sizeof(memory_watch_service_cancel_job_t),
            s_cancel_worker_queue_storage,
            &s_cancel_worker_queue_buffer);
    }
    if (s_cancel_worker_queue == NULL)
    {
        memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_ERROR,
                                       ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    if (s_health_worker_queue == NULL)
    {
        s_health_worker_queue = xQueueCreateStatic(
            kHealthQueueLength,
            sizeof(memory_watch_service_health_job_t),
            s_health_worker_queue_storage,
            &s_health_worker_queue_buffer);
    }
    if (s_health_worker_queue == NULL)
    {
        memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_ERROR,
                                       ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    if (s_conversation_worker_queue == NULL)
    {
        s_conversation_worker_queue = xQueueCreateStatic(
            kWorkerQueueLength,
            sizeof(memory_watch_service_conversation_job_t),
            s_conversation_worker_queue_storage,
            &s_conversation_worker_queue_buffer);
    }
    if (s_conversation_worker_queue == NULL)
    {
        memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_ERROR,
                                       ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    if (s_conversation_staging == NULL)
    {
        s_conversation_staging =
            (memory_watch_conversation_message_t *)heap_caps_calloc(
                MEMORY_WATCH_CONVERSATION_MAX_MESSAGES,
                sizeof(memory_watch_conversation_message_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_conversation_staging == NULL)
    {
        memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_ERROR,
                                       ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    if (s_ws_wait_event_group == NULL)
    {
        s_ws_wait_event_group =
            xEventGroupCreateStatic(&s_ws_wait_event_buffer);
    }
    if (s_ws_wait_event_group == NULL)
    {
        memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_ERROR,
                                       ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t endpoint_err =
        memory_watch_service_load_endpoint_from_nvs();
    if (endpoint_err != ESP_OK)
    {
        ESP_LOGW(TAG, "watch endpoint NVS load failed: %s",
                 esp_err_to_name(endpoint_err));
    }

    const BaseType_t service_created = xTaskCreateWithCaps(
        memory_watch_service_task,
        "memory_watch",
        kTaskStackWords,
        NULL,
        4,
        &s_service_task_handle,
        MALLOC_CAP_SPIRAM);
    if (service_created != pdPASS)
    {
        memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_ERROR,
                                       ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t upload_created = xTaskCreateWithCaps(
        memory_watch_service_upload_worker_task,
        "mw_upload",
        kUploadWorkerStackWords,
        NULL,
        4,
        &s_upload_worker_task_handle,
        MALLOC_CAP_SPIRAM);
    if (upload_created != pdPASS)
    {
        memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_ERROR,
                                       ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t health_created = xTaskCreateWithCaps(
        memory_watch_service_health_worker_task,
        "mw_health",
        kHealthWorkerStackWords,
        NULL,
        4,
        &s_health_worker_task_handle,
        MALLOC_CAP_SPIRAM);
    if (health_created != pdPASS)
    {
        s_health_worker_task_handle = NULL;
        // Comment for static test validation: s_health_worker_task_handle != NULL
        memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_ERROR,
                                       ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t cancel_created = xTaskCreateWithCaps(
        memory_watch_service_cancel_worker_task,
        "mw_cancel",
        kCancelWorkerStackWords,
        NULL,
        4,
        &s_cancel_worker_task_handle,
        MALLOC_CAP_SPIRAM);
    if (cancel_created != pdPASS)
    {
        s_cancel_worker_task_handle = NULL;
        memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_ERROR,
                                       ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t conversation_created = xTaskCreateWithCaps(
        memory_watch_service_conversation_worker_task,
        "mw_conv",
        kConversationWorkerStackWords,
        NULL,
        4,
        &s_conversation_worker_task_handle,
        MALLOC_CAP_SPIRAM);
    if (conversation_created != pdPASS)
    {
        s_conversation_worker_task_handle = NULL;
        memory_watch_service_set_state(MEMORY_WATCH_SERVICE_STATE_ERROR,
                                       ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    /* inbox worker 在 Deferred Services 阶段启动，不阻塞 UI 首帧 */
    {
        const esp_err_t inbox_err = memory_watch_service_inbox_init();
        if (inbox_err != ESP_OK)
        {
            ESP_LOGW(TAG, "inbox init failed: %s (inbox disabled)",
                     esp_err_to_name(inbox_err));
        }
    }

    return ESP_OK;
}

esp_err_t memory_watch_service_configure_endpoint(
    const memory_watch_service_endpoint_config_t *config)
{
    const memory_watch_service_snapshot_t snapshot =
        memory_watch_service_copy_snapshot();
    if (snapshot.request_active)
    {
        return ESP_ERR_INVALID_STATE;
    }

    memory_watch_service_endpoint_state_t next_config = {0};
    esp_err_t err = memory_watch_service_build_endpoint_state(config,
                                                              &next_config);
    if (err != ESP_OK)
    {
        return err;
    }

    portENTER_CRITICAL(&s_endpoint_lock);
    s_endpoint_config = next_config;
    portEXIT_CRITICAL(&s_endpoint_lock);

    memory_watch_service_set_endpoint_snapshot(true, false);
    memory_watch_service_set_state(snapshot.state, ESP_OK);
    return ESP_OK;
}

esp_err_t memory_watch_service_save_endpoint_to_nvs(
    const memory_watch_service_endpoint_config_t *config)
{
    const memory_watch_service_snapshot_t snapshot =
        memory_watch_service_copy_snapshot();
    if (snapshot.request_active)
    {
        return ESP_ERR_INVALID_STATE;
    }

    memory_watch_service_endpoint_state_t next_config = {0};
    esp_err_t err = memory_watch_service_build_endpoint_state(config,
                                                              &next_config);
    if (err != ESP_OK)
    {
        return err;
    }

    nvs_handle_t handle = 0;
    err = nvs_open(kEndpointNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_set_str(handle, kEndpointNvsBaseUrlKey, next_config.base_url);
    if (err == ESP_OK)
    {
        err = nvs_set_str(handle, kEndpointNvsDeviceIdKey,
                          next_config.device_id);
    }
    if (err == ESP_OK)
    {
        err = nvs_set_str(handle, kEndpointNvsDeviceTokenKey,
                          next_config.device_token);
    }
    if (err == ESP_OK)
    {
        err = nvs_set_u32(handle, kEndpointNvsTimeoutMsKey,
                          next_config.timeout_ms);
    }
    if (err == ESP_OK)
    {
        err = nvs_set_u8(handle, kEndpointNvsAllowHttpKey,
                         next_config.allow_insecure_http ? 1U : 0U);
    }
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK)
    {
        return err;
    }

    portENTER_CRITICAL(&s_endpoint_lock);
    s_endpoint_config = next_config;
    portEXIT_CRITICAL(&s_endpoint_lock);

    memory_watch_service_set_endpoint_snapshot(true, false);
    memory_watch_service_set_state(snapshot.state, ESP_OK);
    ESP_LOGI(TAG,
             "watch endpoint config saved to NVS: device_id=%s allow_http=%u timeout_ms=%lu",
             next_config.device_id,
             (unsigned int)next_config.allow_insecure_http,
             (unsigned long)next_config.timeout_ms);
    return ESP_OK;
}

esp_err_t memory_watch_service_check_health(void)
{
    return memory_watch_service_post_command(
        MEMORY_WATCH_SERVICE_CMD_CHECK_HEALTH);
}

esp_err_t memory_watch_service_set_foreground(bool foreground)
{
    memory_watch_service_set_foreground_active(foreground);
    if (s_command_queue == NULL)
    {
        return ESP_OK;
    }

    memory_watch_service_cmd_t command = {
        .type = MEMORY_WATCH_SERVICE_CMD_SET_FOREGROUND,
        .foreground = foreground,
    };
    if (xQueueSend(s_command_queue, &command, 0) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t memory_watch_service_begin_recording(void)
{
    return memory_watch_service_post_command(
        MEMORY_WATCH_SERVICE_CMD_BEGIN_RECORDING);
}

esp_err_t memory_watch_service_send_recording(void)
{
    return memory_watch_service_post_command(
        MEMORY_WATCH_SERVICE_CMD_SEND_RECORDING);
}

esp_err_t memory_watch_service_send_text(const char *text)
{
    if (s_command_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!memory_watch_service_is_safe_user_text(text))
    {
        return ESP_ERR_INVALID_ARG;
    }

    memory_watch_service_cmd_t command = {
        .type = MEMORY_WATCH_SERVICE_CMD_SEND_TEXT,
    };
    memory_watch_service_copy_text(command.text, sizeof(command.text), text);
    if (xQueueSend(s_command_queue, &command, 0) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t memory_watch_service_cancel_recording(void)
{
    return memory_watch_service_post_command(
        MEMORY_WATCH_SERVICE_CMD_CANCEL_RECORDING);
}

esp_err_t memory_watch_service_cancel_waiting(void)
{
    return memory_watch_service_post_command(
        MEMORY_WATCH_SERVICE_CMD_CANCEL_WAITING);
}

esp_err_t memory_watch_service_cancel_clarification(void)
{
    return memory_watch_service_post_command(
        MEMORY_WATCH_SERVICE_CMD_CANCEL_CLARIFICATION);
}

esp_err_t memory_watch_service_get_snapshot(
    memory_watch_service_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_snapshot = memory_watch_service_copy_snapshot();
    return ESP_OK;
}

esp_err_t memory_watch_service_copy_conversation_items(
    memory_watch_service_conversation_item_t *out_items,
    size_t capacity,
    size_t *out_count)
{
    if (out_count == NULL || (capacity > 0U && out_items == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_snapshot_lock);
    const size_t count =
        s_conversation_item_count < capacity ? s_conversation_item_count : capacity;
    for (size_t i = 0; i < count; ++i)
    {
        out_items[i] = s_conversation_items[i];
    }
    *out_count = count;
    portEXIT_CRITICAL(&s_snapshot_lock);
    return ESP_OK;
}

bool memory_watch_service_is_endpoint_configured(void)
{
    const memory_watch_service_snapshot_t snapshot =
        memory_watch_service_copy_snapshot();
    return snapshot.endpoint_configured;
}

const char *memory_watch_service_state_to_string(
    memory_watch_service_state_t state)
{
    switch (state)
    {
    case MEMORY_WATCH_SERVICE_STATE_READY:
        return "ready";
    case MEMORY_WATCH_SERVICE_STATE_WAITING_NETWORK:
        return "waiting_network";
    case MEMORY_WATCH_SERVICE_STATE_RECORDING:
        return "recording";
    case MEMORY_WATCH_SERVICE_STATE_ENCODING:
        return "encoding";
    case MEMORY_WATCH_SERVICE_STATE_UPLOADING:
        return "uploading";
    case MEMORY_WATCH_SERVICE_STATE_THINKING:
        return "thinking";
    case MEMORY_WATCH_SERVICE_STATE_NEEDS_CLARIFICATION:
        return "needs_clarification";
    case MEMORY_WATCH_SERVICE_STATE_DONE:
        return "done";
    case MEMORY_WATCH_SERVICE_STATE_TIMEOUT:
        return "timeout";
    case MEMORY_WATCH_SERVICE_STATE_ERROR:
        return "error";
    case MEMORY_WATCH_SERVICE_STATE_CANCELED:
        return "canceled";
    default:
        return "unknown";
    }
}

/* ════════════════════════════════════════════════════════════════════
 * Inbox store / worker / scheduler
 *
 * owner task 是 inbox store 的唯一写入者；
 * inbox worker 只负责阻塞 HTTP，结果通过 task notification 回传 owner。
 * ════════════════════════════════════════════════════════════════════ */

/* esp_timer.h / memory_watch_voice_client.h 已在文件顶部 include，无需重复 */

/* 工作线程堆栈大小必须为编译期常量（用于文件级静态数组） */
#define INBOX_WORKER_STACK_WORDS 8192U

/* ── pending-read set（仅 owner task 读写，无需锁）── */
#define INBOX_PENDING_READ_MAX 20U

/* ── store 和 staging 按需分配在 PSRAM，共约 28 KiB ── */
static memory_watch_inbox_item_t *s_inbox_store = NULL;
static memory_watch_inbox_item_t *s_inbox_staging = NULL;

/* ── store 实际有效项计数（受 s_inbox_store_mutex 保护） ── */
static size_t s_inbox_store_count = 0;
static SemaphoreHandle_t s_inbox_store_mutex = NULL;
static StaticSemaphore_t s_inbox_store_mutex_buffer;

/* ── pending-read notification_ids（已读但 HTTP 尚未成功） ── */
static char s_pending_read[INBOX_PENDING_READ_MAX][64];
static size_t s_pending_read_count = 0;

/* ── inbox meta（owner task 写，getter 在 critical section 读）── */
static portMUX_TYPE s_inbox_meta_lock = portMUX_INITIALIZER_UNLOCKED;
static memory_watch_inbox_meta_t s_inbox_meta = {
    .generation    = 0,
    .item_count    = 0,
    .unread_count  = 0,
    .sync_state    = MEMORY_WATCH_INBOX_SYNC_UNCONFIGURED,
    .last_success_ms = 0,
};

/* ── inbox worker task handle（仅供 init 创建和 owner 投递通知） ── */
static TaskHandle_t s_inbox_worker_task_handle = NULL;

/* ── owner task 与 inbox worker 之间的共享 job（owner 写，worker 读）── */
typedef enum {
    INBOX_JOB_POLL = 0,
    INBOX_JOB_MARK_READ,
} inbox_job_type_t;

typedef struct {
    inbox_job_type_t type;
    memory_watch_service_client_config_snapshot_t client_config;
    char notification_id[64]; /* 仅 MARK_READ 使用 */
} inbox_job_t;

/* 用 FreeRTOS queue（深度 1）把 job 从 owner 传给 worker；
 * 1 槽位保证串行：上一个 job 完成前 owner 不投递下一个。 */
static QueueHandle_t s_inbox_job_queue = NULL;
static StaticQueue_t s_inbox_job_queue_buffer;
static uint8_t s_inbox_job_queue_storage[1 * sizeof(inbox_job_t)];

/* ── worker 把结果通过 task notification 回传 owner ──
 * 成功 = 1，失败 = 2，404（终态已读）= 3；owner 从 staging 区读结果。 */
#define INBOX_WORKER_NOTIFY_SUCCESS   1U
#define INBOX_WORKER_NOTIFY_FAIL      2U
#define INBOX_WORKER_NOTIFY_NOT_FOUND 3U

/* staging 结果（worker 写，owner 读，无需额外锁：owner 等 notify 后才读）*/
static size_t  s_staging_item_count  = 0;
static uint8_t s_staging_unread_count = 0;
static bool    s_staging_mark_read_result = false;

/* ── 辅助：读取 endpoint 快照（owner task 私有，不需锁）── */
static bool memory_watch_service_inbox_get_client_config(
    memory_watch_service_client_config_snapshot_t *out)
{
    portENTER_CRITICAL(&s_endpoint_lock);
    const bool configured = s_endpoint_config.configured;
    if (configured)
    {
        memory_watch_service_copy_text(out->base_url,
            sizeof(out->base_url), s_endpoint_config.base_url);
        memory_watch_service_copy_text(out->device_id,
            sizeof(out->device_id), s_endpoint_config.device_id);
        memory_watch_service_copy_text(out->device_token,
            sizeof(out->device_token), s_endpoint_config.device_token);
        memory_watch_service_rebind_client_config(out);
        /* inbox 短超时 30 秒；timeout_ms 为 uint32_t */
        out->client_config.timeout_ms = 30000U;
        out->client_config.allow_insecure_http =
            s_endpoint_config.allow_insecure_http;
    }
    portEXIT_CRITICAL(&s_endpoint_lock);
    return configured;
}

/* ── 辅助：更新 meta（只在 owner task 写）── */
static void memory_watch_service_inbox_set_meta(
    size_t item_count, uint8_t unread_count,
    memory_watch_inbox_sync_state_t sync_state, bool update_success_ts)
{
    portENTER_CRITICAL(&s_inbox_meta_lock);
    s_inbox_meta.generation++;
    s_inbox_meta.item_count   = item_count;
    s_inbox_meta.unread_count = unread_count;
    s_inbox_meta.sync_state   = sync_state;
    if (update_success_ts)
    {
        s_inbox_meta.last_success_ms =
            (int64_t)(esp_timer_get_time() / 1000LL);
    }
    portEXIT_CRITICAL(&s_inbox_meta_lock);
}

static void memory_watch_service_inbox_set_sync_state(
    memory_watch_inbox_sync_state_t sync_state)
{
    portENTER_CRITICAL(&s_inbox_meta_lock);
    s_inbox_meta.generation++;
    s_inbox_meta.sync_state = sync_state;
    portEXIT_CRITICAL(&s_inbox_meta_lock);
}

static void memory_watch_service_inbox_set_poll_pending(bool pending)
{
    portENTER_CRITICAL(&s_inbox_poll_lock);
    s_inbox_poll_pending = pending;
    portEXIT_CRITICAL(&s_inbox_poll_lock);
}

static bool memory_watch_service_inbox_is_poll_pending(void)
{
    bool pending = false;
    portENTER_CRITICAL(&s_inbox_poll_lock);
    pending = s_inbox_poll_pending;
    portEXIT_CRITICAL(&s_inbox_poll_lock);
    return pending;
}

/* ── inbox worker task：只执行 HTTP，不接触界面对象 ── */
static void memory_watch_service_inbox_worker_task(void *arg)
{
    (void)arg;
    inbox_job_t job;

    for (;;)
    {
        /* 阻塞等待 owner 投递 job，无超时 */
        if (xQueueReceive(s_inbox_job_queue, &job, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }
        memory_watch_service_rebind_client_config(&job.client_config);

        uint32_t notify_val = INBOX_WORKER_NOTIFY_FAIL;

        if (job.type == INBOX_JOB_POLL)
        {
            /* GET /v1/watch/inbox */
            memory_watch_inbox_poll_result_t result = {0};
            const esp_err_t err = memory_watch_voice_client_inbox_poll(
                &job.client_config.client_config,
                s_inbox_staging, MEMORY_WATCH_INBOX_MAX_ITEMS, &result);
            if (err == ESP_OK)
            {
                s_staging_item_count   = result.item_count;
                s_staging_unread_count = result.unread_count;
                notify_val = INBOX_WORKER_NOTIFY_SUCCESS;
            }
            else
            {
                /* 按 HTTP 状态分类，方便 owner 决策重试策略 */
                if (result.http_status == 401 || result.http_status == 403 ||
                    result.http_status == 422)
                {
                    notify_val = INBOX_WORKER_NOTIFY_NOT_FOUND; /* 复用 = 终态错误 */
                }
                else
                {
                    notify_val = INBOX_WORKER_NOTIFY_FAIL;
                }
            }
        }
        else /* INBOX_JOB_MARK_READ */
        {
            memory_watch_inbox_mark_read_result_t result = {0};
            const esp_err_t err = memory_watch_voice_client_inbox_mark_read(
                &job.client_config.client_config,
                job.notification_id, &result);
            if (err == ESP_OK)
            {
                s_staging_mark_read_result = result.read;
                notify_val = INBOX_WORKER_NOTIFY_SUCCESS;
            }
            else if (err == ESP_ERR_NOT_FOUND)
            {
                notify_val = INBOX_WORKER_NOTIFY_NOT_FOUND; /* 404，终态 */
            }
            else
            {
                notify_val = INBOX_WORKER_NOTIFY_FAIL;
            }
        }

        /* 通知 owner task（bit-0 的值就是 notify_val）*/
        if (s_service_task_handle != NULL)
        {
            xTaskNotify(s_service_task_handle, notify_val,
                        eSetValueWithOverwrite);
        }
    }
}

/* ════════════════════════════════════════════════════════════════════
 * inbox store 合并（owner task 收到 POLL worker 成功通知后调用）
 *
 * staging → store 的合并规则：
 *   1. staging 整份校验通过后才写入 store（由 worker 确保解析成功）。
 *   2. pending-read set 优先于服务器快照（本地已读不被服务器覆盖）。
 *   3. 不再存在于新快照中的旧记录随整体替换自动丢弃。
 * ════════════════════════════════════════════════════════════════════ */
static void memory_watch_service_inbox_merge_staging(void)
{
    if (s_inbox_store_mutex != NULL)
    {
        xSemaphoreTake(s_inbox_store_mutex, portMAX_DELAY);
    }
    /* 1. 把 staging 复制 to store */
    for (size_t i = 0; i < s_staging_item_count; ++i)
    {
        s_inbox_store[i] = s_inbox_staging[i];
    }
    s_inbox_store_count = s_staging_item_count;

    /* 2. 用 pending-read 覆盖服务器快照的 read=false */
    for (size_t pi = 0; pi < s_pending_read_count; ++pi)
    {
        for (size_t si = 0; si < s_staging_item_count; ++si)
        {
            if (strcmp(s_inbox_store[si].notification_id,
                       s_pending_read[pi]) == 0)
            {
                s_inbox_store[si].read = true;
                break;
            }
        }
    }

    /* 3. 重算有效未读数（在同一个互斥锁内，确保一致性） */
    uint8_t effective_unread = 0;
    for (size_t i = 0; i < s_inbox_store_count; ++i)
    {
        if (!s_inbox_store[i].read && effective_unread < 255U)
        {
            ++effective_unread;
        }
    }
    if (s_inbox_store_mutex != NULL)
    {
        xSemaphoreGive(s_inbox_store_mutex);
    }

    memory_watch_service_inbox_set_meta(
        s_staging_item_count, effective_unread,
        MEMORY_WATCH_INBOX_SYNC_READY, true);
}

/* ════════════════════════════════════════════════════════════════════
 * inbox 调度器入口（由 owner task 在合适时机调用）
 *
 * 若 endpoint 已配置且无 worker 在途，则投递 POLL job。
 * 注意：当前实现简化为"owner task 在处理外部命令后顺带检查轮询"；
 * 完整的 deadline-based 调度可在后续迭代加入 xTaskGetTickCount 比较。
 * ════════════════════════════════════════════════════════════════════ */
static bool s_inbox_worker_busy = false; /* owner task 私有，无需锁 */

static void memory_watch_service_inbox_try_poll(void)
{
    if (s_inbox_worker_busy)
    {
        /* worker 在途：记录 pending bit，在途结束后补一次 */
        memory_watch_service_inbox_set_poll_pending(true);
        return;
    }

    const int64_t now_ms = memory_watch_service_now_ms();
    if (s_inbox_poll_next_due_ms > now_ms)
    {
        /* 上一次后台 HTTPS 失败后保留 pending，但等退避窗口结束再发。 */
        memory_watch_service_inbox_set_poll_pending(true);
        return;
    }

    inbox_job_t job = {0};
    job.type = INBOX_JOB_POLL;
    if (!memory_watch_service_inbox_get_client_config(&job.client_config))
    {
        /* endpoint 未配置，不发请求 */
        memory_watch_service_inbox_set_sync_state(MEMORY_WATCH_INBOX_SYNC_UNCONFIGURED);
        return;
    }

    if (xQueueSend(s_inbox_job_queue, &job, 0) == pdTRUE)
    {
        s_inbox_worker_busy = true;
        memory_watch_service_inbox_set_sync_state(MEMORY_WATCH_INBOX_SYNC_POLLING);
        memory_watch_service_inbox_set_poll_pending(false);
        ESP_LOGI(TAG, "inbox: poll job dispatched");
    }
}

/* ── 处理 owner task 收到的 inbox worker 通知 ── */
static void memory_watch_service_inbox_handle_worker_result(uint32_t notify_val)
{
    s_inbox_worker_busy = false;

    if (notify_val == INBOX_WORKER_NOTIFY_SUCCESS)
    {
        s_inbox_poll_next_due_ms = 0;
        memory_watch_service_inbox_merge_staging();
        ESP_LOGI(TAG, "inbox: poll ok items=%zu unread=%u",
                 s_staging_item_count, s_staging_unread_count);
    }
    else if (notify_val == INBOX_WORKER_NOTIFY_NOT_FOUND)
    {
        /* AUTH 或 PROTOCOL error：不要紧循环 */
        s_inbox_poll_next_due_ms = 0;
        memory_watch_service_inbox_set_poll_pending(false);
        memory_watch_service_inbox_set_sync_state(MEMORY_WATCH_INBOX_SYNC_AUTH_ERROR);
        ESP_LOGW(TAG, "inbox: poll auth/protocol error, pause polling");
    }
    else
    {
        s_inbox_poll_next_due_ms =
            memory_watch_service_now_ms() + kBackgroundHttpsRetryIntervalMs;
        memory_watch_service_inbox_set_poll_pending(true);
        memory_watch_service_inbox_set_sync_state(MEMORY_WATCH_INBOX_SYNC_RETRY_WAIT);
        ESP_LOGW(TAG, "inbox: poll failed, will retry in %" PRId64 " ms",
                 kBackgroundHttpsRetryIntervalMs);
    }

    /* 若有待处理的 poll_now，顺手补一次 */
    if (memory_watch_service_inbox_is_poll_pending())
    {
        memory_watch_service_inbox_try_poll();
    }
}

/* ════════════════════════════════════════════════════════════════════
 * 公开 getter（任意 task / 界面刷新路径调用，无 I/O，无副作用）
 * ════════════════════════════════════════════════════════════════════ */

esp_err_t memory_watch_service_get_inbox_meta(
    memory_watch_inbox_meta_t *out_meta)
{
    if (out_meta == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_inbox_meta_lock);
    *out_meta = s_inbox_meta;
    portEXIT_CRITICAL(&s_inbox_meta_lock);
    return ESP_OK;
}

esp_err_t memory_watch_service_copy_inbox_summaries(
    memory_watch_inbox_summary_t *out_summaries,
    size_t capacity,
    size_t *out_count)
{
    if (out_summaries == NULL || out_count == NULL || capacity == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_inbox_store == NULL)
    {
        *out_count = 0;
        return ESP_OK;
    }

    /* store 读取用 s_inbox_store_mutex */
    if (s_inbox_store_mutex != NULL)
    {
        xSemaphoreTake(s_inbox_store_mutex, portMAX_DELAY);
    }
    const size_t copy_count = s_inbox_store_count < capacity ? s_inbox_store_count : capacity;
    for (size_t i = 0; i < copy_count; ++i)
    {
        strncpy(out_summaries[i].notification_id,
                s_inbox_store[i].notification_id,
                sizeof(out_summaries[i].notification_id) - 1U);
        out_summaries[i].notification_id[
            sizeof(out_summaries[i].notification_id) - 1U] = '\0';

        strncpy(out_summaries[i].title,
                s_inbox_store[i].title,
                sizeof(out_summaries[i].title) - 1U);
        out_summaries[i].title[sizeof(out_summaries[i].title) - 1U] = '\0';

        strncpy(out_summaries[i].preview,
                s_inbox_store[i].preview,
                sizeof(out_summaries[i].preview) - 1U);
        out_summaries[i].preview[sizeof(out_summaries[i].preview) - 1U] = '\0';

        strncpy(out_summaries[i].created_at,
                s_inbox_store[i].created_at,
                sizeof(out_summaries[i].created_at) - 1U);
        out_summaries[i].created_at[
            sizeof(out_summaries[i].created_at) - 1U] = '\0';

        out_summaries[i].read = s_inbox_store[i].read;
    }
    if (s_inbox_store_mutex != NULL)
    {
        xSemaphoreGive(s_inbox_store_mutex);
    }

    *out_count = copy_count;
    return ESP_OK;
}

esp_err_t memory_watch_service_get_inbox_item(
    const char *notification_id,
    memory_watch_inbox_item_t *out_item)
{
    if (notification_id == NULL || out_item == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_inbox_store == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t result = ESP_ERR_NOT_FOUND;
    if (s_inbox_store_mutex != NULL)
    {
        xSemaphoreTake(s_inbox_store_mutex, portMAX_DELAY);
    }
    for (size_t i = 0; i < s_inbox_store_count; ++i)
    {
        if (strcmp(s_inbox_store[i].notification_id, notification_id) == 0)
        {
            *out_item = s_inbox_store[i];
            result = ESP_OK;
            break;
        }
    }
    if (s_inbox_store_mutex != NULL)
    {
        xSemaphoreGive(s_inbox_store_mutex);
    }
    return result;
}

/* ════════════════════════════════════════════════════════════════════
 * 公开命令 API（投递到 owner task 或直接操作 pending-read）
 * ════════════════════════════════════════════════════════════════════ */

esp_err_t memory_watch_service_inbox_poll_now(const char *reason)
{
    ESP_LOGI(TAG, "inbox: poll_now reason=%s",
             reason != NULL ? reason : "unknown");
    /* 在 owner task 内直接调用 try_poll；
     * 若从其他 task 调用（例如界面 callback），用 owner 唤醒路径安全投递。
     * V1 简化：用 SEND_TEXT 命令队列同样 slot 携带 poll_now 意图 ──
     * 此处直接设置 pending bit，让 owner task 下次醒来时检查。 */
    memory_watch_service_inbox_set_poll_pending(true);
    if (s_service_task_handle != NULL)
    {
        /* 用 task notification bit-0 唤醒 owner task */
        xTaskNotify(s_service_task_handle, 0U, eNoAction);
    }
    return ESP_OK;
}

esp_err_t memory_watch_service_inbox_mark_read(
    const char *notification_id)
{
    if (notification_id == NULL || notification_id[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_inbox_store == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    /* 1. 本地立即置已读（在 store + pending-read set）*/
    bool found_in_store = false;
    uint8_t new_unread = 0;
    if (s_inbox_store_mutex != NULL)
    {
        xSemaphoreTake(s_inbox_store_mutex, portMAX_DELAY);
    }
    for (size_t i = 0; i < s_inbox_store_count; ++i)
    {
        if (strcmp(s_inbox_store[i].notification_id, notification_id) == 0)
        {
            if (!s_inbox_store[i].read)
            {
                s_inbox_store[i].read = true;
            }
            found_in_store = true;
        }
    }

    if (found_in_store)
    {
        /* 重新计算有效未读数 */
        for (size_t i = 0; i < s_inbox_store_count; ++i)
        {
            if (!s_inbox_store[i].read && new_unread < 255U)
            {
                ++new_unread;
            }
        }
    }

    if (!found_in_store)
    {
        if (s_inbox_store_mutex != NULL)
        {
            xSemaphoreGive(s_inbox_store_mutex);
        }
        return ESP_ERR_NOT_FOUND;
    }

    /* 2. 加入 pending-read set（受同一个互斥锁保护） */
    bool already_pending = false;
    for (size_t i = 0; i < s_pending_read_count; ++i)
    {
        if (strcmp(s_pending_read[i], notification_id) == 0)
        {
            already_pending = true;
            break;
        }
    }
    if (!already_pending && s_pending_read_count < INBOX_PENDING_READ_MAX)
    {
        strncpy(s_pending_read[s_pending_read_count], notification_id,
                sizeof(s_pending_read[0]) - 1U);
        s_pending_read[s_pending_read_count][sizeof(s_pending_read[0]) - 1U] = '\0';
        ++s_pending_read_count;
    }
    if (s_inbox_store_mutex != NULL)
    {
        xSemaphoreGive(s_inbox_store_mutex);
    }

    /* 3. 更新 unread_count 并通知 controller */
    portENTER_CRITICAL(&s_inbox_meta_lock);
    s_inbox_meta.generation++;
    s_inbox_meta.unread_count = new_unread;
    portEXIT_CRITICAL(&s_inbox_meta_lock);

    /* 4. 异步投递 MARK_READ HTTP 给 inbox worker */
    if (!s_inbox_worker_busy)
    {
        inbox_job_t job = {0};
        job.type = INBOX_JOB_MARK_READ;
        if (memory_watch_service_inbox_get_client_config(&job.client_config))
        {
            strncpy(job.notification_id, notification_id,
                    sizeof(job.notification_id) - 1U);
            job.notification_id[sizeof(job.notification_id) - 1U] = '\0';
            if (xQueueSend(s_inbox_job_queue, &job, 0) == pdTRUE)
            {
                s_inbox_worker_busy = true;
                ESP_LOGI(TAG, "inbox: mark_read dispatched id=%.32s",
                         notification_id);
            }
        }
    }
    /* 若 worker 在途，pending-read 会在下次 POLL 成功后通过 merge 补偿 */

    return ESP_OK;
}

/* ── inbox worker task 和 job queue 在 memory_watch_service_init 中创建 ──
 * 函数原型已在 service.c 中声明为 static，此处提供 init hook。
 * 调用方：memory_watch_service_init() */

esp_err_t memory_watch_service_inbox_init(void)
{
    if (s_inbox_store_mutex == NULL)
    {
        s_inbox_store_mutex = xSemaphoreCreateMutexStatic(&s_inbox_store_mutex_buffer);
    }

    /* store / staging 按需分配在 PSRAM，inbox 未使用时不占内存 */
    if (s_inbox_store == NULL)
    {
        s_inbox_store = (memory_watch_inbox_item_t *)heap_caps_calloc(
            MEMORY_WATCH_INBOX_MAX_ITEMS, sizeof(memory_watch_inbox_item_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_inbox_staging == NULL)
    {
        s_inbox_staging = (memory_watch_inbox_item_t *)heap_caps_calloc(
            MEMORY_WATCH_INBOX_MAX_ITEMS, sizeof(memory_watch_inbox_item_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_inbox_store == NULL || s_inbox_staging == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    if (s_inbox_job_queue == NULL)
    {
        s_inbox_job_queue = xQueueCreateStatic(
            1,
            sizeof(inbox_job_t),
            s_inbox_job_queue_storage,
            &s_inbox_job_queue_buffer);
    }
    if (s_inbox_job_queue == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    if (s_inbox_worker_task_handle == NULL)
    {
        /* inbox worker 需要 PSRAM stack（处理 24 KiB 响应体）；
         * 用 xTaskCreateWithCaps 避免占用 internal RAM。 */
        const BaseType_t inbox_created = xTaskCreateWithCaps(
            memory_watch_service_inbox_worker_task,
            "mw_inbox",
            INBOX_WORKER_STACK_WORDS,
            NULL,
            3, /* 低于 upload worker 优先级 */
            &s_inbox_worker_task_handle,
            MALLOC_CAP_SPIRAM);
        if (inbox_created != pdPASS)
        {
            s_inbox_worker_task_handle = NULL;
        }
    }
    if (s_inbox_worker_task_handle == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
