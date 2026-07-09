# lvgl_port 组件构建脚本
# - SRCS: LVGL 端口层核心实现（显示、输入、tick）
# - REQUIRES: 显示面板、触摸、定时器与 LVGL 依赖
idf_component_register(
    SRCS
        "lv_port.c"
        "lv_port_display.c"
        "lv_port_input.c"
        "lv_port_tick.c"
    INCLUDE_DIRS "."
    REQUIRES co5300_panel touch_ft5x06 esp_lcd esp_timer heap freertos lvgl
)
