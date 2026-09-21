/**
 * @file ap_portal_routes.c
 * @brief AP 门户适配层的 HTTP 路由与静态资源分发。
 */

#include "ap_portal_routes.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "freertos/portmacro.h"

/** @brief 内嵌 `index.html` 资源起始地址。 */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
/** @brief 内嵌 `app.js` 资源起始地址。 */
extern const uint8_t app_js_start[] asm("_binary_app_js_start");
/** @brief 内嵌 `app.css` 资源起始地址。 */
extern const uint8_t app_css_start[] asm("_binary_app_css_start");
/** @brief 内嵌 `prov_client.js` 资源起始地址。 */
extern const uint8_t prov_client_js_start[] asm("_binary_prov_client_js_start");
/** @brief 内嵌 `prov_proto_bundle.js` 资源起始地址。 */
extern const uint8_t prov_proto_bundle_js_start[] asm("_binary_prov_proto_bundle_js_start");

/** @brief Memory Watch endpoint 配置请求体最大字节数，防止门户 HTTPD 栈和堆被大包拖垮。 */
enum
{
    kMemoryWatchConfigMaxBodyBytes = 768U,
    /** @brief 限制半包请求反复超时次数，避免共享 HTTPD task 被单个客户端长期占住。 */
    kMemoryWatchConfigMaxRecvTimeouts = 4U,
};
/** @brief 上层注册的 Memory Watch 配置保存回调。 */
static ap_portal_memory_watch_config_cb_t s_memory_watch_config_callback = NULL;
/** @brief 透传给 Memory Watch 配置保存回调的用户上下文。 */
static void *s_memory_watch_config_user_ctx = NULL;
/** @brief 上层注册的 Memory Watch 配置状态查询回调。 */
static ap_portal_memory_watch_configured_cb_t s_memory_watch_configured_callback = NULL;
/** @brief 透传给 Memory Watch 配置状态查询回调的用户上下文。 */
static void *s_memory_watch_configured_user_ctx = NULL;
/** @brief 保护配置回调指针，避免 HTTPD task 读取到半更新状态。 */
static portMUX_TYPE s_memory_watch_config_lock = portMUX_INITIALIZER_UNLOCKED;

/**
 * @brief 发送内嵌的文本静态资源。
 *
 * 这里统一用于发送 `html/css/javascript/json` 文本，避免每个 handler 都重复写响应头和
 * 文本发送逻辑。
 *
 * @param[in] req HTTP 请求对象。
 * @param[in] content_type 返回的 `Content-Type`。
 * @param[in] payload 要发送的文本内容。
 * @return `ESP_OK` 表示响应已提交；其他错误表示 HTTP 框架发送失败。
 */
static esp_err_t ap_portal_send_text(httpd_req_t *req, const char *content_type,
                                     const char *payload)
{
    if (req == NULL || content_type == NULL || payload == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_resp_set_type(req, content_type);
    return httpd_resp_send(req, payload, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ap_portal_send_json_error(httpd_req_t *req,
                                           const char *status,
                                           const char *error)
{
    char payload[96] = {0};

    if (req == NULL || status == NULL || error == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    (void)snprintf(payload, sizeof(payload),
                   "{\"ok\":false,\"error\":\"%s\"}", error);
    httpd_resp_set_status(req, status);
    return ap_portal_send_text(req, "application/json; charset=utf-8",
                               payload);
}

static bool ap_portal_get_memory_watch_config_callback(
    ap_portal_memory_watch_config_cb_t *out_callback, void **out_user_ctx)
{
    if (out_callback == NULL || out_user_ctx == NULL)
    {
        return false;
    }

    taskENTER_CRITICAL(&s_memory_watch_config_lock);
    *out_callback = s_memory_watch_config_callback;
    *out_user_ctx = s_memory_watch_config_user_ctx;
    taskEXIT_CRITICAL(&s_memory_watch_config_lock);

    return *out_callback != NULL;
}

static bool ap_portal_get_memory_watch_configured_callback(
    ap_portal_memory_watch_configured_cb_t *out_callback, void **out_user_ctx)
{
    if (out_callback == NULL || out_user_ctx == NULL)
    {
        return false;
    }

    taskENTER_CRITICAL(&s_memory_watch_config_lock);
    *out_callback = s_memory_watch_configured_callback;
    *out_user_ctx = s_memory_watch_configured_user_ctx;
    taskEXIT_CRITICAL(&s_memory_watch_config_lock);

    return *out_callback != NULL;
}

static esp_err_t ap_portal_read_json_body(httpd_req_t *req, char *dst,
                                          size_t dst_len,
                                          size_t *out_body_len)
{
    if (req == NULL || dst == NULL || dst_len == 0U ||
        out_body_len == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t content_len = (size_t)req->content_len;
    if (content_len == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (content_len >= dst_len ||
        content_len > kMemoryWatchConfigMaxBodyBytes)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t received_total = 0;
    uint32_t timeout_count = 0;
    while (received_total < content_len)
    {
        const int received = httpd_req_recv(
            req, dst + received_total, content_len - received_total);
        if (received == HTTPD_SOCK_ERR_TIMEOUT)
        {
            ++timeout_count;
            if (timeout_count >= kMemoryWatchConfigMaxRecvTimeouts)
            {
                dst[0] = '\0';
                return ESP_ERR_TIMEOUT;
            }
            continue;
        }
        if (received <= 0)
        {
            dst[0] = '\0';
            return ESP_FAIL;
        }
        timeout_count = 0;
        received_total += (size_t)received;
    }

    dst[content_len] = '\0';
    *out_body_len = content_len;
    return ESP_OK;
}

static esp_err_t ap_portal_json_copy_required_string(
    const cJSON *root, const char *key, char *dst, size_t dst_len)
{
    if (root == NULL || key == NULL || dst == NULL || dst_len == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        item->valuestring[0] == '\0')
    {
        dst[0] = '\0';
        return ESP_ERR_INVALID_ARG;
    }

    const size_t len = strlen(item->valuestring);
    if (len >= dst_len)
    {
        dst[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(dst, item->valuestring, len + 1U);
    return ESP_OK;
}

static esp_err_t ap_portal_parse_memory_watch_config(
    const char *body, size_t body_len,
    ap_portal_memory_watch_config_t *out_config)
{
    if (body == NULL || body_len == 0U || out_config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_ParseWithLength(body, body_len);
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    ap_portal_memory_watch_config_t config = {0};
    esp_err_t err = ap_portal_json_copy_required_string(
        root, "base_url", config.base_url, sizeof(config.base_url));
    if (err == ESP_OK)
    {
        err = ap_portal_json_copy_required_string(
            root, "device_id", config.device_id, sizeof(config.device_id));
    }
    if (err == ESP_OK)
    {
        err = ap_portal_json_copy_required_string(
            root, "device_token", config.device_token,
            sizeof(config.device_token));
    }

    const cJSON *timeout = cJSON_GetObjectItemCaseSensitive(root,
                                                            "timeout_ms");
    if (err == ESP_OK && timeout != NULL)
    {
        if (!cJSON_IsNumber(timeout) || timeout->valuedouble < 0.0 ||
            timeout->valuedouble > (double)UINT32_MAX)
        {
            err = ESP_ERR_INVALID_ARG;
        }
        else
        {
            config.timeout_ms = (uint32_t)timeout->valuedouble;
        }
    }

    const cJSON *allow_http = cJSON_GetObjectItemCaseSensitive(root,
                                                               "allow_http");
    if (allow_http == NULL)
    {
        allow_http = cJSON_GetObjectItemCaseSensitive(root,
                                                      "allow_insecure_http");
    }
    if (err == ESP_OK && allow_http != NULL)
    {
        if (!cJSON_IsBool(allow_http))
        {
            err = ESP_ERR_INVALID_ARG;
        }
        else
        {
            config.allow_insecure_http = cJSON_IsTrue(allow_http);
        }
    }

    cJSON_Delete(root);
    if (err != ESP_OK)
    {
        return err;
    }

    *out_config = config;
    return ESP_OK;
}

/**
 * @brief 返回门户首页 HTML。
 *
 * 当前 `index.html` 已从旧 `apcfg.html` 拆到独立资源目录，后续页面样式和脚本可独立演进，
 * 不再把整份前端混在单个 C 字符串里维护。
 *
 * @param[in] req HTTP 请求对象。
 * @return `ESP_OK` 表示响应已提交。
 */
static esp_err_t ap_portal_root_get_handler(httpd_req_t *req)
{
    return ap_portal_send_text(req, "text/html; charset=utf-8",
                               (const char *)index_html_start);
}

/**
 * @brief 返回门户前端脚本资源。
 *
 * 页面改为通过独立脚本文件承接浏览器交互逻辑，后续接设备侧 API 时无需再改 C 字符串。
 *
 * @param[in] req HTTP 请求对象。
 * @return `ESP_OK` 表示响应已提交。
 */
static esp_err_t ap_portal_app_js_handler(httpd_req_t *req)
{
    return ap_portal_send_text(req, "application/javascript; charset=utf-8",
                               (const char *)app_js_start);
}

/**
 * @brief 返回门户样式资源。
 *
 * @param[in] req HTTP 请求对象。
 * @return `ESP_OK` 表示响应已提交。
 */
static esp_err_t ap_portal_app_css_handler(httpd_req_t *req)
{
    return ap_portal_send_text(req, "text/css; charset=utf-8",
                               (const char *)app_css_start);
}

/**
 * @brief 返回 provisioning client 模块脚本。
 *
 * 浏览器入口 `app.js` 会通过 ES module 直接导入该文件；如果这里不显式注册 GET 路由，
 * SoftAP 门户虽然能打开首页，但模块导入会在浏览器端直接 `404`，导致官方 provisioning
 * client 根本无法初始化。
 *
 * @param[in] req HTTP 请求对象。
 * @return `ESP_OK` 表示响应已提交。
 */
static esp_err_t ap_portal_prov_client_js_handler(httpd_req_t *req)
{
    return ap_portal_send_text(req, "application/javascript; charset=utf-8",
                               (const char *)prov_client_js_start);
}

/**
 * @brief 返回最小 protobuf 编解码模块脚本。
 *
 * 该脚本由 `prov_client.js` 继续导入；把协议层拆到独立模块后，路由层也必须同步暴露对应
 * 的静态资源，否则浏览器会在第二层 import 处失败。
 *
 * @param[in] req HTTP 请求对象。
 * @return `ESP_OK` 表示响应已提交。
 */
static esp_err_t ap_portal_prov_proto_bundle_js_handler(httpd_req_t *req)
{
    return ap_portal_send_text(req, "application/javascript; charset=utf-8",
                               (const char *)prov_proto_bundle_js_start);
}

/**
 * @brief 返回最小门户状态接口。
 *
 * 这是 `ap_portal_adapter` 第一版显式暴露给页面的 HTTP API 接缝。当前先只提供门户与设备
 * 侧的占位状态，后续扫描和配置接口会在此基础上继续扩展。
 *
 * @param[in] req HTTP 请求对象。
 * @return `ESP_OK` 表示响应已提交。
 */
static esp_err_t ap_portal_status_handler(httpd_req_t *req)
{
    ap_portal_memory_watch_config_cb_t callback = NULL;
    void *user_ctx = NULL;
    const bool memory_watch_supported =
        ap_portal_get_memory_watch_config_callback(&callback, &user_ctx);

    ap_portal_memory_watch_configured_cb_t configured_cb = NULL;
    void *configured_ctx = NULL;
    const bool has_configured_cb =
        ap_portal_get_memory_watch_configured_callback(&configured_cb,
                                                       &configured_ctx);
    const bool endpoint_configured =
        has_configured_cb && configured_cb(configured_ctx);

    char status_json[256] = {0};

    (void)snprintf(status_json, sizeof(status_json),
                   "{"
                   "\"ok\":true,"
                   "\"portal\":\"ap_portal_adapter\","
                   "\"api_version\":\"v1\","
                   "\"scan_supported\":false,"
                   "\"configure_supported\":false,"
                   "\"memory_watch_config_supported\":%s,"
                   "\"memory_watch_endpoint_configured\":%s"
                   "}",
                   memory_watch_supported ? "true" : "false",
                   endpoint_configured ? "true" : "false");
    return ap_portal_send_text(req, "application/json; charset=utf-8",
                               status_json);
}

/**
 * @brief 返回旧 JSON 接口的兼容提示。
 *
 * 当前门户正式主链路已经迁到浏览器端官方 provisioning client，因此旧 `/api/scan`
 * 与 `/api/configure` 只保留兼容提示，不再承载真实配网逻辑。这里使用 `410 Gone`
 * 明确告诉调用方：该 JSON 接口已经退役，而不是暂时未实现。
 *
 * @param[in] req HTTP 请求对象。
 * @return `ESP_OK` 表示响应已提交。
 */
static esp_err_t ap_portal_legacy_api_handler(httpd_req_t *req)
{
    /* 使用稳定英文错误码与提示文案，便于旧调用方和源码测试统一识别接口已迁移事实。 */
    static const char *kLegacyJson =
        "{"
        "\"ok\":false,"
        "\"error\":\"legacy_api_removed\","
        "\"message\":\"Legacy JSON API has been replaced by official provisioning client.\""
        "}";

    httpd_resp_set_status(req, "410 Gone");
    return ap_portal_send_text(req, "application/json; charset=utf-8",
                               kLegacyJson);
}

/**
 * @brief 保存 AI Memory Watch endpoint 配置。
 *
 * 请求体是 JSON，只接收 `base_url/device_id/device_token/timeout_ms/allow_http`。
 * 响应不会回显 token，HTTPD 日志也不打印请求体，避免设备 token 泄露。
 *
 * @param[in] req HTTP 请求对象。
 * @return `ESP_OK` 表示响应已提交。
 */
static esp_err_t ap_portal_memory_watch_config_handler(httpd_req_t *req)
{
    ap_portal_memory_watch_config_cb_t callback = NULL;
    void *user_ctx = NULL;
    if (!ap_portal_get_memory_watch_config_callback(&callback, &user_ctx))
    {
        return ap_portal_send_json_error(
            req, "501 Not Implemented",
            "memory_watch_config_unavailable");
    }

    char body[kMemoryWatchConfigMaxBodyBytes + 1U] = {0};
    size_t body_len = 0;
    esp_err_t err = ap_portal_read_json_body(req, body, sizeof(body),
                                             &body_len);
    if (err == ESP_ERR_INVALID_SIZE)
    {
        return ap_portal_send_json_error(req, "413 Payload Too Large",
                                         "payload_too_large");
    }
    if (err == ESP_ERR_TIMEOUT)
    {
        return ap_portal_send_json_error(req, "408 Request Timeout",
                                         "request_timeout");
    }
    if (err != ESP_OK)
    {
        return ap_portal_send_json_error(req, "400 Bad Request",
                                         "invalid_json");
    }

    ap_portal_memory_watch_config_t config = {0};
    err = ap_portal_parse_memory_watch_config(body, body_len, &config);
    if (err == ESP_ERR_INVALID_SIZE)
    {
        return ap_portal_send_json_error(req, "400 Bad Request",
                                         "field_too_long");
    }
    if (err != ESP_OK)
    {
        return ap_portal_send_json_error(req, "400 Bad Request",
                                         "invalid_config");
    }

    err = callback(&config, user_ctx);
    if (err == ESP_ERR_INVALID_STATE)
    {
        return ap_portal_send_json_error(req, "409 Conflict",
                                         "request_active");
    }
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE)
    {
        return ap_portal_send_json_error(req, "400 Bad Request",
                                         "invalid_config");
    }
    if (err != ESP_OK)
    {
        return ap_portal_send_json_error(req, "500 Internal Server Error",
                                         "save_failed");
    }

    return ap_portal_send_text(
        req, "application/json; charset=utf-8",
        "{\"ok\":true,\"memory_watch_configured\":true}");
}

/**
 * @brief 处理 `favicon.ico` 请求并返回空响应。
 *
 * 浏览器默认会请求 `favicon.ico`，这里显式返回 `204`，避免调试阶段被多余 404 日志污染。
 *
 * @param[in] req HTTP 请求对象。
 * @return `ESP_OK` 表示响应已提交。
 */
static esp_err_t ap_portal_favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/**
 * @brief 将未知 HTTP 路径重定向到门户首页。
 *
 * 当前不能使用“全量通配重定向”去吞掉所有请求，因为官方 `prov-*` endpoint 与静态资源
 * 都复用同一个 HTTPD。选择 404 error handler 的原因是：只把系统联网探测访问到的未知
 * 路径引到 `/`，而不误伤已经注册完成的协议接口和前端资源。
 *
 * @param[in] req HTTP 请求对象。
 * @param[in] err HTTP 错误码；当前主要处理 `HTTPD_404_NOT_FOUND`。
 * @return `ESP_OK` 表示重定向响应已提交。
 */
static esp_err_t ap_portal_captive_redirect_error_handler(httpd_req_t *req,
                                                          httpd_err_code_t err)
{
    (void)err;

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    /* iOS captive portal 检测通常要求响应体存在实际内容，单纯返回空 redirect
     * 有机会被系统忽略，导致“已连热点但不自动弹页”。 */
    return httpd_resp_send(req, "Redirect to the captive portal",
                           HTTPD_RESP_USE_STRLEN);
}

esp_err_t ap_portal_routes_set_memory_watch_config_callback(
    ap_portal_memory_watch_config_cb_t callback, void *user_ctx)
{
    taskENTER_CRITICAL(&s_memory_watch_config_lock);
    s_memory_watch_config_callback = callback;
    s_memory_watch_config_user_ctx = user_ctx;
    taskEXIT_CRITICAL(&s_memory_watch_config_lock);

    return ESP_OK;
}

esp_err_t ap_portal_routes_set_memory_watch_configured_callback(
    ap_portal_memory_watch_configured_cb_t callback, void *user_ctx)
{
    taskENTER_CRITICAL(&s_memory_watch_config_lock);
    s_memory_watch_configured_callback = callback;
    s_memory_watch_configured_user_ctx = user_ctx;
    taskEXIT_CRITICAL(&s_memory_watch_config_lock);

    return ESP_OK;
}

/**
 * @brief 向给定 HTTPD 注册 AP 门户路由。
 *
 * 当前会注册：
 * - 静态页面 `/`
 * - 前端资源 `/app.js`、`/app.css`、`/prov_client.js` 与 `/prov_proto_bundle.js`
 * - 最小状态接口 `/api/status`
 * - 兼容提示接口 `/api/scan` 与 `/api/configure`
 * - AI Memory Watch 配置接口 `/api/memory-watch/config`
 *
 * @param[in] server 已启动的 HTTPD 句柄。
 * @return `ESP_OK` 表示注册成功；其他错误表示参数非法或路由注册失败。
 */
esp_err_t ap_portal_routes_register(httpd_handle_t server)
{
    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = ap_portal_root_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t js_uri = {
        .uri = "/app.js",
        .method = HTTP_GET,
        .handler = ap_portal_app_js_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t css_uri = {
        .uri = "/app.css",
        .method = HTTP_GET,
        .handler = ap_portal_app_css_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t prov_client_uri = {
        .uri = "/prov_client.js",
        .method = HTTP_GET,
        .handler = ap_portal_prov_client_js_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t prov_proto_bundle_uri = {
        .uri = "/prov_proto_bundle.js",
        .method = HTTP_GET,
        .handler = ap_portal_prov_proto_bundle_js_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = ap_portal_status_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t scan_uri = {
        .uri = "/api/scan",
        .method = HTTP_POST,
        .handler = ap_portal_legacy_api_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t configure_uri = {
        .uri = "/api/configure",
        .method = HTTP_POST,
        .handler = ap_portal_legacy_api_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t memory_watch_config_uri = {
        .uri = "/api/memory-watch/config",
        .method = HTTP_POST,
        .handler = ap_portal_memory_watch_config_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t favicon_uri = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = ap_portal_favicon_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t *uris[] = {
        &root_uri,
        &js_uri,
        &css_uri,
        &prov_client_uri,
        &prov_proto_bundle_uri,
        &status_uri,
        &scan_uri,
        &configure_uri,
        &memory_watch_config_uri,
        &favicon_uri,
    };
    esp_err_t ret = ESP_OK;
    size_t index = 0;

    if (server == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    for (index = 0; index < (sizeof(uris) / sizeof(uris[0])); ++index)
    {
        ret = httpd_register_uri_handler(server, uris[index]);
        if (ret != ESP_OK)
        {
            return ret;
        }
    }

    ret = httpd_register_err_handler(server, HTTPD_404_NOT_FOUND,
                                     ap_portal_captive_redirect_error_handler);
    if (ret != ESP_OK)
    {
        return ret;
    }

    return ESP_OK;
}
