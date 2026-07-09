# wifi_control 组件构建脚本
# - 只承载 Wi-Fi STA runtime control
# - 不包含 BLE / AP provisioning 语义
idf_component_register(
    SRCS "src/wifi_control.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_wifi esp_event esp_netif nvs_flash log
)
