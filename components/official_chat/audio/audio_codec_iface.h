#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace official_chat {

class AudioCodecIface {
 public:
  virtual ~AudioCodecIface() = default;

  virtual bool Initialize() = 0;
  virtual void Shutdown() = 0;
  virtual void Start() = 0;
  virtual void SetOutputVolume(int volume) = 0;
  virtual void SetInputGain(float gain_db) = 0;
  virtual void EnableInput(bool enable) = 0;
  virtual void EnableOutput(bool enable) = 0;
  virtual void OutputData(std::vector<int16_t> &data) = 0;
  virtual bool InputData(std::vector<int16_t> &data) = 0;
  virtual bool duplex() const = 0;
  virtual const char *input_format() const = 0;
  virtual bool input_reference() const = 0;
  virtual int input_sample_rate() const = 0;
  virtual int input_channels() const = 0;
  virtual int output_sample_rate() const = 0;
  virtual int output_channels() const = 0;
  virtual int output_volume() const = 0;
  virtual float input_gain() const = 0;
  virtual bool input_enabled() const = 0;
  virtual bool output_enabled() const = 0;
};

}  // namespace official_chat
