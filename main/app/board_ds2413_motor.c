#include "app/board_ds2413_motor.h"

#include "driver/gpio.h"
#include "ds2413.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "onewire_bus.h"
#include "onewire_bus_impl_rmt.h"

static const char *TAG = "board_ds2413_motor";

// GPIO18 上只有一个 1-Wire bus 实例；初始化成功后由本模块持有到关机。
static onewire_bus_handle_t s_bus;
// ROM 地址来自启动期枚举结果；后续写 PIO 时必须 MATCH ROM 避免误操作混挂设备。
static ds2413_device_t s_device;
// DS2413 事务包含 reset / match / read-write，多任务同时访问会破坏 1-Wire 时序。
static SemaphoreHandle_t s_lock;
// 只表示 bus + ROM + 默认关断已完成；false 时任何马达开关请求都应失败。
static bool s_initialized;

// PIOB 当前不用，保持 release 避免无意拉低未接功能脚；PIOA pull-low 才是本板马达关闭态。
static const ds2413_latch_state_t k_motor_off_latch = {
    .pioa_release = false,
    .piob_release = true,
};

// PIOA release 后由板上 R9 / Q1 驱动级让马达开启；调用方必须控制开启时长。
static const ds2413_latch_state_t k_motor_on_latch = {
    .pioa_release = true,
    .piob_release = true,
};

static esp_err_t board_ds2413_motor_write_locked(
    const ds2413_latch_state_t *latch_state, ds2413_state_t *out_state)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "DS2413 motor is not initialized");
    return ds2413_write_latch(&s_device, latch_state, out_state);
}

static void board_ds2413_motor_cleanup_bus(void)
{
    if (s_bus != NULL)
    {
        esp_err_t ret = onewire_bus_del(s_bus);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "delete 1-Wire bus failed: %s",
                     esp_err_to_name(ret));
        }
        s_bus = NULL;
    }
    s_initialized = false;
}

static esp_err_t board_ds2413_motor_new_bus_rmt(
    onewire_bus_handle_t *out_bus)
{
    onewire_bus_config_t bus_cfg = {
        .bus_gpio_num = GPIO_NUM_18,
        .flags = {
            .en_pull_up = false, // GPIO18 已有外部 R22 4.7k 上拉；再开内部上拉会改变总线等效阻值。
        },
    };
    onewire_bus_rmt_config_t rmt_cfg = {
        .max_rx_bytes = 10, // DS2413 ROM 8 字节 + 状态/ACK 字节足够，避免为单器件总线预留过大缓冲。
    };
    return onewire_new_bus_rmt(&bus_cfg, &rmt_cfg, out_bus);
}

static esp_err_t board_ds2413_motor_probe_bus(const char *backend_name)
{
    esp_err_t ret = ds2413_find_first(s_bus, &s_device);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "%s backend did not find DS2413: %s", backend_name,
                 esp_err_to_name(ret));
        return ret;
    }

    const uint8_t *rom = (const uint8_t *)&s_device.address;
    ESP_LOGI(TAG, "DS2413 ROM via %s: %02X %02X %02X %02X %02X %02X %02X %02X",
             backend_name, rom[0], rom[1], rom[2], rom[3], rom[4], rom[5],
             rom[6], rom[7]);
    return ESP_OK;
}

esp_err_t board_ds2413_motor_init(void)
{
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG,
                            "create motor mutex failed");
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_initialized)
    {
        ds2413_state_t state = {0};
        esp_err_t ret =
            board_ds2413_motor_write_locked(&k_motor_off_latch, &state);
        xSemaphoreGive(s_lock);
        return ret;
    }

    gpio_reset_pin(GPIO_NUM_18);
    gpio_set_direction(GPIO_NUM_18, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_18, GPIO_FLOATING);
    // reset 后给外部上拉一个短稳定窗口；若总线仍为低电平，继续枚举可能被误判为器件无响应。
    vTaskDelay(pdMS_TO_TICKS(2));
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_FALSE(gpio_get_level(GPIO_NUM_18) == 1, ESP_ERR_INVALID_STATE,
                      fail, TAG, "GPIO18 1-Wire bus is held low");

    // 当前主线只保留 RMT backend；不保留串口兜底，避免占用串口资源并减少隐藏初始化路径。
    ret = board_ds2413_motor_new_bus_rmt(&s_bus);
    if (ret == ESP_OK)
    {
        ret = board_ds2413_motor_probe_bus("RMT");
        if (ret != ESP_OK)
        {
            board_ds2413_motor_cleanup_bus();
        }
    }

    ESP_GOTO_ON_ERROR(ret, fail, TAG, "probe DS2413 on RMT failed");

    // 先标记 initialized，才能复用统一的 write_locked 校验路径写入默认关闭态。
    s_initialized = true;
    ds2413_state_t state = {0};
    ret = board_ds2413_motor_write_locked(&k_motor_off_latch, &state);
    ESP_GOTO_ON_ERROR(ret, fail, TAG, "set motor default off failed");
    ESP_LOGI(TAG, "DS2413 motor default off: raw=0x%02X PIOA(state=%d latch=%d)",
             state.raw, state.pioa_state, state.pioa_latch);

    xSemaphoreGive(s_lock);
    return ESP_OK;

fail:
    // 初始化中途失败时释放半创建的 bus，下一次 init 可重新从干净状态探测。
    board_ds2413_motor_cleanup_bus();
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t board_ds2413_motor_set_enabled(bool enabled)
{
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "DS2413 motor lock is not created");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    ds2413_state_t state = {0};
    // enabled=true 对应 open-drain release；硬件上不是 ESP GPIO 直接输出高电平。
    const ds2413_latch_state_t *latch =
        enabled ? &k_motor_on_latch : &k_motor_off_latch;
    esp_err_t ret = board_ds2413_motor_write_locked(latch, &state);
    if (ret == ESP_OK)
    {
        ESP_LOGD(TAG, "motor %s: raw=0x%02X PIOA(state=%d latch=%d)",
                 enabled ? "on" : "off", state.raw, state.pioa_state,
                 state.pioa_latch);
    }
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t board_ds2413_motor_pulse(uint32_t on_ms)
{
    esp_err_t ret = board_ds2413_motor_set_enabled(true);
    if (ret != ESP_OK)
    {
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(on_ms));

    return board_ds2413_motor_set_enabled(false);
}
