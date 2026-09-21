/**
 * @file lv_port_input.c
 * @brief LVGL 触摸输入链路实现
 */

#include "esp_log.h"
#include "lv_port_internal.h"
#include "touch_ft5x06.h"

// `lvgl_port` 组件不直接依赖 `main` 目录头路径，这里只前向声明最小活跃通知接口。
void ui_refresh_policy_notify_touch(void);

/**
 * @brief LVGL 触摸输入读取回调。
 * @param[in] indev LVGL 输入设备句柄，当前实现未使用。
 * @param[out] data 输出到 LVGL 的输入数据结构，需填充坐标和按压状态。
 * @return 无返回值。
 *
 * @note 该回调运行在 LVGL 输入轮询上下文中，因此这里只读取触摸芯片并同步最近坐标，
 *       不做复杂业务处理。
 */
static void lv_port_indev_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    uint16_t x[1] = {0};   // 触摸芯片返回的 X 坐标缓存；当前只消费第一个点。
    uint16_t y[1] = {0};   // 触摸芯片返回的 Y 坐标缓存；当前只消费第一个点。
    uint8_t point_num = 0; // 本次扫描到的触点数量。
    esp_err_t ret = touch_ft5x06_read_points(x, y, &point_num, 1);

    if (ret == ESP_OK && point_num > 0)
    {
        ui_refresh_policy_notify_touch();
        s_last_x = x[0];
        s_last_y = y[0];
        data->point.x = s_last_x;
        data->point.y = s_last_y;
        data->state = LV_INDEV_STATE_PRESSED;
    }
    else
    {
        data->point.x = s_last_x;
        data->point.y = s_last_y;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/**
 * @brief 初始化 LVGL 指针输入设备。
 * @return 无返回值。
 */
void lv_port_indev_init(void)
{
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, lv_port_indev_read);

    ESP_LOGI(LV_PORT_TAG, "LVGL 9.5 输入设备初始化完成");
}

/**
 * @brief 初始化 FT5x06 触摸驱动并同步运行时句柄。
 * @return 无返回值。
 *
 * @note 若初始化或句柄获取失败，会统一把 `s_touch` 置空，避免后续路径误用旧指针。
 */
void lv_port_touch_init(void)
{
    if (touch_ft5x06_init() == ESP_OK)
    {
        if (touch_ft5x06_get_handle(&s_touch) == ESP_OK)
        {
            ESP_LOGI(LV_PORT_TAG, "FT5x06 触摸初始化完成");
        }
        else
        {
            ESP_LOGE(LV_PORT_TAG, "获取触摸句柄失败");
            s_touch = NULL;
        }
    }
    else
    {
        ESP_LOGE(LV_PORT_TAG, "FT5x06 触摸初始化失败");
        s_touch = NULL;
    }
}
