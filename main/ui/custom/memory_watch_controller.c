#include "memory_watch_controller.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"
#include "memory_watch_view.h"
#include "services/memory_watch_service.h"
#include "ui_chinese_fonts.h"

static const char *TAG = "memory_watch_ui";

typedef struct
{
    bool valid;
    bool voice_button_enabled;
    bool cancel_visible;
    bool cancel_is_clarification;
    memory_watch_view_page_t page;
    memory_watch_view_connection_state_t connection_state;
    size_t conversation_item_count;
    uint32_t conversation_revision;
    uint32_t inbox_generation;
    size_t inbox_item_count;
    size_t selected_inbox_index;
    uint8_t inbox_unread_count;
    char top_status_text[40];
    char state_text[40];
    char user_text[MEMORY_WATCH_SERVICE_TEXT_MAX_BYTES];
    char reply_text[MEMORY_WATCH_SERVICE_TEXT_MAX_BYTES];
    char voice_button_text[24];
} memory_watch_render_cache_t;

static lv_ui *s_ui = NULL;
static memory_watch_view_t *s_view = NULL;
static lv_timer_t *s_destroy_timer = NULL;
static memory_watch_view_t *s_pending_destroy_view = NULL;
static memory_watch_render_cache_t s_render_cache = {0};
static memory_watch_view_page_t s_render_page = MEMORY_WATCH_VIEW_PAGE_VOICE;
static size_t s_selected_inbox_index = 0;
static lv_obj_t *s_back_screen = NULL;
static memory_watch_service_conversation_item_t
    s_conversation_items[MEMORY_WATCH_SERVICE_CONVERSATION_MAX_ITEMS];
static memory_watch_view_conversation_item_t
    s_conversation_view_items[MEMORY_WATCH_SERVICE_CONVERSATION_MAX_ITEMS];
static size_t s_conversation_item_count = 0;
static uint32_t s_conversation_revision = 0;

/* ── 真实 inbox snapshot 缓冲区（controller 私有，LVGL task 写读）── */
#define MEMORY_WATCH_INBOX_CTRL_MAX 20U

#ifdef AGENT_PREVIEW_HOST
/* host 预览保留静态 mock，方便截图验证 */
static memory_watch_view_inbox_item_t s_preview_inbox_items[] = {
    {
        .notification_id = "preview-006",
        .created_at = "今天 16:20",
        .text = "Hermes 已把下午三点取快递整理成提醒. 到点前我会把这件事带回手表.",
        .read = false,
    },
    {
        .notification_id = "preview-005",
        .created_at = "今天 11:42",
        .text = "电池日志可以晚饭后再看. Hermes 建议先确认待机电流和屏幕亮度曲线.",
        .read = false,
    },
    {
        .notification_id = "preview-004",
        .created_at = "昨天 22:08",
        .text = "已经记录 UI 优化方向: 米白画布, 浅边框卡片, 低饱和状态色.",
        .read = true,
    },
};
static uint32_t s_inbox_generation  = 0;
static memory_watch_inbox_item_t s_inbox_detail;
static bool s_inbox_detail_valid = false;

static void memory_watch_controller_set_preview_detail(size_t index)
{
    if (index >= (sizeof(s_preview_inbox_items) /
                  sizeof(s_preview_inbox_items[0])))
    {
        s_inbox_detail_valid = false;
        return;
    }

    const memory_watch_view_inbox_item_t *item = &s_preview_inbox_items[index];
    memset(&s_inbox_detail, 0, sizeof(s_inbox_detail));
    snprintf(s_inbox_detail.notification_id, sizeof(s_inbox_detail.notification_id),
             "%s", item->notification_id != NULL ? item->notification_id : "");
    snprintf(s_inbox_detail.created_at, sizeof(s_inbox_detail.created_at),
             "%s", item->created_at != NULL ? item->created_at : "");
    snprintf(s_inbox_detail.title, sizeof(s_inbox_detail.title), "Hermes 提示");
    snprintf(s_inbox_detail.preview, sizeof(s_inbox_detail.preview), "%s",
             item->text != NULL ? item->text : "");
    snprintf(s_inbox_detail.body, sizeof(s_inbox_detail.body),
             "%s\n\n这是 host 预览里的完整正文区域. 打开详情会本地标记已读, "
             "但不会回复、删除或调用真实服务器.",
             item->text != NULL ? item->text : "");
    s_inbox_detail.read = true;
    s_inbox_detail_valid = true;
}
#else
/* 板端：summary 缓冲区，controller 在 generation 变化时刷新 */
static memory_watch_inbox_summary_t
    s_inbox_summaries[MEMORY_WATCH_INBOX_CTRL_MAX];
static memory_watch_view_inbox_item_t
    s_inbox_view_items[MEMORY_WATCH_INBOX_CTRL_MAX];
static size_t  s_inbox_item_count   = 0;
static uint8_t s_inbox_unread_count = 0;
static uint32_t s_inbox_generation  = 0;
/* 详情缓冲区：进入详情时按 ID 拷贝完整 item */
static memory_watch_inbox_item_t s_inbox_detail;
static bool s_inbox_detail_valid = false;
#endif

static void memory_watch_controller_refresh(void);

static const memory_watch_view_inbox_item_t *memory_watch_inbox_items(void)
{
#ifdef AGENT_PREVIEW_HOST
    return s_preview_inbox_items;
#else
    return s_inbox_view_items;
#endif
}

static size_t memory_watch_inbox_item_count(void)
{
#ifdef AGENT_PREVIEW_HOST
    return sizeof(s_preview_inbox_items) / sizeof(s_preview_inbox_items[0]);
#else
    return s_inbox_item_count;
#endif
}

static uint8_t memory_watch_inbox_unread_count(void)
{
#ifdef AGENT_PREVIEW_HOST
    uint8_t count = 0;
    const memory_watch_view_inbox_item_t *items = memory_watch_inbox_items();
    const size_t item_count = memory_watch_inbox_item_count();
    for (size_t i = 0; i < item_count; ++i)
    {
        if (!items[i].read && count < UINT8_MAX)
        {
            ++count;
        }
    }
    return count;
#else
    return s_inbox_unread_count;
#endif
}

/**
 * @brief 从 service snapshot 同步 inbox summary（仅在 generation 变化时调用）。
 */
#ifndef AGENT_PREVIEW_HOST
static void memory_watch_controller_sync_inbox(void)
{
    memory_watch_inbox_meta_t meta = {0};
    if (memory_watch_service_get_inbox_meta(&meta) != ESP_OK)
    {
        return;
    }
    if (meta.generation == s_inbox_generation && s_inbox_generation != 0U)
    {
        return; /* 无变化，跳过拷贝 */
    }
    s_inbox_generation  = meta.generation;
    s_inbox_unread_count = meta.unread_count;

    size_t new_count = 0;
    (void)memory_watch_service_copy_inbox_summaries(
        s_inbox_summaries, MEMORY_WATCH_INBOX_CTRL_MAX, &new_count);
    s_inbox_item_count = new_count;

    /* 将 summary 映射到 view item（text 字段用 preview 填充） */
    for (size_t i = 0; i < new_count; ++i)
    {
        s_inbox_view_items[i].notification_id =
            s_inbox_summaries[i].notification_id;
        s_inbox_view_items[i].created_at =
            s_inbox_summaries[i].created_at;
        /* view 层 text 字段 = preview（列表截断展示） */
        s_inbox_view_items[i].text = s_inbox_summaries[i].preview;
        s_inbox_view_items[i].read = s_inbox_summaries[i].read;
    }
}
#endif

static void memory_watch_controller_flush_pending_destroy(void)
{
    if (s_destroy_timer != NULL)
    {
        lv_timer_delete(s_destroy_timer);
        s_destroy_timer = NULL;
    }

    if (s_pending_destroy_view != NULL)
    {
        memory_watch_view_destroy(s_pending_destroy_view);
        s_pending_destroy_view = NULL;
    }
}

static void memory_watch_controller_destroy_view_cb(lv_timer_t *timer)
{
    if (timer != NULL)
    {
        lv_timer_delete(timer);
    }
    s_destroy_timer = NULL;

    if (s_pending_destroy_view != NULL)
    {
        memory_watch_view_destroy(s_pending_destroy_view);
        s_pending_destroy_view = NULL;
    }
}

static void memory_watch_controller_schedule_view_destroy(void)
{
    if (s_view == NULL)
    {
        return;
    }

    memory_watch_view_t *view_to_destroy = s_view;
    s_view = NULL;
    s_render_cache.valid = false;

    memory_watch_controller_flush_pending_destroy();

    s_pending_destroy_view = view_to_destroy;
    s_destroy_timer =
        lv_timer_create(memory_watch_controller_destroy_view_cb, 350, NULL);
}

static void memory_watch_copy_text(char *dst, size_t dst_size,
                                   const char *src)
{
    if (dst == NULL || dst_size == 0U)
    {
        return;
    }
    snprintf(dst, dst_size, "%s", src != NULL ? src : "");
}

static const memory_watch_view_conversation_item_t *
memory_watch_conversation_view_items(void)
{
    for (size_t i = 0; i < s_conversation_item_count; ++i)
    {
        switch (s_conversation_items[i].role)
        {
        case MEMORY_WATCH_SERVICE_CONVERSATION_USER:
            s_conversation_view_items[i].role =
                MEMORY_WATCH_VIEW_CONVERSATION_USER;
            break;
        case MEMORY_WATCH_SERVICE_CONVERSATION_HERMES:
            s_conversation_view_items[i].role =
                MEMORY_WATCH_VIEW_CONVERSATION_HERMES;
            break;
        default:
            s_conversation_view_items[i].role =
                MEMORY_WATCH_VIEW_CONVERSATION_SYSTEM;
            break;
        }
        s_conversation_view_items[i].request_id =
            s_conversation_items[i].request_id;
        s_conversation_view_items[i].text = s_conversation_items[i].text;
    }
    return s_conversation_view_items;
}

static void memory_watch_controller_sync_conversation(
    const memory_watch_service_snapshot_t *snapshot)
{
    if (snapshot == NULL ||
        snapshot->conversation_generation == s_conversation_revision)
    {
        return;
    }

    size_t count = 0;
    if (memory_watch_service_copy_conversation_items(
            s_conversation_items, MEMORY_WATCH_SERVICE_CONVERSATION_MAX_ITEMS,
            &count) == ESP_OK)
    {
        s_conversation_item_count = count;
        s_conversation_revision = snapshot->conversation_generation;
    }
}

static memory_watch_view_connection_state_t memory_watch_connection_state(
    const memory_watch_service_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return MEMORY_WATCH_VIEW_CONNECTION_UNKNOWN;
    }
    if (!snapshot->network_ready || !snapshot->endpoint_configured)
    {
        return MEMORY_WATCH_VIEW_CONNECTION_OFFLINE;
    }
    if (snapshot->hermes_online)
    {
        return MEMORY_WATCH_VIEW_CONNECTION_ONLINE;
    }
    return MEMORY_WATCH_VIEW_CONNECTION_UNKNOWN;
}

static bool memory_watch_is_busy_state(memory_watch_service_state_t state)
{
    return state == MEMORY_WATCH_SERVICE_STATE_RECORDING ||
           state == MEMORY_WATCH_SERVICE_STATE_ENCODING ||
           state == MEMORY_WATCH_SERVICE_STATE_UPLOADING ||
           state == MEMORY_WATCH_SERVICE_STATE_THINKING;
}

static const char *memory_watch_state_text(
    const memory_watch_service_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return "待检测";
    }

    switch (snapshot->state)
    {
    case MEMORY_WATCH_SERVICE_STATE_READY:
        return "待命";
    case MEMORY_WATCH_SERVICE_STATE_WAITING_NETWORK:
        return "等待网络";
    case MEMORY_WATCH_SERVICE_STATE_RECORDING:
        return "正在聆听";
    case MEMORY_WATCH_SERVICE_STATE_ENCODING:
    case MEMORY_WATCH_SERVICE_STATE_UPLOADING:
        return "上传中";
    case MEMORY_WATCH_SERVICE_STATE_THINKING:
        return "思考中";
    case MEMORY_WATCH_SERVICE_STATE_NEEDS_CLARIFICATION:
        return "需要补充说明";
    case MEMORY_WATCH_SERVICE_STATE_DONE:
        return "已完成";
    case MEMORY_WATCH_SERVICE_STATE_TIMEOUT:
        return "超时";
    case MEMORY_WATCH_SERVICE_STATE_ERROR:
        return "异常";
    case MEMORY_WATCH_SERVICE_STATE_CANCELED:
        return "已取消";
    default:
        return "未知状态";
    }
}

static const char *memory_watch_top_status_text(
    const memory_watch_service_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return "Hermes 待检测";
    }
    if (!snapshot->network_ready)
    {
        return "Hermes 未联网";
    }
    if (!snapshot->endpoint_configured)
    {
        return "Hermes 未配置";
    }
    if (snapshot->hermes_online)
    {
        return "Hermes 在线";
    }
    return "Hermes 待检测";
}

static bool memory_watch_can_use_voice_button(
    const memory_watch_service_snapshot_t *snapshot)
{
    if (snapshot == NULL || !snapshot->network_ready ||
        !snapshot->endpoint_configured)
    {
        return false;
    }

    switch (snapshot->state)
    {
    case MEMORY_WATCH_SERVICE_STATE_READY:
    case MEMORY_WATCH_SERVICE_STATE_RECORDING:
    case MEMORY_WATCH_SERVICE_STATE_DONE:
    case MEMORY_WATCH_SERVICE_STATE_TIMEOUT:
    case MEMORY_WATCH_SERVICE_STATE_ERROR:
    case MEMORY_WATCH_SERVICE_STATE_CANCELED:
    case MEMORY_WATCH_SERVICE_STATE_NEEDS_CLARIFICATION:
        return true;
    case MEMORY_WATCH_SERVICE_STATE_WAITING_NETWORK:
    case MEMORY_WATCH_SERVICE_STATE_ENCODING:
    case MEMORY_WATCH_SERVICE_STATE_UPLOADING:
    case MEMORY_WATCH_SERVICE_STATE_THINKING:
    default:
        return false;
    }
}

static const char *memory_watch_voice_button_text(
    const memory_watch_service_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return "等待";
    }
    if (!snapshot->network_ready)
    {
        return "等待网络";
    }
    if (!snapshot->endpoint_configured)
    {
        return "未配置";
    }
    if (snapshot->state == MEMORY_WATCH_SERVICE_STATE_RECORDING)
    {
        return "松开发送";
    }
    if (snapshot->state == MEMORY_WATCH_SERVICE_STATE_ENCODING ||
        snapshot->state == MEMORY_WATCH_SERVICE_STATE_UPLOADING ||
        snapshot->state == MEMORY_WATCH_SERVICE_STATE_THINKING)
    {
        return "处理中";
    }
    return "按住说话";
}

static const char *memory_watch_default_reply_text(
    const memory_watch_service_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return "按住按钮说话";
    }
    if (!snapshot->network_ready)
    {
        return "等待 Wi-Fi 后再交给 Hermes";
    }
    if (!snapshot->endpoint_configured)
    {
        return "请先配置 watch endpoint";
    }

    switch (snapshot->state)
    {
    case MEMORY_WATCH_SERVICE_STATE_RECORDING:
        return "松手后发送给 Hermes";
    case MEMORY_WATCH_SERVICE_STATE_ENCODING:
    case MEMORY_WATCH_SERVICE_STATE_UPLOADING:
        return "正在上传语音";
    case MEMORY_WATCH_SERVICE_STATE_THINKING:
        return "Hermes 正在处理";
    case MEMORY_WATCH_SERVICE_STATE_NEEDS_CLARIFICATION:
        return "Hermes 需要你补充一句";
    case MEMORY_WATCH_SERVICE_STATE_DONE:
        return "已完成";
    case MEMORY_WATCH_SERVICE_STATE_TIMEOUT:
        return "这次等待超时";
    case MEMORY_WATCH_SERVICE_STATE_ERROR:
        return "请求失败";
    case MEMORY_WATCH_SERVICE_STATE_CANCELED:
        return "已取消";
    case MEMORY_WATCH_SERVICE_STATE_READY:
    case MEMORY_WATCH_SERVICE_STATE_WAITING_NETWORK:
    default:
        return "按住按钮说话";
    }
}

static void memory_watch_fill_user_text(
    const memory_watch_service_snapshot_t *snapshot,
    char *buffer,
    size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0U)
    {
        return;
    }
    if (snapshot == NULL)
    {
        buffer[0] = '\0';
        return;
    }
    if (snapshot->asr_text[0] != '\0')
    {
        memory_watch_copy_text(buffer, buffer_size, snapshot->asr_text);
        return;
    }

    switch (snapshot->state)
    {
    case MEMORY_WATCH_SERVICE_STATE_RECORDING:
        memory_watch_copy_text(buffer, buffer_size, "正在聆听...");
        break;
    case MEMORY_WATCH_SERVICE_STATE_ENCODING:
        memory_watch_copy_text(buffer, buffer_size, "正在整理语音...");
        break;
    case MEMORY_WATCH_SERVICE_STATE_UPLOADING:
    case MEMORY_WATCH_SERVICE_STATE_THINKING:
        memory_watch_copy_text(buffer, buffer_size, "语音已发送");
        break;
    default:
        buffer[0] = '\0';
        break;
    }
}

static bool memory_watch_render_cache_matches(
    const memory_watch_render_cache_t *cache,
    const memory_watch_view_model_t *model)
{
    if (cache == NULL || model == NULL || !cache->valid)
    {
        return false;
    }

    return cache->voice_button_enabled == model->voice_button_enabled &&
           cache->cancel_visible == model->cancel_visible &&
           cache->cancel_is_clarification == model->cancel_is_clarification &&
           cache->page == model->page &&
           cache->connection_state == model->connection_state &&
           cache->conversation_item_count == model->conversation_item_count &&
           cache->conversation_revision == s_conversation_revision &&
           cache->inbox_generation == s_inbox_generation &&
           cache->inbox_item_count == model->inbox_item_count &&
           cache->selected_inbox_index == model->selected_inbox_index &&
           cache->inbox_unread_count == model->inbox_unread_count &&
           strcmp(cache->top_status_text, model->top_status_text) == 0 &&
           strcmp(cache->state_text, model->state_text) == 0 &&
           strcmp(cache->user_text, model->user_text) == 0 &&
           strcmp(cache->reply_text, model->reply_text) == 0 &&
           strcmp(cache->voice_button_text, model->voice_button_text) == 0;
}

static void memory_watch_render_cache_store(
    memory_watch_render_cache_t *cache,
    const memory_watch_view_model_t *model)
{
    if (cache == NULL || model == NULL)
    {
        return;
    }

    cache->valid = true;
    cache->voice_button_enabled = model->voice_button_enabled;
    cache->cancel_visible = model->cancel_visible;
    cache->cancel_is_clarification = model->cancel_is_clarification;
    cache->page = model->page;
    cache->connection_state = model->connection_state;
    cache->conversation_item_count = model->conversation_item_count;
    cache->conversation_revision = s_conversation_revision;
    cache->inbox_generation = s_inbox_generation;
    cache->inbox_item_count = model->inbox_item_count;
    cache->selected_inbox_index = model->selected_inbox_index;
    cache->inbox_unread_count = model->inbox_unread_count;
    memory_watch_copy_text(cache->top_status_text,
                           sizeof(cache->top_status_text),
                           model->top_status_text);
    memory_watch_copy_text(cache->state_text, sizeof(cache->state_text),
                           model->state_text);
    memory_watch_copy_text(cache->user_text, sizeof(cache->user_text),
                           model->user_text);
    memory_watch_copy_text(cache->reply_text, sizeof(cache->reply_text),
                           model->reply_text);
    memory_watch_copy_text(cache->voice_button_text,
                           sizeof(cache->voice_button_text),
                           model->voice_button_text);
}

static void memory_watch_controller_back(void *user_data)
{
    (void)user_data;

    if (s_ui == NULL || s_ui->screen_main == NULL || s_view == NULL)
    {
        return;
    }

    memory_watch_service_snapshot_t snapshot = {0};
    if (memory_watch_service_get_snapshot(&snapshot) == ESP_OK)
    {
        if (snapshot.state == MEMORY_WATCH_SERVICE_STATE_RECORDING ||
            snapshot.state == MEMORY_WATCH_SERVICE_STATE_ENCODING)
        {
            (void)memory_watch_service_cancel_recording();
        }
        else if (snapshot.state ==
                 MEMORY_WATCH_SERVICE_STATE_NEEDS_CLARIFICATION)
        {
            (void)memory_watch_service_cancel_clarification();
        }
        /* V2.1 语义：离开 Hermes 页面不等于取消已上传的 Hermes 任务。
         * V2.2 起离页会让 service 关闭前台 WS，并用后台 conversation polling
         * 继续等待结果；只有页面内“取消”等待按钮才显式取消。 */
    }

    (void)memory_watch_service_set_foreground(false);
    s_render_cache.valid = false;
    lv_obj_t *target_scr = s_back_screen != NULL ? s_back_screen : s_ui->screen_main;
    lv_screen_load_anim(target_scr, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
    s_back_screen = NULL;
    memory_watch_controller_schedule_view_destroy();
}

static void memory_watch_controller_press_start(void *user_data)
{
    (void)user_data;
    (void)memory_watch_service_begin_recording();
    s_render_cache.valid = false;
    memory_watch_controller_refresh();
}

static void memory_watch_controller_release_send(void *user_data)
{
    (void)user_data;
    (void)memory_watch_service_send_recording();
    s_render_cache.valid = false;
    memory_watch_controller_refresh();
}

static void memory_watch_controller_slide_cancel(void *user_data)
{
    (void)user_data;
    (void)memory_watch_service_cancel_recording();
    s_render_cache.valid = false;
    memory_watch_controller_refresh();
}

static void memory_watch_controller_cancel_waiting(void *user_data)
{
    (void)user_data;
    (void)memory_watch_service_cancel_waiting();
    s_render_cache.valid = false;
    memory_watch_controller_refresh();
}

static void memory_watch_controller_cancel_clarification(void *user_data)
{
    (void)user_data;
    (void)memory_watch_service_cancel_clarification();
    s_render_cache.valid = false;
    memory_watch_controller_refresh();
}

static void memory_watch_controller_open_inbox(void *user_data)
{
    (void)user_data;
    s_render_page = MEMORY_WATCH_VIEW_PAGE_INBOX;
    s_render_cache.valid = false;
#ifndef AGENT_PREVIEW_HOST
    /* 打开收件箱：立即触发一次 inbox 拉取（poll_now），按计划规则 */
    (void)memory_watch_service_inbox_poll_now("open_inbox");
    /* 强制同步一次 summary，确保列表不因旧缓存显示旧数据 */
    memory_watch_controller_sync_inbox();
#endif
    memory_watch_controller_refresh();
}

static void memory_watch_controller_open_voice(void *user_data)
{
    (void)user_data;
    s_render_page = MEMORY_WATCH_VIEW_PAGE_VOICE;
    s_render_cache.valid = false;
    memory_watch_controller_refresh();
}

static void memory_watch_controller_inbox_back(void *user_data)
{
    (void)user_data;
    s_render_page = MEMORY_WATCH_VIEW_PAGE_INBOX;
    s_render_cache.valid = false;
    memory_watch_controller_refresh();
}

static void memory_watch_controller_open_inbox_item(size_t index,
                                                    void *user_data)
{
    (void)user_data;
    if (index >= memory_watch_inbox_item_count())
    {
        return;
    }

    s_selected_inbox_index = index;
#ifdef AGENT_PREVIEW_HOST
    s_preview_inbox_items[index].read = true;
    memory_watch_controller_set_preview_detail(index);
#else
    /* 本地立即标记已读，异步上报服务器 */
    const char *nid =
        s_inbox_summaries[index].notification_id;
    if (nid != NULL && nid[0] != '\0')
    {
        (void)memory_watch_service_inbox_mark_read(nid);
        /* 同步 summary 中的已读状态，避免 UI 刷新时回退 */
        s_inbox_summaries[index].read = true;
        s_inbox_view_items[index].read = true;
        if (s_inbox_unread_count > 0U)
        {
            --s_inbox_unread_count;
        }
        /* 拷贝详情（含 body）供 detail 页使用 */
        const esp_err_t detail_err =
            memory_watch_service_get_inbox_item(nid, &s_inbox_detail);
        s_inbox_detail_valid = (detail_err == ESP_OK);
    }
#endif
    s_render_page = MEMORY_WATCH_VIEW_PAGE_INBOX_DETAIL;
    s_render_cache.valid = false;
    memory_watch_controller_refresh();
}

static bool memory_watch_controller_is_foreground(void)
{
    return s_view != NULL &&
           lv_screen_active() == memory_watch_view_get_screen(s_view);
}

static void memory_watch_controller_ensure_view_created(void)
{
    if (s_view != NULL)
    {
        return;
    }

    static const memory_watch_view_config_t kConfig = {
        .back_cb = memory_watch_controller_back,
        .press_start_cb = memory_watch_controller_press_start,
        .release_send_cb = memory_watch_controller_release_send,
        .slide_cancel_cb = memory_watch_controller_slide_cancel,
        .cancel_waiting_cb = memory_watch_controller_cancel_waiting,
        .cancel_clarification_cb = memory_watch_controller_cancel_clarification,
        .open_inbox_cb = memory_watch_controller_open_inbox,
        .open_voice_cb = memory_watch_controller_open_voice,
        .inbox_back_cb = memory_watch_controller_inbox_back,
        .open_inbox_item_cb = memory_watch_controller_open_inbox_item,
        .user_data = NULL,
    };

    s_view = memory_watch_view_create(&kConfig);
    if (s_view == NULL)
    {
        ESP_LOGE(TAG, "memory_watch_view_create failed");
    }
}

static void memory_watch_controller_refresh(void)
{
    if (s_view == NULL)
    {
        return;
    }

    memory_watch_service_snapshot_t snapshot = {0};
    const esp_err_t err = memory_watch_service_get_snapshot(&snapshot);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "get memory watch snapshot failed: %s",
                 esp_err_to_name(err));
        return;
    }

    char user_text[MEMORY_WATCH_SERVICE_TEXT_MAX_BYTES];
    char reply_text[MEMORY_WATCH_SERVICE_TEXT_MAX_BYTES];
    memory_watch_fill_user_text(&snapshot, user_text, sizeof(user_text));
    memory_watch_copy_text(reply_text, sizeof(reply_text),
                           snapshot.reply_text[0] != '\0'
                               ? snapshot.reply_text
                               : memory_watch_default_reply_text(&snapshot));
    memory_watch_controller_sync_conversation(&snapshot);
#ifndef AGENT_PREVIEW_HOST
    /* 每次 refresh 以极低成本检查 inbox generation；仅在 generation 变化时拷贝 summary */
    memory_watch_controller_sync_inbox();
#endif

    const bool busy = memory_watch_is_busy_state(snapshot.state);
    const memory_watch_view_model_t model = {
        .top_status_text = memory_watch_top_status_text(&snapshot),
        .state_text = memory_watch_state_text(&snapshot),
        .user_text = user_text,
        .reply_text = reply_text,
        .voice_button_text = memory_watch_voice_button_text(&snapshot),
        .page = s_render_page,
        .conversation_items = memory_watch_conversation_view_items(),
        .conversation_item_count = s_conversation_item_count,
        .connection_state = memory_watch_connection_state(&snapshot),
        .inbox_items = memory_watch_inbox_items(),
        .inbox_item_count = memory_watch_inbox_item_count(),
        .selected_inbox_index = s_selected_inbox_index,
        .inbox_unread_count = memory_watch_inbox_unread_count(),
        .detail_body = s_inbox_detail_valid ? s_inbox_detail.body : NULL,
        .voice_button_enabled = memory_watch_can_use_voice_button(&snapshot),
        .cancel_visible =
            busy ||
            snapshot.state == MEMORY_WATCH_SERVICE_STATE_NEEDS_CLARIFICATION,
        .cancel_is_clarification =
            snapshot.state == MEMORY_WATCH_SERVICE_STATE_NEEDS_CLARIFICATION,
    };

    if (memory_watch_render_cache_matches(&s_render_cache, &model))
    {
        return;
    }
    memory_watch_view_apply_model(s_view, &model);
    memory_watch_render_cache_store(&s_render_cache, &model);
}

void memory_watch_controller_init(lv_ui *ui)
{
    s_ui = ui;
}

void memory_watch_controller_open(void)
{
    /* 记录当前活跃的 screen，供返回使用 */
    s_back_screen = lv_screen_active();

    memory_watch_controller_ensure_view_created();
    if (s_view == NULL)
    {
        return;
    }

    s_render_page = MEMORY_WATCH_VIEW_PAGE_VOICE;
    (void)memory_watch_service_set_foreground(true);
    (void)memory_watch_service_check_health();
    s_render_cache.valid = false;
    memory_watch_controller_refresh();
    lv_screen_load_anim(memory_watch_view_get_screen(s_view),
                        LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}

void memory_watch_controller_poll_ui(void)
{
    /* 1. 设置 inbox list 活跃状态（避免前台列表弹气泡） */
    const bool is_fg = memory_watch_controller_is_foreground();
    static bool s_last_foreground = false;
    if (is_fg != s_last_foreground)
    {
        (void)memory_watch_service_set_foreground(is_fg);
        s_last_foreground = is_fg;
    }
    watch_nc_set_inbox_list_active(is_fg && (s_render_page == MEMORY_WATCH_VIEW_PAGE_INBOX));

    /* 2. 无论是否在前台，都获取 snapshot 以进行状态变化检测（后台回复气泡） */
    memory_watch_service_snapshot_t snapshot = {0};
    static memory_watch_service_state_t s_last_service_state = MEMORY_WATCH_SERVICE_STATE_READY;
    
    if (memory_watch_service_get_snapshot(&snapshot) == ESP_OK)
    {
        if (!is_fg)
        {
            if ((s_last_service_state == MEMORY_WATCH_SERVICE_STATE_UPLOADING ||
                 s_last_service_state == MEMORY_WATCH_SERVICE_STATE_THINKING) &&
                (snapshot.state == MEMORY_WATCH_SERVICE_STATE_DONE ||
                 snapshot.state == MEMORY_WATCH_SERVICE_STATE_ERROR ||
                 snapshot.state == MEMORY_WATCH_SERVICE_STATE_TIMEOUT ||
                 snapshot.state == MEMORY_WATCH_SERVICE_STATE_CANCELED))
            {
                /* 触发后台回复全局气泡 */
                watch_nc_notify_hermes_reply(NULL);
            }
        }
        s_last_service_state = snapshot.state;
    }

    if (!is_fg)
    {
        return;
    }
    memory_watch_controller_refresh();
}

void memory_watch_controller_open_via_notification(watch_nc_nav_target_t target,
                                                  const char *notification_id)
{
    /* 1. 记录进入前的页面，用于返回 */
    lv_obj_t *curr_scr = lv_screen_active();
    if (s_view == NULL || curr_scr != memory_watch_view_get_screen(s_view))
    {
        s_back_screen = curr_scr;
    }

    /* 2. 确保 view 已经创建并加载 */
    memory_watch_controller_ensure_view_created();
    if (s_view == NULL)
    {
        return;
    }

    /* 3. 设置页面渲染状态 */
    if (target == WATCH_NC_NAV_HERMES_VOICE)
    {
        s_render_page = MEMORY_WATCH_VIEW_PAGE_VOICE;
        (void)memory_watch_service_set_foreground(true);
    }
    else if (target == WATCH_NC_NAV_INBOX_LIST)
    {
        s_render_page = MEMORY_WATCH_VIEW_PAGE_INBOX;
#ifndef AGENT_PREVIEW_HOST
        (void)memory_watch_service_inbox_poll_now("open_inbox");
        memory_watch_controller_sync_inbox();
#endif
    }
    else if (target == WATCH_NC_NAV_INBOX_DETAIL)
    {
        s_render_page = MEMORY_WATCH_VIEW_PAGE_INBOX_DETAIL;
#ifdef AGENT_PREVIEW_HOST
        s_selected_inbox_index = 0;
        size_t count = sizeof(s_preview_inbox_items) / sizeof(s_preview_inbox_items[0]);
        for (size_t i = 0; i < count; ++i)
        {
            if (strcmp(s_preview_inbox_items[i].notification_id, notification_id) == 0)
            {
                s_selected_inbox_index = i;
                s_preview_inbox_items[i].read = true;
                break;
            }
        }
        memory_watch_controller_set_preview_detail(s_selected_inbox_index);
#else
        memory_watch_controller_sync_inbox();
        s_selected_inbox_index = 0;
        for (size_t i = 0; i < s_inbox_item_count; ++i)
        {
            if (strcmp(s_inbox_summaries[i].notification_id, notification_id) == 0)
            {
                s_selected_inbox_index = i;
                (void)memory_watch_service_inbox_mark_read(notification_id);
                s_inbox_summaries[i].read = true;
                s_inbox_view_items[i].read = true;
                if (s_inbox_unread_count > 0U)
                {
                    --s_inbox_unread_count;
                }
                break;
            }
        }
        const esp_err_t detail_err =
            memory_watch_service_get_inbox_item(notification_id, &s_inbox_detail);
        s_inbox_detail_valid = (detail_err == ESP_OK);
#endif
    }

    s_render_cache.valid = false;
    memory_watch_controller_refresh();

    /* 4. 加载页面动画 */
    if (lv_screen_active() != memory_watch_view_get_screen(s_view))
    {
        lv_screen_load_anim(memory_watch_view_get_screen(s_view),
                            LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
    }
}
