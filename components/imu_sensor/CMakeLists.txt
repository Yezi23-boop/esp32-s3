# 通用 IMU 传感器窄接口
# - 对 service 层隐藏具体芯片 driver
# - 当前实现只适配 QMI8658C，不拥有 GPIO ISR 或采样 task
idf_component_register(
    SRCS "imu_sensor.c"
    INCLUDE_DIRS "include"
    REQUIRES qmi8658c
)

