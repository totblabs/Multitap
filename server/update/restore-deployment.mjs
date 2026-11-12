// What the privileged agent does to the installation when a backup is restored
// (백업·복원). It works on a fixed set of files in the broker's data directory:
// the database (with its write-ahead files) and the vault key. No name or path
// comes from the backup file or from the broker; the broker only names the
// operation and says which hashes it staged.
//
// Order, each step durable before the next:
//   prepare  copy the staged files into a root-only operation directory and
//            check them there (hash, size, SQLite check, schema version)
//   apply    stop the broker, copy the live files into the same directory
//            (the safety copy), write safety.json, put the new files in place,
//            start the broker
//   verify   the broker answers its health check as before
//   rollback stop, put the safety copy back, start, health check
// The data directory belongs to the broker's own account, so nothing in it is
// trusted: files are opened without following links, must be regular files of
// that account, and are replaced by a rename of a file created here.
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { constants } from 'node:fs';
import { lstat, mkdir, open, rename, unlink, realpath, statfs, readdir, rm } from 'node:fs/promises';
import { join } from 'node:path';
import { createHash } from 'node:crypto';

const execute = promisify(execFile);
const uuid = /^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$/, hex = /^[a-f0-9]{64}$/;
const fail = code => { throw Error('restore_' + code); };
const defaultRun = async (program, args) => (await execute(program, args, { maxBuffer: 1048576, timeout: 60000, env: { PATH: '/usr/sbin:/usr/bin:/sbin:/bin', LANG: 'C.UTF-8' } })).stdout;
// The live files a restore replaces, and the two the broker stages.
export const restoreLiveFiles = Object.freeze(['broker.sqlite', 'broker.sqlite-wal', 'broker.sqlite-shm', 'vault.key']);
export const restoreStagedFiles = Object.freeze(['broker.sqlite', 'vault.key']);
export const restoreStagingPath = (data, operationId) => join(data, 'backup-work', 'restore', operationId);
const maximumDatabase = 256 * 1024 * 1024, maximumLive = 64 * 1024 * 1024 * 1024;
const digestOf = bytes => createHash('sha256').update(bytes).digest('hex');
const samePath = (a, b) => process.platform === 'win32' ? a.toLowerCase() === b.toLowerCase() : a === b;

export class ServerRestoreDeployment {
  // `access` is the initialised ServerAccessDeployment of the same agent: it
  // already knows the data directory, its owner, how to probe the broker and
  // how to ask it to end its connections. Nothing of that is duplicated here.
  constructor({ access, directory, run = defaultRun, sleep = ms => new Promise(resolve => setTimeout(resolve, ms)), space = path => statfs(path), posix = process.platform !== 'win32', healthAttempts = 40, healthDelayMs = 500 }) {
    Object.assign(this, { access, directory, run, sleep, space, posix, healthAttempts, healthDelayMs });
  }
  get data() { return this.access.data; }
  async privateDirectory(path) {
    const stat = await lstat(path);
    if (!stat.isDirectory() || stat.isSymbolicLink() || !samePath(await realpath(path), path) || (this.posix && (stat.uid !== 0 || (stat.mode & 0o077)))) fail('storage_invalid');
  }
  async plainDirectory(path) {
    const stat = await lstat(path);
    if (!stat.isDirectory() || stat.isSymbolicLink() || !samePath(await realpath(path), path)) fail('storage_invalid');
    return stat;
  }
  async initialize() {
    if (!['systemd', 'docker'].includes(this.access.kind)) fail('deployment_unsupported');
    await mkdir(this.directory, { recursive: true, mode: 0o700 }); await this.privateDirectory(this.directory);
    await this.plainDirectory(this.data);
  }
  current() { return this.access.current(); }
  activity(current, token) { return this.access.activity(current, token); }
  endConnections(current, token) { return this.access.endConnections(current, token); }
  unchanged(previous, token) { return this.access.unchanged(previous, token); }

  // Copies one regular file. `owner` (a uid) is required of a source in the
  // data directory; a link, a device, a hard-linked or someone else's file is
  // refused. Returns what was actually read.
  async copy(from, to, { maximum, owner = null, optional = false }) {
    let source;
    try { source = await open(from, constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0)); }
    catch (error) { if (optional && error.code === 'ENOENT') return null; fail('storage_invalid'); }
    let target;
    try {
      const stat = await source.stat();
      if (!stat.isFile() || stat.nlink !== 1 || stat.size > maximum || (this.posix && owner !== null && stat.uid !== owner)) fail('storage_invalid');
      target = await open(to, 'wx', 0o600);
      const hash = createHash('sha256'), buffer = Buffer.allocUnsafe(1048576); let position = 0;
      while (true) {
        const { bytesRead } = await source.read(buffer, 0, buffer.length, position);
        if (!bytesRead) break;
        position += bytesRead; if (position > maximum) fail('storage_invalid');
        hash.update(buffer.subarray(0, bytesRead)); await target.write(buffer.subarray(0, bytesRead));
      }
      await target.sync();
      return { size: position, sha256: hash.digest('hex') };
    } finally { await source.close(); await target?.close(); }
  }
  async sync(directory) { if (process.platform !== 'linux') return; const handle = await open(directory, 'r'); try { await handle.sync(); } finally { await handle.close(); } }
  async hashOf(path, maximum) {
    const file = await open(path, constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0));
    try {
      const stat = await file.stat(); if (!stat.isFile() || stat.size > maximum) fail('storage_invalid');
      const hash = createHash('sha256'), buffer = Buffer.allocUnsafe(1048576); let position = 0;
      while (true) { const { bytesRead } = await file.read(buffer, 0, buffer.length, position); if (!bytesRead) break; position += bytesRead; hash.update(buffer.subarray(0, bytesRead)); }
      return { size: position, sha256: hash.digest('hex') };
    } finally { await file.close(); }
  }
  async durable(path, text) {
    const file = await open(path, 'wx', 0o600);
    try { await file.writeFile(text); await file.sync(); } finally { await file.close(); }
    await this.sync(join(path, '..'));
  }
  async checkDatabase(path, schemaVersion) {
    const { DatabaseSync } = await import('node:sqlite'), db = new DatabaseSync(path, { readOnly: true, allowExtension: false });
    try {
      db.exec('PRAGMA trusted_schema=OFF;');
      const rows = db.prepare('PRAGMA quick_check(1)').all();
      if (rows.length !== 1 || Object.values(rows[0])[0] !== 'ok' || db.prepare('PRAGMA user_version').get().user_version !== schemaVersion) fail('staged_database_invalid');
    } catch (error) { if (/^restore_/.test(error.message)) throw error; fail('staged_database_invalid'); }
    finally { db.close(); }
  }
  // Safety copies of earlier restores are kept for the two most recent
  // operations; older ones are removed before a new one is prepared.
  async prune(keep = 1) {
    const entries = [];
    for (const name of await readdir(this.directory)) {
      if (!uuid.test(name)) continue;
      const stat = await lstat(join(this.directory, name));
      if (stat.isDirectory() && !stat.isSymbolicLink()) entries.push({ name, time: stat.mtimeMs });
    }
    entries.sort((a, b) => b.time - a.time);
    for (const entry of entries.slice(keep)) await rm(join(this.directory, entry.name), { recursive: true, force: true });
  }
  async prepare(input, { previous }) {
    const operationId = input.operationId;
    if (!uuid.test(operationId ?? '') || !hex.test(input.database?.sha256 ?? '') || !hex.test(input.vault?.sha256 ?? '') || !Number.isSafeInteger(input.database.size) || input.database.size < 512 || input.database.size > maximumDatabase ||
        !Number.isSafeInteger(input.schemaVersion) || input.schemaVersion < 1 || input.schemaVersion > 100000) fail('command_invalid');
    await this.privateDirectory(this.directory);
    const data = await this.plainDirectory(this.data), staging = restoreStagingPath(this.data, operationId);
    for (const path of [join(this.data, 'backup-work'), join(this.data, 'backup-work', 'restore'), staging]) { try { await this.plainDirectory(path); } catch { fail('staging_invalid'); } }
    await this.prune(1);
    // Room for the staged copy and the safety copy beside it, and for the new
    // database in the data directory, before anything is stopped.
    let live = 0;
    for (const name of restoreLiveFiles) { try { const stat = await lstat(join(this.data, name)); if (stat.isFile()) live += stat.size; } catch (error) { if (error.code !== 'ENOENT') fail('storage_invalid'); } }
    const room = async (path, needed) => { const stat = await this.space(path); if (Number(stat.bavail) * Number(stat.bsize) < needed + 64 * 1024 * 1024) fail('disk_space'); };
    await room(this.directory, input.database.size + live); await room(this.data, input.database.size);
    const operation = join(this.directory, operationId);
    try { await mkdir(operation, { mode: 0o700 }); } catch { fail('operation_reused'); }
    await mkdir(join(operation, 'after'), { mode: 0o700 }); await mkdir(join(operation, 'before'), { mode: 0o700 });
    const files = {};
    for (const name of restoreStagedFiles) {
      let copied;
      try { copied = await this.copy(join(staging, name), join(operation, 'after', name), { maximum: name === 'vault.key' ? 32 : maximumDatabase, owner: data.uid }); }
      catch { fail('staging_invalid'); }
      const expected = name === 'vault.key' ? { sha256: input.vault.sha256, size: 32 } : input.database;
      if (copied.sha256 !== expected.sha256 || copied.size !== expected.size) fail('staging_changed');
      files[name] = copied;
    }
    await this.checkDatabase(join(operation, 'after', 'broker.sqlite'), input.schemaVersion);
    const job = { schema: 1, operationId, operation, files, databaseSchema: input.schemaVersion, kind: this.access.kind, containerId: previous.containerId ?? null,
      previous: { revision: previous.revision, envSha256: previous.envSha256 ?? null, accessSha256: previous.accessSha256 ?? null, transport: previous.transport ?? null, config: previous.config ?? null, installationId: previous.installationId ?? null, internalPort: previous.internalPort ?? null, containerId: previous.containerId ?? null } };
    await this.durable(join(operation, 'plan.json'), JSON.stringify(job));
    return { ...job, planSha256: digestOf(Buffer.from(JSON.stringify(job))) };
  }
  async validateJob(job) {
    if (!job || job.schema !== 1 || !uuid.test(job.operationId ?? '') || !samePath(job.operation ?? '', join(this.directory, job.operationId)) || !hex.test(job.planSha256 ?? '')) fail('recovery_required');
    await this.privateDirectory(this.directory); await this.privateDirectory(job.operation);
    const plan = await open(join(job.operation, 'plan.json'), constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0));
    let bytes; try { bytes = await plan.readFile(); } finally { await plan.close(); }
    const { planSha256, ...expected } = job;
    if (bytes.length > 65536 || digestOf(bytes) !== planSha256 || bytes.toString('utf8') !== JSON.stringify(expected)) fail('recovery_required');
    if (Object.keys(job.files).sort().join(',') !== [...restoreStagedFiles].sort().join(',')) fail('recovery_required');
    for (const name of restoreStagedFiles) {
      const actual = await this.hashOf(join(job.operation, 'after', name), maximumDatabase);
      if (actual.sha256 !== job.files[name].sha256 || actual.size !== job.files[name].size) fail('recovery_required');
    }
  }
  // Stopping must succeed before a database file is touched.
  async stop(job) {
    if (job.kind === 'systemd') await this.run('/bin/systemctl', ['stop', this.access.serviceName]);
    else await this.access.adapter.run(['container', 'stop', '--time', '15', job.containerId]);
  }
  async start(job) {
    if (job.kind === 'systemd') await this.run('/bin/systemctl', ['start', this.access.serviceName]);
    else await this.access.adapter.run(['container', 'start', job.containerId]);
  }
  // A file made here, given to the broker's account and renamed over the live
  // name. Neither step follows a link that may have been planted there.
  async place(job, side, name) {
    const data = await this.plainDirectory(this.data), from = join(job.operation, side, name), temporary = join(this.data, name + '.restore-' + job.operationId + '.tmp');
    try { await unlink(temporary); } catch (error) { if (error.code !== 'ENOENT') fail('storage_invalid'); }
    const source = await open(from, constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0));
    let target;
    try {
      target = await open(temporary, 'wx', 0o600);
      if (this.posix) { await target.chown(data.uid, data.gid); await target.chmod(0o600); }
      const buffer = Buffer.allocUnsafe(1048576); let position = 0;
      while (true) { const { bytesRead } = await source.read(buffer, 0, buffer.length, position); if (!bytesRead) break; position += bytesRead; await target.write(buffer.subarray(0, bytesRead)); }
      await target.sync();
    } finally { await source.close(); await target?.close(); }
    await rename(temporary, join(this.data, name)); await this.sync(this.data);
  }
  async remove(name) {
    try { await unlink(join(this.data, name)); } catch (error) { if (error.code !== 'ENOENT') fail('storage_invalid'); }
    await this.sync(this.data);
  }
  async safety(job) {
    let file;
    try { file = await open(join(job.operation, 'safety.json'), constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0)); }
    catch (error) { if (error.code === 'ENOENT') return null; fail('recovery_required'); }
    let value;
    try { const bytes = await file.readFile(); if (bytes.length > 4096) fail('recovery_required'); value = JSON.parse(bytes.toString('utf8')); }
    catch { fail('recovery_required'); } finally { await file.close(); }
    if (value?.schema !== 1 || value.operationId !== job.operationId || Object.keys(value.files ?? {}).sort().join(',') !== [...restoreLiveFiles].sort().join(',')) fail('recovery_required');
    for (const name of restoreLiveFiles) {
      const expected = value.files[name];
      if (expected === null) continue;
      if (!hex.test(expected?.sha256 ?? '') || !Number.isSafeInteger(expected.size)) fail('recovery_required');
      const actual = await this.hashOf(join(job.operation, 'before', name), maximumLive);
      if (actual.sha256 !== expected.sha256 || actual.size !== expected.size) fail('recovery_required');
    }
    return value;
  }
  async apply(job) {
    await this.validateJob(job);
    const current = await this.access.current();
    if (current.revision !== job.previous.revision || (current.envSha256 ?? null) !== job.previous.envSha256 || (current.accessSha256 ?? null) !== job.previous.accessSha256 || (current.containerId ?? null) !== job.containerId) fail('installation_changed');
    if (await this.safety(job)) fail('operation_reused');
    const data = await this.plainDirectory(this.data);
    await this.stop(job);
    // The safety copy: every live file exactly as the stopped broker left it.
    const files = {};
    for (const name of restoreLiveFiles) files[name] = await this.copy(join(this.data, name), join(job.operation, 'before', name), { maximum: maximumLive, owner: data.uid, optional: true });
    if (!files['broker.sqlite'] || !files['vault.key']) fail('installation_changed');
    await this.durable(join(job.operation, 'safety.json'), JSON.stringify({ schema: 1, operationId: job.operationId, files }));
    // Write-ahead files of the old database must never meet the new one.
    await this.remove('broker.sqlite-wal'); await this.remove('broker.sqlite-shm');
    await this.place(job, 'after', 'vault.key'); await this.place(job, 'after', 'broker.sqlite');
    await this.start(job);
  }
  async healthy(job) {
    for (let attempt = 0; attempt < this.healthAttempts; attempt++) {
      try { if (await this.access.unchangedAfterRestart(job.previous) === true) return true; } catch { /* Not answering yet. */ }
      await this.sleep(this.healthDelayMs);
    }
    return false;
  }
  verify(job) { return this.healthy(job); }
  // Puts the safety copy back. Without safety.json nothing was replaced: the
  // broker is started again (it may have been stopped) with its own data.
  async rollback(job) {
    await this.validateJob(job);
    const safety = await this.safety(job);
    if (!safety) { await this.start(job); return this.healthy(job); }
    await this.stop(job);
    for (const name of restoreLiveFiles) {
      if (safety.files[name] === null) await this.remove(name);
      else await this.place(job, 'before', name);
    }
    await this.start(job);
    return this.healthy(job);
  }
}
