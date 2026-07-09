#include "audio/local_audio_codec_adapter.h"

#include <cstring>

#include "audio_codec.h"
#include "audio_platform_config.h"
#include "esp_log.h"

namespace official_chat {

namespace {
constexpr const char *kTag = "official_chat_codec";
constexpr uint32_t kInputSessionTimeoutMs = 500U;
constexpr uint32_t kOutputSessionTimeoutMs = 500U;
}  // namespace

bool LocalAudioCodecAdapter::Initialize() {
  int volume = 0;
  if (audio_codec_get_volume(&volume) != ESP_OK) {
    return false;
  }
  output_volume_ = volume;
  return true;
}

void LocalAudioCodecAdapter::Shutdown() {
  EnableInput(false);
  EnableOutput(false);
}

void LocalAudioCodecAdapter::Start() {}

void LocalAudioCodecAdapter::SetOutputVolume(int volume) {
  if (audio_codec_set_volume(volume) == ESP_OK) {
    output_volume_ = volume;
  }
}

void LocalAudioCodecAdapter::SetInputGain(float gain_db) {
  if (audio_codec_set_record_gain(gain_db) == ESP_OK) {
    input_gain_db_ = gain_db;
  }
}

void LocalAudioCodecAdapter::EnableInput(bool enable) {
  if (enable) {
    if (input_session_acquired_) {
      return;
    }

    const esp_err_t ret = audio_codec_acquire_input(
        AUDIO_CODEC_OWNER_OFFICIAL_CHAT, kInputSessionTimeoutMs);
    if (ret != ESP_OK) {
      ESP_LOGW(kTag, "official_chat input session acquire failed: %s",
               esp_err_to_name(ret));
      return;
    }

    input_session_acquired_ = true;
    return;
  }

  if (!input_session_acquired_) {
    return;
  }

  const esp_err_t ret =
      audio_codec_release_input(AUDIO_CODEC_OWNER_OFFICIAL_CHAT);
  if (ret != ESP_OK) {
    ESP_LOGW(kTag, "official_chat input session release failed: %s",
             esp_err_to_name(ret));
    return;
  }
  input_session_acquired_ = false;
}

void LocalAudioCodecAdapter::EnableOutput(bool enable) {
  if (enable) {
    if (output_session_acquired_) {
      (void)audio_codec_set_pa_enable(true);
      return;
    }

    const esp_err_t ret = audio_codec_acquire_output(
        AUDIO_CODEC_OWNER_OFFICIAL_CHAT, kOutputSessionTimeoutMs);
    if (ret != ESP_OK) {
      ESP_LOGW(kTag, "official_chat output session acquire failed: %s",
               esp_err_to_name(ret));
      return;
    }

    output_session_acquired_ = true;
    (void)audio_codec_set_pa_enable(true);
    return;
  }

  if (!output_session_acquired_) {
    return;
  }

  (void)audio_codec_set_pa_enable(false);
  const esp_err_t ret =
      audio_codec_release_output(AUDIO_CODEC_OWNER_OFFICIAL_CHAT);
  if (ret != ESP_OK) {
    ESP_LOGW(kTag, "official_chat output session release failed: %s",
             esp_err_to_name(ret));
    return;
  }
  output_session_acquired_ = false;
}

void LocalAudioCodecAdapter::OutputData(std::vector<int16_t> &data) {
  if (data.empty() || !output_session_acquired_) {
    return;
  }
  (void)audio_codec_write(data.data(), data.size() * sizeof(int16_t));
}

bool LocalAudioCodecAdapter::InputData(std::vector<int16_t> &data) {
  if (!input_session_acquired_) {
    return false;
  }

  size_t bytes_read = 0;
  const size_t bytes = data.size() * sizeof(int16_t);
  return audio_codec_read(data.data(), bytes, &bytes_read, portMAX_DELAY) ==
             ESP_OK &&
         bytes_read == bytes;
}

bool LocalAudioCodecAdapter::duplex() const {
  return true;
}

const char *LocalAudioCodecAdapter::input_format() const {
  return AUDIO_PLATFORM_ADC_CHANNEL_FORMAT;
}

bool LocalAudioCodecAdapter::input_reference() const {
  for (const char *cursor = AUDIO_PLATFORM_ADC_CHANNEL_FORMAT;
       cursor != nullptr && *cursor != '\0'; ++cursor) {
    if (*cursor == 'R') {
      return true;
    }
  }
  return false;
}

int LocalAudioCodecAdapter::input_sample_rate() const {
  return AUDIO_PLATFORM_HW_SAMPLE_RATE;
}

int LocalAudioCodecAdapter::input_channels() const {
  return AUDIO_PLATFORM_HW_INPUT_CHANNELS;
}

int LocalAudioCodecAdapter::output_sample_rate() const {
  return AUDIO_PLATFORM_HW_SAMPLE_RATE;
}

int LocalAudioCodecAdapter::output_channels() const {
  return AUDIO_PLATFORM_HW_OUTPUT_CHANNELS;
}

int LocalAudioCodecAdapter::output_volume() const {
  return output_volume_;
}

float LocalAudioCodecAdapter::input_gain() const {
  return input_gain_db_;
}

bool LocalAudioCodecAdapter::input_enabled() const {
  return input_session_acquired_;
}

bool LocalAudioCodecAdapter::output_enabled() const {
  return output_session_acquired_;
}

}  // namespace official_chat
