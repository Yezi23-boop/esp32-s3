# DS2413 1-Wire 双路开漏 IO 最小驱动。
# 只提供设备枚举、状态读取和输出写入；GPIO 时序交给共享 1-Wire 组件。
idf_component_register(
    SRCS "ds2413.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__onewire_bus log
)
