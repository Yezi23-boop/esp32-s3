import path from "node:path";

function boolEnv(value, fallback = false) {
  if (value == null || value === "") return fallback;
  return ["1", "true", "yes", "on"].includes(value.toLowerCase());
}

function parseDeviceTokens(value) {
  const tokens = new Map();
  for (const item of (value || "").split(",")) {
    const separator = item.indexOf("=");
    if (separator <= 0) continue;
    const deviceId = item.slice(0, separator).trim();
    const token = item.slice(separator + 1).trim();
    if (deviceId && token) tokens.set(deviceId, token);
  }
  return tokens;
}

export function loadConfig(overrides = {}) {
  const dataDir = overrides.dataDir || process.env.MUSIC_DATA_DIR || "/data";
  return {
    host: overrides.host || process.env.MUSIC_HOST || "0.0.0.0",
    port: Number(overrides.port || process.env.MUSIC_PORT || 8788),
    dataDir,
    dbPath: overrides.dbPath || process.env.MUSIC_DB_PATH || path.join(dataDir, "music.db"),
    testMode: overrides.testMode ?? boolEnv(process.env.MUSIC_TEST_MODE),
    testStreamPath: overrides.testStreamPath || process.env.MUSIC_TEST_STREAM_PATH || "",
    neteaseCookiePath: overrides.neteaseCookiePath || process.env.NETEASE_COOKIE_PATH || path.join(dataDir, "netease-cookie.txt"),
    ffmpegPath: overrides.ffmpegPath || process.env.FFMPEG_PATH || "ffmpeg",
    streamAttachTimeoutMs: Number(
      overrides.streamAttachTimeoutMs || process.env.MUSIC_STREAM_ATTACH_TIMEOUT_MS || 60000,
    ),
    streamDisconnectCleanupMs: Number(
      overrides.streamDisconnectCleanupMs ||
        process.env.MUSIC_STREAM_DISCONNECT_CLEANUP_MS || 15000,
    ),
    mcpToken: overrides.mcpToken || process.env.MUSIC_MCP_TOKEN || "",
    mcpDeviceId: overrides.mcpDeviceId || process.env.MUSIC_MCP_DEVICE_ID || "watch-001",
    remoteCommandTtlMs: Number(
      overrides.remoteCommandTtlMs || process.env.MUSIC_REMOTE_COMMAND_TTL_MS || 30000,
    ),
    mcpAckWaitMs: Number(
      overrides.mcpAckWaitMs || process.env.MUSIC_MCP_ACK_WAIT_MS || 5000,
    ),
    deviceTokens: overrides.deviceTokens || parseDeviceTokens(process.env.WATCH_DEVICE_TOKENS),
  };
}
