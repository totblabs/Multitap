// 백업·복원 for a Docker Compose install: what `sudo multitap restore <file>`
// (deploy/docker/multitap) runs in a one-off container of the server's own
// image. The server's restore code does the work, exactly as on the systemd
// install: the backup file is read and checked by backup-format.mjs, its
// database is rebuilt into this image's own schema by the same helper process
// (backup-helper.mjs, backup-database.mjs), and the files are put in place the
// way the updater does it (update/restore-deployment.mjs). What a restore
// replaces is the same too: the database and the vault key. The rest of the
// data volume (downloads, this server's access files) and the address settings
// in .env stay this server's own; the backup's address settings are never
// applied here (on systemd they are applied only when asked for).
//
//   inspect --input <file> --work <dir> [--passphrase-stdin]
//       Checks a console backup file (format, every frame's tag, versions) and
//       stages the database and vault key a restore would install in
//       <dir>/staged, with <dir>/staged.json naming their hashes. Nothing of the
//       running server is read or changed. The passphrase is the first line of
//       stdin (--passphrase-stdin) or MULTITAP_BACKUP_PASSPHRASE; it is never
//       printed.
//   apply --work <dir> --data <data dir> [--origin <url>]
//       With the server stopped: checks the staged files against staged.json,
//       adds the restore marker the restored server reads at its first start,
//       and replaces the database and vault key in the data directory.
//   inspect-volume --work <dir>
//       Checks the database and vault key taken out of a `multitap backup`
//       volume copy into <dir>/volume (schema not newer than this image).
//
// Output: `key=value` lines (ok=1 or ok=0 and code=<fixed code>), nothing else.
// No secret, path from a file, SQL text or row content is ever printed.
import { readFileSync } from 'node:fs';
import { mkdir, rm, writeFile, readFile, lstat, copyFile } from 'node:fs/promises';
import { randomUUID } from 'node:crypto';
import { DatabaseSync } from 'node:sqlite';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { BackupError, readBackupHeader, readBackup, backupPassphrase, backupMaxDatabaseBytes } from './backup-format.mjs';
import { backupCounts } from './backup-database.mjs';
import { spawnBackupHelper, writeRestoreMarker, defaultBackupHelperPath } from './backup.mjs';
import { Store } from './store.mjs';
import { Vault } from './vault.mjs';
import { compareVersion } from '../update/protocol.mjs';
import { ServerRestoreDeployment, restoreStagedFiles } from '../update/restore-deployment.mjs';

export const VERSION = JSON.parse(readFileSync(new URL('../package.json', import.meta.url), 'utf8')).version;
export const restoreCliActor = 'multitap restore';
const uuid = /^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$/, hex = /^[a-f0-9]{64}$/;
const originPattern = /^https?:\/\/[^\s\x00-\x20\x7f\\@?#%]{1,500}$/;
const fail = code => { throw new BackupError(code); };
const exact = (value, keys) => value !== null && typeof value === 'object' && !Array.isArray(value) && Object.keys(value).length === keys.length && keys.every(key => Object.hasOwn(value, key));
const currentSchemaVersion = () => { const store = new Store(':memory:'); try { return Number(store.db.prepare('PRAGMA user_version').get().user_version); } finally { store.close(); } };
const deployment = (work, data = work) => new ServerRestoreDeployment({ access: { data }, directory: work });

// ---- inspect: a console backup file ------------------------------------
export async function inspectBackup({ input, work, passphrase, version = VERSION, now = Date.now, helperPath = defaultBackupHelperPath, helperTimeoutMs = 300000 }) {
  try { backupPassphrase(passphrase).fill(0); } catch (error) { throw error instanceof BackupError ? error : new BackupError('backup_passphrase_invalid'); }
  const header = await readBackupHeader(input);
  // A backup is restored on the same or a newer server version, never an older one.
  if (compareVersion(header.serverVersion, version) > 0) fail('backup_from_newer_server');
  const scratch = join(work, 'scratch'), staged = join(work, 'staged'), planPath = join(work, 'staged.json');
  const clear = () => Promise.all([staged, planPath].map(path => rm(path, { recursive: true, force: true })));
  await clear(); await rm(scratch, { recursive: true, force: true });
  await mkdir(scratch, { recursive: true, mode: 0o700 });
  try {
    const { manifest } = await readBackup({ path: input, passphrase, databasePath: join(scratch, 'uploaded.sqlite') });
    await mkdir(staged, { mode: 0o700 });
    const result = await spawnBackupHelper({ mode: 'rebuild', source: join(scratch, 'uploaded.sqlite'), work: join(scratch, 'migrate.sqlite'), output: join(staged, 'broker.sqlite'), vaultKey: manifest.vaultKey, now: now() }, { helperPath, timeoutMs: helperTimeoutMs });
    await writeFile(join(staged, 'vault.key'), Buffer.from(manifest.vaultKey, 'base64'), { flag: 'wx', mode: 0o600 });
    const files = deployment(work), database = await files.hashOf(join(staged, 'broker.sqlite'), backupMaxDatabaseBytes), key = await files.hashOf(join(staged, 'vault.key'), 32);
    const plan = { schema: 1, operationId: randomUUID(), schemaVersion: result.currentSchemaVersion, database, vault: { sha256: key.sha256 },
      backup: { createdAt: header.createdAt, serverVersion: header.serverVersion, origin: manifest.origin } };
    await writeFile(planPath, JSON.stringify(plan), { flag: 'wx', mode: 0o600 });
    return { kind: 'console', createdAt: header.createdAt, serverVersion: header.serverVersion, origin: manifest.origin, deploymentKind: manifest.deploymentKind, counts: result.counts, proxmox: result.proxmox,
      unreadableSecrets: result.unreadableSecrets, schemaVersion: result.schemaVersion, currentSchemaVersion: result.currentSchemaVersion, migrated: result.migrated === true, ignoredObjects: result.ignoredObjects, operationId: plan.operationId };
  } catch (error) { await clear(); throw error; }
  finally { await rm(scratch, { recursive: true, force: true, maxRetries: 3, retryDelay: 50 }); }
}

// ---- apply: the staged files into the data directory -------------------
async function readPlan(path) {
  let plan;
  try { plan = JSON.parse(await readFile(path, 'utf8')); } catch { fail('restore_staging_invalid'); }
  if (!exact(plan, ['schema', 'operationId', 'schemaVersion', 'database', 'vault', 'backup']) || plan.schema !== 1 || !uuid.test(plan.operationId ?? '') || !Number.isSafeInteger(plan.schemaVersion) ||
      !exact(plan.database, ['size', 'sha256']) || !hex.test(plan.database.sha256 ?? '') || !Number.isSafeInteger(plan.database.size) || !exact(plan.vault, ['sha256']) || !hex.test(plan.vault.sha256 ?? '') ||
      !exact(plan.backup, ['createdAt', 'serverVersion', 'origin']) || typeof plan.backup.createdAt !== 'string' || typeof plan.backup.serverVersion !== 'string') fail('restore_staging_invalid');
  return plan;
}
// The server must be stopped (the multitap command stops it first). Order as
// restore-deployment.mjs: the old write-ahead files go, then the vault key and
// the database are each written beside the live name and renamed over it. The
// safety copy is the multitap command's: the whole volume, taken just before.
export async function applyStaged({ work, data, origin = null, now = Date.now, version = VERSION }) {
  const plan = await readPlan(join(work, 'staged.json')), files = deployment(work, data), staged = join(work, 'staged');
  for (const name of restoreStagedFiles) {
    let actual;
    try { actual = await files.hashOf(join(staged, name), name === 'vault.key' ? 32 : backupMaxDatabaseBytes); } catch { fail('restore_staging_invalid'); }
    const expected = name === 'vault.key' ? { sha256: plan.vault.sha256, size: 32 } : plan.database;
    if (actual.sha256 !== expected.sha256 || actual.size !== expected.size) fail('restore_staging_changed');
  }
  await files.checkDatabase(join(staged, 'broker.sqlite'), plan.schemaVersion);
  if (plan.schemaVersion !== currentSchemaVersion()) fail('restore_staging_invalid');
  await files.plainDirectory(data);
  // The same marker the console's restore adds (backup.mjs apply). The access
  // settings stay this server's: `access: null`.
  const marker = { operationId: plan.operationId, restoredAt: new Date(now()).toISOString(), backup: plan.backup, actor: restoreCliActor,
    replaced: { origin: typeof origin === 'string' && originPattern.test(origin) ? origin : null, serverVersion: version }, access: null };
  writeRestoreMarker(join(staged, 'broker.sqlite'), marker);
  const job = { operation: work, operationId: plan.operationId };
  await files.remove('broker.sqlite-wal'); await files.remove('broker.sqlite-shm');
  await files.place(job, 'staged', 'vault.key'); await files.place(job, 'staged', 'broker.sqlite');
  return { operationId: plan.operationId };
}

// ---- inspect-volume: a `multitap backup` copy ----------------------------
// The copy is this server's own command's output, the whole volume as it was.
// Its database is checked on a scratch copy and brought up to this image's
// schema there, as the server would at its next start.
export async function inspectVolume({ work }) {
  const volume = join(work, 'volume'), scratch = join(work, 'scratch');
  const regular = async (path, optional = false) => {
    try { const stat = await lstat(path); if (!stat.isFile() || stat.isSymbolicLink()) fail('backup_file_invalid'); return stat; }
    catch (error) { if (error instanceof BackupError) throw error; if (optional && error.code === 'ENOENT') return null; fail('backup_file_invalid'); }
  };
  const database = await regular(join(volume, 'broker.sqlite')), key = await regular(join(volume, 'vault.key')), wal = await regular(join(volume, 'broker.sqlite-wal'), true);
  if (key.size !== 32 || database.size < 512) fail('backup_file_invalid');
  if (database.size > 64 * 1024 * 1024 * 1024) fail('backup_too_large');
  await rm(scratch, { recursive: true, force: true }); await mkdir(scratch, { recursive: true, mode: 0o700 });
  try {
    const check = join(scratch, 'check.sqlite');
    await copyFile(join(volume, 'broker.sqlite'), check);
    if (wal) await copyFile(join(volume, 'broker.sqlite-wal'), check + '-wal');
    let version;
    const db = new DatabaseSync(check, { allowExtension: false });
    try {
      db.exec('PRAGMA trusted_schema=OFF;');
      const rows = db.prepare('PRAGMA quick_check(1)').all();
      if (rows.length !== 1 || Object.values(rows[0])[0] !== 'ok') fail('backup_database_invalid');
      version = Number(db.prepare('PRAGMA user_version').get().user_version);
    } catch (error) { if (error instanceof BackupError) throw error; fail('backup_database_invalid'); }
    finally { db.close(); }
    const current = currentSchemaVersion();
    if (!Number.isSafeInteger(version) || version < 1) fail('backup_database_invalid');
    if (version > current) fail('backup_schema_newer');
    let counts, proxmox = { configured: false, readable: false, endpoint: null };
    let store;
    try {
      store = new Store(check);
      counts = backupCounts(store.db);
      const sealed = store.setting('proxmox');
      if (sealed) {
        let value = null;
        try { value = new Vault(Buffer.from(await readFile(join(volume, 'vault.key')))).open(sealed); } catch { value = null; }
        proxmox = { configured: true, readable: value !== null, endpoint: typeof value?.endpoint === 'string' && /^https:\/\/[^\s\x00-\x20\x7f\\@?#%]{1,300}$/.test(value.endpoint) ? value.endpoint : null };
      }
    } catch { fail('backup_database_invalid'); }
    finally { store?.close(); }
    return { kind: 'volume', schemaVersion: version, currentSchemaVersion: current, migrated: version < current, counts, proxmox };
  } finally { await rm(scratch, { recursive: true, force: true, maxRetries: 3, retryDelay: 50 }); }
}

// ---- the command line ----------------------------------------------------
const safe = value => String(value ?? '').replace(/[^A-Za-z0-9 .:/_+@-]/g, '').slice(0, 300);
export function formatResult(result) {
  const lines = ['ok=1'];
  const put = (key, value) => { if (value !== undefined && value !== null) lines.push(key + '=' + safe(value)); };
  put('kind', result.kind); put('createdAt', result.createdAt); put('serverVersion', result.serverVersion); put('origin', result.origin); put('deploymentKind', result.deploymentKind);
  put('schemaVersion', result.schemaVersion); put('currentSchemaVersion', result.currentSchemaVersion); if (result.migrated !== undefined) put('migrated', result.migrated ? 1 : 0); put('ignoredObjects', result.ignoredObjects);
  if (result.counts) for (const key of ['accounts', 'admins', 'desktops', 'assignments', 'hosts']) put(key, Number(result.counts[key]));
  if (result.proxmox) { put('proxmox', !result.proxmox.configured ? 'none' : result.proxmox.readable ? 'readable' : 'unreadable'); put('proxmoxEndpoint', result.proxmox.endpoint); }
  put('unreadableSecrets', result.unreadableSecrets); put('operationId', result.operationId);
  return lines.join('\n') + '\n';
}
const codeOf = error => error instanceof BackupError && /^(?:backup|restore)_[a-z_]{1,60}$/.test(error.code) ? error.code : /^restore_[a-z_]{1,80}$/.test(error?.message ?? '') ? error.message : 'restore_failed';
async function readSecretLine(stream) {
  const chunks = []; let size = 0;
  for await (const chunk of stream) { size += chunk.length; if (size > 4096) fail('backup_passphrase_invalid'); chunks.push(chunk); }
  const text = Buffer.concat(chunks).toString('utf8'), end = text.indexOf('\n');
  return (end < 0 ? text : text.slice(0, end)).replace(/\r$/, '');
}
const usage = 'usage: restore-cli.mjs inspect --input <file> --work <dir> [--passphrase-stdin]\n       restore-cli.mjs apply --work <dir> --data <dir> [--origin <url>]\n       restore-cli.mjs inspect-volume --work <dir>\n';
export async function main(argv, { stdin = process.stdin, stdout = process.stdout, env = process.env, ...options } = {}) {
  const [command, ...rest] = argv, flags = {};
  for (let index = 0; index < rest.length; index++) {
    const name = rest[index];
    if (name === '--passphrase-stdin') flags.passphraseStdin = true;
    else if (['--input', '--work', '--data', '--origin'].includes(name) && index + 1 < rest.length) flags[name.slice(2)] = rest[++index];
    else { stdout.write(usage); return 64; }
  }
  if (!['inspect', 'apply', 'inspect-volume'].includes(command) || !flags.work || (command === 'inspect' && !flags.input) || (command === 'apply' && !flags.data)) { stdout.write(usage); return 64; }
  try {
    let result;
    if (command === 'inspect') {
      const passphrase = flags.passphraseStdin ? await readSecretLine(stdin) : env.MULTITAP_BACKUP_PASSPHRASE;
      result = await inspectBackup({ input: resolve(flags.input), work: resolve(flags.work), passphrase, ...options });
    } else if (command === 'apply') result = { kind: 'applied', ...(await applyStaged({ work: resolve(flags.work), data: resolve(flags.data), origin: flags.origin ?? null, ...options })) };
    else result = await inspectVolume({ work: resolve(flags.work) });
    stdout.write(formatResult(result));
    return 0;
  } catch (error) { stdout.write('ok=0\ncode=' + codeOf(error) + '\n'); return 2; }
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  process.umask(0o077);
  process.exitCode = await main(process.argv.slice(2));
}
