#pragma once

#include <cstdint>
#include <cstring>
#include <functional>

namespace official_chat {

class OggDemuxer {
 private:
  enum class ParseState : int8_t {
    kFindPage,
    kParseHeader,
    kParseSegments,
    kParseData,
  };

  struct OpusInfo {
    bool head_seen = false;
    bool tags_seen = false;
    int sample_rate = 48000;
  };

  struct Context {
    bool packet_continued = false;
    uint8_t header[27];
    uint8_t seg_table[255];
    uint8_t packet_buf[8192];
    size_t packet_len = 0;
    size_t seg_count = 0;
    size_t seg_index = 0;
    size_t data_offset = 0;
    size_t bytes_needed = 0;
    size_t seg_remaining = 0;
    size_t body_size = 0;
    size_t body_offset = 0;
  };

 public:
  OggDemuxer();

  void Reset();
  size_t Process(const uint8_t *data, size_t size);
  void OnDemuxerFinished(
      std::function<void(const uint8_t *data, int sample_rate, size_t len)>
          on_demuxer_finished);

 private:
  ParseState state_ = ParseState::kFindPage;
  Context ctx_ = {};
  OpusInfo opus_info_ = {};
  std::function<void(const uint8_t *, int, size_t)> on_demuxer_finished_;
};

}  // namespace official_chat
