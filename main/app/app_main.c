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
#include "services/weather/weather_service.h"
#include "features/alerts/app_alert_manager.h"
#include "ui/lvgl_task.h"
#include "hardware_init.h"
#include "services/network/network_service.h"
#include "services/memory_watch/memory_watch_service.h"
#include "services/memory_watch/watch_endpoint_service.h"
#include "services/music/music_service.h"
#include "services/official_chat_service.h"
#include "services/power/power_service.h"
#include "services/power/power_policy.h"
#include "services/power/sleep_coordinator.h"
#include "services/power/wakeup_evidence_service.h"
#include "services/time/system_time_service.h"
#include "services/sensors/imu_service.h"
#include "services/fall_detection_service.h"
#include "services/runtime/runtime_coordinator.h"
#include "services/runtime/safety_monitor_policy.h"
#include "services/ota/ota_service.h"
#include "services/ota/ota_boot_check.h"
#include "services/runtime/startup_readiness.h"

static const char *TAG = "MAIN";
/* 栈缩为 6144B：高压实测 free=4996B（61% 空闲），缩 2KB PSRAM 仍有余量。 */
static const uint32_t kTimeWeatherTaskStackBytes = 6144;
/* 默认关闭只控制启动策略；运行时仍可直接调用两个 service 的 start/destroy API。 */
static const bool kMotionServicesEnabledByDefault = false;

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

    /* audio bridge 在 policy task 启动前注册：要求 audio codec 已初始化
     * （board_foundation 阶段完成），保证首次预算能读到初始音频事实。 */
    if (power_policy_audio_bridge_register() != ESP_OK)
    {
        ESP_LOGW(TAG, "Power policy audio bridge register failed");
    }

    /* Safety 的省电参与者注册同样必须在 power_policy task 启动前完成，
     * 与 audio bridge 同一时机；真实 Safety 运行仍由 start_service_managers 启动。 */
    if (safety_monitor_policy_register_power_participant() != ESP_OK)
    {
        ESP_LOGW(TAG, "Safety monitor power participant register failed");
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

    if (runtime_coordinator_start() != ESP_OK)
    {
        ESP_LOGW(TAG, "Runtime coordinator start failed");
        return;
    }

    /* Safety policy 只托管危险识别开关，跨 owner 交接由 coordinator 负责。 */
    if (safety_monitor_policy_start() != ESP_OK)
    {
        ESP_LOGW(TAG, "Safety monitor policy start failed");
        return;
    }

    if (ota_service_start() != ESP_OK)
    {
        ESP_LOGW(TAG, "OTA service start failed");
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

    if (music_service_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "Music service init failed");
    }
    else
    {
        ESP_LOGI(TAG, "boot_stage: music_service_ready");
    }

    if (watch_endpoint_service_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "Watch endpoint service init failed");
    }
    else
    {
        ESP_LOGI(TAG, "boot_stage: watch_endpoint_ready");
    }

    /* 默认关闭 IMU/Fall 后台链路；运行时仍可通过 service API 动态调整。 */
    if (kMotionServicesEnabledByDefault)
    {
        if (imu_service_start() != ESP_OK)
        {
            ESP_LOGW(TAG, "IMU service start failed");
        }
        else
        {
            ESP_LOGI(TAG, "boot_stage: imu_service_ready");
        }

        if (fall_detection_service_start() != ESP_OK)
        {
            ESP_LOGW(TAG, "Fall detection service start failed");
        }
        else
        {
            ESP_LOGI(TAG, "boot_stage: fall_detection_ready");
        }
    }

    else
    {
        /* 用正常生命周期 API 保证默认关闭与动态销毁走同一条路径。 */
        (void)fall_detection_service_destroy();
        (void)imu_service_destroy();
        ESP_LOGI(TAG, "boot_stage: imu_service_disabled_by_default");
        ESP_LOGI(TAG, "boot_stage: fall_detection_disabled_by_default");
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

    if (ota_boot_check_run() != ESP_OK)
    {
        ESP_LOGE(TAG, "OTA boot check failed; startup halted");
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
