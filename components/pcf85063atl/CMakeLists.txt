# PCF85063ATL RTC 最小驱动
# - 仅提供探测、读时、倒计时中断证据接口
# - 依赖共享 I2C 管理器 i2c_manager
idf_component_register(
    SRCS "pcf85063atl.c"
    INCLUDE_DIRS "include"
    REQUIRES i2c_manager
)
