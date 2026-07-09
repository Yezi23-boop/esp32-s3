#include "main_dropdown_controller.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network_manager.h"
#include "wifi_management_controller.h"
#include "watch_notification_center.h"
#include "memory_watch_controller.h"
#include "services/memory_watch_service.h"
#include "features/danger_detection/danger_detection_service.h"
#include "services/background_https_gate.h"
#include "services/background_service_manager.h"
#include "services/foreground_runtime_gate.h"

static const char *TAG = "main_dropdown";
static const uint32_t kStatusSyncPeriodMs = 250U;
static const uint32_t kToastDurationMs = 1800U;
static const uint32_t kBleQuietRetryMs = 800U;

static lv_ui *s_ui = NULL;
static lv_timer_t *s_status_sync_timer = NULL;
static lv_timer_t *s_toast_timer = NULL;
static lv_obj_t *s_toast_label = NULL;
static bool s_last_wifi_checked = false;
static bool s_last_wifi_checked_valid = false;
static bool s_last_bluetooth_checked = false;
static bool s_last_bluetooth_checked_valid = false;

static lv_obj_t *main_dropdown_controller_get_wifi_button(void);
static lv_obj_t *main_dropdown_controller_get_bluetooth_button(void);
static bool main_dropdown_controller_is_main_screen_active(void);
static bool main_dropdown_controller_get_network_status(
    network_manager_status_t *status);
static void main_dropdown_controller_sync_wifi_button(void);
static void main_dropdown_controller_sync_bluetooth_button(void);
static void main_dropdown_controller_status_sync_timer_cb(lv_timer_t *timer);
static void main_dropdown_controller_toast_timer_cb(lv_timer_t *timer);
static void main_dropdown_controller_hide_toast(void);
static void main_dropdown_controller_show_toast(const char *text);

/**
 * @brief 获取主界面 Wi-Fi 图标按钮对象。
 * @return 返回当前 UI 中的 Wi-Fi 按钮；若 UI 尚未绑定则返回 `NULL`。
 */
static lv_obj_t *main_dropdown_controller_get_wifi_button(void)
{
    if (s_ui == NULL)
    {
        return NULL;
    }

    return s_ui->screen_main_Wifi;
}

/**
 * @brief 获取主界面蓝牙图标按钮对象。
 * @return 返回当前 UI 中的蓝牙按钮；若 UI 尚未绑定则返回 `NULL`。
 */
static lv_obj_t *main_dropdown_controller_get_bluetooth_button(void)
{
    if (s_ui == NULL)
    {
        return NULL;
    }

    return s_ui->screen_main_Bluetooth;
}

/**
 * @brief 判断当前是否仍停留在主界面。
 * @return true 表示当前活动屏幕就是主界面，可继续刷新图标状态。
 */
static bool main_dropdown_controller_is_main_screen_active(void)
{
    return s_ui != NULL && s_ui->screen_main != NULL &&
           lv_screen_active() == s_ui->screen_main;
}

/**
 * @brief 读取当前统一网络状态快照。
 *
 * 这里统一通过 `network_manager` 取快照，避免 Wi-Fi 图标和蓝牙图标各自
 * 读不同接口，导致同一帧内状态不一致。
 *
 * @param[out] status 输出状态快照。
 * @return true 表示读取成功；false 表示快照不可用。
 */
static bool main_dropdown_controller_get_network_status(
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
 * @brief 按真实 Wi-Fi 连接状态同步主界面 Wi-Fi 图标。
 *
 * 图标只表达“当前是否已经连上 Wi-Fi”，不表达是否存在保存凭据，
 * 也不表达是否处在配网流程中。
 */
static void main_dropdown_controller_sync_wifi_button(void)
{
    lv_obj_t *button = main_dropdown_controller_get_wifi_button();
    network_manager_status_t status = {0};
    bool connected = false;

    if (button == NULL)
    {
        return;
    }

    if (main_dropdown_controller_get_network_status(&status))
    {
        connected = status.wifi_connected;
    }

    if (!s_last_wifi_checked_valid || s_last_wifi_checked != connected)
    {
        ESP_LOGI(TAG, "sync WiFi button: checked=%d connected=%d",
                 connected ? 1 : 0, connected ? 1 : 0);
        s_last_wifi_checked = connected;
        s_last_wifi_checked_valid = true;
    }

    if (connected)
    {
        lv_obj_add_state(button, LV_STATE_CHECKED);
        return;
    }

    lv_obj_remove_state(button, LV_STATE_CHECKED);
}

/**
 * @brief 按 BLE 总开关偏好同步主界面蓝牙图标。
 *
 * 当前主界面蓝牙按钮语义是“BLE 总开关”，因此 checked 状态表达
 * `ble_enabled`，而不是当前 transport 是否真的在广播。
 */
static void main_dropdown_controller_sync_bluetooth_button(void)
{
    lv_obj_t *button = main_dropdown_controller_get_bluetooth_button();
    network_manager_status_t status = {0};
    bool ble_enabled = network_manager_is_ble_enabled();
    bool ble_active = network_manager_is_ble_active();
    bool checked = ble_enabled;

    if (button == NULL)
    {
        return;
    }

    if (main_dropdown_controller_get_network_status(&status))
    {
        ble_enabled = status.ble_enabled;
        ble_active = status.ble_active;
        checked = ble_enabled;
    }

    if (!s_last_bluetooth_checked_valid || s_last_bluetooth_checked != checked)
    {
        ESP_LOGI(TAG, "sync Bluetooth button: checked=%d ble_enabled=%d ble_active=%d",
                 checked ? 1 : 0, ble_enabled ? 1 : 0, ble_active ? 1 : 0);
        s_last_bluetooth_checked = checked;
        s_last_bluetooth_checked_valid = true;
    }

    if (checked)
    {
        lv_obj_add_state(button, LV_STATE_CHECKED);
        return;
    }

    lv_obj_remove_state(button, LV_STATE_CHECKED);
}

/**
 * @brief 主界面状态同步定时器回调。
 *
 * 仅在主界面可见时刷新图标状态；离开主界面后不继续显示临时 toast，
 * 避免提示残留到其他页面上。
 */
static void main_dropdown_controller_status_sync_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (!main_dropdown_controller_is_main_screen_active())
    {
        main_dropdown_controller_hide_toast();
    }
    else
    {
        main_dropdown_controller_sync_wifi_button();
        main_dropdown_controller_sync_bluetooth_button();
    }

    /* notification center 全局 poll（与 main screen 状态无关） */
    {
        memory_watch_service_snapshot_t snap = {0};
        bool is_recording = false;
        if (memory_watch_service_get_snapshot(&snap) == ESP_OK)
        {
            is_recording = (snap.state == MEMORY_WATCH_SERVICE_STATE_RECORDING ||
                            snap.state == MEMORY_WATCH_SERVICE_STATE_ENCODING ||
                            snap.state == MEMORY_WATCH_SERVICE_STATE_UPLOADING);
        }
        /* 从真实的 service 获取告警是否激活（避免遮挡告警红屏） */
        bool safety_alert_active = false;
        const danger_detection_snapshot_t dd_snap = danger_detection_service_get_snapshot();
        const background_service_manager_snapshot_t bsm_snap = background_service_manager_get_snapshot();
        if (bsm_snap.danger_enabled_by_user &&
            dd_snap.state == DANGER_DETECTION_STATE_RUNNING &&
            dd_snap.risk_state == DANGER_DETECTION_RISK_ALERTING)
        {
            safety_alert_active = true;
        }

        watch_nc_poll(is_recording, safety_alert_active);
    }
}

/**
 * @brief toast 超时后自动隐藏的定时器回调。
 * @param[in] timer LVGL 定时器句柄，当前实现未直接使用。
 */
static void main_dropdown_controller_toast_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    main_dropdown_controller_hide_toast();
}

/**
 * @brief 隐藏主界面临时 toast。
 *
 * 这里不会销毁对象，而是重复复用同一个 label，避免用户频繁点击时
 * 持续创建/释放 LVGL 对象带来额外碎片和状态管理复杂度。
 */
static void main_dropdown_controller_hide_toast(void)
{
    if (s_toast_label != NULL)
    {
        lv_obj_add_flag(s_toast_label, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_toast_timer != NULL)
    {
        lv_timer_pause(s_toast_timer);
    }
}

/**
 * @brief 在主界面底部显示一个短时 toast 提示。
 *
 * @param[in] text 需要展示的提示文本；为空时直接忽略。
 */
static void main_dropdown_controller_show_toast(const char *text)
{
    if (text == NULL || text[0] == '\0')
    {
        return;
    }

    ESP_LOGI(TAG, "show BLE toast: %s", text);

    if (s_toast_label == NULL)
    {
        s_toast_label = lv_label_create(lv_layer_top());
        lv_obj_set_width(s_toast_label, 280);
        lv_label_set_long_mode(s_toast_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_radius(s_toast_label, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(s_toast_label, LV_OPA_90, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(s_toast_label, lv_color_hex(0x20242b),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(s_toast_label, lv_color_hex(0xffffff),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_align(s_toast_label, LV_TEXT_ALIGN_CENTER,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_top(s_toast_label, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_bottom(s_toast_label, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_left(s_toast_label, 16, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_right(s_toast_label, 16, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_flag(s_toast_label, LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text(s_toast_label, text);
    lv_obj_remove_flag(s_toast_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_toast_label, LV_ALIGN_BOTTOM_MID, 0, -56);
    lv_obj_move_foreground(s_toast_label);

    if (s_toast_timer == NULL)
    {
        s_toast_timer = lv_timer_create(main_dropdown_controller_toast_timer_cb,
                                        kToastDurationMs, NULL);
        if (s_toast_timer != NULL)
        {
            lv_timer_set_repeat_count(s_toast_timer, 1);
            lv_timer_set_auto_delete(s_toast_timer, false);
        }
    }

    if (s_toast_timer != NULL)
    {
        lv_timer_set_period(s_toast_timer, kToastDurationMs);
        lv_timer_set_repeat_count(s_toast_timer, 1);
        lv_timer_resume(s_toast_timer);
        lv_timer_reset(s_toast_timer);
    }
}

static void main_dropdown_nc_click_cb(watch_nc_nav_target_t target,
                                      const char *notification_id,
                                      void *user_data)
{
    (void)user_data;
    memory_watch_controller_open_via_notification(target, notification_id);
}

/**
 * @brief 绑定主界面下拉菜单控制器。
 *
 * 该入口负责把手写控制逻辑接到 generated UI 上，并恢复蓝牙按钮的
 * 可见性，使其真正承担 BLE 总开关语义。
 *
 * @param[in] ui 当前 UI 句柄。
 */
void main_dropdown_controller_bind(lv_ui *ui)
{
    if (ui == NULL)
    {
        return;
    }

    s_ui = ui;
    wifi_management_controller_init(ui);
    ESP_LOGI(TAG, "bind BLE dropdown controller");
    ESP_LOGI(TAG, "bind main dropdown controller");
    if (s_status_sync_timer == NULL)
    {
        s_status_sync_timer = lv_timer_create(
            main_dropdown_controller_status_sync_timer_cb, kStatusSyncPeriodMs,
            NULL);
    }

    if (s_ui->screen_main_Bluetooth != NULL)
    {
        lv_obj_remove_flag(s_ui->screen_main_Bluetooth, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ui->screen_main_Bluetooth_label != NULL)
    {
        lv_obj_remove_flag(s_ui->screen_main_Bluetooth_label, LV_OBJ_FLAG_HIDDEN);
    }

    main_dropdown_controller_sync_wifi_button();
    main_dropdown_controller_sync_bluetooth_button();

    /* notification center 初始化（幂等，只在首次 bind 时真正执行） */
    static bool s_nc_initialized = false;
    if (!s_nc_initialized)
    {
        static const watch_nc_config_t kNcConfig = {
            .click_cb   = main_dropdown_nc_click_cb,
            .dismiss_cb = NULL,
            .user_data  = NULL,
        };
        watch_nc_init(&kNcConfig);
        s_nc_initialized = true;
    }
}

/**
 * @brief 处理主界面 Wi-Fi 图标点击。
 *
 * 主界面 Wi-Fi 图标只承担“进入 Wi-Fi 管理页”入口，不在主界面直接
 * 承载复杂联网操作，避免下拉菜单承载过重状态机交互。
 */
void main_dropdown_controller_handle_wifi_click(void)
{
    ESP_LOGI(TAG, "WiFi button clicked");
    wifi_management_controller_open();
}

/**
 * @brief 处理主界面蓝牙图标点击。
 *
 * 该按钮当前是 BLE 总开关：
 * - 已开启时点击会关闭 BLE；
 * - 已关闭时点击会尝试开启 BLE；
 * - 若底层更新 BLE 总开关失败，则保持当前图标状态并给出 toast 提示。
 */
void main_dropdown_controller_handle_bluetooth_click(void)
{
    const bool ble_enabled = network_manager_is_ble_enabled();
    const bool ble_active = network_manager_is_ble_active();
    esp_err_t ret = ESP_OK;

    ESP_LOGI(TAG, "Bluetooth button clicked: ble_enabled=%d ble_active=%d",
             ble_enabled ? 1 : 0, ble_active ? 1 : 0);

    if (ble_enabled)
    {
        ret = network_manager_set_ble_enabled(false);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "disable BLE provisioning failed: %s",
                     esp_err_to_name(ret));
        }
        main_dropdown_controller_sync_bluetooth_button();
        return;
    }

    ret = foreground_runtime_gate_acquire(
        FOREGROUND_RUNTIME_OWNER_BLE_PROVISIONING, 0U);
    if (ret != ESP_OK)
    {
        main_dropdown_controller_show_toast("BLE switch update failed");
        ESP_LOGW(TAG, "enable BLE gate acquire failed: %s",
                 esp_err_to_name(ret));
        main_dropdown_controller_sync_bluetooth_button();
        return;
    }
    (void)background_service_manager_notify_foreground_runtime_changed();

    background_https_gate_quiet_for(kBleQuietRetryMs,
                                    "main_ble_enable_start");
    ret = network_manager_set_ble_enabled(true);
    if (ret == ESP_ERR_NO_MEM)
    {
        background_https_gate_quiet_for(kBleQuietRetryMs,
                                        "main_ble_enable_retry");
        vTaskDelay(pdMS_TO_TICKS(kBleQuietRetryMs));
        ret = network_manager_set_ble_enabled(true);
    }
    (void)foreground_runtime_gate_release(
        FOREGROUND_RUNTIME_OWNER_BLE_PROVISIONING);
    (void)background_service_manager_notify_foreground_runtime_changed();

    if (ret != ESP_OK)
    {
        main_dropdown_controller_show_toast("BLE switch update failed");
        ESP_LOGW(TAG, "enable BLE switch failed: %s",
                 esp_err_to_name(ret));
    }

    main_dropdown_controller_sync_bluetooth_button();
}
