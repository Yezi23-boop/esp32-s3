#include "audio/audio_service.h"

#include "sdkconfig.h"

#include <algorithm>
#include <cstring>
#include <type_traits>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/idf_additions.h>

#include "audio/input_format_utils.h"
#include "audio/local_audio_codec_adapter.h"
#include "audio/processors/afe_audio_processor.h"
#include "audio/processors/no_audio_processor.h"
#include "audio/wake_words/afe_wake_word.h"

namespace official_chat {

namespace {

constexpr char kTag[] = "official_audio";
constexpr TickType_t kAudioTaskStopTimeoutTicks = pdMS_TO_TICKS(1000);
constexpr char kAudioInputTaskName[] = "oa_input";
constexpr char kAudioOutputTaskName[] = "oa_output";
constexpr char kAudioOpusTaskName[] = "oa_opus";

#define RATE_CVT_CFG(_src_rate, _dest_rate, _channel)                      \
  (esp_ae_rate_cvt_cfg_t) {                                                \
    .src_rate = (uint32_t)(_src_rate),                                     \
    .dest_rate = (uint32_t)(_dest_rate),                                   \
    .channel = (uint8_t)(_channel),                                        \
    .bits_per_sample = ESP_AUDIO_BIT16,                                    \
    .complexity = 2,                                                       \
    .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,                          \
  }

#define OPUS_DEC_CFG(_sample_rate, _frame_duration_ms)                     \
  (esp_opus_dec_cfg_t) {                                                   \
    .sample_rate = (uint32_t)(_sample_rate),                               \
    .channel = ESP_AUDIO_MONO,                                             \
    .frame_duration =                                                      \
        (esp_opus_dec_frame_duration_t)AS_OPUS_GET_FRAME_DUR_ENUM(         \
            _frame_duration_ms),                                           \
    .self_delimited = false,                                               \
  }

void LogTaskStackHighWater(const char *task_name) {
  const auto high_water = uxTaskGetStackHighWaterMark(nullptr);
  ESP_LOGI(kTag, "%s stack high watermark: %lu",
           task_name != nullptr ? task_name : "audio_task",
           static_cast<unsigned long>(high_water));
}

void LogAudioTaskHeapState(const char *stage) {
  ESP_LOGI(
      kTag,
      "audio task heap (%s): internal_free=%u internal_largest=%u spiram_free=%u "
      "spiram_largest=%u",
      stage != nullptr ? stage : "unknown",
      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
      static_cast<unsigned>(
          heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
}

static_assert(std::is_base_of<AudioCodecIface, LocalAudioCodecAdapter>::value,
              "LocalAudioCodecAdapter must implement AudioCodecIface");

}  // namespace

AudioService::AudioService() { event_group_ = xEventGroupCreate(); }

AudioService::~AudioService() {
  Stop();
  if (audio_power_timer_ != nullptr) {
    esp_timer_stop(audio_power_timer_);
    esp_timer_delete(audio_power_timer_);
    audio_power_timer_ = nullptr;
  }
  if (event_group_ != nullptr) {
    vEventGroupDelete(event_group_);
    event_group_ = nullptr;
  }
  if (opus_encoder_ != nullptr) {
    esp_opus_enc_close(opus_encoder_);
    opus_encoder_ = nullptr;
  }
  if (opus_decoder_ != nullptr) {
    esp_opus_dec_close(opus_decoder_);
    opus_decoder_ = nullptr;
  }
  if (input_resampler_ != nullptr) {
    esp_ae_rate_cvt_close(input_resampler_);
    input_resampler_ = nullptr;
  }
  if (output_resampler_ != nullptr) {
    esp_ae_rate_cvt_close(output_resampler_);
    output_resampler_ = nullptr;
  }
}

void AudioService::Initialize(AudioCodecIface *codec) {
  codec_ = codec;
  if (codec_ == nullptr) {
    ESP_LOGE(kTag, "audio codec adapter is null");
    return;
  }
  codec_->Start();

  esp_opus_dec_cfg_t opus_dec_cfg =
      OPUS_DEC_CFG(codec_->output_sample_rate(), kOpusFrameDurationMs);
  auto ret =
      esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
  if (opus_decoder_ == nullptr) {
    ESP_LOGE(kTag, "failed to create decoder: %d", ret);
  } else {
    decoder_sample_rate_ = codec_->output_sample_rate();
    decoder_duration_ms_ = kOpusFrameDurationMs;
    decoder_frame_size_ = decoder_sample_rate_ / 1000 * decoder_duration_ms_;
  }

  esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
  ret =
      esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &opus_encoder_);
  if (opus_encoder_ == nullptr) {
    ESP_LOGE(kTag, "failed to create encoder: %d", ret);
  } else {
    encoder_sample_rate_ = 16000;
    encoder_duration_ms_ = kOpusFrameDurationMs;
    esp_opus_enc_get_frame_size(opus_encoder_, &encoder_frame_size_,
                                &encoder_outbuf_size_);
    encoder_frame_size_ = encoder_frame_size_ / sizeof(int16_t);
  }

  if (codec_->input_sample_rate() != 16000) {
    esp_ae_rate_cvt_cfg_t input_resampler_cfg =
        RATE_CVT_CFG(codec_->input_sample_rate(), ESP_AUDIO_SAMPLE_RATE_16K,
                     codec_->input_channels());
    const auto resampler_ret =
        esp_ae_rate_cvt_open(&input_resampler_cfg, &input_resampler_);
    if (input_resampler_ == nullptr) {
      ESP_LOGE(kTag, "failed to create input resampler: %d", resampler_ret);
    }
  }

  EnsureAudioProcessorCreated();

  esp_timer_create_args_t audio_power_timer_args = {
      .callback =
          [](void *arg) {
            static_cast<AudioService *>(arg)->CheckAndUpdateAudioPowerState();
          },
      .arg = this,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "official_audio_power",
      .skip_unhandled_events = true,
  };
  esp_timer_create(&audio_power_timer_args, &audio_power_timer_);
}

esp_err_t AudioService::Start() {
  if (started_ || codec_ == nullptr) {
    return codec_ != nullptr ? ESP_OK : ESP_ERR_INVALID_STATE;
  }

  started_ = true;
  service_stopped_ = false;
  xEventGroupClearBits(event_group_, kAsEventAudioTestingRunning |
                                         kAsEventWakeWordRunning |
                                         kAsEventAudioProcessorRunning |
                                         kAsEventInputTaskExited |
                                         kAsEventOutputTaskExited |
                                         kAsEventOpusTaskExited);
  if (audio_power_timer_ != nullptr) {
    esp_timer_start_periodic(audio_power_timer_, 1000000);
  }
  LogAudioTaskHeapState("before-create");

  const BaseType_t input_created = xTaskCreate(
      [](void *arg) {
        auto *self = static_cast<AudioService *>(arg);
        LogTaskStackHighWater(kAudioInputTaskName);
        self->AudioInputTask();
        self->audio_input_task_handle_ = nullptr;
        if (self->event_group_ != nullptr) {
          xEventGroupSetBits(self->event_group_, kAsEventInputTaskExited);
        }
        vTaskDelete(nullptr);
      },
      kAudioInputTaskName, kAudioInputTaskStackBytes, this, 8,
      &audio_input_task_handle_);

  const BaseType_t output_created = xTaskCreate(
      [](void *arg) {
        auto *self = static_cast<AudioService *>(arg);
        LogTaskStackHighWater(kAudioOutputTaskName);
        self->AudioOutputTask();
        self->audio_output_task_handle_ = nullptr;
        if (self->event_group_ != nullptr) {
          xEventGroupSetBits(self->event_group_, kAsEventOutputTaskExited);
        }
        vTaskDelete(nullptr);
      },
      kAudioOutputTaskName, kAudioOutputTaskStackBytes, this, 4,
      &audio_output_task_handle_);

  const BaseType_t opus_created = xTaskCreateWithCaps(
      [](void *arg) {
        auto *self = static_cast<AudioService *>(arg);
        LogTaskStackHighWater(kAudioOpusTaskName);
        self->OpusCodecTask();
        self->opus_codec_task_handle_ = nullptr;
        if (self->event_group_ != nullptr) {
          xEventGroupSetBits(self->event_group_, kAsEventOpusTaskExited);
        }
        vTaskDeleteWithCaps(nullptr);
      },
      kAudioOpusTaskName, kAudioOpusTaskStackBytes, this, 2,
      &opus_codec_task_handle_, MALLOC_CAP_SPIRAM);

  if (input_created != pdPASS || output_created != pdPASS ||
      opus_created != pdPASS) {
    LogAudioTaskHeapState("create-failed");
    ESP_LOGE(kTag,
             "failed to create audio service tasks: in=%ld out=%ld opus=%ld",
             static_cast<long>(input_created), static_cast<long>(output_created),
             static_cast<long>(opus_created));
    Stop();
    return ESP_FAIL;
  }

  return ESP_OK;
}

void AudioService::Stop() {
  if (!started_) {
    return;
  }

  started_ = false;
  service_stopped_ = true;
  StopSpeechFeatures();
  if (audio_power_timer_ != nullptr) {
    esp_timer_stop(audio_power_timer_);
  }
  if (event_group_ != nullptr) {
    xEventGroupSetBits(event_group_, kAsEventAudioTestingRunning |
                                         kAsEventWakeWordRunning |
                                         kAsEventAudioProcessorRunning);
  }
  {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    audio_encode_queue_.clear();
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    audio_testing_queue_.clear();
  }
  audio_queue_cv_.notify_all();

  EventBits_t expected_exit_bits = 0;
  if (audio_input_task_handle_ != nullptr) {
    expected_exit_bits |= kAsEventInputTaskExited;
  }
  if (audio_output_task_handle_ != nullptr) {
    expected_exit_bits |= kAsEventOutputTaskExited;
  }
  if (opus_codec_task_handle_ != nullptr) {
    expected_exit_bits |= kAsEventOpusTaskExited;
  }
  if (expected_exit_bits != 0 && event_group_ != nullptr) {
    const EventBits_t bits =
        xEventGroupWaitBits(event_group_, expected_exit_bits, pdTRUE, pdTRUE,
                            kAudioTaskStopTimeoutTicks);
    if ((bits & expected_exit_bits) != expected_exit_bits) {
      ESP_LOGW(
          kTag,
          "audio tasks did not exit before shutdown: expected=0x%x actual=0x%x",
          expected_exit_bits, bits);
    }
  }
  audio_input_task_handle_ = nullptr;
  audio_output_task_handle_ = nullptr;
  opus_codec_task_handle_ = nullptr;
}

void AudioService::EncodeWakeWord() {
  if (wake_word_ != nullptr) {
    wake_word_->EncodeWakeWordData();
  }
}

std::unique_ptr<AudioStreamPacket> AudioService::PopWakeWordPacket() {
  if (wake_word_ == nullptr) {
    return nullptr;
  }
  auto packet = std::make_unique<AudioStreamPacket>();
  if (wake_word_->GetWakeWordOpus(packet->payload)) {
    return packet;
  }
  return nullptr;
}

const std::string &AudioService::GetLastWakeWord() const {
  static const std::string kEmpty;
  return wake_word_ != nullptr ? wake_word_->GetLastDetectedWakeWord() : kEmpty;
}

bool AudioService::IsIdle() {
  std::lock_guard<std::mutex> lock(audio_queue_mutex_);
  return audio_encode_queue_.empty() && audio_decode_queue_.empty() &&
         audio_playback_queue_.empty() && audio_testing_queue_.empty();
}

bool AudioService::HasPendingDownlinkPlayback() {
  std::lock_guard<std::mutex> lock(audio_queue_mutex_);
  return !audio_decode_queue_.empty() || !audio_playback_queue_.empty();
}

void AudioService::WaitForPlaybackQueueEmpty() {
  std::unique_lock<std::mutex> lock(audio_queue_mutex_);
  audio_queue_cv_.wait(lock, [this]() {
    return service_stopped_ ||
           (audio_decode_queue_.empty() && audio_playback_queue_.empty());
  });
}

bool AudioService::WaitForDownlinkPlaybackDrain(int quiet_ms, int timeout_ms) {
  const auto quiet_duration = std::chrono::milliseconds(std::max(quiet_ms, 0));
  const auto timeout_duration =
      std::chrono::milliseconds(std::max(timeout_ms, 0));
  std::unique_lock<std::mutex> lock(audio_queue_mutex_);
  const auto deadline = std::chrono::steady_clock::now() + timeout_duration;
  while (!service_stopped_) {
    const auto now = std::chrono::steady_clock::now();
    const bool queues_empty =
        audio_decode_queue_.empty() && audio_playback_queue_.empty();
    const bool decode_quiet =
        now - last_decode_enqueue_time_ >= quiet_duration;
    const bool playback_quiet =
        now - last_playback_enqueue_time_ >= quiet_duration;
    if (queues_empty && decode_quiet && playback_quiet) {
      return true;
    }
    if (now >= deadline) {
      return false;
    }
    const auto wake_deadline = std::min(deadline, now + quiet_duration);
    audio_queue_cv_.wait_until(lock, wake_deadline);
  }
  return true;
}

bool AudioService::IsWakeWordRunning() const {
  return event_group_ != nullptr &&
         (xEventGroupGetBits(event_group_) & kAsEventWakeWordRunning) != 0;
}

bool AudioService::IsAudioProcessorRunning() const {
  return event_group_ != nullptr &&
         (xEventGroupGetBits(event_group_) & kAsEventAudioProcessorRunning) != 0;
}

bool AudioService::IsAfeWakeWord() {
  return wake_word_is_afe_ && wake_word_ != nullptr;
}

void AudioService::EnableWakeWordDetection(bool enable) {
  if (!enable) {
    if (wake_word_ != nullptr) {
      wake_word_->Stop();
    }
    if (event_group_ != nullptr) {
      xEventGroupClearBits(event_group_, kAsEventWakeWordRunning);
    }
    return;
  }

  if (wake_word_ == nullptr) {
    if (models_list_ == nullptr) {
      ESP_LOGW(kTag, "wake word detection requested without SR models");
      return;
    }
    wake_word_ = std::make_unique<AfeWakeWord>();
    wake_word_is_afe_ = true;
    BindWakeWordCallbacks();
  }

  if (!wake_word_initialized_) {
    wake_word_initialized_ = wake_word_->Initialize(codec_, models_list_);
    if (!wake_word_initialized_) {
      ESP_LOGW(kTag, "failed to initialize wake word detection");
      wake_word_.reset();
      wake_word_is_afe_ = false;
      return;
    }
  }

  {
    std::lock_guard<std::mutex> lock(input_resampler_mutex_);
    if (input_resampler_ != nullptr) {
      esp_ae_rate_cvt_reset(input_resampler_);
    }
  }
  wake_word_->Start();
  if (event_group_ != nullptr) {
    xEventGroupSetBits(event_group_, kAsEventWakeWordRunning);
  }
}

void AudioService::EnableVoiceProcessing(bool enable) {
  EnsureAudioProcessorCreated();
  if (audio_processor_ == nullptr) {
    return;
  }

  ESP_LOGI(kTag, "enable voice processing: %d", enable ? 1 : 0);

  if (enable) {
    if (!audio_processor_initialized_) {
      audio_processor_->Initialize(codec_, kOpusFrameDurationMs, models_list_);
      audio_processor_initialized_ = true;
    }
    ResetDecoder();
    audio_input_need_warmup_ = true;
    {
      std::lock_guard<std::mutex> lock(input_resampler_mutex_);
      if (input_resampler_ != nullptr) {
        esp_ae_rate_cvt_reset(input_resampler_);
      }
    }
    audio_processor_->Start();
    xEventGroupSetBits(event_group_, kAsEventAudioProcessorRunning);
  } else {
    audio_processor_->Stop();
    xEventGroupClearBits(event_group_, kAsEventAudioProcessorRunning);
  }
}

void AudioService::EnableAudioTesting(bool enable) {
  if (enable) {
    xEventGroupSetBits(event_group_, kAsEventAudioTestingRunning);
  } else {
    xEventGroupClearBits(event_group_, kAsEventAudioTestingRunning);
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    audio_decode_queue_ = std::move(audio_testing_queue_);
    audio_queue_cv_.notify_all();
  }
}

void AudioService::EnableDeviceAec(bool enable) {
  EnsureAudioProcessorCreated();
  if (!audio_processor_initialized_ && audio_processor_ != nullptr) {
    audio_processor_->Initialize(codec_, kOpusFrameDurationMs, models_list_);
    audio_processor_initialized_ = true;
  }
  if (audio_processor_ != nullptr) {
    audio_processor_->EnableDeviceAec(enable);
  }
}

void AudioService::SetCallbacks(AudioServiceCallbacks &callbacks) {
  callbacks_ = callbacks;
  BindWakeWordCallbacks();
}

bool AudioService::PushPacketToDecodeQueue(
    std::unique_ptr<AudioStreamPacket> packet, bool wait) {
  std::unique_lock<std::mutex> lock(audio_queue_mutex_);
  if (audio_decode_queue_.size() >= kMaxDecodePacketsInQueue) {
    if (wait) {
      audio_queue_cv_.wait(lock, [this]() {
        return audio_decode_queue_.size() < kMaxDecodePacketsInQueue;
      });
    } else {
      debug_statistics_.decode_queue_drop_count++;
      if (decode_drop_log_budget_ > 0) {
        ESP_LOGW(kTag,
                 "audio decode queue full, dropping packet size=%u decode_queue=%u",
                 packet != nullptr ? static_cast<unsigned>(packet->payload.size())
                                  : 0U,
                 static_cast<unsigned>(audio_decode_queue_.size()));
        decode_drop_log_budget_--;
      }
      return false;
    }
  }
  audio_decode_queue_.push_back(std::move(packet));
  debug_statistics_.decode_queue_push_count++;
  last_decode_enqueue_time_ = std::chrono::steady_clock::now();
  audio_queue_cv_.notify_all();
  return true;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue() {
  std::lock_guard<std::mutex> lock(audio_queue_mutex_);
  if (audio_send_queue_.empty()) {
    return nullptr;
  }
  auto packet = std::move(audio_send_queue_.front());
  audio_send_queue_.pop_front();
  audio_queue_cv_.notify_all();
  return packet;
}

void AudioService::PlaySound(const std::string_view &ogg) {
  if (codec_ == nullptr) {
    return;
  }
  if (!codec_->output_enabled()) {
    if (audio_power_timer_ != nullptr) {
      esp_timer_stop(audio_power_timer_);
      esp_timer_start_periodic(audio_power_timer_,
                               kAudioPowerCheckIntervalMs * 1000);
    }
    codec_->EnableOutput(true);
  }

  auto demuxer = std::make_unique<OggDemuxer>();
  demuxer->OnDemuxerFinished([this](const uint8_t *data, int sample_rate,
                                    size_t payload_size) {
    auto packet = std::make_unique<AudioStreamPacket>();
    packet->sample_rate = sample_rate;
    packet->frame_duration = kOpusFrameDurationMs;
    packet->payload.assign(data, data + payload_size);
    PushPacketToDecodeQueue(std::move(packet), true);
  });
  demuxer->Reset();
  demuxer->Process(reinterpret_cast<const uint8_t *>(ogg.data()), ogg.size());
}

bool AudioService::ReadAudioData(std::vector<int16_t> &data, int sample_rate,
                                 int samples) {
  if (codec_ == nullptr) {
    return false;
  }
  if (!codec_->input_enabled()) {
    if (audio_power_timer_ != nullptr) {
      esp_timer_stop(audio_power_timer_);
      esp_timer_start_periodic(audio_power_timer_,
                               kAudioPowerCheckIntervalMs * 1000);
    }
    codec_->EnableInput(true);
  }

  if (codec_->input_sample_rate() != sample_rate) {
    data.resize(static_cast<size_t>(samples) * codec_->input_sample_rate() /
                sample_rate * codec_->input_channels());
    if (!codec_->InputData(data)) {
      if (input_read_log_budget_ > 0) {
        ESP_LOGW(kTag, "audio input read failed rate=%d channels=%d samples=%d",
                 sample_rate, codec_->input_channels(), samples);
        input_read_log_budget_--;
      }
      return false;
    }
    if (input_resampler_ != nullptr) {
      std::lock_guard<std::mutex> lock(input_resampler_mutex_);
      uint32_t in_sample_num =
          static_cast<uint32_t>(data.size() / codec_->input_channels());
      uint32_t output_samples = 0;
      esp_ae_rate_cvt_get_max_out_sample_num(input_resampler_, in_sample_num,
                                             &output_samples);
      auto resampled =
          std::vector<int16_t>(output_samples * codec_->input_channels());
      uint32_t actual_output = output_samples;
      esp_ae_rate_cvt_process(
          input_resampler_, reinterpret_cast<esp_ae_sample_t *>(data.data()),
          in_sample_num, reinterpret_cast<esp_ae_sample_t *>(resampled.data()),
          &actual_output);
      resampled.resize(actual_output * codec_->input_channels());
      data = std::move(resampled);
    }
  } else {
    data.resize(static_cast<size_t>(samples) * codec_->input_channels());
    if (!codec_->InputData(data)) {
      if (input_read_log_budget_ > 0) {
        ESP_LOGW(kTag, "audio input read failed rate=%d channels=%d samples=%d",
                 sample_rate, codec_->input_channels(), samples);
        input_read_log_budget_--;
      }
      return false;
    }
  }

  last_input_time_ = std::chrono::steady_clock::now();
  debug_statistics_.input_count++;
  if (input_read_log_budget_ > 0) {
    ESP_LOGI(kTag, "audio input read ok rate=%d channels=%d samples=%d words=%u",
             sample_rate, codec_->input_channels(), samples,
             static_cast<unsigned>(data.size()));
    input_read_log_budget_--;
  }
  MaybeLogQueueStats();
  return true;
}

void AudioService::ResetDecoder() {
  std::lock_guard<std::mutex> lock(audio_queue_mutex_);
  std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
  if (opus_decoder_ != nullptr) {
    esp_opus_dec_reset(opus_decoder_);
  }
  decoder_lock.unlock();
  timestamp_queue_.clear();
  audio_decode_queue_.clear();
  audio_playback_queue_.clear();
  audio_testing_queue_.clear();
  audio_queue_cv_.notify_all();
}

void AudioService::SetModelsList(srmodel_list_t *models_list) {
  models_list_ = models_list;
  wake_word_initialized_ = false;
  wake_word_.reset();
  wake_word_is_afe_ = false;
  BindWakeWordCallbacks();
}

void AudioService::AudioInputTask() {
  while (true) {
    const EventBits_t bits = xEventGroupWaitBits(
        event_group_, kAsEventAudioTestingRunning | kAsEventWakeWordRunning |
                          kAsEventAudioProcessorRunning,
        pdFALSE, pdFALSE, portMAX_DELAY);

    if (service_stopped_) {
      break;
    }
    if (audio_input_need_warmup_) {
      audio_input_need_warmup_ = false;
      vTaskDelay(pdMS_TO_TICKS(120));
      continue;
    }

    if ((bits & kAsEventAudioTestingRunning) != 0) {
      if (audio_testing_queue_.size() >=
          kAudioTestingMaxDurationMs / kOpusFrameDurationMs) {
        if (callbacks_.on_audio_testing_queue_full) {
          callbacks_.on_audio_testing_queue_full();
        }
        EnableAudioTesting(false);
        continue;
      }
      std::vector<int16_t> data;
      const int samples = kOpusFrameDurationMs * 16000 / 1000;
      if (ReadAudioData(data, 16000, samples)) {
        data = ExtractPrimaryMicChannel(data, codec_);
        PushTaskToEncodeQueue(kAudioTaskTypeEncodeToTestingQueue,
                              std::move(data));
        continue;
      }
    }

    if ((bits & (kAsEventWakeWordRunning | kAsEventAudioProcessorRunning)) !=
        0) {
      std::vector<int16_t> data;
      int samples = 160;
      if ((bits & kAsEventWakeWordRunning) != 0 && wake_word_ != nullptr) {
        samples = std::max(samples, static_cast<int>(wake_word_->GetFeedSize()));
      }
      if ((bits & kAsEventAudioProcessorRunning) != 0 &&
          audio_processor_ != nullptr) {
        samples = std::max(samples,
                           static_cast<int>(audio_processor_->GetFeedSize()));
      }
      if (ReadAudioData(data, 16000, samples)) {
        if ((bits & kAsEventWakeWordRunning) != 0 && wake_word_ != nullptr) {
          wake_word_->Feed(data);
        }
        if ((bits & kAsEventAudioProcessorRunning) != 0 &&
            audio_processor_ != nullptr) {
          audio_processor_->Feed(std::move(data));
        }
        continue;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void AudioService::AudioOutputTask() {
  while (true) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    audio_queue_cv_.wait(lock, [this]() {
      return !audio_playback_queue_.empty() || service_stopped_;
    });
    if (service_stopped_) {
      break;
    }

    auto task = std::move(audio_playback_queue_.front());
    audio_playback_queue_.pop_front();
    audio_queue_cv_.notify_all();
    lock.unlock();

    if (!codec_->output_enabled()) {
      if (audio_power_timer_ != nullptr) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_,
                                 kAudioPowerCheckIntervalMs * 1000);
      }
      codec_->EnableOutput(true);
    }

    codec_->OutputData(task->pcm);
    last_output_time_ = std::chrono::steady_clock::now();
    debug_statistics_.playback_count++;
    debug_statistics_.output_write_count++;
  }
}

void AudioService::OpusCodecTask() {
  while (true) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    audio_queue_cv_.wait(lock, [this]() {
      return service_stopped_ ||
             (!audio_encode_queue_.empty() &&
              audio_send_queue_.size() < kMaxSendPacketsInQueue) ||
             (!audio_decode_queue_.empty() &&
              audio_playback_queue_.size() < kMaxPlaybackTasksInQueue);
    });
    if (service_stopped_) {
      break;
    }

    if (!audio_decode_queue_.empty() &&
        audio_playback_queue_.size() < kMaxPlaybackTasksInQueue) {
      auto packet = std::move(audio_decode_queue_.front());
      audio_decode_queue_.pop_front();
      audio_queue_cv_.notify_all();
      lock.unlock();

      auto task = std::make_unique<AudioTask>();
      task->type = kAudioTaskTypeDecodeToPlaybackQueue;
      task->timestamp = packet->timestamp;
      SetDecodeSampleRate(packet->sample_rate, packet->frame_duration);

      if (opus_decoder_ != nullptr) {
        task->pcm.resize(decoder_frame_size_);
        esp_audio_dec_in_raw_t raw = {
            .buffer = packet->payload.data(),
            .len = static_cast<uint32_t>(packet->payload.size()),
            .consumed = 0,
            .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
        };
        esp_audio_dec_out_frame_t out_frame = {
            .buffer = reinterpret_cast<uint8_t *>(task->pcm.data()),
            .len = static_cast<uint32_t>(task->pcm.size() * sizeof(int16_t)),
            .needed_size = 0,
            .decoded_size = 0,
        };
        esp_audio_dec_info_t dec_info = {};
        std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
        const auto ret =
            esp_opus_dec_decode(opus_decoder_, &raw, &out_frame, &dec_info);
        decoder_lock.unlock();
        if (ret == ESP_AUDIO_ERR_OK) {
          task->pcm.resize(out_frame.decoded_size / sizeof(int16_t));
          if (decoder_sample_rate_ != codec_->output_sample_rate() &&
              output_resampler_ != nullptr) {
            uint32_t target_size = 0;
            esp_ae_rate_cvt_get_max_out_sample_num(output_resampler_,
                                                   task->pcm.size(),
                                                   &target_size);
            std::vector<int16_t> resampled(target_size);
            uint32_t actual_output = target_size;
            esp_ae_rate_cvt_process(
                output_resampler_,
                reinterpret_cast<esp_ae_sample_t *>(task->pcm.data()),
                task->pcm.size(),
                reinterpret_cast<esp_ae_sample_t *>(resampled.data()),
                &actual_output);
            resampled.resize(actual_output);
            task->pcm = std::move(resampled);
          }
          lock.lock();
          audio_playback_queue_.push_back(std::move(task));
          audio_queue_cv_.notify_all();
          debug_statistics_.decode_count++;
          debug_statistics_.playback_queue_push_count++;
          last_playback_enqueue_time_ = std::chrono::steady_clock::now();
        } else {
          ESP_LOGE(kTag, "failed to decode audio: %d", ret);
          lock.lock();
        }
      } else {
        ESP_LOGE(kTag, "audio decoder is not configured");
        lock.lock();
      }
    }

    if (!audio_encode_queue_.empty() &&
        audio_send_queue_.size() < kMaxSendPacketsInQueue) {
      auto task = std::move(audio_encode_queue_.front());
      audio_encode_queue_.pop_front();
      audio_queue_cv_.notify_all();
      lock.unlock();

      auto packet = std::make_unique<AudioStreamPacket>();
      packet->frame_duration = kOpusFrameDurationMs;
      packet->sample_rate = 16000;
      packet->timestamp = task->timestamp;

      if (opus_encoder_ != nullptr &&
          static_cast<int>(task->pcm.size()) == encoder_frame_size_) {
        std::vector<uint8_t> buf(encoder_outbuf_size_);
        esp_audio_enc_in_frame_t in = {
            .buffer = reinterpret_cast<uint8_t *>(task->pcm.data()),
            .len = static_cast<uint32_t>(encoder_frame_size_ * sizeof(int16_t)),
        };
        esp_audio_enc_out_frame_t out = {
            .buffer = buf.data(),
            .len = static_cast<uint32_t>(encoder_outbuf_size_),
            .encoded_bytes = 0,
            .pts = 0,
        };
        const auto ret = esp_opus_enc_process(opus_encoder_, &in, &out);
        if (ret == ESP_AUDIO_ERR_OK) {
          packet->payload.assign(buf.data(), buf.data() + out.encoded_bytes);
          if (task->type == kAudioTaskTypeEncodeToSendQueue) {
            {
              std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
              audio_send_queue_.push_back(std::move(packet));
              if (send_queue_log_budget_ > 0) {
                ESP_LOGI(kTag, "audio send queue push size=%u",
                         static_cast<unsigned>(audio_send_queue_.size()));
                send_queue_log_budget_--;
              }
            }
            if (callbacks_.on_send_queue_available) {
              callbacks_.on_send_queue_available();
            }
          } else {
            std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
            audio_testing_queue_.push_back(std::move(packet));
          }
          debug_statistics_.encode_count++;
          if (encode_log_budget_ > 0) {
            ESP_LOGI(kTag,
                     "opus encode ok bytes=%u frame_samples=%d type=%d count=%lu",
                     static_cast<unsigned>(out.encoded_bytes),
                     encoder_frame_size_, static_cast<int>(task->type),
                     static_cast<unsigned long>(debug_statistics_.encode_count));
            encode_log_budget_--;
          }
          MaybeLogQueueStats();
        } else {
          ESP_LOGE(kTag, "failed to encode audio: %d", ret);
        }
      } else {
        ESP_LOGE(
            kTag,
            "failed to encode audio: encoder not configured or invalid frame size (got %u expected %d)",
            static_cast<unsigned>(task->pcm.size()), encoder_frame_size_);
      }
      lock.lock();
    }
  }
}

void AudioService::PushTaskToEncodeQueue(AudioTaskType type,
                                          std::vector<int16_t> &&pcm) {
  auto task = std::make_unique<AudioTask>();
  task->type = type;
  task->pcm = std::move(pcm);

  std::unique_lock<std::mutex> lock(audio_queue_mutex_);
  if (type == kAudioTaskTypeEncodeToSendQueue && !timestamp_queue_.empty()) {
    if (timestamp_queue_.size() <= kMaxTimestampsInQueue) {
      task->timestamp = timestamp_queue_.front();
    }
    timestamp_queue_.pop_front();
  }

  audio_queue_cv_.wait(lock, [this]() {
    return audio_encode_queue_.size() < kMaxEncodeTasksInQueue ||
           service_stopped_;
  });
  if (service_stopped_) {
    return;
  }
  audio_encode_queue_.push_back(std::move(task));
  audio_queue_cv_.notify_all();
}

void AudioService::SetDecodeSampleRate(int sample_rate, int frame_duration) {
  if (codec_ == nullptr) {
    return;
  }
  if (decoder_sample_rate_ == sample_rate &&
      decoder_duration_ms_ == frame_duration) {
    return;
  }

  std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
  if (opus_decoder_ != nullptr) {
    esp_opus_dec_close(opus_decoder_);
    opus_decoder_ = nullptr;
  }
  decoder_lock.unlock();

  esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(sample_rate, frame_duration);
  const auto ret =
      esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
  if (opus_decoder_ == nullptr) {
    ESP_LOGE(kTag, "failed to recreate decoder: %d", ret);
    return;
  }

  decoder_sample_rate_ = sample_rate;
  decoder_duration_ms_ = frame_duration;
  decoder_frame_size_ = decoder_sample_rate_ / 1000 * frame_duration;

  if (output_resampler_ != nullptr) {
    esp_ae_rate_cvt_close(output_resampler_);
    output_resampler_ = nullptr;
  }
  if (decoder_sample_rate_ != codec_->output_sample_rate()) {
    esp_ae_rate_cvt_cfg_t output_resampler_cfg =
        RATE_CVT_CFG(decoder_sample_rate_, codec_->output_sample_rate(),
                     ESP_AUDIO_MONO);
    const auto resampler_ret =
        esp_ae_rate_cvt_open(&output_resampler_cfg, &output_resampler_);
    if (output_resampler_ == nullptr) {
      ESP_LOGE(kTag, "failed to create output resampler: %d", resampler_ret);
    }
  }
}

void AudioService::CheckAndUpdateAudioPowerState() {
  if (codec_ == nullptr) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  const auto input_elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                            last_input_time_)
          .count();
  const auto output_elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                            last_output_time_)
          .count();
  if (input_elapsed > kAudioPowerTimeoutMs && codec_->input_enabled()) {
    codec_->EnableInput(false);
  }
  if (output_elapsed > kAudioPowerTimeoutMs && codec_->output_enabled()) {
    if (!(codec_->duplex() && codec_->input_enabled())) {
      codec_->EnableOutput(false);
    }
  }
  if (!codec_->input_enabled() && !codec_->output_enabled() &&
      audio_power_timer_ != nullptr) {
    esp_timer_stop(audio_power_timer_);
  }
}

void AudioService::MaybeLogQueueStats() {
  const auto now = std::chrono::steady_clock::now();
  const auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now - last_statistics_log_time_)
          .count();
  if (elapsed_ms < 1000) {
    return;
  }

  size_t send_queue_size = 0;
  size_t decode_queue_size = 0;
  size_t playback_queue_size = 0;
  const auto last_decode_enqueue_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now - last_decode_enqueue_time_)
          .count();
  const auto last_playback_enqueue_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now - last_playback_enqueue_time_)
          .count();
  const auto last_output_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now - last_output_time_)
          .count();
  {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    send_queue_size = audio_send_queue_.size();
    decode_queue_size = audio_decode_queue_.size();
    playback_queue_size = audio_playback_queue_.size();
  }

  if (!IsAudioProcessorRunning() && !IsWakeWordRunning() &&
      send_queue_size == 0 && decode_queue_size == 0 &&
      playback_queue_size == 0) {
    return;
  }

  last_statistics_log_time_ = now;
  ESP_LOGI(kTag,
           "audio queue stats: input=%lu encode=%lu decode=%lu playback=%lu "
           "decode_push=%lu decode_drop=%lu playback_push=%lu output_write=%lu "
           "send_queue=%u decode_queue=%u playback_queue=%u "
           "last_decode_ms=%lld last_playback_ms=%lld last_output_ms=%lld",
           static_cast<unsigned long>(debug_statistics_.input_count),
           static_cast<unsigned long>(debug_statistics_.encode_count),
           static_cast<unsigned long>(debug_statistics_.decode_count),
           static_cast<unsigned long>(debug_statistics_.playback_count),
           static_cast<unsigned long>(debug_statistics_.decode_queue_push_count),
           static_cast<unsigned long>(debug_statistics_.decode_queue_drop_count),
           static_cast<unsigned long>(debug_statistics_.playback_queue_push_count),
           static_cast<unsigned long>(debug_statistics_.output_write_count),
           static_cast<unsigned>(send_queue_size),
           static_cast<unsigned>(decode_queue_size),
           static_cast<unsigned>(playback_queue_size),
           static_cast<long long>(last_decode_enqueue_ms),
           static_cast<long long>(last_playback_enqueue_ms),
           static_cast<long long>(last_output_ms));
}

void AudioService::EnsureAudioProcessorCreated() {
  if (audio_processor_ != nullptr) {
    return;
  }
#if CONFIG_USE_AUDIO_PROCESSOR
  audio_processor_ = std::make_unique<AfeAudioProcessor>();
#else
  audio_processor_ = std::make_unique<NoAudioProcessor>();
#endif
  BindAudioProcessorCallbacks();
}

void AudioService::BindAudioProcessorCallbacks() {
  if (audio_processor_ == nullptr) {
    return;
  }
  audio_processor_->OnOutput([this](std::vector<int16_t> &&data) {
    PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, std::move(data));
  });
  audio_processor_->OnVadStateChange([this](bool speaking) {
    voice_detected_ = speaking;
    if (callbacks_.on_vad_change) {
      callbacks_.on_vad_change(speaking);
    }
  });
}

void AudioService::BindWakeWordCallbacks() {
  if (wake_word_ == nullptr) {
    return;
  }
  wake_word_->OnWakeWordDetected([this](const std::string &wake_word) {
    (void)wake_word;
    if (callbacks_.on_wake_word_detected) {
      callbacks_.on_wake_word_detected(GetLastWakeWord());
    }
  });
}

void AudioService::StopSpeechFeatures() {
  if (wake_word_ != nullptr) {
    wake_word_->Stop();
  }
  if (audio_processor_ != nullptr) {
    audio_processor_->Stop();
  }
  if (event_group_ != nullptr) {
    xEventGroupClearBits(event_group_, kAsEventWakeWordRunning |
                                           kAsEventAudioProcessorRunning);
  }
}

}  // namespace official_chat
