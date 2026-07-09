#include "wifi_management_controller.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "network_manager.h"

LV_FONT_DECLARE(lv_font_montserrat_lxgw_lv_font_wifi_subset_16_16_4);
LV_FONT_DECLARE(lv_font_montserrat_lxgw_lv_font_wifi_subset_24_24_4);
LV_FONT_DECLARE(lv_font_montserrat_lxgw_tghz_level1_3500_16_4);

static const char *TAG = "wifi_mgmt_ui";
static const uint32_t kStatusRefreshMs = 300U;

static lv_ui *s_ui = NULL;
static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_status_panel = NULL;
static lv_obj_t *s_status_dot = NULL;
static lv_obj_t *s_status_title = NULL;
static lv_obj_t *s_status_detail = NULL;
static lv_obj_t *s_status_wifi_icon = NULL;
static lv_obj_t *s_retry_saved_btn = NULL;
static lv_obj_t *s_disconnect_btn = NULL;
static lv_obj_t *s_ble_provision_btn = NULL;
static lv_obj_t *s_softap_provision_btn = NULL;
static lv_timer_t *s_status_timer = NULL;

static void wifi_management_controller_refresh(void);
static void wifi_management_controller_ensure_screen_created(void);
static void wifi_management_controller_reset_screen_refs(void);
static bool wifi_management_controller_get_status(
    network_manager_status_t *status);
static lv_obj_t *wifi_management_controller_create_action_button(
    lv_obj_t *parent, const char *text, const char *left_icon, const char *right_icon, uint32_t text_color_hex, uint32_t left_icon_color_hex, lv_coord_t x, lv_coord_t y);
static void wifi_management_controller_set_locked(lv_obj_t *obj, bool locked);
static void wifi_management_controller_refresh_action_lock_state(
    const network_manager_status_t *status);
static bool wifi_management_controller_is_screen_alive(void);
static void wifi_management_controller_show_inline_message(
    const char *title, const char *detail);

/**
 * @brief 在 Wi-Fi 管理页对象被删除时清空缓存指针。
 *
 * 当前页面通过静态全局指针缓存 `screen / label / button`，方便二次打开时复用。
 * 但返回主界面时 `lv_screen_load_anim(..., true)` 会删除旧 screen；如果这里不在
 * `LV_EVENT_DELETE` 回调里同步清空缓存，下一次再次打开页面时就会把已经释放的
 * `lv_obj_t *` 当成活对象继续调用 `lv_obj_add_state()`，最终在 `lv_style_get_prop()`
 * 里因悬空对象触发 `LoadProhibited`。
 *
 * @param[in] e LVGL 事件对象，当前只用于表明删除事件已到达。
 */
static void wifi_management_screen_delete_event_cb(lv_event_t *e)
{
    (void)e;
    wifi_management_controller_reset_screen_refs();
}

/**
 * @brief 处理 Wi-Fi 管理页返回主界面的点击事件。
 * @param[in] e LVGL 事件对象，当前实现未直接读取。
 */
static void wifi_management_back_event_cb(lv_event_t *e)
{
    (void)e;

    if (s_ui == NULL || s_ui->screen_main == NULL)
    {
        return;
    }

    lv_screen_load_anim(s_ui->screen_main, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0,
                        true);
}

/**
 * @brief 处理“Use Saved Wi-Fi”按钮点击。
 *
 * 该按钮语义是再次使用最近一次成功连接的 Wi-Fi 凭据发起连接，
 * 适用于开机后因为环境变化或暂时失败而未连上的场景。
 *
 * @param[in] e LVGL 事件对象，当前实现未直接读取。
 */
static void wifi_management_retry_saved_event_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t ret = ESP_OK;

    ESP_LOGI(TAG, "request latest saved Wi-Fi");
    ret = network_manager_use_latest_wifi();
    if (ret == ESP_ERR_NOT_FOUND)
    {
        ESP_LOGW(TAG, "no saved Wi-Fi credentials, choose provisioning first");
        wifi_management_controller_show_inline_message(
            "No Saved Wi-Fi", "Use BLE Provision or AP Web Fallback first");
    }
    else if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "saved Wi-Fi retry failed: %s", esp_err_to_name(ret));
        wifi_management_controller_show_inline_message(
            "Retry Failed", "Choose BLE or web provisioning if it keeps failing");
    }
    wifi_management_controller_refresh();
}

/**
 * @brief 处理“Disconnect”按钮点击。
 *
 * 该按钮会主动断开当前 Wi-Fi，并暂停自动重连，直到用户再次手动发起
 * “Use Saved Wi-Fi”或“Reprovision”。
 *
 * @param[in] e LVGL 事件对象，当前实现未直接读取。
 */
static void wifi_management_disconnect_event_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "request Wi-Fi disconnect");
    (void)network_manager_disconnect();
    wifi_management_controller_refresh();
}

/**
 * @brief 处理“BLE Provision”按钮点击。
 *
 * 该按钮是小程序 BLE 配网的唯一显式入口。主界面蓝牙按钮只负责
 * BLE 总开关，不会自动启动官方 provisioning 广播。
 *
 * @param[in] e LVGL 事件对象，当前实现未直接读取。
 */
static void wifi_management_ble_provision_event_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t ret = ESP_OK;

    ESP_LOGI(TAG, "request BLE provisioning from Wi-Fi page");
    ret = network_manager_start_ble_provisioning();
    if (ret == ESP_ERR_INVALID_STATE)
    {
        wifi_management_controller_show_inline_message(
            "Bluetooth Off", "Turn on Bluetooth from the main switch first");
        return;
    }
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "start BLE provisioning failed: %s",
                 esp_err_to_name(ret));
        wifi_management_controller_show_inline_message(
            "BLE Failed", "Could not start BLE provisioning");
        return;
    }

    wifi_management_controller_refresh();
}

/**
 * @brief 处理“AP Web Fallback”按钮点击。
 *
 * 该按钮保留原有 AP 网页配网兜底能力，和 BLE 小程序配网互为备选入口。
 *
 * @param[in] e LVGL 事件对象，当前实现未直接读取。
 */
static void wifi_management_softap_provision_event_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t ret = ESP_OK;

    ESP_LOGI(TAG, "request SoftAP provisioning from Wi-Fi page");
    ret = network_manager_start_softap_provisioning();
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "start SoftAP provisioning failed: %s",
                 esp_err_to_name(ret));
        wifi_management_controller_show_inline_message(
            "AP Failed", "Could not start AP web fallback");
        return;
    }

    wifi_management_controller_refresh();
}

/**
 * @brief Wi-Fi 管理页状态刷新定时器。
 *
 * 页面不在前台时不刷新，避免后台页面继续消耗无意义的绘制和状态读取。
 *
 * @param[in] timer LVGL 定时器句柄，当前实现未直接读取。
 */
static void wifi_management_status_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (!wifi_management_controller_is_screen_alive() ||
        lv_screen_active() != s_screen)
    {
        return;
    }

    wifi_management_controller_refresh();
}

/**
 * @brief 读取当前网络状态快照。
 *
 * @param[out] status 输出状态结构。
 * @return true 表示读取成功；false 表示状态暂不可用。
 */
static bool wifi_management_controller_get_status(
    network_manager_status_t *status)
{
    if (status == NULL)
    {
        return false;
    }

    memset(status, 0, sizeof(*status));
    if (network_manager_get_status(status) != ESP_OK)
    {
        ESP_LOGW(TAG, "failed to read network manager status");
        return false;
    }

    return true;
}

/**
 * @brief 创建主操作区按钮。
 *
 * @param[in] parent 父对象。
 * @param[in] text 按钮文案。
 * @param[in] x 左上角 X 坐标。
 * @param[in] y 左上角 Y 坐标。
 * @return 创建好的按钮对象。
 */
static lv_obj_t *wifi_management_controller_create_action_button(
    lv_obj_t *parent, const char *text, const char *left_icon, const char *right_icon, uint32_t text_color_hex, uint32_t left_icon_color_hex, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, 330, 48); // 宽度 330px，配合 x=40 右边缘恰好 370（CO5300 安全区边界）
    lv_obj_set_style_radius(button, 24, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(button, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scrollbar_mode(button, LV_SCROLLBAR_MODE_OFF);
    
    bool is_disconnect = (text_color_hex == 0xFF453A);

    if (is_disconnect) {
        // Disconnect 按钮样式：粉红水晶玻璃质感
        lv_obj_set_style_bg_color(button, lv_color_hex(0xFFECEE), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(button, 190, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(button, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(button, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_opa(button, 180, LV_PART_MAIN | LV_STATE_DEFAULT);
        
        lv_obj_set_style_shadow_width(button, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_color(button, lv_color_hex(0xF4D0D2), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_opa(button, 80, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_ofs_y(button, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        
        lv_obj_set_style_bg_color(button, lv_color_hex(0xFFD2D6), LV_PART_MAIN | LV_STATE_PRESSED);
    } else {
        // 普通操作按钮样式：纯白毛玻璃质感
        lv_obj_set_style_bg_color(button, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(button, 180, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(button, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(button, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_opa(button, 180, LV_PART_MAIN | LV_STATE_DEFAULT);

        lv_obj_set_style_shadow_width(button, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_color(button, lv_color_hex(0xC4CFCA), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_opa(button, 60, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_ofs_y(button, 2, LV_PART_MAIN | LV_STATE_DEFAULT);

        lv_obj_set_style_bg_color(button, lv_color_hex(0xEAECEE), LV_PART_MAIN | LV_STATE_PRESSED);
    }

    // 左侧图标
    if (left_icon && left_icon[0] != '\0') {
        lv_obj_t *licon = lv_label_create(button);
        lv_label_set_text(licon, left_icon);
        lv_obj_set_style_text_color(licon, lv_color_hex(left_icon_color_hex), LV_PART_MAIN);
        lv_obj_set_style_text_font(licon, &lv_font_montserratMedium_16, LV_PART_MAIN);
        lv_obj_align(licon, LV_ALIGN_LEFT_MID, 20, 0);
        lv_obj_remove_flag(licon, LV_OBJ_FLAG_CLICKABLE); // 移除点击标志以穿透到父按钮
    }

    // 中间文本
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(text_color_hex), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_lxgw_lv_font_wifi_subset_16_16_4, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    if (left_icon && left_icon[0] != '\0') {
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 46, 0);
    } else {
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 20, 0);
    }
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE); // 移除点击标志以穿透到父按钮

    // 右侧图标/电源/箭头
    if (right_icon && right_icon[0] != '\0') {
        lv_obj_t *ricon = lv_label_create(button);
        lv_label_set_text(ricon, right_icon);
        lv_obj_set_style_text_color(ricon, lv_color_hex(is_disconnect ? 0xFF453A : 0x8D9099), LV_PART_MAIN);
        lv_obj_set_style_text_font(ricon, &lv_font_montserratMedium_16, LV_PART_MAIN);
        lv_obj_align(ricon, LV_ALIGN_RIGHT_MID, -20, 0);
        lv_obj_remove_flag(ricon, LV_OBJ_FLAG_CLICKABLE); // 移除点击标志以穿透到父按钮
    }

    return button;
}

static lv_obj_t *wifi_management_controller_create_bento_button(
    lv_obj_t *parent, const char *title, const char *icon_symbol, uint32_t icon_color_hex, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, 160, 120); // Bento 按钮：160px 宽，两个并排 x=40/210 右边缘恰好 370
    lv_obj_set_style_radius(button, 24, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(button, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scrollbar_mode(button, LV_SCROLLBAR_MODE_OFF);
    
    // 苹果浅色 Bento 卡片样式：纯白毛玻璃，悬浮阴影
    lv_obj_set_style_bg_color(button, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(button, 180, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(button, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(button, 180, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_set_style_shadow_width(button, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(button, lv_color_hex(0xC4CFCA), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(button, 70, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_ofs_y(button, 4, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_set_style_bg_color(button, lv_color_hex(0xEAECEE), LV_PART_MAIN | LV_STATE_PRESSED);

    // 标题 Label
    lv_obj_t *title_lbl = lv_label_create(button);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(0x1C1C1E), LV_PART_MAIN);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_lxgw_lv_font_wifi_subset_16_16_4, LV_PART_MAIN);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_remove_flag(title_lbl, LV_OBJ_FLAG_CLICKABLE); // 移除点击标志

    // 圆形图标容器 wrapper
    lv_obj_t *circle = lv_obj_create(button);
    lv_obj_set_size(circle, 56, 56);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_all(circle, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(circle, LV_SCROLLBAR_MODE_OFF);
    
    // 背景色 10% 不透明度
    lv_obj_set_style_bg_color(circle, lv_color_hex(icon_color_hex), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(circle, LV_OPA_10, LV_PART_MAIN);
    
    // 描边 30% 不透明度
    lv_obj_set_style_border_width(circle, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(circle, lv_color_hex(icon_color_hex), LV_PART_MAIN);
    lv_obj_set_style_border_opa(circle, LV_OPA_30, LV_PART_MAIN);
    
    lv_obj_align(circle, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_remove_flag(circle, LV_OBJ_FLAG_CLICKABLE); // 移除点击标志

    // 容器内图标 Label
    lv_obj_t *icon_lbl = lv_label_create(circle);
    lv_label_set_text(icon_lbl, icon_symbol);
    lv_obj_set_style_text_color(icon_lbl, lv_color_hex(icon_color_hex), LV_PART_MAIN);
    lv_obj_set_style_text_font(icon_lbl, &lv_font_montserratMedium_27, LV_PART_MAIN);
    lv_obj_center(icon_lbl);
    lv_obj_remove_flag(icon_lbl, LV_OBJ_FLAG_CLICKABLE); // 移除点击标志

    return button;
}

/**
 * @brief 创建 provisioning transport 选择按钮。
 *
 * @param[in] parent 父对象。
 * @param[in] text 按钮文案。
 * @param[in] x 左上角 X 坐标。
 * @param[in] y 左上角 Y 坐标。
 * @return 创建好的按钮对象。
 */
/**
 * @brief 判断当前缓存的 Wi-Fi 管理页 screen 是否仍然有效。
 *
 * @return true 表示 `s_screen` 仍是 LVGL 有效对象；false 表示 screen 为空或已被删除。
 */
static bool wifi_management_controller_is_screen_alive(void)
{
    return s_screen != NULL && lv_obj_is_valid(s_screen);
}

/**
 * @brief 清空当前页面相关的所有对象缓存。
 *
 * 这里不会删除对象本身，而是在 LVGL 已经进入删除流程后把 hand-written 控制层的静态
 * 指针全部置空，确保下一次打开页面时重新创建一套全新的 screen 和子控件。
 *
 * @return 无返回值。
 */
static void wifi_management_controller_reset_screen_refs(void)
{
    s_screen = NULL;
    s_status_title = NULL;
    s_status_detail = NULL;
    s_retry_saved_btn = NULL;
    s_disconnect_btn = NULL;
    s_ble_provision_btn = NULL;
    s_softap_provision_btn = NULL;
}

/**
 * @brief 按是否锁定刷新单个按钮的交互态。
 *
 * 当前 Wi-Fi 管理页使用该辅助函数统一收口“配网进行中”的禁用逻辑，避免多个按钮
 * 各自散落地直接操作 `LV_STATE_DISABLED`。
 *
 * @param[in] obj 目标按钮对象，可为 `NULL`。
 * @param[in] locked true 表示禁用交互；false 表示恢复可点击。
 */
static void wifi_management_controller_set_locked(lv_obj_t *obj, bool locked)
{
    if (obj == NULL || !lv_obj_is_valid(obj))
    {
        return;
    }

    if (locked)
    {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
    }
    else
    {
        lv_obj_remove_state(obj, LV_STATE_DISABLED);
    }
}

/**
 * @brief 根据当前网络快照刷新页面操作锁定态。
 *
 * BLE / SoftAP provisioning 期间，重复点击两个配网入口会把当前会话直接
 * stop -> start 掉，导致用户自己把小程序连接或 AP 门户抖断。因此这里
 * 在任一 provisioning 会话进行中锁住两个入口。
 *
 * 其余操作暂保持原样，避免扩大本轮改动范围。
 *
 * @param[in] status 当前 `network_manager` 快照。
 */
static void wifi_management_controller_refresh_action_lock_state(
    const network_manager_status_t *status)
{
    if (status == NULL)
    {
        return;
    }

    const bool provisioning_locked =
        status->ble_active ||
        (status->state == NETWORK_MANAGER_STATE_PROVISIONING_BLE) ||
        (status->state == NETWORK_MANAGER_STATE_PROVISIONING_SOFTAP);

    wifi_management_controller_set_locked(s_ble_provision_btn,
                                          provisioning_locked);
    wifi_management_controller_set_locked(s_softap_provision_btn,
                                          provisioning_locked);
}

/**
 * @brief 在状态区显示一次内联提示。
 *
 * Wi-Fi 管理页没有额外 toast 层，这里复用标题和详情区域表达点击失败原因，
 * 避免用户点“BLE 配网”后无反馈。
 *
 * @param[in] title 提示标题。
 * @param[in] detail 提示详情。
 */
static void wifi_management_controller_show_inline_message(
    const char *title, const char *detail)
{
    if (s_status_title == NULL || s_status_detail == NULL ||
        !lv_obj_is_valid(s_status_title) || !lv_obj_is_valid(s_status_detail))
    {
        return;
    }

    lv_label_set_text(s_status_title, title != NULL ? title : "错误");
    lv_label_set_text(s_status_detail, detail != NULL ? detail : "");
}

/**
 * @brief 刷新 Wi-Fi 管理页状态文本与设置态。
 *
 * 页面顶部状态区只表达用户可理解的联网语义，不直接暴露底层 driver
 * 细节。这里统一从 `network_manager` 读取状态，避免 UI 直接依赖旧的
 * `network_service` 兼容层。
 */
static void wifi_management_controller_refresh(void)
{
    network_manager_status_t status = {0};
    char detail[96] = {0};

    if (!wifi_management_controller_is_screen_alive() ||
        s_status_title == NULL || s_status_detail == NULL ||
        !lv_obj_is_valid(s_status_title) || !lv_obj_is_valid(s_status_detail))
    {
        return;
    }

    if (!wifi_management_controller_get_status(&status))
    {
        lv_label_set_text(s_status_title, "错误");
        lv_label_set_text(s_status_detail, "当前通道错误");
        return;
    }

    wifi_management_controller_refresh_action_lock_state(&status);

    uint32_t status_color_hex = 0x8E8E93;

    if (status.ble_active && status.wifi_connected)
    {
        lv_label_set_text(s_status_title, "已连接");
        status_color_hex = 0x32D74B; // Apple Green
        if (status.ip[0] != '\0')
        {
            snprintf(detail, sizeof(detail), "IP: %s (蓝牙活跃)", status.ip);
        }
        else
        {
            snprintf(detail, sizeof(detail), "蓝牙活跃");
        }
    }
    else if (status.ble_active)
    {
        lv_label_set_text(s_status_title, "配网中");
        status_color_hex = 0x0A84FF; // Apple Blue
        snprintf(detail, sizeof(detail), "请打开微信小程序");
    }
    else if (status.wifi_connected)
    {
        lv_label_set_text(s_status_title, "已连接");
        status_color_hex = 0x32D74B; // Apple Green
        if (status.ip[0] != '\0')
        {
            snprintf(detail, sizeof(detail), "IP: %s", status.ip);
        }
        else
        {
            snprintf(detail, sizeof(detail), "Wi-Fi 已连接");
        }
    }
    else if (status.state == NETWORK_MANAGER_STATE_CONNECTING_LATEST)
    {
        lv_label_set_text(s_status_title, "连接中");
        status_color_hex = 0x0A84FF; // Apple Blue
        snprintf(detail, sizeof(detail), "正在尝试保存的网络");
    }
    else if (status.state == NETWORK_MANAGER_STATE_PROVISIONING_BLE ||
             status.state == NETWORK_MANAGER_STATE_PROVISIONING_SOFTAP)
    {
        lv_label_set_text(s_status_title, "配网中");
        status_color_hex = 0x0A84FF; // Apple Blue
        snprintf(detail, sizeof(detail), "当前通道: %s", 
                 status.state == NETWORK_MANAGER_STATE_PROVISIONING_BLE ? "蓝牙" : "AP");
    }
    else if (status.state == NETWORK_MANAGER_STATE_DISCONNECTED_BY_USER)
    {
        lv_label_set_text(s_status_title, "未连接");
        status_color_hex = 0x8E8E93; // Apple Gray
        snprintf(detail, sizeof(detail), "自动重连已暂停");
    }
    else if (status.state == NETWORK_MANAGER_STATE_ERROR)
    {
        lv_label_set_text(s_status_title, "错误");
        status_color_hex = 0xFF453A; // Apple Red
        snprintf(detail, sizeof(detail), "请重新配网");
    }
    else
    {
        lv_label_set_text(s_status_title, "未连接");
        status_color_hex = 0x8E8E93; // Apple Gray
        snprintf(detail, sizeof(detail), "请在下方添加网络");
    }

    lv_obj_set_style_text_color(s_status_title, lv_color_hex(status_color_hex), LV_PART_MAIN);
    if (s_status_dot != NULL) {
        lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(status_color_hex), LV_PART_MAIN);
        lv_obj_set_style_shadow_color(s_status_dot, lv_color_hex(status_color_hex), LV_PART_MAIN);
    }
    if (s_status_wifi_icon != NULL) {
        lv_obj_set_style_text_color(s_status_wifi_icon, lv_color_hex(status_color_hex), LV_PART_MAIN);
    }

    lv_label_set_text(s_status_detail, detail);

    // 始终显示两个操作长条按钮以完美对齐设计图
    if (s_retry_saved_btn != NULL && s_disconnect_btn != NULL)
    {
        lv_obj_clear_flag(s_retry_saved_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_disconnect_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief 按需创建 Wi-Fi 管理页。
 *
 * 当前继续沿用 hand-written LVGL 页面，不要求用户在 GUI Guider 里新增
 * 页面结构，只在现有工程内生成一张全新的管理页。
 */
static void pulse_anim_cb(void *var, int32_t v)
{
    lv_obj_t *dot = (lv_obj_t *)var;
    if (!lv_obj_is_valid(dot)) return;
    
    lv_obj_set_style_shadow_width(dot, v, LV_PART_MAIN);
    lv_opa_t opa = 255 - (v * 255 / 24);
    lv_obj_set_style_shadow_opa(dot, opa, LV_PART_MAIN);
}

static void wifi_management_controller_ensure_screen_created(void)
{
    if (wifi_management_controller_is_screen_alive())
    {
        return;
    }

    wifi_management_controller_reset_screen_refs();

    s_screen = lv_obj_create(NULL);
    // 淡绿白毛玻璃环境背景渐变
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0xF2F7F5), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_color(s_screen, lv_color_hex(0xDCE4E0), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(s_screen, LV_GRAD_DIR_VER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(s_screen, wifi_management_screen_delete_event_cb, LV_EVENT_DELETE, NULL);

    // 标题 "Wi-Fi"
    lv_obj_t *title = lv_label_create(s_screen);
    lv_label_set_text(title, "Wi-Fi");
    lv_obj_set_style_text_color(title, lv_color_hex(0x1C1C1E), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_lxgw_lv_font_wifi_subset_24_24_4, LV_PART_MAIN);
    lv_obj_set_pos(title, 40, 28);

    // 圆形返回按钮
    lv_obj_t *back_btn = lv_btn_create(s_screen);
    lv_obj_set_size(back_btn, 44, 44);
    lv_obj_set_pos(back_btn, 324, 20);
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xE4E7EB), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_90, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(back_btn, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(back_btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(back_btn, LV_OPA_70, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(back_btn, wifi_management_back_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_icon = lv_label_create(back_btn);
    lv_label_set_text(back_icon, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(back_icon, lv_color_hex(0x1C1C1E), LV_PART_MAIN);
    lv_obj_center(back_icon);
    
    // Status Panel (高阶毛玻璃悬浮卡片)
    s_status_panel = lv_obj_create(s_screen);
    lv_obj_set_size(s_status_panel, 330, 94);
    lv_obj_set_pos(s_status_panel, 40, 84);
    lv_obj_set_style_radius(s_status_panel, 24, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_status_panel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_status_panel, 180, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_status_panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_status_panel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_status_panel, 200, LV_PART_MAIN);

    lv_obj_set_style_shadow_width(s_status_panel, 28, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(s_status_panel, lv_color_hex(0xC4CFCA), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s_status_panel, 90, LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_y(s_status_panel, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_status_panel, 0, LV_PART_MAIN);

    // "状态" 灰色小字
    lv_obj_t *status_lbl = lv_label_create(s_status_panel);
    lv_label_set_text(status_lbl, "状态");
    lv_obj_set_style_text_color(status_lbl, lv_color_hex(0x8D9099), LV_PART_MAIN);
    lv_obj_set_style_text_font(status_lbl, &lv_font_montserrat_lxgw_tghz_level1_3500_16_4, LV_PART_MAIN);
    lv_obj_align(status_lbl, LV_ALIGN_TOP_LEFT, 20, 16);

    // 状态呼吸灯
    s_status_dot = lv_obj_create(s_status_panel);
    lv_obj_remove_style_all(s_status_dot); // 彻底清除默认样式，防止画出多余的指示十字线
    lv_obj_set_size(s_status_dot, 10, 10);
    lv_obj_set_style_radius(s_status_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_status_dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(0x32D74B), LV_PART_MAIN);
    lv_obj_set_style_shadow_color(s_status_dot, lv_color_hex(0x32D74B), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_status_dot, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s_status_dot, 0, LV_PART_MAIN);
    lv_obj_set_pos(s_status_dot, 20, 54);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_status_dot);
    lv_anim_set_values(&a, 0, 24);
    lv_anim_set_time(&a, 1500);
    lv_anim_set_playback_time(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, pulse_anim_cb);
    lv_anim_start(&a);

    // 状态文本
    s_status_title = lv_label_create(s_status_panel);
    lv_label_set_text(s_status_title, "未连接");
    lv_obj_set_style_text_color(s_status_title, lv_color_hex(0x1C1C1E), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_status_title, &lv_font_montserrat_lxgw_lv_font_wifi_subset_24_24_4, LV_PART_MAIN);
    lv_obj_set_pos(s_status_title, 38, 46);

    // 状态详情 (位于底部)
    s_status_detail = lv_label_create(s_status_panel);
    lv_obj_set_width(s_status_detail, 200);
    lv_label_set_long_mode(s_status_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_status_detail, lv_color_hex(0x8D9099), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_status_detail, &lv_font_montserrat_lxgw_lv_font_wifi_subset_16_16_4, LV_PART_MAIN);
    lv_obj_set_pos(s_status_detail, 20, 72);

    // 右侧大 Wi-Fi 信号图标 (Montserrat 46)
    s_status_wifi_icon = lv_label_create(s_status_panel);
    lv_label_set_text(s_status_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(s_status_wifi_icon, &lv_font_montserratMedium_46, LV_PART_MAIN);
    lv_obj_align(s_status_wifi_icon, LV_ALIGN_RIGHT_MID, -24, 0);

    // Bento 按钮创建
    s_ble_provision_btn = wifi_management_controller_create_bento_button(
        s_screen, "蓝牙配网", LV_SYMBOL_BLUETOOTH, 0x0A84FF, 40, 192);
    lv_obj_add_event_cb(s_ble_provision_btn, wifi_management_ble_provision_event_cb, LV_EVENT_CLICKED, NULL);

    s_softap_provision_btn = wifi_management_controller_create_bento_button(
        s_screen, "网页配网", LV_SYMBOL_WIFI, 0xBF5AF2, 210, 192);
    lv_obj_add_event_cb(s_softap_provision_btn, wifi_management_softap_provision_event_cb, LV_EVENT_CLICKED, NULL);

    // 长条操作按钮创建 (重试已保存网络)
    s_retry_saved_btn = wifi_management_controller_create_action_button(
        s_screen, "重试已保存网络", LV_SYMBOL_WIFI, LV_SYMBOL_RIGHT, 0x1C1C1E, 0x32D74B, 40, 326);
    lv_obj_add_event_cb(s_retry_saved_btn, wifi_management_retry_saved_event_cb, LV_EVENT_CLICKED, NULL);

    // 断开连接按钮创建
    s_disconnect_btn = wifi_management_controller_create_action_button(
        s_screen, "断开连接", NULL, LV_SYMBOL_POWER, 0xFF453A, 0xFF453A, 40, 394);
    lv_obj_add_event_cb(s_disconnect_btn, wifi_management_disconnect_event_cb, LV_EVENT_CLICKED, NULL);

    if (s_status_timer == NULL)
    {
        s_status_timer =
            lv_timer_create(wifi_management_status_timer_cb, kStatusRefreshMs,
                            NULL);
    }

    wifi_management_controller_refresh();
    ESP_LOGI(TAG, "Wi-Fi management screen created");
}

/**
 * @brief 初始化 Wi-Fi 管理页控制器。
 * @param[in] ui 当前 UI 句柄。
 */
void wifi_management_controller_init(lv_ui *ui)
{
    s_ui = ui;
}

/**
 * @brief 打开 Wi-Fi 管理页。
 *
 * 首次打开时会先创建页面，之后复用同一张页面并在进入前刷新状态。
 */
void wifi_management_controller_open(void)
{
    wifi_management_controller_ensure_screen_created();
    if (s_screen == NULL)
    {
        return;
    }

    wifi_management_controller_refresh();
    lv_screen_load_anim(s_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}
