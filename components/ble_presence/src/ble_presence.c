/**
 * @file ble_presence.c
 * @brief 主界面蓝牙开关对应的普通 BLE 可发现广播。
 */

#include "ble_presence.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

/** @brief 组件日志标签。 */
static const char *TAG = "ble_presence";
/** @brief 普通蓝牙可发现名称，保持和当前设备命名风格一致，方便手机侧识别。 */
static const char *kBlePresenceDeviceName = "ESP32S3-723C";
/** @brief 等待 NimBLE host task 退出的最长时间，单位毫秒。 */
static const uint32_t kBlePresenceStopTimeoutMs = 1000;
/** @brief 等待 host task 自删除完成的让步时间，单位毫秒。 */
static const uint32_t kBlePresenceTaskExitGraceMs = 30;
/** @brief 启动 BLE controller 前预留的最小 internal 8-bit heap，避免底层大块申请失败后触发 controller assert。 */
static const size_t kBlePresenceMinInternalFreeBytes = 64U * 1024U;
/** @brief BLE controller 初始化期间已观察到约 30 KiB 连续块申请，largest block 必须额外留出余量。 */
static const size_t kBlePresenceMinInternalLargestBlockBytes = 40U * 1024U;

/**
 * @brief `ble_presence` 单实例运行态。
 *
 * 该组件只允许一个 owner：主界面蓝牙开关。官方 BLE provisioning 启动前必须先
 * stop 本组件，避免两个模块同时写 `ble_hs_cfg` 或同时持有 advertising。
 */
typedef struct
{
    bool initialized;  /**< 已经成功初始化 NimBLE host。 */
    bool host_synced;  /**< NimBLE host 已完成 sync，此后才允许从普通任务重试 GAP advertising。 */
    bool advertising;  /**< 当前是否已启动 GAP advertising。 */
    bool stop_pending; /**< stop 正在进行，用于阻止 sync 回调重新拉起广播。 */
    uint8_t own_addr_type; /**< NimBLE 自动推断出的本机地址类型。 */
} ble_presence_runtime_t;

/** @brief 运行态 mutex 静态存储。 */
static StaticSemaphore_t s_presence_mutex_buffer;
/** @brief 串行化 start/stop/query 与 NimBLE 回调的 mutex。 */
static SemaphoreHandle_t s_presence_mutex = NULL;
/** @brief 保护 mutex 首次创建路径的临界区锁。 */
static portMUX_TYPE s_presence_bootstrap_lock = portMUX_INITIALIZER_UNLOCKED;
/** @brief host task 退出通知的静态 semaphore 存储。 */
static StaticSemaphore_t s_stop_done_buffer;
/** @brief `nimble_port_run()` 返回后由 host task 释放，供 stop 等待。 */
static SemaphoreHandle_t s_stop_done = NULL;
/** @brief 单实例运行态。 */
static ble_presence_runtime_t s_runtime = {0};

static esp_err_t ble_presence_ensure_primitives(void);
static esp_err_t ble_presence_check_internal_heap(void);
static esp_err_t ble_presence_start_advertising(void);
static void ble_presence_on_sync(void);
static void ble_presence_on_reset(int reason);
static int ble_presence_gap_event(struct ble_gap_event *event, void *arg);
static void ble_presence_host_task(void *param);

/**
 * @brief 确保 mutex 和 stop semaphore 已创建。
 *
 * 这两个同步对象使用静态内存，避免 BLE 开关路径反复动态分配，也便于在内存紧张
 * 时保持行为可预测。
 *
 * @return `ESP_OK` 表示同步原语可用；其他错误表示创建失败。
 */
static esp_err_t ble_presence_ensure_primitives(void)
{
    if (s_presence_mutex != NULL && s_stop_done != NULL)
    {
        return ESP_OK;
    }

    taskENTER_CRITICAL(&s_presence_bootstrap_lock);
    if (s_presence_mutex == NULL)
    {
        s_presence_mutex =
            xSemaphoreCreateMutexStatic(&s_presence_mutex_buffer);
    }
    if (s_stop_done == NULL)
    {
        s_stop_done = xSemaphoreCreateBinaryStatic(&s_stop_done_buffer);
    }
    taskEXIT_CRITICAL(&s_presence_bootstrap_lock);

    return (s_presence_mutex != NULL && s_stop_done != NULL) ? ESP_OK
                                                             : ESP_FAIL;
}

/**
 * @brief 检查 NimBLE controller 初始化前的 internal heap 余量。
 *
 * `nimble_port_init()` 内部会进入 BT controller 初始化。实机日志已观察到
 * `BLE_INIT: Malloc failed` 后 controller 在 `emi.c` assert，因此这里在进入
 * controller 路径前先看 internal 8-bit heap 的总量和最大连续块，内存不足时
 * 直接把失败返回给 UI，而不是让底层 panic。
 *
 * @return `ESP_OK` 表示可以尝试初始化；`ESP_ERR_NO_MEM` 表示当前内存不足。
 */
static esp_err_t ble_presence_check_internal_heap(void)
{
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t free_bytes = heap_caps_get_free_size(caps);
    const size_t largest_block = heap_caps_get_largest_free_block(caps);

    if (free_bytes < kBlePresenceMinInternalFreeBytes ||
        largest_block < kBlePresenceMinInternalLargestBlockBytes)
    {
        ESP_LOGW(TAG,
                 "BLE presence start skipped: internal heap low free=%u largest=%u min_free=%u min_largest=%u",
                 (unsigned)free_bytes, (unsigned)largest_block,
                 (unsigned)kBlePresenceMinInternalFreeBytes,
                 (unsigned)kBlePresenceMinInternalLargestBlockBytes);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/**
 * @brief 在 NimBLE host 已同步后启动 GAP advertising。
 *
 * advertising 只放设备名和标准 discoverable flags，不注册 Wi-Fi 配网 service。
 * 这样手机/小程序扫描能看到设备，但不会误以为它已经进入 provisioning 会话。
 *
 * @return `ESP_OK` 表示 advertising 已启动；其他错误表示 NimBLE GAP 调用失败。
 */
static esp_err_t ble_presence_start_advertising(void)
{
    struct ble_hs_adv_fields fields = {0};
    struct ble_gap_adv_params adv_params = {0};
    int rc = 0;

    if (xSemaphoreTake(s_presence_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }
    if (s_runtime.stop_pending)
    {
        xSemaphoreGive(s_presence_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_presence_mutex);

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (uint8_t *)kBlePresenceDeviceName;
    fields.name_len = strlen(kBlePresenceDeviceName);
    fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "设置 BLE presence 广播字段失败: rc=%d", rc);
        return ESP_FAIL;
    }

    /* 采用 non-connectable 广播：满足“能被扫描发现”的需求，同时避免手机系统
     * 误连后占用唯一 BLE connection，从而影响后续小程序 provisioning。 */
    adv_params.conn_mode = BLE_GAP_CONN_MODE_NON;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_runtime.own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_presence_gap_event, NULL);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "启动 BLE presence 广播失败: rc=%d", rc);
        if (xSemaphoreTake(s_presence_mutex, portMAX_DELAY) == pdTRUE)
        {
            s_runtime.advertising = false;
            xSemaphoreGive(s_presence_mutex);
        }
        return ESP_FAIL;
    }

    if (xSemaphoreTake(s_presence_mutex, portMAX_DELAY) == pdTRUE)
    {
        s_runtime.advertising = true;
        xSemaphoreGive(s_presence_mutex);
    }

    ESP_LOGI(TAG, "BLE presence advertising: %s", kBlePresenceDeviceName);
    ESP_LOGI(TAG, "BLE adv internal_free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

/**
 * @brief NimBLE host 同步完成回调。
 *
 * NimBLE 地址和 advertising 必须在 host sync 之后启动；过早调用 GAP API 会失败。
 *
 * @return 无返回值。
 */
static void ble_presence_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "准备 BLE 地址失败: rc=%d", rc);
        return;
    }

    if (xSemaphoreTake(s_presence_mutex, portMAX_DELAY) != pdTRUE)
    {
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_runtime.own_addr_type);
    if (rc == 0)
    {
        s_runtime.host_synced = true;
    }
    xSemaphoreGive(s_presence_mutex);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "推断 BLE 地址类型失败: rc=%d", rc);
        return;
    }

    (void)ble_presence_start_advertising();
}

/**
 * @brief NimBLE host reset 回调。
 *
 * reset 多数来自底层控制器或 host 异常，记录原因即可；实际恢复由上层开关或下一次
 * start/stop 重新收敛。
 *
 * @param[in] reason NimBLE reset 原因码。
 * @return 无返回值。
 */
static void ble_presence_on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE presence host reset: reason=%d", reason);
}

/**
 * @brief 处理 BLE presence 的 GAP 事件。
 *
 * 当前只关心 advertising 非预期结束：如果不是 stop 流程触发，则尝试重新广播，
 * 让主界面蓝牙开关保持“打开后持续可发现”的语义。
 *
 * @param[in] event GAP 事件。
 * @param[in] arg 未使用。
 * @return 固定返回 0，表示事件已处理。
 */
static int ble_presence_gap_event(struct ble_gap_event *event, void *arg)
{
    bool should_restart = false;

    (void)arg;
    if (event == NULL)
    {
        return 0;
    }

    if (event->type == BLE_GAP_EVENT_ADV_COMPLETE)
    {
        if (xSemaphoreTake(s_presence_mutex, portMAX_DELAY) == pdTRUE)
        {
            s_runtime.advertising = false;
            should_restart =
                s_runtime.initialized && !s_runtime.stop_pending;
            xSemaphoreGive(s_presence_mutex);
        }

        if (should_restart)
        {
            (void)ble_presence_start_advertising();
        }
    }

    return 0;
}

/**
 * @brief NimBLE host task 入口。
 *
 * `nimble_port_run()` 只会在 `nimble_port_stop()` 后返回。这里先通知 stop 等待方，
 * 再调用 FreeRTOS 适配层 deinit 删除 host task，和 ESP-IDF NimBLE 示例保持一致。
 *
 * @param[in] param 未使用。
 * @return 无返回值。
 */
static void ble_presence_host_task(void *param)
{
    (void)param;

    ESP_LOGI(TAG, "BLE presence host task started");
    nimble_port_run();

    if (s_stop_done != NULL)
    {
        xSemaphoreGive(s_stop_done);
    }
    nimble_port_freertos_deinit();
}

/**
 * @brief 启动普通 BLE 可发现广播。
 *
 * 该函数初始化 NimBLE host 并在 host sync 后发布 non-connectable 广播，供手机或
 * 小程序扫描发现设备。它不注册 provisioning service，因此不会替代 Wi-Fi 页面里的
 * `BLE Provision` 入口。
 *
 * @return `ESP_OK` 表示启动成功或已在运行；其他错误表示同步原语或 NimBLE 初始化失败。
 *
 * @note 可阻塞，只能在任务上下文调用。
 */
esp_err_t ble_presence_start(void)
{
    esp_err_t ret = ble_presence_ensure_primitives();
    if (ret != ESP_OK)
    {
        return ret;
    }

    if (xSemaphoreTake(s_presence_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }
    if (s_runtime.initialized)
    {
        const bool can_retry_advertising =
            s_runtime.host_synced && !s_runtime.advertising &&
            !s_runtime.stop_pending;
        xSemaphoreGive(s_presence_mutex);
        if (can_retry_advertising)
        {
            return ble_presence_start_advertising();
        }
        return ESP_OK;
    }

    while (xSemaphoreTake(s_stop_done, 0) == pdTRUE)
    {
    }

    ret = ble_presence_check_internal_heap();
    if (ret != ESP_OK)
    {
        xSemaphoreGive(s_presence_mutex);
        return ret;
    }

    memset(&s_runtime, 0, sizeof(s_runtime));
    s_runtime.initialized = true;
    xSemaphoreGive(s_presence_mutex);

    ret = nimble_port_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "初始化 NimBLE presence 失败: %s",
                 esp_err_to_name(ret));
        if (xSemaphoreTake(s_presence_mutex, portMAX_DELAY) == pdTRUE)
        {
            memset(&s_runtime, 0, sizeof(s_runtime));
            xSemaphoreGive(s_presence_mutex);
        }
        return ret;
    }

    ble_hs_cfg.reset_cb = ble_presence_on_reset;
    ble_hs_cfg.sync_cb = ble_presence_on_sync;
    ble_svc_gap_device_name_set(kBlePresenceDeviceName);
    nimble_port_freertos_init(ble_presence_host_task);

    return ESP_OK;
}

/**
 * @brief 停止普通 BLE 可发现广播并释放 NimBLE host。
 *
 * 该函数会先停止 advertising，再停止 host task，最后 deinit NimBLE host。这样官方
 * BLE provisioning adapter 可以在后续重新成为 BLE owner。
 *
 * @return `ESP_OK` 表示停止成功或本就空闲；其他错误表示 stop/deinit 过程失败。
 *
 * @note 可阻塞，只能在任务上下文调用。
 */
esp_err_t ble_presence_stop(void)
{
    bool was_initialized = false;
    bool was_advertising = false;
    int rc = 0;

    esp_err_t ret = ble_presence_ensure_primitives();
    if (ret != ESP_OK)
    {
        return ret;
    }

    if (xSemaphoreTake(s_presence_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }
    was_initialized = s_runtime.initialized;
    was_advertising = s_runtime.advertising;
    if (!was_initialized)
    {
        xSemaphoreGive(s_presence_mutex);
        return ESP_OK;
    }
    s_runtime.stop_pending = true;
    s_runtime.advertising = false;
    xSemaphoreGive(s_presence_mutex);

    if (was_advertising)
    {
        rc = ble_gap_adv_stop();
        if (rc != 0 && rc != BLE_HS_EALREADY)
        {
            ESP_LOGW(TAG, "停止 BLE presence 广播返回异常: rc=%d", rc);
        }
    }

    rc = nimble_port_stop();
    if (rc != 0)
    {
        ESP_LOGE(TAG, "停止 NimBLE presence host 失败: rc=%d", rc);
        return ESP_FAIL;
    }

    if (xSemaphoreTake(s_stop_done,
                       pdMS_TO_TICKS(kBlePresenceStopTimeoutMs)) != pdTRUE)
    {
        ESP_LOGE(TAG, "等待 BLE presence host task 退出超时，保留 runtime 以便重试停止");
        return ESP_ERR_TIMEOUT;
    }
    vTaskDelay(pdMS_TO_TICKS(kBlePresenceTaskExitGraceMs));
    nimble_port_deinit();

    if (xSemaphoreTake(s_presence_mutex, portMAX_DELAY) == pdTRUE)
    {
        memset(&s_runtime, 0, sizeof(s_runtime));
        xSemaphoreGive(s_presence_mutex);
    }

    ESP_LOGI(TAG, "BLE presence stopped");
    return ESP_OK;
}

/**
 * @brief 查询普通 BLE presence 是否正在广播。
 *
 * @return true 表示普通可发现广播已经成功启动。
 */
bool ble_presence_is_active(void)
{
    bool active = false;

    if (ble_presence_ensure_primitives() != ESP_OK)
    {
        return false;
    }
    if (xSemaphoreTake(s_presence_mutex, portMAX_DELAY) != pdTRUE)
    {
        return false;
    }

    active = s_runtime.advertising;
    xSemaphoreGive(s_presence_mutex);
    return active;
}
