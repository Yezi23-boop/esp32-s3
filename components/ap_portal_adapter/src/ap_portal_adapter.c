/**
 * @file ap_portal_adapter.c
 * @brief AP 门户适配层，负责最小 HTTPD 与 SoftAP provisioning handle 复用。
 */

#include "ap_portal_adapter.h"

#include "ap_portal_routes.h"
#include "captive_portal_dns.h"

#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "network_provisioning/scheme_softap.h"

/** @brief 组件日志标签。 */
static const char *TAG = "ap_portal";
/** @brief 当前门户 HTTPD 句柄；官方 SoftAP provisioning 会复用这个实例。 */
static httpd_handle_t s_portal_server = NULL;
/** @brief 指向门户 HTTPD 句柄变量本身的稳定地址；供 protocomm 以“外部句柄存储”形式解引用。 */
static httpd_handle_t *const s_portal_server_ref = &s_portal_server;
/** @brief 为“自定义门户路由 + 官方 prov-* endpoint”预留的 URI handler 槽位数。 */
static const uint16_t kPortalMaxUriHandlers = 20;
/** @brief AP 门户句柄保护 mutex 的静态存储。 */
static StaticSemaphore_t s_portal_mutex_buffer;
/** @brief 串行化门户 HTTPD 启停与句柄发布的 mutex。 */
static SemaphoreHandle_t s_portal_mutex = NULL;
/** @brief 保护门户 mutex 首次创建路径的最小临界区锁。 */
static portMUX_TYPE s_portal_bootstrap_lock = portMUX_INITIALIZER_UNLOCKED;
/** @brief SoftAP 默认 netif key；DHCP Option 114 需要挂到同一个 AP netif 上。 */
static const char *kWifiApIfKey = "WIFI_AP_DEF";
/** @brief `http://` 前缀长度；用于拼接 Captive Portal URI。 */
static const size_t kCaptivePortalSchemeLength = 7;
/** @brief DHCP Option 114 URI 静态缓冲；ESP-NETIF 只复制指针，不复制字符串内容。 */
static char s_captive_portal_uri[32] = {0};
/** @brief Captive Portal 探测期间需要降噪的 HTTPD tag 数量。 */
static const size_t kPortalMutedHttpdTagCount = 3;
/** @brief Captive Portal 活跃期间需要临时降到 `ERROR` 的 HTTPD 子模块 tag。 */
static const char *const kPortalMutedHttpdTags[] = {
    "httpd_uri",
    "httpd_txrx",
    "httpd_parse",
};
/** @brief 启动门户前保存的 HTTPD 子模块原始日志级别，用于 stop 时恢复。 */
static esp_log_level_t s_portal_muted_httpd_levels[3] = {0};
/** @brief 当前是否已经保存并覆盖过 HTTPD 子模块日志级别。 */
static bool s_portal_httpd_logs_muted = false;

static esp_err_t ap_portal_adapter_ensure_mutex(void);
static esp_err_t ap_portal_adapter_ensure_softap_netif(void);
static esp_err_t ap_portal_adapter_set_dhcp_captive_portal_uri(void);
static void ap_portal_adapter_mute_httpd_probe_logs(void);
static void ap_portal_adapter_restore_httpd_probe_logs(void);

/**
 * @brief 确保 AP 门户运行态 mutex 已创建。
 *
 * 该 mutex 用来串行化门户 HTTPD 的启动、停止和句柄查询，避免不同任务同时改写
 * `s_portal_server` 时把官方 SoftAP provisioning 绑定到过期或半停止的 handle。
 *
 * @return `ESP_OK` 表示 mutex 已可用；其他错误表示创建失败。
 */
static esp_err_t ap_portal_adapter_ensure_mutex(void)
{
    if (s_portal_mutex != NULL)
    {
        return ESP_OK;
    }

    taskENTER_CRITICAL(&s_portal_bootstrap_lock);
    if (s_portal_mutex == NULL)
    {
        s_portal_mutex = xSemaphoreCreateMutexStatic(&s_portal_mutex_buffer);
    }
    taskEXIT_CRITICAL(&s_portal_bootstrap_lock);

    return s_portal_mutex != NULL ? ESP_OK : ESP_FAIL;
}

/**
 * @brief 确保默认 SoftAP netif 已存在。
 *
 * `ap_portal_adapter_start()` 发生在 `network_provisioning_adapter_start_softap()` 之前，
 * 因此这里不能假设 `WIFI_AP_DEF` 已经由后者提前补建。若直接在 AP netif 缺席时设置
 * DHCP Option 114，就会因为拿不到目标 netif 而提前返回 `ESP_ERR_INVALID_STATE`，
 * 进而把整个门户启动流程误判为失败。
 *
 * @return `ESP_OK` 表示默认 AP netif 已可用；其他错误表示补建失败。
 */
static esp_err_t ap_portal_adapter_ensure_softap_netif(void)
{
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey(kWifiApIfKey);

    if (ap_netif != NULL)
    {
        return ESP_OK;
    }

    ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == NULL)
    {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "已补建默认 SoftAP netif，供 DHCP Option 114 与门户生命周期复用");
    return ESP_OK;
}

/**
 * @brief 为 SoftAP DHCP server 设置 Captive Portal URI（Option 114）。
 *
 * 系统自动弹页不只依赖 DNS 劫持；部分客户端还会读取 DHCP Option 114 判断“这个热点
 * 有专用门户页”。ESP-NETIF 这里不会复制 URI 字符串内容，而是只保存指针，所以必须把
 * URI 放在静态缓冲里，保证其生命周期覆盖整个 DHCP server 运行期。
 *
 * @return `ESP_OK` 表示 URI 已写入 DHCP server；其他错误表示 AP netif 不存在或配置失败。
 */
static esp_err_t ap_portal_adapter_set_dhcp_captive_portal_uri(void)
{
    esp_netif_t *ap_netif = NULL;
    esp_netif_ip_info_t ip_info = {0};
    char ip_text[16] = {0};
    esp_err_t ret = ESP_OK;
    esp_err_t dhcp_ret = ESP_OK;

    ret = ap_portal_adapter_ensure_softap_netif();
    if (ret != ESP_OK)
    {
        return ret;
    }

    ap_netif = esp_netif_get_handle_from_ifkey(kWifiApIfKey);

    if (ap_netif == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    ret = esp_netif_get_ip_info(ap_netif, &ip_info);
    if (ret != ESP_OK)
    {
        return ret;
    }

    inet_ntoa_r(ip_info.ip.addr, ip_text, sizeof(ip_text));
    memset(s_captive_portal_uri, 0, sizeof(s_captive_portal_uri));
    memcpy(s_captive_portal_uri, "http://", kCaptivePortalSchemeLength);
    strncat(s_captive_portal_uri, ip_text,
            sizeof(s_captive_portal_uri) - kCaptivePortalSchemeLength - 1);

    /* DHCP option 需要在 server 停止态下修改，避免部分 IDF 版本直接返回
     * `ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED` 或保留旧 URI。 */
    dhcp_ret = esp_netif_dhcps_stop(ap_netif);
    if (dhcp_ret != ESP_OK)
    {
        ESP_LOGW(TAG, "停止 DHCP server 以设置 Captive Portal URI 失败: %s",
                 esp_err_to_name(dhcp_ret));
    }
    ret = esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                                 ESP_NETIF_CAPTIVEPORTAL_URI,
                                 s_captive_portal_uri,
                                 strlen(s_captive_portal_uri));
    if (ret != ESP_OK)
    {
        dhcp_ret = esp_netif_dhcps_start(ap_netif);
        if (dhcp_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "恢复 DHCP server 失败: %s", esp_err_to_name(dhcp_ret));
        }
        return ret;
    }

    ret = esp_netif_dhcps_start(ap_netif);
    return ret;
}

/**
 * @brief 在 Captive Portal 活跃期间临时压低 HTTPD 探测噪声日志。
 *
 * Android/iOS/Windows 的联网探测会频繁访问未知 URI，且还可能带着半截探测连接中途断开。
 * 这些 warning 对“自动弹页是否成功”的判断价值很低，却会把 `network_prov_mgr`、`wifi_ctrl`
 * 等真正关键日志淹没掉。因此这里在门户启动时把 `httpd_uri/httpd_txrx/httpd_parse` 临时
 * 收到 `ERROR`，等门户停止后再恢复到调用前级别。
 *
 * @return 无返回值。
 */
static void ap_portal_adapter_mute_httpd_probe_logs(void)
{
    size_t index = 0;

    if (s_portal_httpd_logs_muted)
    {
        return;
    }

    for (index = 0; index < kPortalMutedHttpdTagCount; ++index)
    {
        s_portal_muted_httpd_levels[index] =
            esp_log_level_get(kPortalMutedHttpdTags[index]);
        esp_log_level_set(kPortalMutedHttpdTags[index], ESP_LOG_ERROR);
    }

    s_portal_httpd_logs_muted = true;
}

/**
 * @brief 恢复门户启动前的 HTTPD 子模块日志级别。
 *
 * 这里必须在 stop 和所有启动失败回滚路径都调用一次，避免门户已经结束，但全局
 * `httpd_uri/httpd_txrx/httpd_parse` 仍然保持在降噪级别，影响后续调试其它 HTTP 服务。
 *
 * @return 无返回值。
 */
static void ap_portal_adapter_restore_httpd_probe_logs(void)
{
    size_t index = 0;

    if (!s_portal_httpd_logs_muted)
    {
        return;
    }

    for (index = 0; index < kPortalMutedHttpdTagCount; ++index)
    {
        esp_log_level_set(kPortalMutedHttpdTags[index],
                          s_portal_muted_httpd_levels[index]);
    }

    s_portal_httpd_logs_muted = false;
}

esp_err_t ap_portal_adapter_set_memory_watch_config_callback(
    ap_portal_memory_watch_config_cb_t callback, void *user_ctx)
{
    return ap_portal_routes_set_memory_watch_config_callback(callback,
                                                              user_ctx);
}

esp_err_t ap_portal_adapter_set_memory_watch_configured_callback(
    ap_portal_memory_watch_configured_cb_t callback, void *user_ctx)
{
    return ap_portal_routes_set_memory_watch_configured_callback(callback,
                                                                  user_ctx);
}

/**
 * @brief 启动 AP 门户适配层的最小 HTTP 服务器。
 *
 * 服务器启动成功后，当前函数会立刻把 `httpd_handle_t` 交给
 * `network_prov_scheme_softap_set_httpd_handle()`，从而确保官方 SoftAP provisioning
 * 后续不会重复启动第二个 HTTPD 实例。
 *
 * @return `ESP_OK` 表示启动成功；其他错误表示 HTTP server 或路由注册失败。
 */
esp_err_t ap_portal_adapter_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    esp_err_t ret = ESP_OK;

    ret = ap_portal_adapter_ensure_mutex();
    if (ret != ESP_OK)
    {
        return ret;
    }
    if (xSemaphoreTake(s_portal_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    if (s_portal_server != NULL)
    {
        /* protocomm 在 external HTTPD 模式下会把传入值当成 `httpd_handle_t *`
         * 再做一次解引用，因此这里必须传“句柄变量地址”而不是“句柄值本身”。
         * 若少这一层 `&`，官方 `prov-*` endpoint 注册阶段会把 HTTPD 内部结构
         * 误当成句柄存储使用，最终在 `httpd_find_uri_handler()` 中读到野指针并 panic。 */
        network_prov_scheme_softap_set_httpd_handle((void *)s_portal_server_ref);
        xSemaphoreGive(s_portal_mutex);
        return ESP_OK;
    }

    /* 这里必须同时容纳：
     * 1. 自定义门户页面自己的静态资源和兼容接口
     * 2. 官方 SoftAP provisioning 动态注册的 `proto-ver / prov-session /
     *    prov-config / prov-scan / prov-ctrl`
     * 如果槽位不够，HTTPD 会在注册阶段直接返回 `ESP_ERR_HTTPD_HANDLERS_FULL`，
     * 现象上就会变成“AP 能起来，但门户页或官方 endpoint 没挂全”。 */
    config.max_uri_handlers = kPortalMaxUriHandlers;
    config.stack_size = 8192;

    ap_portal_adapter_mute_httpd_probe_logs();
    ret = httpd_start(&s_portal_server, &config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "启动 AP 门户 HTTPD 失败: %s", esp_err_to_name(ret));
        ap_portal_adapter_restore_httpd_probe_logs();
        xSemaphoreGive(s_portal_mutex);
        return ret;
    }

    ret = ap_portal_routes_register(s_portal_server);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "注册 AP 门户路由失败: %s", esp_err_to_name(ret));
        httpd_stop(s_portal_server);
        s_portal_server = NULL;
        ap_portal_adapter_restore_httpd_probe_logs();
        xSemaphoreGive(s_portal_mutex);
        return ret;
    }

    ret = ap_portal_adapter_set_dhcp_captive_portal_uri();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "设置 Captive Portal DHCP URI 失败: %s", esp_err_to_name(ret));
        httpd_stop(s_portal_server);
        s_portal_server = NULL;
        ap_portal_adapter_restore_httpd_probe_logs();
        xSemaphoreGive(s_portal_mutex);
        return ret;
    }

    ret = captive_portal_dns_start();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "启动 Captive Portal DNS 失败: %s", esp_err_to_name(ret));
        httpd_stop(s_portal_server);
        s_portal_server = NULL;
        ap_portal_adapter_restore_httpd_probe_logs();
        xSemaphoreGive(s_portal_mutex);
        return ret;
    }

    /* 这里同样传递句柄变量地址，保证官方 SoftAP provisioning 在注册 `prov-session /
     * prov-scan / prov-config` 等 endpoint 时拿到的是真实 HTTPD handle。 */
    network_prov_scheme_softap_set_httpd_handle((void *)s_portal_server_ref);
    ESP_LOGI(TAG, "AP 门户 HTTPD 已启动并复用给 SoftAP provisioning");
    xSemaphoreGive(s_portal_mutex);
    return ESP_OK;
}

/**
 * @brief 停止 AP 门户适配层的 HTTP 服务器。
 *
 * @return `ESP_OK` 表示停止成功或服务器本就未启动；其他错误表示停止失败。
 */
esp_err_t ap_portal_adapter_stop(void)
{
    esp_err_t ret = ap_portal_adapter_ensure_mutex();
    if (ret != ESP_OK)
    {
        return ret;
    }
    if (xSemaphoreTake(s_portal_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    if (s_portal_server == NULL)
    {
        (void)captive_portal_dns_stop();
        network_prov_scheme_softap_set_httpd_handle(NULL);
        ap_portal_adapter_restore_httpd_probe_logs();
        xSemaphoreGive(s_portal_mutex);
        return ESP_OK;
    }

    (void)captive_portal_dns_stop();
    network_prov_scheme_softap_set_httpd_handle(NULL);

    ret = httpd_stop(s_portal_server);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "停止 AP 门户 HTTPD 失败: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_portal_mutex);
        return ret;
    }

    s_portal_server = NULL;
    ap_portal_adapter_restore_httpd_probe_logs();
    xSemaphoreGive(s_portal_mutex);
    return ESP_OK;
}

/**
 * @brief 获取当前 AP 门户 HTTPD 句柄。
 *
 * @return 当前 HTTPD 句柄；若门户未启动则返回 `NULL`。
 */
httpd_handle_t ap_portal_adapter_get_httpd_handle(void)
{
    httpd_handle_t server = NULL;

    if (ap_portal_adapter_ensure_mutex() != ESP_OK)
    {
        return NULL;
    }
    if (xSemaphoreTake(s_portal_mutex, portMAX_DELAY) != pdTRUE)
    {
        return NULL;
    }

    server = s_portal_server;
    xSemaphoreGive(s_portal_mutex);
    return server;
}
