# network_credentials 组件构建脚本
# - 只负责 recent Wi-Fi 凭据存储
# - 不包含 BLE / AP / provisioning 语义
idf_component_register(
    SRCS "src/network_credentials.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash log
)
