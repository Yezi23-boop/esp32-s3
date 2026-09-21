/**
 * @file ble_control.c
 * @brief BLE 总开关控制层，只表达 enabled 偏好与 active 运行态。
 */

#include "ble_control.h"

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

/** @brief BLE 偏好命名空间，沿用网络服务侧已有命名思路。 */
static const char *kBlePrefNamespace = "network_svc";
/** @brief BLE 总开关键。 */
static const char *kBlePrefKey = "ble_enabled";

/** @brief BLE 控制层日志标签。 */
static const char *TAG = "ble_ctrl";

/** @brief BLE 控制层运行态上下文。 */
typedef struct
{
    bool initialized; /**< 是否已完成初始化。 */
    bool enabled; /**< BLE 总开关偏好。 */
    bool active; /**< BLE 当前运行态。 */
} ble_control_runtime_t;

/** @brief 运行态单例。 */
static ble_control_runtime_t s_runtime = {
    .initialized = false,
    .enabled = true,
    .active = false,
};

/** @brief 最小并发保护 mutex 静态存储。 */
static StaticSemaphore_t s_ble_mutex_buffer;
/** @brief 运行态保护 mutex。 */
static SemaphoreHandle_t s_ble_mutex = NULL;
/** @brief 保护 BLE mutex 创建路径的最小临界区锁。 */
static portMUX_TYPE s_ble_bootstrap_lock = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t ble_control_ensure_mutex(void);
static esp_err_t ble_control_ensure_nvs_ready(void);
static esp_err_t ble_control_load_enabled_pref(void);
static esp_err_t ble_control_store_enabled_pref(bool enabled);

/**
 * @brief 确保运行态 mutex 已创建。
 * @return `ESP_OK` 表示 mutex 可用；其他错误表示内存分配失败。
 */
static esp_err_t ble_control_ensure_mutex(void)
{
    if (s_ble_mutex != NULL)
    {
        return ESP_OK;
    }

    taskENTER_CRITICAL(&s_ble_bootstrap_lock);
    if (s_ble_mutex == NULL)
    {
        s_ble_mutex = xSemaphoreCreateMutexStatic(&s_ble_mutex_buffer);
    }
    taskEXIT_CRITICAL(&s_ble_bootstrap_lock);

    if (s_ble_mutex == NULL)
    {
        ESP_LOGE(TAG, "创建 BLE mutex 失败");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief 确保 NVS 可用。
 * @return `ESP_OK` 表示 NVS 可用；其他错误表示初始化失败。
 */
static esp_err_t ble_control_ensure_nvs_ready(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
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
    }
    return ret;
}

/**
 * @brief 从 NVS 读取 BLE enabled 偏好。
 * @return `ESP_OK` 表示读取成功或回退到默认值；其他错误表示读取失败。
 */
static esp_err_t ble_control_load_enabled_pref(void)
{
    nvs_handle_t nvs_handle = 0;
    uint8_t enabled_raw = 1;
    esp_err_t ret = nvs_open(kBlePrefNamespace, NVS_READONLY, &nvs_handle);

    if (ret == ESP_ERR_NVS_NOT_FOUND)
    {
        s_runtime.enabled = true;
        return ESP_OK;
    }
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "打开 BLE 偏好命名空间失败: %s", esp_err_to_name(ret));
        s_runtime.enabled = true;
        return ret;
    }

    ret = nvs_get_u8(nvs_handle, kBlePrefKey, &enabled_raw);
    if (ret == ESP_ERR_NVS_NOT_FOUND)
    {
        s_runtime.enabled = true;
        ret = ESP_OK;
    }
    else if (ret == ESP_OK)
    {
        s_runtime.enabled = (enabled_raw != 0U);
    }
    else
    {
        ESP_LOGW(TAG, "读取 BLE 偏好失败: %s", esp_err_to_name(ret));
        s_runtime.enabled = true;
    }

    nvs_close(nvs_handle);
    return ret;
}

/**
 * @brief 将 BLE enabled 偏好写入 NVS。
 * @param[in] enabled 目标偏好。
 * @return `ESP_OK` 表示写入成功；其他错误表示写入失败。
 */
static esp_err_t ble_control_store_enabled_pref(bool enabled)
{
    nvs_handle_t nvs_handle = 0;
    esp_err_t ret = nvs_open(kBlePrefNamespace, NVS_READWRITE, &nvs_handle);

    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "打开 BLE 偏好命名空间失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_u8(nvs_handle, kBlePrefKey, enabled ? 1U : 0U);
    if (ret == ESP_OK)
    {
        ret = nvs_commit(nvs_handle);
    }

    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "保存 BLE 偏好失败: %s", esp_err_to_name(ret));
    }

    nvs_close(nvs_handle);
    return ret;
}

/**
 * @brief 初始化 BLE 总开关控制层。
 *
 * 这里只准备 NVS 偏好和运行态保护，不会启动或停止 BLE。
 *
 * @return `ESP_OK` 表示初始化成功；其他错误表示 NVS 或 mutex 准备失败。
 */
esp_err_t ble_control_init(void)
{
    esp_err_t ret = ESP_OK;

    ret = ble_control_ensure_mutex();
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = xSemaphoreTake(s_ble_mutex, portMAX_DELAY) == pdTRUE ? ESP_OK
                                                                : ESP_FAIL;
    if (ret != ESP_OK)
    {
        return ret;
    }

    if (s_runtime.initialized)
    {
        xSemaphoreGive(s_ble_mutex);
        return ESP_OK;
    }

    ret = ble_control_ensure_nvs_ready();
    if (ret != ESP_OK)
    {
        xSemaphoreGive(s_ble_mutex);
        return ret;
    }

    ret = ble_control_load_enabled_pref();
    if (ret == ESP_OK)
    {
        s_runtime.initialized = true;
        s_runtime.active = false;
    }

    xSemaphoreGive(s_ble_mutex);
    return ret;
}

/**
 * @brief 设置 BLE 总开关偏好。
 *
 * 该接口只持久化 enabled 偏好，不会启动或停止 BLE。
 *
 * @param[in] enabled true 表示允许 BLE；false 表示关闭 BLE。
 * @return `ESP_OK` 表示写入成功；其他错误表示 NVS 写入失败。
 */
esp_err_t ble_control_set_enabled(bool enabled)
{
    esp_err_t ret = ble_control_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    if (xSemaphoreTake(s_ble_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    ret = ble_control_store_enabled_pref(enabled);
    if (ret == ESP_OK)
    {
        s_runtime.enabled = enabled;
    }
    xSemaphoreGive(s_ble_mutex);
    return ret;
}

/**
 * @brief 查询 BLE 总开关偏好。
 * @return true 表示允许 BLE。
 */
bool ble_control_is_enabled(void)
{
    bool enabled = true;

    if (ble_control_init() != ESP_OK)
    {
        return true;
    }

    if (xSemaphoreTake(s_ble_mutex, portMAX_DELAY) != pdTRUE)
    {
        return true;
    }

    enabled = s_runtime.enabled;
    xSemaphoreGive(s_ble_mutex);
    return enabled;
}

/**
 * @brief 设置 BLE 当前运行态。
 *
 * 该接口只表达运行态，不做任何 BLE 启停。
 *
 * @param[in] active true 表示 BLE 当前活跃。
 * @return `ESP_OK` 表示状态更新成功；其他错误表示内部状态未就绪。
 */
esp_err_t ble_control_set_active(bool active)
{
    esp_err_t ret = ble_control_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    if (xSemaphoreTake(s_ble_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    s_runtime.active = active;
    xSemaphoreGive(s_ble_mutex);
    return ESP_OK;
}

/**
 * @brief 查询 BLE 当前运行态。
 * @return true 表示 BLE 当前活跃。
 */
bool ble_control_is_active(void)
{
    bool active = false;

    if (ble_control_init() != ESP_OK)
    {
        return false;
    }

    if (xSemaphoreTake(s_ble_mutex, portMAX_DELAY) != pdTRUE)
    {
        return false;
    }

    active = s_runtime.active;
    xSemaphoreGive(s_ble_mutex);
    return active;
}
