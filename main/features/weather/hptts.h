#ifndef HPTTS_H
#define HPTTS_H

#include "esp_err.h"
#include "esp_http_client.h"
extern struct tm timeinfo;

/*
 * 心知天气实时数据结构
 * 字段均为指向 JSON 节点字符串的指针，生命周期受 cJSON 解析对象影响。
 */
typedef struct
{
    char *id;
    char *name;
    char *country;
    char *path;
    char *timezone;
    char *timezone_offset;
    char *weather_text;
    char *weather_code;
    char *temperature;
    char *last_update;
} user_seniverse_now_config_t;
/* HTTP 客户端事件回调：接收并拼接天气接口返回的数据。 */
esp_err_t _http_event_handler(esp_http_client_event_t *evt);
/* 发起天气 HTTP 请求，并在解析成功写入天气快照后返回 ESP_OK。 */
esp_err_t http_rest_with_url(void);
#endif // HPTTS_H
