#pragma once

#include <stddef.h>

#include <cstring>
#include <vector>

#include "audio/audio_codec_iface.h"

namespace official_chat {

inline const char *GetInputFormatOrDefault(const AudioCodecIface *codec) {
  if (codec == nullptr) {
    return "M";
  }
  const char *format = codec->input_format();
  if (format == nullptr || format[0] == '\0') {
    return "M";
  }
  return format;
}

inline int FindPrimaryMicChannelIndex(const AudioCodecIface *codec) {
  const char *format = GetInputFormatOrDefault(codec);
  const char *mic = std::strchr(format, 'M');
  if (mic == nullptr) {
    return 0;
  }
  return static_cast<int>(mic - format);
}

inline std::vector<int16_t> ExtractPrimaryMicChannel(
    const std::vector<int16_t> &data, const AudioCodecIface *codec) {
  if (codec == nullptr) {
    return data;
  }

  const int channels = codec->input_channels();
  if (channels <= 1 || data.empty()) {
    return data;
  }

  const int primary_channel = FindPrimaryMicChannelIndex(codec);
  const size_t frame_count = data.size() / static_cast<size_t>(channels);
  std::vector<int16_t> mono_data;
  mono_data.reserve(frame_count);

  for (size_t frame = 0; frame < frame_count; ++frame) {
    const size_t sample_index =
        frame * static_cast<size_t>(channels) +
        static_cast<size_t>(primary_channel);
    if (sample_index >= data.size()) {
      break;
    }
    mono_data.push_back(data[sample_index]);
  }

  return mono_data;
}

}  // namespace official_chat
