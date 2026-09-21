#include "music_http_client.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
static const size_t kResponseBytes = 4096U;
static const size_t kQrResponseBytes = 8192U;
/* 失效的长连接只重试一次，避免控制路径无限重试挤占媒体流。 */
static const uint8_t kControlRequestAttempts = 2U;
/* OpenResty 当前控制面 keepalive_timeout 为 60 秒，提前 15 秒轮换空闲连接。 */
static const int64_t kControlIdleRefreshUs = 45LL * 1000LL * 1000LL;
/* 媒体流读超时短于默认控制超时；超时只让 reader 让出调度，不终止流。 */
static const int kMediaReadTimeoutMs = 250;

struct music_http_stream
{
    esp_http_client_handle_t client;
    char content_type[40];
};

static esp_err_t music_http_stream_event_handler(esp_http_client_event_t *event)
{
    if (event == NULL || event->user_data == NULL ||
        event->event_id != HTTP_EVENT_ON_HEADER || event->header_key == NULL ||
        event->header_value == NULL ||
        strcasecmp(event->header_key, "Content-Type") != 0)
    {
        return ESP_OK;
    }
    music_http_stream_t *stream = (music_http_stream_t *)event->user_data;
    snprintf(stream->content_type, sizeof(stream->content_type), "%s",
             event->header_value);
    return ESP_OK;
}

static bool music_http_is_insecure_allowed(
    const music_http_client_config_t *config)
{
    return config->allow_insecure_http ||
           strncmp(config->base_url, "https://", 8U) == 0;
}

static esp_err_t music_http_build_url(const music_http_client_config_t *config,
                                      const char *path, char *url,
                                      size_t url_size)
{
    if (config == NULL || path == NULL || url == NULL ||
        config->base_url[0] == '\0' || config->device_id[0] == '\0' ||
        config->device_token[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!music_http_is_insecure_allowed(config))
    {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t base_len = strlen(config->base_url);
    const bool has_slash = base_len > 0U && config->base_url[base_len - 1U] == '/';
    /* music-service 以 query 中的设备 ID 决定设备白名单；所有请求统一携带。 */
    const char *const query_separator = strchr(path, '?') != NULL ? "&" : "?";
    const int written = snprintf(url, url_size, "%s%s%s%sdevice_id=%s",
                                 config->base_url, has_slash ? "" : "/",
                                 path[0] == '/' ? path + 1 : path,
                                 query_separator, config->device_id);
    return written > 0 && (size_t)written < url_size ? ESP_OK
                                                     : ESP_ERR_INVALID_SIZE;
}

static esp_err_t music_http_set_auth(esp_http_client_handle_t client,
                                     const char *token)
{
    char authorization[MUSIC_SERVICE_DEVICE_TOKEN_MAX_BYTES + 8U];
    const int written = snprintf(authorization, sizeof(authorization),
                                 "Bearer %s", token);
    if (written <= 0 || (size_t)written >= sizeof(authorization))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t ret = esp_http_client_set_header(client, "Authorization",
                                               authorization);
    if (ret == ESP_OK)
    {
        ret = esp_http_client_set_header(client, "Accept", "application/json");
    }
    return ret;
}

static esp_err_t music_http_read_json_response(esp_http_client_handle_t client,
                                               char *response,
                                               size_t response_capacity,
                                               size_t *out_length)
{
    size_t offset = 0U;
    while (true)
    {
        if (offset + 1U >= response_capacity)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        const int read = esp_http_client_read(
            client, response + offset, (int)(response_capacity - offset - 1U));
        if (read < 0)
        {
            return ESP_FAIL;
        }
        if (read == 0)
        {
            break;
        }
        offset += (size_t)read;
    }
    response[offset] = '\0';
    if (out_length != NULL)
    {
        *out_length = offset;
    }
    return ESP_OK;
}

static void music_http_copy_json_string(const cJSON *root, const char *key,
                                        char *output, size_t output_size)
{
    if (output == NULL || output_size == 0U)
    {
        return;
    }
    output[0] = '\0';
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring != NULL)
    {
        snprintf(output, output_size, "%s", item->valuestring);
    }
}

static music_service_state_t music_http_parse_state(const cJSON *item)
{
    if (!cJSON_IsString(item) || item->valuestring == NULL)
    {
        return MUSIC_SERVICE_STATE_ERROR;
    }
    if (strcmp(item->valuestring, "buffering") == 0)
        return MUSIC_SERVICE_STATE_BUFFERING;
    if (strcmp(item->valuestring, "playing") == 0 ||
        strcmp(item->valuestring, "streaming") == 0)
        return MUSIC_SERVICE_STATE_PLAYING;
    if (strcmp(item->valuestring, "paused") == 0)
        return MUSIC_SERVICE_STATE_PAUSED;
    if (strcmp(item->valuestring, "stopped") == 0)
        return MUSIC_SERVICE_STATE_STOPPED;
    return MUSIC_SERVICE_STATE_ERROR;
}

static music_service_mode_t music_http_parse_mode(const cJSON *item)
{
    if (cJSON_IsString(item) && item->valuestring != NULL)
    {
        if (strcmp(item->valuestring, "repeat_one") == 0)
            return MUSIC_SERVICE_MODE_REPEAT_ONE;
        if (strcmp(item->valuestring, "shuffle") == 0)
            return MUSIC_SERVICE_MODE_SHUFFLE;
        if (strcmp(item->valuestring, "order") == 0 ||
            strcmp(item->valuestring, "single_stop") == 0)
            return MUSIC_SERVICE_MODE_ORDER;
        if (strcmp(item->valuestring, "smart") == 0)
            return MUSIC_SERVICE_MODE_SMART;
    }
    return MUSIC_SERVICE_MODE_REPEAT_ALL;
}

static esp_err_t music_http_parse_session(const char *payload, int status,
                                          music_http_session_result_t *result)
{
    cJSON *root = cJSON_Parse(payload);
    if (root == NULL)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    result->http_status = status;
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    result->state = music_http_parse_state(state);
    result->mode = music_http_parse_mode(
        cJSON_GetObjectItemCaseSensitive(root, "mode"));
    const cJSON *position = cJSON_GetObjectItemCaseSensitive(root, "position_ms");
    result->position_ms = cJSON_IsNumber(position) && position->valuedouble >= 0
                              ? (uint32_t)position->valuedouble
                              : 0U;
    music_http_copy_json_string(root, "music_session_id",
                                result->music_session_id,
                                sizeof(result->music_session_id));
    music_http_copy_json_string(root, "stream_id", result->stream_id,
                                sizeof(result->stream_id));
    music_http_copy_json_string(root, "source_id", result->source_id,
                                sizeof(result->source_id));
    music_http_copy_json_string(root, "track_id", result->track_id,
                                sizeof(result->track_id));
    music_http_copy_json_string(root, "error_code", result->error_code,
                                sizeof(result->error_code));
    const cJSON *track = cJSON_GetObjectItemCaseSensitive(root, "track");
    if (cJSON_IsObject(track))
    {
        music_http_copy_json_string(track, "track_id", result->track_id,
                                    sizeof(result->track_id));
        music_http_copy_json_string(track, "title", result->title,
                                    sizeof(result->title));
        music_http_copy_json_string(track, "artist", result->artist,
                                    sizeof(result->artist));
    }
    cJSON_Delete(root);
    return status >= 200 && status < 300 && result->state != MUSIC_SERVICE_STATE_ERROR
               ? ESP_OK
               : ESP_FAIL;
}

void music_http_client_control_reset(music_http_control_client_t *control)
{
    if (control == NULL)
    {
        return;
    }
    if (control->handle != NULL)
    {
        esp_http_client_handle_t client =
            (esp_http_client_handle_t)control->handle;
        (void)esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    control->handle = NULL;
    control->base_url[0] = '\0';
    control->last_request_completed_us = 0;
}

static esp_err_t music_http_control_prepare(
    music_http_control_client_t *control,
    const music_http_client_config_t *config, const char *url,
    esp_http_client_method_t method, esp_http_client_handle_t *out_client)
{
    if (control == NULL || config == NULL || url == NULL || out_client == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_http_client_handle_t client =
        (esp_http_client_handle_t)control->handle;
    if (client != NULL && strcmp(control->base_url, config->base_url) != 0)
    {
        music_http_client_control_reset(control);
        client = NULL;
    }
    if (client != NULL && control->last_request_completed_us > 0 &&
        esp_timer_get_time() - control->last_request_completed_us >=
            kControlIdleRefreshUs)
    {
        ESP_LOGI("music_http",
                 "refreshing idle control connection before server keepalive expiry");
        music_http_client_control_reset(control);
        client = NULL;
    }
    if (client == NULL)
    {
        esp_http_client_config_t http_config = {
            .url = url,
            .method = method,
            .timeout_ms = config->timeout_ms > 0U ? (int)config->timeout_ms : 10000,
            .buffer_size = 4096,
            .buffer_size_tx = 2048,
            .keep_alive_enable = true,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        client = esp_http_client_init(&http_config);
        if (client == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
        control->handle = client;
        snprintf(control->base_url, sizeof(control->base_url), "%s",
                 config->base_url);
        ESP_LOGI("music_http", "opening control HTTP/1.1 connection");
    }
    esp_err_t ret = esp_http_client_set_url(client, url);
    if (ret == ESP_OK)
    {
        ret = esp_http_client_set_method(client, method);
    }
    if (ret == ESP_OK)
    {
        ret = music_http_set_auth(client, config->device_token);
    }
    if (ret != ESP_OK)
    {
        music_http_client_control_reset(control);
        return ret;
    }
    *out_client = client;
    return ESP_OK;
}

static esp_err_t music_http_control_request(
    music_http_control_client_t *control,
    const music_http_client_config_t *config, const char *url,
    esp_http_client_method_t method, const char *body, const char *command_id,
    char *response, size_t response_capacity, int *out_status)
{
    if (control == NULL || response == NULL || out_status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t body_len = body == NULL ? 0U : strlen(body);
    esp_err_t last_error = ESP_FAIL;
    for (uint8_t attempt = 0U; attempt < kControlRequestAttempts; ++attempt)
    {
        esp_http_client_handle_t client = NULL;
        *out_status = 0;
        memset(response, 0, response_capacity);
        esp_err_t ret = music_http_control_prepare(control, config, url, method,
                                                   &client);
        if (ret == ESP_OK)
        {
            if (body_len > 0U)
            {
                ret = esp_http_client_set_header(client, "Content-Type",
                                                 "application/json");
            }
            else
            {
                ret = esp_http_client_delete_header(client, "Content-Type");
                if (ret == ESP_ERR_NOT_FOUND)
                {
                    ret = ESP_OK;
                }
            }
        }
        if (ret == ESP_OK)
        {
            if (command_id != NULL && command_id[0] != '\0')
            {
                ret = esp_http_client_set_header(client, "X-Command-Id",
                                                 command_id);
            }
            else
            {
                ret = esp_http_client_delete_header(client, "X-Command-Id");
                if (ret == ESP_ERR_NOT_FOUND)
                {
                    ret = ESP_OK;
                }
            }
        }
        if (ret == ESP_OK)
        {
            ret = esp_http_client_open(client, body_len);
        }
        if (ret == ESP_OK && body_len > 0U)
        {
            ret = esp_http_client_write(client, body, (int)body_len) ==
                          (int)body_len
                      ? ESP_OK
                      : ESP_FAIL;
        }
        if (ret == ESP_OK)
        {
            (void)esp_http_client_fetch_headers(client);
            *out_status = esp_http_client_get_status_code(client);
            ret = music_http_read_json_response(client, response,
                                                response_capacity, NULL);
        }
        if (ret == ESP_OK)
        {
            control->last_request_completed_us = esp_timer_get_time();
            return ESP_OK;
        }
        last_error = ret;
        music_http_client_control_reset(control);
        if (attempt + 1U < kControlRequestAttempts)
        {
            ESP_LOGW("music_http", "control request failed; reconnecting once: %s",
                     esp_err_to_name(ret));
        }
    }
    return last_error;
}

static esp_err_t music_http_perform_json(
    music_http_control_client_t *control,
    const music_http_client_config_t *config, const char *path,
    esp_http_client_method_t method, const char *body, const char *command_id,
    music_http_session_result_t *out_result)
{
    char url[384];
    esp_err_t ret = music_http_build_url(config, path, url, sizeof(url));
    if (ret != ESP_OK)
    {
        return ret;
    }

    char *response = heap_caps_calloc(1U, kResponseBytes,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (response == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    int status = 0;
    ret = music_http_control_request(control, config, url, method, body,
                                     command_id, response, kResponseBytes,
                                     &status);
    if (ret == ESP_OK && out_result != NULL)
    {
        memset(out_result, 0, sizeof(*out_result));
        out_result->transport_error = ESP_OK;
        ret = music_http_parse_session(response, status, out_result);
    }
    if (out_result != NULL)
    {
        out_result->http_status = status;
        out_result->transport_error = ret;
    }
    heap_caps_free(response);
    return ret;
}

static esp_err_t music_http_perform_raw(
    music_http_control_client_t *control,
    const music_http_client_config_t *config, const char *path,
    esp_http_client_method_t method, const char *body, const char *command_id,
    char *response, size_t response_capacity, int *out_status)
{
    if (response == NULL || response_capacity == 0U || out_status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    char url[480];
    esp_err_t ret = music_http_build_url(config, path, url, sizeof(url));
    if (ret != ESP_OK)
    {
        return ret;
    }
    return music_http_control_request(control, config, url, method, body,
                                      command_id, response, response_capacity,
                                      out_status);
}

static const char *music_http_mode_text(music_service_mode_t mode)
{
    switch (mode)
    {
    case MUSIC_SERVICE_MODE_REPEAT_ONE:
        return "repeat_one";
    case MUSIC_SERVICE_MODE_SHUFFLE:
        return "shuffle";
    case MUSIC_SERVICE_MODE_ORDER:
        return "order";
    case MUSIC_SERVICE_MODE_SMART:
        return "smart";
    case MUSIC_SERVICE_MODE_REPEAT_ALL:
    default:
        return "repeat_all";
    }
}

static const char *music_http_state_text(music_service_state_t state)
{
    switch (state)
    {
    case MUSIC_SERVICE_STATE_BUFFERING:
        return "buffering";
    case MUSIC_SERVICE_STATE_PLAYING:
        return "playing";
    case MUSIC_SERVICE_STATE_PAUSED:
        return "paused";
    case MUSIC_SERVICE_STATE_ERROR:
        return "error";
    case MUSIC_SERVICE_STATE_STOPPED:
    default:
        return "stopped";
    }
}

esp_err_t music_http_client_poll_remote_command(
    music_http_control_client_t *control,
    const music_http_client_config_t *config,
    music_service_remote_command_t *out_command)
{
    if (control == NULL || config == NULL || out_command == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_command, 0, sizeof(*out_command));
    char *response = heap_caps_calloc(1U, kResponseBytes,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (response == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    int status = 0;
    esp_err_t ret = music_http_perform_raw(
        control, config, "/v1/music/remote-commands/next", HTTP_METHOD_GET,
        NULL, NULL, response, kResponseBytes, &status);
    if (ret == ESP_OK)
    {
        cJSON *root = cJSON_Parse(response);
        if (root == NULL)
        {
            ret = ESP_ERR_INVALID_RESPONSE;
        }
        else
        {
            const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
            const bool claimed = cJSON_IsString(state) &&
                                 strcmp(state->valuestring, "claimed") == 0;
            if (claimed)
            {
                music_http_copy_json_string(
                    root, "command_id", out_command->command_id,
                    sizeof(out_command->command_id));
                music_http_copy_json_string(root, "action", out_command->action,
                                            sizeof(out_command->action));
                const cJSON *expires =
                    cJSON_GetObjectItemCaseSensitive(root, "expires_at");
                if (cJSON_IsNumber(expires) && expires->valuedouble >= 0.0)
                {
                    out_command->expires_at_ms = (uint64_t)expires->valuedouble;
                }
                const cJSON *payload =
                    cJSON_GetObjectItemCaseSensitive(root, "payload");
                if (cJSON_IsObject(payload))
                {
                    music_http_copy_json_string(
                        payload, "source_id", out_command->source_id,
                        sizeof(out_command->source_id));
                    music_http_copy_json_string(
                        payload, "track_id", out_command->track_id,
                        sizeof(out_command->track_id));
                    const cJSON *mode =
                        cJSON_GetObjectItemCaseSensitive(payload, "mode");
                    if (cJSON_IsString(mode) && mode->valuestring != NULL)
                    {
                        out_command->mode = music_http_parse_mode(mode);
                        out_command->has_mode = true;
                    }
                    const cJSON *volume =
                        cJSON_GetObjectItemCaseSensitive(payload, "volume");
                    if (cJSON_IsNumber(volume) && volume->valuedouble >= 0.0 &&
                        volume->valuedouble <= 100.0 &&
                        volume->valuedouble == (double)(int)volume->valuedouble)
                    {
                        out_command->volume = (int)volume->valuedouble;
                        out_command->has_volume = true;
                    }
                }
                if (out_command->command_id[0] == '\0' ||
                    out_command->action[0] == '\0')
                {
                    ret = ESP_ERR_INVALID_RESPONSE;
                }
                else
                {
                    out_command->available = true;
                }
            }
            else if (!cJSON_IsString(state) ||
                     strcmp(state->valuestring, "idle") != 0)
            {
                ret = ESP_ERR_INVALID_RESPONSE;
            }
            cJSON_Delete(root);
        }
    }
    heap_caps_free(response);
    return ret;
}

esp_err_t music_http_client_ack_remote_command(
    music_http_control_client_t *control,
    const music_http_client_config_t *config, const char *command_id,
    const char *state, const char *error_code,
    const music_service_snapshot_t *snapshot)
{
    if (control == NULL || config == NULL || command_id == NULL ||
        command_id[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *result = cJSON_CreateObject();
    cJSON *snapshot_json = cJSON_CreateObject();
    if (root == NULL || result == NULL || snapshot_json == NULL)
    {
        cJSON_Delete(root);
        cJSON_Delete(result);
        cJSON_Delete(snapshot_json);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(result, "state",
                            state != NULL && state[0] != '\0' ? state
                                                               : "executed");
    if (error_code != NULL && error_code[0] != '\0')
    {
        cJSON_AddStringToObject(result, "error_code", error_code);
    }
    cJSON_AddItemToObject(root, "result", result);
    if (snapshot != NULL)
    {
        cJSON_AddStringToObject(snapshot_json, "state",
                                music_http_state_text(snapshot->state));
        cJSON_AddStringToObject(snapshot_json, "mode",
                                music_http_mode_text(snapshot->mode));
        cJSON_AddNumberToObject(snapshot_json, "position_ms",
                                snapshot->position_ms);
        cJSON_AddNumberToObject(snapshot_json, "volume", snapshot->volume);
        if (snapshot->music_session_id[0] != '\0')
            cJSON_AddStringToObject(snapshot_json, "music_session_id",
                                    snapshot->music_session_id);
        if (snapshot->source_id[0] != '\0')
            cJSON_AddStringToObject(snapshot_json, "source_id",
                                    snapshot->source_id);
        if (snapshot->track_id[0] != '\0')
            cJSON_AddStringToObject(snapshot_json, "track_id",
                                    snapshot->track_id);
        if (snapshot->title[0] != '\0')
            cJSON_AddStringToObject(snapshot_json, "title", snapshot->title);
        if (snapshot->artist[0] != '\0')
            cJSON_AddStringToObject(snapshot_json, "artist", snapshot->artist);
    }
    cJSON_AddItemToObject(root, "snapshot", snapshot_json);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    char path[256];
    const int written = snprintf(
        path, sizeof(path), "/v1/music/remote-commands/%s/ack", command_id);
    if (written <= 0 || (size_t)written >= sizeof(path))
    {
        cJSON_free(body);
        return ESP_ERR_INVALID_SIZE;
    }
    char *response = heap_caps_calloc(1U, kResponseBytes,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (response == NULL)
    {
        cJSON_free(body);
        return ESP_ERR_NO_MEM;
    }
    int status = 0;
    esp_err_t ret = music_http_perform_raw(
        control, config, path, HTTP_METHOD_POST, body, NULL, response,
        kResponseBytes, &status);
    if (ret == ESP_OK && status >= 200 && status < 300)
    {
        cJSON *ack = cJSON_Parse(response);
        const cJSON *ack_state = ack != NULL
                                     ? cJSON_GetObjectItemCaseSensitive(ack, "state")
                                     : NULL;
        ret = cJSON_IsString(ack_state) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
        cJSON_Delete(ack);
    }
    heap_caps_free(response);
    cJSON_free(body);
    return ret;
}

static music_service_account_state_t music_http_parse_account_state(
    const cJSON *item)
{
    if (!cJSON_IsString(item) || item->valuestring == NULL)
    {
        return MUSIC_SERVICE_ACCOUNT_ERROR;
    }
    if (strcmp(item->valuestring, "logged_in") == 0)
        return MUSIC_SERVICE_ACCOUNT_LOGGED_IN;
    if (strcmp(item->valuestring, "logged_out") == 0)
        return MUSIC_SERVICE_ACCOUNT_LOGGED_OUT;
    if (strcmp(item->valuestring, "qr_pending") == 0)
        return MUSIC_SERVICE_ACCOUNT_QR_PENDING;
    if (strcmp(item->valuestring, "qr_confirming") == 0)
        return MUSIC_SERVICE_ACCOUNT_QR_CONFIRMING;
    if (strcmp(item->valuestring, "expired") == 0)
        return MUSIC_SERVICE_ACCOUNT_EXPIRED;
    return MUSIC_SERVICE_ACCOUNT_ERROR;
}

static esp_err_t music_http_parse_account(const char *payload, int status,
                                          music_http_account_result_t *result)
{
    cJSON *root = cJSON_Parse(payload);
    if (root == NULL)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    result->http_status = status;
    result->state = music_http_parse_account_state(
        cJSON_GetObjectItemCaseSensitive(root, "state"));
    const cJSON *expires = cJSON_GetObjectItemCaseSensitive(root, "expires_at");
    if (cJSON_IsNumber(expires) && expires->valuedouble >= 0.0)
    {
        result->expires_at_ms = (uint64_t)expires->valuedouble;
    }
    music_http_copy_json_string(root, "login_id", result->login_id,
                                sizeof(result->login_id));
    music_http_copy_json_string(root, "error_code", result->error_code,
                                sizeof(result->error_code));
    const cJSON *qr = cJSON_GetObjectItemCaseSensitive(root, "qr");
    const cJSON *size = cJSON_IsObject(qr)
                            ? cJSON_GetObjectItemCaseSensitive(qr, "size")
                            : NULL;
    const cJSON *data = cJSON_IsObject(qr)
                            ? cJSON_GetObjectItemCaseSensitive(qr, "data")
                            : NULL;
    if (cJSON_IsNumber(size) && size->valuedouble > 0.0 &&
        size->valuedouble <= 255.0 && cJSON_IsString(data) &&
        data->valuestring != NULL && result->qr_data != NULL)
    {
        const uint16_t qr_size = (uint16_t)size->valuedouble;
        /* QR 模块按位打包；拒绝不完整数据，避免 UI 静默绘制截断二维码。 */
        const size_t qr_required_bytes =
            ((size_t)qr_size * (size_t)qr_size + 7U) / 8U;
        if ((double)qr_size != size->valuedouble ||
            qr_required_bytes > result->qr_capacity)
        {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }
        size_t decoded = result->qr_capacity;
        const int ret = mbedtls_base64_decode(
            result->qr_data, result->qr_capacity, &decoded,
            (const unsigned char *)data->valuestring,
            strlen(data->valuestring));
        if (ret != 0 || decoded < qr_required_bytes)
        {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }
        result->qr_size = qr_size;
        result->qr_bytes = decoded;
    }
    cJSON_Delete(root);
    if (result->state == MUSIC_SERVICE_ACCOUNT_ERROR)
    {
        return status == 409 || status == 410 ? ESP_ERR_INVALID_STATE : ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t music_http_request_account(
    const music_http_client_config_t *config, const char *path,
    esp_http_client_method_t method, music_http_account_result_t *result)
{
    if (config == NULL || path == NULL || result == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    char url[480];
    esp_err_t ret = music_http_build_url(config, path, url, sizeof(url));
    if (ret != ESP_OK)
    {
        return ret;
    }
    char *response = heap_caps_calloc(1U, kQrResponseBytes,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (response == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    const uint8_t *qr_data = result->qr_data;
    const size_t qr_capacity = result->qr_capacity;
    memset(result, 0, sizeof(*result));
    result->qr_data = (uint8_t *)qr_data;
    result->qr_capacity = qr_capacity;
    esp_http_client_config_t http_config = {
        .url = url,
        .method = method,
        .timeout_ms = config->timeout_ms > 0U ? (int)config->timeout_ms : 10000,
        .buffer_size = 4096,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (client == NULL)
    {
        heap_caps_free(response);
        return ESP_ERR_NO_MEM;
    }
    ret = music_http_set_auth(client, config->device_token);
    if (ret == ESP_OK && method == HTTP_METHOD_POST)
    {
        ret = esp_http_client_set_header(client, "Content-Type",
                                         "application/json");
    }
    if (ret == ESP_OK)
    {
        ret = esp_http_client_open(client, 0);
    }
    int status = 0;
    if (ret == ESP_OK)
    {
        (void)esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        ret = music_http_read_json_response(client, response,
                                            kQrResponseBytes, NULL);
    }
    if (ret == ESP_OK)
    {
        ret = music_http_parse_account(response, status, result);
    }
    result->http_status = status;
    result->transport_error = ret;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    heap_caps_free(response);
    return ret;
}

esp_err_t music_http_client_get_account(
    const music_http_client_config_t *config,
    music_http_account_result_t *out_result)
{
    return music_http_request_account(config, "/v1/music/account",
                                      HTTP_METHOD_GET, out_result);
}

esp_err_t music_http_client_create_qr(
    const music_http_client_config_t *config,
    music_http_account_result_t *out_result)
{
    return music_http_request_account(config, "/v1/music/account/qr",
                                      HTTP_METHOD_POST, out_result);
}

esp_err_t music_http_client_poll_qr(
    const music_http_client_config_t *config, const char *login_id,
    music_http_account_result_t *out_result)
{
    if (login_id == NULL || login_id[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }
    char path[320];
    const int written = snprintf(path, sizeof(path),
                                 "/v1/music/account/qr/%s", login_id);
    if (written <= 0 || (size_t)written >= sizeof(path))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    return music_http_request_account(config, path, HTTP_METHOD_GET,
                                      out_result);
}

static esp_err_t music_http_parse_catalog(const char *payload, int status,
                                          const char *source_id,
                                          uint32_t offset,
                                          music_service_catalog_snapshot_t *out)
{
    cJSON *root = cJSON_Parse(payload);
    if (root == NULL)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    memset(out, 0, sizeof(*out));
    out->offset = offset;
    snprintf(out->source_id, sizeof(out->source_id), "%s",
             source_id != NULL ? source_id : "");
    const cJSON *total = cJSON_GetObjectItemCaseSensitive(root, "total");
    if (cJSON_IsNumber(total) && total->valuedouble >= 0.0)
    {
        out->total = (uint32_t)total->valuedouble;
    }
    const cJSON *tracks = cJSON_GetObjectItemCaseSensitive(root, "tracks");
    if (cJSON_IsArray(tracks))
    {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, tracks)
        {
            if (out->track_count >= MUSIC_SERVICE_CATALOG_PAGE_SIZE ||
                !cJSON_IsObject(item))
            {
                break;
            }
            music_service_catalog_track_t *track =
                &out->tracks[out->track_count];
            music_http_copy_json_string(
                item, "track_id", track->track_id, sizeof(track->track_id));
            music_http_copy_json_string(item, "title", track->title,
                                        sizeof(track->title));
            music_http_copy_json_string(item, "artist", track->artist,
                                        sizeof(track->artist));
            if (track->track_id[0] != '\0')
            {
                ++out->track_count;
            }
        }
    }
    cJSON_Delete(root);
    out->valid = status >= 200 && status < 300;
    return out->valid ? ESP_OK : ESP_FAIL;
}

static esp_err_t music_http_build_session_body(
    const music_http_client_config_t *config, const char *source_id,
    const char *track_id, const char *mode, char **out_body)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "device_id", config->device_id);
    if (source_id != NULL)
        cJSON_AddStringToObject(root, "source_id", source_id);
    if (track_id != NULL)
        cJSON_AddStringToObject(root, "track_id", track_id);
    if (mode != NULL)
        cJSON_AddStringToObject(root, "mode", mode);
    *out_body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *out_body == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

esp_err_t music_http_client_create_session(
    music_http_control_client_t *control,
    const music_http_client_config_t *config, const char *source_id,
    const char *track_id, const char *command_id,
    music_http_session_result_t *out_result)
{
    return music_http_client_create_session_mode(
        control, config, source_id, track_id, NULL, command_id, out_result);
}

esp_err_t music_http_client_create_session_mode(
    music_http_control_client_t *control,
    const music_http_client_config_t *config, const char *source_id,
    const char *track_id, const char *mode, const char *command_id,
    music_http_session_result_t *out_result)
{
    if (control == NULL || config == NULL || out_result == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    char *body = NULL;
    esp_err_t ret = music_http_build_session_body(config, source_id, track_id,
                                                  mode, &body);
    if (ret == ESP_OK)
    {
        ret = music_http_perform_json(control, config, "/v1/music/sessions",
                                      HTTP_METHOD_POST, body, command_id,
                                      out_result);
    }
    cJSON_free(body);
    return ret;
}

esp_err_t music_http_client_fetch_tracks(
    music_http_control_client_t *control,
    const music_http_client_config_t *config, const char *source_id,
    uint32_t offset, music_service_catalog_snapshot_t *out_catalog)
{
    if (control == NULL || config == NULL || source_id == NULL || source_id[0] == '\0' ||
        out_catalog == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    char path[384];
    const int written = snprintf(
        path, sizeof(path), "/v1/music/sources/%s/tracks?device_id=%s&offset=%lu&limit=%u",
        source_id, config->device_id, (unsigned long)offset,
        (unsigned)MUSIC_SERVICE_CATALOG_PAGE_SIZE);
    if (written <= 0 || (size_t)written >= sizeof(path))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    char url[480];
    esp_err_t ret = music_http_build_url(config, path, url, sizeof(url));
    if (ret != ESP_OK)
    {
        return ret;
    }
    char *response = heap_caps_calloc(1U, kResponseBytes,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (response == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    int status = 0;
    ret = music_http_control_request(control, config, url, HTTP_METHOD_GET,
                                     NULL, NULL, response, kResponseBytes,
                                     &status);
    if (ret == ESP_OK)
    {
        ret = music_http_parse_catalog(response, status, source_id, offset,
                                       out_catalog);
    }
    heap_caps_free(response);
    return ret;
}

esp_err_t music_http_client_session_command(
    music_http_control_client_t *control,
    const music_http_client_config_t *config, const char *session_id,
    const char *action, const char *mode, const char *command_id,
    music_http_session_result_t *out_result)
{
    if (control == NULL || config == NULL || session_id == NULL || session_id[0] == '\0' ||
        action == NULL || action[0] == '\0' || out_result == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(action, "pause") != 0 && strcmp(action, "resume") != 0 &&
        strcmp(action, "previous") != 0 && strcmp(action, "next") != 0 &&
        strcmp(action, "mode") != 0 && strcmp(action, "destroy") != 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    char path[320];
    const int written = strcmp(action, "destroy") == 0
                            ? snprintf(path, sizeof(path),
                                       "/v1/music/sessions/%s?device_id=%s",
                                       session_id, config->device_id)
                            : snprintf(path, sizeof(path),
                                       "/v1/music/sessions/%s/%s?device_id=%s",
                                       session_id, action, config->device_id);
    if (written <= 0 || (size_t)written >= sizeof(path))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    char *body = NULL;
    esp_err_t ret = music_http_build_session_body(config, NULL, NULL, mode,
                                                  &body);
    if (ret == ESP_OK)
    {
        ret = music_http_perform_json(
            control, config, path,
            strcmp(action, "destroy") == 0 ? HTTP_METHOD_DELETE
                                           : HTTP_METHOD_POST,
            body, command_id, out_result);
    }
    cJSON_free(body);
    return ret;
}

esp_err_t music_http_client_open_stream(
    const music_http_client_config_t *config, const char *stream_id,
    music_http_stream_t **out_stream)
{
    if (config == NULL || stream_id == NULL || stream_id[0] == '\0' ||
        out_stream == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_stream = NULL;
    char path[192];
    const int written = snprintf(path, sizeof(path), "/v1/music/streams/%s",
                                 stream_id);
    if (written <= 0 || (size_t)written >= sizeof(path))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    char url[480];
    esp_err_t ret = music_http_build_url(config, path, url, sizeof(url));
    if (ret != ESP_OK)
    {
        return ret;
    }
    music_http_stream_t *stream = heap_caps_calloc(
        1U, sizeof(*stream), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (stream == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    const esp_http_client_config_t http_config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = (int)config->timeout_ms,
        .buffer_size = 1024,
        .keep_alive_enable = false,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = music_http_stream_event_handler,
        .user_data = stream,
    };
    stream->client = esp_http_client_init(&http_config);
    if (stream->client == NULL)
    {
        heap_caps_free(stream);
        return ESP_ERR_NO_MEM;
    }
    ret = esp_http_client_set_header(stream->client, "Accept",
                                     "application/x-watch-opus");
    if (ret == ESP_OK)
    {
        ret = esp_http_client_open(stream->client, 0);
    }
    if (ret == ESP_OK)
    {
        (void)esp_http_client_fetch_headers(stream->client);
        const int status = esp_http_client_get_status_code(stream->client);
        if (status != 200 ||
            strcmp(stream->content_type, "application/x-watch-opus") != 0)
        {
            ESP_LOGW("music_http", "media response rejected: status=%d type=%s",
                     status, stream->content_type[0] == '\0'
                                 ? "(missing)"
                                 : stream->content_type);
            ret = ESP_ERR_INVALID_RESPONSE;
        }
        else
        {
            ret = esp_http_client_set_timeout_ms(stream->client,
                                                 kMediaReadTimeoutMs);
        }
    }
    if (ret != ESP_OK)
    {
        music_http_client_close_stream(stream);
        return ret;
    }
    *out_stream = stream;
    return ESP_OK;
}

esp_err_t music_http_client_read_stream(music_http_stream_t *stream,
                                        uint8_t *buffer, size_t capacity,
                                        size_t *out_bytes)
{
    if (stream == NULL || stream->client == NULL || buffer == NULL ||
        capacity == 0U || out_bytes == NULL || capacity > INT_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_bytes = 0U;
    const int read = esp_http_client_read(stream->client, (char *)buffer,
                                          (int)capacity);
    if (read > 0)
    {
        *out_bytes = (size_t)read;
        return ESP_OK;
    }
    if (read == 0 && esp_http_client_is_complete_data_received(stream->client))
    {
        return ESP_ERR_NOT_FOUND;
    }
    if (read == -ESP_ERR_HTTP_EAGAIN || read == 0)
    {
        return ESP_ERR_HTTP_EAGAIN;
    }
    return ESP_FAIL;
}

void music_http_client_close_stream(music_http_stream_t *stream)
{
    if (stream == NULL)
    {
        return;
    }
    if (stream->client != NULL)
    {
        (void)esp_http_client_close(stream->client);
        esp_http_client_cleanup(stream->client);
    }
    heap_caps_free(stream);
}
