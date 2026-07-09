#include "touch_ft5x06.h"
#include "i2c_manager.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_idf_version.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
#include "driver/i2c_master.h"
#endif
#include "co5300_panel_defaults.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch_ft5x06";

// FT5x06/FT3168 在当前寄存器路径上兼容，驱动复用同一组寄存器定义。
#define FT5X06_ADDR 0x38            // FT5x06/FT3168 的 7 位 I2C 地址。
#define FT5X06_REG_NUM_TOUCHES 0x02 // 当前触点数量寄存器地址。
#define FT5X06_REG_TOUCH1_XH 0x03   // 第一个触点坐标寄存器起始地址。
#define FT5X06_MAX_TOUCHES 5        // 当前驱动最多处理的触点数；LVGL 主链路实际只消费单点。

/* FT3168 在事件类型位上的兼容性不稳定，当前驱动只依赖触点数量和坐标，
 * 避免把不同芯片型号上不可靠的事件编码引入上层状态机。 */

typedef struct
{
    uint16_t x;    /**< X 坐标。 */
    uint16_t y;    /**< Y 坐标。 */
    uint8_t event; /**< 触摸事件类型；当前实现未使用，仅保留兼容字段。 */
    uint8_t id;    /**< 触点 ID；当前实现未使用。 */
} touch_point_t;

typedef struct
{
    int rst_gpio;                             /**< 复位脚 GPIO。 */
    int int_gpio;                             /**< 中断脚 GPIO；当前轮询链路保留但未使用。 */
    uint16_t max_x;                           /**< 屏幕 X 方向最大分辨率。 */
    uint16_t max_y;                           /**< 屏幕 Y 方向最大分辨率。 */
    uint8_t point_num;                        /**< 最近一次读取到的触点数量。 */
    touch_point_t points[FT5X06_MAX_TOUCHES]; /**< 最近一次读取到的触点缓存。 */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    i2c_master_dev_handle_t dev_handle; /**< IDF 5.3+ 下的 I2C 设备句柄。 */
#endif
} touch_ft5x06_t;

static touch_ft5x06_t *s_touch = NULL; // 触摸控制器实例指针，由驱动初始化创建并长期持有。

/**
 * @brief 从 FT5x06 读取寄存器数据。
 *
 * 该辅助函数统一兼容 IDF 5.3+ 的 `i2c_master` 新接口和旧版命令链接口，
 * 避免上层触摸解析逻辑分叉维护两套总线代码。
 *
 * @param[in] touch 触摸控制器结构体指针。
 * @param[in] reg 寄存器地址。
 * @param[out] data 读取数据缓冲区。
 * @param[in] len 读取数据长度，单位为字节。
 * @return `ESP_OK` 表示成功；其他错误表示 I2C 访问失败。
 */
static esp_err_t touch_ft5x06_i2c_read(touch_ft5x06_t *touch, uint8_t reg, uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(touch != NULL, ESP_ERR_INVALID_ARG, TAG, "touch is null");
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, TAG, "data is null");
    ESP_RETURN_ON_FALSE(len > 0, ESP_ERR_INVALID_ARG, TAG, "len must be > 0");

    ESP_RETURN_ON_FALSE(touch->dev_handle != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "touch i2c device not ready");
    return i2c_master_transmit_receive(touch->dev_handle, &reg, sizeof(reg),
                                       data, len, 500);
}

/**
 * @brief 复位 FT5x06 触摸控制器。
 *
 * 复位脉冲和后续等待窗口是上电时序的一部分；过早开始寄存器访问会放大冷启动读失败概率。
 *
 * @param[in] touch 触摸控制器结构体指针。
 * @return `ESP_OK` 表示复位流程完成。
 */
static esp_err_t touch_ft5x06_reset(touch_ft5x06_t *touch)
{
    if (touch->rst_gpio >= 0)
    {
        gpio_set_level(touch->rst_gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(touch->rst_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return ESP_OK;
}

/**
 * @brief 初始化 FT5x06/FT3168 触摸控制器。
 *
 * 当前驱动依赖共享 I2C 总线和板级分辨率配置；初始化成功后，LVGL 输入层只通过读点接口取坐标。
 *
 * @return `ESP_OK` 表示初始化成功或之前已初始化；其他错误表示 I2C、GPIO 或设备注册失败。
 */
esp_err_t touch_ft5x06_init(void)
{
    esp_err_t ret;

    if (s_touch)
    {
        return ESP_OK;
    }

    // 触摸和其他外设共用 I2C 总线，因此先确保共享总线管理器已就绪。
    ESP_RETURN_ON_ERROR(i2c_manager_init(), TAG, "i2c manager init failed");

    s_touch = calloc(1, sizeof(touch_ft5x06_t));
    ESP_RETURN_ON_FALSE(s_touch, ESP_ERR_NO_MEM, TAG, "alloc touch failed");

    s_touch->rst_gpio = TOUCH_FT5X06_RST_GPIO;
    if (s_touch->rst_gpio >= 0)
    {
        gpio_config_t rst_cfg = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = BIT64(s_touch->rst_gpio),
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&rst_cfg), err, TAG, "RST GPIO config failed");
    }

    // 触摸坐标范围直接复用面板分辨率，避免显示链和输入链维护两份边界。
    s_touch->max_x = CO5300_PANEL_H_RES;
    s_touch->max_y = CO5300_PANEL_V_RES;

    ESP_GOTO_ON_ERROR(touch_ft5x06_reset(s_touch), err, TAG, "reset failed");

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    i2c_master_bus_handle_t bus_handle = i2c_manager_get_bus_handle();
    ESP_GOTO_ON_FALSE(bus_handle != NULL, ESP_ERR_INVALID_STATE, err, TAG,
                      "i2c master bus not ready");
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = FT5X06_ADDR,
        .scl_speed_hz = I2C_MANAGER_FREQ_HZ,
    };
    ESP_GOTO_ON_ERROR(
        i2c_master_bus_add_device(bus_handle, &dev_cfg, &s_touch->dev_handle),
        err, TAG, "add FT5x06 device failed");
#endif

    ESP_LOGI(TAG, "FT5x06/FT3168 initialized successfully");
    return ESP_OK;

err:
    if (s_touch)
    {
        free(s_touch);
        s_touch = NULL;
    }
    return ret;
}

/**
 * @brief 读取当前触摸点坐标。
 *
 * 读失败时当前实现会退化为“无触摸”并返回 `ESP_OK`，
 * 这是为了避免共享 I2C 总线短暂忙碌时把普通轮询路径放大成错误刷屏。
 *
 * @param[out] x 输出 X 坐标数组。
 * @param[out] y 输出 Y 坐标数组。
 * @param[out] num_points 输出触摸点数量。
 * @param[in] max_points 调用方可接收的最大点数。
 * @return `ESP_OK` 表示读取流程完成；参数或状态非法时返回错误。
 */
esp_err_t touch_ft5x06_read_points(uint16_t *x, uint16_t *y, uint8_t *num_points, uint8_t max_points)
{
    ESP_RETURN_ON_FALSE(s_touch, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(x && y && num_points, ESP_ERR_INVALID_ARG, TAG, "invalid args");

    uint8_t data[4];

    esp_err_t ret = touch_ft5x06_i2c_read(s_touch, FT5X06_REG_NUM_TOUCHES, data, 1);
    if (ret != ESP_OK)
    {
        *num_points = 0;
        return ESP_OK;
    }

    uint8_t point_count = data[0] & 0x0F;
    if (point_count == 0 || point_count > FT5X06_MAX_TOUCHES)
    {
        *num_points = 0;
        return ESP_OK;
    }

    ret = touch_ft5x06_i2c_read(s_touch, FT5X06_REG_TOUCH1_XH, data, 4);
    if (ret != ESP_OK)
    {
        *num_points = 0;
        return ESP_OK;
    }

    x[0] = ((data[0] & 0x0F) << 8) | data[1];
    y[0] = ((data[2] & 0x0F) << 8) | data[3];
    *num_points = (point_count > max_points) ? max_points : point_count;

    return ESP_OK;
}

/**
 * @brief 获取内部触摸控制器句柄。
 *
 * 该接口主要供端口层在初始化后保留句柄，不建议业务层直接依赖底层结构体定义。
 *
 * @param[out] out_handle 输出句柄指针。
 * @return `ESP_OK` 表示成功；参数非法或尚未初始化时返回错误。
 */
esp_err_t touch_ft5x06_get_handle(void **out_handle)
{
    ESP_RETURN_ON_FALSE(out_handle, ESP_ERR_INVALID_ARG, TAG, "invalid arg");
    ESP_RETURN_ON_FALSE(s_touch, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    *out_handle = s_touch;
    return ESP_OK;
}
