#ifndef POWER_SERVICE_H
#define POWER_SERVICE_H

#include "app/board_power.h"

/*
 * 电源服务层：
 * - 周期采样 `board_power`，为 UI 和上层业务提供稳定、只读的电量快照；
 * - 通过双缓冲与临界区切换，避免读取方拿到半更新状态；
 * - 在采样失败时保留最近一次有效数据，并标记为 stale，便于界面做降级展示。
 */

#ifdef __cplusplus
extern "C"
{
#endif

    /* 电源状态变化回调，运行在电源服务后台任务上下文。 */
    /* 回调参数和 get_state 返回值都是服务层拥有的只读快照视图，
     * 如需长期持有请自行复制。 */
    typedef void (*power_state_changed_cb_t)(const board_power_state_t *state);

    /* 初始化服务内部缓存，不创建后台任务。 */
    esp_err_t power_service_init(void);

    /* 启动后台采样任务；重复调用安全。
     * 采样节奏由实现层动态调整：成功约 1s，连续失败会退避到 2s/5s。 */
    esp_err_t power_service_start(void);

    /* 注册状态变化回调；回调在服务任务上下文执行。 */
    void power_service_register_callback(power_state_changed_cb_t cb);

    /* 复制当前电源状态快照；getter 不做 I/O、不阻塞、不推进采样状态机。 */
    esp_err_t power_service_get_snapshot(board_power_state_t *out_state);

    /* 返回服务层拥有的只读快照视图；兼容旧调用方，新增代码优先用 get_snapshot(out)。 */
    const board_power_state_t *power_service_get_state(void);

#ifdef __cplusplus
}
#endif

#endif // POWER_SERVICE_H
