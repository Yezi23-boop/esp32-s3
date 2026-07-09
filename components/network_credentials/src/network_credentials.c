/**
 * @file network_credentials.c
 * @brief recent Wi-Fi 凭据存储组件。
 */

#include "network_credentials.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

/** @brief 组件日志标签。 */
static const char *TAG = "net_creds";

/** @brief recent Wi-Fi 列表的 NVS 命名空间。 */
static const char *kCredentialsNamespace = "net_creds";
/** @brief recent Wi-Fi 列表整体 blob 的 NVS 键名。 */
static const char *kCredentialsBlobKey = "recent_list";

/**
 * @brief recent Wi-Fi 列表的固定大小持久化结构。
 *
 * 使用固定大小数组可以避免在高频读取路径里引入动态内存分配。
 */
typedef struct
{
    uint8_t count; /**< 当前有效条目数，最大为 `NETWORK_CREDENTIALS_MAX_NETWORKS`。 */
    network_credentials_entry_t entries[NETWORK_CREDENTIALS_MAX_NETWORKS]; /**< recent 列表，索引 0 永远表示最新。 */
} network_credentials_storage_t;

/**
 * @brief 组件运行时上下文。
 *
 * 该上下文只负责 recent Wi-Fi 凭据缓存与加载状态，不承载联网或配网控制逻辑。
 */
typedef struct
{
    bool initialized; /**< 是否已经完成基础初始化。 */
    bool loaded; /**< 是否已经从 NVS 成功加载过 recent 列表。 */
    network_credentials_storage_t storage; /**< 当前缓存的 recent Wi-Fi 列表。 */
} network_credentials_runtime_t;

/** @brief 组件单实例运行时。 */
static network_credentials_runtime_t s_runtime = {
    .initialized = false,
    .loaded = false,
    .storage = {0},
};

/** @brief 保护 `s_runtime` 的最小临界区锁。 */
static portMUX_TYPE s_runtime_lock = portMUX_INITIALIZER_UNLOCKED;
/** @brief 串行化 recent 列表读改写事务的静态互斥锁存储。 */
static StaticSemaphore_t s_operation_mutex_buffer;
/** @brief recent 列表操作互斥锁，避免并发保存相互覆盖。 */
static SemaphoreHandle_t s_operation_mutex = NULL;

static esp_err_t network_credentials_ensure_nvs_ready(void);
static esp_err_t network_credentials_ensure_operation_mutex(void);
static esp_err_t network_credentials_load_if_needed_locked(void);
static esp_err_t network_credentials_load_from_nvs(void);
static esp_err_t network_credentials_store_to_nvs(
    const network_credentials_storage_t *storage);
static void network_credentials_copy_entry(network_credentials_entry_t *dst,
                                           const char *ssid,
                                           const char *password);
static bool network_credentials_ssid_matches(
    const network_credentials_entry_t *entry, const char *ssid);
static void network_credentials_get_storage_copy(
    network_credentials_storage_t *storage_copy);
static void network_credentials_set_storage_copy(
    const network_credentials_storage_t *storage_copy);

/**
 * @brief 确保 NVS 基础环境已可用。
 *
 * 这里不区分其他模块是否也在用 NVS，只保证 recent Wi-Fi 存储所需的基础环境存在。
 *
 * @return `ESP_OK` 表示 NVS 可用；其他错误表示初始化或擦除失败。
 */
static esp_err_t network_credentials_ensure_nvs_ready(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS 初始化需要擦除，正在重试");
        ret = nvs_flash_erase();
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "擦除 NVS 失败: %s", esp_err_to_name(ret));
            return ret;
        }
        ret = nvs_flash_init();
    }

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "初始化 NVS 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

/**
 * @brief 确保 recent 列表事务互斥锁已创建。
 *
 * 该互斥锁用来串行化“读当前快照 -> 重排 -> 写回 NVS -> 更新运行态”整条事务，
 * 避免两个成功连接几乎同时写入时互相覆盖。
 *
 * @return `ESP_OK` 表示互斥锁可用；其他错误表示创建失败。
 */
static esp_err_t network_credentials_ensure_operation_mutex(void)
{
    if (s_operation_mutex != NULL)
    {
        return ESP_OK;
    }

    portENTER_CRITICAL(&s_runtime_lock);
    if (s_operation_mutex == NULL)
    {
        s_operation_mutex =
            xSemaphoreCreateMutexStatic(&s_operation_mutex_buffer);
    }
    portEXIT_CRITICAL(&s_runtime_lock);

    return s_operation_mutex != NULL ? ESP_OK : ESP_FAIL;
}

/**
 * @brief 从 NVS 读取 recent Wi-Fi 列表。
 *
 * 若当前还没有存储任何 recent 记录，会把内存态清成空列表并视为成功。
 *
 * @return `ESP_OK` 表示读取完成；其他错误表示 NVS 打开或读取失败。
 */
static esp_err_t network_credentials_load_from_nvs(void)
{
    nvs_handle_t handle = 0;
    network_credentials_storage_t loaded_storage = {0};
    size_t blob_len = sizeof(loaded_storage);
    esp_err_t ret =
        nvs_open(kCredentialsNamespace, NVS_READONLY, &handle);

    if (ret == ESP_ERR_NVS_NOT_FOUND)
    {
        network_credentials_set_storage_copy(&loaded_storage);
        return ESP_OK;
    }
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "打开 recent 凭据命名空间失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_get_blob(handle, kCredentialsBlobKey, &loaded_storage, &blob_len);
    nvs_close(handle);

    if (ret == ESP_ERR_NVS_NOT_FOUND)
    {
        network_credentials_set_storage_copy(&loaded_storage);
        return ESP_OK;
    }
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "读取 recent 凭据失败: %s", esp_err_to_name(ret));
        return ret;
    }

    if (blob_len != sizeof(loaded_storage))
    {
        ESP_LOGW(TAG, "recent 凭据长度异常，按空列表回退");
        memset(&loaded_storage, 0, sizeof(loaded_storage));
    }

    if (loaded_storage.count > NETWORK_CREDENTIALS_MAX_NETWORKS)
    {
        loaded_storage.count = NETWORK_CREDENTIALS_MAX_NETWORKS;
    }

    network_credentials_set_storage_copy(&loaded_storage);
    return ESP_OK;
}

/**
 * @brief 把 recent Wi-Fi 列表整体写回 NVS。
 *
 * 由于列表最多只有 3 条，直接用单个 blob 做原子化更新更容易维持排序一致性。
 *
 * @param[in] storage 目标 recent 列表快照。
 * @return `ESP_OK` 表示持久化成功；其他错误表示 NVS 写入失败。
 */
static esp_err_t network_credentials_store_to_nvs(
    const network_credentials_storage_t *storage)
{
    nvs_handle_t handle = 0;
    esp_err_t ret = ESP_OK;

    if (storage == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ret = nvs_open(kCredentialsNamespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "打开 recent 凭据命名空间失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_blob(handle, kCredentialsBlobKey, storage, sizeof(*storage));
    if (ret == ESP_OK)
    {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "写入 recent 凭据失败: %s", esp_err_to_name(ret));
    }

    return ret;
}

/**
 * @brief 在需要时加载 recent Wi-Fi 列表。
 *
 * 该接口要求调用方已经持有 `s_operation_mutex`，这样 recent 列表的加载与后续
 * 读改写事务可以保持同一串行上下文。
 *
 * @return `ESP_OK` 表示当前缓存已可用；其他错误表示初始化或加载失败。
 */
static esp_err_t network_credentials_load_if_needed_locked(void)
{
    if (s_runtime.loaded)
    {
        return ESP_OK;
    }

    return network_credentials_load_from_nvs();
}

/**
 * @brief 把一条 SSID/密码安全复制到 recent 记录里。
 *
 * 密码允许为空字符串，以兼容开放网络或上层暂未提供密码的场景。
 *
 * @param[out] dst 目标 recent 记录。
 * @param[in] ssid 目标 SSID。
 * @param[in] password 目标密码，可为 `NULL`。
 * @return 无返回值。
 */
static void network_credentials_copy_entry(network_credentials_entry_t *dst,
                                           const char *ssid,
                                           const char *password)
{
    if (dst == NULL)
    {
        return;
    }

    snprintf(dst->ssid, sizeof(dst->ssid), "%s", ssid != NULL ? ssid : "");
    snprintf(dst->password, sizeof(dst->password), "%s",
             password != NULL ? password : "");
}

/**
 * @brief 判断一条 recent 记录是否与目标 SSID 匹配。
 *
 * recent 列表按 SSID 去重；若同一 SSID 的密码变化，则以后写入的新密码覆盖旧值。
 *
 * @param[in] entry 现有 recent 记录。
 * @param[in] ssid 目标 SSID。
 * @return true 表示两者属于同一网络记录。
 */
static bool network_credentials_ssid_matches(
    const network_credentials_entry_t *entry, const char *ssid)
{
    if (entry == NULL || ssid == NULL)
    {
        return false;
    }

    return strncmp(entry->ssid, ssid, sizeof(entry->ssid)) == 0;
}

/**
 * @brief 读取一份 recent 列表快照。
 *
 * 这里统一通过临界区做整体拷贝，避免调用方读到半更新状态。
 *
 * @param[out] storage_copy 输出快照。
 * @return 无返回值。
 */
static void network_credentials_get_storage_copy(
    network_credentials_storage_t *storage_copy)
{
    if (storage_copy == NULL)
    {
        return;
    }

    portENTER_CRITICAL(&s_runtime_lock);
    *storage_copy = s_runtime.storage;
    portEXIT_CRITICAL(&s_runtime_lock);
}

/**
 * @brief 将一份 recent 列表快照写回运行时。
 *
 * @param[in] storage_copy 目标快照。
 * @return 无返回值。
 */
static void network_credentials_set_storage_copy(
    const network_credentials_storage_t *storage_copy)
{
    if (storage_copy == NULL)
    {
        return;
    }

    portENTER_CRITICAL(&s_runtime_lock);
    s_runtime.storage = *storage_copy;
    s_runtime.loaded = true;
    portEXIT_CRITICAL(&s_runtime_lock);
}

/**
 * @brief 初始化 recent Wi-Fi 凭据组件。
 *
 * 该接口只准备 NVS 环境和内存态，不会触发联网、配网或 BLE/AP 逻辑。
 *
 * @return `ESP_OK` 表示初始化成功；其他错误表示 NVS 初始化或加载失败。
 */
esp_err_t network_credentials_init(void)
{
    esp_err_t ret = ESP_OK;

    ret = network_credentials_ensure_operation_mutex();
    if (ret != ESP_OK)
    {
        return ret;
    }

    if (xSemaphoreTake(s_operation_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    portENTER_CRITICAL(&s_runtime_lock);
    if (s_runtime.initialized)
    {
        portEXIT_CRITICAL(&s_runtime_lock);
        xSemaphoreGive(s_operation_mutex);
        return ESP_OK;
    }
    portEXIT_CRITICAL(&s_runtime_lock);

    ret = network_credentials_ensure_nvs_ready();
    if (ret != ESP_OK)
    {
        xSemaphoreGive(s_operation_mutex);
        return ret;
    }

    ret = network_credentials_load_if_needed_locked();
    if (ret != ESP_OK)
    {
        xSemaphoreGive(s_operation_mutex);
        return ret;
    }

    portENTER_CRITICAL(&s_runtime_lock);
    s_runtime.initialized = true;
    portEXIT_CRITICAL(&s_runtime_lock);
    xSemaphoreGive(s_operation_mutex);

    ESP_LOGI(TAG, "recent Wi-Fi credentials initialized");
    return ESP_OK;
}

/**
 * @brief 获取最近一次成功连接的 Wi-Fi 记录。
 *
 * 若当前 recent 列表为空，返回 `ESP_ERR_NOT_FOUND`。
 *
 * @param[out] entry 输出最近一条记录。
 * @return `ESP_OK` 表示成功；`ESP_ERR_NOT_FOUND` 表示当前没有记录；
 *         `ESP_ERR_INVALID_ARG` 表示参数非法；其他错误表示底层加载失败。
 */
esp_err_t network_credentials_get_latest(network_credentials_entry_t *entry)
{
    esp_err_t ret = ESP_OK;

    if (entry == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ret = network_credentials_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    if (xSemaphoreTake(s_operation_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    if (s_runtime.storage.count == 0)
    {
        xSemaphoreGive(s_operation_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    *entry = s_runtime.storage.entries[0];
    xSemaphoreGive(s_operation_mutex);
    return ESP_OK;
}

/**
 * @brief 枚举当前保存的 recent Wi-Fi 列表。
 *
 * 若调用方不需要条目内容，可以传 `entries=NULL` 仅查询当前列表长度。
 *
 * @param[out] entries 输出数组，可为 `NULL`。
 * @param[in] max_entries 调用方可接收的最大条目数。
 * @return 当前真实条目数。
 */
esp_err_t network_credentials_list(network_credentials_entry_t *entries,
                                   size_t max_entries,
                                   size_t *out_count)
{
    size_t copy_count = 0;
    size_t index = 0;
    esp_err_t ret = ESP_OK;

    if (out_count == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ret = network_credentials_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    if (xSemaphoreTake(s_operation_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    *out_count = s_runtime.storage.count;

    if (entries != NULL && max_entries > 0)
    {
        copy_count =
            s_runtime.storage.count < max_entries ? s_runtime.storage.count
                                                  : max_entries;
        for (index = 0; index < copy_count; ++index)
        {
            entries[index] = s_runtime.storage.entries[index];
        }
    }

    xSemaphoreGive(s_operation_mutex);
    return ESP_OK;
}

/**
 * @brief 保存或前移一条 recent Wi-Fi 记录。
 *
 * 该接口应在“连接成功后”调用，而不是在“收到配网凭据”时提前调用。
 *
 * @param[in] ssid 最近成功连接的 SSID。
 * @param[in] password 最近成功连接时使用的密码；可为 `NULL`。
 * @return `ESP_OK` 表示成功；`ESP_ERR_INVALID_ARG` 表示参数非法；其他错误表示持久化失败。
 */
esp_err_t network_credentials_save_or_promote(const char *ssid,
                                              const char *password)
{
    network_credentials_storage_t current_storage = {0};
    network_credentials_storage_t updated_storage = {0};
    size_t source_index = 0;
    uint8_t write_count = 0;
    esp_err_t ret = ESP_OK;

    if (ssid == NULL || ssid[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    ret = network_credentials_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    if (xSemaphoreTake(s_operation_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    current_storage = s_runtime.storage;

    network_credentials_copy_entry(&updated_storage.entries[0], ssid, password);
    updated_storage.count = 1;

    for (source_index = 0;
         source_index < current_storage.count &&
         updated_storage.count < NETWORK_CREDENTIALS_MAX_NETWORKS;
         ++source_index)
    {
        if (network_credentials_ssid_matches(&current_storage.entries[source_index],
                                             ssid))
        {
            continue;
        }

        write_count = updated_storage.count;
        updated_storage.entries[write_count] = current_storage.entries[source_index];
        updated_storage.count++;
    }

    ret = network_credentials_store_to_nvs(&updated_storage);
    if (ret != ESP_OK)
    {
        xSemaphoreGive(s_operation_mutex);
        return ret;
    }

    s_runtime.storage = updated_storage;
    s_runtime.loaded = true;
    xSemaphoreGive(s_operation_mutex);
    ESP_LOGI(TAG, "recent Wi-Fi updated: latest_ssid=%s count=%u", ssid,
             (unsigned)updated_storage.count);
    return ESP_OK;
}
