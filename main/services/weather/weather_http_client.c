#include <string.h>
#include <sys/param.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "services/weather/weather_http_client.h"

#define MAX_HTTP_OUTPUT_BUFFER 1024 // HTTP响应缓冲区最大长度

/*
 * 天气接口实现说明：
 * - 使用 ESP HTTP Client 访问心知天气接口；
 * - 事件回调负责拼接分片响应，结束后再统一做 JSON 解析；
 * - 解析结果写入全局天气快照，供其他模块读取最新值。
 */

static const char *TAG = "HTTP_CLIENT";           // HTTP 相关日志标签
static int weather_http_client_parse(char *json_data,
                                     weather_http_result_t *out);
static bool s_last_weather_parse_ok = false;      // 最近一次 HTTP 响应是否成功解析并写入快照。

/**
 * @brief HTTP客户端事件处理函数
 *
 * 该函数用于处理ESP-IDF HTTP客户端的各种事件，包括连接、数据接收、断开等。
 * 在接收到心知天气API的数据时，会将数据累积到 weather_buffer，
 * 并在数据接收完成后调用 user_cjson_parse_now 进行JSON解析和信息打印。
 *
 * @param evt HTTP客户端事件结构体，包含事件类型和相关数据
 * @return esp_err_t ESP_OK表示成功处理
 */
static esp_err_t weather_http_event_handler(esp_http_client_event_t *evt)
{
    static int output_len = 0;                                // 已读取的字节数，累计接收数据长度
    static char weather_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0}; // 用于存储心知天气API返回的JSON数据

    switch (evt->event_id)
    {
    case HTTP_EVENT_ERROR:
        // 发生错误时的事件
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        // 与服务器成功建立连接
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        output_len = 0; // 每次连接时重置计数器
        break;
    case HTTP_EVENT_HEADER_SENT:
        // 已发送HTTP请求头
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        // 收到HTTP响应头部
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        // 收到HTTP响应数据（可能多次调用，需累积数据）
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        // 计算本次可复制的数据长度，防止缓冲区溢出
        int copy_len = MIN(evt->data_len, (MAX_HTTP_OUTPUT_BUFFER - output_len - 1));
        if (copy_len > 0)
        {
            // 将本次收到的数据追加到 weather_buffer
            memcpy(weather_buffer + output_len, evt->data, copy_len);
            output_len += copy_len;
            weather_buffer[output_len] = '\0'; // 确保字符串以'\0'结尾，便于后续解析
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        // 数据接收完成（所有数据已收到）
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        // 如果已收到数据，则进行JSON解析
        if (output_len > 0)
        {
            s_last_weather_parse_ok = weather_http_client_parse(
                weather_buffer, (weather_http_result_t *)evt->user_data) == 0;
        }
        output_len = 0; // 重置计数器，准备下次接收
        break;
    case HTTP_EVENT_DISCONNECTED:
        // 与服务器断开连接
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    case HTTP_EVENT_REDIRECT:
        // 收到重定向响应
        ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
        break;
    }
    return ESP_OK;
}

/**
 * @brief 从心知天气API获取天气数据
 *
 * 发送HTTPS GET请求获取广州当前天气数据
 */
esp_err_t weather_http_client_fetch(weather_http_result_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    s_last_weather_parse_ok = false;

    esp_http_client_config_t config = {
        .url = "https://api.seniverse.com/v3/weather/now.json?key=<YOUR_SENIVERSE_API_KEY>&location=guangzhou&language=zh-Hans&unit=c",
        .method = HTTP_METHOD_GET,
        .event_handler = weather_http_event_handler,
        .user_data = out,
        .disable_auto_redirect = true,
        .timeout_ms = 10000,                        // HTTPS需要更长超时时间
        .crt_bundle_attach = esp_crt_bundle_attach, // 使用系统内置根证书
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "HTTP client init failed");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    int content_length = esp_http_client_get_content_length(client);
    ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d", status_code, content_length);
    esp_http_client_cleanup(client); // 清理HTTP客户端

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status_code != 200) {
        ESP_LOGW(TAG, "HTTP request returned status code: %d", status_code);
        return ESP_FAIL;
    }
    if (!s_last_weather_parse_ok) {
        ESP_LOGW(TAG, "Weather response parse failed");
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief 解析心知天气实时天气JSON数据
 * @param json_data 心知天气API返回的原始JSON字符串
 * @return 0表示解析成功，-1表示失败
 *
 * 解析结果复制到调用方 DTO，不保留 cJSON 节点指针。
 */
static int weather_http_client_parse(char *json_data,
                                     weather_http_result_t *out)
{
    if (json_data == NULL || out == NULL) {
        return -1;
    }
    cJSON *root = NULL;
    cJSON *results = NULL;
    cJSON *item = NULL;
    cJSON *location = NULL;
    cJSON *now = NULL;

    root = cJSON_Parse(json_data); // 解析 JSON 字符串
    if (!root)
    {
        ESP_LOGI(TAG, "JSON解析失败: [%s]\n", cJSON_GetErrorPtr());
        return -1;
    }

    results = cJSON_GetObjectItem(root, "results"); // 获取results数组
    if (!results || !cJSON_IsArray(results))
    {
        ESP_LOGI(TAG, "未找到results数组");
        cJSON_Delete(root);
        return -1;
    }

    item = cJSON_GetArrayItem(results, 0); // 取第一个城市结果
    if (!item)
    {
        ESP_LOGI(TAG, "results数组为空");
        cJSON_Delete(root);
        return -1;
    }

    const char *location_name = "";
    location = cJSON_GetObjectItem(item, "location");
    if (location)
    {
        cJSON *name = cJSON_GetObjectItem(location, "name");
        if (cJSON_IsString(name) && name->valuestring != NULL) {
            location_name = name->valuestring;
        }
    }

    // 解析now对象
    now = cJSON_GetObjectItem(item, "now");
    cJSON *text = now != NULL ? cJSON_GetObjectItem(now, "text") : NULL;
    cJSON *code = now != NULL ? cJSON_GetObjectItem(now, "code") : NULL;
    cJSON *temperature =
        now != NULL ? cJSON_GetObjectItem(now, "temperature") : NULL;
    if (!cJSON_IsString(text) || text->valuestring == NULL ||
        !cJSON_IsString(code) || code->valuestring == NULL ||
        !cJSON_IsString(temperature) || temperature->valuestring == NULL) {
        ESP_LOGI(TAG, "天气字段不完整");
        cJSON_Delete(root);
        return -1;
    }

    out->temperature = atoi(temperature->valuestring);
    strncpy(out->text, text->valuestring, sizeof(out->text) - 1);
    strncpy(out->code, code->valuestring, sizeof(out->code) - 1);

    cJSON *last_update_item = cJSON_GetObjectItem(item, "last_update");
    const char *last_update =
        cJSON_IsString(last_update_item) ? last_update_item->valuestring : "";

    // 时间格式转换：ISO8601 -> "YYYY-MM-DD HH:MM:SS"
    char formatted_time[64] = {0};
    char year[5] = {0}, month[3] = {0}, day[3] = {0}, hour[3] = {0}, minute[3] = {0}, second[3] = {0};

    // 从ISO 8601格式中提取日期和时间部分
    // 格式示例: "2025-09-05T15:37:36+08:00"
    if (strlen(last_update) >= 19)
    {
        strncpy(year, last_update, 4);
        strncpy(month, last_update + 5, 2);
        strncpy(day, last_update + 8, 2);
        strncpy(hour, last_update + 11, 2);
        strncpy(minute, last_update + 14, 2);
        strncpy(second, last_update + 17, 2);

        sprintf(formatted_time, "%s-%s-%s %s:%s:%s",
                year, month, day, hour, minute, second);
    }
    else
    {
        strncpy(formatted_time, last_update, sizeof(formatted_time) - 1);
    }

    // 打印解析结果
    ESP_LOGI(TAG, "城市: %s", location_name);
    ESP_LOGI(TAG, "天气: %s", out->text);
    ESP_LOGI(TAG, "温度: %d°C", out->temperature);
    ESP_LOGI(TAG, "天气更新时间: %s", formatted_time); // 使用转换后的时间格式

    cJSON_Delete(root); // 释放内存
    return 0;
}
