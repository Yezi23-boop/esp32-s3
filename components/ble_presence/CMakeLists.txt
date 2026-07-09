# ble_presence 组件构建脚本
# - 只负责主界面蓝牙开关对应的普通 BLE 可发现广播
# - 不承载 Wi-Fi provisioning 协议，避免和官方 provisioning adapter 职责混在一起
idf_component_register(
    SRCS "src/ble_presence.c"
    INCLUDE_DIRS "include"
    REQUIRES bt freertos log
)
