#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include "esp_err.h"
#include "esp_timer.h"

#include "audio/audio_service.h"
#include "assets_runtime.h"
#include "audio/local_audio_codec_adapter.h"
#include "device_state_machine.h"
#include "official_chat.h"
#include "ota.h"
#include "protocol_config.h"
#include "protocols/protocol.h"

namespace official_chat {

constexpr EventBits_t kMainEventSchedule = BIT0;                 /**< 主线程队列有新任务的调度通知位。 */
constexpr EventBits_t kMainEventSendAudio = BIT1;                /**< 发送上行音频数据的通知位。 */
constexpr EventBits_t kMainEventToggleChat = BIT2;               /**< 切换聊天状态（开始/停止）的触发位。 */
constexpr EventBits_t kMainEventStartListening = BIT3;           /**< 启动录音/收音监听的指令位。 */
constexpr EventBits_t kMainEventStopListening = BIT4;            /**< 停止录音/收音监听的指令位。 */
constexpr EventBits_t kMainEventStateChanged = BIT5;             /**< 内部状态机发生变化的通知位。 */
constexpr EventBits_t kMainEventProtocolClosed = BIT6;           /**< 底层通信协议断开或报错退出的通知位。 */
constexpr EventBits_t kMainEventWakeWordDetected = BIT7;         /**< 语音模型检测到唤醒词的通知位。 */
constexpr EventBits_t kMainEventVadChange = BIT8;                /**< VAD（静音检测）状态变化的通知位。 */
constexpr EventBits_t kMainEventWorkerExited = BIT9;             /**< 后台工作任务（如异步线程）退出的通知位。 */
constexpr EventBits_t kMainEventActivationDone = BIT10;          /**< 激活流程（包括网络或配置拉取）完成的通知位。 */
constexpr EventBits_t kMainEventUpgradeProgress = BIT11;         /**< OTA/固件升级进度更新的通知位。 */
constexpr EventBits_t kMainEventGracefulButtonStopTimeout = BIT12; /**< 优雅停机超时（防止长按死锁的兜底退出）通知位。 */
constexpr EventBits_t kMainEventPrepareAudioChannel = BIT13;     /**< 只预连接音频通道，不进入录音态。 */

/**
 * @brief srmodel_list_t 的自定义删除器。
 *
 * 配合 std::unique_ptr 自动清理模型列表，确保不发生内存或句柄泄漏。
 */
struct ModelListDeleter {
  void operator()(srmodel_list_t *models) const;
};

/**
 * @brief Official Chat 业务层的核心主类。
 *
 * 统筹管理设备状态机、音频收发、网络协议及 OTA 等子模块。
 * 依靠内部的一个主循环 (RunLoop) 统一消费事件位，避免多线程直接互调造成状态竞态。
 */
class Application {
 public:
  Application();
  ~Application();

  /**
   * @brief 初始化 Application 及底层相关资源。
   *
   * 负责申请相关句柄、队列、及设置初始配置。
   *
   * @param[in] config 从外层（配置存储或默认值）读取到的官方聊天配置。
   * @return ESP_OK 表示初始化成功；错误码表示分配资源或挂载组件失败。
   *
   * @note 调用此函数前，底层外设的时钟、I2C 或 SPI 配置理论上应已经处于可用状态。
   */
  esp_err_t Initialize(const official_chat_config_t &config);

  /**
   * @brief 启动业务主循环及辅助任务。
   *
   * @return ESP_OK 表示启动成功。
   *
   * @note 启动后会在后台常驻 RunLoop 监听各类主事件。
   */
  esp_err_t Start();

  /**
   * @brief 提供安全的停机前准备。
   *
   * 保存必要状态并阻止后续新事件触发，以便应用和硬件能正常离场。
   * @return ESP_OK
   */
  esp_err_t PrepareForShutdown();

  /**
   * @brief 手动发起录音监听。
   *
   * 推送 kMainEventStartListening，主要给按键或外部 UI 等调用。
   * @return 触发成功与否状态。
   */
  esp_err_t StartListening();

  /**
   * @brief 预先建立语音通道。
   *
   * 用于 AI 页面进入后的 WebSocket 预连接。该接口只打开协议音频通道，
   * 不发送开始聆听指令，也不启用麦克风采集。
   *
   * @return ESP_OK 表示预连接事件已投递。
   */
  esp_err_t PrepareAudioChannel();

  /**
   * @brief 触发人工模拟的语音唤醒事件。
   *
   * 从外部或触摸屏直接跳过本地唤醒词模型阶段，强制进入语音交互。
   * @return ESP_OK
   */
  esp_err_t StartSyntheticWakeWord();

  /**
   * @brief 在开启、停止之间翻转当前的聊天状态。
   *
   * 推送 kMainEventToggleChat。
   * @return ESP_OK
   */
  esp_err_t ToggleChat();

  /**
   * @brief 主动停止当前正在进行的录音或交互过程。
   * @return ESP_OK
   */
  esp_err_t StopListening();

  /**
   * @brief 重载网络协议配置。
   *
   * 在变更网络凭据或端点配置后调用以使新策略生效。
   * @return ESP_OK
   */
  esp_err_t ReloadProtocol();

  /**
   * @brief 在运行时启停设备端的 AEC (回声消除)。
   *
   * @param[in] enabled true为开启，false为关闭。
   * @return ESP_OK
   */
  esp_err_t SetDeviceAecEnabled(bool enabled);

  /**
   * @brief 注册上层事件回调函数。
   *
   * 通过该回调将交互过程（如消息接收、录音开始、错误）告知 UI 层。
   * @param[in] callback 用户回调地址。
   * @param[in] user_data 透传到回调函数的用户指针。
   * @return ESP_OK
   */
  esp_err_t SetEventCallback(official_chat_event_callback_t callback,
                             void *user_data);

  /**
   * @brief 获取 AEC 是否已被启用。
   * @return 返回启停状态
   */
  bool GetDeviceAecEnabled() const;

  /**
   * @brief 获取设备当前的核心交互状态。
   * @return DeviceState 状态枚举，如 Idle / Listening / Speaking 等。
   */
  DeviceState GetState() const;

  /**
   * @brief 查询底层协议音频通道是否已打开。
   * @return true 表示按住说话可以直接进入录音态。
   */
  bool IsAudioChannelReady() const;

 private:
  struct ProgressSnapshot {
    int progress = 0;
    size_t speed_bytes_per_sec = 0;
    bool valid = false;
  };

  void Schedule(std::function<void()> &&callback);
  void RunLoop();
  void HandleStateChanged();
  void HandleActivationDoneEvent();
  void HandleUpgradeProgressEvent();
  void HandleToggleChatEvent();
  void HandlePrepareAudioChannelEvent();
  void HandleStartListeningEvent();
  void HandleStartListeningEvent(ListeningMode mode);
  void HandleStopListeningEvent();
  void HandleProtocolClosedEvent();
  void HandleWakeWordDetectedEvent();
  void HandleGracefulButtonStopTimeout();
  void StartActivationTask();
  void ActivationTask();
  void ContinuePrepareAudioChannel();
  void ContinueOpenAudioChannel(ListeningMode mode);
  void ContinueWakeWordInvoke(const std::string &wake_word);
  ListeningMode GetDefaultListeningMode() const;
  void SetListeningMode(ListeningMode mode);
  void StartGracefulButtonStop();
  void CancelGracefulButtonStop(const char *reason);
  void TryFinalizeGracefulButtonStop(const char *reason);
  void FinalizeGracefulButtonStopToIdle(bool timed_out, const char *reason);
  bool SetDeviceState(DeviceState state);
  std::string ResolveOtaUrl() const;
  void EmitEvent(official_chat_event_type_t type, official_chat_state_t state,
                 const std::string &message, int progress, size_t speed,
                 esp_err_t error);
  void EmitStateChangedEvent();
  void EmitMessageEvent(official_chat_event_type_t type,
                        const std::string &message);
  void EmitProgressEvent(official_chat_event_type_t type, int progress,
                         size_t speed);
  void EmitErrorEvent(esp_err_t error, const std::string &message);
  void EmitRebootingEvent();
  void ReplayEventSnapshot();
  void InitializeProtocol();
  esp_err_t InitializeAudioService();
  bool ShouldAcceptIncomingDownlinkAudio(DeviceState state) const;
  void SetDownlinkAudioActive(bool active, const char *reason);

  int speak_volume_ = 0;
  float record_gain_db_ = 0.0f;
  std::string websocket_url_;
  std::string access_token_;
  std::string ota_url_;
  official_chat_ensure_time_cb_t ensure_time_valid_ = nullptr;
  official_chat_apply_server_time_cb_t apply_server_time_ = nullptr;
  void *time_user_ctx_ = nullptr;
  bool has_public_websocket_config_ = false;
  std::mutex mutex_;
  std::deque<std::function<void()>> main_tasks_;
  std::unique_ptr<Protocol> protocol_;
  std::unique_ptr<LocalAudioCodecAdapter> codec_;
  std::unique_ptr<Ota> ota_;
  AssetsRuntime assets_runtime_;
  EventGroupHandle_t event_group_ = nullptr;
  esp_timer_handle_t graceful_button_stop_timer_ = nullptr;
  TaskHandle_t worker_task_handle_ = nullptr;
  TaskHandle_t activation_task_handle_ = nullptr;
  StackType_t *activation_task_stack_ = nullptr;
  StaticTask_t *activation_task_tcb_ = nullptr;
  DeviceStateMachine state_machine_;
  ListeningMode listening_mode_ = kListeningModeManualStop;
  std::unique_ptr<srmodel_list_t, ModelListDeleter> models_list_;
  AudioService audio_service_;
  bool device_aec_enabled_ = false;
  bool started_ = false;
  official_chat_event_callback_t event_callback_ = nullptr;
  void *event_callback_user_data_ = nullptr;
  bool replay_events_pending_ = false;
  std::string last_event_message_;
  ProgressSnapshot last_assets_progress_;
  ProgressSnapshot last_upgrade_progress_;
  esp_err_t last_error_ = ESP_OK;
  std::atomic<bool> downlink_audio_active_{false};
  std::atomic<uint32_t> downlink_audio_accepted_while_pending_count_{0};
  std::atomic<uint32_t> downlink_audio_dropped_by_gate_count_{0};
  std::atomic<int> downlink_audio_pending_log_budget_{3};
  std::atomic<int> downlink_audio_drop_log_budget_{5};
  std::atomic<int> downlink_audio_queue_drop_log_budget_{5};
  bool button_stop_pending_ = false;
  bool button_stop_tts_stop_seen_ = false;
  bool button_stop_channel_closed_ = false;
  bool button_stop_finalizing_ = false;
  bool wake_word_init_after_activation_pending_ = false;
  int64_t button_stop_deadline_us_ = 0;
  std::atomic<bool> shutting_down_{false};
};

}  // namespace official_chat
