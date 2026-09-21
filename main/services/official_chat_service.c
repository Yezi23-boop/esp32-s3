#include "official_chat_service.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "audio_codec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "services/runtime/runtime_coordinator.h"
#include "services/runtime/safety_monitor_policy.h"
#include "services/network/network_service.h"
#include "official_chat.h"
#include "sdkconfig.h"
#include "services/time/system_time_service.h"

/*
 * 官方聊天服务实现说明：
 * - 底层 `official_chat` 更像一次会话实例，本模块把它包装成“可长期驻留的服务”；
 * - UI 只需要声明前台/后台意图，本模块负责等网络、拉起实例、接收事件、缓存文本和安全销毁；
 * - 关闭流程额外引入 quiet period，是为了给传输层和音频链路一个收敛窗口，降低异常销毁概率。
 */

static const char *TAG = "official_chat_srv";
static const size_t kLastUserTextMaxBytes = 192;              /* 最近一条用户文本缓存上限，单位为字节。 */
static const size_t kLastAssistantTextMaxBytes = 256;         /* 最近一条助手文本缓存上限，单位为字节。 */
static const size_t kMessageHistoryCapacity = 8;              /* 固定消息历史容量；满后丢弃最旧条目。 */
static const uint32_t kShutdownTransportQuietPeriodMs = 1500; /* 关闭前等待传输层静默的窗口，单位为毫秒。 */
static const uint32_t kShutdownWaitTimeoutMs = 4000;          /* 同步关闭接口的最大等待时间，单位为毫秒。 */
static const UBaseType_t kCommandQueueLength = 8;             /* 外部意图命令队列深度，避免 UI 直接写服务内部状态。 */

typedef struct
{
    official_chat_service_cmd_type_t type;
    uint32_t generation;
    esp_err_t result;
} official_chat_service_cmd_t;

static TaskHandle_t s_service_task_handle = NULL;                 /* 服务任务句柄，只在初始化阶段写入，其他路径只读。 */
static QueueHandle_t s_command_queue = NULL;                      /* UI/API 向 owner task 投递生命周期命令。 */
static StaticQueue_t s_command_queue_buffer;                      /* 静态队列控制块，避免服务初始化时堆分配。 */
static uint8_t s_command_queue_storage[8 * sizeof(official_chat_service_cmd_t)];
static official_chat_handle_t s_chat_handle = NULL;               /* 底层会话句柄，仅服务任务负责创建和销毁。 */
static bool s_foreground_intent = false;                          /* 页面请求仍有效；grant 前不启动真实会话。 */
static bool s_foreground_requested = false;                       /* coordinator 已 grant，允许 owner 启动会话。 */
static bool s_shutdown_requested = false;                         /* 服务任务私有的关闭流程状态。 */
static bool s_shutdown_stop_requested = false;                    /* 标记关闭流程中是否已发送 stop_listening。 */
static uint32_t s_coordinator_request_generation = 0;             /* 当前 official chat 前台请求代次。 */
static uint32_t s_coordinator_quiesce_generation = 0;             /* 停机完成后必须回报的排空代次。 */
static bool s_coordinator_start_reported = false;                 /* 防止同一 grant 重复报告启动结果。 */
static TickType_t s_shutdown_destroy_deadline_ticks = 0;          /* quiet period 截止 tick，仅服务任务推进。 */
static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static official_chat_service_snapshot_t s_snapshot = {
    .state = OFFICIAL_CHAT_SERVICE_STATE_STOPPED,
    .foreground_active = false,
    .stop_pending = false,
    .audio_channel_ready = false,
    .last_error = ESP_OK,
};
static StaticSemaphore_t s_text_mutex_buffer;                      /* 静态互斥锁存储，避免初始化时额外堆分配。 */
static SemaphoreHandle_t s_text_mutex = NULL;                      /* 文本缓存保护锁，事件回调与 UI 查询共用。 */
/* 文本只在普通任务/事件回调中访问，不参与 DMA、ISR 或 flash cache 冻结，可常驻 PSRAM。 */
static char *s_last_user_text = NULL;                              /* 最近用户文本快照，由事件回调更新。 */
static char *s_last_assistant_text = NULL;                         /* 最近助手文本快照，由事件回调更新。 */
static official_chat_service_message_t *s_message_history = NULL; /* 固定容量历史消息快照，仅在持锁下访问。 */
static size_t s_message_count = 0;                                 /* 当前有效历史条目数，仅在持锁下读写。 */

/**
 * @brief 获取文本缓存互斥锁。
 *
 * 事件回调和 UI 查询接口会并发访问最近文本与消息历史，因此所有相关读写都必须先持有该锁。
 *
 * @return 无返回值。
 *
 * @note 仅允许在任务上下文调用；该函数会阻塞等待锁，不可在 ISR 中使用。
 */
static void official_chat_service_lock(void)
{
    /* 文本缓存会被事件回调与 UI 查询同时访问，因此统一用互斥锁保护。 */
    if (s_text_mutex != NULL)
    {
        xSemaphoreTake(s_text_mutex, portMAX_DELAY);
    }
}

/**
 * @brief 释放文本缓存互斥锁。
 *
 * @return 无返回值。
 *
 * @note 调用方必须已经成功持有 `s_text_mutex`。
 */
static void official_chat_service_unlock(void)
{
    if (s_text_mutex != NULL)
    {
        xSemaphoreGive(s_text_mutex);
    }
}

/**
 * @brief 为仅供 UI 快照读取的长期文本缓存分配 PSRAM。
 *
 * 这些对象不在 `esp_partition_mmap()` 的 cache-freeze 临界路径中使用；保留
 * service task 与 FreeRTOS 控制块在 internal RAM，给 TLS AES DMA 临时块留连续空间。
 */
static esp_err_t official_chat_service_alloc_text_caches(void)
{
    if (s_last_user_text == NULL)
    {
        s_last_user_text = heap_caps_calloc(
            kLastUserTextMaxBytes, sizeof(*s_last_user_text),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_last_assistant_text == NULL)
    {
        s_last_assistant_text = heap_caps_calloc(
            kLastAssistantTextMaxBytes, sizeof(*s_last_assistant_text),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_message_history == NULL)
    {
        s_message_history = heap_caps_calloc(
            kMessageHistoryCapacity, sizeof(*s_message_history),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    if (s_last_user_text != NULL && s_last_assistant_text != NULL &&
        s_message_history != NULL)
    {
        return ESP_OK;
    }

    heap_caps_free(s_last_user_text);
    heap_caps_free(s_last_assistant_text);
    heap_caps_free(s_message_history);
    s_last_user_text = NULL;
    s_last_assistant_text = NULL;
    s_message_history = NULL;
    return ESP_ERR_NO_MEM;
}

/**
 * @brief 复制服务生命周期快照。
 *
 * snapshot 是跨任务共享状态，因此用极短 critical section 保护复制过程；
 * getter 不访问 I/O、不等待队列、不推进状态机。
 */
static official_chat_service_snapshot_t official_chat_service_copy_snapshot(void)
{
    official_chat_service_snapshot_t snapshot;

    taskENTER_CRITICAL(&s_snapshot_lock);
    snapshot = s_snapshot;
    taskEXIT_CRITICAL(&s_snapshot_lock);

    return snapshot;
}

/**
 * @brief 原子更新服务快照的状态字段。
 * @param state 新服务状态。
 */
static void official_chat_service_set_state(
    official_chat_service_state_t state)
{
    taskENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot.state = state;
    taskEXIT_CRITICAL(&s_snapshot_lock);
}

/**
 * @brief 原子更新最近错误字段。
 * @param error 最近一次错误码。
 */
static void official_chat_service_set_last_error(esp_err_t error)
{
    taskENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot.last_error = error;
    taskEXIT_CRITICAL(&s_snapshot_lock);
}

/**
 * @brief 原子更新底层语音通道预连接状态。
 * @param ready true 表示 WebSocket/音频通道已经打开。
 */
static void official_chat_service_set_audio_channel_ready(bool ready)
{
    taskENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot.audio_channel_ready = ready;
    taskEXIT_CRITICAL(&s_snapshot_lock);
}

/**
 * @brief 原子更新前台与停机意图字段。
 * @param foreground_active true 表示聊天处于前台意图。
 * @param stop_pending true 表示已有停机请求待服务任务收敛。
 */
static void official_chat_service_set_lifecycle_intent(
    bool foreground_active, bool stop_pending)
{
    taskENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot.foreground_active = foreground_active;
    s_snapshot.stop_pending = stop_pending;
    taskEXIT_CRITICAL(&s_snapshot_lock);
}

/**
 * @brief 向后台资源管理器声明官方聊天的前台麦克风意图。
 *
 * official_chat 的真实麦克风 owner 仍由 `LocalAudioCodecAdapter` 申请；
 * 这里提前通知后台 Safety Monitor 让出麦克风，避免双方在 codec session
 * acquire 阶段互相等待。
 *
 * @param[in] active true 表示官方聊天前台链路即将使用麦克风。
 * @param[in] reason 日志原因。
 */
static void official_chat_service_set_foreground_audio_active(bool active,
                                                              const char *reason)
{
    esp_err_t ret = safety_monitor_policy_set_foreground_audio_active(
        active, reason);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "foreground audio signal failed: active=%d reason=%s err=%s",
                 active, reason != NULL ? reason : "unknown",
                 esp_err_to_name(ret));
    }
}

/**
 * @brief 在持锁前提下清空所有文本缓存和消息历史。
 *
 * @return 无返回值。
 *
 * @note 仅允许在已经持有 `s_text_mutex` 的前提下调用。
 */
static void official_chat_service_clear_cached_text_locked(void)
{
    memset(s_last_user_text, 0, kLastUserTextMaxBytes);
    memset(s_last_assistant_text, 0, kLastAssistantTextMaxBytes);
    memset(s_message_history, 0,
           kMessageHistoryCapacity * sizeof(*s_message_history));
    s_message_count = 0;
}

/**
 * @brief 在持锁前提下更新最近文本缓存。
 *
 * 该辅助函数统一处理空指针保护和安全截断，避免事件回调在多个分支里重复维护字符串拷贝逻辑。
 *
 * @param[out] target 目标缓存。
 * @param[in] target_size 目标缓存长度，单位为字节。
 * @param[in] text 要缓存的文本；为 NULL 时直接返回。
 * @return 无返回值。
 *
 * @note 仅允许在已经持有 `s_text_mutex` 的前提下调用。
 */
static void official_chat_service_store_text_locked(char *target,
                                                    size_t target_size,
                                                    const char *text)
{
    if (target == NULL || target_size == 0 || text == NULL)
    {
        return;
    }

    snprintf(target, target_size, "%s", text);
}

/**
 * @brief 在持锁前提下压入一条历史消息。
 *
 * 固定容量历史的目标是给 UI 提供最近若干条对话快照，而不是完整持久化日志，因此容量打满时会丢弃最旧条目。
 *
 * @param[in] role 消息角色。
 * @param[in] text 消息文本；为 NULL 时会退化为空字符串。
 * @return 无返回值。
 *
 * @note 仅允许在已经持有 `s_text_mutex` 的前提下调用。
 */
static void official_chat_service_enqueue_message_locked(
    official_chat_service_message_role_t role, const char *text)
{
    official_chat_service_message_t message = {
        .role = role,
    };

    snprintf(message.text, sizeof(message.text), "%s",
             text != NULL ? text : "");

    if (s_message_count == kMessageHistoryCapacity)
    {
        /* 固定容量环形历史的简化实现：满了就丢最旧的一条。 */
        memmove(&s_message_history[0], &s_message_history[1],
                (kMessageHistoryCapacity - 1) *
                    sizeof(s_message_history[0]));
        s_message_history[kMessageHistoryCapacity - 1] = message;
        return;
    }

    s_message_history[s_message_count] = message;
    s_message_count++;
}

/**
 * @brief 在持锁前提下把缓存文本复制给调用方。
 *
 * 返回值显式区分“当前没有文本”与“参数非法”，避免 UI 误把空字符串当成一次有效回复。
 *
 * @param[in] source 内部缓存源字符串。
 * @param[out] buffer 输出缓冲区。
 * @param[in] size 输出缓冲区长度，单位为字节。
 * @return `ESP_OK` 表示成功复制；
 *         `ESP_ERR_NOT_FOUND` 表示当前没有可用文本；
 *         `ESP_ERR_INVALID_ARG` 表示输出参数非法。
 *
 * @note 仅允许在已经持有 `s_text_mutex` 的前提下调用。
 */
static esp_err_t official_chat_service_copy_text_locked(const char *source,
                                                        char *buffer,
                                                        size_t size)
{
    if (buffer == NULL || size == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const bool has_text = source != NULL && source[0] != '\0';
    if (has_text)
    {
        snprintf(buffer, size, "%s", source);
    }
    else
    {
        buffer[0] = '\0';
    }

    return has_text ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t official_chat_service_ensure_time_valid(uint32_t timeout_ms,
                                                         void *user_ctx)
{
    (void)user_ctx;
    return system_time_service_ensure_valid_for_tls(timeout_ms);
}

static esp_err_t official_chat_service_apply_server_time(int64_t unix_seconds,
                                                         void *user_ctx)
{
    (void)user_ctx;
    return system_time_service_apply_server_time(unix_seconds);
}

/**
 * @brief 把底层 `official_chat` 状态映射成服务层状态。
 *
 * 服务层状态比底层更稳定，目的是给 UI 提供有限且可预测的状态集合，避免页面直接耦合底层细节。
 *
 * @param[in] state 底层会话状态。
 * @return 对应的服务层状态枚举。
 */
static official_chat_service_state_t map_official_chat_state(
    official_chat_state_t state)
{
    switch (state)
    {
    case OFFICIAL_CHAT_STATE_ACTIVATING:
        return OFFICIAL_CHAT_SERVICE_STATE_ACTIVATING;
    case OFFICIAL_CHAT_STATE_CONNECTING:
        return OFFICIAL_CHAT_SERVICE_STATE_CONNECTING;
    case OFFICIAL_CHAT_STATE_IDLE:
        return OFFICIAL_CHAT_SERVICE_STATE_IDLE;
    case OFFICIAL_CHAT_STATE_LISTENING:
        return OFFICIAL_CHAT_SERVICE_STATE_LISTENING;
    case OFFICIAL_CHAT_STATE_SPEAKING:
        return OFFICIAL_CHAT_SERVICE_STATE_SPEAKING;
    case OFFICIAL_CHAT_STATE_UPGRADING:
        return OFFICIAL_CHAT_SERVICE_STATE_STARTING;
    case OFFICIAL_CHAT_STATE_UNKNOWN:
    default:
        return OFFICIAL_CHAT_SERVICE_STATE_STARTING;
    }
}

/**
 * @brief 判断当前底层状态在销毁前是否需要 quiet period。
 *
 * 当传输层或音频链路仍可能活跃时，立即销毁句柄会放大竞争和尾包丢失风险，因此需要先等待收敛窗口。
 *
 * @param[in] state 当前底层会话状态。
 * @return true 表示销毁前需要额外等待。
 */
static bool official_chat_service_requires_shutdown_quiet_period(
    official_chat_state_t state)
{
    /* 这些状态下传输或音频链路仍可能活跃，销毁前需要额外等待。 */
    return state == OFFICIAL_CHAT_STATE_CONNECTING ||
           state == OFFICIAL_CHAT_STATE_IDLE ||
           state == OFFICIAL_CHAT_STATE_LISTENING ||
           state == OFFICIAL_CHAT_STATE_SPEAKING;
}

/**
 * @brief 在 owner task 上下文开始停机收敛。
 *
 * 真正的 prepare、stop、quiet period 和 destroy 都由服务任务推进；
 * UI/API 只能投递命令，不能直接改这些生命周期字段。
 */
static void official_chat_service_begin_shutdown_from_task(void)
{
    s_foreground_intent = false;
    s_foreground_requested = false;
    s_shutdown_requested = true;
    s_shutdown_stop_requested = false;
    s_shutdown_destroy_deadline_ticks = 0;
    official_chat_service_set_lifecycle_intent(false, true);
}

/**
 * @brief 在 owner task 上下文处理外部命令。
 * @param command 命令内容。
 */
static void official_chat_service_handle_command(
    const official_chat_service_cmd_t *command)
{
    if (command == NULL)
    {
        return;
    }

    switch (command->type)
    {
    case OFFICIAL_CHAT_SERVICE_CMD_ENTER_FOREGROUND:
    {
        if (s_foreground_intent || s_foreground_requested ||
            s_coordinator_request_generation != 0U)
        {
            ESP_LOGI(TAG, "command: foreground request already active");
            break;
        }
        s_shutdown_requested = false;
        s_shutdown_stop_requested = false;
        s_shutdown_destroy_deadline_ticks = 0;
        s_foreground_intent = true;
        s_foreground_requested = false;
        s_coordinator_start_reported = false;
        uint32_t request_generation = 0U;
        const esp_err_t request_ret = runtime_coordinator_request_foreground(
            RUNTIME_COORDINATOR_PARTICIPANT_OFFICIAL_CHAT,
            &request_generation);
        if (request_ret != ESP_OK)
        {
            s_foreground_intent = false;
            s_foreground_requested = false;
            official_chat_service_set_lifecycle_intent(false, false);
            official_chat_service_set_last_error(request_ret);
            official_chat_service_set_state(OFFICIAL_CHAT_SERVICE_STATE_ERROR);
            ESP_LOGW(TAG, "command: foreground request failed: %s",
                     esp_err_to_name(request_ret));
            break;
        }
        s_coordinator_request_generation = request_generation;
        official_chat_service_set_lifecycle_intent(true, false);
        official_chat_service_set_state(OFFICIAL_CHAT_SERVICE_STATE_STARTING);
        ESP_LOGI(TAG, "command: enter_foreground request=%u",
                 (unsigned)request_generation);
        break;
    }
    case OFFICIAL_CHAT_SERVICE_CMD_LEAVE_FOREGROUND_AND_STOP:
        s_foreground_intent = false;
        if (s_coordinator_request_generation != 0U)
        {
            (void)runtime_coordinator_cancel_request(
                RUNTIME_COORDINATOR_PARTICIPANT_OFFICIAL_CHAT,
                s_coordinator_request_generation);
        }
        else
        {
            official_chat_service_begin_shutdown_from_task();
        }
        ESP_LOGI(TAG, "command: leave_foreground_and_stop");
        break;
    case OFFICIAL_CHAT_SERVICE_CMD_COORDINATOR_GRANTED:
        if (s_foreground_intent &&
            command->generation == s_coordinator_request_generation)
        {
            s_foreground_requested = true;
            official_chat_service_set_foreground_audio_active(
                true, "official_chat");
            official_chat_service_set_lifecycle_intent(true, false);
        }
        else
        {
            (void)runtime_coordinator_report_start_result(
                RUNTIME_COORDINATOR_PARTICIPANT_OFFICIAL_CHAT,
                command->generation, ESP_ERR_INVALID_STATE);
        }
        break;
    case OFFICIAL_CHAT_SERVICE_CMD_COORDINATOR_QUIESCE:
        s_coordinator_quiesce_generation = command->generation;
        official_chat_service_begin_shutdown_from_task();
        break;
    case OFFICIAL_CHAT_SERVICE_CMD_COORDINATOR_CANCELLED:
        if (command->generation == s_coordinator_request_generation)
        {
            s_coordinator_request_generation = 0U;
            s_foreground_intent = false;
            if (s_chat_handle != NULL || s_foreground_requested)
            {
                official_chat_service_begin_shutdown_from_task();
            }
            else
            {
                official_chat_service_set_lifecycle_intent(false, false);
                official_chat_service_set_state(OFFICIAL_CHAT_SERVICE_STATE_STOPPED);
            }
            official_chat_service_set_last_error(command->result);
        }
        break;
    case OFFICIAL_CHAT_SERVICE_CMD_PREPARE_AUDIO_CHANNEL:
        if (s_chat_handle != NULL && !s_shutdown_requested)
        {
            const esp_err_t ret =
                official_chat_prepare_audio_channel(s_chat_handle);
            if (ret != ESP_OK)
            {
                official_chat_service_set_last_error(ret);
                ESP_LOGW(TAG, "prepare_audio_channel failed: %s",
                         esp_err_to_name(ret));
            }
        }
        ESP_LOGI(TAG, "command: prepare_audio_channel");
        break;
    case OFFICIAL_CHAT_SERVICE_CMD_START_LISTENING:
        if (s_chat_handle != NULL && !s_shutdown_requested)
        {
            const esp_err_t ret = official_chat_start_listening(s_chat_handle);
            if (ret != ESP_OK)
            {
                ESP_LOGW(TAG, "start_listening failed: %s",
                         esp_err_to_name(ret));
            }
        }
        ESP_LOGI(TAG, "command: start_listening");
        break;
    case OFFICIAL_CHAT_SERVICE_CMD_STOP_LISTENING:
        if (s_chat_handle != NULL)
        {
            const esp_err_t ret = official_chat_stop_listening(s_chat_handle);
            if (ret != ESP_OK)
            {
                ESP_LOGW(TAG, "stop_listening failed: %s",
                         esp_err_to_name(ret));
            }
        }
        ESP_LOGI(TAG, "command: stop_listening");
        break;
    case OFFICIAL_CHAT_SERVICE_CMD_NETWORK_READY:
    case OFFICIAL_CHAT_SERVICE_CMD_BUDGET_CHANGED:
    default:
        ESP_LOGD(TAG, "command ignored: type=%d", (int)command->type);
        break;
    }
}

/**
 * @brief 投递服务命令。
 * @param type 命令类型。
 * @param timeout_ticks 入队等待时间。
 * @return `ESP_OK` 表示命令已入队。
 */
static esp_err_t official_chat_service_post_command(
    official_chat_service_cmd_type_t type, TickType_t timeout_ticks)
{
    if (s_command_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    official_chat_service_cmd_t command = {
        .type = type,
    };

    if (xQueueSend(s_command_queue, &command, timeout_ticks) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    if (type == OFFICIAL_CHAT_SERVICE_CMD_ENTER_FOREGROUND)
    {
        official_chat_service_set_lifecycle_intent(true, false);
    }
    else if (type == OFFICIAL_CHAT_SERVICE_CMD_LEAVE_FOREGROUND_AND_STOP)
    {
        official_chat_service_set_lifecycle_intent(false, true);
    }

    if (s_service_task_handle != NULL)
    {
        xTaskAbortDelay(s_service_task_handle);
    }

    return ESP_OK;
}

static esp_err_t official_chat_service_post_coordinator_command(
    official_chat_service_cmd_type_t type, uint32_t generation,
    esp_err_t result)
{
    if (s_command_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const official_chat_service_cmd_t command = {
        .type = type,
        .generation = generation,
        .result = result,
    };
    return xQueueSend(s_command_queue, &command, 0) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

static esp_err_t official_chat_service_coordinator_quiesce(
    uint32_t generation, void *user_ctx)
{
    (void)user_ctx;
    return official_chat_service_post_coordinator_command(
        OFFICIAL_CHAT_SERVICE_CMD_COORDINATOR_QUIESCE,
        generation, ESP_OK);
}

static esp_err_t official_chat_service_coordinator_grant(
    uint32_t generation, void *user_ctx)
{
    (void)user_ctx;
    return official_chat_service_post_coordinator_command(
        OFFICIAL_CHAT_SERVICE_CMD_COORDINATOR_GRANTED,
        generation, ESP_OK);
}

static esp_err_t official_chat_service_coordinator_cancel(
    uint32_t generation, esp_err_t reason, void *user_ctx)
{
    (void)user_ctx;
    return official_chat_service_post_coordinator_command(
        OFFICIAL_CHAT_SERVICE_CMD_COORDINATOR_CANCELLED,
        generation, reason);
}

/**
 * @brief 把服务状态转换成日志和 UI 可直接展示的字符串。
 * @param state 服务层状态枚举。
 * @return 状态字符串。
 */
const char *official_chat_service_state_to_string(
    official_chat_service_state_t state)
{
    switch (state)
    {
    case OFFICIAL_CHAT_SERVICE_STATE_STOPPED:
        return "stopped";
    case OFFICIAL_CHAT_SERVICE_STATE_WAITING_NETWORK:
        return "waiting_network";
    case OFFICIAL_CHAT_SERVICE_STATE_STARTING:
        return "starting";
    case OFFICIAL_CHAT_SERVICE_STATE_ACTIVATING:
        return "activating";
    case OFFICIAL_CHAT_SERVICE_STATE_CONNECTING:
        return "connecting";
    case OFFICIAL_CHAT_SERVICE_STATE_IDLE:
        return "idle";
    case OFFICIAL_CHAT_SERVICE_STATE_LISTENING:
        return "listening";
    case OFFICIAL_CHAT_SERVICE_STATE_SPEAKING:
        return "speaking";
    case OFFICIAL_CHAT_SERVICE_STATE_ERROR:
    default:
        return "error";
    }
}

/**
 * @brief 底层官方聊天事件回调。
 * @param event 底层上报的事件。
 * @param user_data 未使用。
 *
 * 事件回调只负责两件事：
 * 1. 同步服务状态；
 * 2. 缓存最近文本和消息历史。
 * 真正的生命周期管理仍由后台服务任务统一控制。
 */
static void official_chat_service_event_cb(const official_chat_event_t *event,
                                           void *user_data)
{
    (void)user_data;
    if (event == NULL)
    {
        return;
    }

    if (official_chat_service_copy_snapshot().stop_pending)
    {
        /* 关闭流程开始后丢弃后续事件，避免缓存重新被写入。 */
        return;
    }

    switch (event->type)
    {
    case OFFICIAL_CHAT_EVENT_STATE_CHANGED:
    {
        const official_chat_service_state_t state =
            map_official_chat_state(event->state);
        official_chat_service_set_state(state);
        official_chat_service_set_audio_channel_ready(
            official_chat_is_audio_channel_ready(s_chat_handle));
        ESP_LOGI(TAG, "state=%s",
                 official_chat_service_state_to_string(state));
        break;
    }
    case OFFICIAL_CHAT_EVENT_USER_TEXT:
        official_chat_service_lock();
        official_chat_service_store_text_locked(s_last_user_text,
                                                kLastUserTextMaxBytes,
                                                event->message);
        official_chat_service_enqueue_message_locked(
            OFFICIAL_CHAT_SERVICE_MESSAGE_ROLE_USER, event->message);
        official_chat_service_unlock();
        ESP_LOGI(TAG, "last_user_text=%s",
                 event->message != NULL ? event->message : "");
        break;
    case OFFICIAL_CHAT_EVENT_ASSISTANT_TEXT:
        official_chat_service_lock();
        official_chat_service_store_text_locked(s_last_assistant_text,
                                                kLastAssistantTextMaxBytes,
                                                event->message);
        official_chat_service_enqueue_message_locked(
            OFFICIAL_CHAT_SERVICE_MESSAGE_ROLE_ASSISTANT, event->message);
        official_chat_service_unlock();
        ESP_LOGI(TAG, "last_assistant_text=%s",
                 event->message != NULL ? event->message : "");
        break;
    case OFFICIAL_CHAT_EVENT_ERROR:
        official_chat_service_set_last_error(event->error);
        official_chat_service_set_state(OFFICIAL_CHAT_SERVICE_STATE_ERROR);
        ESP_LOGE(TAG, "error=%s message=%s", esp_err_to_name(event->error),
                 event->message != NULL ? event->message : "");
        break;
    case OFFICIAL_CHAT_EVENT_ACTIVATION_CODE:
    case OFFICIAL_CHAT_EVENT_ACTIVATION_MESSAGE:
    case OFFICIAL_CHAT_EVENT_ASSETS_PROGRESS:
    case OFFICIAL_CHAT_EVENT_UPGRADE_PROGRESS:
    case OFFICIAL_CHAT_EVENT_REBOOTING:
    default:
        break;
    }
}

/*
 * 创建并启动底层聊天实例。
 * 所有配置都在服务层集中指定，避免页面或控制器各自维护一份会话参数。
 */
static esp_err_t official_chat_service_start_internal(void)
{
    int speaker_volume = 60;
    if (audio_codec_get_volume(&speaker_volume) != ESP_OK)
    {
        ESP_LOGW(TAG, "speaker volume unavailable, use default 60%%");
    }

    /* 启动参数由服务层集中指定，避免页面或控制器分散管理底层配置。 */
    official_chat_config_t config = {
        .speak_volume = speaker_volume,
        .record_gain_db = 24.0f,
        .websocket_url = NULL,
        .access_token = NULL,
        .ota_url = CONFIG_OFFICIAL_CHAT_OTA_URL,
        .ensure_time_valid = official_chat_service_ensure_time_valid,
        .apply_server_time = official_chat_service_apply_server_time,
        .time_user_ctx = NULL,
    };

    official_chat_service_set_state(OFFICIAL_CHAT_SERVICE_STATE_STARTING);
    official_chat_service_set_last_error(ESP_OK);
    official_chat_service_set_audio_channel_ready(false);

    s_chat_handle = official_chat_create(&config);
    if (s_chat_handle == NULL)
    {
        official_chat_service_set_last_error(ESP_FAIL);
        official_chat_service_set_state(OFFICIAL_CHAT_SERVICE_STATE_ERROR);
        ESP_LOGE(TAG, "official_chat_create failed");
        return ESP_FAIL;
    }

    esp_err_t ret = official_chat_set_event_callback(
        s_chat_handle, official_chat_service_event_cb, NULL);
    if (ret != ESP_OK)
    {
        official_chat_destroy(s_chat_handle);
        s_chat_handle = NULL;
        official_chat_service_set_last_error(ret);
        official_chat_service_set_state(OFFICIAL_CHAT_SERVICE_STATE_ERROR);
        ESP_LOGE(TAG, "official_chat_set_event_callback failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ret = official_chat_start(s_chat_handle);
    if (ret != ESP_OK)
    {
        official_chat_set_event_callback(s_chat_handle, NULL, NULL);
        official_chat_destroy(s_chat_handle);
        s_chat_handle = NULL;
        official_chat_service_set_last_error(ret);
        official_chat_service_set_state(OFFICIAL_CHAT_SERVICE_STATE_ERROR);
        ESP_LOGE(TAG, "official_chat_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

/**
 * @brief 官方聊天后台服务任务。
 * @param arg 未使用，保留任务签名。
 *
 * 后台任务负责：
 * - 等待前台请求；
 * - 等待网络可用；
 * - 创建底层实例；
 * - 执行带 quiet period 的安全关闭。
 */
static void official_chat_service_task(void *arg)
{
    (void)arg;

    while (1)
    {
        official_chat_service_cmd_t command;
        while (s_command_queue != NULL &&
               xQueueReceive(s_command_queue, &command, 0) == pdTRUE)
        {
            official_chat_service_handle_command(&command);
        }

        if (s_shutdown_requested)
        {
            /*
             * 关闭流程分三步：
             * 1. 请求底层进入可关闭状态；
             * 2. 等待 idle，并额外留出 quiet period；
             * 3. 解绑回调、销毁实例、清空缓存。
             */
            official_chat_handle_t chat_handle = s_chat_handle;

            if (chat_handle != NULL)
            {
                const esp_err_t prepare_ret =
                    official_chat_prepare_shutdown(chat_handle);
                if (prepare_ret != ESP_OK)
                {
                    ESP_LOGW(TAG, "official_chat_prepare_shutdown failed: %s",
                             esp_err_to_name(prepare_ret));
                }
                official_chat_state_t chat_state =
                    official_chat_get_state(chat_handle);
                official_chat_service_set_state(
                    map_official_chat_state(chat_state));
                if (official_chat_service_requires_shutdown_quiet_period(
                        chat_state))
                {
                    if (chat_state != OFFICIAL_CHAT_STATE_IDLE &&
                        !s_shutdown_stop_requested)
                    {
                        const esp_err_t stop_ret =
                            official_chat_stop_listening(chat_handle);
                        if (stop_ret != ESP_OK)
                        {
                            ESP_LOGW(
                                TAG,
                                "official_chat_stop_listening during shutdown failed: %s",
                                esp_err_to_name(stop_ret));
                        }
                        s_shutdown_stop_requested = true;
                        ESP_LOGI(
                            TAG,
                            "shutdown waiting for idle before destroy state=%s",
                            official_chat_service_state_to_string(
                                map_official_chat_state(chat_state)));
                    }

                    if (chat_state != OFFICIAL_CHAT_STATE_IDLE)
                    {
                        if (s_shutdown_destroy_deadline_ticks != 0)
                        {
                            s_shutdown_destroy_deadline_ticks = 0;
                            ESP_LOGI(
                                TAG,
                                "shutdown quiet period canceled until idle state=%s",
                                official_chat_service_state_to_string(
                                    map_official_chat_state(chat_state)));
                        }
                        vTaskDelay(pdMS_TO_TICKS(50));
                        continue;
                    }

                    official_chat_service_set_state(
                        OFFICIAL_CHAT_SERVICE_STATE_IDLE);
                    if (s_shutdown_destroy_deadline_ticks == 0)
                    {
                        s_shutdown_destroy_deadline_ticks =
                            xTaskGetTickCount() +
                            pdMS_TO_TICKS(kShutdownTransportQuietPeriodMs);
                        ESP_LOGI(
                            TAG,
                            "shutdown reached idle, arming destroy quiet period wait_ms=%lu",
                            (unsigned long)kShutdownTransportQuietPeriodMs);
                        ESP_LOGI(TAG,
                                 "shutdown transport quiet period armed state=idle wait_ms=%lu",
                                 (unsigned long)kShutdownTransportQuietPeriodMs);
                    }

                    if (xTaskGetTickCount() <
                        s_shutdown_destroy_deadline_ticks)
                    {
                        vTaskDelay(pdMS_TO_TICKS(50));
                        continue;
                    }
                }
            }

            if (s_shutdown_stop_requested &&
                xTaskGetTickCount() < s_shutdown_destroy_deadline_ticks)
            {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }

            if (chat_handle != NULL)
            {
                official_chat_set_event_callback(chat_handle, NULL, NULL);
                official_chat_destroy(chat_handle);
            }

            s_chat_handle = NULL;

            official_chat_service_lock();
            official_chat_service_clear_cached_text_locked();
            official_chat_service_unlock();

            official_chat_service_set_foreground_audio_active(false, "official_chat");
            if (s_coordinator_quiesce_generation != 0U)
            {
                (void)runtime_coordinator_report_quiesce_result(
                    RUNTIME_COORDINATOR_PARTICIPANT_OFFICIAL_CHAT,
                    s_coordinator_quiesce_generation, ESP_OK);
                s_coordinator_quiesce_generation = 0U;
            }
            s_coordinator_request_generation = 0U;
            s_coordinator_start_reported = false;

            official_chat_service_set_last_error(ESP_OK);
            official_chat_service_set_audio_channel_ready(false);
            official_chat_service_set_state(OFFICIAL_CHAT_SERVICE_STATE_STOPPED);
            official_chat_service_set_lifecycle_intent(false, false);
            s_shutdown_requested = false;
            s_shutdown_stop_requested = false;
            s_shutdown_destroy_deadline_ticks = 0;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (!s_foreground_requested)
        {
            /* 未进入前台时保留任务本身，但不主动创建会话实例。 */
            if (s_chat_handle == NULL)
            {
                official_chat_service_set_state(
                    OFFICIAL_CHAT_SERVICE_STATE_STOPPED);
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (s_chat_handle != NULL)
        {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!network_service_is_service_ready())
        {
            /* 会话严格依赖网络服务层，而不是自己重复做联网判断。 */
            official_chat_service_set_state(
                OFFICIAL_CHAT_SERVICE_STATE_WAITING_NETWORK);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        const esp_err_t start_ret = official_chat_service_start_internal();
        if (start_ret != ESP_OK)
        {
            if (!s_coordinator_start_reported)
            {
                (void)runtime_coordinator_report_start_result(
                    RUNTIME_COORDINATOR_PARTICIPANT_OFFICIAL_CHAT,
                    s_coordinator_request_generation, start_ret);
                s_coordinator_start_reported = true;
            }
            official_chat_service_begin_shutdown_from_task();
            official_chat_service_set_last_error(start_ret);
            official_chat_service_set_state(OFFICIAL_CHAT_SERVICE_STATE_ERROR);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (!s_coordinator_start_reported)
        {
            (void)runtime_coordinator_report_start_result(
                RUNTIME_COORDINATOR_PARTICIPANT_OFFICIAL_CHAT,
                s_coordinator_request_generation, ESP_OK);
            s_coordinator_start_reported = true;
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/**
 * @brief 初始化官方聊天后台服务。
 * @return ESP_OK 表示已初始化或本次初始化成功。
 */
esp_err_t official_chat_service_init(void)
{
    if (s_service_task_handle != NULL)
    {
        return ESP_OK;
    }

    /* 使用静态互斥锁，避免服务初始化时再引入额外堆分配不确定性。 */
    if (s_text_mutex == NULL)
    {
        s_text_mutex = xSemaphoreCreateMutexStatic(&s_text_mutex_buffer);
    }

    const esp_err_t text_cache_err = official_chat_service_alloc_text_caches();
    if (text_cache_err != ESP_OK)
    {
        official_chat_service_set_state(OFFICIAL_CHAT_SERVICE_STATE_ERROR);
        official_chat_service_set_last_error(text_cache_err);
        return text_cache_err;
    }

    if (s_command_queue == NULL)
    {
        s_command_queue = xQueueCreateStatic(
            kCommandQueueLength,
            sizeof(official_chat_service_cmd_t),
            s_command_queue_storage,
            &s_command_queue_buffer);
    }

    const runtime_coordinator_participant_config_t participant = {
        .id = RUNTIME_COORDINATOR_PARTICIPANT_OFFICIAL_CHAT,
        .name = "official_chat",
        .capabilities =
            RUNTIME_COORDINATOR_CAPABILITY_FOREGROUND_EXCLUSIVE,
        .request_quiesce = official_chat_service_coordinator_quiesce,
        .grant_foreground = official_chat_service_coordinator_grant,
        .cancel_pending_request = official_chat_service_coordinator_cancel,
    };
    const esp_err_t register_ret = runtime_coordinator_register(&participant);
    if (register_ret != ESP_OK)
    {
        return register_ret;
    }

    if (s_command_queue == NULL)
    {
        official_chat_service_set_state(OFFICIAL_CHAT_SERVICE_STATE_ERROR);
        official_chat_service_set_last_error(ESP_FAIL);
        return ESP_FAIL;
    }

    /* 栈 4096B：该 task 只做命令分发与生命周期管理，重活由动态子任务承担，
     * 高压实测 free=7440B（90.8% 空闲），缩半仍有充裕余量。
     * 栈必须放 internal RAM：其调用链会触发 esp_partition_mmap（SR 模型加载），
     * flash mmap 期间 cache 冻结会导致 PSRAM 栈不可访问，引发 cache-freeze ASSERT。 */
    BaseType_t result = xTaskCreatePinnedToCoreWithCaps(
        official_chat_service_task, "official_chat_service", 1024 * 4, NULL, 5,
        &s_service_task_handle, 0, MALLOC_CAP_INTERNAL);
    if (result != pdPASS)
    {
        s_service_task_handle = NULL;
        official_chat_service_set_state(OFFICIAL_CHAT_SERVICE_STATE_ERROR);
        official_chat_service_set_last_error(ESP_FAIL);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief 标记聊天页进入前台。
 *
 * 该操作不会立即阻塞等待网络或实例启动，只是向后台任务声明“允许拉起会话”。
 */
void official_chat_service_enter_foreground(void)
{
    const esp_err_t ret = official_chat_service_post_command(
        OFFICIAL_CHAT_SERVICE_CMD_ENTER_FOREGROUND, 0);
    if (ret != ESP_OK)
    {
        official_chat_service_set_last_error(ret);
        ESP_LOGW(TAG, "enter_foreground command failed: %s",
                 esp_err_to_name(ret));
    }
}

/**
 * @brief 标记聊天页离开前台。
 *
 * V1 聊天是纯前台按需服务：页面退出就投递完整停机命令，
 * 不保留“离开页面但后台 idle”的隐式长期会话。
 */
void official_chat_service_leave_foreground(void)
{
    const esp_err_t ret = official_chat_service_post_command(
        OFFICIAL_CHAT_SERVICE_CMD_LEAVE_FOREGROUND_AND_STOP, 0);
    if (ret != ESP_OK)
    {
        official_chat_service_set_last_error(ret);
        ESP_LOGW(TAG, "leave_foreground command failed: %s",
                 esp_err_to_name(ret));
    }
}

/**
 * @brief 请求异步关闭聊天实例。
 */
void official_chat_service_request_shutdown(void)
{
    official_chat_service_leave_foreground();
}

/**
 * @brief 查询是否存在待处理的关闭请求。
 * @return true 表示后台任务仍在执行关闭流程。
 */
bool official_chat_service_is_shutdown_pending(void)
{
    return official_chat_service_copy_snapshot().stop_pending;
}

/**
 * @brief 同步等待聊天实例关闭完成。
 * @return 超时返回 `ESP_ERR_TIMEOUT`。
 */
esp_err_t official_chat_service_shutdown(void)
{
    const esp_err_t command_ret = official_chat_service_post_command(
        OFFICIAL_CHAT_SERVICE_CMD_LEAVE_FOREGROUND_AND_STOP,
        pdMS_TO_TICKS(100));
    if (command_ret != ESP_OK)
    {
        official_chat_service_set_last_error(command_ret);
        return command_ret;
    }

    const TickType_t start_ticks = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(kShutdownWaitTimeoutMs);

    while (true)
    {
        const official_chat_service_snapshot_t snapshot =
            official_chat_service_copy_snapshot();
        if (!snapshot.stop_pending &&
            snapshot.state == OFFICIAL_CHAT_SERVICE_STATE_STOPPED)
        {
            break;
        }

        if ((xTaskGetTickCount() - start_ticks) >= timeout_ticks)
        {
            return ESP_ERR_TIMEOUT;
        }

        if (s_service_task_handle != NULL)
        {
            xTaskAbortDelay(s_service_task_handle);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return ESP_OK;
}

/**
 * @brief 获取当前服务层状态。
 * @return 聊天服务状态枚举。
 */
official_chat_service_state_t official_chat_service_get_state(void)
{
    return official_chat_service_copy_snapshot().state;
}

/**
 * @brief 获取最近一次底层错误。
 * @return 错误码，`ESP_OK` 表示当前未记录错误。
 */
esp_err_t official_chat_service_get_last_error(void)
{
    return official_chat_service_copy_snapshot().last_error;
}

/**
 * @brief 获取服务生命周期快照。
 * @param out_snapshot 输出快照，不能为空。
 * @return `ESP_OK` 表示成功复制。
 */
esp_err_t official_chat_service_get_snapshot(
    official_chat_service_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_snapshot = official_chat_service_copy_snapshot();
    return ESP_OK;
}

/**
 * @brief 获取当前缓存消息条数。
 * @return 固定容量消息历史中当前有效条目数。
 */
size_t official_chat_service_get_message_count(void)
{
    size_t message_count = 0;

    official_chat_service_lock();
    message_count = s_message_count;
    official_chat_service_unlock();

    return message_count;
}

/**
 * @brief 按索引读取一条历史消息快照。
 * @param index 消息索引，越大表示越新的消息。
 * @param out_message 输出参数。
 * @return 越界时返回 `ESP_ERR_NOT_FOUND`。
 */
esp_err_t official_chat_service_get_message(
    size_t index, official_chat_service_message_t *out_message)
{
    if (out_message == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    official_chat_service_lock();
    if (index < s_message_count)
    {
        *out_message = s_message_history[index];
        ret = ESP_OK;
    }
    official_chat_service_unlock();

    return ret;
}

/**
 * @brief 获取最近一条用户文本。
 * @param buffer 输出缓冲区。
 * @param size 输出缓冲区长度。
 * @return 无缓存文本时返回 `ESP_ERR_NOT_FOUND`。
 */
esp_err_t official_chat_service_get_last_user_text(char *buffer, size_t size)
{
    official_chat_service_lock();
    esp_err_t ret = official_chat_service_copy_text_locked(s_last_user_text,
                                                           buffer, size);
    official_chat_service_unlock();
    return ret;
}

/**
 * @brief 获取最近一条助手文本。
 * @param buffer 输出缓冲区。
 * @param size 输出缓冲区长度。
 * @return 无缓存文本时返回 `ESP_ERR_NOT_FOUND`。
 */
esp_err_t official_chat_service_get_last_assistant_text(char *buffer,
                                                        size_t size)
{
    official_chat_service_lock();
    esp_err_t ret = official_chat_service_copy_text_locked(
        s_last_assistant_text, buffer, size);
    official_chat_service_unlock();
    return ret;
}

/**
 * @brief 供 AI 页面进入后触发语音通道预连接。
 */
esp_err_t official_chat_service_prepare_audio_channel(void)
{
    const esp_err_t ret = official_chat_service_post_command(
        OFFICIAL_CHAT_SERVICE_CMD_PREPARE_AUDIO_CHANNEL, 0);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "prepare_audio_channel command failed: %s",
                 esp_err_to_name(ret));
    }
    return ret;
}

/**
 * @brief 供 UI 按键触发开始聆听。
 */
esp_err_t official_chat_service_start_listening(void)
{
    const esp_err_t ret = official_chat_service_post_command(
        OFFICIAL_CHAT_SERVICE_CMD_START_LISTENING, 0);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "start_listening command failed: %s",
                 esp_err_to_name(ret));
    }
    return ret;
}

/**
 * @brief 供 UI 按键触发停止聆听。
 */
esp_err_t official_chat_service_stop_listening(void)
{
    const esp_err_t ret = official_chat_service_post_command(
        OFFICIAL_CHAT_SERVICE_CMD_STOP_LISTENING, pdMS_TO_TICKS(50));
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "stop_listening command failed: %s",
                 esp_err_to_name(ret));
    }
    return ret;
}
