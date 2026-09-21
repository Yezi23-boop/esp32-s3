#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    int temp;              /**< 实时温度，单位摄氏度。 */
    char weather_text[32]; /**< 天气描述。 */
    char icon_path[64];    /**< 对应的本地 LVGL 图片路径。 */
    bool is_valid;         /**< 已成功拉取并发布天气快照。 */
} weather_info_t;

/**
 * @brief 天气后台任务入口。
 *
 * 保留既有函数名以避免改变启动合同；任务负责按网络和电源预算刷新快照。
 */
void time_and_weather(void *pvParameters);

/**
 * @brief 获取最新天气快照。
 *
 * @param[out] out 天气快照。
 * @return ESP_OK 表示读取成功。
 */
esp_err_t weather_service_get_info(weather_info_t *out);
