import test from 'node:test';
import assert from 'node:assert/strict';
import { DatabaseSync } from 'node:sqlite';
import { copyFileSync, readFileSync, writeFileSync, existsSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { snapshotDatabase, sanitizeSnapshot, rebuildFromBackup, backupIncludedTables, backupExcludedTables, backupExcludedSettings, restorePendingSetting } from '../src/backup-database.mjs';
import { BackupError } from '../src/backup-format.mjs';
import { seedServer, temporaryDirectory, travellingRows, tableCount, fixtureProxmox, fixtureTotp, ids } from './backup-fixture.mjs';

const code = expected => error => error instanceof BackupError && error.code === expected;
const schema = path => { const db = new DatabaseSync(path, { readOnly: true }); try { return db.prepare("SELECT type,name,sql FROM sqlite_schema WHERE name NOT LIKE 'sqlite_%' ORDER BY type,name").all().map(row => ({ ...row })); } finally { db.close(); } };
// A cleaned snapshot of a seeded server, as an export makes it.
async function snapshot(t, options) {
  const directory = temporaryDirectory(t), server = seedServer(directory, options), path = join(directory, 'snapshot.sqlite');
  await snapshotDatabase(server.store.db, path);
  const info = sanitizeSnapshot(path, { now: 1794300999000 });
  const key = Buffer.from(server.vault.key); server.store.close();
  return { directory, path, key, info, server };
}
let serial = 0;
const rebuild = (state, source = state.path, extra = {}) => { const n = ++serial; return { output: join(state.directory, `out-${n}.sqlite`), summary: rebuildFromBackup({ source, work: join(state.directory, `work-${n}.sqlite`), output: join(state.directory, `out-${n}.sqlite`), vaultKey: state.key, ...extra }) }; };
function variant(state, change) {
  const path = join(state.directory, `variant-${++serial}.sqlite`); copyFileSync(state.path, path);
  const db = new DatabaseSync(path); try { change(db); } finally { db.close(); }
  return path;
}

test('every table of this server is either copied or deliberately left out', t => {
  const directory = temporaryDirectory(t), store = new Store(join(directory, 'fresh.sqlite'));
  const tables = store.db.prepare("SELECT name FROM sqlite_schema WHERE type='table' AND name NOT LIKE 'sqlite_%'").all().map(row => row.name).sort(); store.close();
  assert.deepEqual([...backupIncludedTables, ...backupExcludedTables].sort(), tables);
  assert.equal(new Set([...backupIncludedTables, ...backupExcludedTables]).size, tables.length);
});

test('the snapshot is taken online and cleaned: sign-ins, codes, operations in flight and diagnostics do not travel', async t => {
  const directory = temporaryDirectory(t), server = seedServer(directory), path = join(directory, 'snapshot.sqlite');
  await snapshotDatabase(server.store.db, path);
  // The live database is untouched and still in use while the copy is cleaned.
  server.store.audit('written_after_snapshot', 'test', '');
  const info = sanitizeSnapshot(path, { now: 1794300999000 });
  assert.equal(info.schemaVersion, 19);
  assert.deepEqual(info.counts, { accounts: 4, admins: 2, desktops: 4, hosts: 4, assignments: 4, adminNames: ['ownera', 'seconda'] });
  for (const name of backupExcludedTables) assert.equal(tableCount(path, name), 0, name);
  for (const name of ['sessions', 'client_diagnostics', 'power_tasks']) assert.ok(tableCount(server.path, name) > 0, 'the live ' + name + ' stays');
  const db = new DatabaseSync(path, { readOnly: true });
  try {
    for (const key of backupExcludedSettings) assert.equal(db.prepare('SELECT 1 FROM settings WHERE key=?').get(key), undefined, key);
    assert.equal(db.prepare('SELECT count(*) AS n FROM usage_logins WHERE ended_at IS NULL OR token_hash IS NOT NULL').get().n, 0);
    assert.equal(db.prepare('SELECT count(*) AS n FROM usage_connections WHERE ended_at IS NULL').get().n, 0);
    assert.equal(db.prepare("SELECT count(*) AS n FROM audit_events WHERE event='written_after_snapshot'").get().n, 0);
    assert.equal(db.prepare('PRAGMA journal_mode').get().journal_mode, 'delete');
  } finally { db.close(); }
  // Removed rows are not left in the file's free pages either.
  const bytes = readFileSync(path).toString('latin1');
  assert.equal(bytes.includes(server.tokens.live), false); assert.equal(bytes.includes(server.tokens.admin), false);
  assert.equal(existsSync(path + '-wal'), false);
  server.store.close();
});

test('a restore copies every travelling row exactly into a database made by this server, and sealed secrets open', async t => {
  const state = await snapshot(t), { output, summary } = rebuild(state);
  assert.deepEqual(travellingRows(output), travellingRows(state.path));
  assert.deepEqual(summary.counts, state.info.counts);
  assert.deepEqual({ schemaVersion: summary.schemaVersion, migrated: summary.migrated, ignoredObjects: summary.ignoredObjects, unreadableSecrets: summary.unreadableSecrets }, { schemaVersion: 19, migrated: false, ignoredObjects: 0, unreadableSecrets: 0 });
  assert.deepEqual(summary.proxmox, { configured: true, readable: true, endpoint: fixtureProxmox.endpoint });
  for (const name of backupExcludedTables) assert.equal(tableCount(output, name), 0, name);
  // The schema objects are this server's own, text for text.
  const fresh = join(state.directory, 'fresh.sqlite'); new Store(fresh).close();
  assert.deepEqual(schema(output), schema(fresh));
  // What the vault sealed is usable with the key that travelled.
  const store = new Store(output), vault = new Vault(state.key);
  try {
    assert.deepEqual(vault.open(store.setting('proxmox')), { ...fixtureProxmox, secret: fixtureProxmox.secret + 'a' });
    assert.equal(vault.open(store.db.prepare('SELECT factor_ciphertext AS v FROM account_mfa WHERE user_id=?').get(ids.second).v).secret, fixtureTotp);
    assert.equal(vault.open(store.db.prepare('SELECT credentials_sealed AS v FROM host_requests').get().v).credentials.hostToken, 'token-a');
    assert.equal(store.findUser('ownera').is_primary_admin, 1);
    assert.equal(store.db.prepare('SELECT count(*) AS n FROM sessions').get().n, 0);
    // The triggers put back afterwards work: a new sign-in is recorded again.
    store.db.prepare("INSERT INTO sessions(token_hash,user_id,created_at,expires_at,purpose,issuer) VALUES('" + 'c'.repeat(64) + "',?,1,2,'client','legacy')").run(ids.member);
    assert.equal(store.db.prepare("SELECT count(*) AS n FROM usage_logins WHERE token_hash='" + 'c'.repeat(64) + "'").get().n, 1);
    const next = store.db.prepare('INSERT INTO usage_connections(session_id,user_id,desktop_id,desktop_name,mode,started_at,last_seen_at) VALUES(?,?,?,?,?,?,?)').run('99999999-0000-4000-8000-000000000001', ids.member, 'pve-101', 'x', 'stream', 1, 1);
    assert.equal(Number(next.lastInsertRowid), 14, 'ids keep growing from where the backup stopped');
  } finally { store.close(); }
});

test('a restore marker is written only when asked, and the wrong vault key is reported, not hidden', async t => {
  const state = await snapshot(t);
  const marked = rebuild(state, state.path, { marker: JSON.stringify({ operationId: 'x' }) });
  const db = new DatabaseSync(marked.output, { readOnly: true });
  try { assert.equal(db.prepare('SELECT value FROM settings WHERE key=?').get(restorePendingSetting).value, '{"operationId":"x"}'); } finally { db.close(); }
  assert.deepEqual(travellingRows(marked.output, { skipSettings: [restorePendingSetting] }), travellingRows(state.path));
  const other = rebuild({ ...state, key: Buffer.alloc(32, 9) });
  assert.deepEqual(other.summary.proxmox, { configured: true, readable: false, endpoint: null }); assert.equal(other.summary.unreadableSecrets, 3);
});

test('sign-ins never come back, even from a file that still carries them', async t => {
  const directory = temporaryDirectory(t), server = seedServer(directory), raw = join(directory, 'raw.sqlite');
  await snapshotDatabase(server.store.db, raw); const key = Buffer.from(server.vault.key); server.store.close();
  const db = new DatabaseSync(raw); db.exec('PRAGMA journal_mode=DELETE'); db.close();
  assert.ok(tableCount(raw, 'sessions') > 0);
  const { output } = rebuild({ directory, key }, raw);
  for (const name of backupExcludedTables) assert.equal(tableCount(output, name), 0, name);
  const out = new DatabaseSync(output, { readOnly: true });
  try {
    assert.equal(out.prepare("SELECT 1 FROM settings WHERE key='initial_setup_code'").get(), undefined);
    assert.equal(out.prepare('SELECT count(*) AS n FROM usage_logins WHERE token_hash IS NOT NULL OR ended_at IS NULL').get().n, 0);
  } finally { out.close(); }
});

test('views, virtual tables, generated columns and odd tables are refused; foreign triggers and tables are neither run nor copied', async t => {
  const state = await snapshot(t);
  assert.throws(() => rebuild(state, variant(state, db => db.exec("CREATE VIEW spy AS SELECT * FROM users"))), code('backup_database_unsupported'));
  assert.throws(() => rebuild(state, variant(state, db => db.exec("ALTER TABLE pc_wake RENAME TO pc_wake_real; CREATE VIEW pc_wake AS SELECT * FROM pc_wake_real"))), code('backup_database_unsupported'));
  assert.throws(() => rebuild(state, variant(state, db => db.exec("CREATE VIRTUAL TABLE notes USING fts5(body)"))), code('backup_database_unsupported'));
  assert.throws(() => rebuild(state, variant(state, db => db.exec("ALTER TABLE users ADD COLUMN shadow TEXT GENERATED ALWAYS AS (upper(username)) VIRTUAL"))), code('backup_database_unsupported'));
  assert.throws(() => rebuild(state, variant(state, db => db.exec("DROP TABLE pc_wake; CREATE TABLE pc_wake (desktop_id TEXT PRIMARY KEY, host_id TEXT NOT NULL, adapters TEXT NOT NULL, reported_at INTEGER NOT NULL) WITHOUT ROWID"))), code('backup_database_unsupported'));
  // A trigger that would make an account an administrator whenever anything is
  // written, a changed copy of one of this server's own triggers, an unknown
  // table and an unknown index: none of them reaches the restored database.
  const hostile = variant(state, db => db.exec(`
    CREATE TRIGGER promote AFTER INSERT ON audit_events BEGIN UPDATE users SET role='admin',disabled=0 WHERE username='손님a'; END;
    DROP TRIGGER usage_login_started;
    CREATE TRIGGER usage_login_started AFTER INSERT ON sessions BEGIN UPDATE users SET role='admin' WHERE role='user'; END;
    CREATE TABLE loot (id INTEGER PRIMARY KEY, body TEXT); INSERT INTO loot(body) VALUES('x');
    CREATE INDEX loot_body ON loot(body);`));
  const { output, summary } = rebuild(state, hostile);
  assert.equal(summary.ignoredObjects, 4);
  const fresh = join(state.directory, 'fresh.sqlite'); new Store(fresh).close();
  assert.deepEqual(schema(output), schema(fresh));
  assert.deepEqual(travellingRows(output), travellingRows(state.path));
  const store = new Store(output);
  try {
    store.audit('anything', 'test', ''); store.db.prepare("INSERT INTO sessions(token_hash,user_id,created_at,expires_at,purpose,issuer) VALUES('" + 'd'.repeat(64) + "',?,1,2,'client','legacy')").run(ids.member);
    assert.deepEqual(store.db.prepare("SELECT username,role,disabled FROM users WHERE role='admin' ORDER BY username").all().map(row => row.username), ['ownera', 'seconda']);
  } finally { store.close(); }
});

test('a damaged file, a write-ahead-log file, a newer schema, broken references and a backup without an administrator are refused', async t => {
  const state = await snapshot(t);
  const junk = join(state.directory, 'junk.sqlite'); writeFileSync(junk, Buffer.alloc(8192, 1));
  assert.throws(() => rebuild(state, junk), code('backup_database_invalid'));
  const bytes = readFileSync(state.path), damaged = join(state.directory, 'damaged.sqlite');
  for (let offset = 4096 * 3; offset < 4096 * 5; offset++) bytes[offset] = 0xa5;
  writeFileSync(damaged, bytes);
  assert.throws(() => rebuild(state, damaged), code('backup_database_invalid'));
  const cut = join(state.directory, 'cut.sqlite'); writeFileSync(cut, readFileSync(state.path).subarray(0, 4096 * 4 + 100));
  assert.throws(() => rebuild(state, cut), code('backup_database_invalid'));
  assert.throws(() => rebuild(state, variant(state, db => db.exec('PRAGMA journal_mode=WAL'))), code('backup_database_invalid'));
  assert.throws(() => rebuild(state, variant(state, db => db.exec('PRAGMA user_version=20'))), code('backup_schema_newer'));
  assert.throws(() => rebuild(state, variant(state, db => db.exec("PRAGMA foreign_keys=OFF; INSERT INTO assignments(user_id,desktop_id) VALUES('nobody','pve-101')"))), code('backup_database_invalid'));
  assert.throws(() => rebuild(state, variant(state, db => db.exec("UPDATE users SET role='user'"))), code('backup_no_administrator'));
  assert.throws(() => rebuild(state, variant(state, db => db.exec("PRAGMA foreign_keys=OFF; PRAGMA ignore_check_constraints=ON; UPDATE users SET role='root' WHERE username='membera'"))), code('backup_database_invalid'));
  assert.throws(() => rebuild(state, variant(state, db => db.exec('DROP TABLE settings'))), code('backup_database_invalid'));
});

test('an older schema is brought up to date by the normal migration code on a scratch copy', async t => {
  const state = await snapshot(t);
  const older = variant(state, db => db.exec(`PRAGMA foreign_keys=OFF;
    DROP TRIGGER users_mfa_authorization_revision; DROP TABLE mfa_recovery_codes; DROP TABLE mfa_failure_budgets; DROP TABLE account_mfa;
    ALTER TABLE users DROP COLUMN authorization_revision; PRAGMA user_version=18;`));
  const before = readFileSync(older);
  const { output, summary } = rebuild(state, older);
  assert.deepEqual({ schemaVersion: summary.schemaVersion, currentSchemaVersion: summary.currentSchemaVersion, migrated: summary.migrated }, { schemaVersion: 18, currentSchemaVersion: 19, migrated: true });
  assert.deepEqual(readFileSync(older), before, 'the uploaded file itself is not changed');
  const expected = travellingRows(state.path), actual = travellingRows(output);
  // What schema 19 added starts at its defaults; everything older is unchanged.
  expected.account_mfa = []; expected.mfa_recovery_codes = []; for (const row of expected.users) row.authorization_revision = 1;
  assert.deepEqual(actual, expected);
  const fresh = join(state.directory, 'fresh.sqlite'); new Store(fresh).close();
  assert.deepEqual(schema(output), schema(fresh));
});

test('the helper process answers one JSON line with a fixed code and nothing of the content', async t => {
  const state = await snapshot(t), helper = fileURLToPath(new URL('../src/backup-helper.mjs', import.meta.url));
  const run = input => { const done = spawnSync(process.execPath, [helper], { input: JSON.stringify(input), encoding: 'utf8', timeout: 60000 }); return { status: done.status, lines: done.stdout.trim().split('\n'), stderr: done.stderr }; };
  const good = run({ mode: 'rebuild', source: state.path, work: join(state.directory, 'hw.sqlite'), output: join(state.directory, 'ho.sqlite'), vaultKey: state.key.toString('base64') });
  assert.equal(good.lines.length, 1); const value = JSON.parse(good.lines[0]);
  assert.equal(value.ok, true); assert.equal(value.result.counts.accounts, 4); assert.ok(statSync(join(state.directory, 'ho.sqlite')).size > 4096);
  const text = JSON.stringify(value); for (const secret of [fixtureProxmox.secret, fixtureTotp, state.key.toString('base64'), 'scrypt-v1']) assert.equal(text.includes(secret), false);
  const bad = run({ mode: 'rebuild', source: variant(state, db => db.exec('CREATE VIEW spy AS SELECT password_hash FROM users')), work: join(state.directory, 'hw2.sqlite'), output: join(state.directory, 'ho2.sqlite'), vaultKey: state.key.toString('base64') });
  assert.deepEqual(JSON.parse(bad.lines[0]), { ok: false, code: 'backup_database_unsupported' }); assert.equal(bad.stderr, '');
  assert.deepEqual(JSON.parse(run({ mode: 'rebuild', source: join(state.directory, 'missing.sqlite'), work: 'w', output: join(state.directory, 'ho3.sqlite'), vaultKey: state.key.toString('base64') }).lines[0]), { ok: false, code: 'backup_helper_failed' });
  assert.deepEqual(JSON.parse(run({ mode: 'shell', command: 'id' }).lines[0]), { ok: false, code: 'backup_unavailable' });
  const cleaned = run({ mode: 'sanitize', path: variant(state, () => {}), now: 5 });
  assert.equal(JSON.parse(cleaned.lines[0]).result.schemaVersion, 19);
});
