#ifndef MEMORY_WATCH_VIEW_H
#define MEMORY_WATCH_VIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct memory_watch_view memory_watch_view_t;

/**
 * @brief 页面动作回调。
 *
 * 回调只表达用户意图，不能在 view 层直接访问网络、录音或 Hermes。
 */
typedef void (*memory_watch_view_action_cb_t)(void *user_data);

/**
 * @brief 收件箱消息点击回调。
 *
 * index 是当前渲染模型里的消息下标，view 层不持有消息 owner。
 */
typedef void (*memory_watch_view_inbox_item_cb_t)(size_t index,
                                                  void *user_data);

/**
 * @brief Hermes 页面内部子页面。
 */
typedef enum
{
    MEMORY_WATCH_VIEW_PAGE_VOICE = 0,        /**< 语音记录主页面。 */
    MEMORY_WATCH_VIEW_PAGE_INBOX,            /**< Hermes 收件箱列表。 */
    MEMORY_WATCH_VIEW_PAGE_INBOX_DETAIL,     /**< 收件箱消息详情。 */
} memory_watch_view_page_t;

/**
 * @brief Hermes 收件箱只读消息项。
 */
typedef struct
{
    const char *notification_id; /**< 服务器侧消息 ID；当前 UI 只展示，不修改。 */
    const char *created_at;      /**< 展示用创建时间。 */
    const char *text;            /**< 消息原文；列表页截断展示，详情页完整展示。 */
    bool read;                   /**< true 表示已读。 */
} memory_watch_view_inbox_item_t;

/**
 * @brief Hermes 本地对话消息角色。
 */
typedef enum
{
    MEMORY_WATCH_VIEW_CONVERSATION_USER = 0, /**< 用户语音/文本内容。 */
    MEMORY_WATCH_VIEW_CONVERSATION_HERMES,   /**< Hermes 回复内容。 */
    MEMORY_WATCH_VIEW_CONVERSATION_SYSTEM,   /**< 手表侧状态提示。 */
} memory_watch_view_conversation_role_t;

/**
 * @brief Hermes 连接状态点。
 */
typedef enum
{
    MEMORY_WATCH_VIEW_CONNECTION_UNKNOWN = 0, /**< 正在检测或状态未知。 */
    MEMORY_WATCH_VIEW_CONNECTION_ONLINE,      /**< Hermes 在线。 */
    MEMORY_WATCH_VIEW_CONNECTION_OFFLINE,     /**< 未联网、未配置或离线。 */
} memory_watch_view_connection_state_t;

/**
 * @brief Hermes 本地连续对话消息。
 */
typedef struct
{
    memory_watch_view_conversation_role_t role; /**< 消息角色，决定对齐和色彩。 */
    const char *request_id; /**< 关联请求 ID，用于调试和去重来源说明。 */
    const char *text;       /**< 展示文本，不能为空字符串。 */
} memory_watch_view_conversation_item_t;

/**
 * @brief Hermes 手表页面回调配置。
 */
typedef struct
{
    memory_watch_view_action_cb_t back_cb;
    memory_watch_view_action_cb_t press_start_cb;
    memory_watch_view_action_cb_t release_send_cb;
    memory_watch_view_action_cb_t slide_cancel_cb;
    memory_watch_view_action_cb_t cancel_waiting_cb;
    memory_watch_view_action_cb_t cancel_clarification_cb;
    memory_watch_view_action_cb_t open_inbox_cb;
    memory_watch_view_action_cb_t open_voice_cb;
    memory_watch_view_action_cb_t inbox_back_cb;
    memory_watch_view_inbox_item_cb_t open_inbox_item_cb;
    void *user_data;
} memory_watch_view_config_t;

/**
 * @brief Hermes 手表页面的只读渲染模型。
 */
typedef struct
{
    const char *top_status_text;
    const char *state_text;
    const char *user_text;
    const char *reply_text;
    const char *voice_button_text;
    memory_watch_view_page_t page;
    const memory_watch_view_conversation_item_t *conversation_items;
    size_t conversation_item_count;
    memory_watch_view_connection_state_t connection_state;
    const memory_watch_view_inbox_item_t *inbox_items;
    size_t inbox_item_count;
    size_t selected_inbox_index;
    uint8_t inbox_unread_count;
    const char *detail_body; /**< 详情页完整正文（controller 提供），NULL 时回退到 preview。 */
    bool voice_button_enabled;
    bool cancel_visible;
    bool cancel_is_clarification;
} memory_watch_view_model_t;

/**
 * @brief 创建独立 Hermes 手表页面。
 *
 * @param[in] config 页面动作回调配置，可以为 NULL。
 * @return 页面实例；内存不足时返回 NULL。
 */
memory_watch_view_t *memory_watch_view_create(
    const memory_watch_view_config_t *config);

/**
 * @brief 销毁 Hermes 手表页面。
 * @param[in] view 页面实例，可以为 NULL。
 */
void memory_watch_view_destroy(memory_watch_view_t *view);

/**
 * @brief 获取页面根 screen。
 * @param[in] view 页面实例。
 * @return LVGL screen 对象；view 为空时返回 NULL。
 */
lv_obj_t *memory_watch_view_get_screen(const memory_watch_view_t *view);

/**
 * @brief 应用 service 快照转换后的页面模型。
 *
 * 该函数只更新 LVGL 对象，不推进业务状态。
 */
void memory_watch_view_apply_model(memory_watch_view_t *view,
                                   const memory_watch_view_model_t *model);

#ifdef __cplusplus
}
#endif

#endif // MEMORY_WATCH_VIEW_H
