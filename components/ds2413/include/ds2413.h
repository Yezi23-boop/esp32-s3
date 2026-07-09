#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "onewire_device.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define DS2413_BOARD_FAMILY_CODE 0xBA /**< 当前样板实测 family code；不再接受通用/clone code，避免误匹配。 */
#define DS2413_ACCESS_READ 0xF5       /**< DS2413 PIO_ACCESS_READ 命令字。 */
#define DS2413_ACCESS_WRITE 0x5A      /**< DS2413 PIO_ACCESS_WRITE 命令字。 */
#define DS2413_ACK_SUCCESS 0xAA       /**< 写锁存成功 ACK；其他值表示器件未接受状态字节。 */

    /**
     * @brief DS2413 器件句柄。
     *
     * 句柄只保存 1-Wire 总线和 ROM 地址，不拥有底层总线资源。
     */
    typedef struct
    {
        onewire_bus_handle_t bus;         /**< 已初始化的 1-Wire bus 句柄。 */
        onewire_device_address_t address; /**< 8 字节 ROM 地址，用于 MATCH ROM。 */
    } ds2413_device_t;

    /**
     * @brief DS2413 读到的 PIO 状态。
     *
     * `pio*_state` 对应引脚瞬时电平，`pio*_latch` 对应输出锁存状态。
     * 数据格式和 DS2413 的 `PIO_ACCESS_READ` 返回字节一致。
     */
    typedef struct
    {
        uint8_t raw;     /**< 原始状态字节。 */
        bool pioa_state; /**< Bit0，PIOA 引脚电平。 */
        bool pioa_latch; /**< Bit1，PIOA 输出锁存。 */
        bool piob_state; /**< Bit2，PIOB 引脚电平。 */
        bool piob_latch; /**< Bit3，PIOB 输出锁存。 */
    } ds2413_state_t;

    /**
     * @brief DS2413 输出锁存状态。
     *
     * 这里保留为语义化布尔值：`false` 表示拉低/使能下拉，`true` 表示释放。
     */
    typedef struct
    {
        bool pioa_release; /**< true 释放 PIOA，false 拉低 PIOA。 */
        bool piob_release; /**< true 释放 PIOB，false 拉低 PIOB。 */
    } ds2413_latch_state_t;

    /**
     * @brief 枚举总线上第一个 DS2413 兼容器件。
     *
     * 只兼容当前板卡实测到的 family code `0xBA`。
     *
     * @param[in] bus 1-Wire bus 句柄。
     * @param[out] device 输出设备句柄。
     * @return
     *      - ESP_OK: 找到设备并填充句柄
     *      - ESP_ERR_NOT_FOUND: 总线上没有 DS2413 兼容器件
     *      - 其他错误: 1-Wire 枚举失败
     */
    esp_err_t ds2413_find_first(onewire_bus_handle_t bus, ds2413_device_t *device);

    /**
     * @brief 按索引枚举总线上的 DS2413 兼容器件。
     *
     * 索引只对 DS2413 兼容 family code 计数，便于总线上混挂其他 1-Wire
     * 设备时单独筛选。
     *
     * @param[in] bus 1-Wire bus 句柄。
     * @param[in] index 第几个 DS2413 兼容器件，从 0 开始。
     * @param[out] device 输出设备句柄。
     * @return
     *      - ESP_OK: 找到设备
     *      - ESP_ERR_NOT_FOUND: 索引越界或总线上没有匹配设备
     *      - 其他错误: 1-Wire 枚举失败
     */
    esp_err_t ds2413_find_by_index(onewire_bus_handle_t bus, size_t index,
                                   ds2413_device_t *device);

    /**
     * @brief 读取一次 DS2413 的 PIO 状态。
     *
     * 读序列为 `MATCH ROM -> PIO_ACCESS_READ -> 读取 1 字节状态`。
     * 返回值会校验状态字节的 4 个反码位，避免把总线毛刺当成有效状态。
     *
     * @param[in] device 目标器件句柄。
     * @param[out] state 输出状态。
     * @return
     *      - ESP_OK: 读取成功
     *      - ESP_ERR_INVALID_CRC: 状态字节校验失败
     *      - ESP_ERR_NOT_FOUND: 器件无应答
     *      - 其他错误: 1-Wire 事务失败
     */
    esp_err_t ds2413_read_state(const ds2413_device_t *device,
                                ds2413_state_t *state);

    /**
     * @brief 写入 DS2413 的 PIO 锁存状态。
     *
     * 写序列为 `MATCH ROM -> PIO_ACCESS_WRITE -> 状态字节 -> 反码字节 ->
     * ACK -> 状态回读`。`false` 表示拉低输出，`true` 表示释放 open-drain。
     *
     * @param[in] device 目标器件句柄。
     * @param[in] latch_state 期望的输出锁存状态。
     * @param[out] verified_state 可选，成功后返回器件回读状态。
     * @return
     *      - ESP_OK: 写入并校验成功
     *      - ESP_ERR_INVALID_CRC: 器件回读校验失败
     *      - ESP_ERR_NOT_FOUND: 器件无应答
     *      - 其他错误: 1-Wire 事务失败
     */
    esp_err_t ds2413_write_latch(const ds2413_device_t *device,
                                 const ds2413_latch_state_t *latch_state,
                                 ds2413_state_t *verified_state);

#ifdef __cplusplus
}
#endif
