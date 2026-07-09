#include "audio/demuxer/ogg_demuxer.h"

#include <algorithm>

#include <esp_log.h>

namespace official_chat {

namespace {

constexpr char kTag[] = "official_ogg";

}  // namespace

OggDemuxer::OggDemuxer() {
  Reset();
}

void OggDemuxer::Reset() {
  opus_info_ = {};
  opus_info_.sample_rate = 48000;

  state_ = ParseState::kFindPage;
  ctx_ = {};
  ctx_.bytes_needed = 4;
  std::memset(ctx_.header, 0, sizeof(ctx_.header));
  std::memset(ctx_.seg_table, 0, sizeof(ctx_.seg_table));
  std::memset(ctx_.packet_buf, 0, sizeof(ctx_.packet_buf));
}

void OggDemuxer::OnDemuxerFinished(
    std::function<void(const uint8_t *data, int sample_rate, size_t len)>
        on_demuxer_finished) {
  on_demuxer_finished_ = std::move(on_demuxer_finished);
}

size_t OggDemuxer::Process(const uint8_t *data, size_t size) {
  size_t processed = 0;

  while (processed < size) {
    switch (state_) {
      case ParseState::kFindPage: {
        if (ctx_.bytes_needed < 4) {
          const size_t to_copy =
              std::min(size - processed, ctx_.bytes_needed);
          std::memcpy(ctx_.header + (4 - ctx_.bytes_needed), data + processed,
                      to_copy);
          processed += to_copy;
          ctx_.bytes_needed -= to_copy;
          if (ctx_.bytes_needed == 0) {
            if (std::memcmp(ctx_.header, "OggS", 4) == 0) {
              state_ = ParseState::kParseHeader;
              ctx_.data_offset = 4;
              ctx_.bytes_needed = 23;
            } else {
              std::memmove(ctx_.header, ctx_.header + 1, 3);
              ctx_.bytes_needed = 1;
            }
          } else {
            return processed;
          }
        } else {
          bool found = false;
          size_t index = 0;
          const size_t remaining = size - processed;
          for (; index + 4 <= remaining; ++index) {
            if (std::memcmp(data + processed + index, "OggS", 4) == 0) {
              found = true;
              break;
            }
          }
          if (found) {
            processed += index + 4;
            state_ = ParseState::kParseHeader;
            ctx_.data_offset = 4;
            ctx_.bytes_needed = 23;
          } else {
            const size_t partial_len = remaining - index;
            if (partial_len > 0) {
              std::memcpy(ctx_.header, data + processed + index, partial_len);
              ctx_.bytes_needed = 4 - partial_len;
              processed += index + partial_len;
            } else {
              processed += index;
            }
            return processed;
          }
        }
        break;
      }
      case ParseState::kParseHeader: {
        const size_t available = size - processed;
        if (available < ctx_.bytes_needed) {
          std::memcpy(ctx_.header + ctx_.data_offset, data + processed,
                      available);
          ctx_.data_offset += available;
          ctx_.bytes_needed -= available;
          processed += available;
          return processed;
        }

        std::memcpy(ctx_.header + ctx_.data_offset, data + processed,
                    ctx_.bytes_needed);
        processed += ctx_.bytes_needed;
        ctx_.data_offset += ctx_.bytes_needed;
        ctx_.bytes_needed = 0;

        if (ctx_.header[4] != 0) {
          ESP_LOGE(kTag, "invalid ogg version: %d", ctx_.header[4]);
          Reset();
          break;
        }

        ctx_.seg_count = ctx_.header[26];
        if (ctx_.seg_count == 0 || ctx_.seg_count > 255) {
          Reset();
          break;
        }
        state_ = ParseState::kParseSegments;
        ctx_.bytes_needed = ctx_.seg_count;
        ctx_.data_offset = 0;
        break;
      }
      case ParseState::kParseSegments: {
        const size_t available = size - processed;
        if (available < ctx_.bytes_needed) {
          std::memcpy(ctx_.seg_table + ctx_.data_offset, data + processed,
                      available);
          ctx_.data_offset += available;
          ctx_.bytes_needed -= available;
          processed += available;
          return processed;
        }

        std::memcpy(ctx_.seg_table + ctx_.data_offset, data + processed,
                    ctx_.bytes_needed);
        processed += ctx_.bytes_needed;
        ctx_.seg_index = 0;
        ctx_.data_offset = 0;
        ctx_.seg_remaining = 0;
        ctx_.body_size = 0;
        ctx_.body_offset = 0;
        for (size_t i = 0; i < ctx_.seg_count; ++i) {
          ctx_.body_size += ctx_.seg_table[i];
        }
        state_ = ParseState::kParseData;
        break;
      }
      case ParseState::kParseData: {
        while (ctx_.seg_index < ctx_.seg_count && processed < size) {
          uint8_t seg_len = ctx_.seg_table[ctx_.seg_index];
          if (ctx_.seg_remaining > 0) {
            seg_len = static_cast<uint8_t>(ctx_.seg_remaining);
          } else {
            ctx_.seg_remaining = seg_len;
          }

          if (ctx_.packet_len + seg_len > sizeof(ctx_.packet_buf)) {
            ESP_LOGE(kTag, "ogg packet buffer overflow");
            Reset();
            return processed;
          }

          const size_t available = size - processed;
          const size_t to_copy = std::min<size_t>(available, seg_len);
          std::memcpy(ctx_.packet_buf + ctx_.packet_len, data + processed,
                      to_copy);
          ctx_.packet_len += to_copy;
          ctx_.seg_remaining -= to_copy;
          ctx_.body_offset += to_copy;
          processed += to_copy;
          if (ctx_.seg_remaining > 0) {
            return processed;
          }

          if (ctx_.seg_table[ctx_.seg_index] < 255) {
            if (!opus_info_.head_seen &&
                ctx_.packet_len >= 19 &&
                std::memcmp(ctx_.packet_buf, "OpusHead", 8) == 0) {
              opus_info_.head_seen = true;
              opus_info_.sample_rate =
                  static_cast<int>(ctx_.packet_buf[12] |
                                   (ctx_.packet_buf[13] << 8) |
                                   (ctx_.packet_buf[14] << 16) |
                                   (ctx_.packet_buf[15] << 24));
            } else if (!opus_info_.tags_seen &&
                       ctx_.packet_len >= 8 &&
                       std::memcmp(ctx_.packet_buf, "OpusTags", 8) == 0) {
              opus_info_.tags_seen = true;
            } else if (on_demuxer_finished_) {
              on_demuxer_finished_(ctx_.packet_buf, opus_info_.sample_rate,
                                   ctx_.packet_len);
            }
            ctx_.packet_len = 0;
          }

          ++ctx_.seg_index;
        }

        if (ctx_.seg_index >= ctx_.seg_count) {
          state_ = ParseState::kFindPage;
          ctx_.bytes_needed = 4;
          ctx_.data_offset = 0;
        }
        break;
      }
    }
  }

  return processed;
}

}  // namespace official_chat
