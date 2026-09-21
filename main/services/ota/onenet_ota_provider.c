#include "services/ota/onenet_ota_provider.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "nvs.h"

static const char *TAG = "onenet_ota";

enum
{
    kAuthorizationMax = 256,
    kResponseMax = 4096,
    kApiBaseMax = 160,
    kModuleVersionMax = 16,
    kTokenTtlSeconds = 3600,
};

static const char *kApiBase = "https://iot-api.heclouds.com/fuse-ota";
static const char *kNvsNamespace = "onenet_ota";
static const char *kPendingTaskIdKey = "pending_tid";
static const char *kPendingTargetKey = "pending_target";
static const char *kModuleVersion = "1.0.0";
/* 用户明确允许产品级 AccessKey 编译进固件；轮换时同步仓库运维文件。 */
static const char *kProductId = "w23kT21Z3x";
static const char *kDeviceName = "watch-001";
static const char *kAccessKey = "09d3LfWlh20jU2c/7QcDbZlPB98ZkUXrjYkTz5OXHTE=";

static bool onenet_ota_md5_is_hex(const char *value)
{
    if (value == NULL || strlen(value) != ONENET_OTA_MD5_HEX_LEN)
    {
        return false;
    }
    for (size_t index = 0U; index < ONENET_OTA_MD5_HEX_LEN; ++index)
    {
        if (!isxdigit((unsigned char)value[index]))
        {
            return false;
        }
    }
    return true;
}

static char onenet_ota_hex_digit(unsigned int value)
{
    return value < 10U ? (char)('0' + value) : (char)('A' + value - 10U);
}

static esp_err_t onenet_ota_url_encode(const char *input, char *output,
                                       size_t output_size)
{
    size_t used = 0U;
    for (const unsigned char *cursor = (const unsigned char *)input;
         *cursor != '\0'; ++cursor)
    {
        const bool safe = isalnum(*cursor) || *cursor == '-' ||
                          *cursor == '_' || *cursor == '.' || *cursor == '~';
        const size_t needed = safe ? 1U : 3U;
        if (used + needed + 1U > output_size)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        if (safe)
        {
            output[used++] = (char)*cursor;
        }
        else
        {
            output[used++] = '%';
            output[used++] = onenet_ota_hex_digit(*cursor >> 4U);
            output[used++] = onenet_ota_hex_digit(*cursor & 0x0FU);
        }
    }
    output[used] = '\0';
    return ESP_OK;
}

static esp_err_t onenet_ota_build_authorization(char output[kAuthorizationMax])
{
    unsigned char decoded_key[64] = {0};
    size_t decoded_length = 0U;
    int ret = mbedtls_base64_decode(decoded_key, sizeof(decoded_key),
                                    &decoded_length,
                                    (const unsigned char *)kAccessKey,
                                    strlen(kAccessKey));
    if (ret != 0 || decoded_length == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const time_t now = time(NULL);
    if (now < 1600000000)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const time_t expire_at = now + kTokenTtlSeconds;
    char resource[ONENET_OTA_PRODUCT_ID_MAX + 10U] = {0};
    char encoded_resource[sizeof(resource) * 3U] = {0};
    char raw_sign[64] = {0};
    char encoded_sign[96] = {0};
    snprintf(resource, sizeof(resource), "products/%s",
             kProductId);
    if (onenet_ota_url_encode(resource, encoded_resource,
                              sizeof(encoded_resource)) != ESP_OK)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    char signature_source[256] = {0};
    const int source_length = snprintf(
        signature_source, sizeof(signature_source),
        "%ld\nsha256\n%s\n2022-05-01", (long)expire_at, resource);
    if (source_length <= 0 || (size_t)source_length >= sizeof(signature_source))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    unsigned char digest[32] = {0};
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info == NULL ||
        mbedtls_md_hmac(md_info, decoded_key, decoded_length,
                        (const unsigned char *)signature_source,
                        (size_t)source_length, digest) != 0)
    {
        return ESP_FAIL;
    }
    size_t encoded_length = 0U;
    ret = mbedtls_base64_encode((unsigned char *)raw_sign,
                                sizeof(raw_sign) - 1U, &encoded_length,
                                digest, sizeof(digest));
    if (ret != 0 || encoded_length >= sizeof(raw_sign))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    raw_sign[encoded_length] = '\0';
    if (onenet_ota_url_encode(raw_sign, encoded_sign,
                              sizeof(encoded_sign)) != ESP_OK)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    const int auth_length = snprintf(
        output, kAuthorizationMax,
        "version=2022-05-01&res=%s&et=%ld&method=sha256&sign=%s",
        encoded_resource, (long)expire_at, encoded_sign);
    return auth_length > 0 && auth_length < kAuthorizationMax
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

static esp_err_t onenet_ota_build_url(const char *suffix, char *output,
                                      size_t output_size)
{
    const int length = snprintf(output, output_size, "%s/%s/%s/%s", kApiBase,
                                kProductId,
                                kDeviceName, suffix);
    return length > 0 && (size_t)length < output_size ? ESP_OK
                                                       : ESP_ERR_INVALID_SIZE;
}

static esp_err_t onenet_ota_http_json(const char *url, const char *authorization,
                                      esp_http_client_method_t method,
                                      const char *body, char *response,
                                      size_t response_size, int *status_code)
{
    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_set_method(client, method);
    if (err == ESP_OK)
    {
        err = esp_http_client_set_header(client, "Authorization", authorization);
    }
    if (err == ESP_OK)
    {
        err = esp_http_client_set_header(client, "Content-Type",
                                         "application/json");
    }
    const size_t body_length = body == NULL ? 0U : strlen(body);
    if (err == ESP_OK)
    {
        err = esp_http_client_open(client, body_length);
    }
    if (err == ESP_OK && body_length > 0U)
    {
        err = esp_http_client_write(client, body, body_length) ==
                      (int)body_length
                  ? ESP_OK
                  : ESP_FAIL;
    }
    if (err == ESP_OK)
    {
        const int content_length = esp_http_client_fetch_headers(client);
        if (content_length < 0 || (size_t)content_length >= response_size)
        {
            err = ESP_ERR_INVALID_SIZE;
        }
        else
        {
            int received = 0;
            while ((size_t)received < (size_t)content_length)
            {
                const int chunk = esp_http_client_read(
                    client, response + received,
                    (int)(response_size - (size_t)received - 1U));
                if (chunk <= 0)
                {
                    err = ESP_ERR_HTTP_EAGAIN;
                    break;
                }
                received += chunk;
            }
            response[received] = '\0';
        }
    }
    if (status_code != NULL)
    {
        *status_code = esp_http_client_get_status_code(client);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t onenet_ota_parse_code(const char *response, int *out_code,
                                       char *out_message,
                                       size_t message_size, cJSON **out_root)
{
    cJSON *root = cJSON_ParseWithLength(response, strlen(response));
    if (root == NULL)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "msg");
    if (!cJSON_IsNumber(code) || !cJSON_IsString(message))
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out_code = code->valueint;
    snprintf(out_message, message_size, "%s", message->valuestring);
    *out_root = root;
    return ESP_OK;
}

esp_err_t onenet_ota_provider_report_version(const char *app_version)
{
    if (app_version == NULL || app_version[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_OK;

    char authorization[kAuthorizationMax] = {0};
    err = onenet_ota_build_authorization(authorization);
    if (err != ESP_OK)
    {
        return err;
    }
    char url[kApiBaseMax] = {0};
    err = onenet_ota_build_url("version", url, sizeof(url));
    if (err != ESP_OK)
    {
        return err;
    }
    char body[128] = {0};
    snprintf(body, sizeof(body), "{\"s_version\":\"%s\",\"f_version\":\"%s\"}",
             app_version, kModuleVersion);
    char response[kResponseMax] = {0};
    int status_code = 0;
    err = onenet_ota_http_json(url, authorization, HTTP_METHOD_POST, body,
                               response, sizeof(response), &status_code);
    if (err != ESP_OK || status_code != 200)
    {
        return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
    }
    int code = -1;
    char message[96] = {0};
    cJSON *root = NULL;
    err = onenet_ota_parse_code(response, &code, message, sizeof(message), &root);
    cJSON_Delete(root);
    if (err != ESP_OK || code != 0)
    {
        ESP_LOGW(TAG, "version report rejected: code=%d msg=%s", code, message);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t onenet_ota_provider_check(const char *current_version,
                                    onenet_ota_task_t *out_task)
{
    if (current_version == NULL || current_version[0] == '\0' ||
        out_task == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_OK;
    char authorization[kAuthorizationMax] = {0};
    err = onenet_ota_build_authorization(authorization);
    if (err != ESP_OK)
    {
        return err;
    }
    char url[kApiBaseMax + 64U] = {0};
    char suffix[96] = {0};
    snprintf(suffix, sizeof(suffix), "check?type=2&version=%s", current_version);
    err = onenet_ota_build_url(suffix, url, sizeof(url));
    if (err != ESP_OK)
    {
        return err;
    }
    char response[kResponseMax] = {0};
    int status_code = 0;
    err = onenet_ota_http_json(url, authorization, HTTP_METHOD_GET, NULL,
                               response, sizeof(response), &status_code);
    if (err != ESP_OK || status_code != 200)
    {
        return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
    }
    int code = -1;
    char message[96] = {0};
    cJSON *root = NULL;
    err = onenet_ota_parse_code(response, &code, message, sizeof(message), &root);
    if (err != ESP_OK)
    {
        return err;
    }
    if (code == 12012)
    {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }
    if (code != 0)
    {
        ESP_LOGW(TAG, "check rejected: code=%d msg=%s", code, message);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON *target = data == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(data, "target");
    cJSON *task_id = data == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(data, "tid");
    cJSON *size = data == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(data, "size");
    cJSON *md5 = data == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(data, "md5");
    cJSON *status = data == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(data, "status");
    cJSON *package_type = data == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(data, "type");
    const bool valid = cJSON_IsString(target) && cJSON_IsNumber(task_id) &&
                       cJSON_IsNumber(size) && cJSON_IsString(md5) &&
                       cJSON_IsNumber(status) && cJSON_IsNumber(package_type) &&
                       strlen(target->valuestring) < ONENET_OTA_TARGET_VERSION_MAX &&
                       size->valuedouble > 0.0 &&
                       onenet_ota_md5_is_hex(md5->valuestring);
    if (!valid)
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    memset(out_task, 0, sizeof(*out_task));
    snprintf(out_task->target, sizeof(out_task->target), "%s", target->valuestring);
    out_task->task_id = (uint32_t)task_id->valuedouble;
    out_task->size = (size_t)size->valuedouble;
    snprintf(out_task->md5, sizeof(out_task->md5), "%s", md5->valuestring);
    out_task->status = status->valueint;
    out_task->package_type = package_type->valueint;
    cJSON_Delete(root);
    return out_task->package_type == 1 ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t onenet_ota_provider_check_plan(const char *current_version,
                                         ota_update_plan_t *out_plan)
{
    if (current_version == NULL || out_plan == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // OneNET 以设备上报的当前版本作为任务匹配依据；协议动作属于 provider，
    // 不应让 ota_service 重新理解 OneNET 的 version 接口。
    esp_err_t ret = onenet_ota_provider_report_version(current_version);
    if (ret != ESP_OK)
    {
        return ret;
    }

    onenet_ota_task_t task = {0};
    ret = onenet_ota_provider_check(current_version, &task);
    if (ret != ESP_OK)
    {
        return ret;
    }

    memset(out_plan, 0, sizeof(*out_plan));
    out_plan->source = OTA_UPDATE_SOURCE_ONENET;
    out_plan->provider_task_id = task.task_id;
    out_plan->size = task.size;
    out_plan->checksum_type = OTA_UPDATE_CHECKSUM_MD5;
    out_plan->use_cert_bundle = true;
    snprintf(out_plan->version, sizeof(out_plan->version), "%s",
             task.target);
    snprintf(out_plan->md5, sizeof(out_plan->md5), "%s", task.md5);
    return ESP_OK;
}

esp_err_t onenet_ota_provider_prepare_download(
    const onenet_ota_task_t *task, char *out_url, size_t url_size,
    char *out_authorization, size_t authorization_size)
{
    if (task == NULL || task->task_id == 0U || task->size == 0U ||
        task->package_type != 1 || out_url == NULL || url_size == 0U ||
        out_authorization == NULL || authorization_size == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_OK;

    char suffix[48] = {0};
    const int suffix_length =
        snprintf(suffix, sizeof(suffix), "%lu/download",
                 (unsigned long)task->task_id);
    if (suffix_length <= 0 || (size_t)suffix_length >= sizeof(suffix))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    char url[ONENET_OTA_DOWNLOAD_URL_MAX] = {0};
    err = onenet_ota_build_url(suffix, url, sizeof(url));
    if (err != ESP_OK || strlen(url) + 1U > url_size)
    {
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }
    char authorization[kAuthorizationMax] = {0};
    err = onenet_ota_build_authorization(authorization);
    if (err != ESP_OK || strlen(authorization) + 1U > authorization_size)
    {
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }
    strcpy(out_url, url);
    strcpy(out_authorization, authorization);
    return ESP_OK;
}

esp_err_t onenet_ota_provider_prepare_plan(ota_update_plan_t *plan)
{
    if (plan == NULL || plan->source != OTA_UPDATE_SOURCE_ONENET ||
        plan->provider_task_id == 0U || plan->size == 0U ||
        plan->version[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    onenet_ota_task_t task = {0};
    task.task_id = plan->provider_task_id;
    task.size = plan->size;
    task.package_type = 1;
    snprintf(task.target, sizeof(task.target), "%s", plan->version);
    snprintf(task.md5, sizeof(task.md5), "%s", plan->md5);
    const esp_err_t ret = onenet_ota_provider_prepare_download(
        &task, plan->url, sizeof(plan->url), plan->authorization,
        sizeof(plan->authorization));
    if (ret == ESP_OK)
    {
        plan->use_cert_bundle = true;
    }
    return ret;
}

esp_err_t onenet_ota_provider_store_pending(const onenet_ota_task_t *task)
{
    if (task == NULL || task->task_id == 0U || task->target[0] == '\0' ||
        strlen(task->target) >= ONENET_OTA_TARGET_VERSION_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK)
    {
        return ret;
    }
    ret = nvs_set_u32(handle, kPendingTaskIdKey, task->task_id);
    if (ret == ESP_OK)
    {
        ret = nvs_set_str(handle, kPendingTargetKey, task->target);
    }
    if (ret == ESP_OK)
    {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

esp_err_t onenet_ota_provider_store_pending_plan(
    const ota_update_plan_t *plan)
{
    if (plan == NULL || plan->source != OTA_UPDATE_SOURCE_ONENET ||
        plan->provider_task_id == 0U || plan->version[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    onenet_ota_task_t task = {0};
    task.task_id = plan->provider_task_id;
    snprintf(task.target, sizeof(task.target), "%s", plan->version);
    return onenet_ota_provider_store_pending(&task);
}

esp_err_t onenet_ota_provider_load_pending(onenet_ota_pending_t *out_pending)
{
    if (out_pending == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (ret != ESP_OK)
    {
        return ret;
    }
    memset(out_pending, 0, sizeof(*out_pending));
    ret = nvs_get_u32(handle, kPendingTaskIdKey, &out_pending->task_id);
    if (ret == ESP_OK)
    {
        size_t length = sizeof(out_pending->target);
        ret = nvs_get_str(handle, kPendingTargetKey, out_pending->target,
                          &length);
    }
    nvs_close(handle);
    if (ret != ESP_OK || out_pending->task_id == 0U ||
        out_pending->target[0] == '\0' ||
        strlen(out_pending->target) >= sizeof(out_pending->target))
    {
        return ret == ESP_OK ? ESP_ERR_INVALID_RESPONSE : ret;
    }
    return ESP_OK;
}

esp_err_t onenet_ota_provider_clear_pending(void)
{
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK)
    {
        return ret;
    }
    ret = nvs_erase_key(handle, kPendingTaskIdKey);
    if (ret == ESP_ERR_NVS_NOT_FOUND)
    {
        ret = ESP_OK;
    }
    if (ret == ESP_OK)
    {
        ret = nvs_erase_key(handle, kPendingTargetKey);
        if (ret == ESP_ERR_NVS_NOT_FOUND)
        {
            ret = ESP_OK;
        }
    }
    if (ret == ESP_OK)
    {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

static bool onenet_ota_status_step_is_valid(int step)
{
    /* 0-100 是进度；101-107 是下载结果；201-208 是升级结果。 */
    return (step >= 0 && step <= 100) ||
           (step >= 101 && step <= 107) ||
           (step >= 201 && step <= 208);
}

esp_err_t onenet_ota_provider_report_status(uint32_t task_id, int step)
{
    if (task_id == 0U || !onenet_ota_status_step_is_valid(step))
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_OK;
    char authorization[kAuthorizationMax] = {0};
    err = onenet_ota_build_authorization(authorization);
    if (err != ESP_OK)
    {
        return err;
    }
    char suffix[48] = {0};
    const int suffix_length =
        snprintf(suffix, sizeof(suffix), "%lu/status", (unsigned long)task_id);
    if (suffix_length <= 0 || (size_t)suffix_length >= sizeof(suffix))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    char url[ONENET_OTA_DOWNLOAD_URL_MAX] = {0};
    err = onenet_ota_build_url(suffix, url, sizeof(url));
    if (err != ESP_OK)
    {
        return err;
    }
    char body[32] = {0};
    snprintf(body, sizeof(body), "{\"step\":%d}", step);
    char response[kResponseMax] = {0};
    int status_code = 0;
    err = onenet_ota_http_json(url, authorization, HTTP_METHOD_POST, body,
                               response, sizeof(response), &status_code);
    if (err != ESP_OK || status_code != 200)
    {
        return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
    }
    int code = -1;
    char message[96] = {0};
    cJSON *root = NULL;
    err = onenet_ota_parse_code(response, &code, message, sizeof(message), &root);
    cJSON_Delete(root);
    /* OneNET returns code=20 when the task reaches the terminal step=100;
     * this is a successful completion response, not a provider error. */
    return err != ESP_OK
               ? err
               : (code == 0 || (step == 100 && code == 20)
                      ? ESP_OK
                      : ESP_ERR_INVALID_RESPONSE);
}
