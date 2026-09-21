import { Transform } from "node:stream";

export const WATCH_OPUS_PACKET_MAX_BYTES = 1536;

/**
 * 将 FFmpeg 的 Ogg/Opus 输出转换为手表私有的长度前缀 Opus packet 流。
 * HTTP/TCP 不保留写入边界，因此每个包以 uint16 big-endian 长度显式分帧。
 */
export class OggOpusPacketTransform extends Transform {
  constructor() {
    super();
    this.pending = Buffer.alloc(0);
    this.packetParts = [];
    this.packetBytes = 0;
    this.seenAudio = false;
  }

  _transform(chunk, _encoding, callback) {
    try {
      this.pending = this.pending.length === 0
        ? Buffer.from(chunk)
        : Buffer.concat([this.pending, chunk]);
      this.#processPages();
      callback();
    } catch (error) {
      callback(error);
    }
  }

  _flush(callback) {
    callback(this.pending.length === 0 && this.packetBytes === 0
      ? undefined
      : new Error("truncated_ogg_opus_stream"));
  }

  #processPages() {
    while (this.pending.length >= 27) {
      if (!this.pending.subarray(0, 4).equals(Buffer.from("OggS")) || this.pending[4] !== 0) {
        throw new Error("invalid_ogg_opus_page");
      }
      const segmentCount = this.pending[26];
      const headerBytes = 27 + segmentCount;
      if (this.pending.length < headerBytes) return;
      const segments = this.pending.subarray(27, headerBytes);
      let bodyBytes = 0;
      for (const size of segments) bodyBytes += size;
      if (this.pending.length < headerBytes + bodyBytes) return;

      const continued = (this.pending[5] & 0x01) !== 0;
      if (continued !== (this.packetBytes > 0)) {
        throw new Error("invalid_ogg_opus_continuation");
      }
      let bodyOffset = headerBytes;
      for (const size of segments) {
        if (size > 0) {
          this.packetParts.push(this.pending.subarray(bodyOffset, bodyOffset + size));
          this.packetBytes += size;
          if (this.packetBytes > WATCH_OPUS_PACKET_MAX_BYTES) {
            throw new Error("opus_packet_too_large");
          }
        }
        bodyOffset += size;
        if (size < 255) this.#emitPacket();
      }
      this.pending = this.pending.subarray(headerBytes + bodyBytes);
    }
  }

  #emitPacket() {
    const packet = Buffer.concat(this.packetParts, this.packetBytes);
    this.packetParts = [];
    this.packetBytes = 0;
    if (!this.seenAudio && (packet.subarray(0, 8).equals(Buffer.from("OpusHead")) ||
        packet.subarray(0, 8).equals(Buffer.from("OpusTags")))) {
      return;
    }
    this.seenAudio = true;
    const framed = Buffer.allocUnsafe(packet.length + 2);
    framed.writeUInt16BE(packet.length);
    packet.copy(framed, 2);
    this.push(framed);
  }
}
