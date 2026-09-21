import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import { DatabaseSync } from "node:sqlite";

export class MusicStore {
  constructor(dbPath) {
    fs.mkdirSync(path.dirname(dbPath), { recursive: true });
    this.db = new DatabaseSync(dbPath);
    this.db.exec(`
      PRAGMA journal_mode = WAL;
      CREATE TABLE IF NOT EXISTS music_sessions (
        device_id TEXT PRIMARY KEY,
        music_session_id TEXT NOT NULL,
        state TEXT NOT NULL,
        source_id TEXT NOT NULL,
        track_id TEXT NOT NULL,
        title TEXT NOT NULL,
        artist TEXT NOT NULL,
        source_path TEXT NOT NULL,
        queue_json TEXT NOT NULL,
        mode TEXT NOT NULL,
        position_ms INTEGER NOT NULL DEFAULT 0,
        stream_id TEXT,
        stream_issued_at INTEGER,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL
      );
      CREATE TABLE IF NOT EXISTS music_commands (
        device_id TEXT NOT NULL,
        command_id TEXT NOT NULL,
        result_json TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        PRIMARY KEY (device_id, command_id)
      );
      CREATE TABLE IF NOT EXISTS music_account (
        id INTEGER PRIMARY KEY CHECK (id = 1),
        uid TEXT NOT NULL,
        nickname TEXT NOT NULL,
        updated_at INTEGER NOT NULL
      );
      CREATE TABLE IF NOT EXISTS music_qr_sessions (
        login_id TEXT PRIMARY KEY,
        qr_key TEXT NOT NULL,
        expires_at INTEGER NOT NULL,
        created_at INTEGER NOT NULL
      );
      CREATE TABLE IF NOT EXISTS music_remote_commands (
        device_id TEXT NOT NULL,
        command_id TEXT NOT NULL,
        action TEXT NOT NULL,
        payload_json TEXT NOT NULL,
        state TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        expires_at INTEGER NOT NULL,
        claimed_at INTEGER,
        acked_at INTEGER,
        result_json TEXT,
        PRIMARY KEY (device_id, command_id)
      );
      CREATE INDEX IF NOT EXISTS music_remote_commands_pending
        ON music_remote_commands(device_id, state, created_at);
      CREATE TABLE IF NOT EXISTS music_device_snapshots (
        device_id TEXT PRIMARY KEY,
        command_id TEXT,
        snapshot_json TEXT NOT NULL,
        updated_at INTEGER NOT NULL
      );
    `);
  }

  getSession(deviceId) {
    const row = this.db
      .prepare("SELECT * FROM music_sessions WHERE device_id = ?")
      .get(deviceId);
    return row ? this.#decodeSession(row) : null;
  }

  saveSession(session) {
    this.db
      .prepare(`
        INSERT INTO music_sessions (
          device_id, music_session_id, state, source_id, track_id, title, artist,
          source_path, queue_json, mode, position_ms, stream_id, stream_issued_at,
          created_at, updated_at
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(device_id) DO UPDATE SET
          music_session_id = excluded.music_session_id,
          state = excluded.state,
          source_id = excluded.source_id,
          track_id = excluded.track_id,
          title = excluded.title,
          artist = excluded.artist,
          source_path = excluded.source_path,
          queue_json = excluded.queue_json,
          mode = excluded.mode,
          position_ms = excluded.position_ms,
          stream_id = excluded.stream_id,
          stream_issued_at = excluded.stream_issued_at,
          updated_at = excluded.updated_at
      `)
      .run(
        session.device_id,
        session.music_session_id,
        session.state,
        session.source_id,
        session.track_id,
        session.title,
        session.artist,
        session.source_path,
        JSON.stringify(session.queue),
        session.mode,
        session.position_ms,
        session.stream_id,
        session.stream_issued_at,
        session.created_at,
        session.updated_at,
      );
    return session;
  }

  deleteSession(deviceId) {
    this.db.prepare("DELETE FROM music_sessions WHERE device_id = ?").run(deviceId);
  }

  getAccount() {
    return this.db.prepare("SELECT uid, nickname, updated_at FROM music_account WHERE id = 1").get() || null;
  }

  saveAccount(account) {
    this.db.prepare(`INSERT INTO music_account (id, uid, nickname, updated_at) VALUES (1, ?, ?, ?)
      ON CONFLICT(id) DO UPDATE SET uid = excluded.uid, nickname = excluded.nickname, updated_at = excluded.updated_at`)
      .run(account.uid, account.nickname, Date.now());
  }

  clearAccount() {
    this.db.exec("DELETE FROM music_account; DELETE FROM music_qr_sessions;");
  }

  saveQr(loginId, key, expiresAt) {
    this.db.prepare("INSERT OR REPLACE INTO music_qr_sessions (login_id, qr_key, expires_at, created_at) VALUES (?, ?, ?, ?)")
      .run(loginId, key, expiresAt, Date.now());
  }

  getQr(loginId) {
    return this.db.prepare("SELECT login_id, qr_key, expires_at FROM music_qr_sessions WHERE login_id = ?").get(loginId) || null;
  }

  getCommand(deviceId, commandId) {
    const row = this.db
      .prepare("SELECT result_json FROM music_commands WHERE device_id = ? AND command_id = ?")
      .get(deviceId, commandId);
    return row ? JSON.parse(row.result_json) : null;
  }

  saveCommand(deviceId, commandId, result) {
    this.db
      .prepare(
        "INSERT OR REPLACE INTO music_commands (device_id, command_id, result_json, created_at) VALUES (?, ?, ?, ?)",
      )
      .run(deviceId, commandId, JSON.stringify(result), Date.now());
  }

  createRemoteCommand(deviceId, action, payload, ttlMs) {
    const commandId = `music-command-${crypto.randomUUID()}`;
    const now = Date.now();
    const expiresAt = now + Math.max(1000, Number(ttlMs) || 30000);
    const insert = this.db.prepare(`
      INSERT INTO music_remote_commands (
        device_id, command_id, action, payload_json, state, created_at, expires_at
      ) VALUES (?, ?, ?, ?, 'pending', ?, ?)
    `);
    const supersede = this.db.prepare(`
      UPDATE music_remote_commands
      SET state = 'superseded', acked_at = ?, result_json = ?
      WHERE device_id = ? AND state = 'pending'
    `);
    this.db.exec("BEGIN IMMEDIATE");
    try {
      supersede.run(
        now,
        JSON.stringify({ state: "superseded", error_code: "superseded" }),
        deviceId,
      );
      insert.run(deviceId, commandId, action, JSON.stringify(payload || {}), now, expiresAt);
      this.db.exec("COMMIT");
    } catch (error) {
      this.db.exec("ROLLBACK");
      throw error;
    }
    return this.getRemoteCommand(deviceId, commandId);
  }

  getRemoteCommand(deviceId, commandId) {
    const row = this.db.prepare(`
      SELECT * FROM music_remote_commands WHERE device_id = ? AND command_id = ?
    `).get(deviceId, commandId);
    return row ? this.#decodeRemoteCommand(row) : null;
  }

  claimRemoteCommand(deviceId) {
    const now = Date.now();
    this.db.exec("BEGIN IMMEDIATE");
    try {
      this.db.prepare(`
        UPDATE music_remote_commands
        SET state = 'expired', acked_at = ?,
            result_json = COALESCE(result_json, ?)
        WHERE device_id = ? AND state = 'pending' AND expires_at <= ?
      `).run(now, JSON.stringify({ state: "expired", error_code: "expired" }), deviceId, now);
      const row = this.db.prepare(`
        SELECT * FROM music_remote_commands
        WHERE device_id = ? AND state = 'pending' AND expires_at > ?
        ORDER BY created_at ASC LIMIT 1
      `).get(deviceId, now);
      if (!row) {
        this.db.exec("COMMIT");
        return null;
      }
      this.db.prepare(`
        UPDATE music_remote_commands SET state = 'claimed', claimed_at = ?
        WHERE device_id = ? AND command_id = ? AND state = 'pending'
      `).run(now, deviceId, row.command_id);
      this.db.exec("COMMIT");
      row.state = "claimed";
      row.claimed_at = now;
      return this.#decodeRemoteCommand(row);
    } catch (error) {
      this.db.exec("ROLLBACK");
      throw error;
    }
  }

  acknowledgeRemoteCommand(deviceId, commandId, result, snapshot) {
    const now = Date.now();
    const existing = this.getRemoteCommand(deviceId, commandId);
    if (!existing) return null;
    if (existing.state === "executed" || existing.state === "expired" ||
        existing.state === "superseded") {
      return existing;
    }
    const normalizedResult = result && typeof result === "object" ? result : {
      state: "executed",
    };
    this.db.prepare(`
      UPDATE music_remote_commands
      SET state = 'executed', acked_at = ?, result_json = ?
      WHERE device_id = ? AND command_id = ? AND state IN ('pending', 'claimed', 'accepted')
    `).run(now, JSON.stringify(normalizedResult), deviceId, commandId);
    if (snapshot && typeof snapshot === "object") {
      this.db.prepare(`
        INSERT INTO music_device_snapshots(device_id, command_id, snapshot_json, updated_at)
        VALUES (?, ?, ?, ?)
        ON CONFLICT(device_id) DO UPDATE SET
          command_id = excluded.command_id,
          snapshot_json = excluded.snapshot_json,
          updated_at = excluded.updated_at
      `).run(deviceId, commandId, JSON.stringify(snapshot), now);
    }
    return this.getRemoteCommand(deviceId, commandId);
  }

  markRemoteAccepted(deviceId, commandId) {
    const now = Date.now();
    this.db.prepare(`
      UPDATE music_remote_commands
      SET state = 'accepted', result_json = ?
      WHERE device_id = ? AND command_id = ? AND state IN ('pending', 'claimed')
    `).run(JSON.stringify({ state: "accepted", command_id: commandId }), deviceId, commandId);
    return this.getRemoteCommand(deviceId, commandId);
  }

  getDeviceSnapshot(deviceId) {
    const row = this.db.prepare(`
      SELECT command_id, snapshot_json, updated_at
      FROM music_device_snapshots WHERE device_id = ?
    `).get(deviceId);
    if (!row) return null;
    return {
      command_id: row.command_id,
      snapshot: JSON.parse(row.snapshot_json),
      updated_at: Number(row.updated_at),
    };
  }

  close() {
    this.db.close();
  }

  #decodeSession(row) {
    return {
      device_id: row.device_id,
      music_session_id: row.music_session_id,
      state: row.state,
      source_id: row.source_id,
      track_id: row.track_id,
      title: row.title,
      artist: row.artist,
      source_path: row.source_path,
      queue: JSON.parse(row.queue_json),
      mode: row.mode,
      position_ms: Number(row.position_ms),
      stream_id: row.stream_id,
      stream_issued_at: row.stream_issued_at == null ? null : Number(row.stream_issued_at),
      created_at: Number(row.created_at),
      updated_at: Number(row.updated_at),
    };
  }

  #decodeRemoteCommand(row) {
    return {
      device_id: row.device_id,
      command_id: row.command_id,
      action: row.action,
      payload: JSON.parse(row.payload_json),
      state: row.state,
      created_at: Number(row.created_at),
      expires_at: Number(row.expires_at),
      claimed_at: row.claimed_at == null ? null : Number(row.claimed_at),
      acked_at: row.acked_at == null ? null : Number(row.acked_at),
      result: row.result_json ? JSON.parse(row.result_json) : null,
    };
  }
}
