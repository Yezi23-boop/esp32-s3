#ifndef OTA_MAINTENANCE_VIEW_H
#define OTA_MAINTENANCE_VIEW_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 在 LVGL owner 线程创建 OTA 全屏维护页面。
     * @return ESP_OK 表示页面已创建或已存在。
     */
    esp_err_t ota_maintenance_view_init(void);

    /**
     * @brief 在功能页懒加载完成后创建独立的 OTA 更新入口。
     *
     * 该函数只能由 LVGL owner 线程调用；重复调用不会重复创建按钮。
     */
    void ota_maintenance_view_bind_entry(void);

    /**
     * @brief 打开 OTA 维护页面。
     *
     * 该函数只能由 LVGL 线程调用；它只切换页面，不执行 gate、网络或 Flash
     * 操作。页面按钮依次提交检查、下载、激活三个 OTA intent。
     */
    esp_err_t ota_maintenance_view_open(void);

    /**
     * @brief 在 LVGL 线程轮询 OTA snapshot 并刷新页面。
     */
    void ota_maintenance_view_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_MAINTENANCE_VIEW_H */
