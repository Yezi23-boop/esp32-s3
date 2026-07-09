#include "audio_codec_bus.h"

#include "audio_codec.h"
#include "audio_platform_config.h"
#include "driver/i2s_tdm.h"
#include "esp_log.h"

static const char *TAG = "audio_codec_bus";

static i2s_chan_handle_t s_i2s_tx_handle = NULL; // 播放 TX 通道句柄
static i2s_chan_handle_t s_i2s_rx_handle = NULL; // 录音 RX 通道句柄
static bool s_i2s_tx_enabled = false;            // TX 通道当前使能状态
static bool s_i2s_rx_enabled = false;            // RX 通道当前使能状态
static const i2s_port_t s_i2s_port = I2S_NUM_0;  // 统一使用 I2S0

esp_err_t audio_codec_bus_init(void)
{
    esp_err_t ret;
    // 双工通道配置：同一控制器上同时创建 TX/RX。
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(s_i2s_port, I2S_ROLE_MASTER);

    // TX 走标准 I2S 立体声模式，驱动 ES8311 DAC。
    i2s_std_config_t std_cfg = {
        .clk_cfg =
            {
                .sample_rate_hz = AUDIO_PLATFORM_HW_SAMPLE_RATE,
                .clk_src = I2S_CLK_SRC_DEFAULT,
                .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            },
        .slot_cfg =
            {
                .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
                .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                .slot_mode = I2S_SLOT_MODE_STEREO,
                .slot_mask = I2S_STD_SLOT_BOTH,
                .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
                .ws_pol = false,
                .bit_shift = true,
                .left_align = true,
                .big_endian = false,
                .bit_order_lsb = false,
            },
        .gpio_cfg =
            {
                .mclk = AUDIO_I2S_MCLK_GPIO,
                .bclk = AUDIO_I2S_SCLK_GPIO,
                .ws = AUDIO_I2S_LRCK_GPIO,
                .dout = AUDIO_I2S_ASDOUT_GPIO, // TX 数据输出到 DAC
                .din = I2S_GPIO_UNUSED,
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv = false,
                    },
            },
    };

    // RX 走 TDM 模式，用于接收 ES7210 多路麦克风时隙数据。
    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg =
            {
                .sample_rate_hz = AUDIO_PLATFORM_HW_SAMPLE_RATE,
                .clk_src = I2S_CLK_SRC_DEFAULT,
                .mclk_multiple = I2S_MCLK_MULTIPLE_256,
                .bclk_div = 8,
            },
        .slot_cfg =
            {
                .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
                .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                .slot_mode = I2S_SLOT_MODE_STEREO,
                .slot_mask = (i2s_tdm_slot_mask_t)(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 |
                                                   I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
                .ws_width = I2S_TDM_AUTO_WS_WIDTH,
                .ws_pol = false,
                .bit_shift = true,
                .left_align = false,
                .big_endian = false,
                .bit_order_lsb = false,
                .skip_mask = false,
                .total_slot = I2S_TDM_AUTO_SLOT_NUM,
            },
        .gpio_cfg =
            {
                .mclk = AUDIO_I2S_MCLK_GPIO,
                .bclk = AUDIO_I2S_SCLK_GPIO,
                .ws = AUDIO_I2S_LRCK_GPIO,
                .dout = I2S_GPIO_UNUSED,
                .din = AUDIO_I2S_DSDIN_GPIO, // RX 数据输入来自 ADC
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv = false,
                    },
            },
    };

    if (s_i2s_tx_handle != NULL && s_i2s_rx_handle != NULL)
    {
        return ESP_OK;
    }

    // auto_clear=true: DMA underflow 时自动补零，减少突发噪声。
    chan_cfg.auto_clear = true;
    ret = i2s_new_channel(&chan_cfg, &s_i2s_tx_handle, &s_i2s_rx_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create I2S duplex channel: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_channel_init_std_mode(s_i2s_tx_handle, &std_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to init I2S TX standard mode: %s",
                 esp_err_to_name(ret));
        audio_codec_bus_deinit();
        return ret;
    }

    ret = i2s_channel_init_tdm_mode(s_i2s_rx_handle, &tdm_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to init I2S RX TDM mode: %s",
                 esp_err_to_name(ret));
        audio_codec_bus_deinit();
        return ret;
    }

    ret = i2s_channel_enable(s_i2s_tx_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to enable I2S TX channel: %s",
                 esp_err_to_name(ret));
        audio_codec_bus_deinit();
        return ret;
    }
    s_i2s_tx_enabled = true;

    ret = i2s_channel_enable(s_i2s_rx_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to enable I2S RX channel: %s",
                 esp_err_to_name(ret));
        audio_codec_bus_deinit();
        return ret;
    }
    s_i2s_rx_enabled = true;

    return ESP_OK;
}

esp_err_t audio_codec_bus_deinit(void)
{
    if (s_i2s_tx_handle != NULL)
    {
        if (s_i2s_tx_enabled)
        {
            (void)i2s_channel_disable(s_i2s_tx_handle);
            s_i2s_tx_enabled = false;
        }
        (void)i2s_del_channel(s_i2s_tx_handle);
        s_i2s_tx_handle = NULL;
    }

    if (s_i2s_rx_handle != NULL)
    {
        if (s_i2s_rx_enabled)
        {
            (void)i2s_channel_disable(s_i2s_rx_handle);
            s_i2s_rx_enabled = false;
        }
        (void)i2s_del_channel(s_i2s_rx_handle);
        s_i2s_rx_handle = NULL;
    }

    return ESP_OK;
}

i2s_chan_handle_t audio_codec_bus_get_tx_handle(void)
{
    return s_i2s_tx_handle;
}

i2s_chan_handle_t audio_codec_bus_get_rx_handle(void)
{
    return s_i2s_rx_handle;
}

i2s_port_t audio_codec_bus_get_port(void)
{
    return s_i2s_port;
}
