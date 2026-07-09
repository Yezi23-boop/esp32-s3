# ble_control 组件构建脚本
# - 只负责 BLE 总开关偏好与运行态表达
# - 不包含 provisioning 启停逻辑
idf_component_register(
    SRCS "src/ble_control.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES freertos nvs_flash log
)
