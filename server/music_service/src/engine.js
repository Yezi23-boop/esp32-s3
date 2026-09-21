import fs from "node:fs";
import crypto from "node:crypto";
import { spawn } from "node:child_process";
import { OggOpusPacketTransform } from "./ogg_opus_packet_transform.js";

function id(prefix) {
  return `${prefix}-${crypto.randomUUID()}`;
}

export class MusicEngine {
  constructor(store, config) {
    this.store = store;
    this.config = config;
    this.child = null;
    this.response = null;
    this.framer = null;
    this.activeDeviceId = null;
    this.disconnectTimer = null;
    this.startedAt = 0;
  }

  getSession(deviceId) {
    return this.store.getSession(deviceId);
  }

  getCommand(deviceId, commandId) {
    return commandId ? this.store.getCommand(deviceId, commandId) : null;
  }

  createSession(deviceId, commandId, definition) {
    if (!definition?.sourceId || !definition?.track?.track_id || !definition.input) {
      return this.error("track_unplayable");
    }
    const mode = definition.mode || "repeat_all";
    if (!["order", "repeat_all", "repeat_one", "shuffle", "smart"].includes(mode)) {
      return this.error("invalid_mode");
    }
    return this.#idempotent(deviceId, commandId, () => {
      this.#stopProcess();
      const now = Date.now();
      const session = {
        device_id: deviceId,
        music_session_id: id("music"),
        state: "buffering",
        source_id: definition.sourceId,
        track_id: definition.track.track_id,
        title: definition.track.title,
        artist: definition.track.artist,
        // source_path remains an opaque server-side FFmpeg input. It may be a
        // local fixture or a private upstream URL and must never be returned.
        source_path: definition.input,
        queue: definition.queue?.length ? definition.queue : [definition.track],
        mode,
        position_ms: 0,
        stream_id: id("stream"),
        stream_issued_at: now,
        created_at: now,
        updated_at: now,
      };
      return this.publicSession(this.store.saveSession(session));
    });
  }

  createTestSession(deviceId, commandId) {
    if (!this.config.testMode || !this.config.testStreamPath) {
      return this.error("music_auth_required");
    }
    if (!fs.existsSync(this.config.testStreamPath)) {
      return this.error("upstream_unavailable");
    }
    return this.createSession(deviceId, commandId, {
      sourceId: "test",
      track: { track_id: "test-track", title: "固定测试流", artist: "AI Memory Watch" },
      queue: [{ track_id: "test-track", title: "固定测试流", artist: "AI Memory Watch" }],
      input: this.config.testStreamPath,
    });
  }

  pause(deviceId, commandId) {
    return this.#idempotent(deviceId, commandId, () => {
      const session = this.store.getSession(deviceId);
      if (!session) return this.error("music_session_not_found");
      this.#stopProcess();
      session.position_ms += this.#elapsedMs(session);
      session.state = "paused";
      session.updated_at = Date.now();
      return this.publicSession(this.store.saveSession(session));
    });
  }

  async resume(deviceId, commandId, resolveInput) {
    const previous = this.getCommand(deviceId, commandId);
    if (previous) return previous;
      const session = this.store.getSession(deviceId);
    if (!session) return this.error("music_session_not_found");
    if (resolveInput) session.source_path = await resolveInput(session.track_id);
    this.#stopProcess();
    session.state = "buffering";
    session.stream_id = id("stream");
    session.stream_issued_at = Date.now();
    session.updated_at = Date.now();
    const result = this.publicSession(this.store.saveSession(session));
    if (commandId) this.store.saveCommand(deviceId, commandId, result);
    return result;
  }

  async setMode(deviceId, commandId, mode, resolveSmartQueue) {
    const previous = this.getCommand(deviceId, commandId);
    if (previous) return previous;
    if (!["order", "repeat_all", "repeat_one", "shuffle", "smart"].includes(mode)) {
      return this.#saveResult(deviceId, commandId, this.error("invalid_mode"));
    }
    const session = this.store.getSession(deviceId);
    if (!session) return this.#saveResult(deviceId, commandId, this.error("music_session_not_found"));
    if (mode === "smart") {
      if (typeof resolveSmartQueue !== "function") {
        return this.#saveResult(deviceId, commandId, this.error("smart_mode_requires_playlist"));
      }
      const smart = await resolveSmartQueue(session);
      if (!smart?.queue?.length) {
        return this.#saveResult(deviceId, commandId, this.error("smart_mode_unavailable"));
      }
      session.queue = smart.queue;
      if (smart.sourceId) session.source_id = smart.sourceId;
    }
    session.mode = mode;
    session.updated_at = Date.now();
    return this.#saveResult(deviceId, commandId, this.publicSession(this.store.saveSession(session)));
  }

  async next(deviceId, commandId, resolveInput) {
    return this.#move(deviceId, commandId, 1, resolveInput);
  }

  async previous(deviceId, commandId, resolveInput) {
    return this.#move(deviceId, commandId, -1, resolveInput);
  }

  destroy(deviceId, commandId) {
    return this.#idempotent(deviceId, commandId, () => {
      this.#stopProcess();
      this.store.deleteSession(deviceId);
      return { state: "stopped", music_session_id: null };
    });
  }

  attachStream(deviceId, streamId, response) {
    const session = this.store.getSession(deviceId);
    if (!session || session.stream_id !== streamId) return this.error("music_stream_expired");
    if (session.state === "paused" || session.state === "stopped") {
      return this.error("music_session_paused");
    }
    if (session.stream_issued_at + this.config.streamAttachTimeoutMs < Date.now()) {
      session.state = "paused";
      session.updated_at = Date.now();
      this.store.saveSession(session);
      return this.error("music_stream_expired");
    }
    if (this.response) return this.error("music_stream_already_attached");
    if (this.child && this.activeDeviceId !== deviceId) {
      return this.error("music_stream_already_attached");
    }
    this.#clearDisconnectTimer();
    this.response = response;
    this.activeDeviceId = deviceId;
    this.startedAt = Date.now();
    session.state = "playing";
    session.updated_at = Date.now();
    this.store.saveSession(session);
    if (!this.child) {
      const child = spawn(
        this.config.ffmpegPath,
        [
          "-hide_banner",
          "-loglevel",
          "error",
          // 手表端 512 KB ring 需要在正常播放前后保留余量；满时由 TCP 背压
          // 约束发送速度，不能用 -re 把媒体流强制限制为实时到达。
          "-i",
          session.source_path,
          "-vn",
          "-ac",
          "1",
          "-ar",
          "48000",
          "-c:a",
          "libopus",
          "-b:a",
          "128k",
          "-frame_duration",
          "20",
          "-page_duration",
          "200000",
          "-flush_packets",
          "1",
          "-f",
          "ogg",
          "pipe:1",
        ],
        { stdio: ["ignore", "pipe", "pipe"] },
      );
      this.child = child;
      child.stderr.on("data", () => {});
      const finish = () => {
        if (this.child !== child) return;
        const elapsedMs = this.#elapsedMs(session);
        this.child = null;
        this.response = null;
        this.activeDeviceId = null;
        this.#clearDisconnectTimer();
        const current = this.store.getSession(deviceId);
        if (!current) return;
        current.position_ms += elapsedMs;
        current.state = "paused";
        current.updated_at = Date.now();
        this.store.saveSession(current);
      };
      child.once("close", finish);
    }
    setImmediate(() => {
      if (this.response === response && this.child && !response.destroyed) {
        this.child.stdout.unpipe();
        const framer = new OggOpusPacketTransform();
        this.framer = framer;
        framer.once("error", () => response.destroy());
        this.child.stdout.pipe(framer).pipe(response);
      }
    });
    response.once("close", () => {
      if (this.response !== response) return;
      this.child?.stdout.unpipe(this.framer);
      this.framer?.unpipe(response);
      this.framer?.destroy();
      this.framer = null;
      this.response = null;
      this.#scheduleDisconnectCleanup(deviceId);
    });
    return { state: "streaming" };
  }

  close() {
    this.#stopProcess();
  }

  publicSession(session) {
    if (!session || session.state === "error") return session;
    return {
      state: session.state,
      music_session_id: session.music_session_id,
      stream_id: session.stream_id,
      source_id: session.source_id,
      track: {
        track_id: session.track_id,
        title: session.title,
        artist: session.artist,
      },
      mode: session.mode,
      position_ms: session.position_ms,
    };
  }

  error(errorCode) {
    return { state: "error", error_code: errorCode };
  }

  async #move(deviceId, commandId, delta, resolveInput) {
    const previous = this.getCommand(deviceId, commandId);
    if (previous) return previous;
    const session = this.store.getSession(deviceId);
    if (!session) return this.error("music_session_not_found");
    const queue = session.queue?.length ? session.queue : [{
      track_id: session.track_id, title: session.title, artist: session.artist,
    }];
    const currentIndex = Math.max(0, queue.findIndex((item) => item.track_id === session.track_id));
    let index = currentIndex;
    if (session.mode === "repeat_one") {
      index = currentIndex;
    } else if (session.mode === "shuffle") {
      index = queue.length > 1 ? (currentIndex + 1 + Math.floor(Math.random() * (queue.length - 1))) % queue.length : currentIndex;
    } else if (session.mode === "order") {
      if (delta > 0 && currentIndex >= queue.length - 1) {
        const result = this.#stopSession(session);
        if (commandId) this.store.saveCommand(deviceId, commandId, result);
        return result;
      }
      index = Math.max(0, Math.min(queue.length - 1, currentIndex + delta));
    } else {
      index = (currentIndex + delta + queue.length) % queue.length;
    }
    const track = queue[index];
    const input = resolveInput ? await resolveInput(track.track_id) : session.source_path;
    const result = this.#replaceTrack(session, track, input);
    if (commandId) this.store.saveCommand(deviceId, commandId, result);
    return result;
  }

  #replaceTrack(session, track, input) {
    this.#stopProcess();
    session.position_ms = 0;
    session.track_id = track.track_id;
    session.title = track.title;
    session.artist = track.artist;
    session.source_path = input;
    session.state = "buffering";
    session.stream_id = id("stream");
    session.stream_issued_at = Date.now();
    session.updated_at = Date.now();
    return this.publicSession(this.store.saveSession(session));
  }

  #stopSession(session) {
    const elapsedMs = this.#elapsedMs(session);
    this.#stopProcess();
    session.position_ms += elapsedMs;
    session.state = "stopped";
    session.stream_id = null;
    session.stream_issued_at = null;
    session.updated_at = Date.now();
    return this.publicSession(this.store.saveSession(session));
  }

  #saveResult(deviceId, commandId, result) {
    if (commandId) this.store.saveCommand(deviceId, commandId, result);
    return result;
  }

  #idempotent(deviceId, commandId, operation) {
    if (!commandId) return operation();
    const previous = this.store.getCommand(deviceId, commandId);
    if (previous) return previous;
    const result = operation();
    this.store.saveCommand(deviceId, commandId, result);
    return result;
  }

  #elapsedMs(session) {
    return this.child && this.startedAt ? Math.max(0, Date.now() - this.startedAt) : 0;
  }

  #stopProcess() {
    this.#clearDisconnectTimer();
    const child = this.child;
    const response = this.response;
    const framer = this.framer;
    this.child = null;
    this.response = null;
    this.framer = null;
    this.activeDeviceId = null;
    if (child) {
      // 先销毁 stdout，避免 FFmpeg 已排队的数据继续占住暂停后的 HTTP 响应。
      child.stdout.unpipe(framer);
      child.stdout.destroy();
      child.kill("SIGTERM");
    }
    framer?.unpipe(response);
    framer?.destroy();
    if (response && !response.destroyed) response.destroy();
  }

  #scheduleDisconnectCleanup(deviceId) {
    this.#clearDisconnectTimer();
    if (!this.child) return;
    this.disconnectTimer = setTimeout(() => {
      this.disconnectTimer = null;
      if (this.response || !this.child || this.activeDeviceId !== deviceId) {
        return;
      }
      const session = this.store.getSession(deviceId);
      if (session) {
        session.position_ms += this.#elapsedMs(session);
        session.state = "paused";
        session.updated_at = Date.now();
        this.store.saveSession(session);
      }
      this.#stopProcess();
    }, this.config.streamDisconnectCleanupMs);
  }

  #clearDisconnectTimer() {
    if (this.disconnectTimer) {
      clearTimeout(this.disconnectTimer);
      this.disconnectTimer = null;
    }
  }
}
