#include "audio/processors/no_audio_processor.h"

#include <esp_log.h>

#include "audio/input_format_utils.h"

namespace official_chat {

namespace {

constexpr char kTag[] = "official_no_ap";

}  // namespace

void NoAudioProcessor::Initialize(AudioCodecIface *codec, int frame_duration_ms,
                                  srmodel_list_t *models_list) {
  (void)models_list;
  codec_ = codec;
  frame_samples_ = frame_duration_ms * 16000 / 1000;
  first_output_logged_ = false;
  output_buffer_.clear();
  output_buffer_.reserve(static_cast<size_t>(frame_samples_));
}

void NoAudioProcessor::Feed(std::vector<int16_t> &&data) {
  if (!is_running_ || codec_ == nullptr || !output_callback_) {
    return;
  }

  if (vad_state_change_callback_) {
    vad_state_change_callback_(true);
  }

  auto mono_data = ExtractPrimaryMicChannel(data, codec_);
  output_buffer_.insert(output_buffer_.end(), mono_data.begin(), mono_data.end());

  while (output_buffer_.size() >= static_cast<size_t>(frame_samples_)) {
    if (!first_output_logged_) {
      ESP_LOGI(kTag, "no-audio processor produced frame samples=%d",
               frame_samples_);
      first_output_logged_ = true;
    }
    if (output_buffer_.size() == static_cast<size_t>(frame_samples_)) {
      output_callback_(std::move(output_buffer_));
      output_buffer_.clear();
      output_buffer_.reserve(static_cast<size_t>(frame_samples_));
    } else {
      output_callback_(std::vector<int16_t>(
          output_buffer_.begin(), output_buffer_.begin() + frame_samples_));
      output_buffer_.erase(output_buffer_.begin(),
                           output_buffer_.begin() + frame_samples_);
    }
  }

  if (vad_state_change_callback_) {
    vad_state_change_callback_(false);
  }
}

void NoAudioProcessor::Start() {
  is_running_ = true;
}

void NoAudioProcessor::Stop() {
  is_running_ = false;
  output_buffer_.clear();
}

bool NoAudioProcessor::IsRunning() {
  return is_running_;
}

void NoAudioProcessor::OnOutput(
    std::function<void(std::vector<int16_t> &&data)> callback) {
  output_callback_ = std::move(callback);
}

void NoAudioProcessor::OnVadStateChange(
    std::function<void(bool speaking)> callback) {
  vad_state_change_callback_ = std::move(callback);
}

size_t NoAudioProcessor::GetFeedSize() {
  if (codec_ == nullptr) {
    return 0;
  }
  return static_cast<size_t>(frame_samples_);
}

void NoAudioProcessor::EnableDeviceAec(bool enable) {
  if (enable) {
    ESP_LOGW(kTag, "device AEC is not supported in local adapter mode");
  }
}

}  // namespace official_chat
