# co5300_panel 组件构建脚本
# - 封装 CO5300 面板初始化、亮度控制、TE 同步与回调注册
# - 依赖 esp_lcd_co5300 和底层 driver/freertos/log
idf_component_register(
    SRCS "co5300_panel.c"
    INCLUDE_DIRS "include" "."
    REQUIRES driver esp_lcd esp_lcd_co5300 freertos log
)
