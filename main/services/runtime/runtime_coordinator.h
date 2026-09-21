#ifndef RUNTIME_COORDINATOR_H
#define RUNTIME_COORDINATOR_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        RUNTIME_COORDINATOR_PARTICIPANT_NONE = 0,
        RUNTIME_COORDINATOR_PARTICIPANT_HERMES,
        RUNTIME_COORDINATOR_PARTICIPANT_OFFICIAL_CHAT,
        RUNTIME_COORDINATOR_PARTICIPANT_NETWORK_PROVISIONING,
        RUNTIME_COORDINATOR_PARTICIPANT_OTA,
        RUNTIME_COORDINATOR_PARTICIPANT_SAFETY_MONITOR,
        RUNTIME_COORDINATOR_PARTICIPANT_BLE_PRESENCE,
        RUNTIME_COORDINATOR_PARTICIPANT_TEST_OWNER,
        RUNTIME_COORDINATOR_PARTICIPANT_TEST_BACKGROUND,
        RUNTIME_COORDINATOR_PARTICIPANT_COUNT,
    } runtime_coordinator_participant_id_t;

    typedef enum
    {
        RUNTIME_COORDINATOR_CAPABILITY_NONE = 0,
        RUNTIME_COORDINATOR_CAPABILITY_FOREGROUND_EXCLUSIVE = 1U << 0,
        RUNTIME_COORDINATOR_CAPABILITY_BACKGROUND_PREEMPTIBLE = 1U << 1,
    } runtime_coordinator_capability_t;

    typedef enum
    {
        RUNTIME_COORDINATOR_STATE_IDLE = 0,
        RUNTIME_COORDINATOR_STATE_QUIESCING,
        RUNTIME_COORDINATOR_STATE_GRANTING,
        RUNTIME_COORDINATOR_STATE_ACTIVE,
        RUNTIME_COORDINATOR_STATE_ROLLING_BACK,
        RUNTIME_COORDINATOR_STATE_DEGRADED_CURRENT,
        RUNTIME_COORDINATOR_STATE_DEGRADED_TARGET,
    } runtime_coordinator_state_t;

    typedef esp_err_t (*runtime_coordinator_quiesce_cb_t)(
        uint32_t transition_generation, void *user_ctx);
    typedef esp_err_t (*runtime_coordinator_grant_cb_t)(
        uint32_t request_generation, void *user_ctx);
    typedef esp_err_t (*runtime_coordinator_cancel_cb_t)(
        uint32_t request_generation, esp_err_t reason, void *user_ctx);
    typedef esp_err_t (*runtime_coordinator_reevaluate_cb_t)(
        uint32_t transition_generation, void *user_ctx);

    /** participant 只提供向自身 owner task 投递命令的非阻塞回调。 */
    typedef struct
    {
        runtime_coordinator_participant_id_t id;
        const char *name;
        uint32_t capabilities;
        runtime_coordinator_quiesce_cb_t request_quiesce;
        runtime_coordinator_grant_cb_t grant_foreground;
        runtime_coordinator_cancel_cb_t cancel_pending_request;
        runtime_coordinator_reevaluate_cb_t request_reevaluate;
        void *user_ctx;
    } runtime_coordinator_participant_config_t;

    /** coordinator 对外只读快照。 */
    typedef struct
    {
        bool started;
        runtime_coordinator_state_t state;
        runtime_coordinator_participant_id_t current_owner;
        runtime_coordinator_participant_id_t target_owner;
        runtime_coordinator_participant_id_t provisional_owner;
        runtime_coordinator_participant_id_t error_participant;
        uint32_t request_generation;
        uint32_t active_request_generation;
        uint32_t transition_generation;
        uint32_t waiting_mask;
        esp_err_t last_error;
    } runtime_coordinator_snapshot_t;

    /** 初始化静态 queue 和 coordinator 状态。 */
    esp_err_t runtime_coordinator_init(void);

    /** 启动仅处理控制消息的 internal-stack coordinator task。 */
    esp_err_t runtime_coordinator_start(void);

    /** 注册固定 participant；重复注册同一 ID 幂等。 */
    esp_err_t runtime_coordinator_register(
        const runtime_coordinator_participant_config_t *config);

    /** 异步请求强前台，真实授权通过 participant grant 回调返回 owner task。 */
    esp_err_t runtime_coordinator_request_foreground(
        runtime_coordinator_participant_id_t id,
        uint32_t *out_request_generation);

    /** 取消尚未完成或正在运行的强前台请求。 */
    esp_err_t runtime_coordinator_cancel_request(
        runtime_coordinator_participant_id_t id,
        uint32_t request_generation);

    /** participant 报告指定排空代次的真实停止结果。 */
    esp_err_t runtime_coordinator_report_quiesce_result(
        runtime_coordinator_participant_id_t id,
        uint32_t transition_generation,
        esp_err_t result);

    /** 强前台 participant 报告 grant 后的真实启动结果。 */
    esp_err_t runtime_coordinator_report_start_result(
        runtime_coordinator_participant_id_t id,
        uint32_t request_generation,
        esp_err_t result);

    /** DEGRADED participant 明确报告资源仍 active，用于恢复 fail-closed 状态。 */
    esp_err_t runtime_coordinator_report_active(
        runtime_coordinator_participant_id_t id);

    runtime_coordinator_snapshot_t runtime_coordinator_get_snapshot(void);
    const char *runtime_coordinator_participant_text(
        runtime_coordinator_participant_id_t id);
    const char *runtime_coordinator_state_text(runtime_coordinator_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* RUNTIME_COORDINATOR_H */
