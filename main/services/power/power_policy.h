#ifndef POWER_POLICY_H
#define POWER_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * 整机资源策略层：
 * - 只把电源快照和运行场景翻译成资源预算；
 * - 不直接读 PMIC 寄存器，不直接操作 LVGL 对象，也不直接启动模型；
 * - 第一阶段接入电源、维护窗口和 UI 活跃度事实，发布运行态 STANDBY 预算。
 */

#ifdef __cplusplus
extern "C"
{
#endif

    /** 整机产品活跃状态；V1 只允许 ACTIVE / STANDBY。 */
    typedef enum
    {
        POWER_POLICY_STATE_ACTIVE = 0,       /**< 正常交互或普通运行态。 */
        POWER_POLICY_STATE_STANDBY,          /**< 运行态待机，不进入 ESP sleep。 */
    } power_policy_state_t;

    /** STANDBY 进入原因；V1 只用于日志和后续扩展，不改变待机档位。 */
    typedef enum
    {
        POWER_POLICY_STANDBY_REASON_NONE = 0,      /**< 当前不处于 STANDBY。 */
        POWER_POLICY_STANDBY_REASON_AUTO_IDLE,     /**< UI 空闲自动进入 STANDBY。 */
        POWER_POLICY_STANDBY_REASON_USER_SCREEN_OFF, /**< 用户主动熄屏，预留。 */
        POWER_POLICY_STANDBY_REASON_LOW_BATTERY_POLICY, /**< 低电量策略触发，预留。 */
        POWER_POLICY_STANDBY_REASON_SYSTEM_REQUEST, /**< 系统主动请求，预留。 */
    } power_policy_standby_reason_t;

    /** 显示预算，只表达目标强度，不包含具体面板调用。 */
    typedef enum
    {
        POWER_POLICY_DISPLAY_FULL = 0, /**< 全亮显示。 */
        POWER_POLICY_DISPLAY_DIM,      /**< 降亮显示，预留。 */
        POWER_POLICY_DISPLAY_OFF,      /**< 近似熄屏或亮度为 0。 */
    } power_policy_display_budget_t;

    /** UI 刷新预算。 */
    typedef enum
    {
        POWER_POLICY_UI_HIGH_REFRESH = 0, /**< 允许高刷新。 */
        POWER_POLICY_UI_LOW_REFRESH,      /**< 降低 LVGL 唤醒频率。 */
    } power_policy_ui_budget_t;

    /** 网络预算，只表达同步许可和省电强度。 */
    typedef enum
    {
        POWER_POLICY_NETWORK_FULL = 0,    /**< 普通联网和同步。 */
        POWER_POLICY_NETWORK_POWER_SAVE,  /**< Wi-Fi runtime 省电，预留。 */
        POWER_POLICY_NETWORK_SYNC_PAUSED, /**< 暂停非关键同步，保留连接。 */
    } power_policy_network_budget_t;

    /** 后台任务预算。 */
    typedef enum
    {
        POWER_POLICY_BACKGROUND_FULL = 0, /**< 后台任务按用户意图运行。 */
        POWER_POLICY_BACKGROUND_THROTTLED, /**< 后台任务降频，预留。 */
        POWER_POLICY_BACKGROUND_PAUSE_OPTIONAL, /**< 暂停可暂停后台任务。 */
    } power_policy_background_budget_t;

    /** CPU 预算；V1 只发布语义，不直接配置 DFS / pm lock。 */
    typedef enum
    {
        POWER_POLICY_CPU_PERFORMANCE = 0, /**< 性能优先。 */
        POWER_POLICY_CPU_BALANCED,        /**< 平衡模式，预留。 */
        POWER_POLICY_CPU_LOW,             /**< 低功耗倾向。 */
    } power_policy_cpu_budget_t;

    /** 电源观测轮询预算。 */
    typedef enum
    {
        POWER_POLICY_POWER_POLL_NORMAL = 0, /**< 正常轮询。 */
        POWER_POLICY_POWER_POLL_SLOW,       /**< 降低轮询频率。 */
    } power_policy_power_poll_budget_t;

    /** 当前最多允许进入的 ESP sleep 深度。 */
    typedef enum
    {
        POWER_POLICY_SLEEP_NONE = 0,   /**< 当前不允许 sleep。 */
        POWER_POLICY_SLEEP_LIGHT_ALLOWED, /**< 条件允许显式 Light Sleep 测试。 */
        POWER_POLICY_SLEEP_DEEP_ALLOWED,  /**< V1 普通路径不产生，仅预留。 */
    } power_policy_sleep_permission_t;

    /** sleep blocker 位图；具体语义由对应 owner 的事实快照提供。 */
    typedef enum
    {
        POWER_POLICY_SLEEP_BLOCKER_NONE = 0,
        POWER_POLICY_SLEEP_BLOCKER_UI_FORCE_ACTIVE = 1u << 0,   /**< UI 强制活跃。 */
        POWER_POLICY_SLEEP_BLOCKER_AUDIO_ACTIVE = 1u << 1,      /**< 音频活跃，预留。 */
        POWER_POLICY_SLEEP_BLOCKER_NETWORK_CRITICAL = 1u << 2,  /**< 网络关键任务，预留。 */
        POWER_POLICY_SLEEP_BLOCKER_BACKGROUND_CRITICAL = 1u << 3, /**< 后台关键任务，预留。 */
        POWER_POLICY_SLEEP_BLOCKER_OTA_ACTIVE = 1u << 4,        /**< OTA 活跃，预留。 */
        POWER_POLICY_SLEEP_BLOCKER_PROVISIONING_ACTIVE = 1u << 5, /**< 配网活跃，预留。 */
        POWER_POLICY_SLEEP_BLOCKER_ALERT_ACTIVE = 1u << 6,      /**< P0 提醒活跃，预留。 */
        POWER_POLICY_SLEEP_BLOCKER_DEBUG_LOCK = 1u << 7,        /**< 调试锁定，预留。 */
    } power_policy_sleep_blocker_t;

    /** 预算修饰 flag；不是产品状态。 */
    typedef enum
    {
        POWER_POLICY_FLAG_NONE = 0,
        POWER_POLICY_FLAG_LOW_BATTERY_WARN = 1u << 0, /**< 低电量预警事实。 */
        POWER_POLICY_FLAG_EXTERNAL_POWER = 1u << 1,   /**< 外部供电存在。 */
        POWER_POLICY_FLAG_CHARGING = 1u << 2,         /**< 正在充电。 */
        POWER_POLICY_FLAG_MAINTENANCE = 1u << 3,      /**< 维护窗口活跃。 */
    } power_policy_flag_t;

    /** 触发 power_policy task 重新聚合预算的原因位。 */
    typedef enum
    {
        POWER_POLICY_NOTIFY_NONE = 0,
        POWER_POLICY_NOTIFY_UI_ACTIVITY = 1u << 0, /**< UI 活跃度或 STANDBY 快照变化。 */
        POWER_POLICY_NOTIFY_POWER_STATE = 1u << 1, /**< 电源、电池或外部供电事实变化。 */
        POWER_POLICY_NOTIFY_MAINTENANCE = 1u << 2, /**< 维护窗口进入或退出。 */
        POWER_POLICY_NOTIFY_MANUAL = 1u << 3,      /**< 调试或启动阶段主动请求重算。 */
        POWER_POLICY_NOTIFY_PERIODIC = 1u << 4,    /**< task 周期兜底重算。 */
        POWER_POLICY_NOTIFY_AUDIO = 1u << 5,       /**< 音频会话变化（power_policy_audio_bridge 上报）。 */
    } power_policy_notify_reason_t;

    /** power_policy 输出给后台服务和资源 owner 的只读预算。 */
    typedef struct
    {
        power_policy_state_t state;       /**< 当前整机资源状态。 */
        power_policy_standby_reason_t standby_reason; /**< STANDBY 原因。 */
        power_policy_display_budget_t display_budget; /**< 显示预算。 */
        power_policy_ui_budget_t ui_budget; /**< UI 刷新预算。 */
        power_policy_network_budget_t network_budget; /**< 网络预算。 */
        power_policy_background_budget_t background_budget; /**< 后台任务预算。 */
        power_policy_cpu_budget_t cpu_budget; /**< CPU 预算语义。 */
        power_policy_power_poll_budget_t power_poll_budget; /**< 电源观测预算。 */
        power_policy_sleep_permission_t sleep_permission; /**< sleep 许可。 */
        uint32_t sleep_blockers;          /**< power_policy_sleep_blocker_t 位图。 */
        uint32_t flags;                   /**< power_policy_flag_t 位图。 */
        uint32_t sleep_interval_hint_ms;  /**< sleep_coordinator dry-run 使用的建议间隔。 */
        bool danger_detection_allowed;    /**< 是否允许后台危险识别运行。 */
        bool network_sync_allowed;        /**< 是否允许普通后台网络同步。 */
        bool maintenance_allowed;         /**< 是否允许模型验证、OTA、日志导出等维护任务。 */
        bool ui_high_refresh_allowed;     /**< UI 是否允许保持 active 刷新预算。 */
        bool haptic_alert_allowed;        /**< 未来接入震动后，P0 提醒是否允许使用触觉通道。 */
        bool low_battery_warn;            /**< 低电量预警事实；V1 不改变预算、不触发 sleep。 */
        bool external_power_present;      /**< 当前是否检测到外部供电。 */
        bool battery_data_valid;          /**< 电池电量和电压字段是否可信。 */
        uint8_t battery_percent;          /**< 电量百分比；仅在 battery_data_valid=true 时有效。 */
        uint16_t battery_mv;              /**< 电池电压，单位为毫伏。 */
        uint32_t budget_version;          /**< 预算快照版本；每次有效变化递增。 */
        uint32_t last_notify_reasons;     /**< 最近一次触发预算重算的原因位。 */
    } power_policy_budget_t;

    /** 省电事实提供方标识；只用于注册表索引和日志，不表达生命周期 owner。 */
    typedef enum
    {
        POWER_POLICY_PROVIDER_NONE = 0,
        POWER_POLICY_PROVIDER_SAFETY_MONITOR,      /**< Safety Monitor 关键运行事实。 */
        POWER_POLICY_PROVIDER_AUDIO,               /**< 音频会话事实（power_policy_audio_bridge 接入）。 */
        POWER_POLICY_PROVIDER_RUNTIME_COORDINATOR, /**< runtime_coordinator 单一当前 owner 快照。 */
        POWER_POLICY_PROVIDER_NETWORK_CRITICAL,    /**< 网络关键 blocker，预留。 */
        POWER_POLICY_PROVIDER_ALERT_OWNER,         /**< P0 告警 blocker，预留。 */
        POWER_POLICY_PROVIDER_DEBUG,               /**< 调试/测试专用登记项，预留。 */
        POWER_POLICY_PROVIDER_COUNT,
    } power_policy_provider_id_t;

    /** 登记项能力位；facts 与 consumer 是两个独立能力，可同时具备。 */
    typedef enum
    {
        POWER_POLICY_PARTICIPANT_FACTS_ONLY = 1u << 0,         /**< 只提供省电事实。 */
        POWER_POLICY_PARTICIPANT_CONSUMER_ONLY = 1u << 1,      /**< 只接收预算变更并唤醒自己的 task。 */
        POWER_POLICY_PARTICIPANT_FACTS_AND_CONSUMER =
            (1u << 0) | (1u << 1),                             /**< 同时提供事实并消费预算。 */
    } power_policy_participant_capability_t;

    /** provider 上报的省电事实最小集合。 */
    typedef struct
    {
        bool running;                /**< 当前是否运行中（关键或普通）。 */
        bool must_keep_alive;        /**< 关键识别/不可中断阶段；只影响 blocker 聚合，不是新预算字段。 */
        bool can_defer_work;         /**< 普通非关键工作是否可延后。 */
        uint32_t sleep_blockers;     /**< power_policy_sleep_blocker_t 位图，由真实资源 owner 提供。 */
        esp_err_t last_error;        /**< 最近一次自身错误，可选。 */
    } power_policy_provider_facts_t;

    /** provider 事实回调：只能快速复制 owner snapshot，不能做 I/O/网络/音频/Flash/模型或等待其他 task。 */
    typedef esp_err_t (*power_policy_get_facts_cb_t)(
        power_policy_provider_facts_t *facts, void *context);

    /** 预算变更通知回调：只能向自己的 owner task 发 task notification 或 queue 消息，真实动作由 owner task 执行。 */
    typedef esp_err_t (*power_policy_budget_changed_cb_t)(
        uint32_t budget_version, void *context);

    /** 省电参与者登记配置；注册事实和能力，不注册 task/resource 生命周期控制权。 */
    typedef struct
    {
        power_policy_provider_id_t id; /**< 稳定 id，注册表索引。 */
        const char *name;              /**< 日志友好名称。 */
        uint32_t capabilities;         /**< power_policy_participant_capability_t 位图。 */
        power_policy_get_facts_cb_t get_facts; /**< 可选；facts-only/facts+consumer 必须提供。 */
        power_policy_budget_changed_cb_t on_budget_changed; /**< 可选；consumer-only/facts+consumer 必须提供。 */
        void *context;                 /**< 回调上下文，可为 NULL。 */
    } power_policy_participant_config_t;

    /**
     * @brief 初始化整机资源策略层。
     * @return ESP_OK 表示初始化成功或已经初始化。
     */
    esp_err_t power_policy_init(void);

    /**
     * @brief 启用整机资源策略层。
     *
     * 第一阶段没有独立任务，只发布初始预算日志；调用方通过
     * `power_policy_get_budget()` 获取最新预算。
     *
     * @return ESP_OK 表示策略层可用。
     */
    esp_err_t power_policy_start(void);

    /**
     * @brief 进入或退出高压维护窗口。
     *
     * 维护窗口用于 OTA、模型替换、模型验证和日志导出等会与 Safety Monitor
     * 争用麦克风、模型 RAM、Flash 或网络吞吐的任务。该接口只记录策略请求，
     * 不直接启动维护任务，也不直接停止具体 runtime；后台 manager 会通过预算
     * 看到危险识别不再允许运行。
     *
     * @param[in] active true 表示进入维护窗口，false 表示退出维护窗口。
     * @param[in] reason 日志原因，可为 NULL。
     * @return ESP_OK 表示状态已记录。
     */
    esp_err_t power_policy_set_maintenance_window(bool active,
                                                  const char *reason);

    /**
     * @brief 通知 power_policy task 需要尽快重算资源预算。
     *
     * notify 只是 FreeRTOS 唤醒信号，不携带最终事实；task 被唤醒后仍通过各
     * owner 的 snapshot API 读取真实状态，避免把状态写权集中到 power_policy。
     *
     * @param[in] reason power_policy_notify_reason_t 位图。
     * @return ESP_OK 表示通知已发送或 task 尚未启动但同步兜底已执行。
     */
    esp_err_t power_policy_notify(uint32_t reason);

    /**
     * @brief 获取当前资源预算。
     * @return 最近一次由 power_policy task 发布的资源预算；task 未启动时会同步兜底计算。
     */
    power_policy_budget_t power_policy_get_budget(void);

    /**
     * @brief 将资源策略状态转换成日志友好的文本。
     * @param[in] state 资源策略状态。
     * @return 静态字符串。
     */
    const char *power_policy_state_text(power_policy_state_t state);

    /**
     * @brief 将 sleep 许可转换成日志友好的文本。
     * @param[in] permission sleep 许可。
     * @return 静态字符串。
     */
    const char *power_policy_sleep_permission_text(
        power_policy_sleep_permission_t permission);

    /**
     * @brief 将 sleep blocker 位图格式化成日志文本。
     *
     * @param[in] blockers power_policy_sleep_blocker_t 位图。
     * @param[out] buffer 输出缓冲区。
     * @param[in] buffer_size 输出缓冲区大小，单位字节。
     */
    void power_policy_format_sleep_blockers(uint32_t blockers, char *buffer,
                                            size_t buffer_size);

    /**
     * @brief 注册省电参与者（facts-only / consumer-only / facts+consumer）。
     *
     * 注册只发生在 service 初始化阶段：固定容量为 8，不提供运行期卸载；同一
     * id 重复注册必须幂等，冲突配置返回 `ESP_ERR_INVALID_ARG`。power_policy
     * 只保存静态配置和聚合结果，不保存业务 task handle，也不调用
     * stop/start/deinit。
     *
     * @param[in] config 参与者配置，不能为 NULL。
     * @return `ESP_OK` 表示注册成功或幂等成功；`ESP_ERR_INVALID_STATE` 表示
     *         policy task 已启动后注册；`ESP_ERR_NO_MEM` 表示表已满。
     */
    esp_err_t power_policy_register_participant(
        const power_policy_participant_config_t *config);

    /**
     * @brief 获取当前预算版本号。
     * @return 最近一次发布的预算版本；每次有效预算变化递增。
     */
    uint32_t power_policy_budget_version(void);

    /**
     * @brief 向所有注册了 on_budget_changed 的参与者广播预算变更。
     *
     * 回调只在预算实际变化时被调用，且不在 power_policy 锁内执行；回调只允许
     * 唤醒自己的 owner task，不允许在 policy task 上下文执行业务动作。
     *
     * @return `ESP_OK` 表示通知已广播。
     */
    esp_err_t power_policy_budget_changed_notify(void);

    /**
     * @brief 注册音频事实跨层桥接。
     *
     * 该接口属于 `power_policy_audio_bridge` 的注册入口：audio_codec 保持
     * AUDIO_ACTIVE 事实 owner，bridge 只读取其非阻塞 cached session snapshot
     * 并把 input/output active 映射为 `POWER_POLICY_SLEEP_BLOCKER_AUDIO_ACTIVE`。
     * 调用时机要求 audio codec 已初始化且 power_policy task 尚未启动。
     *
     * @return `ESP_OK` 表示注册成功。
     */
    esp_err_t power_policy_audio_bridge_register(void);

#ifdef __cplusplus
}
#endif

#endif // POWER_POLICY_H
