# QMI8658C IMU 最小本地组件
# - 仅提供共享 I2C 下的探测、配置和原始六轴数据读取
# - 不拥有连续采样任务或 UI 生命周期
idf_component_register(
    SRCS "qmi8658c.c"
    INCLUDE_DIRS "include"
    REQUIRES i2c_manager
)
