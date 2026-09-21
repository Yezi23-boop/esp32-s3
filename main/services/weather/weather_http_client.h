#pragma once

#include "esp_err.h"

typedef struct {
    int temperature; /**< 摄氏温度。 */
    char text[32];    /**< 天气描述。 */
    char code[8];     /**< 心知天气代码。 */
} weather_http_result_t;

/**
 * @brief 同步拉取并解析一次天气数据。
 *
 * 调用方必须在后台任务中执行；请求调度和失败重试由 weather owner 负责。
 *
 * @param[out] out 解析后的天气数据。
 * @return ESP_OK 表示 HTTP 200 且响应解析成功。
 */
esp_err_t weather_http_client_fetch(weather_http_result_t *out);
