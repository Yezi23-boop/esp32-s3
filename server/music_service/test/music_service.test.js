import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { createServer } from "node:http";
import { PassThrough } from "node:stream";
import test from "node:test";
import { createApp } from "../src/app.js";
import { NeteaseProvider } from "../src/netease_provider.js";
import { OggOpusPacketTransform } from "../src/ogg_opus_packet_transform.js";
import { MockWatchClient } from "./mock_watch_client.js";

const here = path.dirname(fileURLToPath(import.meta.url));
const fixture = path.resolve(
  here,
  "../../../managed_components/chmorgan__esp-audio-player/test/gs-16b-1c-44100hz.mp3",
);

async function runningApp(overrides = {}) {
  const dataDir = fs.mkdtempSync(path.join(os.tmpdir(), "watch-music-"));
  const app = createApp({
    dataDir,
    dbPath: path.join(dataDir, "music.db"),
    testMode: true,
    testStreamPath: fixture,
    deviceTokens: new Map([["watch-001", "test-token"]]),
    ...overrides,
  });
  const server = createServer(app);
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const address = server.address();
  const base = `http://127.0.0.1:${address.port}`;
  return {
    app,
    base,
    close: async () => {
      app.closeMusic();
      await new Promise((resolve) => server.close(resolve));
      fs.rmSync(dataDir, { recursive: true, force: true });
    },
  };
}

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

function headers(commandId) {
  return {
    Authorization: "Bearer test-token",
    "Content-Type": "application/json",
    ...(commandId ? { "X-Command-Id": commandId } : {}),
  };
}

test("music endpoints independently authenticate device token", async () => {
  const running = await runningApp();
  try {
    const missing = await fetch(`${running.base}/v1/music/sources?device_id=watch-001`);
    assert.equal(missing.status, 401);
    const wrong = await fetch(`${running.base}/v1/music/sources?device_id=watch-001`, {
      headers: { Authorization: "Bearer wrong" },
    });
    assert.equal(wrong.status, 403);
    const ok = await fetch(`${running.base}/v1/music/sources?device_id=watch-001`, {
      headers: { Authorization: "Bearer test-token" },
    });
    assert.equal(ok.status, 200);
    assert.equal((await ok.json()).sources[0].source_id, "test");
  } finally {
    await running.close();
  }
});

test("session commands are idempotent and pause/resume preserves session", async () => {
  const running = await runningApp();
  try {
    const create = await fetch(`${running.base}/v1/music/sessions`, {
      method: "POST",
      headers: headers("create-1"),
      body: JSON.stringify({ device_id: "watch-001", source_id: "test", track_id: "test-track" }),
    });
    assert.equal(create.status, 200);
    const first = await create.json();
    const duplicate = await fetch(`${running.base}/v1/music/sessions`, {
      method: "POST",
      headers: headers("create-1"),
      body: JSON.stringify({ device_id: "watch-001", source_id: "test", track_id: "test-track" }),
    });
    assert.deepEqual(await duplicate.json(), first);

    const pause = await fetch(
      `${running.base}/v1/music/sessions/${first.music_session_id}/pause?device_id=watch-001`,
      { method: "POST", headers: headers("pause-1"), body: "{}" },
    );
    assert.equal((await pause.json()).state, "paused");
    const resume = await fetch(
      `${running.base}/v1/music/sessions/${first.music_session_id}/resume?device_id=watch-001`,
      { method: "POST", headers: headers("resume-1"), body: "{}" },
    );
    const resumed = await resume.json();
    assert.equal(resumed.state, "buffering");
    assert.notEqual(resumed.stream_id, first.stream_id);
  } finally {
    await running.close();
  }
});

test("short-lived stream capability works without exposing the device token", async () => {
  const running = await runningApp();
  try {
    const create = await fetch(`${running.base}/v1/music/sessions?device_id=watch-001`, {
      method: "POST",
      headers: headers("create-stream"),
      body: JSON.stringify({ source_id: "test", track_id: "test-track" }),
    });
    const session = await create.json();
    const invalid = await fetch(
      `${running.base}/v1/music/streams/stream-invalid?device_id=watch-001`,
    );
    assert.equal(invalid.status, 409);
    const stream = await fetch(
      `${running.base}/v1/music/streams/${session.stream_id}?device_id=watch-001`,
    );
    assert.equal(stream.status, 200);
    assert.equal(stream.headers.get("content-type"), "application/x-watch-opus");
    const reader = stream.body.getReader();
    const first = await reader.read();
    assert.ok(first.value.byteLength > 0);
    await reader.cancel();
  } finally {
    await running.close();
  }
});

function oggPage(segments, body, headerType = 0) {
  const header = Buffer.alloc(27 + segments.length);
  header.write("OggS");
  header[4] = 0;
  header[5] = headerType;
  header[26] = segments.length;
  Buffer.from(segments).copy(header, 27);
  return Buffer.concat([header, body]);
}

test("Ogg Opus transform skips headers and frames packets across chunks/pages", async () => {
  const transform = new OggOpusPacketTransform();
  const output = [];
  transform.on("data", (chunk) => output.push(chunk));
  const head = Buffer.from("OpusHead\x01\x01");
  const tags = Buffer.from("OpusTags\x00");
  const audio = Buffer.alloc(258, 0x5a);
  const first = Buffer.concat([
    oggPage([head.length, tags.length, 255], Buffer.concat([head, tags, audio.subarray(0, 255)])),
    oggPage([3], audio.subarray(255), 0x01),
  ]);
  transform.write(first.subarray(0, 31));
  transform.end(first.subarray(31));
  await new Promise((resolve, reject) => transform.once("end", resolve).once("error", reject));
  const framed = Buffer.concat(output);
  assert.equal(framed.readUInt16BE(0), audio.length);
  assert.deepEqual(framed.subarray(2), audio);
  assert.equal(output.length, 1);
});

test("pausing an attached stream closes the HTTP response", async () => {
  const running = await runningApp();
  try {
    const create = await fetch(`${running.base}/v1/music/sessions?device_id=watch-001`, {
      method: "POST",
      headers: headers("pause-stream-create"),
      body: JSON.stringify({ source_id: "test", track_id: "test-track" }),
    });
    const session = await create.json();
    const stream = await fetch(
      `${running.base}/v1/music/streams/${session.stream_id}?device_id=watch-001`,
    );
    const reader = stream.body.getReader();
    const first = await reader.read();
    assert.ok(first.value.byteLength > 0);

    const pause = await fetch(
      `${running.base}/v1/music/sessions/${session.music_session_id}/pause?device_id=watch-001`,
      { method: "POST", headers: headers("pause-stream-command"), body: "{}" },
    );
    assert.equal((await pause.json()).state, "paused");

    const stopped = await Promise.race([
      reader.read().then(() => ({ closed: true })).catch(() => ({ closed: true })),
      delay(1000).then(() => ({ timeout: true })),
    ]);
    assert.notEqual(stopped.timeout, true, "paused stream did not close");
    assert.equal(stopped.closed, true);
  } finally {
    await running.close();
  }
});

test("disconnected stream can reattach during grace and is then reclaimed", async () => {
  const running = await runningApp({ streamDisconnectCleanupMs: 80 });
  try {
    const session = running.app.engine.createTestSession("watch-001", "direct-1");
    const firstResponse = new PassThrough();
    assert.equal(
      running.app.engine.attachStream(
        "watch-001",
        session.stream_id,
        firstResponse,
      ).state,
      "streaming",
    );
    await new Promise((resolve) => firstResponse.once("data", resolve));
    firstResponse.destroy();
    await delay(10);

    const secondResponse = new PassThrough();
    assert.equal(
      running.app.engine.attachStream(
        "watch-001",
        session.stream_id,
        secondResponse,
      ).state,
      "streaming",
    );
    secondResponse.destroy();
    await delay(130);
    assert.equal(running.app.engine.child, null);
    assert.equal(running.app.engine.getSession("watch-001").state, "paused");
  } finally {
    await running.close();
  }
});

test("private Netease adapter exposes QR, source summaries, and no upstream URL", async () => {
  let loggedIn = false;
  const provider = {
    hasCookie: () => loggedIn,
    createQr: async () => ({ key: "private-key", expires_at: Date.now() + 120_000, modules: { size: 21, data: "AA==" } }),
    checkQr: async () => {
      loggedIn = true;
      return { state: "logged_in", account: { uid: "42", nickname: "测试用户" } };
    },
    clearCookie: () => { loggedIn = false; },
    sources: async () => ({
      sources: [{ source_id: "today", title: "今日推荐", kind: "tracks", track_count: 1 }],
      playlists: [],
    }),
    tracks: async () => ({ tracks: [{ track_id: "song-1", title: "测试歌曲", artist: "测试歌手" }], total: 1 }),
    playback: async (_sourceId, _trackId) => ({
      sourceId: "today",
      track: { track_id: "song-1", title: "测试歌曲", artist: "测试歌手" },
      queue: [{ track_id: "song-1", title: "测试歌曲", artist: "测试歌手" }],
      input: "https://private-upstream.invalid/audio",
    }),
  };
  const running = await runningApp({ testMode: false, provider });
  try {
    const qr = await fetch(`${running.base}/v1/music/account/qr?device_id=watch-001`, {
      method: "POST", headers: headers("qr-1"), body: "{}",
    });
    const qrPayload = await qr.json();
    assert.equal(qrPayload.state, "qr_pending");
    assert.equal(qrPayload.qr.size, 21);

    const login = await fetch(`${running.base}/v1/music/account/qr/${qrPayload.login_id}?device_id=watch-001`, {
      headers: { Authorization: "Bearer test-token" },
    });
    assert.equal((await login.json()).state, "logged_in");

    const source = await fetch(`${running.base}/v1/music/sources/today/tracks?device_id=watch-001`, {
      headers: { Authorization: "Bearer test-token" },
    });
    assert.equal((await source.json()).tracks[0].track_id, "song-1");

    const session = await fetch(`${running.base}/v1/music/sessions`, {
      method: "POST", headers: headers("real-session-1"),
      body: JSON.stringify({ device_id: "watch-001", source_id: "today", track_id: "song-1" }),
    });
    const payload = await session.json();
    assert.equal(payload.track.title, "测试歌曲");
    assert.equal(JSON.stringify(payload).includes("private-upstream"), false);

    const missingConfirm = await fetch(`${running.base}/v1/music/account?device_id=watch-001`, {
      method: "DELETE", headers: headers("logout-1"), body: "{}",
    });
    assert.equal(missingConfirm.status, 400);
    const logout = await fetch(`${running.base}/v1/music/account?device_id=watch-001`, {
      method: "DELETE", headers: headers("logout-2"), body: JSON.stringify({ confirm: true }),
    });
    assert.equal((await logout.json()).state, "logged_out");
  } finally {
    await running.close();
  }
});

test("source-only session lets the server choose the first track", async () => {
  let requestedTrackId = "unset";
  const provider = {
    hasCookie: () => true,
    sources: async () => ({ sources: [], playlists: [] }),
    playback: async (_sourceId, trackId) => {
      requestedTrackId = trackId;
      return {
        sourceId: "today",
        track: { track_id: "song-1", title: "首曲", artist: "歌手" },
        queue: [{ track_id: "song-1", title: "首曲", artist: "歌手" }],
        input: fixture,
      };
    },
  };
  const running = await runningApp({ testMode: false, provider });
  try {
    running.app.store.saveAccount({ uid: "42", nickname: "测试用户" });
    const response = await fetch(`${running.base}/v1/music/sessions`, {
      method: "POST", headers: headers("source-only-1"),
      body: JSON.stringify({ device_id: "watch-001", source_id: "today" }),
    });
    assert.equal(response.status, 200);
    assert.equal(requestedTrackId, null);
    assert.equal((await response.json()).track.track_id, "song-1");
  } finally {
    await running.close();
  }
});

test("next uses the persisted queue and resolves only the selected upstream track", async () => {
  const resolved = [];
  const provider = {
    hasCookie: () => true,
    playback: async () => ({
      sourceId: "today",
      track: { track_id: "song-1", title: "第一首", artist: "歌手" },
      queue: [
        { track_id: "song-1", title: "第一首", artist: "歌手" },
        { track_id: "song-2", title: "第二首", artist: "歌手" },
      ],
      input: fixture,
    }),
    resolveTrack: async (trackId) => {
      resolved.push(trackId);
      return fixture;
    },
  };
  const running = await runningApp({ testMode: false, provider });
  try {
    running.app.store.saveAccount({ uid: "42", nickname: "测试用户" });
    const create = await fetch(`${running.base}/v1/music/sessions`, {
      method: "POST", headers: headers("queue-create"),
      body: JSON.stringify({ device_id: "watch-001", source_id: "today" }),
    });
    const first = await create.json();
    const next = await fetch(`${running.base}/v1/music/sessions/${first.music_session_id}/next?device_id=watch-001`, {
      method: "POST", headers: headers("queue-next"), body: "{}",
    });
    const second = await next.json();
    assert.equal(second.track.track_id, "song-2");
    assert.deepEqual(resolved, ["song-2"]);
  } finally {
    await running.close();
  }
});

test("resume re-resolves the current track instead of reusing an expired URL", async () => {
  const resolved = [];
  const provider = {
    hasCookie: () => true,
    playback: async () => ({
      sourceId: "today",
      track: { track_id: "song-1", title: "第一首", artist: "歌手" },
      queue: [{ track_id: "song-1", title: "第一首", artist: "歌手" }],
      input: "old-url",
    }),
    resolveTrack: async (trackId) => {
      resolved.push(trackId);
      return "fresh-url";
    },
  };
  const running = await runningApp({ testMode: false, provider });
  try {
    running.app.store.saveAccount({ uid: "42", nickname: "测试用户" });
    const create = await fetch(`${running.base}/v1/music/sessions`, {
      method: "POST", headers: headers("resume-create"),
      body: JSON.stringify({ device_id: "watch-001", source_id: "today" }),
    });
    const first = await create.json();
    await fetch(`${running.base}/v1/music/sessions/${first.music_session_id}/pause?device_id=watch-001`, {
      method: "POST", headers: headers("resume-pause"), body: "{}",
    });
    const resume = await fetch(`${running.base}/v1/music/sessions/${first.music_session_id}/resume?device_id=watch-001`, {
      method: "POST", headers: headers("resume-resume"), body: "{}",
    });
    assert.equal((await resume.json()).state, "buffering");
    assert.deepEqual(resolved, ["song-1"]);
  } finally {
    await running.close();
  }
});

test("a persisted cookie can rehydrate the account summary after SQLite metadata loss", async () => {
  let accountCalls = 0;
  const provider = {
    hasCookie: () => true,
    account: async () => {
      accountCalls += 1;
      return { uid: "42", nickname: "重启用户" };
    },
  };
  const running = await runningApp({ testMode: false, provider });
  try {
    const response = await fetch(`${running.base}/v1/music/account?device_id=watch-001`, {
      headers: { Authorization: "Bearer test-token" },
    });
    const payload = await response.json();
    assert.equal(payload.state, "logged_in");
    assert.equal(payload.account.nickname, "重启用户");
    assert.equal(accountCalls, 1);
    assert.equal(running.app.store.getAccount().uid, "42");
  } finally {
    await running.close();
  }
});

test("MCP requires its own token and exposes the fixed tool set", async () => {
  const running = await runningApp({ mcpToken: "mcp-test" });
  try {
    const missing = await fetch(`${running.base}/v1/music/mcp`, {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ jsonrpc: "2.0", id: 1, method: "initialize", params: {} }),
    });
    assert.equal(missing.status, 401);
    const wrong = await fetch(`${running.base}/v1/music/mcp`, {
      method: "POST",
      headers: { Authorization: "Bearer wrong", "Content-Type": "application/json" },
      body: JSON.stringify({ jsonrpc: "2.0", id: 1, method: "initialize", params: {} }),
    });
    assert.equal(wrong.status, 403);

    const initialized = await fetch(`${running.base}/v1/music/mcp`, {
      method: "POST",
      headers: { Authorization: "Bearer mcp-test", "Content-Type": "application/json" },
      body: JSON.stringify({ jsonrpc: "2.0", id: 1, method: "initialize", params: {} }),
    });
    assert.equal(initialized.status, 200);
    assert.ok(initialized.headers.get("mcp-session-id"));
    const listed = await fetch(`${running.base}/v1/music/mcp`, {
      method: "POST",
      headers: { Authorization: "Bearer mcp-test", "Content-Type": "application/json" },
      body: JSON.stringify({ jsonrpc: "2.0", id: 2, method: "tools/list", params: {} }),
    });
    const tools = (await listed.json()).result.tools;
    assert.deepEqual(tools.map((tool) => tool.name), [
      "music_search", "music_status", "music_play", "music_pause", "music_resume",
      "music_previous", "music_next", "music_stop", "music_set_mode", "music_set_volume",
    ]);
  } finally {
    await running.close();
  }
});

test("MCP write reaches a mock watch and returns executed without leaking its input URL", async () => {
  const provider = {
    hasCookie: () => true,
    search: async () => [{ track_id: "song-1", title: "晴天", artist: "周杰伦" }],
    playback: async (_sourceId, trackId, _account, options) => ({
      sourceId: "search",
      track: { track_id: trackId || "song-1", title: "晴天", artist: "周杰伦" },
      queue: [{ track_id: trackId || "song-1", title: "晴天", artist: "周杰伦" }],
      mode: options.mode,
      input: "https://private-upstream.invalid/should-not-leak",
    }),
  };
  const running = await runningApp({
    testMode: false,
    provider,
    mcpToken: "mcp-test",
    mcpAckWaitMs: 1000,
  });
  running.app.store.saveAccount({ uid: "42", nickname: "测试用户" });
  try {
    const mockWatch = new MockWatchClient(running.base);
    const request = fetch(`${running.base}/v1/music/mcp`, {
      method: "POST",
      headers: { Authorization: "Bearer mcp-test", "Content-Type": "application/json" },
      body: JSON.stringify({
        jsonrpc: "2.0", id: 3, method: "tools/call",
        params: { name: "music_play", arguments: { query: "周杰伦 晴天" } },
      }),
    });
    let command;
    for (let attempt = 0; attempt < 20 && !command; attempt += 1) {
      const poll = await fetch(`${running.base}/v1/music/remote-commands/next?device_id=watch-001`, {
        headers: { Authorization: "Bearer test-token" },
      });
      const value = await poll.json();
      if (value.state === "claimed") command = value;
      else await delay(20);
    }
    assert.ok(command);
    assert.equal(command.action, "play");
    assert.equal(command.payload.track.title, "晴天");
    assert.equal(JSON.stringify(command).includes("private-upstream"), false);
    const ack = await mockWatch.ack(
      command.command_id,
      { state: "executed" },
      { state: "playing", track_id: "song-1", mode: "repeat_all" },
    );
    assert.equal(ack.state, "executed");
    const response = await request;
    assert.equal(response.status, 200);
    const payload = await response.json();
    const result = JSON.parse(payload.result.content[0].text);
    assert.equal(result.state, "executed");
    assert.equal(result.action, "play");
    assert.equal(JSON.stringify(payload).includes("private-upstream"), false);
  } finally {
    await running.close();
  }
});

test("remote command pending state is latest-wins and expired commands are not claimable", () => {
  const dataDir = fs.mkdtempSync(path.join(os.tmpdir(), "watch-music-store-"));
  const app = createApp({
    dataDir,
    dbPath: path.join(dataDir, "music.db"),
    testMode: true,
    remoteCommandTtlMs: 10,
  });
  try {
    const first = app.store.createRemoteCommand("watch-001", "pause", {}, 1000);
    const second = app.store.createRemoteCommand("watch-001", "stop", {}, 10);
    assert.equal(app.store.getRemoteCommand("watch-001", first.command_id).state, "superseded");
    assert.equal(app.store.claimRemoteCommand("watch-001").command_id, second.command_id);
    const third = app.store.createRemoteCommand("watch-001", "next", {}, 1);
    assert.equal(third.state, "pending");
    const originalNow = Date.now;
    Date.now = () => originalNow() + 2000;
    try {
      assert.equal(app.store.claimRemoteCommand("watch-001"), null);
      assert.equal(app.store.getRemoteCommand("watch-001", third.command_id).state, "expired");
    } finally {
      Date.now = originalNow;
    }
  } finally {
    app.closeMusic();
    fs.rmSync(dataDir, { recursive: true, force: true });
  }
});

test("Netease adapter searches five tracks and asks the smart endpoint for a dynamic queue", async () => {
  const dataDir = fs.mkdtempSync(path.join(os.tmpdir(), "watch-music-provider-"));
  const cookiePath = path.join(dataDir, "cookie.txt");
  fs.writeFileSync(cookiePath, "MUSIC_COOKIE");
  const calls = [];
  const provider = new NeteaseProvider({ neteaseCookiePath: cookiePath }, {
    search: async (params) => {
      calls.push(["search", params]);
      return { body: { result: { songs: [{ id: 1, name: "晴天", ar: [{ name: "周杰伦" }] }] } } };
    },
    user_playlist: async () => ({ body: { playlist: [{ id: 99, specialType: 5, name: "我喜欢的音乐" }] } }),
    song_detail: async () => ({ body: { songs: [{ id: 1, name: "晴天", ar: [{ name: "周杰伦" }] }] } }),
    playmode_intelligence_list: async (params) => {
      calls.push(["smart", params]);
      return { body: { data: [{ id: 1, name: "晴天", ar: [{ name: "周杰伦" }] }] } };
    },
    song_url: async () => ({ body: { data: [{ url: "https://private.invalid/song" }] } }),
  });
  try {
    const account = { uid: "42", nickname: "测试用户" };
    assert.equal((await provider.search("晴天", account, 5))[0].track_id, "1");
    const definition = await provider.playback("liked", "1", account, { mode: "smart" });
    assert.equal(definition.mode, "smart");
    assert.equal(definition.sourceId, "playlist:99");
    assert.equal(calls[0][0], "search");
    assert.equal(calls[1][0], "smart");
    assert.equal(calls[1][1].pid, "99");
  } finally {
    fs.rmSync(dataDir, { recursive: true, force: true });
  }
});
