import crypto from "node:crypto";

const MCP_PROTOCOL_VERSION = "2025-06-18";
const MODES = ["order", "repeat_all", "repeat_one", "shuffle", "smart"];
const TERMINAL_COMMAND_STATES = new Set(["executed", "expired", "superseded"]);

const TOOL_NAMES = [
  "music_search",
  "music_status",
  "music_play",
  "music_pause",
  "music_resume",
  "music_previous",
  "music_next",
  "music_stop",
  "music_set_mode",
  "music_set_volume",
];

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

function textResult(value, isError = false) {
  const result = {
    content: [{ type: "text", text: JSON.stringify(value) }],
    structuredContent: value,
  };
  if (isError) result.isError = true;
  return result;
}

function errorResult(errorCode) {
  return textResult({ state: "error", error_code: errorCode }, true);
}

function commandResult(command, extra = {}) {
  return {
    state: command.state,
    command_id: command.command_id,
    action: command.action,
    ...(command.result && typeof command.result === "object"
      ? { result: publicResult(command.result) }
      : {}),
    ...(extra.snapshot ? { snapshot: publicSnapshot(extra.snapshot) } : {}),
  };
}

function publicTrack(track) {
  if (!track || typeof track !== "object") return undefined;
  return {
    track_id: track.track_id,
    title: track.title,
    artist: track.artist,
  };
}

function publicSnapshot(snapshot) {
  if (!snapshot || typeof snapshot !== "object") return null;
  const value = {};
  for (const key of [
    "state", "mode", "volume", "position_ms", "music_session_id", "source_id", "track_id",
  ]) {
    if (snapshot[key] !== undefined) value[key] = snapshot[key];
  }
  if (snapshot.track) value.track = publicTrack(snapshot.track);
  if (Array.isArray(snapshot.queue)) value.queue = snapshot.queue.map(publicTrack).filter(Boolean);
  return value;
}

function publicResult(result) {
  if (!result || typeof result !== "object") return {};
  const value = {};
  for (const key of [
    "state", "error_code", "mode", "volume", "position_ms", "music_session_id", "source_id",
  ]) {
    if (result[key] !== undefined) value[key] = result[key];
  }
  if (result.track) value.track = publicTrack(result.track);
  if (result.snapshot) value.snapshot = publicSnapshot(result.snapshot);
  return value;
}

function requireString(value, name, maxLength = 128) {
  if (typeof value !== "string" || value.trim().length === 0 || value.length > maxLength) {
    throw new Error(`invalid_${name}`);
  }
  return value.trim();
}

function validateMode(mode) {
  if (typeof mode !== "string" || !MODES.includes(mode)) throw new Error("invalid_mode");
  return mode;
}

export class MusicMcpServer {
  constructor({ store, engine, provider, config, ensureAccount }) {
    this.store = store;
    this.engine = engine;
    this.provider = provider;
    this.config = config;
    this.ensureAccount = ensureAccount;
    this.sessions = new Set();
  }

  async handleHttp(request, response, body) {
    const method = body?.method;
    const requestId = body?.id ?? null;
    if (method === "notifications/initialized" || method === "notifications/cancelled") {
      response.statusCode = 202;
      response.end();
      return;
    }
    if (method === "initialize") {
      const sessionId = crypto.randomUUID();
      this.sessions.add(sessionId);
      response.setHeader("Mcp-Session-Id", sessionId);
    }
    let result;
    try {
      result = await this.dispatch(method, body?.params || {});
    } catch (error) {
      result = { __error: error instanceof Error && error.message ? error.message : "internal_error" };
    }
    response.setHeader("MCP-Protocol-Version", MCP_PROTOCOL_VERSION);
    response.setHeader("Content-Type", "application/json");
    response.setHeader("Cache-Control", "no-store");
    if (result?.__error) {
      response.statusCode = 200;
      response.end(JSON.stringify({
        jsonrpc: "2.0",
        id: requestId,
        error: { code: -32000, message: result.__error },
      }));
      return;
    }
    response.statusCode = 200;
    response.end(JSON.stringify({ jsonrpc: "2.0", id: requestId, result }));
  }

  async dispatch(method, params) {
    if (method === "initialize") {
      return {
        protocolVersion: MCP_PROTOCOL_VERSION,
        capabilities: { tools: {} },
        serverInfo: { name: "watch-music", version: "0.1.0" },
      };
    }
    if (method === "ping") return {};
    if (method === "tools/list") return { tools: this.toolDefinitions() };
    if (method === "tools/call") {
      const name = requireString(params.name, "tool_name", 64);
      if (!TOOL_NAMES.includes(name)) return errorResult("unknown_tool");
      try {
        return await this.callTool(name, params.arguments || {});
      } catch (error) {
        return errorResult(error instanceof Error && error.message ? error.message : "internal_error");
      }
    }
    throw new Error("method_not_found");
  }

  toolDefinitions() {
    return [
      {
        name: "music_search",
        description: "搜索网易云歌曲并返回最多 5 个候选，不会直接播放。",
        inputSchema: {
          type: "object",
          properties: { query: { type: "string", minLength: 1, maxLength: 128 }, limit: { type: "integer", minimum: 1, maximum: 5 } },
          required: ["query"],
          additionalProperties: false,
        },
      },
      {
        name: "music_status",
        description: "读取 watch-001 当前音乐会话、模式和最近设备 ACK。",
        inputSchema: { type: "object", properties: {}, additionalProperties: false },
      },
      {
        name: "music_play",
        description: "搜索或选择网易云歌曲后立即请求 watch-001 播放。",
        inputSchema: {
          type: "object",
          properties: {
            query: { type: "string", minLength: 1, maxLength: 128 },
            source_id: { type: "string", maxLength: 64 },
            track_id: { type: "string", maxLength: 96 },
            mode: { type: "string", enum: MODES },
          },
          additionalProperties: false,
        },
      },
      ...["pause", "resume", "previous", "next", "stop"].map((action) => ({
        name: `music_${action}`,
        description: `${action} watch-001 当前音乐会话。`,
        inputSchema: { type: "object", properties: {}, additionalProperties: false },
      })),
      {
        name: "music_set_mode",
        description: "切换 watch-001 当前播放模式。",
        inputSchema: {
          type: "object",
          properties: { mode: { type: "string", enum: MODES } },
          required: ["mode"],
          additionalProperties: false,
        },
      },
      {
        name: "music_set_volume",
        description: "设置 watch-001 系统扬声器音量。",
        inputSchema: {
          type: "object",
          properties: { volume: { type: "integer", minimum: 0, maximum: 100 } },
          required: ["volume"],
          additionalProperties: false,
        },
      },
    ];
  }

  async callTool(name, args) {
    if (name === "music_search") return textResult(await this.search(args));
    if (name === "music_status") return textResult(this.status());
    if (name === "music_play") return textResult(await this.play(args));
    if (name === "music_set_mode") return textResult(await this.enqueue("mode", { mode: validateMode(args.mode) }));
    if (name === "music_set_volume") {
      if (!Number.isInteger(args.volume) || args.volume < 0 || args.volume > 100) {
        throw new Error("invalid_volume");
      }
      return textResult(await this.enqueue("volume", { volume: args.volume }));
    }
    const action = name.slice("music_".length);
    return textResult(await this.enqueue(action, {}));
  }

  async search(args) {
    const query = requireString(args.query, "query");
    const limit = Math.min(5, Math.max(1, Number.isInteger(args.limit) ? args.limit : 5));
    const account = await this.accountForProvider();
    if (typeof this.provider.search !== "function") throw new Error("music_search_unavailable");
    const tracks = await this.provider.search(query, account, limit);
    return { state: "ready", query, tracks: (tracks || []).slice(0, limit) };
  }

  status() {
    const session = this.engine.getSession(this.config.mcpDeviceId);
    const publicSession = session ? this.engine.publicSession(session) : null;
    if (publicSession) delete publicSession.stream_id;
    return {
      state: session?.state || "stopped",
      session: publicSession,
      device_id: this.config.mcpDeviceId,
      last_ack: (() => {
        const snapshot = this.store.getDeviceSnapshot(this.config.mcpDeviceId);
        return snapshot ? { ...snapshot, snapshot: publicSnapshot(snapshot.snapshot) } : null;
      })(),
    };
  }

  async play(args) {
    const hasQuery = typeof args.query === "string" && args.query.trim() !== "";
    const hasSource = typeof args.source_id === "string" && args.source_id.trim() !== "";
    if (!hasQuery && !hasSource) throw new Error("music_source_required");
    if (hasQuery && hasSource) throw new Error("query_source_mutually_exclusive");
    const mode = args.mode == null ? "repeat_all" : validateMode(args.mode);
    const account = await this.accountForProvider();
    let sourceId = hasSource ? requireString(args.source_id, "source_id", 64) : "search";
    let trackId = typeof args.track_id === "string" && args.track_id.trim() !== ""
      ? requireString(args.track_id, "track_id", 96) : null;
    let track = null;
    let queue = [];
    if (hasQuery) {
      if (typeof this.provider.search !== "function") throw new Error("music_search_unavailable");
      const results = await this.provider.search(requireString(args.query, "query"), account, 5);
      track = results?.[0] || null;
      if (!track) throw new Error("track_unplayable");
      trackId = track.track_id;
      queue = results;
    }
    if (typeof this.provider.playback === "function") {
      const definition = await this.provider.playback(sourceId, trackId, account, {
        mode,
        selectedTrack: track,
      });
      sourceId = definition.sourceId || sourceId;
      track = definition.track || track;
      queue = definition.queue || queue;
      trackId = track?.track_id || trackId;
    }
    if (!trackId) throw new Error("track_unplayable");
    const result = await this.enqueue("play", {
      source_id: sourceId,
      track_id: trackId,
      track,
      queue,
      mode,
    });
    return { ...result, track, source_id: sourceId, mode };
  }

  async enqueue(action, payload) {
    const command = this.store.createRemoteCommand(
      this.config.mcpDeviceId,
      action,
      payload,
      this.config.remoteCommandTtlMs,
    );
    const deadline = Date.now() + Math.max(100, this.config.mcpAckWaitMs);
    while (Date.now() < deadline) {
      const current = this.store.getRemoteCommand(this.config.mcpDeviceId, command.command_id);
      if (current && TERMINAL_COMMAND_STATES.has(current.state)) {
        return commandResult(current, { snapshot: current.result?.snapshot });
      }
      await delay(50);
    }
    const accepted = this.store.markRemoteAccepted(
      this.config.mcpDeviceId,
      command.command_id,
    );
    return commandResult(accepted || command, { state: "accepted" });
  }

  async accountForProvider() {
    if (this.config.testMode) return null;
    const account = await this.ensureAccount();
    if (!account) throw new Error("music_auth_required");
    return account;
  }
}

export { MODES };
