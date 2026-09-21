#include "ui/custom/ota_maintenance_view.h"

#include "esp_log.h"
#include "gui_guider.h"
#include "lvgl.h"
#include "ota_cloud_download_icon.h"
#include "services/ota/ota_service.h"
#include "ui_chinese_fonts.h"

static const char *TAG = "ota_view";
static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_progress_bar = NULL;
static lv_obj_t *s_progress_label = NULL;
static lv_obj_t *s_primary_label = NULL;
static lv_obj_t *s_entry = NULL;
static lv_obj_t *s_previous_screen = NULL;

static const char *ota_view_state_text(ota_service_state_t state)
{
    switch (state)
    {
    case OTA_SERVICE_STATE_IDLE:
        return "等待操作";
    case OTA_SERVICE_STATE_PREPARING:
        return "准备中";
    case OTA_SERVICE_STATE_QUIESCING:
        return "暂停后台服务";
    case OTA_SERVICE_STATE_QUIESCED:
        return "后台服务已暂停";
    case OTA_SERVICE_STATE_READY:
        return "发现更新";
    case OTA_SERVICE_STATE_NO_UPDATE:
        return "没有可用更新";
    case OTA_SERVICE_STATE_DOWNLOADING:
        return "下载中";
    case OTA_SERVICE_STATE_STAGED:
        return "等待重启安装";
    case OTA_SERVICE_STATE_VERIFYING:
        return "校验中";
    case OTA_SERVICE_STATE_RESTARTING:
        return "正在重启";
    case OTA_SERVICE_STATE_FAILED:
        return "更新失败";
    case OTA_SERVICE_STATE_PENDING_VERIFY:
        return "等待系统确认";
    case OTA_SERVICE_STATE_VALID:
        return "更新成功";
    case OTA_SERVICE_STATE_ROLLED_BACK:
        return "已回滚";
    default:
        return "未知状态";
    }
}

static void ota_view_primary_event(lv_event_t *event)
{
    (void)event;
    ota_service_snapshot_t snapshot = {0};
    if (ota_service_get_snapshot(&snapshot) != ESP_OK)
    {
        return;
    }

    if (snapshot.state == OTA_SERVICE_STATE_IDLE ||
        snapshot.state == OTA_SERVICE_STATE_FAILED ||
        snapshot.state == OTA_SERVICE_STATE_NO_UPDATE)
    {
        (void)ota_service_request_check();
    }
    else if (snapshot.state == OTA_SERVICE_STATE_READY)
    {
        (void)ota_service_request_download();
    }
    else if (snapshot.state == OTA_SERVICE_STATE_STAGED)
    {
        (void)ota_service_request_activate();
    }
}

static void ota_view_cancel_event(lv_event_t *event)
{
    (void)event;
    (void)ota_service_request_cancel();
}

static void ota_view_back_event(lv_event_t *event)
{
    (void)event;

    lv_obj_t *target = s_previous_screen;
    if (target == NULL || target == s_screen || !lv_obj_is_valid(target))
    {
        target = guider_ui.screen_main;
    }
    if (target == NULL || target == s_screen)
    {
        return;
    }

    s_previous_screen = NULL;
    lv_screen_load_anim(target, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
    ESP_LOGI(TAG, "maintenance page closed");
}

static void ota_view_entry_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    static lv_point_t start_point;

    switch (code)
    {
    case LV_EVENT_PRESSED:
        lv_indev_get_point(lv_indev_get_act(), &start_point);
        break;
    case LV_EVENT_CLICKED:
    {
        lv_point_t current_point;
        lv_indev_get_point(lv_indev_get_act(), &current_point);
        if (LV_ABS(current_point.x - start_point.x) <= 12 &&
            LV_ABS(current_point.y - start_point.y) <= 12)
        {
            (void)ota_maintenance_view_open();
        }
        break;
    }
    default:
        break;
    }
}

void ota_maintenance_view_bind_entry(void)
{
    if (s_entry != NULL || guider_ui.screen_main_Function_main == NULL)
    {
        return;
    }

    /* 复用功能页现有纵向卡片布局，按钮会排在用户卡片之后。 */
    s_entry = lv_obj_create(guider_ui.screen_main_Function_main);
    if (s_entry == NULL)
    {
        return;
    }
    lv_obj_set_size(s_entry, 256, 90);
    lv_obj_set_scrollbar_mode(s_entry, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(s_entry, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_entry, 18, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_entry, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_entry, lv_color_hex(0x947CB6), LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_entry, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_entry, 6, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(s_entry, lv_color_hex(0x7C73AF), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s_entry, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_spread(s_entry, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_offset_x(s_entry, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_offset_y(s_entry, 8, LV_PART_MAIN);
    lv_obj_add_flag(s_entry, LV_OBJ_FLAG_CLICKABLE);
    /* 让功能页已有的横滑处理继续收到这张动态卡片的触摸事件。 */
    lv_obj_add_flag(s_entry, LV_OBJ_FLAG_EVENT_BUBBLE);
    /* 与功能页其它卡片保持一致：滑动不触发打开，短按才进入维护页。 */
    lv_obj_add_event_cb(s_entry, ota_view_entry_event, LV_EVENT_ALL, NULL);

    lv_obj_t *icon = lv_image_create(s_entry);
    lv_image_set_src(icon, &ota_cloud_download_icon);
    lv_obj_set_pos(icon, 13, 3);
    lv_obj_update_layout(guider_ui.screen_main_Function_main);
    lv_obj_send_event(guider_ui.screen_main_Function_main, LV_EVENT_SCROLL, NULL);
    ESP_LOGI(TAG, "maintenance entry created on function page");
}

esp_err_t ota_maintenance_view_init(void)
{
    if (s_screen != NULL)
    {
        return ESP_OK;
    }

    s_screen = lv_obj_create(NULL);
    if (s_screen == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_size(s_screen, 410, 502);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x10131A),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_obj_set_pos(title, 40, 42);
    lv_obj_set_size(title, 330, 40);
    lv_label_set_text(title, "系统维护");
    lv_obj_set_style_text_font(title,
                               &lv_font_montserrat_lxgw_common_5500_22_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *back = lv_button_create(s_screen);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 72, 56);
    lv_obj_set_pos(back, 40, 24);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_event_cb(back, ota_view_back_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_label = lv_label_create(back);
    lv_label_set_text(back_label, "<");
    lv_obj_set_style_text_font(back_label, &lv_font_montserratMedium_27,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(back_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(back_label);

    s_status_label = lv_label_create(s_screen);
    lv_obj_set_pos(s_status_label, 40, 108);
    lv_obj_set_size(s_status_label, 330, 44);
    lv_label_set_text(s_status_label, "等待操作");
    lv_obj_set_style_text_font(
        s_status_label, &lv_font_montserrat_lxgw_common_5500_22_4,
        LV_PART_MAIN);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xD7DCE8),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);

    s_progress_bar = lv_bar_create(s_screen);
    lv_obj_set_pos(s_progress_bar, 40, 220);
    lv_obj_set_size(s_progress_bar, 330, 18);
    lv_bar_set_range(s_progress_bar, 0, 100);
    lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);

    s_progress_label = lv_label_create(s_screen);
    lv_obj_set_pos(s_progress_label, 40, 250);
    lv_obj_set_size(s_progress_label, 330, 32);
    lv_label_set_text(s_progress_label, "0%");
    lv_obj_set_style_text_color(s_progress_label, lv_color_white(),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(s_progress_label, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);

    lv_obj_t *prepare = lv_button_create(s_screen);
    lv_obj_set_pos(prepare, 40, 320);
    lv_obj_set_size(prepare, 160, 54);
    lv_obj_add_event_cb(prepare, ota_view_primary_event, LV_EVENT_CLICKED,
                        NULL);
    s_primary_label = lv_label_create(prepare);
    lv_obj_set_style_text_font(
        s_primary_label, &lv_font_montserrat_lxgw_common_5500_22_4,
        LV_PART_MAIN);
    lv_label_set_text(s_primary_label, "检查更新");
    lv_obj_center(s_primary_label);

    lv_obj_t *cancel = lv_button_create(s_screen);
    lv_obj_set_pos(cancel, 210, 320);
    lv_obj_set_size(cancel, 160, 54);
    lv_obj_add_event_cb(cancel, ota_view_cancel_event, LV_EVENT_CLICKED,
                        NULL);
    lv_obj_t *cancel_label = lv_label_create(cancel);
    lv_obj_set_style_text_font(
        cancel_label, &lv_font_montserrat_lxgw_common_5500_22_4,
        LV_PART_MAIN);
    lv_label_set_text(cancel_label, "取消");
    lv_obj_center(cancel_label);

    return ESP_OK;
}

esp_err_t ota_maintenance_view_open(void)
{
    if (s_screen == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    lv_obj_t *active_screen = lv_screen_active();
    if (active_screen != NULL && active_screen != s_screen)
    {
        s_previous_screen = active_screen;
    }
    lv_screen_load_anim(s_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
    ESP_LOGI(TAG, "maintenance page opened");
    return ESP_OK;
}

void ota_maintenance_view_poll(void)
{
    if (s_screen == NULL || s_status_label == NULL ||
        s_progress_bar == NULL || s_progress_label == NULL ||
        s_primary_label == NULL)
    {
        return;
    }

    ota_service_snapshot_t snapshot = {0};
    if (ota_service_get_snapshot(&snapshot) != ESP_OK)
    {
        return;
    }

    if (snapshot.update_available && snapshot.state == OTA_SERVICE_STATE_READY)
    {
        lv_label_set_text_fmt(s_status_label, "目标 %s",
                              snapshot.target_version);
    }
    else if (snapshot.state == OTA_SERVICE_STATE_NO_UPDATE)
    {
        lv_label_set_text(s_status_label, "没有可用更新");
    }
    else
    {
        lv_label_set_text(s_status_label, ota_view_state_text(snapshot.state));
    }
    lv_bar_set_value(s_progress_bar, snapshot.progress_percent, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_progress_label, "%u%%",
                          (unsigned int)snapshot.progress_percent);

    if (snapshot.state == OTA_SERVICE_STATE_READY && snapshot.update_available)
    {
        lv_label_set_text(s_primary_label, "下载更新");
    }
    else if (snapshot.state == OTA_SERVICE_STATE_STAGED)
    {
        lv_label_set_text(s_primary_label, "重启安装");
    }
    else
    {
        lv_label_set_text(s_primary_label, "检查更新");
    }
}
