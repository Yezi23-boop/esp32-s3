# axp2101 组件构建脚本
# - 提供 AXP2101 PMIC 只读探测/快照/IRQ 状态接口
# - 依赖共享 I2C 管理器 i2c_manager
idf_component_register(
    SRCS "axp2101.c"
    INCLUDE_DIRS "include"
    REQUIRES i2c_manager
)
