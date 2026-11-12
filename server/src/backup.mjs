// 백업·복원: the broker's part. It makes the backup file, takes an uploaded one
// apart without changing anything, and hands a restore to the privileged
// updater (update/restore-engine.mjs), which is the only one that replaces
// files. Secrets never reach a log, an audit row or an error: errors are fixed
// codes, and the passphrase lives only in the request that carries it.
//
// Working files live in <data>/backup-work, private to the broker's account:
//   export-<id>.mtbk      an encrypted file waiting to be downloaded (10 min)
//   upload-<id>.part      an encrypted upload being received
//   work-<id>/            the snapshot or the decrypted upload while it is read
//   restore/<operation>/  the rebuilt database and vault key a restore would
//                         install (15 min); the updater reads exactly these
// Everything in it is removed when the broker starts.
import { spawn } from 'node:child_process';
import { randomUUID, randomBytes, createHash } from 'node:crypto';
import { DatabaseSync } from 'node:sqlite';
import { mkdir, rm, open, statfs, readdir, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { HttpError } from './errors.mjs';
import { BackupError, writeBackup, readBackup, readBackupHeader, backupMaxFileBytes, backupMaxDatabaseBytes, backupPassphrase, backupPassphraseMinimum, backupKdf } from './backup-format.mjs';
import { snapshotDatabase, backupExcludedTables, backupExcludedSettings, restorePendingSetting } from './backup-database.mjs';
import { normalizeServerAccess, serverAccessHash } from '../update/server-access-policy.mjs';
import { compareVersion } from '../update/protocol.mjs';

export const backupProtocol = 'backup-v1';
export const restoreLastSetting = 'restore_last', backupLastExportSetting = 'backup_last_export';
const statuses = { backup_passphrase_too_short: 400, backup_passphrase_invalid: 400, backup_busy: 409, backup_too_large: 413, backup_unavailable: 503, backup_helper_failed: 503, backup_helper_timeout: 503, backup_table_unclassified: 503 };
// A fixed code is all that is said about a failure.
function httpError(error) {
  if (error instanceof HttpError) return error;
  if (error instanceof BackupError && /^backup_[a-z_]{1,60}$/.test(error.code)) return new HttpError(statuses[error.code] ?? 422, error.code);
  return new HttpError(503, 'backup_unavailable');
}
// Attempts per administrator in a time window (a few keys at most).
class Attempts {
  constructor(limit, windowMs, now) { Object.assign(this, { limit, windowMs, now }); this.entries = new Map(); }
  take(key) {
    const now = this.now();
    for (const [name, entry] of this.entries) if (entry.until <= now) this.entries.delete(name);
    if (!this.entries.has(key) && this.entries.size >= 1000) return false;
    const entry = this.entries.get(key) ?? { count: 0, until: now + this.windowMs }; this.entries.set(key, entry);
    return ++entry.count <= this.limit;
  }
}
const terminal = new Set(['succeeded', 'rolled_back', 'failed', 'recovery_required']);
const uuid = /^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$/;
const identifier = () => randomBytes(24).toString('base64url');
export const defaultBackupHelperPath = fileURLToPath(new URL('./backup-helper.mjs', import.meta.url));
// A Docker Compose install has no root updater to apply a restore; the
// `multitap` command on that machine does it (deploy/docker/multitap).
export const dockerRestoreCommand = 'sudo multitap restore';

// Runs backup-helper.mjs once. The helper gets no environment of its caller
// and prints one line; only a fixed code is said about a failure. Used by the
// broker and by restore-cli.mjs (the Docker restore), so both read a backup
// file the same way.
export function spawnBackupHelper(input, { helperPath = defaultBackupHelperPath, timeoutMs = 120000 } = {}) {
  return new Promise((resolve, reject) => {
    const child = spawn(process.execPath, ['--max-old-space-size=256', helperPath], { stdio: ['pipe', 'pipe', 'ignore'], env: process.env.SystemRoot ? { SystemRoot: process.env.SystemRoot } : {}, windowsHide: true });
    let size = 0, settled = false; const chunks = [];
    const finish = (error, value) => { if (settled) return; settled = true; clearTimeout(timer); error ? reject(error) : resolve(value); };
    const timer = setTimeout(() => { child.kill('SIGKILL'); finish(new BackupError('backup_helper_timeout')); }, timeoutMs);
    child.stdout.on('data', chunk => { size += chunk.length; if (size > 65536) { child.kill('SIGKILL'); finish(new BackupError('backup_helper_failed')); } else chunks.push(chunk); });
    child.once('error', () => finish(new BackupError('backup_helper_failed')));
    child.once('close', () => {
      try {
        const value = JSON.parse(Buffer.concat(chunks).toString('utf8'));
        if (value?.ok === true && value.result && typeof value.result === 'object') finish(null, value.result);
        else finish(new BackupError(typeof value?.code === 'string' && /^backup_[a-z_]{1,60}$/.test(value.code) ? value.code : 'backup_helper_failed'));
      } catch { finish(new BackupError('backup_helper_failed')); }
    });
    child.stdin.on('error', () => {}); child.stdin.end(JSON.stringify(input));
  });
}

// The one row a restore adds to the database it installs: what the restored
// server reads at its first start (BackupService.startup).
export function writeRestoreMarker(databasePath, marker) {
  const database = new DatabaseSync(databasePath);
  try { database.prepare('INSERT INTO settings(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value').run(restorePendingSetting, JSON.stringify(marker)); } finally { database.close(); }
}

export class BackupService {
  constructor({ store, vault, directory = null, version, origin = null, accessConfig = null, deploymentKind = 'unknown', restoreClient = null, accessRuntime = null, proxmox = null, now = Date.now,
    helperPath = defaultBackupHelperPath, helperTimeoutMs = 120000, exportTtlMs = 600000, stagingTtlMs = 900000, uploadChunkBytes = 196608, maxUploadBytes = backupMaxFileBytes, space = path => statfs(path), afterRestoreDelayMs = 2000, kdf = backupKdf }) {
    Object.assign(this, { store, vault, directory, version, origin, accessConfig, deploymentKind, restoreClient, accessRuntime, proxmox, now, helperPath, helperTimeoutMs, exportTtlMs, stagingTtlMs, uploadChunkBytes, maxUploadBytes, space, afterRestoreDelayMs, kdf });
    this.exports = new Map(); this.upload = null; this.operation = null; this.exporting = false; this.inspecting = false; this.ready = null; this.followUp = null; this.followed = null;
    // Per administrator: password checks, files made, uploads started and
    // passphrases tried (each try costs the server a key derivation).
    this.limits = { reauthentication: new Attempts(5, 60000, now), exports: new Attempts(3, 600000, now), uploads: new Attempts(6, 600000, now), inspections: new Attempts(10, 600000, now), applies: new Attempts(5, 600000, now) };
    this.timer = setInterval(() => { void this.expire(); }, 30000); this.timer.unref();
  }
  close() { clearInterval(this.timer); clearTimeout(this.followTimer); }
  get available() { return typeof this.directory === 'string'; }
  get restoreSupported() { return this.available && !!this.restoreClient && !!this.accessRuntime; }
  path(...parts) { return join(this.directory, ...parts); }
  // The working directory, emptied once per start. Only this service writes there.
  prepare() {
    if (!this.available) throw new HttpError(503, 'backup_unavailable');
    return this.ready ??= (async () => {
      await mkdir(this.directory, { recursive: true, mode: 0o700 });
      for (const name of await readdir(this.directory)) if (/^(?:export-[A-Za-z0-9_-]{32}\.mtbk|upload-[A-Za-z0-9_-]{32}\.part|work-[A-Za-z0-9_-]{32}|restore)$/.test(name)) await rm(this.path(name), { recursive: true, force: true });
      await mkdir(this.path('restore'), { recursive: true, mode: 0o700 });
    })().catch(error => { this.ready = null; throw error; });
  }
  helper(input) { return spawnBackupHelper(input, { helperPath: this.helperPath, timeoutMs: this.helperTimeoutMs }); }
  async discard(...paths) { for (const path of paths) if (path) await rm(path, { recursive: true, force: true, maxRetries: 3, retryDelay: 50 }).catch(() => {}); }
  async expire() {
    const now = this.now();
    for (const [id, entry] of this.exports) if (entry.expires <= now) { this.exports.delete(id); await this.discard(entry.path); }
    if (this.upload && this.upload.expires <= now && this.upload.state !== 'applying') await this.dropUpload();
  }
  async dropUpload() {
    const upload = this.upload; this.upload = null;
    if (upload) await this.discard(upload.path, upload.staging?.directory);
  }
  setting(key) { try { const value = JSON.parse(this.store.setting(key) ?? 'null'); return value && typeof value === 'object' && !Array.isArray(value) ? value : null; } catch { return null; } }
  saveSetting(key, value) { this.store.db.prepare('INSERT INTO settings(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value').run(key, JSON.stringify(value)); }

  // ---- export -----------------------------------------------------------
  async export({ passphrase, actor, actorName }) {
    await this.prepare();
    try { backupPassphrase(passphrase).fill(0); } catch (error) { throw httpError(error); }
    if (this.exporting || this.inspecting) throw new HttpError(409, 'backup_busy');
    this.exporting = true;
    const id = identifier(), work = this.path('work-' + id), output = this.path('export-' + id + '.mtbk');
    try {
      const pages = this.store.db.prepare('PRAGMA page_count').get().page_count * this.store.db.prepare('PRAGMA page_size').get().page_size;
      // The diagnostics that do not travel are at most a few tens of megabytes;
      // a database far over the bound cannot fit and is not even copied.
      if (pages > backupMaxDatabaseBytes * 2) throw new BackupError('backup_too_large');
      const free = await this.space(this.directory);
      if (Number(free.bavail) * Number(free.bsize) < pages * 3 + 64 * 1024 * 1024) throw new HttpError(507, 'backup_disk_space');
      await mkdir(work, { mode: 0o700 });
      const snapshot = join(work, 'snapshot.sqlite');
      await snapshotDatabase(this.store.db, snapshot);
      const createdAtMs = this.now(), cleaned = await this.helper({ mode: 'sanitize', path: snapshot, now: createdAtMs });
      const createdAt = new Date(createdAtMs).toISOString();
      const written = await writeBackup({ passphrase, createdAt, serverVersion: this.version, databasePath: snapshot, outputPath: output, kdf: this.kdf,
        manifest: { schema: 1, origin: this.origin, deploymentKind: this.deploymentKind, database: { schemaVersion: cleaned.schemaVersion }, vaultKey: Buffer.from(this.vault.key).toString('base64'), serverAccess: this.accessConfig ?? null,
          excluded: [...backupExcludedTables, ...backupExcludedSettings.map(key => 'settings.' + key)] } });
      const filename = 'multitap-backup-' + createdAt.slice(0, 10).replaceAll('-', '') + '-' + createdAt.slice(11, 16).replace(':', '') + '.mtbk';
      this.exports.set(id, { path: output, size: written.size, filename, createdAt, expires: this.now() + this.exportTtlMs });
      // Who, when and how large. Never the passphrase or anything from inside.
      this.store.transaction(() => { this.saveSetting(backupLastExportSetting, { at: createdAt, size: written.size, actor: actorName }); this.store.audit('backup_exported', actor, String(written.size)); });
      return { id, size: written.size, createdAt, filename, counts: cleaned.counts, expiresAt: new Date(this.now() + this.exportTtlMs).toISOString() };
    } catch (error) { await this.discard(output); throw httpError(error); }
    finally { this.exporting = false; await this.discard(work); }
  }
  download(id) {
    const entry = typeof id === 'string' ? this.exports.get(id) : null;
    if (!entry || entry.expires <= this.now()) throw new HttpError(404, 'backup_download_expired');
    return entry;
  }

  // ---- restore: receive, read, show --------------------------------------
  restoring() { return this.operation !== null || this.upload?.state === 'applying'; }
  async beginUpload({ size, owner, actor }) {
    await this.prepare();
    if (!Number.isSafeInteger(size) || size < 64) throw new HttpError(400, 'backup_file_invalid');
    if (size > this.maxUploadBytes) throw new HttpError(413, 'backup_too_large');
    if (this.restoring() || this.inspecting) throw new HttpError(409, 'restore_busy');
    if (!this.limits.uploads.take(actor)) throw new HttpError(429, 'backup_rate_limited');
    const free = await this.space(this.directory);
    if (Number(free.bavail) * Number(free.bsize) < size * 4 + 64 * 1024 * 1024) throw new HttpError(507, 'backup_disk_space');
    await this.dropUpload();
    const id = identifier(), path = this.path('upload-' + id + '.part');
    await writeFile(path, '', { flag: 'wx', mode: 0o600 });
    this.upload = { id, owner, path, size, received: 0, state: 'receiving', expires: this.now() + this.stagingTtlMs, staging: null };
    return { uploadId: id, chunkBytes: this.uploadChunkBytes, size };
  }
  mine(uploadId, owner) {
    const upload = this.upload;
    if (!upload || upload.id !== uploadId || upload.owner !== owner || upload.expires <= this.now()) throw new HttpError(404, 'backup_upload_not_found');
    return upload;
  }
  async appendUpload({ uploadId, owner, offset, data }) {
    const upload = this.mine(uploadId, owner);
    if (upload.state !== 'receiving' || upload.writing) throw new HttpError(409, 'backup_upload_state');
    if (!Number.isSafeInteger(offset) || offset !== upload.received) throw new HttpError(409, 'backup_upload_offset', 'backup_upload_offset', { received: upload.received });
    if (typeof data !== 'string' || !data || data.length > Math.ceil(this.uploadChunkBytes / 3) * 4 || !/^[A-Za-z0-9+/]+={0,2}$/.test(data)) throw new HttpError(400, 'backup_file_invalid');
    const bytes = Buffer.from(data, 'base64');
    if (bytes.toString('base64') !== data || bytes.length > this.uploadChunkBytes || upload.received + bytes.length > upload.size) throw new HttpError(400, 'backup_file_invalid');
    upload.writing = true;
    try {
      const file = await open(upload.path, 'r+');
      try { await file.write(bytes, 0, bytes.length, upload.received); } finally { await file.close(); }
      upload.received += bytes.length; upload.expires = this.now() + this.stagingTtlMs;
    } finally { upload.writing = false; }
    return { received: upload.received, complete: upload.received === upload.size };
  }
  async cancelUpload({ uploadId, owner }) {
    const upload = this.mine(uploadId, owner);
    if (upload.state === 'applying') throw new HttpError(409, 'restore_busy');
    await this.dropUpload();
    return { ok: true };
  }
  // Decrypts and validates. Nothing of the running server changes: the result
  // is two files in the staging directory and what the console shows.
  async inspect({ uploadId, owner, passphrase, actor }) {
    const upload = this.mine(uploadId, owner);
    if (upload.state !== 'receiving' && upload.state !== 'inspected') throw new HttpError(409, 'backup_upload_state');
    if (upload.state === 'receiving' && upload.received !== upload.size) throw new HttpError(409, 'backup_upload_incomplete');
    if (upload.state === 'inspected') return { uploadId, preview: upload.staging.preview };
    try { backupPassphrase(passphrase).fill(0); } catch (error) { throw httpError(error); }
    if (this.exporting || this.inspecting) throw new HttpError(409, 'backup_busy');
    this.inspecting = true;
    const operationId = randomUUID(), work = this.path('work-' + identifier()), staging = this.path('restore', operationId);
    let keep = false;
    try {
      const header = await readBackupHeader(upload.path);
      // A backup is restored on the same or a newer server version, never an older one.
      if (compareVersion(header.serverVersion, this.version) > 0) throw new BackupError('backup_from_newer_server');
      await mkdir(work, { mode: 0o700 });
      let manifest;
      try { ({ manifest } = await readBackup({ path: upload.path, passphrase, databasePath: join(work, 'uploaded.sqlite') })); }
      catch (error) { keep = error instanceof BackupError && ['backup_unreadable', 'backup_busy'].includes(error.code); throw error; }
      await mkdir(staging, { recursive: true, mode: 0o700 });
      const result = await this.helper({ mode: 'rebuild', source: join(work, 'uploaded.sqlite'), work: join(work, 'migrate.sqlite'), output: join(staging, 'broker.sqlite'), vaultKey: manifest.vaultKey, now: this.now() });
      await writeFile(join(staging, 'vault.key'), Buffer.from(manifest.vaultKey, 'base64'), { flag: 'wx', mode: 0o600 });
      let access = null;
      try { access = manifest.serverAccess ? this.portableAccess(normalizeServerAccess(manifest.serverAccess)) : null; } catch { access = null; }
      const differs = access !== null && (!this.accessConfig || serverAccessHash(access) !== serverAccessHash(this.accessConfig));
      const expires = this.now() + this.stagingTtlMs;
      const preview = { createdAt: header.createdAt, serverVersion: header.serverVersion, origin: manifest.origin, counts: result.counts, proxmox: result.proxmox, unreadableSecrets: result.unreadableSecrets,
        schemaVersion: result.schemaVersion, migrated: result.migrated === true, ignoredObjects: result.ignoredObjects, access: { available: access !== null, differs, primaryOrigin: access?.primaryOrigin ?? null }, expiresAt: new Date(expires).toISOString() };
      await this.discard(upload.path);
      Object.assign(upload, { state: 'inspected', expires, path: null, staging: { operationId, directory: staging, schemaVersion: result.currentSchemaVersion, preview, access: differs ? access : null, backup: { createdAt: header.createdAt, serverVersion: header.serverVersion, origin: manifest.origin } } });
      keep = true;
      this.store.audit('backup_inspected', actor, header.createdAt);
      return { uploadId, preview };
    } catch (error) {
      await this.discard(staging);
      if (!keep && this.upload === upload) await this.dropUpload();
      throw httpError(error);
    } finally { this.inspecting = false; await this.discard(work); }
  }
  // No path from a backup file is ever used. A backup of a server that served
  // TLS itself names certificate files; its settings can be applied only with
  // this server's own certificate files, and not at all where this server has none.
  portableAccess(config) {
    if (config.https.mode !== 'direct') return config;
    const mine = this.accessConfig?.https;
    if (!(mine?.mode === 'direct' && mine.certFile && mine.keyFile)) return null;
    return normalizeServerAccess({ ...config, https: { ...config.https, certFile: mine.certFile, keyFile: mine.keyFile } });
  }
  async fileDigest(path) {
    const file = await open(path, 'r'), hash = createHash('sha256');
    try {
      const buffer = Buffer.allocUnsafe(1048576); let position = 0;
      while (true) { const { bytesRead } = await file.read(buffer, 0, buffer.length, position); if (!bytesRead) break; position += bytesRead; hash.update(buffer.subarray(0, bytesRead)); }
      return { sha256: hash.digest('hex'), size: position };
    } finally { await file.close(); }
  }

  // ---- restore: hand over to the updater ----------------------------------
  async apply({ uploadId, owner, restoreAccess = false, actor, actorName }) {
    const upload = this.mine(uploadId, owner);
    if (upload.state !== 'inspected') throw new HttpError(409, 'backup_inspection_required');
    if (!this.restoreSupported) throw new HttpError(503, 'restore_unavailable');
    if (this.operation) throw new HttpError(409, 'restore_busy');
    const before = await this.restoreClient.status();
    if (before.busy === true) throw new HttpError(409, 'restore_busy');
    if (before.state === 'recovery_required') throw new HttpError(409, 'restore_recovery_required');
    const { operationId, directory, schemaVersion, backup } = upload.staging, access = restoreAccess === true ? upload.staging.access : null;
    // What the restored server reads at its first start (startup below). It
    // names the backup and who restored it; it is the only thing added.
    const marker = { operationId, restoredAt: new Date(this.now()).toISOString(), backup, actor: actorName, replaced: { origin: this.origin, serverVersion: this.version }, access };
    writeRestoreMarker(join(directory, 'broker.sqlite'), marker);
    const staged = await this.fileDigest(join(directory, 'broker.sqlite')), key = await this.fileDigest(join(directory, 'vault.key'));
    const requestedAt = this.now();
    const lease = this.accessRuntime.hold({ operationId, actor, event: 'backup_restore_connections_ended', observe: () => this.observe(operationId, requestedAt) });
    this.operation = { operationId, lease, actor, requestedAt, backup }; upload.state = 'applying';
    try { this.store.audit('backup_restore_requested', actor, `${operationId}:${backup.createdAt}:${access ? 'access' : 'keep-access'}`); } catch { /* The audit row cannot stop it. */ }
    try {
      const result = await this.restoreClient.apply({ operationId, database: { sha256: staged.sha256, size: staged.size }, vault: { sha256: key.sha256 }, schemaVersion, confirm: true }, { fence: { token: lease.token, operationId } });
      if (result?.operationId !== operationId) throw new HttpError(503, 'restore_result_unknown');
      return { accepted: true, operationId, state: result.state };
    } catch (error) {
      // A refusal is final. A lost answer may follow an acceptance: the fence
      // stays until the updater's own status settles it (observe).
      if ([400, 409].includes(error?.status)) { this.accessRuntime.release(lease); this.operation = null; upload.state = 'inspected'; }
      throw error instanceof HttpError ? error : new HttpError(503, 'restore_unavailable');
    }
  }
  // Called while the fence is held. True once the operation is over; a restore
  // that succeeded never gets here, because this process was stopped.
  async observe(operationId, requestedAt) {
    const status = await this.restoreClient.status();
    const over = status.operationId === operationId ? terminal.has(status.state) : status.busy !== true && this.now() - requestedAt > 15000;
    if (!over) return false;
    if (this.operation?.operationId === operationId) {
      const started = status.operationId === operationId, event = started && status.state === 'succeeded' ? 'backup_restore_finished' : 'backup_restore_failed';
      try { this.store.audit(event, this.operation.actor ?? 'server', `${operationId}:${started ? status.errorCode ?? status.state : 'not_started'}`); } catch { /* best effort */ }
      this.operation = null;
      if (this.upload?.staging?.operationId === operationId) await this.dropUpload();
    }
    return true;
  }
  async status({ owner = null, restricted = false } = {}) {
    const upload = this.upload && this.upload.owner === owner && this.upload.expires > this.now() ? this.upload : null;
    const result = { protocol: backupProtocol, export: { available: this.available, passphraseMinimum: backupPassphraseMinimum, maxDatabaseBytes: backupMaxDatabaseBytes, ...(restricted ? {} : { last: this.setting(backupLastExportSetting) }) },
      restore: { supported: this.restoreSupported, state: this.restoreSupported ? 'unknown' : 'unavailable', chunkBytes: this.uploadChunkBytes, maxFileBytes: this.maxUploadBytes },
      upload: upload ? { uploadId: upload.id, size: upload.size, received: upload.received, state: upload.state, ...(upload.staging ? { preview: upload.staging.preview, operationId: upload.staging.operationId } : {}) } : null,
      ...(restricted ? {} : { last: this.setting(restoreLastSetting) }) };
    // Docker Compose without an updater: the console checks a file, the server
    // itself restores with the command it names (no file path: the console
    // shows its own placeholder after it).
    if (!this.restoreSupported && this.deploymentKind === 'docker') result.restore = { ...result.restore, state: 'docker-command', command: dockerRestoreCommand };
    if (this.restoreSupported) {
      try {
        const status = await this.restoreClient.status();
        // An operation id, a state and fixed codes: enough for the console that
        // started a restore to recognise its own after the server came back.
        result.restore = { ...result.restore, state: status.state, busy: status.busy === true,
          ...Object.fromEntries(['operationId', 'errorCode', 'blockers', 'waitUntil', 'completedAt'].filter(key => status[key] !== undefined).map(key => [key, status[key]])) };
      } catch { result.restore = { ...result.restore, supported: false, state: 'unavailable' }; }
    }
    return result;
  }

  // ---- the first start after a restore ------------------------------------
  // The restored database carries one marker. It becomes the record of the
  // last restore and an audit row; then the Proxmox connection is checked and,
  // only if it was asked for, the backup's access settings are applied through
  // the ordinary access pipeline (its own test, health check and rollback).
  startup() {
    const pending = this.setting(restorePendingSetting);
    if (!pending) return null;
    const valid = uuid.test(pending.operationId ?? '') && typeof pending.restoredAt === 'string' && pending.backup && typeof pending.backup.createdAt === 'string';
    let access = null;
    try { access = valid && pending.access ? normalizeServerAccess(pending.access) : null; } catch { access = null; }
    const record = valid ? { operationId: pending.operationId, restoredAt: pending.restoredAt, backup: { createdAt: pending.backup.createdAt, serverVersion: String(pending.backup.serverVersion ?? ''), origin: typeof pending.backup.origin === 'string' ? pending.backup.origin : null }, actor: typeof pending.actor === 'string' ? pending.actor.slice(0, 64) : null,
      proxmox: { state: this.store.setting('proxmox') ? 'checking' : 'none' }, access: { state: access ? 'pending' : 'kept' } } : null;
    this.store.transaction(() => {
      this.store.db.prepare('DELETE FROM settings WHERE key=?').run(restorePendingSetting);
      if (record) { this.saveSetting(restoreLastSetting, record); this.store.audit('backup_restored', 'server', `${record.operationId}:${record.backup.createdAt}`); }
    });
    if (!record) return null;
    this.followUp = { operationId: record.operationId, access };
    this.followTimer = setTimeout(() => { this.followed = this.afterRestore().catch(() => {}); }, this.afterRestoreDelayMs); this.followTimer.unref();
    return record;
  }
  updateLast(change) { const current = this.setting(restoreLastSetting); if (current) this.saveSetting(restoreLastSetting, { ...current, ...change }); }
  async afterRestore() {
    const follow = this.followUp; if (!follow) return;
    this.followUp = null;
    const sealed = this.store.setting('proxmox');
    if (sealed) {
      let outcome;
      try {
        let config; try { config = this.vault.open(sealed); } catch { throw new HttpError(503, 'proxmox_settings_unreadable'); }
        if (typeof this.proxmox?.call !== 'function') throw new HttpError(503, 'proxmox_validation_unavailable');
        await this.proxmox.call(config, '/access/permissions?path=%2Fvms');
        outcome = { state: 'ok' };
      } catch (error) { outcome = { state: 'error', errorCode: error instanceof HttpError && /^[a-z_]{1,60}$/.test(error.code) ? error.code : 'proxmox_request_failed' }; }
      this.updateLast({ proxmox: { ...outcome, checkedAt: new Date(this.now()).toISOString() } });
    }
    if (!follow.access) return;
    let outcome;
    try {
      if (!this.accessRuntime?.client) throw new HttpError(503, 'server_access_unavailable');
      // The updater is still finishing the restore itself (its health check).
      for (let attempt = 0; attempt < 60; attempt++) {
        const status = await this.restoreClient.status();
        if (status.busy !== true) break;
        await new Promise(resolve => { const timer = setTimeout(resolve, 1500); timer.unref(); });
      }
      const context = { managementOrigin: this.origin }, current = await this.accessRuntime.status(context);
      if (current.revision === serverAccessHash(follow.access)) outcome = { state: 'same' };
      else {
        const tested = await this.accessRuntime.test({ config: follow.access, baseRevision: current.revision }, context);
        const saved = await this.accessRuntime.save({ config: follow.access, testId: tested.testId, baseRevision: current.revision, confirm: true }, context, { actor: 'restore' });
        outcome = { state: 'requested', operationId: saved.operationId };
      }
    } catch (error) { outcome = { state: 'failed', errorCode: typeof error?.code === 'string' && /^[a-z_]{1,80}$/.test(error.code) ? error.code : 'server_access_unavailable' }; }
    this.updateLast({ access: { ...outcome, at: new Date(this.now()).toISOString() } });
    const event = { same: 'backup_restore_access_same', requested: 'backup_restore_access_requested', failed: 'backup_restore_access_failed' }[outcome.state];
    try { this.store.audit(event, 'server', `${follow.operationId}:${outcome.errorCode ?? outcome.operationId ?? ''}`); } catch { /* best effort */ }
  }
}
