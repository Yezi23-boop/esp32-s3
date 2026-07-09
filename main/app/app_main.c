#include <stdbool.h>
#include <stdio.h>
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "ble_control.h"
#include "ap_portal_adapter.h"
#include "gui_guider.h"
#include "events_init.h"
#include "esp_timer.h"
#include "esp_freertos_hooks.h"
#include "features/weather/time_weather.h"
#include "features/alerts/app_alert_manager.h"
#include "ui/lvgl_task.h"
#include "hardware_init.h"
#include "services/network_service.h"
#include "services/memory_watch_service.h"
#include "services/watch_endpoint_service.h"
#include "services/official_chat_service.h"
#include "services/power_service.h"
#include "services/power_policy.h"
#include "services/sleep_coordinator.h"
#include "services/wakeup_evidence_service.h"
#include "services/system_time_service.h"
#include "services/imu_service.h"
#include "services/fall_detection_service.h"
#include "services/background_service_manager.h"
#include "services/startup_readiness.h"

static const char *TAG = "MAIN";
/* 栈缩为 6144B：高压实测 free=4996B（61% 空闲），缩 2KB PSRAM 仍有余量。 */
static const uint32_t kTimeWeatherTaskStackBytes = 6144;

/**
 * @brief AP 门户保存 AI Memory Watch endpoint 配置的桥接回调。
 *
 * `ap_portal_adapter` 只负责解析 SoftAP 门户请求；真正的 endpoint NVS
 * 持久化仍由 `memory_watch_service` 这个 owner 完成，避免组件反向依赖
 * `main/services`。该路径只处理 watch device token，不接收 Hermes/API/MiMo key。
 */
static esp_err_t app_memory_watch_portal_config_cb(
    const ap_portal_memory_watch_config_t *portal_config, void *user_ctx)
{
    (void)user_ctx;

    if (portal_config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const memory_watch_service_endpoint_config_t service_config = {
        .base_url = portal_config->base_url,
        .device_id = portal_config->device_id,
        .device_token = portal_config->device_token,
        .timeout_ms = portal_config->timeout_ms,
        .allow_insecure_http = portal_config->allow_insecure_http,
    };
    return memory_watch_service_save_endpoint_to_nvs(&service_config);
}

/**
 * @brief AP 门户查询 AI Memory Watch endpoint 是否已配置的桥接回调。
 *
 * 只返回布尔状态，不返回配置内容；`/api/status` 用它填充
 * `memory_watch_endpoint_configured` 字段。
 */
static bool app_memory_watch_portal_configured_cb(void *user_ctx)
{
    (void)user_ctx;
    return memory_watch_service_is_endpoint_configured();
}

/*
 * 应用主入口说明：
 * - `app_main()` 只负责系统级启动编排，不承载长期业务循环；
 * - 启动顺序遵循"先硬件基础设施，再 UI，再后台服务"的原则；
 * - 这样可以保证即便联网或聊天服务尚未就绪，设备也能尽快进入可交互状态。
 */

/*
 * 任务句柄说明：
 * - lvgl_task_handle: UI 主任务，负责 LVGL 渲染与事件处理。
 * - lvgl_time_handle: 时间天气任务句柄，负责低频拉取天气快照。
 */
TaskHandle_t lvgl_task_handle = NULL; // UI 主任务句柄，仅启动阶段写入。
TaskHandle_t lvgl_time_handle = NULL; // 时间天气任务句柄；由后台服务阶段创建。

/**
 * @brief 启动 Board Foundation 阶段。
 * @return true 表示板级基础能力已可继续启动后续阶段。
 */
static bool start_board_foundation(void)
{
    esp_err_t ret = hardware_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "boot_stage: board_foundation_failed err=%s",
                 esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(TAG, "boot_stage: board_foundation_done");
    return true;
}

/**
 * @brief 创建 Display/UI 前台任务。
 *
 * 这里只负责创建 `lvgl_task`；Display Foundation 和 UI First Frame
 * 的真正 ready 日志由 UI 任务在对应边界打印。
 *
 * @return true 表示 UI 任务已创建，后续阶段可以继续启动。
 */
static bool start_display_and_ui(void)
{
    if (startup_readiness_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "boot_stage: startup_readiness_failed");
        return false;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(lvgl_task, "lvgl_task", 1024 * 10,
                                            NULL, 6, &lvgl_task_handle, 1);
    if (ok != pdPASS)
    {
        ESP_LOGE(TAG, "boot_stage: ui_task_create_failed");
        return false;
    }

    ESP_LOGI(TAG, "boot_stage: ui_task_created");
    return true;
}

/**
 * @brief 启动电源观测和整机资源预算层。
 */
static void start_core_policy(void)
{
    if (power_service_init() != ESP_OK)
    {
        ESP_LOGW(TAG, "Power service init failed");
    }
    else if (power_service_start() != ESP_OK)
    {
        ESP_LOGW(TAG, "Power service start failed");
    }

    if (power_policy_start() != ESP_OK)
    {
        ESP_LOGW(TAG, "Power policy start failed");
        return;
    }

    if (sleep_coordinator_start() != ESP_OK)
    {
        ESP_LOGW(TAG, "Sleep coordinator start failed");
    }

    if (wakeup_evidence_service_start() != ESP_OK)
    {
        ESP_LOGW(TAG, "Wakeup evidence service start failed");
    }

    if (system_time_service_start() != ESP_OK)
    {
        ESP_LOGW(TAG, "System time service start failed");
    }

    ESP_LOGI(TAG, "boot_stage: policy_ready");
}

/**
 * @brief 启动系统级后台功能开关层。
 */
static void start_service_managers(void)
{
    if (app_alert_manager_init() != ESP_OK)
    {
        ESP_LOGW(TAG, "App alert manager init failed");
    }

    /*
     * 后台服务管理器是系统级功能开关层。第一阶段先托管危险识别，
     * 让它脱离"进入专页才运行、离开专页就停止"的页面生命周期。
     */
    if (background_service_manager_start() != ESP_OK)
    {
        ESP_LOGW(TAG, "Background service manager start failed");
        return;
    }

    ESP_LOGI(TAG, "boot_stage: managers_ready");
}

/**
 * @brief 启动可延后的后台服务入口。
 */
static void start_deferred_services(void)
{
    /*
     * 天气任务会执行 HTTPS/TLS 和 cJSON 解析，4KB 栈在证书校验路径上已出现
     * stack overflow，因此按网络型后台任务预留 8KB 栈。
     */
    /* 
     * 提前在 SRAM 任务上下文中完成 ble_control 初始化，以防 time 任务（在 PSRAM 栈）
     * 首次调用 network_manager_get_state 时隐式触发 NVS 读写引发 Cache-disabled 断言崩溃。
     */
    (void)ble_control_init();

    BaseType_t time_task_ok = xTaskCreatePinnedToCoreWithCaps(
        time_and_weather, "time", kTimeWeatherTaskStackBytes, NULL, 5,
        &lvgl_time_handle, 0, MALLOC_CAP_SPIRAM);
    if (time_task_ok != pdPASS)
    {
        ESP_LOGW(TAG, "Time/weather service task create failed");
    }

    // BLE/AP/自动联网都由网络服务层统一调度，主入口不直接处理配网细节。
    if (ap_portal_adapter_set_memory_watch_config_callback(
            app_memory_watch_portal_config_cb, NULL) != ESP_OK)
    {
        ESP_LOGW(TAG, "Memory watch portal config callback register failed");
    }

    if (ap_portal_adapter_set_memory_watch_configured_callback(
            app_memory_watch_portal_configured_cb, NULL) != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "Memory watch portal configured callback register failed");
    }

    if (network_service_start() != ESP_OK)
    {
        ESP_LOGE(TAG, "Background network service start failed");
    }
    else
    {
        ESP_LOGI(TAG, "boot_stage: network_service_ready");
    }

    // 聊天服务只初始化后台任务；真正拉起会话仍取决于网络和前台页面意图。
    if (official_chat_service_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "Official chat service init failed");
    }
    else
    {
        ESP_LOGI(TAG, "boot_stage: official_chat_ready");
    }

    /*
     * AI Memory Watch 只初始化 owner task。Hermes 地址、设备 token 和
     * 录音上传都由独立页面/配置入口按用户意图触发，启动期不主动连服务器。
     */
    if (memory_watch_service_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "Memory watch service init failed");
    }
    else
    {
        ESP_LOGI(TAG, "boot_stage: memory_watch_ready");
    }

    if (watch_endpoint_service_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "Watch endpoint service init failed");
    }
    else
    {
        ESP_LOGI(TAG, "boot_stage: watch_endpoint_ready");
    }

    /*
     * IMU service 当前做统一配置、GPIO21 ISR 事件计数和 50Hz 周期采样。
     * 让它随 deferred services 启动，便于开机日志直接验证 IMU 链路与采样稳定性。
     */
    if (imu_service_start() != ESP_OK)
    {
        ESP_LOGW(TAG, "IMU service start failed");
    }
    else
    {
        ESP_LOGI(TAG, "boot_stage: imu_service_ready");
    }

    /*
     * Fall detection v1 只消费 IMU service 的完整 4 秒窗口并输出日志。
     * 它不直接读取 IMU 硬件，也不触发 UI 或告警策略。
     */
    if (fall_detection_service_start() != ESP_OK)
    {
        ESP_LOGW(TAG, "Fall detection service start failed");
    }
    else
    {
        ESP_LOGI(TAG, "boot_stage: fall_detection_ready");
    }
}

/**
 * @brief 应用程序主入口。
 *
 * 该入口只做系统级启动编排，不承载长期业务循环。
 * 启动顺序遵循"先硬件基础能力，再 UI，再后台服务"，
 * 这样即使联网链路暂未就绪，设备也能尽快进入可交互状态。
 *
 * @return 无返回值。
 *
 * @note 运行在 ESP-IDF 应用主任务上下文中；若基础硬件初始化失败，后续任务不会继续创建。
 */
void app_main(void)
{
    ESP_LOGI(TAG, "boot_stage: app_start");

    if (!start_board_foundation())
    {
        ESP_LOGE(TAG, "Hardware init failed, halting system");
        // 这里保留停机/重启分支作为后续容错入口，避免半初始化系统继续运行。
        // esp_restart();
        return;
    }

    if (!start_display_and_ui())
    {
        ESP_LOGE(TAG, "UI task create failed, halting startup sequence");
        return;
    }

    start_core_policy();
    start_service_managers();
    start_deferred_services();

    ESP_LOGI(TAG, "boot_stage: startup_sequence_done");
}
