// 백업·복원 through the broker's real HTTP routes. The privileged updater is
// replaced by its own engine and file code running in this process against a
// temporary data directory; "stopping" and "starting" the service closes and
// reopens the broker on that directory. No Linux service manager, Unix socket
// or production data is involved.
import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdirSync, readFileSync, writeFileSync, existsSync, readdirSync } from 'node:fs';
import { join } from 'node:path';
import { createHash } from 'node:crypto';
import { DatabaseSync } from 'node:sqlite';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer, VERSION } from '../src/http.mjs';
import { adminRoutes } from '../src/admin-http.mjs';
import { HttpError } from '../src/errors.mjs';
import { hashPassword, initialPasswordHash, newToken, tokenHash } from '../src/security.mjs';
import { writeBackup, backupMagic } from '../src/backup-format.mjs';
import { snapshotDatabase, sanitizeSnapshot, backupIncludedTables } from '../src/backup-database.mjs';
import { BackupService } from '../src/backup.mjs';
import { normalizeServerAccess } from '../update/server-access-policy.mjs';
import { ServerRestoreEngine } from '../update/restore-engine.mjs';
import { ServerRestoreDeployment } from '../update/restore-deployment.mjs';
import { seedServer, temporaryDirectory, travellingRows, fixtureProxmox, fixtureTotp, ids } from './backup-fixture.mjs';

const origin = 'https://backup.fixture.example', fast = { name: 'scrypt', N: 32768, r: 8, p: 1 };
const password = { a: 'Owner-of-server-A-71!', b: 'Owner-of-server-B-82!' }, passphrase = '백업 암호는 열두 글자 이상';
const sha = bytes => createHash('sha256').update(bytes).digest('hex');
const accessOf = host => normalizeServerAccess({ protocol: 'server-access-v1', primaryOrigin: `https://${host}`, https: { enabled: true, origin: `https://${host}`, mode: 'proxy', listenHost: '0.0.0.0', listenPort: 18480 }, http: { enabled: false, origin: null, listenHost: '0.0.0.0', listenPort: 18481 }, trustedProxies: ['192.0.2.10'] });
const hashes = {};
const hashOf = async seed => hashes[seed] ??= await hashPassword(password[seed]);

// One installation: a data directory, the broker running on it, and (when
// asked) an updater that can restore into it.
async function site(t, { seed = 'a', fresh = false, updater = true, options = {}, proxmoxFails = false, healthy = () => true } = {}) {
  // Hooks run in the order they were registered: the broker must be closed
  // before its directory is removed.
  const state = {}; t.after(() => state.stop?.());
  const root = temporaryDirectory(t, 'multitap-backup-http-'), data = join(root, 'data'); mkdirSync(data);
  if (fresh) {
    const store = new Store(join(data, 'broker.sqlite')); Vault.open(join(data, 'vault.key'));
    const id = store.addUser({ name: 'admin', role: 'admin', passwordHash: await initialPasswordHash() });
    store.db.prepare('UPDATE users SET must_change_credentials=1 WHERE id=?').run(id); store.close();
  } else {
    // The fixture's power task stays as a row (it must not travel) but is not in flight: a restore waits for one that is.
    const seeded = seedServer(data, { seed, passwordHash: await hashOf(seed) }); seeded.store.db.prepare('UPDATE power_tasks SET active=0').run(); seeded.store.close();
  }
  Object.assign(state, { root, data, seed, logs: [], announced: [], proxmoxCalls: [], accessCalls: [], restoreCalls: [], starts: 0, cookie: '', csrf: '', bodies: [], accessState: { state: 'idle', operationId: null } });
  const accessClient = { status: async () => ({ protocol: 'server-access-v1', managed: true, applySupported: true, deploymentKind: 'systemd', revision: 'c'.repeat(64), config: null, pendingConfig: null, ...state.accessState }),
    test: async (input, context) => { state.accessCalls.push({ kind: 'test', input, context }); return { testId: 'b'.repeat(64), baseRevision: input.baseRevision }; },
    save: async (input, context) => { state.accessCalls.push({ kind: 'save', input, context: { managementOrigin: context.managementOrigin } }); state.accessState = { state: 'succeeded', operationId: context.fence.operationId }; return { accepted: true, operationId: context.fence.operationId, state: 'waiting_idle' }; } };
  const proxmox = { call: async (config, path) => { state.proxmoxCalls.push({ endpoint: config.endpoint, secret: config.secret, path }); if (proxmoxFails) throw new HttpError(502, 'proxmox_permission_denied'); return {}; } };
  let engine = null;
  const restoreClient = updater ? { status: async () => engine.status(), apply: async (input, context) => { state.restoreCalls.push({ input, context }); return engine.apply(input, context); } } : null;
  state.start = async () => {
    state.store = new Store(join(data, 'broker.sqlite')); state.vault = Vault.open(join(data, 'vault.key')); state.starts++;
    state.server = await buildServer({ store: state.store, vault: state.vault, publicOrigin: origin, proxmox, accessClient, restoreClient, backupDir: join(data, 'backup-work'),
      logger: { error: value => state.logs.push(String(value)) }, initialSetupOptions: { announce: code => state.announced.push(code) },
      backupOptions: { kdf: fast, afterRestoreDelayMs: 0, ...options } });
    await new Promise(resolve => state.server.listen(0, '127.0.0.1', resolve));
    state.local = `http://127.0.0.1:${state.server.address().port}`; state.running = true;
  };
  state.stop = async () => { if (!state.running) return; state.running = false; await new Promise(resolve => state.server.close(resolve)); state.store.close(); };
  await state.start();
  if (updater) {
    const probe = (path, method, token) => fetch(state.local + path, { method, headers: { 'x-multitap-access-fence': token } }).then(response => response.ok ? response.json() : null).catch(() => null);
    const access = { kind: 'systemd', serviceName: 'multitap-broker', data, current: async () => ({ revision: 'r'.repeat(64), envSha256: 'd'.repeat(64), accessSha256: null, transport: null, config: null, installationId: 'i'.repeat(64) }),
      activity: (_current, token) => probe('/internal/server-access/activity', 'GET', token), endConnections: (_current, token) => probe('/internal/server-access/end-connections', 'POST', token),
      unchanged: async () => true, unchangedAfterRestart: async () => state.running && healthy(state) };
    const journal = { value: null, initialize: async () => {}, read: async () => journal.value, write: async value => { journal.value = structuredClone(value); } };
    const deployment = new ServerRestoreDeployment({ access, directory: join(root, 'operations'), sleep: async () => {}, healthAttempts: 3, run: async (_program, args) => { if (args[0] === 'stop') await state.stop(); else await state.start(); } });
    engine = new ServerRestoreEngine({ adapter: deployment, storage: journal, sleep: ms => new Promise(resolve => setTimeout(resolve, Math.min(ms, 5))), pollMs: 5, maxWaitMs: 2000 });
    await engine.initialize(); state.engine = engine; state.journal = journal;
  }
  state.call = async (path, method = 'GET', body, { headers = {}, raw = false } = {}) => {
    const response = await fetch(state.local + '/api/v1/admin' + path, { method, headers: { Cookie: state.cookie, ...(method !== 'GET' ? { Origin: origin, 'X-CSRF-Token': state.csrf, 'Content-Type': 'application/json' } : {}), ...headers }, body: body === undefined ? undefined : JSON.stringify(body) });
    if (raw) return response;
    const text = await response.text(); state.bodies.push(text);
    return { status: response.status, body: text ? JSON.parse(text) : null, code: text ? JSON.parse(text)?.error?.code : undefined, headers: response.headers };
  };
  state.login = async (username, secret) => {
    const response = await state.call('/login', 'POST', { username, password: secret }, { headers: { 'X-CSRF-Token': '' } });
    if (response.status === 200) { state.cookie = response.headers.get('set-cookie').split(';')[0]; state.csrf = response.body.csrfToken; }
    return response;
  };
  state.upload = async (bytes, extra = {}, chunk = null) => {
    const begun = await state.call('/backup/restore/upload', 'POST', { size: bytes.length, ...extra });
    if (begun.status !== 201) return begun;
    const step = chunk ?? begun.body.chunkBytes;
    for (let offset = 0; offset < bytes.length; offset += step) {
      const sent = await state.call('/backup/restore/upload/' + begun.body.uploadId, 'PUT', { offset, data: bytes.subarray(offset, offset + step).toString('base64') });
      assert.equal(sent.status, 200, JSON.stringify(sent.body));
    }
    return begun;
  };
  state.settled = async () => { await state.engine.task; for (let i = 0; i < 200 && state.server.backup.followUp; i++) await new Promise(resolve => setTimeout(resolve, 5)); await state.server.backup.followed; };
  return state;
}
async function exported(t, from, phrase = passphrase) {
  const made = await from.call('/backup/export', 'POST', { password: password[from.seed], passphrase: phrase, passphraseConfirmation: phrase });
  assert.equal(made.status, 201, JSON.stringify(made.body));
  const response = await from.call('/backup/export/' + made.body.download.id, 'GET', undefined, { raw: true });
  assert.equal(response.status, 200);
  return { info: made.body.download, bytes: Buffer.from(await response.arrayBuffer()), headers: response.headers };
}
const auditRows = store => store.db.prepare('SELECT event,actor,subject FROM audit_events ORDER BY id').all().map(row => ({ ...row }));
const events = store => auditRows(store).map(row => row.event);

test('only a signed-in administrator, from this site, with its CSRF token reaches the backup routes', async t => {
  const a = await site(t);
  for (const [path, method, body] of [['/backup', 'GET'], ['/backup/export', 'POST', {}], ['/backup/restore/upload', 'POST', { size: 100 }], ['/backup/restore/inspect', 'POST', {}], ['/backup/restore/apply', 'POST', {}], ['/backup/export/' + 'x'.repeat(32), 'GET']])
    assert.equal((await a.call(path, method, body)).status, 401, path);
  assert.equal((await a.login('membera', 'anything-at-all')).status, 401, 'an ordinary account has no administrator sign-in');
  assert.equal((await a.login('ownera', password.a)).status, 200);
  assert.equal((await a.call('/backup/export', 'POST', {}, { headers: { 'X-CSRF-Token': 'A'.repeat(43) } })).code, 'csrf_required');
  assert.equal((await a.call('/backup/export', 'POST', {}, { headers: { Origin: 'https://elsewhere.example' } })).status, 403);
  assert.equal((await a.call('/backup/restore/apply', 'POST', {}, { headers: { 'X-CSRF-Token': '' } })).code, 'csrf_required');
  assert.equal((await a.call('/backup', 'POST', {})).status, 405);
  assert.equal((await a.call('/backup/export', 'GET')).status, 405);
  assert.equal((await a.call('/backup/unknown', 'GET')).status, 404);
  const status = (await a.call('/backup')).body;
  assert.deepEqual({ protocol: status.protocol, supported: status.restore.supported, state: status.restore.state, reauthentication: status.reauthentication }, { protocol: 'backup-v1', supported: true, state: 'idle', reauthentication: { setupCode: false, mfa: false } });
  // A sign-in that ended is no longer an administrator for a download either.
  const made = await exported(t, a);
  await a.call('/logout', 'POST', {});
  assert.equal((await a.call('/backup/export/' + made.info.id, 'GET')).status, 401);
});

test('making a file asks for the password again, says who and how large, and keeps every secret out of records', async t => {
  const a = await site(t); await a.login('ownera', password.a);
  const attempt = body => a.call('/backup/export', 'POST', body);
  assert.equal((await attempt({ password: 'wrong-password-123', passphrase, passphraseConfirmation: passphrase })).code, 'current_password_incorrect');
  assert.equal((await attempt({ password: password.a, passphrase, passphraseConfirmation: passphrase + 'x' })).code, 'backup_passphrase_mismatch');
  assert.equal((await attempt({ password: password.a, passphrase: 'short', passphraseConfirmation: 'short' })).code, 'backup_passphrase_too_short');
  assert.equal((await attempt({ password: password.a, passphrase, passphraseConfirmation: passphrase, path: '/tmp/x' })).code, 'unexpected_field');
  assert.equal((await attempt({ password: password.a, code: '123456', passphrase, passphraseConfirmation: passphrase })).code, 'unexpected_field', 'no second factor on this account');
  assert.deepEqual(readdirSync(join(a.data, 'backup-work')).filter(name => name !== 'restore'), [], 'nothing was made');
  assert.deepEqual(events(a.store).filter(name => name.startsWith('backup_')), ['backup_reauthentication_failed']);
  const fresh = await site(t, { seed: 'a' }); await fresh.login('ownera', password.a);
  const made = await exported(t, fresh);
  assert.deepEqual([...made.bytes.subarray(0, 8)], [...backupMagic]); assert.equal(made.bytes.length, made.info.size);
  assert.match(made.headers.get('content-disposition'), /^attachment; filename="multitap-backup-\d{8}-\d{4}\.mtbk"$/);
  assert.equal(made.headers.get('cache-control'), 'no-store'); assert.equal(made.headers.get('content-type'), 'application/octet-stream');
  assert.deepEqual(made.info.counts, { accounts: 4, admins: 2, desktops: 4, hosts: 4, assignments: 4, adminNames: ['ownera', 'seconda'] });
  const rows = auditRows(fresh.store).filter(row => row.event.startsWith('backup_'));
  assert.deepEqual(rows, [{ event: 'backup_exported', actor: ids.owner, subject: String(made.info.size) }, { event: 'backup_downloaded', actor: ids.owner, subject: String(made.info.size) }]);
  const last = JSON.parse(fresh.store.setting('backup_last_export'));
  assert.deepEqual({ size: last.size, actor: last.actor }, { size: made.info.size, actor: 'ownera' });
  assert.deepEqual((await fresh.call('/backup')).body.export.last, last);
  assert.equal(fresh.store.db.prepare('SELECT count(*) AS n FROM sessions').get().n > 0, true, 'the live server keeps its sign-ins');
  // Everything the server wrote down or answered, searched for every secret involved.
  const written = JSON.stringify(auditRows(fresh.store)) + JSON.stringify(fresh.store.db.prepare("SELECT key,value FROM settings WHERE key<>'proxmox'").all()) + fresh.logs.join('\n') + fresh.bodies.join('\n') + a.bodies.join('\n') + a.logs.join('\n');
  for (const secret of [passphrase, password.a, 'wrong-password-123', Buffer.from(fresh.vault.key).toString('base64'), fixtureProxmox.secret, fixtureTotp]) assert.equal(written.includes(secret), false, secret);
  assert.deepEqual(fresh.logs, []);
  // The work files are gone; only the encrypted file waits for its download.
  assert.deepEqual(readdirSync(join(fresh.data, 'backup-work')).map(name => name.replace(/-[A-Za-z0-9_-]{32}/, '-ID')).sort(), ['export-ID.mtbk', 'restore']);
  assert.equal((await fresh.call('/backup/export/' + 'y'.repeat(32), 'GET')).code, 'backup_download_expired');
});

test('password checks and exports are limited per administrator', async t => {
  const a = await site(t); await a.login('ownera', password.a);
  const wrong = () => a.call('/backup/export', 'POST', { password: 'wrong-password-123', passphrase, passphraseConfirmation: passphrase });
  // Three files in ten minutes, and that budget is spent before the password is checked.
  for (let index = 0; index < 3; index++) assert.equal((await wrong()).code, 'current_password_incorrect');
  assert.equal((await wrong()).code, 'backup_rate_limited');
  assert.equal((await a.call('/backup/export', 'POST', { password: password.a, passphrase, passphraseConfirmation: passphrase })).code, 'backup_rate_limited');
  // Five password checks a minute in all, whichever action asks for them.
  const guess = () => a.call('/backup/restore/apply', 'POST', { uploadId: 'u'.repeat(32), confirm: true, password: 'wrong-password-123' });
  assert.equal((await guess()).code, 'current_password_incorrect'); assert.equal((await guess()).code, 'current_password_incorrect');
  const refused = await guess();
  assert.deepEqual({ status: refused.status, code: refused.code }, { status: 429, code: 'too_many_attempts' });
  assert.equal(events(a.store).filter(name => name === 'backup_reauthentication_failed').length, 5);
});

test('a second factor is asked for when the account has one', async t => {
  const open = []; t.after(() => { for (const close of open) close(); });
  const directory = temporaryDirectory(t), seeded = seedServer(directory, { passwordHash: await hashOf('a') }), calls = [];
  open.push(() => seeded.store.close());
  const token = newToken(), hash = tokenHash(token);
  seeded.store.createSession(seeded.store.findUser('ownera'), hash, Date.now(), 60000, 'admin');
  const backup = new BackupService({ store: seeded.store, vault: seeded.vault, directory: join(directory, 'backup-work'), version: VERSION, origin, kdf: fast }); t.after(() => backup.close());
  const mfaRoutes = { enabled: id => id === ids.owner, manage: async (action, input, principal) => { calls.push({ action, input: { ...input }, purpose: principal.purpose, hash: principal.hash }); if (input.code !== '123456') throw new HttpError(400, 'mfa_invalid_code'); return { ok: true }; } };
  const ask = async body => {
    let answer; const send = (status, value) => { answer = { status, body: value }; };
    const req = { method: 'POST', headers: { origin, cookie: `__Secure-multitap-admin=${token}`, 'x-csrf-token': seeded.vault.csrf(token) } };
    try { await adminRoutes({ store: seeded.store, vault: seeded.vault, backup, mfaRoutes, publicOrigin: origin, passwordJob: action => action(), body: async () => ({ ...body }), initialSetup: { verify: () => assert.fail('not a setup') } })(req, {}, '/api/v1/admin/backup/export', send); }
    catch (error) { return { status: error.status, code: error.code }; }
    return answer;
  };
  assert.deepEqual(await ask({ password: password.a, passphrase, passphraseConfirmation: passphrase }), { status: 400, code: 'mfa_code_required' });
  assert.deepEqual(await ask({ password: password.a, code: '000000', passphrase, passphraseConfirmation: passphrase }), { status: 400, code: 'mfa_invalid_code' });
  assert.deepEqual(readdirSync(directory).includes('backup-work') ? readdirSync(join(directory, 'backup-work')).filter(name => name.startsWith('export-')) : [], []);
  const made = await ask({ password: password.a, code: '123456', passphrase, passphraseConfirmation: passphrase });
  assert.equal(made.status, 201);
  assert.deepEqual(calls.at(-1), { action: 'reauth', input: { currentPassword: password.a, code: '123456' }, purpose: 'admin', hash });
});

test('round trip into a different, populated server: nothing changes until the restore, then everything is the backup', async t => {
  const a = await site(t, { seed: 'a', options: { accessConfig: accessOf('vdi-a.fixture.example') } }), b = await site(t, { seed: 'b', options: { accessConfig: accessOf('vdi-b.fixture.example') } });
  await a.login('ownera', password.a); await b.login('ownerb', password.b);
  // What the file should hold: server A as it is at this moment, cleaned the same way.
  const expected = join(a.root, 'expected.sqlite'); await snapshotDatabase(a.store.db, expected); sanitizeSnapshot(expected);
  const file = await exported(t, a);
  const before = travellingRows(join(b.data, 'broker.sqlite')), keyBefore = sha(readFileSync(join(b.data, 'vault.key')));
  const begun = await b.upload(file.bytes, {}, 50000); assert.equal(begun.status, 201);
  // A wrong passphrase reads nothing and keeps the upload for another try.
  const wrong = await b.call('/backup/restore/inspect', 'POST', { uploadId: begun.body.uploadId, passphrase: passphrase + '?' });
  assert.deepEqual({ status: wrong.status, code: wrong.code }, { status: 422, code: 'backup_unreadable' });
  const shown = await b.call('/backup/restore/inspect', 'POST', { uploadId: begun.body.uploadId, passphrase });
  assert.equal(shown.status, 200, JSON.stringify(shown.body));
  const preview = shown.body.preview;
  assert.deepEqual({ createdAt: preview.createdAt, serverVersion: preview.serverVersion, origin: preview.origin, counts: preview.counts, proxmox: preview.proxmox, unreadableSecrets: preview.unreadableSecrets, migrated: preview.migrated, access: preview.access },
    { createdAt: file.info.createdAt, serverVersion: VERSION, origin, counts: { accounts: 4, admins: 2, desktops: 4, hosts: 4, assignments: 4, adminNames: ['ownera', 'seconda'] }, proxmox: { configured: true, readable: true, endpoint: fixtureProxmox.endpoint }, unreadableSecrets: 0, migrated: false,
      access: { available: true, differs: true, primaryOrigin: 'https://vdi-a.fixture.example' } });
  // Looking changed nothing on this server.
  assert.deepEqual(travellingRows(join(b.data, 'broker.sqlite'), { skipSettings: [] }).users, before.users);
  assert.equal(sha(readFileSync(join(b.data, 'vault.key'))), keyBefore); assert.deepEqual(b.restoreCalls, []); assert.equal(b.server.accessRuntime.blocked, false); assert.equal(b.starts, 1);
  assert.deepEqual((await b.call('/backup')).body.upload.preview, preview);
  // The restore itself: confirmation and the password again.
  const apply = body => b.call('/backup/restore/apply', 'POST', { uploadId: begun.body.uploadId, ...body });
  assert.equal((await apply({ password: password.b })).code, 'restore_confirmation_required');
  assert.equal((await apply({ confirm: true })).code, 'current_password_required');
  assert.equal((await apply({ confirm: true, password: 'wrong-password-123' })).code, 'current_password_incorrect');
  assert.equal((await apply({ confirm: true, password: password.b, restoreAccess: 'yes' })).code, 'restore_confirmation_required');
  assert.deepEqual(b.restoreCalls, []);
  const accepted = await apply({ confirm: true, password: password.b });
  assert.equal(accepted.status, 202, JSON.stringify(accepted.body));
  const sent = b.restoreCalls[0];
  assert.deepEqual(Object.keys(sent.input).sort(), ['confirm', 'database', 'operationId', 'schemaVersion', 'vault']); assert.equal(sent.input.schemaVersion, 19);
  assert.match(sent.context.fence.token, /^[a-f0-9]{64}$/); assert.equal(sent.context.fence.operationId, sent.input.operationId);
  assert.equal(JSON.stringify(accepted.body).includes(sent.context.fence.token), false);
  await b.settled();
  assert.equal(b.engine.status().state, 'succeeded', JSON.stringify(b.engine.status())); assert.equal(b.starts, 2, 'the broker was stopped and started once');
  // Server B now holds exactly what server A's backup held, in a database of B's own making.
  const after = travellingRows(join(b.data, 'broker.sqlite'), { skipSettings: ['restore_last'] }), wanted = travellingRows(expected);
  // The one sign-in that was open on A is closed at the moment each snapshot was cleaned.
  for (const rows of [after.usage_logins, wanted.usage_logins]) rows.at(-1).ended_at = null;
  for (const name of backupIncludedTables.filter(name => name !== 'audit_events')) assert.deepEqual(after[name], wanted[name], name);
  assert.deepEqual(after.audit_events.slice(0, wanted.audit_events.length), wanted.audit_events);
  assert.deepEqual(after.audit_events.slice(wanted.audit_events.length).map(row => row.event), ['backup_restored']);
  assert.deepEqual(readFileSync(join(b.data, 'vault.key')), readFileSync(join(a.data, 'vault.key')));
  assert.deepEqual(b.vault.open(b.store.setting('proxmox')), { ...fixtureProxmox, secret: fixtureProxmox.secret + 'a' });
  assert.equal(b.vault.open(b.store.db.prepare('SELECT factor_ciphertext AS v FROM account_mfa').get().v).secret, fixtureTotp);
  assert.equal(b.store.db.prepare('SELECT count(*) AS n FROM sessions').get().n, 0, 'nobody is signed in');
  // The administrator who restored is signed out; the backup's administrator signs in.
  assert.equal((await b.call('/backup')).status, 401);
  assert.equal((await b.login('ownerb', password.b)).status, 401);
  assert.equal((await b.login('ownera', password.a)).status, 200);
  const status = (await b.call('/backup')).body;
  assert.deepEqual({ backup: status.last.backup, actor: status.last.actor, proxmox: status.last.proxmox.state, access: status.last.access }, { backup: { createdAt: file.info.createdAt, serverVersion: VERSION, origin }, actor: 'ownerb', proxmox: 'ok', access: { state: 'kept' } });
  assert.equal(status.restore.state, 'succeeded'); assert.equal(status.upload, null);
  // The Proxmox connection was checked with the restored token; the access settings were left alone.
  assert.deepEqual(b.proxmoxCalls, [{ endpoint: fixtureProxmox.endpoint, secret: fixtureProxmox.secret + 'a', path: '/access/permissions?path=%2Fvms' }]);
  assert.deepEqual(b.accessCalls, []);
  assert.deepEqual(readdirSync(join(b.data, 'backup-work', 'restore')), [], 'the staged plaintext is gone after the restart');
  assert.equal(b.server.accessRuntime.blocked, false);
  // Server A never noticed.
  assert.deepEqual(a.restoreCalls, []); assert.equal(a.starts, 1);
});

test('the access settings travel only when asked, through the ordinary access pipeline; a dead Proxmox token is said plainly', async t => {
  const a = await site(t, { seed: 'a', options: { accessConfig: accessOf('vdi-a.fixture.example') } }), b = await site(t, { seed: 'b', proxmoxFails: true, options: { accessConfig: accessOf('vdi-b.fixture.example') } });
  await a.login('ownera', password.a); await b.login('ownerb', password.b);
  const file = await exported(t, a), begun = await b.upload(file.bytes);
  await b.call('/backup/restore/inspect', 'POST', { uploadId: begun.body.uploadId, passphrase });
  assert.equal((await b.call('/backup/restore/apply', 'POST', { uploadId: begun.body.uploadId, confirm: true, restoreAccess: true, password: password.b })).status, 202);
  await b.settled();
  assert.deepEqual(b.accessCalls.map(call => call.kind), ['test', 'save']);
  assert.deepEqual(b.accessCalls[0].input.config, accessOf('vdi-a.fixture.example')); assert.deepEqual(b.accessCalls[1].input.config, accessOf('vdi-a.fixture.example'));
  assert.equal(b.accessCalls[1].input.confirm, true); assert.equal(b.accessCalls[1].input.testId, 'b'.repeat(64)); assert.equal(b.accessCalls[0].context.managementOrigin, origin);
  await b.login('ownera', password.a);
  const last = (await b.call('/backup')).body.last;
  assert.equal(last.access.state, 'requested'); assert.match(last.access.operationId, /^[a-f0-9-]{36}$/);
  assert.deepEqual({ state: last.proxmox.state, errorCode: last.proxmox.errorCode }, { state: 'error', errorCode: 'proxmox_permission_denied' });
  assert.deepEqual(events(b.store).filter(name => name.startsWith('backup_')), ['backup_restored', 'backup_restore_access_requested']);
  // A broker that starts again later does not repeat any of it.
  await b.stop(); await b.start(); await new Promise(resolve => setTimeout(resolve, 30));
  assert.equal(b.accessCalls.length, 2); assert.equal(b.proxmoxCalls.length, 1);
});

test('a restored server that does not come up healthy is rolled back, and the administrator is still signed in', async t => {
  const a = await site(t, { seed: 'a' }), b = await site(t, { seed: 'b', healthy: state => state.store.db.prepare("SELECT 1 FROM users WHERE username='ownerb'").get() !== undefined });
  await a.login('ownera', password.a); await b.login('ownerb', password.b);
  const before = travellingRows(join(b.data, 'broker.sqlite')), keyBefore = sha(readFileSync(join(b.data, 'vault.key')));
  const file = await exported(t, a), begun = await b.upload(file.bytes);
  await b.call('/backup/restore/inspect', 'POST', { uploadId: begun.body.uploadId, passphrase });
  const accepted = await b.call('/backup/restore/apply', 'POST', { uploadId: begun.body.uploadId, confirm: true, password: password.b });
  await b.engine.task;
  assert.equal(b.starts, 3, 'started with the backup, then started again with the safety copy');
  const status = await b.call('/backup');
  assert.equal(status.status, 200, 'the sign-in of before the restore works again');
  assert.deepEqual({ state: status.body.restore.state, errorCode: status.body.restore.errorCode, operationId: status.body.restore.operationId }, { state: 'rolled_back', errorCode: 'restore_health_failed', operationId: accepted.body.operationId });
  const after = travellingRows(join(b.data, 'broker.sqlite'));
  for (const name of ['users', 'desktops', 'assignments', 'hosts', 'settings', 'usage_logins']) assert.deepEqual(after[name], before[name], name);
  assert.equal(sha(readFileSync(join(b.data, 'vault.key'))), keyBefore); assert.equal(status.body.last, null);
  assert.equal(b.server.accessRuntime.blocked, false); assert.deepEqual(b.accessCalls, []);
  assert.deepEqual(events(b.store).filter(name => name === 'backup_restored'), [], 'what the short-lived restored server wrote went with it');
  assert.equal((await b.login('ownera', password.a)).status, 401);
});

test('a restore the updater refuses or never starts releases the fence and keeps the server as it was', async t => {
  const a = await site(t, { seed: 'a' }), b = await site(t, { seed: 'b' });
  await a.login('ownera', password.a); await b.login('ownerb', password.b);
  const file = await exported(t, a), begun = await b.upload(file.bytes);
  await b.call('/backup/restore/inspect', 'POST', { uploadId: begun.body.uploadId, passphrase });
  const real = b.engine.apply.bind(b.engine);
  b.engine.apply = async () => { throw new HttpError(409, 'restore_busy'); };
  assert.equal((await b.call('/backup/restore/apply', 'POST', { uploadId: begun.body.uploadId, confirm: true, password: password.b })).code, 'restore_busy');
  assert.equal(b.server.accessRuntime.blocked, false); assert.equal(b.starts, 1);
  // The same checked upload can be applied once the updater is free.
  b.engine.apply = real;
  assert.equal((await b.call('/backup/restore/apply', 'POST', { uploadId: begun.body.uploadId, confirm: true, password: password.b })).status, 202);
  assert.equal(b.server.accessRuntime.blocked, true, 'admission is closed while the restore runs');
  await b.settled(); assert.equal(b.engine.status().state, 'succeeded');
  const none = await site(t, { seed: 'b', updater: false }); await none.login('ownerb', password.b);
  const status = (await none.call('/backup')).body;
  assert.deepEqual({ supported: status.restore.supported, state: status.restore.state, available: status.export.available }, { supported: false, state: 'unavailable', available: true });
  const again = await none.upload(file.bytes);
  assert.equal((await none.call('/backup/restore/inspect', 'POST', { uploadId: again.body.uploadId, passphrase })).status, 200, 'a file can still be checked there');
  assert.equal((await none.call('/backup/restore/apply', 'POST', { uploadId: again.body.uploadId, confirm: true, password: password.b })).code, 'restore_unavailable');
});

test('a fresh server offers the restore under the protection of its first setup: that session and the setup code', async t => {
  const a = await site(t, { seed: 'a' }), f = await site(t, { fresh: true });
  await a.login('ownera', password.a);
  const file = await exported(t, a);
  assert.equal((await f.call('/backup')).status, 401, 'no session, nothing');
  assert.equal((await f.login('admin', 'admin')).status, 200);
  const code = f.announced.at(-1); assert.match(code, /^[0-9A-Z]{4}-[0-9A-Z]{4}-[0-9A-Z]{4}$/);
  const status = (await f.call('/backup')).body;
  assert.deepEqual(status.reauthentication, { setupCode: true, mfa: false }); assert.equal('last' in status, false); assert.equal('last' in status.export, false); assert.equal('connections' in status, false);
  assert.equal((await f.call('/backup/export', 'POST', { password: 'admin', passphrase, passphraseConfirmation: passphrase })).code, 'credentials_change_required');
  assert.equal((await f.call('/backup/restore/upload', 'POST', { size: file.bytes.length })).code, 'setup_code_invalid');
  assert.equal((await f.call('/backup/restore/upload', 'POST', { size: file.bytes.length, setupCode: 'AAAA-BBBB-CCCC' })).code, 'setup_code_invalid');
  assert.equal((await f.call('/backup/restore/upload', 'POST', { size: file.bytes.length, setupCode: code, password: 'admin' })).code, 'unexpected_field');
  const begun = await f.upload(file.bytes, { setupCode: code }); assert.equal(begun.status, 201);
  assert.equal((await f.call('/backup/restore/inspect', 'POST', { uploadId: begun.body.uploadId, passphrase })).code, 'setup_code_invalid');
  const shown = await f.call('/backup/restore/inspect', 'POST', { uploadId: begun.body.uploadId, passphrase, setupCode: code });
  assert.equal(shown.status, 200); assert.equal(shown.body.preview.counts.accounts, 4);
  assert.equal((await f.call('/backup/restore/apply', 'POST', { uploadId: begun.body.uploadId, confirm: true, password: 'admin' })).code, 'unexpected_field');
  assert.equal((await f.call('/backup/restore/apply', 'POST', { uploadId: begun.body.uploadId, confirm: true })).code, 'setup_code_invalid');
  assert.equal(f.starts, 1);
  // Requests without the code spent nothing of the budgets the real operator needs.
  assert.equal(f.server.backup.limits.applies.entries.size, 0); assert.equal([...f.server.backup.limits.inspections.entries.values()][0].count, 1);
  assert.equal((await f.call('/backup/restore/apply', 'POST', { uploadId: begun.body.uploadId, confirm: true, setupCode: code.toLowerCase() })).status, 202);
  await f.settled();
  assert.equal(f.engine.status().state, 'succeeded');
  assert.equal((await f.login('admin', 'admin')).status, 401, 'the first-setup account is gone');
  const entered = await f.login('ownera', password.a);
  assert.equal(entered.status, 200); assert.equal(entered.body.user.mustChangeCredentials, false);
  assert.equal(f.store.setting('initial_setup_code'), undefined);
  // An established server does not take a setup code in place of the password.
  assert.equal((await a.call('/backup/restore/upload', 'POST', { size: 1000, setupCode: code })).code, 'unexpected_field');
  // Guesses at the code are limited for the session that makes them (10 a minute, as for the setup itself).
  const g = await site(t, { fresh: true }); await g.login('admin', 'admin');
  const codes = [];
  for (let index = 0; index < 12; index++) codes.push((await g.call('/backup/restore/upload', 'POST', { size: 1000, setupCode: 'AAAA-BBBB-CC' + String(index).padStart(2, '0') })).code);
  assert.deepEqual([...new Set(codes.slice(0, 10))], ['setup_code_invalid']); assert.equal(codes[11], 'too_many_setup_attempts');
});

test('uploads are bounded, in order, and belong to the session that started them', async t => {
  const a = await site(t, { seed: 'a', options: { maxUploadBytes: 400000 } }); await a.login('ownera', password.a);
  assert.equal((await a.call('/backup/restore/upload', 'POST', { size: 400001 })).status, 413);
  for (const size of [0, 10, -5, 1.5, '1000', null]) assert.equal((await a.call('/backup/restore/upload', 'POST', { size })).status, 400, String(size));
  const begun = (await a.call('/backup/restore/upload', 'POST', { size: 1000 })).body, put = (body, id = begun.uploadId) => a.call('/backup/restore/upload/' + id, 'PUT', body);
  const data = Buffer.alloc(600, 7).toString('base64');
  assert.deepEqual((await put({ offset: 0, data })).body, { received: 600, complete: false });
  const skipped = await put({ offset: 700, data });
  assert.deepEqual({ status: skipped.status, code: skipped.code, details: skipped.body.error.details }, { status: 409, code: 'backup_upload_offset', details: { received: 600 } });
  assert.equal((await put({ offset: 600, data })).status, 400, 'more than was announced');
  assert.equal((await put({ offset: 600, data: '!!!' })).status, 400); assert.equal((await put({ offset: 600, data: 'QUJD\n' })).status, 400);
  assert.equal((await put({ offset: 600, data, name: '../../etc/passwd' })).code, 'unexpected_field');
  assert.equal((await a.call('/backup/restore/inspect', 'POST', { uploadId: begun.uploadId, passphrase })).code, 'backup_upload_incomplete');
  assert.equal((await put({ offset: 600, data: Buffer.alloc(400, 7).toString('base64') })).body.complete, true);
  assert.equal((await put({ offset: 0, data }, 'z'.repeat(32))).code, 'backup_upload_not_found');
  // Another administrator's session cannot continue, read or cancel it.
  const other = { cookie: a.cookie, csrf: a.csrf }; a.store.db.prepare('UPDATE users SET password_hash=? WHERE username=?').run(await hashOf('b'), 'seconda');
  a.store.db.prepare('DELETE FROM account_mfa').run(); a.cookie = ''; assert.equal((await a.login('seconda', password.b)).status, 200);
  assert.equal((await put({ offset: 1000, data })).code, 'backup_upload_not_found');
  assert.equal((await a.call('/backup/restore/inspect', 'POST', { uploadId: begun.uploadId, passphrase })).code, 'backup_upload_not_found');
  assert.equal((await a.call('/backup/restore/upload/' + begun.uploadId, 'DELETE')).code, 'backup_upload_not_found');
  assert.equal((await a.call('/backup')).body.upload, null);
  Object.assign(a, other);
  // 1000 bytes of sevens are not a backup file: refused, and the upload is dropped.
  assert.equal((await a.call('/backup/restore/inspect', 'POST', { uploadId: begun.uploadId, passphrase })).code, 'backup_file_invalid');
  assert.equal((await a.call('/backup/restore/inspect', 'POST', { uploadId: begun.uploadId, passphrase })).code, 'backup_upload_not_found');
  assert.deepEqual(readdirSync(join(a.data, 'backup-work')).filter(name => name !== 'restore'), []); assert.deepEqual(readdirSync(join(a.data, 'backup-work', 'restore')), []);
});

test('a file from a newer server, a hostile database and a helper that takes too long are refused and leave nothing behind', async t => {
  const b = await site(t, { seed: 'b' }); await b.login('ownerb', password.b);
  const directory = temporaryDirectory(t), seeded = seedServer(directory, { seed: 'a' }), snapshot = join(directory, 'snapshot.sqlite');
  await snapshotDatabase(seeded.store.db, snapshot); sanitizeSnapshot(snapshot); const key = Buffer.from(seeded.vault.key).toString('base64'); seeded.store.close();
  const manifest = { schema: 1, origin, deploymentKind: 'systemd', database: { schemaVersion: 19 }, vaultKey: key, serverAccess: null, excluded: [] };
  const make = async (name, { serverVersion = VERSION, change = null, manifestChange = {} } = {}) => {
    const source = join(directory, name + '.sqlite'); writeFileSync(source, readFileSync(snapshot));
    if (change) { const db = new DatabaseSync(source); try { db.exec(change); } finally { db.close(); } }
    const output = join(directory, name + '.mtbk');
    await writeBackup({ passphrase, createdAt: '2026-11-10T07:30:00.000Z', serverVersion, manifest: { ...manifest, ...manifestChange }, databasePath: source, outputPath: output, kdf: fast });
    return readFileSync(output);
  };
  const tried = async bytes => { b.server.backup.limits.uploads.entries.clear(); b.server.backup.limits.inspections.entries.clear(); const begun = await b.upload(bytes); return b.call('/backup/restore/inspect', 'POST', { uploadId: begun.body.uploadId, passphrase }); };
  const clean = () => { assert.deepEqual(readdirSync(join(b.data, 'backup-work')).filter(name => name !== 'restore'), []); assert.deepEqual(readdirSync(join(b.data, 'backup-work', 'restore')), []); };
  assert.deepEqual([(await tried(await make('newer', { serverVersion: '99.0.0' }))).status, (await tried(await make('newer2', { serverVersion: '99.0.0' }))).code], [422, 'backup_from_newer_server']); clean();
  assert.equal((await tried(await make('schema', { change: 'PRAGMA user_version=20' }))).code, 'backup_schema_newer'); clean();
  assert.equal((await tried(await make('view', { change: 'CREATE VIEW spy AS SELECT password_hash FROM users' }))).code, 'backup_database_unsupported'); clean();
  assert.equal((await tried(await make('virtual', { change: 'CREATE VIRTUAL TABLE notes USING fts5(body)' }))).code, 'backup_database_unsupported'); clean();
  assert.equal((await tried(await make('noadmin', { change: "UPDATE users SET disabled=1 WHERE role='admin'" }))).code, 'backup_no_administrator'); clean();
  // A hostile trigger is accepted as a file but never reaches the staged database.
  const hostile = await tried(await make('trigger', { change: "CREATE TRIGGER promote AFTER INSERT ON audit_events BEGIN UPDATE users SET role='admin'; END" }));
  assert.equal(hostile.status, 200); assert.equal(hostile.body.preview.ignoredObjects, 1);
  const staged = join(b.data, 'backup-work', 'restore', readdirSync(join(b.data, 'backup-work', 'restore'))[0]);
  assert.deepEqual(readdirSync(staged).sort(), ['broker.sqlite', 'vault.key'], 'two files, named by the server');
  const db = new DatabaseSync(join(staged, 'broker.sqlite'), { readOnly: true }); try { assert.equal(db.prepare("SELECT count(*) AS n FROM sqlite_schema WHERE name='promote'").get().n, 0); } finally { db.close(); }
  // The staging area is forgotten after its time.
  const timed = await site(t, { seed: 'b', options: { now: (() => { let time = Date.now(); const clock = () => time; clock.jump = ms => { time += ms; }; return clock; })() } }); await timed.login('ownerb', password.b);
  const kept = await timed.upload(await make('ok')); assert.equal((await timed.call('/backup/restore/inspect', 'POST', { uploadId: kept.body.uploadId, passphrase })).status, 200);
  timed.server.backup.now.jump(16 * 60000);
  assert.equal((await timed.call('/backup/restore/apply', 'POST', { uploadId: kept.body.uploadId, confirm: true, password: password.b })).code, 'backup_upload_not_found');
  await timed.server.backup.expire(); assert.deepEqual(readdirSync(join(timed.data, 'backup-work', 'restore')), []);
  // A helper that does not finish in time is ended.
  const slow = await site(t, { seed: 'b', options: { helperTimeoutMs: 1 } }); await slow.login('ownerb', password.b);
  const failed = await slow.call('/backup/export', 'POST', { password: password.b, passphrase, passphraseConfirmation: passphrase });
  assert.deepEqual({ status: failed.status, code: failed.code }, { status: 503, code: 'backup_helper_timeout' });
  assert.deepEqual(readdirSync(join(slow.data, 'backup-work')).filter(name => name !== 'restore'), []);
  assert.deepEqual(events(slow.store).filter(name => name.startsWith('backup_')), []);
  assert.deepEqual([...b.logs, ...slow.logs, ...timed.logs], []);
});

test('certificate file paths never come from a backup: a server that serves TLS itself keeps its own files', async t => {
  const direct = (host, cert, key) => normalizeServerAccess({ protocol: 'server-access-v1', primaryOrigin: `https://${host}`, https: { enabled: true, origin: `https://${host}`, mode: 'direct', listenHost: '0.0.0.0', listenPort: 18443, certFile: cert, keyFile: key }, http: { enabled: false, origin: null, listenHost: '0.0.0.0', listenPort: 18481 }, trustedProxies: [] });
  const fromBackup = direct('vdi-a.fixture.example', '/etc/shadow-like/cert.pem', '/root/private/key.pem');
  const service = accessConfig => { const value = new BackupService({ store: null, vault: null, version: VERSION, accessConfig }); t.after(() => value.close()); return value; };
  assert.equal(service(accessOf('vdi-b.fixture.example')).portableAccess(fromBackup), null, 'a server behind a proxy has no certificate files to use');
  assert.equal(service(null).portableAccess(fromBackup), null, 'neither has a fresh server');
  const own = service(direct('vdi-c.fixture.example', '/etc/multitap/tls/cert.pem', '/etc/multitap/tls/key.pem')).portableAccess(fromBackup);
  assert.deepEqual({ origin: own.primaryOrigin, certFile: own.https.certFile, keyFile: own.https.keyFile }, { origin: 'https://vdi-a.fixture.example', certFile: '/etc/multitap/tls/cert.pem', keyFile: '/etc/multitap/tls/key.pem' });
  assert.equal(JSON.stringify(own).includes('shadow-like'), false); assert.equal(JSON.stringify(own).includes('/root/private'), false);
  assert.deepEqual(service(null).portableAccess(accessOf('vdi-a.fixture.example')), accessOf('vdi-a.fixture.example'), 'proxy-mode settings carry no path and pass as they are');
});

test('the broker empties its working directory when it starts', async t => {
  const a = await site(t, { seed: 'a' }); await a.login('ownera', password.a);
  await exported(t, a);
  const work = join(a.data, 'backup-work'); mkdirSync(join(work, 'restore', '11111111-1111-4111-8111-111111111111'), { recursive: true });
  writeFileSync(join(work, 'restore', '11111111-1111-4111-8111-111111111111', 'vault.key'), Buffer.alloc(32)); writeFileSync(join(work, 'upload-' + 'u'.repeat(32) + '.part'), 'x'); writeFileSync(join(work, 'keep-me.txt'), 'not ours');
  await a.stop(); await a.start(); await a.server.backup.prepare();
  assert.deepEqual(readdirSync(work).sort(), ['keep-me.txt', 'restore']); assert.deepEqual(readdirSync(join(work, 'restore')), []);
  assert.equal(existsSync(join(a.data, 'broker.sqlite')), true);
});
