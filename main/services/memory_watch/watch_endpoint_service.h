#ifndef WATCH_ENDPOINT_SERVICE_H
#define WATCH_ENDPOINT_SERVICE_H

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define WATCH_ENDPOINT_DANGER_TYPE_MAX_BYTES 32
#define WATCH_ENDPOINT_DANGER_MESSAGE_MAX_BYTES 96

    /**
     * @brief watch endpoint 危险告警上报请求。
     *
     * 该中性 facade 表示“手表向统一 endpoint 上传业务事件”，避免危险识别层
     * 直接依赖 Hermes / Memory Watch 命名。
     */
    typedef struct
    {
        const char *danger_type; /**< 危险类型，例如 `danger`、`horn`、`siren`。 */
        float danger_prob;       /**< 进入 Alerting 时的 danger 概率，范围 0..1。 */
        uint32_t alert_sequence; /**< 本机告警序号，用于云端日志去重与排查。 */
        const char *message;     /**< 给手机端展示的短提示文案。 */
    } watch_endpoint_danger_alert_t;

    /**
     * @brief 初始化 watch endpoint 后台告警 worker。
     *
     * 当前服务使用 1 深度静态 FreeRTOS queue 承接危险告警事件，并在独立
     * PSRAM worker task 内执行 HTTPS POST，避免阻塞 ESP-DL 推理回调。
     *
     * @return `ESP_OK` 表示已初始化或本次初始化成功。
     */
    esp_err_t watch_endpoint_service_init(void);

    /**
     * @brief 异步上报一条危险声音告警到 watch endpoint。
     *
     * 调用方只表达业务事件；实际 endpoint 配置快照、网络 ready 判断、
     * worker queue 和 HTTPS POST 由本服务承接。
     *
     * @param[in] alert 已确认的危险告警。
     * @return `ESP_OK` 表示已投递到后台 worker。
     */
    esp_err_t watch_endpoint_service_post_danger_alert(
        const watch_endpoint_danger_alert_t *alert);

#ifdef __cplusplus
}
#endif

#endif // WATCH_ENDPOINT_SERVICE_H
