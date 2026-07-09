#pragma once

#include "driver/i2c.h"
#include "esp_err.h"
#include "esp_idf_version.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
#include "driver/i2c_master.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* 当前板级 I2C 总线固定接在如下端口和 GPIO 上。 */
#define I2C_MANAGER_PORT I2C_NUM_0
#define I2C_MANAGER_SCL_GPIO 14
#define I2C_MANAGER_SDA_GPIO 15
#define I2C_MANAGER_FREQ_HZ 400000 /* I2C 总线时钟，单位为 Hz。 */

/**
 * @brief 初始化共享 I2C 总线。
 * @return `ESP_OK` 表示初始化成功或之前已初始化；其他错误表示驱动安装失败。
 */
esp_err_t i2c_manager_init(void);

/**
 * @brief 获取当前 I2C 端口号。
 * @return 当前共享 I2C 端口。
 */
i2c_port_t i2c_manager_get_port(void);

/**
 * @brief 释放共享 I2C 总线。
 * @return `ESP_OK` 表示成功；其他错误表示底层驱动删除失败。
 */
esp_err_t i2c_manager_deinit(void);

/**
 * @brief 扫描当前 I2C 总线上的设备。
 * @return `ESP_OK` 表示扫描流程完成；其他错误表示总线尚未初始化。
 */
esp_err_t i2c_manager_scan(void);

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
/**
 * @brief 获取 IDF 5.3+ 下的 I2C master bus 句柄。
 * @return 已初始化时返回总线句柄；未初始化时返回 NULL。
 */
i2c_master_bus_handle_t i2c_manager_get_bus_handle(void);
#endif

#ifdef __cplusplus
}
#endif
