#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**< 官方聊天组件的对外不透明句柄。用于多实例隔离和隐藏 C++ 实现。 */
typedef struct official_chat_handle *official_chat_handle_t;

/**
 * @brief 设备端当前交互状态枚举。
 * 反映内部状态机所处节点，供用户界面 (UI) 进行动画切换或录音状态反馈。
 */
typedef enum {
    OFFICIAL_CHAT_STATE_UNKNOWN = 0,    /**< 初始/未知状态，尚未完成探测或通信已中断。 */
    OFFICIAL_CHAT_STATE_ACTIVATING,     /**< 尚在激活/配网/拉取鉴权配置阶段阶段。 */
    OFFICIAL_CHAT_STATE_UPGRADING,      /**< 正在进行 OTA 或固件升级，此时应阻塞其他交互。 */
    OFFICIAL_CHAT_STATE_IDLE,           /**< 闲置状态，随时可触发唤醒。 */
    OFFICIAL_CHAT_STATE_CONNECTING,     /**< 正在与服务器建立通信信道。 */
    OFFICIAL_CHAT_STATE_LISTENING,      /**< 正在收录麦克风音频以供后续识别/对讲。 */
    OFFICIAL_CHAT_STATE_SPEAKING,       /**< 正在播放远端返回的音频/TTS数据。 */
} official_chat_state_t;

/**
 * @brief 透传给应用层的事件类型枚举。
 * 包含状态机变更、文字展示、升级等各类通知，以便将底层模型状态更新给前端。
 */
typedef enum {
    OFFICIAL_CHAT_EVENT_STATE_CHANGED = 0,    /**< 状态机流转通知。 */
    OFFICIAL_CHAT_EVENT_ACTIVATION_CODE,      /**< 设备获取到设备激活码，用于展示在屏幕或日志。 */
    OFFICIAL_CHAT_EVENT_ACTIVATION_MESSAGE,   /**< 验证阶段的附加文本提示事件。 */
    OFFICIAL_CHAT_EVENT_USER_TEXT,            /**< 用户说出且被系统 ASR 识别的文字结果展示。 */
    OFFICIAL_CHAT_EVENT_ASSISTANT_TEXT,       /**< AI 助手回复的 TTS 字幕事件展示。 */
    OFFICIAL_CHAT_EVENT_ASSETS_PROGRESS,      /**< 下载模型或静态资源的进度通知。 */
    OFFICIAL_CHAT_EVENT_UPGRADE_PROGRESS,     /**< OTA 固件下载进度通知。 */
    OFFICIAL_CHAT_EVENT_ERROR,                /**< 组件出现严重软硬件级系统或通信错误。 */
    OFFICIAL_CHAT_EVENT_REBOOTING,            /**< 组件提示即将软重启生效。 */
} official_chat_event_type_t;

/**
 * @brief 组件透传至应用层回调的通用事件载体。
 *
 * 并非每个字段在每次事件中都有意义；具体有效字段取决于 type 枚举。
 */
typedef struct {
    official_chat_event_type_t type;    /**< 事件基础分类 */
    official_chat_state_t state;        /**< 当前所处的设备互动状态 (对 STATE_CHANGED 事件有意义) */
    const char *message;                /**< 文字信息承载（ASR、TTS或错误描述字符串），使用完后不必手动释放 */
    int progress;                       /**< 当前百分比进度 (0-100)，对进度类事件有意义 */
    size_t speed_bytes_per_sec;         /**< 下载或传输的实时速率(Byte/s)，供界面参考 */
    esp_err_t error;                    /**< 驱动/通信底层返回的具体 esp_err_t 错误码（如有） */
} official_chat_event_t;

/**
 * @brief 组件回调函数签名。
 *
 * @param[in] event 最新触发的事件详情。指针仅在回调有效，切勿异步持久化该指针。
 * @param[in] user_data 用户注册时透传的上下文参数。
 *
 * @note 调用发生在组件的事件回放或工作线程，切勿在此回调内执行无限阻塞逻辑或高耗时操作。
 */
typedef void (*official_chat_event_callback_t)(const official_chat_event_t *event,
                                               void *user_data);

/**
 * @brief official_chat 在 HTTPS/TLS 请求前向外层索要可信系统时间的回调。
 *
 * 组件自身不直接启动 SNTP，也不访问 RTC；外层可把该回调接到统一
 * system_time owner。
 *
 * @param[in] timeout_ms 最大等待时间，单位为毫秒。
 * @param[in] user_ctx 调用方透传上下文。
 * @return ESP_OK 表示系统时间已可信。
 */
typedef esp_err_t (*official_chat_ensure_time_cb_t)(uint32_t timeout_ms,
                                                    void *user_ctx);

/**
 * @brief official_chat 将服务端可信时间交给外层 owner 的回调。
 *
 * 组件自身不直接调用 `settimeofday()`，避免形成第二个系统时间 owner。
 *
 * @param[in] unix_seconds Unix 时间戳，单位为秒。
 * @param[in] user_ctx 调用方透传上下文。
 * @return ESP_OK 表示外层已接受该时间。
 */
typedef esp_err_t (*official_chat_apply_server_time_cb_t)(int64_t unix_seconds,
                                                          void *user_ctx);

/**
 * @brief 官方聊天组件启动的基础配置结构体。
 */
typedef struct {
    int speak_volume;             /**< 初始 TTS 播放音量大小 (0-100)。 */
    float record_gain_db;         /**< 麦克风录音模拟增益 (dB)，影响唤醒率和拾音灵敏度。 */
    const char *websocket_url;    /**< 建立长链接端点的可选 WebSocket 地址（由协议动态管理时可为 NULL）。 */
    const char *access_token;     /**< 初始设备授权 Token（可选）。 */
    const char *ota_url;          /**< 对应的 OTA 升级服务器地址池。 */
    official_chat_ensure_time_cb_t ensure_time_valid;       /**< HTTPS/TLS 前确保系统时间可信的外部回调。 */
    official_chat_apply_server_time_cb_t apply_server_time; /**< 接收服务端可信时间的外部回调。 */
    void *time_user_ctx;                                     /**< 时间回调透传上下文。 */
} official_chat_config_t;

/**
 * @brief MQTT 子协议连接配置。
 */
typedef struct {
    const char *endpoint;         /**< MQTT 接入点 URL 或 IP。 */
    const char *publish_topic;    /**< 设备推送上行事件/音频的目标主题。 */
    const char *client_id;        /**< 设备自身的唯一 Client ID 标识。 */
    const char *username;         /**< MQTT 鉴权用户名（如有）。 */
    const char *password;         /**< MQTT 鉴权密码（如有）。 */
    int keepalive;                /**< 协议 Ping 超时保活心跳时间参数，单位：秒。 */
} official_chat_mqtt_config_t;

/**
 * @brief WebSocket 端点重载配置。
 */
typedef struct {
    const char *url;              /**< WebSocket 端点新地址。 */
    const char *token;            /**< 新下发的凭据或 Session Key。 */
    int version;                  /**< 配置下发的版本号或流水控制字。 */
} official_chat_websocket_config_t;

/**
 * @brief 创建一个 Official Chat 组件实例。
 *
 * 分配资源、实例化内部工作队列及相关基础服务。
 * @param[in] config 组件应用基础参数。如果传 NULL，则使用内置默认参数。
 * @return 成功返回不透明句柄；失败返回 NULL。
 *
 * @note 内部会发起大量动态内存分配。确保堆空间具有连续块支撑。
 */
official_chat_handle_t official_chat_create(const official_chat_config_t *config);

/**
 * @brief 释放并销毁已经分配的 Official Chat 句柄实例。
 *
 * @param[in] handle 待销毁的实例句柄。
 */
void official_chat_destroy(official_chat_handle_t handle);

/**
 * @brief 注册上层事件回调函数。
 *
 * 通过该回调将交互过程（如消息接收、录音开始、错误等）通知外层 UI 展现。
 * @param[in] handle 有效实例。
 * @param[in] callback 用户实现的回调接收地址。
 * @param[in] user_data 供回调函数的上下文指针，由调用方管理生命周期。
 * @return ESP_OK 表示绑定成功；ESP_ERR_INVALID_ARG 时表示参数句柄无效。
 */
esp_err_t official_chat_set_event_callback(official_chat_handle_t handle,
                                           official_chat_event_callback_t callback,
                                           void *user_data);

/**
 * @brief 根据设定配置，启动设备的组件业务生命流。
 * @param[in] handle 有效实例。
 * @return ESP_OK 成功唤起，开始进入激活或空闲等待。
 */
esp_err_t official_chat_start(official_chat_handle_t handle);

/**
 * @brief 安全停机前准备。
 *
 * 关断正在交互的流、屏蔽按键并释放硬件锁，为重置或掉电做准备。
 * @param[in] handle 有效实例。
 * @return ESP_OK
 */
esp_err_t official_chat_prepare_shutdown(official_chat_handle_t handle);

/**
 * @brief 预先建立语音通道但不开始录音。
 *
 * AI 页面可在进入前台后调用它完成 WebSocket/音频通道预连接，
 * 用户后续按住说话时就不需要把首次连接延迟压到按键动作上。
 *
 * @param[in] handle 有效实例。
 * @return ESP_OK 表示预连接请求已投递。
 */
esp_err_t official_chat_prepare_audio_channel(official_chat_handle_t handle);

/**
 * @brief 手动通过信令或按键触发机器开始录音监听。
 * @param[in] handle 有效实例。
 * @return ESP_OK 成功投递开始事件至核心处理状态机。
 */
esp_err_t official_chat_start_listening(official_chat_handle_t handle);

/**
 * @brief 触发人工模拟的唤醒功能。
 * @param[in] handle 有效实例。
 * @return ESP_OK
 */
esp_err_t official_chat_start_synthetic_wakeword(official_chat_handle_t handle);

/**
 * @brief 翻转当前的交互模式状态（空闲->记录，记录->空闲）。
 * @param[in] handle 有效实例。
 * @return ESP_OK
 */
esp_err_t official_chat_toggle_chat(official_chat_handle_t handle);

/**
 * @brief 主动停掉当前记录/听音逻辑。
 * @param[in] handle 有效实例。
 * @return ESP_OK
 */
esp_err_t official_chat_stop_listening(official_chat_handle_t handle);

/**
 * @brief 动态开关设备端 AEC (回声消除模块)。
 *
 * 允许运行期间根据使用场景屏蔽/开启滤波器模块，降低性能占用或优化麦克风。
 * @param[in] handle 有效实例。
 * @param[in] enabled 是否启用AEC。
 * @return ESP_OK
 */
esp_err_t official_chat_set_device_aec_enabled(official_chat_handle_t handle,
                                               bool enabled);

/**
 * @brief 查阅现有的 AEC (回声消除) 开关情况。
 * @param[in] handle 有效实例。
 * @return 开启或者关闭状态
 */
bool official_chat_get_device_aec_enabled(official_chat_handle_t handle);

/**
 * @brief 同步获取目前核心业务状态机节点。
 * @param[in] handle 有效实例。
 * @return official_chat_state_t 状态枚举
 */
official_chat_state_t official_chat_get_state(official_chat_handle_t handle);

/**
 * @brief 查询语音通道是否已经打开。
 *
 * 该接口用于服务层生成 UI 快照，避免页面只凭 IDLE 状态猜测
 * WebSocket 是否已经预连接完成。
 *
 * @param[in] handle 有效实例。
 * @return true 表示后续按住说话可直接进入录音态。
 */
bool official_chat_is_audio_channel_ready(official_chat_handle_t handle);

/**
 * @brief 全局或局部变更 MQTT 协议栈所需网络安全及队列配置。
 *
 * 调用前应确保传入的 config 字符串有效且安全，以防止建立失效的 MQTT 握手。
 * @param[in] config 指向用户配置的新参数（深拷贝入组件内部，调用完毕可释放传入结构体）。
 * @return ESP_OK
 */
esp_err_t official_chat_set_mqtt_protocol_config(
    const official_chat_mqtt_config_t *config);

/**
 * @brief 全局或局部变更 WebSocket 协议栈所需参数。
 * @param[in] config 指向配置内容结构体（内部深拷贝内容）。
 * @return ESP_OK
 */
esp_err_t official_chat_set_websocket_protocol_config(
    const official_chat_websocket_config_t *config);

/**
 * @brief 在运行时，应用因更改了 WebSocket / MQTT 配置而发起的新协议重载和信道复位。
 * @param[in] handle 有效实例。
 * @return ESP_OK 操作受入队列。
 */
esp_err_t official_chat_reload_protocol(official_chat_handle_t handle);

#ifdef __cplusplus
}
#endif
