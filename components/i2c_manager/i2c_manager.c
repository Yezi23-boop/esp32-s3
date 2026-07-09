#include "i2c_manager.h"

#include "esp_check.h"
#include "esp_log.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
#include "driver/i2c_master.h"
#endif

static const char *TAG = "i2c_manager";

static bool s_ready = false;                    // 共享 I2C 总线是否已初始化。
static const i2c_port_t s_i2c_port = I2C_MANAGER_PORT; // 共享 I2C 端口号。

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
static i2c_master_bus_handle_t s_bus_handle = NULL; // IDF 5.3+ 下的 master bus 句柄。
#endif

/**
 * @brief 初始化共享 I2C 总线。
 * @return `ESP_OK` 表示初始化成功或之前已初始化；其他错误表示底层驱动安装失败。
 */
esp_err_t i2c_manager_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = s_i2c_port,
        .scl_io_num = I2C_MANAGER_SCL_GPIO,
        .sda_io_num = I2C_MANAGER_SDA_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,
    };

    ESP_RETURN_ON_ERROR(
        i2c_new_master_bus(&bus_cfg, &s_bus_handle), TAG,
        "new master bus failed");
    ESP_LOGI(TAG, "I2C initialized (master bus) SCL:%d SDA:%d Freq:%d",
             I2C_MANAGER_SCL_GPIO, I2C_MANAGER_SDA_GPIO,
             I2C_MANAGER_FREQ_HZ);
#else
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MANAGER_SDA_GPIO,
        .scl_io_num = I2C_MANAGER_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_DISABLE,
        .scl_pullup_en = GPIO_PULLUP_DISABLE,
        .master.clk_speed = I2C_MANAGER_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(s_i2c_port, &conf), TAG,
                        "legacy param config failed");
    ESP_RETURN_ON_ERROR(i2c_driver_install(s_i2c_port, conf.mode, 0, 0, 0),
                        TAG, "legacy driver install failed");

    ESP_LOGI(TAG, "I2C initialized (legacy) SCL:%d SDA:%d Freq:%d",
             I2C_MANAGER_SCL_GPIO, I2C_MANAGER_SDA_GPIO, I2C_MANAGER_FREQ_HZ);
#endif

    s_ready = true;
    return ESP_OK;
}

/**
 * @brief 释放共享 I2C 总线。
 * @return `ESP_OK` 表示成功；其他错误表示底层驱动删除失败。
 */
esp_err_t i2c_manager_deinit(void)
{
    if (!s_ready) {
        return ESP_OK;
    }

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    if (s_bus_handle != NULL) {
        ESP_RETURN_ON_ERROR(i2c_del_master_bus(s_bus_handle), TAG,
                            "delete master bus failed");
    }
    s_bus_handle = NULL;
#else
    i2c_driver_delete(s_i2c_port);
#endif

    s_ready = false;
    return ESP_OK;
}

/**
 * @brief 扫描当前 I2C 总线上的设备。
 * @return `ESP_OK` 表示扫描流程完成；其他错误表示总线尚未初始化。
 */
esp_err_t i2c_manager_scan(void)
{
    if (!s_ready) {
        ESP_LOGE(TAG, "I2C not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Scanning I2C bus (0x03-0x77)...");
    int found_count = 0;

    for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
        esp_err_t ret = i2c_master_probe(s_bus_handle, addr, 50);
#else
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t ret = i2c_master_cmd_begin(s_i2c_port, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
#endif
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "  Found device: 0x%02X", addr);
            found_count++;
        }
    }

    ESP_LOGI(TAG, "Scan complete, found %d device(s)", found_count);

    return ESP_OK;
}

/**
 * @brief 获取当前共享 I2C 端口号。
 * @return 当前共享 I2C 端口。
 */
i2c_port_t i2c_manager_get_port(void)
{
    if (!s_ready) {
        ESP_LOGW(TAG, "I2C bus not initialized");
    }
    return s_i2c_port;
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
/**
 * @brief 获取 IDF 5.3+ 下的 I2C master bus 句柄。
 * @return 已初始化时返回总线句柄；未初始化时返回 NULL。
 */
i2c_master_bus_handle_t i2c_manager_get_bus_handle(void)
{
    if (!s_ready) {
        ESP_LOGW(TAG, "I2C master bus not initialized");
        return NULL;
    }
    return s_bus_handle;
}
#endif
