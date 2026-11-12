// `sudo multitap restore` on a Docker install runs src/restore-cli.mjs in a
// one-off container. Here it runs in this process against temporary
// directories: server A makes the backup, server B is the one restored. The
// multitap command's own steps (stop, safety copy, start, put back) are bash
// and Docker and are not run here.
import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdirSync, readFileSync, writeFileSync, existsSync, readdirSync, copyFileSync } from 'node:fs';
import { join } from 'node:path';
import { createHash } from 'node:crypto';
import { Readable } from 'node:stream';
import { DatabaseSync } from 'node:sqlite';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { writeBackup } from '../src/backup-format.mjs';
import { snapshotDatabase, sanitizeSnapshot, backupExcludedSettings } from '../src/backup-database.mjs';
import { BackupService, dockerRestoreCommand } from '../src/backup.mjs';
import { inspectBackup, applyStaged, inspectVolume, main, VERSION, restoreCliActor } from '../src/restore-cli.mjs';
import { seedServer, temporaryDirectory, travellingRows, tableCount, fixtureProxmox } from './backup-fixture.mjs';

const origin = 'https://vdi-a.fixture.example', fast = { name: 'scrypt', N: 32768, r: 8, p: 1 };
const passphrase = '백업 암호는 열두 글자 이상 7!';
const sha = bytes => createHash('sha256').update(bytes).digest('hex');
const fileHash = path => sha(readFileSync(path));
const code = expected => error => { assert.equal(error.code, expected); return true; };

// Server A as a backup file, made the way the console makes one (snapshot,
// clean, encrypt), and server B, populated differently, as the data volume.
async function fixture(t, { serverVersion = VERSION, change = null } = {}) {
  // Hooks run in the order they were registered: what a test opens is closed before the directory goes.
  const closers = []; t.after(() => { for (const close of closers.reverse()) close(); });
  const root = temporaryDirectory(t, 'multitap-restore-cli-'), a = join(root, 'a'), data = join(root, 'data'), work = join(root, 'work');
  for (const path of [a, data, work]) mkdirSync(path);
  const seeded = seedServer(a, { seed: 'a' }), snapshot = join(root, 'snapshot.sqlite');
  await snapshotDatabase(seeded.store.db, snapshot); sanitizeSnapshot(snapshot);
  const key = Buffer.from(seeded.vault.key); seeded.store.close();
  if (change) { const db = new DatabaseSync(snapshot); try { db.exec(change); } finally { db.close(); } }
  const input = join(work, 'backup.mtbk');
  await writeBackup({ passphrase, createdAt: '2026-11-10T07:30:00.000Z', serverVersion, kdf: fast, databasePath: snapshot, outputPath: input,
    manifest: { schema: 1, origin, deploymentKind: 'docker', database: { schemaVersion: 19 }, vaultKey: key.toString('base64'), serverAccess: null, excluded: [] } });
  // B: its own accounts and secrets, a notice and live sign-ins (which do not
  // travel), its downloads and access files (which a restore leaves alone),
  // and write-ahead files of a server that was running.
  seedServer(data, { seed: 'b' }).store.close();
  mkdirSync(join(data, 'downloads')); writeFileSync(join(data, 'downloads', 'MultitapClient.exe'), 'client');
  writeFileSync(join(data, 'server-access.identity'), 'f'.repeat(64)); writeFileSync(join(data, 'broker.sqlite-wal'), 'stale wal'); writeFileSync(join(data, 'broker.sqlite-shm'), 'stale shm');
  return { root, a, data, work, input, snapshot, key, closers };
}
const before = data => Object.fromEntries(['broker.sqlite', 'vault.key', 'broker.sqlite-wal', 'broker.sqlite-shm'].map(name => [name, existsSync(join(data, name)) ? fileHash(join(data, name)) : null]));

test('a good console backup: checked and staged without touching the server, then applied: the backup\'s data, this server\'s files', async t => {
  const f = await fixture(t), untouched = before(f.data);
  const preview = await inspectBackup({ input: f.input, work: f.work, passphrase });
  assert.deepEqual(before(f.data), untouched, 'inspect reads nothing of the data volume and changes nothing');
  assert.deepEqual({ kind: preview.kind, createdAt: preview.createdAt, serverVersion: preview.serverVersion, origin: preview.origin },
    { kind: 'console', createdAt: '2026-11-10T07:30:00.000Z', serverVersion: VERSION, origin });
  assert.deepEqual({ accounts: preview.counts.accounts, admins: preview.counts.admins, desktops: preview.counts.desktops, assignments: preview.counts.assignments, hosts: preview.counts.hosts },
    { accounts: 4, admins: 2, desktops: 4, assignments: 4, hosts: 4 });
  assert.deepEqual(preview.proxmox, { configured: true, readable: true, endpoint: fixtureProxmox.endpoint });
  assert.deepEqual(readdirSync(f.work).sort(), ['backup.mtbk', 'staged', 'staged.json'], 'the decrypted scratch copy is gone');
  assert.deepEqual(readdirSync(join(f.work, 'staged')).sort(), ['broker.sqlite', 'vault.key']);
  const plan = JSON.parse(readFileSync(join(f.work, 'staged.json'), 'utf8'));
  assert.equal(JSON.stringify(plan).includes(f.key.toString('base64')), false, 'the plan names hashes, not the key');

  const travelled = travellingRows(f.snapshot, { skipSettings: backupExcludedSettings });
  const applied = await applyStaged({ work: f.work, data: f.data, origin: 'http://192.0.2.20:18480' });
  assert.equal(applied.operationId, plan.operationId);
  // Replaced: the database (rows of A exactly) and the vault key (A's).
  assert.deepEqual(travellingRows(join(f.data, 'broker.sqlite'), { skipSettings: [...backupExcludedSettings] }), travelled);
  assert.deepEqual(readFileSync(join(f.data, 'vault.key')), f.key);
  assert.equal(existsSync(join(f.data, 'broker.sqlite-wal')), false); assert.equal(existsSync(join(f.data, 'broker.sqlite-shm')), false);
  // Kept: the rest of the volume.
  assert.equal(readFileSync(join(f.data, 'downloads', 'MultitapClient.exe'), 'utf8'), 'client');
  assert.equal(readFileSync(join(f.data, 'server-access.identity'), 'utf8'), 'f'.repeat(64));
  // What does not travel is not there, on either side: B's sign-ins and setup code went with B's database.
  for (const name of ['sessions', 'session_activity', 'host_enrollments', 'power_tasks', 'client_diagnostics', 'notices']) assert.equal(tableCount(join(f.data, 'broker.sqlite'), name), 0, name);
  assert.equal(readdirSync(f.data).filter(name => name.includes('.restore-')).length, 0, 'no temporary file is left');

  // The restored server's first start: the record of this restore, the access settings kept.
  const store = new Store(join(f.data, 'broker.sqlite')), vault = Vault.open(join(f.data, 'vault.key'));
  f.closers.push(() => store.close());
  assert.equal(store.findUser('ownerb') ?? null, null); assert.ok(store.findUser('ownera'));
  assert.equal(store.setting('initial_setup_code') ?? null, null);
  assert.equal(vault.open(store.setting('proxmox')).secret, fixtureProxmox.secret + 'a', 'secrets open with the restored key');
  const service = new BackupService({ store, vault, version: VERSION, deploymentKind: 'docker', afterRestoreDelayMs: 60000 }); f.closers.push(() => service.close());
  const record = service.startup();
  assert.deepEqual({ operationId: record.operationId, backup: record.backup, actor: record.actor, access: record.access },
    { operationId: plan.operationId, backup: { createdAt: '2026-11-10T07:30:00.000Z', serverVersion: VERSION, origin }, actor: restoreCliActor, access: { state: 'kept' } });
  assert.equal(store.setting('restore_pending') ?? null, null, 'the marker is read once');
  assert.equal(store.db.prepare("SELECT count(*) AS n FROM audit_events WHERE event='backup_restored'").get().n, 1);
});

test('a wrong passphrase, a changed file and something that is not a backup are refused; nothing is staged', async t => {
  const f = await fixture(t), untouched = before(f.data);
  await assert.rejects(inspectBackup({ input: f.input, work: f.work, passphrase: passphrase + 'x' }), code('backup_unreadable'));
  await assert.rejects(inspectBackup({ input: f.input, work: f.work, passphrase: 'short' }), code('backup_passphrase_too_short'));
  await assert.rejects(inspectBackup({ input: f.input, work: f.work, passphrase: undefined }), code('backup_passphrase_invalid'));
  const bytes = readFileSync(f.input), tampered = join(f.work, 'tampered.mtbk'), cut = join(f.work, 'cut.mtbk'), other = join(f.work, 'other.mtbk');
  const changed = Buffer.from(bytes); changed[changed.length - 40] ^= 1; writeFileSync(tampered, changed);
  await assert.rejects(inspectBackup({ input: tampered, work: f.work, passphrase }), code('backup_unreadable'), 'the last frame\'s tag fails');
  const header = Buffer.from(bytes); header[20] ^= 1; writeFileSync(tampered, header);
  await assert.rejects(inspectBackup({ input: tampered, work: f.work, passphrase }), error => ['backup_file_invalid', 'backup_unreadable'].includes(error.code), 'the header is authenticated too');
  writeFileSync(cut, bytes.subarray(0, bytes.length - 1));
  await assert.rejects(inspectBackup({ input: cut, work: f.work, passphrase }), code('backup_file_invalid'));
  writeFileSync(other, Buffer.concat([Buffer.from([0x1f, 0x8b]), Buffer.alloc(200)]));
  await assert.rejects(inspectBackup({ input: other, work: f.work, passphrase }), code('backup_file_invalid'));
  assert.deepEqual(readdirSync(f.work).sort(), ['backup.mtbk', 'cut.mtbk', 'other.mtbk', 'tampered.mtbk'], 'no staged files, no scratch copy');
  await assert.rejects(applyStaged({ work: f.work, data: f.data }), code('restore_staging_invalid'), 'nothing staged, nothing applied');
  assert.deepEqual(before(f.data), untouched);
});

test('a backup from a newer server or with a newer database schema is refused', async t => {
  const newer = await fixture(t, { serverVersion: '99.0.0' });
  await assert.rejects(inspectBackup({ input: newer.input, work: newer.work, passphrase }), code('backup_from_newer_server'));
  const schema = await fixture(t, { change: 'PRAGMA user_version=20' });
  await assert.rejects(inspectBackup({ input: schema.input, work: schema.work, passphrase }), code('backup_schema_newer'));
  assert.deepEqual(readdirSync(schema.work), ['backup.mtbk']);
});

test('apply refuses staged files that changed after the check, and leaves the data as it was', async t => {
  const f = await fixture(t), untouched = before(f.data);
  await inspectBackup({ input: f.input, work: f.work, passphrase });
  const staged = join(f.work, 'staged', 'broker.sqlite'), original = readFileSync(staged);
  writeFileSync(staged, Buffer.concat([original.subarray(0, original.length - 1), Buffer.from([original.at(-1) ^ 1])]));
  await assert.rejects(applyStaged({ work: f.work, data: f.data }), code('restore_staging_changed'));
  writeFileSync(staged, original); writeFileSync(join(f.work, 'staged', 'vault.key'), Buffer.alloc(32, 9));
  await assert.rejects(applyStaged({ work: f.work, data: f.data }), code('restore_staging_changed'));
  writeFileSync(join(f.work, 'staged.json'), '{"schema":1}');
  await assert.rejects(applyStaged({ work: f.work, data: f.data }), code('restore_staging_invalid'));
  assert.deepEqual(before(f.data), untouched);
});

test('a multitap backup volume copy: checked on a scratch copy, refused when its schema is newer or a file is missing', async t => {
  const f = await fixture(t), volume = join(f.work, 'volume'); mkdirSync(volume);
  for (const name of ['broker.sqlite', 'vault.key']) copyFileSync(join(f.a, name), join(volume, name));
  const untouched = fileHash(join(volume, 'broker.sqlite'));
  const preview = await inspectVolume({ work: f.work });
  assert.deepEqual({ kind: preview.kind, accounts: preview.counts.accounts, desktops: preview.counts.desktops, proxmox: preview.proxmox.readable, migrated: preview.migrated }, { kind: 'volume', accounts: 4, desktops: 4, proxmox: true, migrated: false });
  assert.equal(fileHash(join(volume, 'broker.sqlite')), untouched, 'the copy itself is not opened for writing');
  assert.deepEqual(readdirSync(f.work).sort(), ['backup.mtbk', 'volume']);
  const db = new DatabaseSync(join(volume, 'broker.sqlite')); try { db.exec('PRAGMA user_version=20'); } finally { db.close(); }
  await assert.rejects(inspectVolume({ work: f.work }), code('backup_schema_newer'));
  writeFileSync(join(volume, 'vault.key'), 'short');
  await assert.rejects(inspectVolume({ work: f.work }), code('backup_file_invalid'));
});

test('the command line prints fixed key=value lines and never a secret', async t => {
  const f = await fixture(t);
  const run = async (argv, { input = '', env = {} } = {}) => {
    let text = ''; const stdout = { write: value => { text += value; } };
    const status = await main(argv, { stdin: Readable.from([Buffer.from(input)]), stdout, env });
    return { status, text, values: Object.fromEntries(text.trim().split('\n').map(line => [line.slice(0, line.indexOf('=')), line.slice(line.indexOf('=') + 1)])) };
  };
  assert.equal((await run([])).status, 64);
  assert.equal((await run(['inspect', '--work', f.work])).status, 64);
  assert.equal((await run(['apply', '--work', f.work, '--unknown'])).status, 64);
  const wrong = await run(['inspect', '--input', f.input, '--work', f.work, '--passphrase-stdin'], { input: 'not the right one at all\n' });
  assert.deepEqual({ status: wrong.status, text: wrong.text }, { status: 2, text: 'ok=0\ncode=backup_unreadable\n' });
  const fromEnv = await run(['inspect', '--input', f.input, '--work', f.work], { env: { MULTITAP_BACKUP_PASSPHRASE: passphrase } });
  assert.equal(fromEnv.values.ok, '1');
  const good = await run(['inspect', '--input', f.input, '--work', f.work, '--passphrase-stdin'], { input: passphrase + '\r\n' });
  assert.equal(good.status, 0);
  assert.deepEqual({ ...good.values, operationId: undefined }, { ok: '1', kind: 'console', createdAt: '2026-11-10T07:30:00.000Z', serverVersion: VERSION, origin, deploymentKind: 'docker', schemaVersion: '19', currentSchemaVersion: '19', migrated: '0',
    ignoredObjects: '0', accounts: '4', admins: '2', desktops: '4', assignments: '4', hosts: '4', proxmox: 'readable', proxmoxEndpoint: fixtureProxmox.endpoint, unreadableSecrets: '0', operationId: undefined });
  for (const secret of [passphrase, fixtureProxmox.secret, f.key.toString('base64'), f.key.toString('hex')]) assert.equal(good.text.includes(secret), false);
  const applied = await run(['apply', '--work', f.work, '--data', f.data, '--origin', 'http://192.0.2.20:18480']);
  assert.deepEqual({ status: applied.status, ok: applied.values.ok, kind: applied.values.kind, operationId: applied.values.operationId }, { status: 0, ok: '1', kind: 'applied', operationId: good.values.operationId });
  assert.deepEqual(readFileSync(join(f.data, 'vault.key')), f.key);
});

test('the backup status tells a Docker install without an updater to use the multitap command; other installs are unchanged', async t => {
  const closers = []; t.after(() => { for (const close of closers.reverse()) close(); });
  const directory = temporaryDirectory(t), seeded = seedServer(directory); closers.push(() => seeded.store.close());
  const status = async options => { const service = new BackupService({ store: seeded.store, vault: seeded.vault, directory: join(directory, 'backup-work'), version: VERSION, ...options }); closers.push(() => service.close()); return (await service.status({ owner: 'x' })).restore; };
  const docker = await status({ deploymentKind: 'docker' });
  assert.deepEqual({ supported: docker.supported, state: docker.state, command: docker.command }, { supported: false, state: 'docker-command', command: 'sudo multitap restore' });
  assert.equal(dockerRestoreCommand, 'sudo multitap restore');
  for (const kind of ['systemd', 'unknown', 'external']) {
    const other = await status({ deploymentKind: kind });
    assert.deepEqual({ supported: other.supported, state: other.state, command: other.command }, { supported: false, state: 'unavailable', command: undefined }, kind);
  }
  // A Docker install whose updater restores (a root agent beside it) keeps the console path.
  const engine = { status: async () => ({ protocol: 'server-restore-v1', supported: true, state: 'idle', busy: false }) };
  const managed = await status({ deploymentKind: 'docker', restoreClient: engine, accessRuntime: { hold() {}, release() {} } });
  assert.deepEqual({ supported: managed.supported, state: managed.state, command: managed.command }, { supported: true, state: 'idle', command: undefined });
});
