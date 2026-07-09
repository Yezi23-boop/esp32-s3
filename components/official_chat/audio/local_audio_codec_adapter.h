#pragma once

#include <vector>

#include "audio_codec_iface.h"

namespace official_chat {

class LocalAudioCodecAdapter : public AudioCodecIface {
 public:
  LocalAudioCodecAdapter() = default;
  ~LocalAudioCodecAdapter() override = default;

  bool Initialize() override;
  void Shutdown() override;
  void Start() override;
  void SetOutputVolume(int volume) override;
  void SetInputGain(float gain_db) override;
  void EnableInput(bool enable) override;
  void EnableOutput(bool enable) override;
  void OutputData(std::vector<int16_t> &data) override;
  bool InputData(std::vector<int16_t> &data) override;
  bool duplex() const override;
  const char *input_format() const override;
  bool input_reference() const override;
  int input_sample_rate() const override;
  int input_channels() const override;
  int output_sample_rate() const override;
  int output_channels() const override;
  int output_volume() const override;
  float input_gain() const override;
  bool input_enabled() const override;
  bool output_enabled() const override;

 private:
  bool input_session_acquired_ = false;
  bool output_session_acquired_ = false;
  int output_volume_ = 0;
  float input_gain_db_ = 0.0f;
};

}  // namespace official_chat
