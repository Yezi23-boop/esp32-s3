/**
 * @file watch_notification_center.c
 * @brief 全局气泡通知中心实现（Stage 4）。
 *
 * 架构要点：
 * - 单例，LVGL task 内调用，不持有任何 RTOS 锁。
 * - surfaced ledger：记录哪些 notification_id 已经弹过气泡，
 *   避免重复打扰（重启后会重新弹，符合计划"合理提醒补偿"语义）。
 * - 气泡常驻 lv_layer_top()；右滑清除气泡但不标已读。
 * - inbox 通道与 conversation reply 通道共用一个气泡视图，但保留独立点击语义。
 */

#include "watch_notification_center.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "lvgl.h"
#include "services/memory_watch_service.h"
#include "ui_chinese_fonts.h"
#include "features/danger_detection/danger_detection_service.h"
#include "services/background_service_manager.h"

static const char *TAG = "watch_nc";

/* ── CO5300 灵动岛安全区尺寸（霓虹赛博撞色胶囊形态） ──
 * x=40, y=20, w=330, h=64, corner_radius=32 */
#define NC_BUBBLE_X      40
#define NC_BUBBLE_Y      20
#define NC_BUBBLE_W      330
#define NC_BUBBLE_H      88
#define NC_BUBBLE_RADIUS 24

/* ── surfaced ledger 最大条目数 ── */
#define NC_SURFACED_MAX 20U
/* ── active_ids 最大条目数（当前气泡承载） ── */
#define NC_ACTIVE_MAX 20U

/* ── 右滑清除阈值（px） ── */
#define NC_SWIPE_DISMISS_PX 48

/* ═══════════════════════════════════════════════════════════
 * 内部状态
 * ═══════════════════════════════════════════════════════════ */

/** @brief 气泡内部状态机。 */
typedef enum
{
    NC_BUBBLE_HIDDEN = 0,   /**< 气泡不可见。 */
    NC_BUBBLE_DEFERRED,     /**< 有待展示消息，但暂缓条件阻止显示。 */
    NC_BUBBLE_VISIBLE,      /**< 气泡正在显示。 */
    NC_BUBBLE_VISIBLE_UPDATED, /**< 气泡已显示且原位更新了文案。 */
} nc_bubble_state_t;

/** @brief 通道类型，决定点击目标。 */
typedef enum
{
    NC_CHANNEL_INBOX = 0,   /**< inbox notification 通道。 */
    NC_CHANNEL_REPLY,       /**< Hermes conversation reply 通道。 */
} nc_channel_t;

/* surfaced ledger：已弹过气泡的 notification_id 集合 */
static char s_surfaced[NC_SURFACED_MAX][64];
static size_t s_surfaced_count = 0;

/* active_ids：当前气泡承载的 inbox notification_id 集合 */
static char s_active_ids[NC_ACTIVE_MAX][64];
static size_t s_active_id_count = 0;

/* 气泡状态 */
static nc_bubble_state_t s_bubble_state = NC_BUBBLE_HIDDEN;
static nc_channel_t      s_bubble_channel = NC_CHANNEL_INBOX;

/* 上次 inbox generation */
static uint32_t s_last_inbox_generation = 0;

/* 待展示的 conversation reply（最新一条） */
static char s_pending_reply_request_id[128];
static bool s_has_pending_reply = false;

/* 收件箱列表页是否在前台 */
static bool s_inbox_list_active = false;

/* 回调 */
static watch_nc_config_t s_config = {0};

/* ── LVGL 对象（顶层气泡） ── */
static lv_obj_t *s_bubble_cont = NULL;
static lv_obj_t *s_bubble_title_label = NULL;
static lv_obj_t *s_bubble_preview_label = NULL;

/* 右滑手势追踪 */
static lv_point_t s_press_point = {0, 0};
static bool s_press_tracking = false;

/* ═══════════════════════════════════════════════════════════
 * surfaced ledger 操作
 * ═══════════════════════════════════════════════════════════ */

static bool nc_is_surfaced(const char *notification_id)
{
    for (size_t i = 0; i < s_surfaced_count; ++i)
    {
        if (strcmp(s_surfaced[i], notification_id) == 0)
        {
            return true;
        }
    }
    return false;
}

static void nc_mark_surfaced(const char *notification_id)
{
    if (nc_is_surfaced(notification_id))
    {
        return;
    }
    if (s_surfaced_count < NC_SURFACED_MAX)
    {
        strncpy(s_surfaced[s_surfaced_count], notification_id,
                sizeof(s_surfaced[0]) - 1U);
        s_surfaced[s_surfaced_count][sizeof(s_surfaced[0]) - 1U] = '\0';
        ++s_surfaced_count;
    }
    else
    {
        /* 循环覆盖最旧记录 */
        memmove(s_surfaced[0], s_surfaced[1],
                (NC_SURFACED_MAX - 1U) * sizeof(s_surfaced[0]));
        strncpy(s_surfaced[NC_SURFACED_MAX - 1U], notification_id,
                sizeof(s_surfaced[0]) - 1U);
        s_surfaced[NC_SURFACED_MAX - 1U][sizeof(s_surfaced[0]) - 1U] = '\0';
    }
}

/**
 * @brief 根据服务器最新快照清理 surfaced ledger 中已不存在且不在 active_ids 的记录。
 *
 * 防止 ledger 无限增长。
 */
static void nc_prune_surfaced(const memory_watch_inbox_summary_t *items,
                               size_t item_count)
{
    size_t write = 0;
    for (size_t ri = 0; ri < s_surfaced_count; ++ri)
    {
        /* 检查是否还在快照里 */
        bool in_snapshot = false;
        for (size_t si = 0; si < item_count; ++si)
        {
            if (strcmp(s_surfaced[ri], items[si].notification_id) == 0)
            {
                in_snapshot = true;
                break;
            }
        }
        /* 检查是否在 active_ids */
        bool in_active = false;
        for (size_t ai = 0; ai < s_active_id_count; ++ai)
        {
            if (strcmp(s_surfaced[ri], s_active_ids[ai]) == 0)
            {
                in_active = true;
                break;
            }
        }
        if (in_snapshot || in_active)
        {
            if (write != ri)
            {
                memcpy(s_surfaced[write], s_surfaced[ri],
                       sizeof(s_surfaced[0]));
            }
            ++write;
        }
    }
    s_surfaced_count = write;
}

/* ═══════════════════════════════════════════════════════════
 * active_ids 操作
 * ═══════════════════════════════════════════════════════════ */

static void nc_add_active_id(const char *notification_id)
{
    /* 去重 */
    for (size_t i = 0; i < s_active_id_count; ++i)
    {
        if (strcmp(s_active_ids[i], notification_id) == 0)
        {
            return;
        }
    }
    if (s_active_id_count < NC_ACTIVE_MAX)
    {
        strncpy(s_active_ids[s_active_id_count], notification_id,
                sizeof(s_active_ids[0]) - 1U);
        s_active_ids[s_active_id_count][sizeof(s_active_ids[0]) - 1U] = '\0';
        ++s_active_id_count;
    }
}

static void nc_clear_active_ids(void)
{
    s_active_id_count = 0;
}

/* ═══════════════════════════════════════════════════════════
 * 气泡 LVGL 对象管理
 * ═══════════════════════════════════════════════════════════ */

static void nc_bubble_show(const char *title_text,
                           const char *preview_text)
{
    if (s_bubble_cont == NULL)
    {
        return;
    }
    lv_label_set_text(s_bubble_title_label,
                      title_text != NULL ? title_text : "");
    lv_label_set_text(s_bubble_preview_label,
                      preview_text != NULL ? preview_text : "");
    lv_obj_clear_flag(s_bubble_cont, LV_OBJ_FLAG_HIDDEN);
}

static void nc_bubble_hide(void)
{
    if (s_bubble_cont == NULL)
    {
        return;
    }
    lv_obj_add_flag(s_bubble_cont, LV_OBJ_FLAG_HIDDEN);
    s_bubble_state = NC_BUBBLE_HIDDEN;
}

/* ═══════════════════════════════════════════════════════════
 * 手势回调
 * ═══════════════════════════════════════════════════════════ */

static void nc_on_bubble_pressed(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (indev == NULL)
    {
        return;
    }
    lv_indev_get_point(indev, &s_press_point);
    s_press_tracking = true;
}

static void nc_on_bubble_released(lv_event_t *e)
{
    (void)e;
    if (!s_press_tracking)
    {
        return;
    }
    s_press_tracking = false;

    lv_indev_t *indev = lv_indev_get_act();
    if (indev == NULL)
    {
        return;
    }
    lv_point_t release_point;
    lv_indev_get_point(indev, &release_point);

    const int32_t dx = release_point.x - s_press_point.x;
    const int32_t dy = release_point.y - s_press_point.y;
    const int32_t adx = dx < 0 ? -dx : dx;
    const int32_t ady = dy < 0 ? -dy : dy;

    if (adx >= NC_SWIPE_DISMISS_PX && adx > ady * 2)
    {
        /* 右滑清除：不标已读，保留 surfaced 状态 */
        ESP_LOGI(TAG, "bubble dismissed by swipe");
        nc_clear_active_ids();
        nc_bubble_hide();
        s_has_pending_reply = false; /* 同时清除待展示 reply 槽位 */
        if (s_config.dismiss_cb != NULL)
        {
            s_config.dismiss_cb(s_config.user_data);
        }
        return;
    }

    /* 点击处理 */
    if (s_bubble_channel == NC_CHANNEL_REPLY)
    {
        /* 跳转到 Hermes 语音/对话页 */
        nc_clear_active_ids();
        nc_bubble_hide();
        if (s_config.click_cb != NULL)
        {
            s_config.click_cb(WATCH_NC_NAV_HERMES_VOICE, NULL,
                              s_config.user_data);
        }
    }
    else if (s_active_id_count == 1U)
    {
        /* 单条 inbox 气泡：进入详情 */
        char id_copy[64];
        strncpy(id_copy, s_active_ids[0], sizeof(id_copy) - 1U);
        id_copy[sizeof(id_copy) - 1U] = '\0';
        nc_clear_active_ids();
        nc_bubble_hide();
        if (s_config.click_cb != NULL)
        {
            s_config.click_cb(WATCH_NC_NAV_INBOX_DETAIL, id_copy,
                              s_config.user_data);
        }
    }
    else
    {
        /* 多条合并气泡：进入收件箱列表 */
        nc_clear_active_ids();
        nc_bubble_hide();
        if (s_config.click_cb != NULL)
        {
            s_config.click_cb(WATCH_NC_NAV_INBOX_LIST, NULL,
                              s_config.user_data);
        }
    }
}

/* ═══════════════════════════════════════════════════════════
 * 初始化
 * ═══════════════════════════════════════════════════════════ */

void watch_nc_init(const watch_nc_config_t *config)
{
    if (config == NULL)
    {
        return;
    }
    s_config = *config;

    /* 在 lv_layer_top() 创建气泡容器（胶囊灵动岛） */
    lv_obj_t *layer = lv_layer_top();

    s_bubble_cont = lv_obj_create(layer);
    lv_obj_set_pos(s_bubble_cont, NC_BUBBLE_X, NC_BUBBLE_Y);
    lv_obj_set_size(s_bubble_cont, NC_BUBBLE_W, NC_BUBBLE_H);
    lv_obj_set_style_radius(s_bubble_cont, NC_BUBBLE_RADIUS, 0);
    /* 莫兰迪调和灰调：粉与绿融合后的烟灰豆沙绿 */
    lv_obj_set_style_bg_color(s_bubble_cont, lv_color_hex(0xC4D2C2), 0);
    lv_obj_set_style_bg_opa(s_bubble_cont, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_bubble_cont, 0, 0);
    lv_obj_set_style_pad_all(s_bubble_cont, 0, 0);
    lv_obj_set_style_clip_corner(s_bubble_cont, true, 0);
    /* 精致柔和投影 */
    lv_obj_set_style_shadow_color(s_bubble_cont, lv_color_hex(0x334433), 0);
    lv_obj_set_style_shadow_width(s_bubble_cont, 20, 0);
    lv_obj_set_style_shadow_opa(s_bubble_cont, LV_OPA_20, 0);
    lv_obj_set_style_shadow_ofs_y(s_bubble_cont, 6, 0);
    /* 气泡常驻覆盖页面，不推动布局 */
    lv_obj_add_flag(s_bubble_cont, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(s_bubble_cont, LV_OBJ_FLAG_HIDDEN); /* 初始隐藏 */

    /* 左侧复古玫瑰豆沙粉指示条 */
    lv_obj_t *pink_bar = lv_obj_create(s_bubble_cont);
    lv_obj_set_size(pink_bar, 6, 44);
    lv_obj_set_style_radius(pink_bar, 3, 0);
    lv_obj_set_style_bg_color(pink_bar, lv_color_hex(0xD85A7A), 0);
    lv_obj_set_style_border_width(pink_bar, 0, 0);
    lv_obj_align(pink_bar, LV_ALIGN_LEFT_MID, 16, 0);

    /* 标题：粗黑单行（强调信息标题） */
    s_bubble_title_label = lv_label_create(s_bubble_cont);
    lv_obj_set_width(s_bubble_title_label, NC_BUBBLE_W - 56);
    lv_obj_set_style_text_color(s_bubble_title_label, lv_color_hex(0x111111), 0);
    lv_label_set_long_mode(s_bubble_title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_bubble_title_label,
                               &lv_font_montserrat_lxgw_tghz_level1_3500_16_4, 0);
    lv_obj_align(s_bubble_title_label, LV_ALIGN_TOP_LEFT, 32, 12);

    /* preview：深灰炭黑内容简述（支持两行显示，超出截断省略） */
    s_bubble_preview_label = lv_label_create(s_bubble_cont);
    lv_obj_set_size(s_bubble_preview_label, NC_BUBBLE_W - 56, 44);
    lv_obj_set_style_text_color(s_bubble_preview_label, lv_color_hex(0x333333), 0);
    lv_label_set_long_mode(s_bubble_preview_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_bubble_preview_label,
                               &lv_font_montserrat_lxgw_tghz_level1_3500_16_4, 0);
    lv_obj_align(s_bubble_preview_label, LV_ALIGN_TOP_LEFT, 32, 34);

    /* 手势事件 */
    lv_obj_add_event_cb(s_bubble_cont, nc_on_bubble_pressed,
                        LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_bubble_cont, nc_on_bubble_released,
                        LV_EVENT_RELEASED, NULL);

    ESP_LOGI(TAG, "notification center initialized");
}

/* ═══════════════════════════════════════════════════════════
 * poll（LVGL timer 频率调用）
 * ═══════════════════════════════════════════════════════════ */

static bool nc_is_blocked(void)
{
    bool is_recording = false;
    memory_watch_service_snapshot_t snap = {0};
    if (memory_watch_service_get_snapshot(&snap) == ESP_OK)
    {
        is_recording = (snap.state == MEMORY_WATCH_SERVICE_STATE_RECORDING ||
                        snap.state == MEMORY_WATCH_SERVICE_STATE_ENCODING ||
                        snap.state == MEMORY_WATCH_SERVICE_STATE_UPLOADING);
    }

    bool safety_alert_active = false;
    const danger_detection_snapshot_t dd_snap = danger_detection_service_get_snapshot();
    const background_service_manager_snapshot_t bsm_snap = background_service_manager_get_snapshot();
    if (bsm_snap.danger_enabled_by_user &&
        dd_snap.state == DANGER_DETECTION_STATE_RUNNING &&
        dd_snap.risk_state == DANGER_DETECTION_RISK_ALERTING)
    {
        safety_alert_active = true;
    }

    return is_recording || safety_alert_active;
}

#define NC_SUMMARY_SCRATCH_MAX 20U
static memory_watch_inbox_summary_t s_nc_scratch[NC_SUMMARY_SCRATCH_MAX]
    __attribute__((section(".ext_ram.bss")));

void watch_nc_poll(bool is_recording, bool safety_alert_active)
{
    if (s_bubble_cont == NULL)
    {
        return;
    }

    const bool blocked = is_recording || safety_alert_active || nc_is_blocked();

    /* 1. 如果处于被阻挡状态，且当前有气泡显示，则必须隐藏并暂缓，确保告警屏无任何覆盖遮挡 */
    if (blocked)
    {
        if (s_bubble_state == NC_BUBBLE_VISIBLE || s_bubble_state == NC_BUBBLE_VISIBLE_UPDATED)
        {
            nc_bubble_hide();
            s_bubble_state = NC_BUBBLE_DEFERRED;
            ESP_LOGI(TAG, "active bubble hidden & deferred due to block");
        }
        return;
    }

    /* 2. 阻挡刚解除且处于 DEFERRED 状态，尝试恢复之前展示的气泡 */
    if (s_bubble_state == NC_BUBBLE_DEFERRED)
    {
        if (s_bubble_channel == NC_CHANNEL_REPLY)
        {
            nc_bubble_show("Hermes 回复已到达", "点击查看对话");
            s_bubble_state = NC_BUBBLE_VISIBLE;
            ESP_LOGI(TAG, "restored deferred reply bubble");
            return;
        }
        else if (s_active_id_count > 0U)
        {
            size_t scratch_count = 0;
            (void)memory_watch_service_copy_inbox_summaries(
                s_nc_scratch, NC_SUMMARY_SCRATCH_MAX, &scratch_count);

            char title_buf[72];
            char preview_buf[136];

            if (s_active_id_count == 1U)
            {
                const char *single_title   = "Hermes 提示";
                const char *single_preview = "";
                for (size_t i = 0; i < scratch_count; ++i)
                {
                    if (strcmp(s_nc_scratch[i].notification_id,
                               s_active_ids[0]) == 0)
                    {
                        single_title   = s_nc_scratch[i].title;
                        single_preview = s_nc_scratch[i].preview;
                        break;
                    }
                }
                strncpy(title_buf, single_title, sizeof(title_buf) - 1U);
                title_buf[sizeof(title_buf) - 1U] = '\0';
                strncpy(preview_buf, single_preview, sizeof(preview_buf) - 1U);
                preview_buf[sizeof(preview_buf) - 1U] = '\0';
            }
            else
            {
                snprintf(title_buf, sizeof(title_buf),
                         "Hermes 有 %zu 条新提示", s_active_id_count);
                preview_buf[0] = '\0';
            }

            nc_bubble_show(title_buf, preview_buf);
            s_bubble_state = NC_BUBBLE_VISIBLE;
            ESP_LOGI(TAG, "restored deferred inbox bubble count=%zu", s_active_id_count);
            return;
        }
        else
        {
            s_bubble_state = NC_BUBBLE_HIDDEN;
        }
    }

    /* 3. 消费语音对话回复挂起状态 */
    if (s_bubble_state == NC_BUBBLE_HIDDEN && s_has_pending_reply)
    {
        s_bubble_channel = NC_CHANNEL_REPLY;
        s_has_pending_reply = false;
        nc_bubble_show("Hermes 回复已到达", "点击查看对话");
        s_bubble_state = NC_BUBBLE_VISIBLE;
        ESP_LOGI(TAG, "deferred reply bubble shown");
        return;
    }

    /* 4. 正常 inbox 轮询检查 */
    memory_watch_inbox_meta_t meta = {0};
    if (memory_watch_service_get_inbox_meta(&meta) != ESP_OK)
    {
        return;
    }

    if (meta.generation == s_last_inbox_generation)
    {
        return; /* inbox 无变化 */
    }

    /* 拉取 summary，找出新到达且尚未 surfaced 的消息 */
    size_t count = 0;
    (void)memory_watch_service_copy_inbox_summaries(
        s_nc_scratch, NC_SUMMARY_SCRATCH_MAX, &count);

    /* 清理 surfaced ledger 中已过期的条目 */
    nc_prune_surfaced(s_nc_scratch, count);

    /* 收集新到达（unread + 未 surfaced）的 notification_id */
    size_t new_count = 0;
    for (size_t i = 0; i < count; ++i)
    {
        if (!s_nc_scratch[i].read &&
            !nc_is_surfaced(s_nc_scratch[i].notification_id))
        {
            ++new_count;
        }
    }

    if (new_count == 0U)
    {
        s_last_inbox_generation = meta.generation;
        return; /* 没有新消息 */
    }

    /* 收件箱列表页在前台时，不弹气泡，只更新列表 */
    if (s_inbox_list_active)
    {
        /* 把这批消息加入 surfaced，不打扰用户 */
        for (size_t i = 0; i < count; ++i)
        {
            if (!s_nc_scratch[i].read &&
                !nc_is_surfaced(s_nc_scratch[i].notification_id))
            {
                nc_mark_surfaced(s_nc_scratch[i].notification_id);
            }
        }
        s_last_inbox_generation = meta.generation;
        return;
    }

    /* 把新消息加入 active_ids 并标记 surfaced */
    for (size_t i = 0; i < count; ++i)
    {
        if (!s_nc_scratch[i].read &&
            !nc_is_surfaced(s_nc_scratch[i].notification_id))
        {
            nc_add_active_id(s_nc_scratch[i].notification_id);
            nc_mark_surfaced(s_nc_scratch[i].notification_id);
        }
    }

    /* 切换到 inbox 通道 */
    s_bubble_channel = NC_CHANNEL_INBOX;

    /* 构建气泡文案 */
    char title_buf[72];
    char preview_buf[136];

    if (s_active_id_count == 1U)
    {
        /* 单条：显示 title + preview */
        const char *single_title   = "Hermes 提示";
        const char *single_preview = "";
        for (size_t i = 0; i < count; ++i)
        {
            if (strcmp(s_nc_scratch[i].notification_id,
                       s_active_ids[0]) == 0)
            {
                single_title   = s_nc_scratch[i].title;
                single_preview = s_nc_scratch[i].preview;
                break;
            }
        }
        strncpy(title_buf, single_title, sizeof(title_buf) - 1U);
        title_buf[sizeof(title_buf) - 1U] = '\0';
        strncpy(preview_buf, single_preview, sizeof(preview_buf) - 1U);
        preview_buf[sizeof(preview_buf) - 1U] = '\0';
    }
    else
    {
        /* 多条合并 */
        snprintf(title_buf, sizeof(title_buf),
                 "Hermes 有 %zu 条新提示", s_active_id_count);
        preview_buf[0] = '\0';
    }

    nc_bubble_show(title_buf, preview_buf);
    s_bubble_state = NC_BUBBLE_VISIBLE;
    s_last_inbox_generation = meta.generation;

    ESP_LOGI(TAG, "bubble shown channel=inbox active_count=%zu",
             s_active_id_count);
}

void watch_nc_notify_hermes_reply(const char *request_id)
{
    if (request_id != NULL)
    {
        strncpy(s_pending_reply_request_id, request_id,
                sizeof(s_pending_reply_request_id) - 1U);
        s_pending_reply_request_id[sizeof(s_pending_reply_request_id) - 1U] =
            '\0';
    }

    const bool blocked = nc_is_blocked();

    /* 如果当前有 active 气泡或处于 blocked 状态，则放入延迟暂缓 */
    if (blocked ||
        s_bubble_state == NC_BUBBLE_VISIBLE ||
        s_bubble_state == NC_BUBBLE_VISIBLE_UPDATED)
    {
        s_has_pending_reply = true;
        ESP_LOGI(TAG, "reply deferred: bubble active or blocked=%d", blocked);
        return;
    }

    /* 没有阻挡，立即显示 reply 气泡 */
    s_bubble_channel = NC_CHANNEL_REPLY;
    s_has_pending_reply = false;
    nc_bubble_show("Hermes 回复已到达", "点击查看对话");
    s_bubble_state = NC_BUBBLE_VISIBLE;

    ESP_LOGI(TAG, "reply bubble shown request_id=%.32s",
             request_id != NULL ? request_id : "");
}

void watch_nc_set_inbox_list_active(bool active)
{
    s_inbox_list_active = active;
}
