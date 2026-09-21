#ifndef WAKEUP_EVIDENCE_SERVICE_H
#define WAKEUP_EVIDENCE_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * RTC/PMIC 唤醒证据服务：
 * - 只做上板证据采集，不进入 ESP light/deep sleep；
 * - 通过 PCF85063ATL countdown timer 验证 RTC_INT(GPIO39)；
 * - 通过 AXP2101 IRQ 状态读清验证 PMIC 事件是否可观测。
 */

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 初始化 RTC/PMIC 唤醒证据服务。
     * @return ESP_OK 表示初始化成功或已经初始化。
     */
    esp_err_t wakeup_evidence_service_init(void);

    /**
     * @brief 启动 RTC/PMIC 唤醒证据后台任务。
     * @return ESP_OK 表示任务已启动或之前已启动。
     */
    esp_err_t wakeup_evidence_service_start(void);

#ifdef __cplusplus
}
#endif

#endif // WAKEUP_EVIDENCE_SERVICE_H
