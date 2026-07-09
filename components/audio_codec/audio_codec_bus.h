#pragma once

#include "driver/i2s_std.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 初始化音频 I2S 双工总线
     * @details 创建同一 I2S 端口上的 TX/RX 通道并按预设模式启用。
     */
    esp_err_t audio_codec_bus_init(void);

    /**
     * @brief 反初始化音频 I2S 总线
     * @details 关闭并删除 TX/RX 通道。
     */
    esp_err_t audio_codec_bus_deinit(void);

    /**
     * @brief 获取 TX 通道句柄（播放链路）
     */
    i2s_chan_handle_t audio_codec_bus_get_tx_handle(void);

    /**
     * @brief 获取 RX 通道句柄（录音链路）
     */
    i2s_chan_handle_t audio_codec_bus_get_rx_handle(void);

    /**
     * @brief 获取绑定的 I2S 端口号
     */
    i2s_port_t audio_codec_bus_get_port(void);

#ifdef __cplusplus
}
#endif
