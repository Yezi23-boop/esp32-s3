#ifndef OFFICIAL_CHAT_SERVICE_H
#define OFFICIAL_CHAT_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/*
 * 官方聊天服务层：
 * - V1 是纯前台按需服务：页面进入才拉起 `official_chat` 会话，页面退出就完整停止会话；
 * - 长期存在的是 owner task，不是聊天会话本身；owner task 负责等待网络、转发状态、缓存文本；
 * - 提供给 UI 层一个稳定、低耦合的查询接口，避免页面直接操作底层会话对象。
 */

#ifdef __cplusplus
extern "C"
{
#endif

    /* 服务层对外暴露的聊天会话状态，统一屏蔽底层 `official_chat` 细节。 */
    typedef enum
    {
        OFFICIAL_CHAT_SERVICE_STATE_STOPPED = 0,     /* 未持有底层聊天实例。 */
        OFFICIAL_CHAT_SERVICE_STATE_WAITING_NETWORK, /* 等待 `network_service` 真正可用。 */
        OFFICIAL_CHAT_SERVICE_STATE_STARTING,        /* 正在创建实例或启动底层服务。 */
        OFFICIAL_CHAT_SERVICE_STATE_ACTIVATING,      /* 设备激活中。 */
        OFFICIAL_CHAT_SERVICE_STATE_CONNECTING,      /* WebSocket 或云端链路建立中。 */
        OFFICIAL_CHAT_SERVICE_STATE_IDLE,            /* 已就绪，等待唤醒或交互。 */
        OFFICIAL_CHAT_SERVICE_STATE_LISTENING,       /* 正在采集用户语音。 */
        OFFICIAL_CHAT_SERVICE_STATE_SPEAKING,        /* 正在播报助手回复。 */
        OFFICIAL_CHAT_SERVICE_STATE_ERROR,           /* 底层会话出错。 */
    } official_chat_service_state_t;

    typedef enum
    {
        OFFICIAL_CHAT_SERVICE_MESSAGE_ROLE_USER = 0,
        OFFICIAL_CHAT_SERVICE_MESSAGE_ROLE_ASSISTANT,
    } official_chat_service_message_role_t;

    typedef enum
    {
        OFFICIAL_CHAT_SERVICE_CMD_ENTER_FOREGROUND = 0,
        OFFICIAL_CHAT_SERVICE_CMD_LEAVE_FOREGROUND_AND_STOP,
        OFFICIAL_CHAT_SERVICE_CMD_NETWORK_READY,
        OFFICIAL_CHAT_SERVICE_CMD_BUDGET_CHANGED,
        OFFICIAL_CHAT_SERVICE_CMD_PREPARE_AUDIO_CHANNEL,
        OFFICIAL_CHAT_SERVICE_CMD_START_LISTENING,
        OFFICIAL_CHAT_SERVICE_CMD_STOP_LISTENING,
    } official_chat_service_cmd_type_t;

    /* 单条消息历史快照，供 UI 直接读取展示。 */
    typedef struct
    {
        official_chat_service_message_role_t role; // 消息角色：用户或助手
        char text[256];                            // UTF-8 文本内容（截断上限 255 字节）
    } official_chat_service_message_t;

    /* 服务生命周期快照；getter 只复制状态，不做 I/O 或状态推进。 */
    typedef struct
    {
        official_chat_service_state_t state; /* 当前服务状态。 */
        bool foreground_active;              /* UI 是否声明聊天前台活跃。 */
        bool stop_pending;                   /* 是否已有退出/停机命令待收敛。 */
        bool audio_channel_ready;            /* 底层 WebSocket/音频通道是否已预连接。 */
        esp_err_t last_error;                /* 最近一次底层或服务错误。 */
    } official_chat_service_snapshot_t;

    /* 初始化后台服务任务；重复调用安全。 */
    esp_err_t official_chat_service_init(void);

    /* 标记聊天页进入前台，后台任务会在网络满足条件后自动拉起会话。 */
    void official_chat_service_enter_foreground(void);

    /* 标记聊天页离开前台，并请求后台任务完整停止聊天服务。 */
    void official_chat_service_leave_foreground(void);

    /* 兼容旧调用：等价于 `official_chat_service_leave_foreground()`。 */
    void official_chat_service_request_shutdown(void);

    /* 查询是否已有关闭流程在进行中。 */
    bool official_chat_service_is_shutdown_pending(void);

    /* 同步等待关闭完成；V1 UI 退出页不直接调用，避免阻塞 LVGL。 */
    esp_err_t official_chat_service_shutdown(void);

    /* 获取服务层状态。 */
    official_chat_service_state_t official_chat_service_get_state(void);

    /* 获取最近一次底层错误。 */
    esp_err_t official_chat_service_get_last_error(void);

    /* 获取服务生命周期快照。 */
    esp_err_t official_chat_service_get_snapshot(
        official_chat_service_snapshot_t *out_snapshot);

    /* 获取当前缓存的消息条数。 */
    size_t official_chat_service_get_message_count(void);

    /* 读取历史消息快照，索引越大表示越新的消息。 */
    esp_err_t official_chat_service_get_message(size_t index,
                                                official_chat_service_message_t *out_message);

    /* 获取最近一条用户文本。 */
    esp_err_t official_chat_service_get_last_user_text(char *buffer, size_t size);

    /* 获取最近一条助手文本。 */
    esp_err_t official_chat_service_get_last_assistant_text(char *buffer,
                                                            size_t size);

    /* 供 UI 页面进入后触发 WebSocket/音频通道预连接。 */
    esp_err_t official_chat_service_prepare_audio_channel(void);

    /* 供 UI 按键触发开始聆听。 */
    esp_err_t official_chat_service_start_listening(void);

    /* 供 UI 按键触发停止聆听。 */
    esp_err_t official_chat_service_stop_listening(void);

    /* 供日志或 UI 文本展示使用的状态字符串。 */
    const char *official_chat_service_state_to_string(
        official_chat_service_state_t state);

#ifdef __cplusplus
}
#endif

#endif // OFFICIAL_CHAT_SERVICE_H
