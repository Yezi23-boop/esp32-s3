#include "application.h"

#include "sdkconfig.h"

#include <cstring>
#include <utility>
#include <vector>

#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>

#include "mcp_server.h"
#include "protocol_config.h"
#include "protocols/mqtt_protocol.h"
#include "protocols/websocket_protocol.h"
#include "settings.h"
#include "wifi_control.h"

namespace official_chat
{

  namespace
  {

    constexpr char kTag[] = "official_app";
    constexpr char kApplicationWorkerTaskName[] = "official_chat";
    constexpr char kActivationTaskName[] = "official_ota";
    constexpr char kDefaultWebsocketUrl[] = "wss://api.tenclass.net/xiaozhi/v1/";
    constexpr char kDefaultAccessToken[] = "test-token";
    constexpr float kDefaultRecordGainDb = 24.0f;
    constexpr uint32_t kApplicationWorkerTaskStackBytes = 8192;
    constexpr uint32_t kActivationTaskStackBytes = 4096;
    constexpr TickType_t kWorkerStopTimeoutTicks = pdMS_TO_TICKS(1000);
    constexpr TickType_t kActivationStopTimeoutTicks = pdMS_TO_TICKS(2000);
    constexpr TickType_t kWakeWordEnableAfterActivationDelayTicks =
        pdMS_TO_TICKS(20);
    constexpr int kMaxActivationAttempts = 10;
    constexpr int64_t kGracefulButtonStopTimeoutUs = 3000000;
    constexpr int kGracefulButtonStopDrainQuietMs = 200;
    const char *DeviceStateToString(DeviceState state)
    {
      switch (state)
      {
      case DeviceState::kActivating:
        return "activating";
      case DeviceState::kUpgrading:
        return "upgrading";
      case DeviceState::kIdle:
        return "idle";
      case DeviceState::kConnecting:
        return "connecting";
      case DeviceState::kListening:
        return "listening";
      case DeviceState::kSpeaking:
        return "speaking";
      case DeviceState::kUnknown:
      default:
        return "unknown";
      }
    }

    official_chat_state_t ToPublicState(DeviceState state)
    {
      switch (state)
      {
      case DeviceState::kActivating:
        return OFFICIAL_CHAT_STATE_ACTIVATING;
      case DeviceState::kUpgrading:
        return OFFICIAL_CHAT_STATE_UPGRADING;
      case DeviceState::kIdle:
        return OFFICIAL_CHAT_STATE_IDLE;
      case DeviceState::kConnecting:
        return OFFICIAL_CHAT_STATE_CONNECTING;
      case DeviceState::kListening:
        return OFFICIAL_CHAT_STATE_LISTENING;
      case DeviceState::kSpeaking:
        return OFFICIAL_CHAT_STATE_SPEAKING;
      case DeviceState::kUnknown:
      default:
        return OFFICIAL_CHAT_STATE_UNKNOWN;
      }
    }

    const char *ListeningModeToString(ListeningMode mode)
    {
      switch (mode)
      {
      case kListeningModeAutoStop:
        return "auto";
      case kListeningModeManualStop:
        return "manual";
      case kListeningModeRealtime:
        return "realtime";
      default:
        return "unknown";
      }
    }

    void LogJsonTextField(const cJSON *root, const char *type_name,
                          const char *field_name)
    {
      const cJSON *field = cJSON_GetObjectItem(root, field_name);
      if (cJSON_IsString(field))
      {
        ESP_LOGI(kTag, "%s %s: %s", type_name, field_name, field->valuestring);
      }
      else
      {
        ESP_LOGI(kTag, "%s without %s", type_name, field_name);
      }
    }

    const char *GetJsonStringField(const cJSON *root, const char *field_name)
    {
      const cJSON *field = cJSON_GetObjectItem(root, field_name);
      if (!cJSON_IsString(field))
      {
        return nullptr;
      }
      return field->valuestring;
    }

    void LogWorkerStackHighWatermark(const char *reason)
    {
      const UBaseType_t watermark =
          uxTaskGetStackHighWaterMark(xTaskGetCurrentTaskHandle());
      ESP_LOGI(kTag, "worker stack high watermark (%s): %u", reason,
               static_cast<unsigned>(watermark));
    }

    void UpdateWifiPowerSaveForState(DeviceState state)
    {
      switch (state)
      {
      case DeviceState::kConnecting:
      case DeviceState::kListening:
      case DeviceState::kSpeaking:
        wifi_control_set_power_save(false);
        break;
      case DeviceState::kActivating:
      case DeviceState::kUpgrading:
      case DeviceState::kIdle:
      case DeviceState::kUnknown:
      default:
        wifi_control_set_power_save(true);
        break;
      }
    }

  } // namespace

  void ModelListDeleter::operator()(srmodel_list_t *models) const
  {
    if (models != nullptr)
    {
      esp_srmodel_deinit(models);
    }
  }

  Application::Application()
  {
    event_group_ = xEventGroupCreate();
#if CONFIG_USE_DEVICE_AEC
    device_aec_enabled_ = true;
#endif
    esp_timer_create_args_t graceful_button_stop_timer_args = {
        .callback =
            [](void *arg)
        {
          auto *self = static_cast<Application *>(arg);
          if (self != nullptr && self->event_group_ != nullptr)
          {
            xEventGroupSetBits(self->event_group_,
                               kMainEventGracefulButtonStopTimeout);
          }
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "official_btn_stop",
        .skip_unhandled_events = true,
    };
    const esp_err_t timer_err = esp_timer_create(&graceful_button_stop_timer_args,
                                                 &graceful_button_stop_timer_);
    if (timer_err != ESP_OK)
    {
      graceful_button_stop_timer_ = nullptr;
      ESP_LOGW(kTag, "failed to create graceful button stop timer: %s",
               esp_err_to_name(timer_err));
    }
  }

  Application::~Application()
  {
    started_ = false;
    McpServer::GetInstance().SetSendCallback({});
    McpServer::GetInstance().SetRuntimeStatusCallback({});
    if (event_group_ != nullptr)
    {
      xEventGroupSetBits(event_group_, kMainEventSchedule | kMainEventSendAudio |
                                           kMainEventToggleChat |
                                           kMainEventStartListening |
                                           kMainEventStopListening |
                                           kMainEventStateChanged |
                                           kMainEventProtocolClosed |
                                           kMainEventWakeWordDetected |
                                           kMainEventVadChange |
                                           kMainEventActivationDone |
                                           kMainEventUpgradeProgress |
                                           kMainEventGracefulButtonStopTimeout);
      const EventBits_t bits =
          xEventGroupWaitBits(event_group_, kMainEventWorkerExited, pdTRUE,
                              pdTRUE, kWorkerStopTimeoutTicks);
      if ((bits & kMainEventWorkerExited) == 0 && worker_task_handle_ != nullptr)
      {
        ESP_LOGW(kTag, "worker task did not exit before shutdown");
      }
    }
    if (activation_task_handle_ != nullptr)
    {
      const int64_t start_time = esp_timer_get_time();
      while (activation_task_handle_ != nullptr &&
             esp_timer_get_time() - start_time <
                 (kActivationStopTimeoutTicks * 1000))
      {
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      if (activation_task_handle_ != nullptr)
      {
        ESP_LOGW(kTag, "activation task did not exit before shutdown");
      }
    }
    free(activation_task_stack_);
    activation_task_stack_ = nullptr;
    free(activation_task_tcb_);
    activation_task_tcb_ = nullptr;
    if (protocol_)
    {
      protocol_->CloseAudioChannel(false);
      protocol_.reset();
    }
    if (graceful_button_stop_timer_ != nullptr)
    {
      esp_timer_stop(graceful_button_stop_timer_);
      esp_timer_delete(graceful_button_stop_timer_);
      graceful_button_stop_timer_ = nullptr;
    }
    audio_service_.Stop();
    if (codec_)
    {
      codec_->Shutdown();
      codec_.reset();
    }
    if (event_group_ != nullptr)
    {
      vEventGroupDelete(event_group_);
      event_group_ = nullptr;
    }
  }

  esp_err_t Application::Initialize(const official_chat_config_t &config)
  {
    speak_volume_ = config.speak_volume;
    record_gain_db_ =
        config.record_gain_db > 0.0f ? config.record_gain_db : kDefaultRecordGainDb;
    has_public_websocket_config_ =
        (config.websocket_url != nullptr && config.websocket_url[0] != '\0') ||
        (config.access_token != nullptr && config.access_token[0] != '\0');
    websocket_url_ = config.websocket_url != nullptr && config.websocket_url[0] != '\0'
                         ? config.websocket_url
                         : kDefaultWebsocketUrl;
    access_token_ = config.access_token != nullptr && config.access_token[0] != '\0'
                        ? config.access_token
                        : kDefaultAccessToken;
    ota_url_ = config.ota_url != nullptr ? config.ota_url : "";
    ensure_time_valid_ = config.ensure_time_valid;
    apply_server_time_ = config.apply_server_time;
    time_user_ctx_ = config.time_user_ctx;
    state_machine_.TransitionTo(DeviceState::kUnknown);
    return ESP_OK;
  }

  esp_err_t Application::Start()
  {
    if (started_)
    {
      return ESP_OK;
    }
    shutting_down_.store(false, std::memory_order_release);

    codec_ = std::make_unique<LocalAudioCodecAdapter>();
    if (!codec_ || !codec_->Initialize())
    {
      ESP_LOGE(kTag, "audio codec adapter init failed");
      return ESP_FAIL;
    }
    codec_->SetOutputVolume(speak_volume_);
    codec_->SetInputGain(record_gain_db_);

    if (InitializeAudioService() != ESP_OK)
    {
      ESP_LOGE(kTag, "audio service init failed");
      codec_->Shutdown();
      codec_.reset();
      return ESP_FAIL;
    }

    ota_url_ = ResolveOtaUrl();
    if (ota_url_.empty())
    {
      ESP_LOGE(kTag, "official_chat start requires ota_url");
      EmitErrorEvent(ESP_ERR_INVALID_ARG, "official_chat start requires ota_url");
      audio_service_.Stop();
      codec_->Shutdown();
      codec_.reset();
      return ESP_ERR_INVALID_ARG;
    }

    started_ = true;
    xEventGroupClearBits(event_group_, kMainEventWorkerExited);

    if (xTaskCreate(
            [](void *arg)
            {
              auto *self = static_cast<Application *>(arg);
              self->RunLoop();
              self->worker_task_handle_ = nullptr;
              if (self->event_group_ != nullptr)
              {
                xEventGroupSetBits(self->event_group_, kMainEventWorkerExited);
              }
              vTaskDelete(nullptr);
            },
            kApplicationWorkerTaskName, kApplicationWorkerTaskStackBytes, this, 6,
            &worker_task_handle_) != pdPASS)
    {
      started_ = false;
      ESP_LOGE(kTag, "failed to create application worker task");
      protocol_.reset();
      audio_service_.Stop();
      if (codec_)
      {
        codec_->Shutdown();
      }
      return ESP_FAIL;
    }

    Schedule([this]()
             {
    SetDeviceState(DeviceState::kActivating);
    ReplayEventSnapshot(); });
    StartActivationTask();
    return ESP_OK;
  }

  esp_err_t Application::PrepareForShutdown()
  {
    if (!started_ || event_group_ == nullptr)
    {
      return ESP_OK;
    }

    const bool already_shutting_down =
        shutting_down_.exchange(true, std::memory_order_acq_rel);
    if (already_shutting_down)
    {
      return ESP_OK;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      main_tasks_.clear();
    }

    xEventGroupClearBits(event_group_,
                         kMainEventSchedule | kMainEventSendAudio |
                             kMainEventToggleChat | kMainEventStartListening |
                             kMainEventPrepareAudioChannel |
                             kMainEventWakeWordDetected |
                             kMainEventActivationDone |
                             kMainEventUpgradeProgress | kMainEventVadChange);
    xEventGroupSetBits(event_group_, kMainEventStateChanged);
    ESP_LOGI(kTag, "prepare shutdown armed");
    return ESP_OK;
  }

  esp_err_t Application::StartListening()
  {
    if (!started_ || event_group_ == nullptr ||
        shutting_down_.load(std::memory_order_acquire))
    {
      return ESP_ERR_INVALID_STATE;
    }
    xEventGroupSetBits(event_group_, kMainEventStartListening);
    return ESP_OK;
  }

  esp_err_t Application::PrepareAudioChannel()
  {
    if (!started_ || event_group_ == nullptr ||
        shutting_down_.load(std::memory_order_acquire))
    {
      return ESP_ERR_INVALID_STATE;
    }
    xEventGroupSetBits(event_group_, kMainEventPrepareAudioChannel);
    return ESP_OK;
  }

  esp_err_t Application::StartSyntheticWakeWord()
  {
    if (!started_ || event_group_ == nullptr ||
        shutting_down_.load(std::memory_order_acquire))
    {
      return ESP_ERR_INVALID_STATE;
    }
    Schedule([this]()
             { HandleStartListeningEvent(GetDefaultListeningMode()); });
    return ESP_OK;
  }

  esp_err_t Application::ToggleChat()
  {
    if (!started_ || event_group_ == nullptr ||
        shutting_down_.load(std::memory_order_acquire))
    {
      return ESP_ERR_INVALID_STATE;
    }
    xEventGroupSetBits(event_group_, kMainEventToggleChat);
    return ESP_OK;
  }

  esp_err_t Application::StopListening()
  {
    if (!started_ || event_group_ == nullptr)
    {
      return ESP_ERR_INVALID_STATE;
    }
    /*
     * StopListening 是 shutdown 收敛路径的一部分：PrepareForShutdown 会先拉起
     * shutting_down_ fence 来屏蔽新的 start/toggle/wakeword，但此时仍必须允许
     * 服务层投递停止事件，把 connecting/listening/speaking 状态推进回 idle。
     */
    xEventGroupSetBits(event_group_, kMainEventStopListening);
    return ESP_OK;
  }

  esp_err_t Application::ReloadProtocol()
  {
    if (!started_ || event_group_ == nullptr ||
        shutting_down_.load(std::memory_order_acquire))
    {
      return ESP_ERR_INVALID_STATE;
    }
    Schedule([this]()
             {
    ESP_LOGI(kTag, "reload protocol requested");
    CancelGracefulButtonStop("protocol-reload");
    SetDownlinkAudioActive(false, "protocol-reload");

    while (audio_service_.PopPacketFromSendQueue()) {
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
      protocol_->CloseAudioChannel(false);
    }
    protocol_.reset();
    InitializeProtocol();
    if (!protocol_) {
      ESP_LOGE(kTag, "failed to reload protocol");
      EmitErrorEvent(ESP_FAIL, "failed to reload protocol");
      return;
    }
    SetDeviceState(DeviceState::kIdle); });
    return ESP_OK;
  }

  esp_err_t Application::SetDeviceAecEnabled(bool enabled)
  {
    device_aec_enabled_ = enabled;
    Schedule([this, enabled]()
             {
    ESP_LOGI(kTag, "set device aec enabled: %d", enabled ? 1 : 0);
    audio_service_.EnableDeviceAec(enabled);
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
      protocol_->CloseAudioChannel(false);
    }
    if (GetState() != DeviceState::kIdle) {
      SetDeviceState(DeviceState::kIdle);
    } else {
      audio_service_.EnableWakeWordDetection(true);
    } });
    return ESP_OK;
  }

  esp_err_t Application::SetEventCallback(official_chat_event_callback_t callback,
                                          void *user_data)
  {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      event_callback_ = callback;
      event_callback_user_data_ = user_data;
      replay_events_pending_ = true;
    }
    Schedule([this]()
             { ReplayEventSnapshot(); });
    return ESP_OK;
  }

  bool Application::GetDeviceAecEnabled() const { return device_aec_enabled_; }

  DeviceState Application::GetState() const { return state_machine_.GetState(); }

  bool Application::IsAudioChannelReady() const
  {
    return protocol_ != nullptr && protocol_->IsAudioChannelOpened();
  }

  bool Application::ShouldAcceptIncomingDownlinkAudio(DeviceState state) const
  {
    return state == DeviceState::kSpeaking ||
           downlink_audio_active_.load(std::memory_order_acquire);
  }

  void Application::SetDownlinkAudioActive(bool active, const char *reason)
  {
    const bool previous =
        downlink_audio_active_.exchange(active, std::memory_order_acq_rel);
    if (previous != active)
    {
      ESP_LOGI(kTag, "downlink audio gate %s reason=%s", active ? "open" : "closed",
               reason != nullptr ? reason : "<none>");
    }
  }

  void Application::StartGracefulButtonStop()
  {
    button_stop_pending_ = true;
    button_stop_tts_stop_seen_ = false;
    button_stop_channel_closed_ = false;
    button_stop_finalizing_ = false;
    button_stop_deadline_us_ = esp_timer_get_time() + kGracefulButtonStopTimeoutUs;
    if (graceful_button_stop_timer_ != nullptr)
    {
      esp_timer_stop(graceful_button_stop_timer_);
      const esp_err_t timer_err = esp_timer_start_once(
          graceful_button_stop_timer_, kGracefulButtonStopTimeoutUs);
      if (timer_err != ESP_OK)
      {
        ESP_LOGW(kTag, "failed to arm graceful button stop timer: %s",
                 esp_err_to_name(timer_err));
      }
    }
    ESP_LOGI(kTag, "graceful button stop armed timeout_ms=%lld",
             static_cast<long long>(kGracefulButtonStopTimeoutUs / 1000));
  }

  void Application::CancelGracefulButtonStop(const char *reason)
  {
    const bool had_pending = button_stop_pending_ || button_stop_tts_stop_seen_ ||
                             button_stop_channel_closed_ ||
                             button_stop_finalizing_;
    if (graceful_button_stop_timer_ != nullptr)
    {
      esp_timer_stop(graceful_button_stop_timer_);
    }
    button_stop_pending_ = false;
    button_stop_tts_stop_seen_ = false;
    button_stop_channel_closed_ = false;
    button_stop_finalizing_ = false;
    button_stop_deadline_us_ = 0;
    if (had_pending)
    {
      ESP_LOGI(kTag, "graceful button stop cleared reason=%s",
               reason != nullptr ? reason : "<none>");
    }
  }

  void Application::FinalizeGracefulButtonStopToIdle(bool timed_out,
                                                     const char *reason)
  {
    if (!button_stop_pending_)
    {
      return;
    }
    if (graceful_button_stop_timer_ != nullptr)
    {
      esp_timer_stop(graceful_button_stop_timer_);
    }

    ESP_LOGI(kTag, "graceful button stop finalize mode=%s reason=%s",
             timed_out ? "timeout" : "drained",
             reason != nullptr ? reason : "<none>");

    if (timed_out)
    {
      if (protocol_ && protocol_->IsAudioChannelOpened())
      {
        protocol_->CloseAudioChannel(false);
      }
      audio_service_.ResetDecoder();
    }
    else if (protocol_ && protocol_->IsAudioChannelOpened())
    {
      protocol_->CloseAudioChannel(true);
    }

    button_stop_pending_ = false;
    button_stop_tts_stop_seen_ = false;
    button_stop_channel_closed_ = false;
    button_stop_finalizing_ = false;
    button_stop_deadline_us_ = 0;
    SetDeviceState(DeviceState::kIdle);
  }

  void Application::TryFinalizeGracefulButtonStop(const char *reason)
  {
    if (!button_stop_pending_)
    {
      return;
    }
    if (button_stop_finalizing_)
    {
      ESP_LOGI(kTag,
               "graceful button stop finalize already in progress reason=%s",
               reason != nullptr ? reason : "<none>");
      return;
    }

    const bool server_signaled_stop =
        button_stop_tts_stop_seen_ || button_stop_channel_closed_ || !protocol_ ||
        !protocol_->IsAudioChannelOpened();
    if (!server_signaled_stop)
    {
      ESP_LOGI(kTag,
               "graceful button stop waiting for tts.stop or channel close reason=%s",
               reason != nullptr ? reason : "<none>");
      return;
    }

    const int64_t now_us = esp_timer_get_time();
    const int timeout_ms = now_us >= button_stop_deadline_us_
                               ? 0
                               : static_cast<int>((button_stop_deadline_us_ -
                                                   now_us) /
                                                  1000);
    if (timeout_ms <= 0)
    {
      FinalizeGracefulButtonStopToIdle(true, "deadline-expired");
      return;
    }

    button_stop_finalizing_ = true;
    ESP_LOGI(kTag,
             "graceful button stop draining downlink reason=%s timeout_ms=%d",
             reason != nullptr ? reason : "<none>", timeout_ms);
    const bool drained = audio_service_.WaitForDownlinkPlaybackDrain(
        kGracefulButtonStopDrainQuietMs, timeout_ms);
    button_stop_finalizing_ = false;

    if (!button_stop_pending_)
    {
      return;
    }
    if (!drained)
    {
      FinalizeGracefulButtonStopToIdle(true, "drain-timeout");
      return;
    }
    FinalizeGracefulButtonStopToIdle(false, reason);
  }

  void Application::HandleGracefulButtonStopTimeout()
  {
    if (!button_stop_pending_)
    {
      return;
    }
    ESP_LOGW(kTag, "graceful button stop timeout fired");
    FinalizeGracefulButtonStopToIdle(true, "timer");
  }

  void Application::Schedule(std::function<void()> &&callback)
  {
    if (!started_ || event_group_ == nullptr ||
        shutting_down_.load(std::memory_order_acquire))
    {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, kMainEventSchedule);
  }

  void Application::RunLoop()
  {
    const EventBits_t all_events =
        kMainEventSchedule | kMainEventSendAudio | kMainEventToggleChat |
        kMainEventStartListening | kMainEventStopListening |
        kMainEventStateChanged | kMainEventProtocolClosed |
        kMainEventWakeWordDetected | kMainEventVadChange |
        kMainEventActivationDone | kMainEventUpgradeProgress |
        kMainEventGracefulButtonStopTimeout | kMainEventPrepareAudioChannel;

    LogWorkerStackHighWatermark("startup");

    while (started_)
    {
      const EventBits_t bits = xEventGroupWaitBits(
          event_group_, all_events, pdTRUE, pdFALSE, portMAX_DELAY);

      if (bits & kMainEventProtocolClosed)
      {
        HandleProtocolClosedEvent();
      }

      if (bits & kMainEventActivationDone)
      {
        if (shutting_down_.load(std::memory_order_acquire))
        {
          ESP_LOGI(kTag, "shutdown ignoring activation done event");
        }
        else
        {
          HandleActivationDoneEvent();
        }
      }

      if (bits & kMainEventUpgradeProgress)
      {
        HandleUpgradeProgressEvent();
      }

      if (bits & kMainEventStateChanged)
      {
        HandleStateChanged();
      }

      if (bits & kMainEventToggleChat)
      {
        if (shutting_down_.load(std::memory_order_acquire))
        {
          ESP_LOGI(kTag, "shutdown ignoring toggle chat event");
        }
        else
        {
          HandleToggleChatEvent();
        }
      }

      if (bits & kMainEventPrepareAudioChannel)
      {
        if (shutting_down_.load(std::memory_order_acquire))
        {
          ESP_LOGI(kTag, "shutdown ignoring prepare audio channel event");
        }
        else
        {
          HandlePrepareAudioChannelEvent();
        }
      }

      if (bits & kMainEventStartListening)
      {
        if (shutting_down_.load(std::memory_order_acquire))
        {
          ESP_LOGI(kTag, "shutdown ignoring start listening event");
        }
        else
        {
          HandleStartListeningEvent();
        }
      }

      if (bits & kMainEventStopListening)
      {
        HandleStopListeningEvent();
      }

      if (bits & kMainEventGracefulButtonStopTimeout)
      {
        HandleGracefulButtonStopTimeout();
      }

      if (bits & kMainEventSendAudio)
      {
        while (auto packet = audio_service_.PopPacketFromSendQueue())
        {
          if (!protocol_ || !protocol_->IsAudioChannelOpened() ||
              !protocol_->SendAudio(std::move(packet)))
          {
            break;
          }
        }
      }

      if (bits & kMainEventWakeWordDetected)
      {
        if (shutting_down_.load(std::memory_order_acquire))
        {
          ESP_LOGI(kTag, "shutdown ignoring wake word event");
        }
        else
        {
          HandleWakeWordDetectedEvent();
        }
      }

      if (bits & kMainEventVadChange)
      {
        ESP_LOGD(kTag, "vad change event: %d",
                 audio_service_.IsVoiceDetected() ? 1 : 0);
      }

      if (bits & kMainEventSchedule)
      {
        std::deque<std::function<void()>> tasks;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          tasks = std::move(main_tasks_);
        }
        for (auto &task : tasks)
        {
          task();
        }
      }
    }
  }

  void Application::HandleStateChanged()
  {
    const DeviceState state = GetState();
    ESP_LOGI(kTag, "HandleStateChanged state=%s", DeviceStateToString(state));
    UpdateWifiPowerSaveForState(state);
    if (shutting_down_.load(std::memory_order_acquire))
    {
      CancelGracefulButtonStop("shutdown");
      SetDownlinkAudioActive(false, "shutdown");
      audio_service_.EnableVoiceProcessing(false);
      audio_service_.EnableWakeWordDetection(false);
      audio_service_.ResetDecoder();
      return;
    }
    switch (state)
    {
    case DeviceState::kActivating:
    case DeviceState::kUpgrading:
      CancelGracefulButtonStop("non-audio state");
      SetDownlinkAudioActive(false, "non-audio state");
      audio_service_.EnableVoiceProcessing(false);
      audio_service_.EnableWakeWordDetection(false);
      audio_service_.ResetDecoder();
      break;
    case DeviceState::kIdle:
      CancelGracefulButtonStop("idle");
      SetDownlinkAudioActive(false, "idle");
      audio_service_.EnableVoiceProcessing(false);
      if (wake_word_init_after_activation_pending_)
      {
        wake_word_init_after_activation_pending_ = false;
        ESP_LOGI(kTag,
                 "delaying wake word initialization after activation wait_ms=%lu",
                 static_cast<unsigned long>(
                     kWakeWordEnableAfterActivationDelayTicks *
                     portTICK_PERIOD_MS));
        vTaskDelay(kWakeWordEnableAfterActivationDelayTicks);
      }
      audio_service_.EnableWakeWordDetection(true);
      audio_service_.ResetDecoder();
      break;
    case DeviceState::kConnecting:
      CancelGracefulButtonStop("connecting");
      SetDownlinkAudioActive(false, "connecting");
      break;
    case DeviceState::kListening:
      CancelGracefulButtonStop("listening");
      SetDownlinkAudioActive(false, "listening");
      if (protocol_ && protocol_->IsAudioChannelOpened())
      {
        ESP_LOGI(kTag, "sending listen/start for mode=%s",
                 ListeningModeToString(listening_mode_));
        protocol_->SendStartListening(listening_mode_);
      }
      else
      {
        ESP_LOGW(kTag, "listening state entered without open audio channel");
      }
      audio_service_.EnableVoiceProcessing(true);
#if CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
      audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
      audio_service_.EnableWakeWordDetection(false);
#endif
      break;
    case DeviceState::kSpeaking:
      audio_service_.EnableVoiceProcessing(false);
      audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
      break;
    case DeviceState::kUnknown:
    default:
      CancelGracefulButtonStop("unknown");
      break;
    }
  }

  void Application::HandleActivationDoneEvent()
  {
    ESP_LOGI(kTag, "HandleActivationDoneEvent");
    if (!protocol_)
    {
      InitializeProtocol();
    }
    // Release activation-only state before idle re-enables wake word/AFE.
    ota_.reset();
    wake_word_init_after_activation_pending_ = true;
    if (protocol_)
    {
      SetDeviceState(DeviceState::kIdle);
    }
    else
    {
      EmitErrorEvent(ESP_FAIL, "failed to initialize protocol after activation");
    }
  }

  void Application::HandleUpgradeProgressEvent()
  {
    if (last_upgrade_progress_.valid)
    {
      EmitProgressEvent(OFFICIAL_CHAT_EVENT_UPGRADE_PROGRESS,
                        last_upgrade_progress_.progress,
                        last_upgrade_progress_.speed_bytes_per_sec);
    }
  }

  void Application::HandleToggleChatEvent()
  {
    const DeviceState state = GetState();
    ESP_LOGI(kTag, "HandleToggleChatEvent state=%s",
             DeviceStateToString(state));

    if (state == DeviceState::kActivating || state == DeviceState::kUpgrading)
    {
      ESP_LOGI(kTag, "toggle chat ignored while %s", DeviceStateToString(state));
      return;
    }

    if (state == DeviceState::kIdle)
    {
      HandleStartListeningEvent(GetDefaultListeningMode());
      return;
    }

    if (state == DeviceState::kSpeaking)
    {
      if (protocol_ && protocol_->IsAudioChannelOpened())
      {
        protocol_->SendAbortSpeaking(kAbortReasonNone);
      }
      StartGracefulButtonStop();
      TryFinalizeGracefulButtonStop("toggle-chat");
      return;
    }

    if (state == DeviceState::kConnecting || state == DeviceState::kListening)
    {
      HandleStopListeningEvent();
    }
  }

  void Application::HandlePrepareAudioChannelEvent()
  {
    const DeviceState state = GetState();
    ESP_LOGI(kTag, "HandlePrepareAudioChannelEvent state=%s",
             DeviceStateToString(state));

    if (!protocol_)
    {
      ESP_LOGE(kTag, "protocol not initialized");
      return;
    }
    if (state == DeviceState::kActivating || state == DeviceState::kUpgrading ||
        state == DeviceState::kListening || state == DeviceState::kSpeaking)
    {
      ESP_LOGI(kTag, "prepare audio channel ignored while %s",
               DeviceStateToString(state));
      return;
    }
    if (state == DeviceState::kConnecting)
    {
      ESP_LOGI(kTag, "prepare audio channel ignored while already connecting");
      return;
    }
    if (protocol_->IsAudioChannelOpened())
    {
      EmitStateChangedEvent();
      return;
    }

    SetDeviceState(DeviceState::kConnecting);
    Schedule([this]()
             { ContinuePrepareAudioChannel(); });
  }

  void Application::HandleStartListeningEvent()
  {
    HandleStartListeningEvent(kListeningModeManualStop);
  }

  void Application::HandleStartListeningEvent(ListeningMode mode)
  {
    if (!protocol_)
    {
      ESP_LOGE(kTag, "protocol not initialized");
      return;
    }

    const DeviceState state = GetState();
    ESP_LOGI(kTag, "HandleStartListeningEvent state=%s mode=%s",
             DeviceStateToString(state), ListeningModeToString(mode));
    if (state == DeviceState::kActivating || state == DeviceState::kUpgrading)
    {
      ESP_LOGI(kTag, "start listening ignored while %s",
               DeviceStateToString(state));
      return;
    }
    if (state == DeviceState::kIdle)
    {
      if (!protocol_->IsAudioChannelOpened())
      {
        SetDeviceState(DeviceState::kConnecting);
        Schedule([this, mode]()
                 { ContinueOpenAudioChannel(mode); });
        return;
      }
      SetListeningMode(mode);
      return;
    }

    if (state == DeviceState::kSpeaking)
    {
      protocol_->SendAbortSpeaking(kAbortReasonNone);
      SetListeningMode(mode);
    }
  }

  void Application::HandleStopListeningEvent()
  {
    const DeviceState state = GetState();
    if (state != DeviceState::kConnecting && state != DeviceState::kListening &&
        state != DeviceState::kSpeaking)
    {
      return;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened())
    {
      if (state == DeviceState::kListening)
      {
        protocol_->SendStopListening();
      }
      protocol_->CloseAudioChannel(false);
    }
    SetDeviceState(DeviceState::kIdle);
  }

  void Application::HandleProtocolClosedEvent()
  {
    ESP_LOGI(kTag, "HandleProtocolClosedEvent");
    if (button_stop_pending_)
    {
      button_stop_channel_closed_ = true;
      if (audio_service_.HasPendingDownlinkPlayback())
      {
        ESP_LOGI(
            kTag,
            "protocol closed while graceful button stop pending, waiting for local playback drain");
      }
      TryFinalizeGracefulButtonStop("protocol-closed");
      return;
    }
    SetDeviceState(DeviceState::kIdle);
  }

  void Application::HandleWakeWordDetectedEvent()
  {
    if (!protocol_)
    {
      return;
    }

    const DeviceState state = GetState();
    if (state == DeviceState::kActivating || state == DeviceState::kUpgrading)
    {
      ESP_LOGI(kTag, "wake word ignored while %s", DeviceStateToString(state));
      return;
    }
    const std::string wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(kTag, "Wake word detected: %s (state=%s)", wake_word.c_str(),
             DeviceStateToString(state));
    LogWorkerStackHighWatermark("before wake word");

    if (state == DeviceState::kIdle)
    {
      audio_service_.EncodeWakeWord();
      LogWorkerStackHighWatermark("after wake word encode");
      if (!protocol_->IsAudioChannelOpened())
      {
        SetDeviceState(DeviceState::kConnecting);
        Schedule(
            [this, wake_word]()
            { ContinueWakeWordInvoke(wake_word); });
        return;
      }
      ContinueWakeWordInvoke(wake_word);
      return;
    }

    if (state == DeviceState::kListening)
    {
      protocol_->SendAbortSpeaking(kAbortReasonWakeWordDetected);
      while (audio_service_.PopPacketFromSendQueue())
      {
      }
      protocol_->SendStartListening(GetDefaultListeningMode());
      audio_service_.ResetDecoder();
#if CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
      audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#endif
      return;
    }

    if (state == DeviceState::kSpeaking)
    {
      protocol_->SendAbortSpeaking(kAbortReasonWakeWordDetected);
      while (audio_service_.PopPacketFromSendQueue())
      {
      }
      SetListeningMode(GetDefaultListeningMode());
    }
  }

  void Application::StartActivationTask()
  {
    if (!started_)
    {
      return;
    }
    if (activation_task_handle_ != nullptr)
    {
      ESP_LOGW(kTag, "activation task already running");
      return;
    }

    // 栈从 PSRAM 分配，避免 internal RAM 碎片化导致 xTaskCreate 失败
    // TCB 必须在 internal RAM（FreeRTOS 硬性约束）
    activation_task_tcb_ = static_cast<StaticTask_t *>(
        heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
    activation_task_stack_ = static_cast<StackType_t *>(
        heap_caps_malloc(kActivationTaskStackBytes, MALLOC_CAP_SPIRAM));
    if (activation_task_tcb_ == nullptr || activation_task_stack_ == nullptr)
    {
      ESP_LOGE(kTag, "failed to alloc activation task memory");
      free(activation_task_stack_);
      activation_task_stack_ = nullptr;
      free(activation_task_tcb_);
      activation_task_tcb_ = nullptr;
      EmitErrorEvent(ESP_FAIL, "failed to create activation task");
      Schedule([this]()
               { SetDeviceState(DeviceState::kIdle); });
      return;
    }

    activation_task_handle_ = xTaskCreateStatic(
        [](void *arg)
        {
          auto *self = static_cast<Application *>(arg);
          self->ActivationTask();
          self->activation_task_handle_ = nullptr;
          vTaskDelete(nullptr);
        },
        kActivationTaskName, kActivationTaskStackBytes / sizeof(StackType_t),
        this, 5, activation_task_stack_, activation_task_tcb_);

    if (activation_task_handle_ == nullptr)
    {
      ESP_LOGE(kTag, "failed to create activation task (static)");
      free(activation_task_stack_);
      activation_task_stack_ = nullptr;
      free(activation_task_tcb_);
      activation_task_tcb_ = nullptr;
      EmitErrorEvent(ESP_FAIL, "failed to create activation task");
      Schedule([this]()
               { SetDeviceState(DeviceState::kIdle); });
    }
  }

  void Application::ActivationTask()
  {
    // OTA 及 assets 下载涉及 flash API，栈在 PSRAM 时会导致 cache freeze crash
    // 暂时跳过，直接完成激活流程
    ESP_LOGI(kTag, "activation task: OTA/assets skipped (PSRAM stack)");

    if (event_group_ != nullptr)
    {
      xEventGroupSetBits(event_group_, kMainEventActivationDone);
    }
  }

  void Application::ContinuePrepareAudioChannel()
  {
    const DeviceState state = GetState();
    ESP_LOGI(kTag, "ContinuePrepareAudioChannel state=%s",
             DeviceStateToString(state));
    if (!protocol_ || state != DeviceState::kConnecting)
    {
      return;
    }
    if (!protocol_->IsAudioChannelOpened() && !protocol_->OpenAudioChannel())
    {
      ESP_LOGW(kTag, "failed to prepare audio channel");
      SetDeviceState(DeviceState::kIdle);
      return;
    }
    SetDeviceState(DeviceState::kIdle);
  }

  void Application::ContinueOpenAudioChannel(ListeningMode mode)
  {
    const DeviceState state = GetState();
    ESP_LOGI(kTag, "ContinueOpenAudioChannel state=%s mode=%s",
             DeviceStateToString(state), ListeningModeToString(mode));
    if (!protocol_ || state != DeviceState::kConnecting)
    {
      return;
    }
    if (!protocol_->IsAudioChannelOpened() && !protocol_->OpenAudioChannel())
    {
      ESP_LOGW(kTag, "failed to open audio channel while connecting");
      SetDeviceState(DeviceState::kIdle);
      return;
    }
    SetListeningMode(mode);
  }

  void Application::ContinueWakeWordInvoke(const std::string &wake_word)
  {
    if (!protocol_ || GetState() != DeviceState::kConnecting)
    {
      return;
    }

    if (!protocol_->IsAudioChannelOpened() && !protocol_->OpenAudioChannel())
    {
      ESP_LOGW(kTag, "failed to open audio channel after wake word");
      SetDeviceState(DeviceState::kIdle);
      return;
    }

    ESP_LOGI(kTag, "ContinueWakeWordInvoke wake_word=%s", wake_word.c_str());
    LogWorkerStackHighWatermark("before wake word uplink");
#if CONFIG_SEND_WAKE_WORD_DATA
    while (auto packet = audio_service_.PopWakeWordPacket())
    {
      if (!protocol_->SendAudio(std::move(packet)))
      {
        break;
      }
    }
    protocol_->SendWakeWordDetected(wake_word);
#endif
    SetListeningMode(GetDefaultListeningMode());
  }

  ListeningMode Application::GetDefaultListeningMode() const
  {
    if (device_aec_enabled_)
    {
      return kListeningModeRealtime;
    }
    return kListeningModeAutoStop;
  }

  void Application::SetListeningMode(ListeningMode mode)
  {
    listening_mode_ = mode;
    ESP_LOGI(kTag, "SetListeningMode mode=%s", ListeningModeToString(mode));
    SetDeviceState(DeviceState::kListening);
  }

  std::string Application::ResolveOtaUrl() const
  {
    if (!ota_url_.empty())
    {
      return ota_url_;
    }
    Settings settings("wifi", false);
    return settings.GetString("ota_url");
  }

  void Application::EmitEvent(official_chat_event_type_t type,
                              official_chat_state_t state,
                              const std::string &message, int progress,
                              size_t speed, esp_err_t error)
  {
    official_chat_event_callback_t callback = nullptr;
    void *user_data = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      last_event_message_ = message;
      callback = event_callback_;
      user_data = event_callback_user_data_;
    }

    if (callback == nullptr)
    {
      return;
    }

    official_chat_event_t event = {};
    event.type = type;
    event.state = state;
    event.message = last_event_message_.empty() ? nullptr : last_event_message_.c_str();
    event.progress = progress;
    event.speed_bytes_per_sec = speed;
    event.error = error;
    callback(&event, user_data);
  }

  void Application::EmitStateChangedEvent()
  {
    EmitEvent(OFFICIAL_CHAT_EVENT_STATE_CHANGED, ToPublicState(GetState()), "", 0,
              0, ESP_OK);
  }

  void Application::EmitMessageEvent(official_chat_event_type_t type,
                                     const std::string &message)
  {
    EmitEvent(type, ToPublicState(GetState()), message, 0, 0, ESP_OK);
  }

  void Application::EmitProgressEvent(official_chat_event_type_t type, int progress,
                                      size_t speed)
  {
    EmitEvent(type, ToPublicState(GetState()), "", progress, speed, ESP_OK);
  }

  void Application::EmitErrorEvent(esp_err_t error, const std::string &message)
  {
    last_error_ = error;
    if (!started_)
    {
      std::lock_guard<std::mutex> lock(mutex_);
      last_event_message_ = message;
      replay_events_pending_ = true;
      return;
    }
    Schedule([this, error, message]()
             {
    last_error_ = error;
    EmitEvent(OFFICIAL_CHAT_EVENT_ERROR, ToPublicState(GetState()), message, 0,
              0, error); });
  }

  void Application::EmitRebootingEvent()
  {
    EmitEvent(OFFICIAL_CHAT_EVENT_REBOOTING, ToPublicState(GetState()), "", 0, 0,
              ESP_OK);
  }

  void Application::ReplayEventSnapshot()
  {
    official_chat_event_callback_t callback = nullptr;
    ProgressSnapshot assets_progress;
    ProgressSnapshot upgrade_progress;
    std::string last_message;
    esp_err_t last_error = ESP_OK;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!replay_events_pending_)
      {
        return;
      }
      callback = event_callback_;
      assets_progress = last_assets_progress_;
      upgrade_progress = last_upgrade_progress_;
      last_message = last_event_message_;
      last_error = last_error_;
      replay_events_pending_ = false;
    }
    if (callback == nullptr)
    {
      return;
    }

    EmitStateChangedEvent();
    if (!last_message.empty())
    {
      EmitEvent(OFFICIAL_CHAT_EVENT_ACTIVATION_MESSAGE, ToPublicState(GetState()),
                last_message, 0, 0, ESP_OK);
    }
    if (assets_progress.valid)
    {
      EmitProgressEvent(OFFICIAL_CHAT_EVENT_ASSETS_PROGRESS, assets_progress.progress,
                        assets_progress.speed_bytes_per_sec);
    }
    if (upgrade_progress.valid)
    {
      EmitProgressEvent(OFFICIAL_CHAT_EVENT_UPGRADE_PROGRESS,
                        upgrade_progress.progress,
                        upgrade_progress.speed_bytes_per_sec);
    }
    if (last_error != ESP_OK)
    {
      EmitEvent(OFFICIAL_CHAT_EVENT_ERROR, ToPublicState(GetState()), last_message,
                0, 0, last_error);
    }
  }

  bool Application::SetDeviceState(DeviceState state)
  {
    const DeviceState old_state = state_machine_.GetState();
    const bool changed = state_machine_.TransitionTo(state);
    ESP_LOGI(kTag, "state transition: %s -> %s changed=%d",
             DeviceStateToString(old_state), DeviceStateToString(state),
             changed ? 1 : 0);
    if (changed && event_group_ != nullptr)
    {
      replay_events_pending_ = true;
      EmitStateChangedEvent();
      xEventGroupSetBits(event_group_, kMainEventStateChanged);
    }
    return changed;
  }

  void Application::InitializeProtocol()
  {
    ProtocolConfigSelection selection = LoadProtocolConfigSelection(
        WebsocketFallbackConfig{.url = websocket_url_,
                                .token = access_token_,
                                .version = 2,
                                .from_public_config =
                                    has_public_websocket_config_});

    if (selection.mqtt_config_present &&
        selection.source != ProtocolConfigSource::kNvsMqtt)
    {
      ESP_LOGW(kTag, "mqtt config invalid, fallback to websocket");
    }

    switch (selection.source)
    {
    case ProtocolConfigSource::kNvsMqtt:
      break;
    case ProtocolConfigSource::kNvsWebsocket:
      break;
    case ProtocolConfigSource::kPublicConfig:
      break;
    case ProtocolConfigSource::kBuiltinDefault:
    default:
      break;
    }

    switch (selection.kind)
    {
    case ProtocolKind::kMqtt:
      protocol_ = std::make_unique<MqttProtocol>(selection.mqtt);
      break;
    case ProtocolKind::kWebsocket:
    default:
      protocol_ = std::make_unique<WebsocketProtocol>(
          selection.websocket.url, selection.websocket.token,
          selection.websocket.version);
      break;
    }
    if (!protocol_)
    {
      return;
    }

    ESP_LOGI(kTag, "protocol selection: kind=%s source=%s",
             ProtocolKindToString(selection.kind),
             ProtocolConfigSourceToString(selection.source));

    const esp_app_desc_t *app_desc = esp_app_get_description();
    McpServer::GetInstance().SetServerInfo(
        app_desc != nullptr ? app_desc->project_name : "official_chat",
        app_desc != nullptr ? app_desc->version : "unknown");
    McpServer::GetInstance().SetRuntimeStatusCallback([this]()
                                                      {
    McpServer::RuntimeStatus status;
    status.chat_state = GetState();
    status.device_aec_enabled = GetDeviceAecEnabled();
    return status; });
    McpServer::GetInstance().AddCommonTools();
    McpServer::GetInstance().SetSendCallback([this](const std::string &payload)
                                             { Schedule([this, payload]()
                                                        {
      if (protocol_) {
        protocol_->SendMcpMessage(payload);
      } }); });

    protocol_->OnNetworkError([this](const std::string &message)
                              {
    ESP_LOGE(kTag, "network error: %s", message.c_str());
    Schedule([this]() { SetDeviceState(DeviceState::kIdle); }); });

    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet)
                               {
    const DeviceState state = GetState();
    if (ShouldAcceptIncomingDownlinkAudio(GetState())) {
      if (state != DeviceState::kSpeaking) {
        const auto accepted_count =
            downlink_audio_accepted_while_pending_count_.fetch_add(
                1, std::memory_order_relaxed) +
            1;
        if (downlink_audio_pending_log_budget_.fetch_sub(
                1, std::memory_order_relaxed) > 0) {
          ESP_LOGI(kTag,
                   "accepted incoming downlink audio before speaking count=%lu state=%s",
                   static_cast<unsigned long>(accepted_count),
                   DeviceStateToString(state));
        }
      }
      if (!audio_service_.PushPacketToDecodeQueue(std::move(packet))) {
        if (downlink_audio_queue_drop_log_budget_.fetch_sub(
                1, std::memory_order_relaxed) > 0) {
          ESP_LOGW(kTag,
                   "dropped incoming downlink audio because decode queue is full state=%s",
                   DeviceStateToString(state));
        }
      }
      return;
    }

    const auto dropped_count = downlink_audio_dropped_by_gate_count_.fetch_add(
                                   1, std::memory_order_relaxed) +
                               1;
    if (downlink_audio_drop_log_budget_.fetch_sub(1,
                                                  std::memory_order_relaxed) >
        0) {
      ESP_LOGW(kTag,
               "dropped incoming downlink audio while gate closed state=%s count=%lu",
               DeviceStateToString(state),
               static_cast<unsigned long>(dropped_count));
    } });

    protocol_->OnAudioChannelOpened([this]()
                                    {
    if (!codec_) {
      return;
    }
    if (protocol_->server_sample_rate() != codec_->output_sample_rate()) {
      ESP_LOGW(kTag, "server sample rate %d differs from local output %d",
               protocol_->server_sample_rate(), codec_->output_sample_rate());
    } });

    protocol_->OnAudioChannelClosed([this]()
                                    {
    if (event_group_ != nullptr) {
      xEventGroupSetBits(event_group_, kMainEventProtocolClosed);
    } });

    protocol_->OnIncomingJson([this](const cJSON *root)
                              {
    const cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
      return;
    }
    if (std::strcmp(type->valuestring, "tts") == 0) {
      const cJSON *state = cJSON_GetObjectItem(root, "state");
      if (!cJSON_IsString(state)) {
        return;
      }
      if (std::strcmp(state->valuestring, "start") == 0) {
        SetDownlinkAudioActive(true, "tts.start");
        if (GetState() != DeviceState::kSpeaking) {
          audio_service_.ResetDecoder();
        }
        Schedule([this]() { SetDeviceState(DeviceState::kSpeaking); });
      } else if (std::strcmp(state->valuestring, "stop") == 0) {
        SetDownlinkAudioActive(false, "tts.stop");
        Schedule([this]() {
          if (GetState() != DeviceState::kSpeaking) {
            return;
          }
          if (button_stop_pending_) {
            button_stop_tts_stop_seen_ = true;
            ESP_LOGI(kTag, "graceful button stop observed tts.stop");
            TryFinalizeGracefulButtonStop("tts.stop");
            return;
          }
          if (listening_mode_ == kListeningModeManualStop) {
            SetDeviceState(DeviceState::kIdle);
          } else {
            SetDeviceState(DeviceState::kListening);
          }
        });
      } else if (std::strcmp(state->valuestring, "sentence_start") == 0) {
        LogJsonTextField(root, "assistant", "text");
        const char *assistant_text = GetJsonStringField(root, "text");
        if (assistant_text != nullptr) {
          EmitMessageEvent(OFFICIAL_CHAT_EVENT_ASSISTANT_TEXT, assistant_text);
        }
      }
    } else if (std::strcmp(type->valuestring, "mcp") == 0) {
      const cJSON *payload = cJSON_GetObjectItem(root, "payload");
      if (cJSON_IsObject(payload)) {
        McpServer::GetInstance().ParseMessage(payload);
      }
    } else if (std::strcmp(type->valuestring, "stt") == 0) {
      LogJsonTextField(root, "stt", "text");
      const char *user_text = GetJsonStringField(root, "text");
      if (user_text != nullptr) {
        EmitMessageEvent(OFFICIAL_CHAT_EVENT_USER_TEXT, user_text);
      }
    } else if (std::strcmp(type->valuestring, "llm") == 0) {
      LogJsonTextField(root, "llm", "text");
    } else if (std::strcmp(type->valuestring, "system") == 0) {
      LogJsonTextField(root, "system", "command");
    } else if (std::strcmp(type->valuestring, "alert") == 0) {
      LogJsonTextField(root, "alert", "status");
      LogJsonTextField(root, "alert", "message");
    } else if (std::strcmp(type->valuestring, "goodbye") == 0) {
      if (event_group_ != nullptr) {
        xEventGroupSetBits(event_group_, kMainEventProtocolClosed);
      }
    } });

    if (!protocol_->Start())
    {
      ESP_LOGW(kTag, "protocol start returned false");
    }
  }

  esp_err_t Application::InitializeAudioService()
  {
    /*
     * 当前产品入口由页面/按键显式触发，不需要本地 wake word。跳过 ESP-SR
     * model loader 可以避免官方 managed component 在模型 mmap/解析阶段因为
     * 内存压力或模型表兼容问题触发整机 panic。AudioService 已能接受空模型表，
     * 后续若误开唤醒词检测会降级为日志告警。
     */
    models_list_.reset();
    audio_service_.Initialize(codec_.get());
    audio_service_.SetModelsList(models_list_.get());
    AudioServiceCallbacks callbacks = {};
    callbacks.on_send_queue_available = [this]()
    {
      if (event_group_ != nullptr)
      {
        xEventGroupSetBits(event_group_, kMainEventSendAudio);
      }
    };
    callbacks.on_wake_word_detected = [this](const std::string &wake_word)
    {
      ESP_LOGI(kTag, "audio service wake word callback: %s", wake_word.c_str());
      if (event_group_ != nullptr &&
          !shutting_down_.load(std::memory_order_acquire))
      {
        xEventGroupSetBits(event_group_, kMainEventWakeWordDetected);
      }
    };
    callbacks.on_vad_change = [this](bool speaking)
    {
      ESP_LOGD(kTag, "vad change: %d", speaking ? 1 : 0);
      if (event_group_ != nullptr)
      {
        xEventGroupSetBits(event_group_, kMainEventVadChange);
      }
    };
    audio_service_.SetCallbacks(callbacks);
    audio_service_.EnableDeviceAec(device_aec_enabled_);
    return audio_service_.Start();
  }

} // namespace official_chat
