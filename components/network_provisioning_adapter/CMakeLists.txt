# network_provisioning_adapter 组件构建脚本
# - 封装官方 network_provisioning / wifi_prov_mgr 生命周期
# - 对上层只暴露单实例、单 transport 的最小控制面
idf_component_register(
    SRCS "src/network_provisioning_adapter.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES network_provisioning esp_wifi esp_event log
)
