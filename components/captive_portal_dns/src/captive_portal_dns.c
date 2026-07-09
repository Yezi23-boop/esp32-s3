/**
 * @file captive_portal_dns.c
 * @brief Captive Portal DNS 劫持服务；把系统探测域名统一回答到 SoftAP IP。
 */

#include "captive_portal_dns.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

/** @brief 组件日志标签。 */
static const char *TAG = "captive_dns";
/** @brief DNS 服务监听端口；Captive Portal 依赖标准 53 端口拦截系统探测域名。 */
static const uint16_t kDnsPort = 53;
/** @brief 单个 DNS 报文的保守缓冲上限；当前只处理轻量 A 记录问答。 */
static const size_t kDnsMaxPacketLength = 256;
/** @brief DNS flags 中的 opcode 掩码；非标准 query 直接忽略，避免把无关请求误答成本机。 */
static const uint16_t kDnsOpcodeMask = 0x7800;
/** @brief DNS 响应标志位中的 QR；回复报文必须显式标记为 answer。 */
static const uint16_t kDnsResponseFlag = (1 << 7);
/** @brief IPv4 A 记录类型值；当前 Captive Portal 只需要接管 IPv4 HTTP 探测。 */
static const uint16_t kDnsQuestionTypeA = 0x0001;
/** @brief Captive Portal DNS 响应 TTL，单位秒；保守使用 300 秒减少系统端过度缓存。 */
static const uint32_t kDnsAnswerTtlSeconds = 300;
/** @brief SoftAP 默认 netif key；用于把全部 DNS 查询回答到当前 AP IP。 */
static const char *kWifiApIfKey = "WIFI_AP_DEF";
/** @brief 通配域名匹配项；表示所有 A 记录查询都回到门户 AP。 */
static const char *kWildcardDnsName = "*";

/**
 * @brief DNS 头部。
 *
 * 使用 packed 结构是因为这里直接按网络报文布局读写头部字段。
 */
typedef struct __attribute__((__packed__))
{
    uint16_t id;       /**< 报文 ID，用于原样回显给客户端匹配请求。 */
    uint16_t flags;    /**< DNS flags 字段。 */
    uint16_t qd_count; /**< question 数量。 */
    uint16_t an_count; /**< answer 数量。 */
    uint16_t ns_count; /**< authority 数量。 */
    uint16_t ar_count; /**< additional 数量。 */
} captive_dns_header_t;

/**
 * @brief DNS question 尾部的 type/class 对。
 */
typedef struct
{
    uint16_t type;  /**< DNS question type。 */
    uint16_t class; /**< DNS question class。 */
} captive_dns_question_t;

/**
 * @brief DNS answer 结构。
 *
 * 当前只生成 IPv4 A 记录 answer，因此字段布局固定。
 */
typedef struct __attribute__((__packed__))
{
    uint16_t ptr_offset; /**< 指向 question name 的压缩偏移。 */
    uint16_t type;       /**< answer type。 */
    uint16_t class;      /**< answer class。 */
    uint32_t ttl;        /**< TTL，单位秒。 */
    uint16_t addr_len;   /**< IPv4 地址长度，固定 4 字节。 */
    uint32_t ip_addr;    /**< 要回答给客户端的 IPv4 地址。 */
} captive_dns_answer_t;

/**
 * @brief DNS 服务运行时句柄。
 */
typedef struct captive_portal_dns_server
{
    bool started;       /**< 任务主循环的运行开关；stop 时先拉低再关闭 socket。 */
    TaskHandle_t task;  /**< DNS 任务句柄。 */
    int socket_fd;      /**< 当前 UDP socket；stop 时需要主动 shutdown 以打断 recvfrom。 */
} captive_portal_dns_server_t;

/** @brief 全局 DNS 服务句柄；当前 AP 门户同一时刻只允许存在一个 DNS 劫持实例。 */
static captive_portal_dns_server_t *s_dns_server = NULL;

static char *captive_portal_dns_parse_name(char *raw_name, char *parsed_name,
                                           size_t parsed_name_max_len);
static int captive_portal_dns_prepare_reply(char *request, size_t request_length,
                                            char *reply, size_t reply_max_length);
static void captive_portal_dns_task(void *parameters);

/**
 * @brief 解析 DNS name 字段为常规点分域名。
 *
 * @param[in] raw_name DNS 报文中的压缩前 name 起始地址。
 * @param[out] parsed_name 输出域名缓冲。
 * @param[in] parsed_name_max_len 输出缓冲大小。
 * @return 成功时返回指向 question type/class 的后继指针；失败时返回 `NULL`。
 */
static char *captive_portal_dns_parse_name(char *raw_name, char *parsed_name,
                                           size_t parsed_name_max_len)
{
    char *label = raw_name;
    char *name_iterator = parsed_name;
    size_t name_length = 0;

    if (raw_name == NULL || parsed_name == NULL || parsed_name_max_len == 0)
    {
        return NULL;
    }

    do
    {
        const int sub_name_length = *label;

        /* 这里额外预留 1 字节给点分隔符；若超界说明报文异常，直接拒绝响应。 */
        name_length += (size_t)(sub_name_length + 1);
        if (name_length > parsed_name_max_len)
        {
            return NULL;
        }

        memcpy(name_iterator, label + 1, (size_t)sub_name_length);
        name_iterator[sub_name_length] = '.';
        name_iterator += (sub_name_length + 1);
        label += (sub_name_length + 1);
    } while (*label != 0);

    parsed_name[name_length - 1] = '\0';
    return label + 1;
}

/**
 * @brief 把 DNS 请求转换为“回答当前 SoftAP IP”的 DNS 响应。
 *
 * @param[in] request 原始 DNS 请求缓冲。
 * @param[in] request_length 请求长度。
 * @param[out] reply 输出响应缓冲。
 * @param[in] reply_max_length 输出缓冲大小。
 * @return `>0` 表示可发送的响应长度；`0` 表示当前请求不应回答；`<0` 表示报文非法。
 */
static int captive_portal_dns_prepare_reply(char *request, size_t request_length,
                                            char *reply, size_t reply_max_length)
{
    captive_dns_header_t *header = NULL;
    char *current_question = NULL;
    char *current_answer = NULL;
    char question_name[128];
    uint16_t question_count = 0;
    int reply_length = 0;
    esp_netif_ip_info_t ip_info = {0};
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey(kWifiApIfKey);

    if (request == NULL || reply == NULL || request_length > reply_max_length ||
        ap_netif == NULL)
    {
        return -1;
    }

    if (esp_netif_get_ip_info(ap_netif, &ip_info) != ESP_OK ||
        ip_info.ip.addr == IPADDR_ANY)
    {
        return -1;
    }

    memset(reply, 0, reply_max_length);
    memcpy(reply, request, request_length);

    header = (captive_dns_header_t *)reply;
    if ((header->flags & kDnsOpcodeMask) != 0)
    {
        return 0;
    }

    header->flags |= kDnsResponseFlag;
    question_count = ntohs(header->qd_count);
    header->an_count = htons(question_count);

    reply_length = (int)(request_length + question_count * sizeof(captive_dns_answer_t));
    if ((size_t)reply_length > reply_max_length)
    {
        return -1;
    }

    current_question = reply + sizeof(captive_dns_header_t);
    current_answer = reply + request_length;

    for (uint16_t index = 0; index < question_count; ++index)
    {
        captive_dns_question_t *question = NULL;
        captive_dns_answer_t *answer = NULL;
        char *name_end = captive_portal_dns_parse_name(
            current_question, question_name, sizeof(question_name));

        if (name_end == NULL)
        {
            ESP_LOGW(TAG, "解析 DNS 域名失败");
            return -1;
        }

        question = (captive_dns_question_t *)name_end;
        if (ntohs(question->type) != kDnsQuestionTypeA)
        {
            current_question = (char *)(question + 1);
            continue;
        }

        /* 当前 Captive Portal 采用通配规则，无论系统探测访问哪个 HTTP 域名，
         * 都统一回答 SoftAP IP，把流量引回本机门户。 */
        if (strcmp(kWildcardDnsName, "*") != 0)
        {
            return -1;
        }

        answer = (captive_dns_answer_t *)current_answer;
        answer->ptr_offset =
            htons((uint16_t)(0xC000 | (uint16_t)(current_question - reply)));
        answer->type = question->type;
        answer->class = question->class;
        answer->ttl = htonl(kDnsAnswerTtlSeconds);
        answer->addr_len = htons(sizeof(ip_info.ip.addr));
        answer->ip_addr = ip_info.ip.addr;

        current_question = (char *)(question + 1);
        current_answer += sizeof(captive_dns_answer_t);
    }

    return reply_length;
}

/**
 * @brief DNS 服务任务主循环。
 *
 * @param[in] parameters 运行时句柄指针。
 */
static void captive_portal_dns_task(void *parameters)
{
    captive_portal_dns_server_t *server = (captive_portal_dns_server_t *)parameters;
    struct sockaddr_in destination = {0};
    char receive_buffer[128];
    char address_buffer[64];

    if (server == NULL)
    {
        vTaskDelete(NULL);
        return;
    }

    destination.sin_addr.s_addr = htonl(INADDR_ANY);
    destination.sin_family = AF_INET;
    destination.sin_port = htons(kDnsPort);

    while (server->started)
    {
        struct sockaddr_in6 source_address = {0};
        socklen_t socket_length = sizeof(source_address);
        const int socket_fd =
            socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);

        if (socket_fd < 0)
        {
            ESP_LOGE(TAG, "创建 DNS socket 失败: errno=%d", errno);
            break;
        }

        server->socket_fd = socket_fd;
        if (bind(socket_fd, (struct sockaddr *)&destination, sizeof(destination)) < 0)
        {
            ESP_LOGE(TAG, "绑定 DNS socket 失败: errno=%d", errno);
            shutdown(socket_fd, 0);
            close(socket_fd);
            server->socket_fd = -1;
            break;
        }

        ESP_LOGI(TAG, "Captive Portal DNS 已启动，监听 UDP/%u", kDnsPort);
        while (server->started)
        {
            char reply[kDnsMaxPacketLength];
            const int receive_length = recvfrom(
                socket_fd, receive_buffer, sizeof(receive_buffer) - 1, 0,
                (struct sockaddr *)&source_address, &socket_length);

            if (receive_length < 0)
            {
                if (server->started)
                {
                    ESP_LOGW(TAG, "DNS recvfrom 失败: errno=%d", errno);
                }
                break;
            }

            receive_buffer[receive_length] = '\0';
            if (source_address.sin6_family == PF_INET)
            {
                inet_ntoa_r(((struct sockaddr_in *)&source_address)->sin_addr.s_addr,
                            address_buffer, sizeof(address_buffer) - 1);
            }
            else
            {
                strcpy(address_buffer, "unknown");
            }

            const int reply_length = captive_portal_dns_prepare_reply(
                receive_buffer, (size_t)receive_length, reply, sizeof(reply));
            if (reply_length <= 0)
            {
                continue;
            }

            if (sendto(socket_fd, reply, reply_length, 0,
                       (struct sockaddr *)&source_address,
                       sizeof(source_address)) < 0)
            {
                ESP_LOGW(TAG, "DNS sendto 失败: errno=%d", errno);
                break;
            }

            ESP_LOGD(TAG, "DNS 将探测请求重定向到本机门户: peer=%s", address_buffer);
        }

        if (server->socket_fd != -1)
        {
            shutdown(server->socket_fd, 0);
            close(server->socket_fd);
            server->socket_fd = -1;
        }
    }

    ESP_LOGI(TAG, "Captive Portal DNS 已停止");
    vTaskDelete(NULL);
}

/**
 * @brief 启动 Captive Portal 使用的 DNS 劫持服务。
 *
 * @return `ESP_OK` 表示服务已启动或本就处于启动态；其他错误表示启动失败。
 */
esp_err_t captive_portal_dns_start(void)
{
    BaseType_t task_created = pdFALSE;

    if (s_dns_server != NULL)
    {
        return ESP_OK;
    }

    s_dns_server = calloc(1, sizeof(*s_dns_server));
    ESP_RETURN_ON_FALSE(s_dns_server != NULL, ESP_ERR_NO_MEM, TAG,
                        "分配 DNS 服务句柄失败");

    s_dns_server->started = true;
    s_dns_server->socket_fd = -1;
    task_created = xTaskCreate(captive_portal_dns_task, "captive_dns", 4096,
                               s_dns_server, 5, &s_dns_server->task);
    if (task_created != pdPASS)
    {
        free(s_dns_server);
        s_dns_server = NULL;
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief 停止 Captive Portal 使用的 DNS 劫持服务。
 *
 * @return `ESP_OK` 表示服务已停止或本就未启动；其他错误表示停止失败。
 */
esp_err_t captive_portal_dns_stop(void)
{
    captive_portal_dns_server_t *server = s_dns_server;

    if (server == NULL)
    {
        return ESP_OK;
    }

    s_dns_server = NULL;
    server->started = false;

    /* 先主动关闭 socket，再删任务，避免 `recvfrom()` 长时间占着 53 端口导致后续 SoftAP
     * 周期重启时 DNS bind 失败。 */
    if (server->socket_fd != -1)
    {
        shutdown(server->socket_fd, 0);
        close(server->socket_fd);
        server->socket_fd = -1;
    }
    if (server->task != NULL)
    {
        vTaskDelete(server->task);
        server->task = NULL;
    }

    free(server);
    return ESP_OK;
}

/**
 * @brief 查询 DNS 劫持服务当前是否处于运行态。
 *
 * @return `true` 表示服务已启动；`false` 表示服务未启动。
 */
bool captive_portal_dns_is_running(void)
{
    return s_dns_server != NULL;
}
