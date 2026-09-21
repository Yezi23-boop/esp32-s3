import fs from "node:fs";
import path from "node:path";
import api from "@neteasecloudmusicapienhanced/api";
import QRCode from "qrcode";

const QR_TTL_MS = 120_000;
const SOURCE_CACHE_TTL_MS = 5 * 60_000;

function bodyOf(result) {
  return result?.body || {};
}

function artistOf(song) {
  const artists = song?.ar || song?.artists || [];
  return artists.map((artist) => artist?.name).filter(Boolean).join(" / ") || "未知歌手";
}

function trackOf(song) {
  const id = song?.id ?? song?.song?.id;
  const value = song?.song || song;
  return {
    track_id: String(id),
    title: value?.name || "未知歌曲",
    artist: artistOf(value),
  };
}

function bitpackQr(modules) {
  const size = modules.size;
  const bytes = Buffer.alloc(Math.ceil((size * size) / 8));
  let bit = 0;
  for (let y = 0; y < size; y += 1) {
    for (let x = 0; x < size; x += 1, bit += 1) {
      if (modules.get(x, y)) bytes[Math.floor(bit / 8)] |= 1 << (7 - (bit % 8));
    }
  }
  return { size, data: bytes.toString("base64") };
}

/**
 * 网易云只在本进程内调用。此模块不启动 api-enhanced 的通用 HTTP server，
 * Cookie 仅落在 /data 私有文件，不能被 watch endpoint 或 ESP32 读取。
 */
export class NeteaseProvider {
  constructor(config, client = api) {
    this.config = config;
    this.client = client;
    this.cookiePath = config.neteaseCookiePath;
    this.cache = new Map();
  }

  hasCookie() {
    return fs.existsSync(this.cookiePath) && fs.statSync(this.cookiePath).size > 0;
  }

  readCookie() {
    return this.hasCookie() ? fs.readFileSync(this.cookiePath, "utf8").trim() : "";
  }

  saveCookie(cookie) {
    fs.mkdirSync(path.dirname(this.cookiePath), { recursive: true, mode: 0o700 });
    fs.writeFileSync(this.cookiePath, cookie, { mode: 0o600 });
    fs.chmodSync(this.cookiePath, 0o600);
  }

  clearCookie() {
    fs.rmSync(this.cookiePath, { force: true });
  }

  async createQr() {
    const key = bodyOf(await this.client.login_qr_key({ timestamp: Date.now() }))?.data?.unikey;
    if (!key) throw new Error("qr_create_failed");
    const created = bodyOf(await this.client.login_qr_create({ key, timestamp: Date.now() }));
    const qrurl = created?.data?.qrurl;
    if (!qrurl) throw new Error("qr_create_failed");
    const qr = QRCode.create(qrurl, { errorCorrectionLevel: "M" });
    return { key, expires_at: Date.now() + QR_TTL_MS, modules: bitpackQr(qr.modules) };
  }

  async checkQr(key) {
    const result = bodyOf(await this.client.login_qr_check({ key, timestamp: Date.now() }));
    const code = Number(result.code);
    if (code === 803 && result.cookie) {
      this.saveCookie(result.cookie);
      const account = await this.account();
      return { state: "logged_in", account };
    }
    if (code === 802) return { state: "qr_confirming" };
    if (code === 801) return { state: "qr_pending" };
    return { state: "expired" };
  }

  async account() {
    const cookie = this.readCookie();
    if (!cookie) return null;
    const body = bodyOf(await this.client.user_account({ cookie, timestamp: Date.now() }));
    const profile = body.profile || body.account || {};
    const uid = profile.userId ?? profile.id;
    if (!uid) throw new Error("account_expired");
    return { uid: String(uid), nickname: profile.nickname || "网易云用户" };
  }

  async search(query, account, limit = 5) {
    const cookie = this.readCookie();
    if (!cookie || !account?.uid) throw new Error("music_auth_required");
    const boundedLimit = Math.min(5, Math.max(1, Number(limit) || 5));
    const result = bodyOf(await this.client.search({
      keywords: query,
      type: 1,
      limit: boundedLimit,
      offset: 0,
      cookie,
      timestamp: Date.now(),
    }));
    const songs = result?.result?.songs || result?.songs || [];
    return songs
      .map(trackOf)
      .filter((track) => track.track_id !== "undefined")
      .slice(0, boundedLimit);
  }

  async sources(account) {
    const cookie = this.readCookie();
    if (!cookie || !account?.uid) throw new Error("music_auth_required");
    const cached = this.cache.get(`sources:${account.uid}`);
    if (cached && cached.expiresAt > Date.now()) return cached.value;
    const playlists = bodyOf(await this.client.user_playlist({
      uid: account.uid, cookie, limit: 100, timestamp: Date.now(),
    }))?.playlist || [];
    const value = {
      sources: [
        { source_id: "today", title: "今日推荐", kind: "tracks", track_count: 0 },
        { source_id: "liked", title: "我喜欢", kind: "tracks", track_count: 0 },
        { source_id: "playlists", title: "我的歌单", kind: "playlists", track_count: playlists.length },
        { source_id: "recent", title: "最近播放", kind: "tracks", track_count: 0 },
      ],
      playlists: playlists.map((playlist) => ({
        source_id: `playlist:${playlist.id}`, title: playlist.name || "未命名歌单",
        kind: "tracks", track_count: Number(playlist.trackCount || 0),
      })),
    };
    this.cache.set(`sources:${account.uid}`, { expiresAt: Date.now() + SOURCE_CACHE_TTL_MS, value });
    return value;
  }

  async tracks(sourceId, account, offset = 0, limit = 10) {
    const cookie = this.readCookie();
    if (!cookie || !account?.uid) throw new Error("music_auth_required");
    const cacheKey = `tracks:${account.uid}:${sourceId}:${offset}:${limit}`;
    const cached = this.cache.get(cacheKey);
    if (cached && cached.expiresAt > Date.now()) return cached.value;
    let songs = [];
    if (sourceId === "today") {
      songs = bodyOf(await this.client.recommend_songs({ cookie, timestamp: Date.now() }))?.data?.dailySongs || [];
    } else if (sourceId === "liked") {
      const ids = bodyOf(await this.client.likelist({ uid: account.uid, cookie, timestamp: Date.now() }))?.ids || [];
      if (ids.length) songs = bodyOf(await this.client.song_detail({ ids: ids.slice(offset, offset + limit).join(","), cookie, timestamp: Date.now() }))?.songs || [];
      const value = { tracks: songs.map(trackOf), total: ids.length };
      this.cache.set(cacheKey, { expiresAt: Date.now() + SOURCE_CACHE_TTL_MS, value });
      return value;
    } else if (sourceId === "recent") {
      const items = bodyOf(await this.client.record_recent_song({ cookie, limit: 100, timestamp: Date.now() }))?.data?.list || [];
      songs = items.map((item) => item.data || item.song || item);
    } else if (sourceId === "playlists") {
      const available = await this.sources(account);
      const firstPlaylist = available.playlists?.[0];
      if (!firstPlaylist) throw new Error("music_source_empty");
      return this.tracks(firstPlaylist.source_id, account, offset, limit);
    } else if (sourceId.startsWith("playlist:")) {
      const playlistId = sourceId.slice("playlist:".length);
      songs = bodyOf(await this.client.playlist_track_all({ id: playlistId, cookie, offset, limit, timestamp: Date.now() }))?.songs || [];
      const value = { tracks: songs.map(trackOf), total: songs.length + offset };
      this.cache.set(cacheKey, { expiresAt: Date.now() + SOURCE_CACHE_TTL_MS, value });
      return value;
    } else {
      throw new Error("music_source_not_found");
    }
    const page = songs.slice(offset, offset + limit).map(trackOf);
    const value = { tracks: page, total: songs.length };
    this.cache.set(cacheKey, { expiresAt: Date.now() + SOURCE_CACHE_TTL_MS, value });
    return value;
  }

  async resolveTrack(trackId) {
    const cookie = this.readCookie();
    if (!cookie) throw new Error("music_auth_required");
    const body = bodyOf(await this.client.song_url({ id: trackId, br: 128000, cookie, timestamp: Date.now() }));
    const url = body?.data?.[0]?.url;
    if (!url) throw new Error("track_unplayable");
    return url;
  }

  async playback(sourceId, trackId, account, options = {}) {
    const selectedTrack = options.selectedTrack || null;
    if (sourceId === "playlists") {
      const available = await this.sources(account);
      const firstPlaylist = available.playlists?.[0];
      if (!firstPlaylist) throw new Error("music_source_empty");
      sourceId = firstPlaylist.source_id;
    }
    let page = { tracks: [], total: 0 };
    // Smart mode already has an explicit seed. Avoid loading an entire source
    // page before calling the provider's dynamic queue endpoint.
    if (sourceId !== "search" && !(options.mode === "smart" && trackId)) {
      page = await this.tracks(sourceId, account, 0, 100);
    }
    let track = selectedTrack || (trackId
      ? page.tracks.find((item) => item.track_id === String(trackId))
      : page.tracks[0]);
    if (!track && trackId) {
      const cookie = this.readCookie();
      const songs = bodyOf(await this.client.song_detail({
        ids: String(trackId), cookie, timestamp: Date.now(),
      }))?.songs || [];
      track = songs.length ? trackOf(songs[0]) : null;
    }
    if (!track || track.track_id === "undefined") throw new Error("track_unplayable");
    if (options.mode === "smart") {
      const smart = await this.smartQueue(sourceId, track.track_id, account);
      sourceId = smart.sourceId;
      page = { tracks: smart.queue, total: smart.queue.length };
      track = page.tracks.find((item) => item.track_id === String(trackId)) || page.tracks[0];
    }
    return {
      sourceId,
      track,
      queue: page.tracks,
      mode: options.mode || "repeat_all",
      input: await this.resolveTrack(track.track_id),
    };
  }

  async smartQueue(sourceId, seedTrackId, account) {
    const cookie = this.readCookie();
    if (!cookie || !account?.uid) throw new Error("music_auth_required");
    const playlistId = await this.#playlistIdForSmart(sourceId, account);
    if (!playlistId || !seedTrackId) throw new Error("smart_mode_requires_playlist");
    const result = bodyOf(await this.client.playmode_intelligence_list({
      id: String(seedTrackId),
      pid: String(playlistId),
      sid: String(seedTrackId),
      count: 100,
      cookie,
      timestamp: Date.now(),
    }));
    const data = result?.data || result?.result?.songs || result?.songs || [];
    const songs = Array.isArray(data) ? data : data?.songs || data?.list || [];
    const queue = songs
      .map((item) => trackOf(item?.song || item))
      .filter((item) => item.track_id !== "undefined");
    if (!queue.length) throw new Error("smart_mode_unavailable");
    return { sourceId: `playlist:${playlistId}`, queue };
  }

  async #playlistIdForSmart(sourceId, account) {
    if (typeof sourceId === "string" && sourceId.startsWith("playlist:")) {
      const playlistId = sourceId.slice("playlist:".length).trim();
      return playlistId || null;
    }
    if (sourceId !== "liked") return null;
    const cookie = this.readCookie();
    const playlists = bodyOf(await this.client.user_playlist({
      uid: account.uid, cookie, limit: 100, timestamp: Date.now(),
    }))?.playlist || [];
    const liked = playlists.find((playlist) => Number(playlist.specialType) === 5)
      || playlists.find((playlist) => String(playlist.name || "").includes("我喜欢"));
    return liked?.id ? String(liked.id) : null;
  }
}
