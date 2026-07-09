# network_manager 组件构建脚本
# - 对外暴露统一网络门面
# - 当前阶段先冻结 façade 契约，后续再逐步接入真实状态机
idf_component_register(
    SRCS "src/network_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES ap_portal_adapter ble_control ble_presence network_credentials network_provisioning_adapter wifi_control
)
