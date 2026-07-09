#include "hardware_init.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "features/audio/audio_app.h"
#include "sd_manager.h"
#include "audio_codec.h"
#include "board_button.h"
#include "board_ds2413_motor.h"
#include "board_power.h"
#include "resource_fs.h"
#include "i2c_manager.h"

/*
 * 硬件初始化实现说明：
 * - 这里收敛主工程对 NVS、音频、存储、PMIC 和按键的依赖顺序；
 * - 目标是把“开机后必须具备的基础能力”一次性准备好；
 * - 真正的联网状态推进交给 `network_service`，避免初始化阶段长时间阻塞。
 */

static const char *TAG = "HARDWARE_INIT";

/**
 * @brief 打印开机时采集到的第一份板级电源快照。
 * @param[in] state 电源状态快照，可为 NULL。
 * @return 无返回值。
 *
 * 该日志主要用于确认 PMIC 是否已正常工作，以及 UI 初始电量展示是否有可信来源。
 */
static void board_power_log_boot_snapshot(const board_power_state_t *state)
{
    if (state == NULL)
    {
        ESP_LOGW(TAG, "Board power boot snapshot unavailable");
        return;
    }

    if (state->battery_data_valid)
    {
        ESP_LOGI(TAG,
                 "Board power boot snapshot: available=%d stale=%d ext=%d bat=%d chg=%d dchg=%d vbat=%umV vsys=%umV soc=%u%%",
                 state->available, state->snapshot_stale,
                 state->external_power_present, state->battery_present,
                 state->charging, state->discharging, state->battery_mv,
                 state->system_mv, state->battery_percent);
        return;
    }

    ESP_LOGI(TAG,
             "Board power boot snapshot: available=%d stale=%d ext=%d bat=%d chg=%d dchg=%d vbat=%umV vsys=%umV soc=unknown",
             state->available, state->snapshot_stale,
             state->external_power_present, state->battery_present,
             state->charging, state->discharging, state->battery_mv,
             state->system_mv);
}

/**
 * @brief 初始化 NVS。
 * @return `ESP_OK` 表示初始化成功；其他错误表示 NVS 不可用。
 */
static esp_err_t hardware_nvs_init(void)
{
    /* 若因分页耗尽或版本不兼容失败，先擦除后重建 NVS。 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS Flash init failed, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

/**
 * @brief 统一初始化基础硬件能力。
 *
 * 该入口负责准备 NVS、音频资源、通用资源分区、SD、codec、板级电源和按键输入，
 * 但不会阻塞等待 Wi-Fi 真正连通。
 *
 * @return `ESP_OK` 表示基础硬件初始化成功；
 *         其他错误表示关键初始化失败。
 */
esp_err_t hardware_init(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "Initializing NVS...");
    ret = hardware_nvs_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /*
     * DS2413 通过 GPIO18 控制马达；PIOA pull-low 是当前硬件的关闭态。
     * 这里在较慢的 SD/codec 初始化前尽早写默认关闭，失败只影响触觉通道。
     */
    ESP_LOGI(TAG, "Initializing DS2413 Motor...");
    ret = board_ds2413_motor_init();
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "DS2413 motor init failed: %s", esp_err_to_name(ret));
    }

    /* 音频应用层先准备目录和控制入口；即使失败，也不阻断整机启动。 */
    ESP_LOGI(TAG, "Initializing Audio SPIFFS...");
    ret = audio_app_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Audio SPIFFS init failed: %s", esp_err_to_name(ret));
    }

    /*
     * 通用资源分区给 LVGL POSIX `A:` 盘符提供 `/resources` 根目录。
     * 挂载失败只影响运行时字体/图片/音频资源，不阻断基础 UI 启动。
     */
    ESP_LOGI(TAG, "Initializing Resource LittleFS...");
    ret = resource_fs_init();
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Resource LittleFS init failed: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "Initializing SD Card...");
    ret = sd_manager_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SD Card init failed: %s", esp_err_to_name(ret));
    }

    /* codec 初始化失败会影响录音和播报，但不一定要阻断整机其余能力。 */
    ESP_LOGI(TAG, "Initializing Audio Codec...");
    ret = audio_codec_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Audio Codec init failed: %d", ret);
    }
    else
    {
        ESP_LOGI(TAG, "Audio system initialized successfully");
        audio_codec_set_volume(60);
    }

    /* 板级电源快照用于 UI 电量展示和后续功耗策略观察。 */
    ESP_LOGI(TAG, "Initializing Board Power...");
    ret = board_power_init();
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Board power init failed: %s", esp_err_to_name(ret));
    }
    else
    {
        board_power_state_t state = {0};
        ret = board_power_refresh(&state);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Board power boot snapshot refresh failed: %s", esp_err_to_name(ret));
        }
        else
        {
            board_power_log_boot_snapshot(&state);
        }
    }

    /* 网络主链路已迁到后台 `network_service`，这里不再同步初始化旧 `wifi_provision`。 */
    ESP_LOGI(TAG, "Initializing Button...");
    ret = board_button_init();
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Button init failed: %s", esp_err_to_name(ret));
    }
    ESP_LOGI(TAG, "Hardware init complete: network startup deferred to background service");
    return ESP_OK;
}
