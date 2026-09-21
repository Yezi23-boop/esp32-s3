# audio_codec 组件构建脚本
# - 封装 I2C 控制、I2S 总线和 ES8311/ES7210 初始化
# - 对上层暴露统一音频读写/音量/静音接口
idf_component_register(
    SRCS "audio_codec.c" "audio_codec_bus.c"
    INCLUDE_DIRS "include"
    REQUIRES 
        driver
        esp_codec_dev
        i2c_manager
        nvs_flash
)
