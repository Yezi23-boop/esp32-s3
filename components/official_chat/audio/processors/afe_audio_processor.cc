#include "audio/processors/afe_audio_processor.h"

#include "sdkconfig.h"

#include <cstring>
#include <string>

#include <esp_log.h>

#include "audio/input_format_utils.h"

namespace official_chat {

namespace {

constexpr char kTag[] = "official_afe_ap";
constexpr TickType_t kWorkerStopTimeoutTicks = pdMS_TO_TICKS(1000);

}  // namespace

AfeAudioProcessor::AfeAudioProcessor() {
  event_group_ = xEventGroupCreate();
#if CONFIG_USE_DEVICE_AEC
  device_aec_enabled_ = true;
#endif
}

AfeAudioProcessor::~AfeAudioProcessor() {
  worker_stop_requested_ = true;
  running_ = false;
  if (afe_data_ != nullptr && afe_iface_ != nullptr) {
    afe_iface_->reset_buffer(afe_data_);
  }
  if (worker_task_handle_ != nullptr) {
    const TickType_t deadline = xTaskGetTickCount() + kWorkerStopTimeoutTicks;
    while (worker_task_handle_ != nullptr && xTaskGetTickCount() < deadline) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
  if (afe_data_ != nullptr && afe_iface_ != nullptr) {
    afe_iface_->destroy(afe_data_);
    afe_data_ = nullptr;
  }
  if (owns_models_ && owned_models_ != nullptr) {
    esp_srmodel_deinit(owned_models_);
    owned_models_ = nullptr;
  }
  if (event_group_ != nullptr) {
    vEventGroupDelete(event_group_);
    event_group_ = nullptr;
  }
}

void AfeAudioProcessor::Initialize(AudioCodecIface *codec, int frame_duration_ms,
                                   srmodel_list_t *models_list) {
  codec_ = codec;
  frame_samples_ = frame_duration_ms * 16000 / 1000;
  output_buffer_.clear();
  output_buffer_.reserve(static_cast<size_t>(frame_samples_));
  input_buffer_.clear();
  is_speaking_ = false;

  if (codec_ == nullptr) {
    ESP_LOGE(kTag, "audio codec adapter is null");
    return;
  }

  if (initialized_) {
    return;
  }

  srmodel_list_t *models = models_list;
  if (models == nullptr) {
    ESP_LOGI(kTag, "SR models disabled, fallback to passthrough");
    return;
  }
  if (models->num == -1) {
    ESP_LOGE(kTag, "invalid SR model list, fallback to passthrough");
    return;
  }

  const char *input_format = codec_->input_format();
  if (input_format == nullptr || input_format[0] == '\0') {
    ESP_LOGE(kTag, "input format is empty, fallback to passthrough");
    return;
  }

  char *ns_model_name = esp_srmodel_filter(models, ESP_NSNET_PREFIX, nullptr);
  char *vad_model_name = esp_srmodel_filter(models, ESP_VADN_PREFIX, nullptr);

  afe_config_t *afe_config =
      afe_config_init(input_format, nullptr, AFE_TYPE_VC,
                      AFE_MODE_HIGH_PERF);
  if (afe_config == nullptr) {
    ESP_LOGE(kTag, "failed to create AFE config");
    return;
  }

  afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
  afe_config->vad_mode = VAD_MODE_0;
  afe_config->vad_min_noise_ms = 100;
  afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
  afe_config->ns_init = ns_model_name != nullptr;
  afe_config->vad_init = !device_aec_enabled_;
  afe_config->aec_init = device_aec_enabled_;
  if (vad_model_name != nullptr) {
    afe_config->vad_model_name = vad_model_name;
  }
  if (ns_model_name != nullptr) {
    afe_config->ns_model_name = ns_model_name;
    afe_config->afe_ns_mode = AFE_NS_MODE_NET;
  }

  afe_iface_ = esp_afe_handle_from_config(afe_config);
  if (afe_iface_ == nullptr) {
    ESP_LOGE(kTag, "failed to get AFE interface, fallback to passthrough");
    return;
  }

  afe_data_ = afe_iface_->create_from_config(afe_config);
  if (afe_data_ == nullptr) {
    ESP_LOGE(kTag, "failed to create AFE handle, fallback to passthrough");
    afe_iface_ = nullptr;
    return;
  }

  worker_stop_requested_ = false;
  if (worker_task_handle_ == nullptr) {
    const BaseType_t created = xTaskCreate(
        [](void *arg) {
          auto *self = static_cast<AfeAudioProcessor *>(arg);
          self->AudioProcessorTask();
          self->worker_task_handle_ = nullptr;
          vTaskDelete(nullptr);
        },
        "afe_proc", 4096, this, 3, &worker_task_handle_);
    if (created != pdPASS) {
      ESP_LOGE(kTag, "failed to create AFE processor task");
      afe_iface_->destroy(afe_data_);
      afe_data_ = nullptr;
      afe_iface_ = nullptr;
      return;
    }
  }

  initialized_ = true;
}

void AfeAudioProcessor::Feed(std::vector<int16_t> &&data) {
  if (!running_) {
    return;
  }
  if (afe_data_ == nullptr || afe_iface_ == nullptr) {
    EmitFallbackFrames(std::move(data));
    return;
  }

  std::lock_guard<std::mutex> lock(input_buffer_mutex_);
  if (!running_) {
    return;
  }

  input_buffer_.insert(input_buffer_.end(), data.begin(), data.end());
  const size_t chunk_size =
      afe_iface_->get_feed_chunksize(afe_data_) * codec_->input_channels();
  while (input_buffer_.size() >= chunk_size) {
    afe_iface_->feed(afe_data_, input_buffer_.data());
    input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + chunk_size);
  }
}

void AfeAudioProcessor::Start() {
  running_ = true;
}

void AfeAudioProcessor::Stop() {
  running_ = false;
  if (afe_data_ != nullptr && afe_iface_ != nullptr) {
    afe_iface_->reset_buffer(afe_data_);
  }
  std::lock_guard<std::mutex> lock(input_buffer_mutex_);
  input_buffer_.clear();
  output_buffer_.clear();
  if (is_speaking_ && vad_state_change_callback_) {
    vad_state_change_callback_(false);
  }
  is_speaking_ = false;
}

bool AfeAudioProcessor::IsRunning() {
  return running_;
}

void AfeAudioProcessor::OnOutput(
    std::function<void(std::vector<int16_t> &&data)> callback) {
  output_callback_ = std::move(callback);
}

void AfeAudioProcessor::OnVadStateChange(
    std::function<void(bool speaking)> callback) {
  vad_state_change_callback_ = std::move(callback);
}

size_t AfeAudioProcessor::GetFeedSize() {
  if (afe_data_ == nullptr || afe_iface_ == nullptr) {
    return static_cast<size_t>(frame_samples_);
  }
  return afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeAudioProcessor::EnableDeviceAec(bool enable) {
  device_aec_enabled_ = enable;
  if (afe_data_ == nullptr || afe_iface_ == nullptr) {
    if (enable) {
      ESP_LOGW(kTag, "device AEC requested but AFE is unavailable");
    }
    return;
  }
  if (enable) {
    afe_iface_->disable_vad(afe_data_);
    afe_iface_->enable_aec(afe_data_);
  } else {
    afe_iface_->disable_aec(afe_data_);
    afe_iface_->enable_vad(afe_data_);
  }
}

void AfeAudioProcessor::AudioProcessorTask() {
  while (!worker_stop_requested_) {
    if (!running_) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (afe_data_ == nullptr || afe_iface_ == nullptr) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    auto *res = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
    if (worker_stop_requested_) {
      break;
    }
    if (!running_) {
      continue;
    }
    if (res == nullptr || res->ret_value == ESP_FAIL) {
      continue;
    }

    if (vad_state_change_callback_) {
      if (res->vad_state == VAD_SPEECH && !is_speaking_) {
        is_speaking_ = true;
        vad_state_change_callback_(true);
      } else if (res->vad_state == VAD_SILENCE && is_speaking_) {
        is_speaking_ = false;
        vad_state_change_callback_(false);
      }
    }

    if (!output_callback_ || res->data == nullptr || res->data_size <= 0) {
      continue;
    }

    const size_t samples =
        static_cast<size_t>(res->data_size / sizeof(int16_t));
    output_buffer_.insert(output_buffer_.end(), res->data, res->data + samples);
    while (output_buffer_.size() >= static_cast<size_t>(frame_samples_)) {
      if (output_buffer_.size() == static_cast<size_t>(frame_samples_)) {
        output_callback_(std::move(output_buffer_));
        output_buffer_.clear();
        output_buffer_.reserve(static_cast<size_t>(frame_samples_));
      } else {
        output_callback_(std::vector<int16_t>(
            output_buffer_.begin(),
            output_buffer_.begin() + frame_samples_));
        output_buffer_.erase(output_buffer_.begin(),
                             output_buffer_.begin() + frame_samples_);
      }
    }
  }
}

void AfeAudioProcessor::EmitFallbackFrames(std::vector<int16_t> &&data) {
  if (!output_callback_ || codec_ == nullptr) {
    return;
  }
  if (vad_state_change_callback_) {
    vad_state_change_callback_(true);
  }
  auto mono_data = ExtractPrimaryMicChannel(data, codec_);
  output_buffer_.insert(output_buffer_.end(), mono_data.begin(), mono_data.end());
  while (output_buffer_.size() >= static_cast<size_t>(frame_samples_)) {
    if (output_buffer_.size() == static_cast<size_t>(frame_samples_)) {
      output_callback_(std::move(output_buffer_));
      output_buffer_.clear();
      output_buffer_.reserve(static_cast<size_t>(frame_samples_));
    } else {
      output_callback_(std::vector<int16_t>(
          output_buffer_.begin(),
          output_buffer_.begin() + frame_samples_));
      output_buffer_.erase(output_buffer_.begin(),
                           output_buffer_.begin() + frame_samples_);
    }
  }
  if (vad_state_change_callback_) {
    vad_state_change_callback_(false);
  }
}

}  // namespace official_chat
