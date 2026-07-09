# ap_portal_adapter 组件构建脚本
# - 提供自定义 AP 门户 HTTPD
# - 向官方 SoftAP provisioning scheme 提供可复用的 HTTPD handle
# - 预埋官方 provisioning client 侧最小静态资源
idf_component_register(
    SRCS
        "src/ap_portal_adapter.c"
        "src/ap_portal_routes.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_http_server network_provisioning log esp_netif captive_portal_dns lwip json
    EMBED_TXTFILES
        "web/index.html"
        "web/app.js"
        "web/prov_client.js"
        "web/prov_proto_bundle.js"
        "web/app.css"
)
