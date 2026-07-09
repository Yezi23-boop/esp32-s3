#ifndef TIME_WEATHER_H
#define TIME_WEATHER_H

#include <stdbool.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
#ifndef ESP_FAIL
#define ESP_FAIL -1
#endif
#ifndef ESP_ERR_INVALID_ARG
#define ESP_ERR_INVALID_ARG -2
#endif
#ifndef ESP_ERR_INVALID_STATE
#define ESP_ERR_INVALID_STATE -3
#endif
#endif

typedef struct {
    int temp;                 // 实时温度（如 24）
    char weather_text[32];    // 天气描述（如 "多云"）
    char icon_path[64];       // 本地图片路径（如 "A:/weather/duoyun.png"）
    bool is_valid;            // 数据是否已成功拉取并有效
} weather_info_t;

/* 后台时间天气服务初始化与拉取任务入口 */
void time_and_weather(void *pvParameters);

/**
 * @brief 从 UI 层或其它模块获取最新的天气快照信息（线程安全）。
 * @param[out] out 写入获取的天气信息快照。
 * @return ESP_OK 表示获取成功。
 */
esp_err_t weather_service_get_info(weather_info_t *out);

/**
 * @brief 写入最新天气快照。
 * @param[in] temp 摄氏温度。
 * @param[in] text 天气描述文本，函数内部会拷贝。
 * @param[in] code 心知天气代码，用于映射本地天气图标。
 */
void weather_service_update_info(int temp, const char *text, const char *code);

#endif // TIME_WEATHER_H
