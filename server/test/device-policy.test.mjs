import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { DatabaseSync } from 'node:sqlite';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Store as Store18 } from './fixtures/mfa-store18.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { devicePolicyKeys, devicePolicyBits, devicePolicyFromMask, devicePolicyMask } from '../src/device-policy.mjs';

// Device permissions of an assignment (src/device-policy.mjs). Fixture-only
// broker and Host sockets.
const origin = 'https://device-policy-fixture.example';
const deny = Object.freeze(Object.fromEntries(devicePolicyKeys.map(key => [key, false])));
const all = Object.freeze(Object.fromEntries(devicePolicyKeys.map(key => [key, true])));
const clipDeny = Object.freeze({ textToRemote: false, textToLocal: false, filesToRemote: false, filesToLocal: false });
const baseCaps = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true };

test('device policy masks are exact, bounded and default closed', () => {
  assert.equal(devicePolicyBits, 1 | 2 | 4 | 8 | 64);
  // The bits 16 and 32 belong to no permission: they are not read and never written from a policy.
  for (let mask = 0; mask < 128; mask++) assert.equal(devicePolicyMask(devicePolicyFromMask(mask)), mask & devicePolicyBits);
  for (const mask of [undefined, null, -1, 128, 1.5, '127', NaN]) assert.deepEqual(devicePolicyFromMask(mask), deny);
  for (const value of [null, undefined, [], {}, true, { ...deny, drives: 'true' }, { ...deny, extra: false }, { drives: true }, { ...deny, clipboard: true }, { ...deny, cameras: false }])
    assert.throws(() => devicePolicyMask(value), /invalid_device_policy/);
  assert.deepEqual(Object.keys(deny), ['drives', 'printers', 'usbDevices', 'smartCards', 'serialPorts']);
  assert.deepEqual(devicePolicyFromMask(64 | 1), { ...deny, drives: true, serialPorts: true });
});

test('schema 12→13 adds a deny-all device permission column and preserves every retained row', async t => {
  const root = await mkdtemp(join(tmpdir(), 'multitap-device-policy-')); const path = join(root, 'fixture.sqlite');
  let store = new Store18(path); t.after(async () => { store.close(); await rm(root, { recursive: true, force: true }); });
  const admin = store.addUser({ name: '관리자', passwordHash: 'retained-admin-hash', role: 'admin', notes: 'retained' });
  const member = store.addUser({ name: '사용자', passwordHash: 'retained-member-hash' });
  store.syncProxmox([{ vmid: 730, node: 'fixture', name: 'retained VM', status: 'running' }], 'fixture'); store.assign('사용자', 'pve-730');
  // A registered Host: assignments of desktops without one are removed once at the first start of this release (hostless-assignments-cleanup.test.mjs).
  store.db.prepare('INSERT INTO hosts(id,desktop_id,token_hash,hostname,machine_uuid,version,created_at) VALUES(?,?,?,?,?,?,?)').run('retained-host-730','pve-730','retained-token-730','retained','12345678-1234-4234-8234-000000000730','1.0.0',1000);
  store.createSession(store.findUser('사용자'), 'retained-session-hash', 1000, 9999999999999);
  store.close(); const previous = new DatabaseSync(path);
  // Schema 12 kept the account-wide clipboard permission on the users row.
  previous.exec("ALTER TABLE users DROP COLUMN rdp_redirections;ALTER TABLE assignments DROP COLUMN sound_card_add;ALTER TABLE assignments DROP COLUMN clipboard_permissions;ALTER TABLE assignments DROP COLUMN rdp_redirections;ALTER TABLE assignments DROP COLUMN favorite;ALTER TABLE assignments DROP COLUMN last_used_at;ALTER TABLE desktops DROP COLUMN pve_tags;ALTER TABLE desktops DROP COLUMN pve_pool;UPDATE users SET clipboard_permissions=1 WHERE username='사용자';DROP TABLE session_end_reasons;DROP TABLE session_activity;DROP INDEX sessions_windows_owner;DROP INDEX sessions_replaced;ALTER TABLE sessions DROP COLUMN replaced_at;ALTER TABLE sessions DROP COLUMN issuer;PRAGMA user_version=12;");
  const before = Object.fromEntries(['users', 'sessions', 'assignments', 'desktops', 'settings'].map(table => [table, previous.prepare('SELECT rowid,* FROM ' + table + ' ORDER BY rowid').all().map(row => ({ ...row }))])); previous.close();
  store = new Store(path);
  assert.equal(store.db.prepare('PRAGMA user_version').get().user_version, 19);
  assert.deepEqual(store.db.prepare('SELECT rowid,* FROM users ORDER BY rowid').all().map(({ rdp_redirections, authorization_revision, ...row }) => { assert.equal(rdp_redirections, 0); assert.equal(authorization_revision, 1); return { ...row }; }), before.users);
  assert.deepEqual(store.db.prepare('SELECT rowid,* FROM assignments ORDER BY rowid').all().map(({ clipboard_permissions, rdp_redirections, favorite, last_used_at, sound_card_add, ...row }) => { assert.equal(clipboard_permissions, 1); assert.equal(rdp_redirections, 0); return { ...row }; }), before.assignments);
  assert.equal(store.setting('hostless_assignments_cleanup'), '0');
  for (const table of ['sessions', 'desktops', 'settings']) assert.deepEqual(store.db.prepare('SELECT rowid,* FROM ' + table + (table === 'settings' ? " WHERE key NOT IN ('hostless_assignments_cleanup','usage_backfill')" : '') + ' ORDER BY rowid').all().map(({ pve_tags, pve_pool, issuer, replaced_at, ...row }) => ({ ...row })), before[table]);
  for (const id of [admin, member]) assert.deepEqual(store.devicePolicy(id, 'pve-730'), deny);
  assert.deepEqual(store.clipboardPolicy(member, 'pve-730'), { ...clipDeny, textToRemote: true }, 'clipboard permission is reused on the assignment, not reset');
  for (const table of ['users', 'assignments']) assert.throws(() => store.db.prepare(`UPDATE ${table} SET rdp_redirections=128 WHERE ${table === 'users' ? 'id' : 'user_id'}=?`).run(member), /CHECK/);
  store.updateUser('사용자', { devicePolicy: { ...deny, drives: true } }, admin);
  assert.deepEqual(store.devicePolicy(member, 'pve-730'), { ...deny, drives: true });
  assert.equal(store.db.prepare('SELECT count(*) AS n FROM sessions').get().n, 1, 'a redirection grant does not revoke login');
  store.assign('관리자', 'pve-730', admin);
  assert.throws(() => store.updateUser('관리자', { role: 'user', devicePolicy: all }, admin), /primary_admin_required/);
  assert.deepEqual(store.devicePolicy(admin, 'pve-730'), deny, 'failed combined updates are atomic');
  store.setUserEnabled('사용자', false, admin); assert.deepEqual(store.devicePolicy(member, 'pve-730'), deny, 'disabled accounts never carry a grant');
  store.close(); const newer = new DatabaseSync(path); newer.exec('PRAGMA user_version=20;'); newer.close();
  assert.throws(() => new Store(path), /database_version_too_new/);
  const restore = new DatabaseSync(path); restore.exec('PRAGMA user_version=19;'); restore.close(); store = new Store(path);
});

async function socket(url, authorization) {
  const ws = new WebSocket(url, { headers: { Authorization: authorization }, perMessageDeflate: false }), queue = [], waiters = []; ws.on('error', () => {});
  ws.closed = new Promise(resolve => ws.once('close', code => { for (const waiter of waiters.splice(0)) { clearTimeout(waiter.timer); waiter.reject(new Error('closed')); } resolve(code); }));
  ws.on('message', (data, binary) => { const value = binary ? data : JSON.parse(data.toString('utf8')); const index = waiters.findIndex(waiter => waiter.predicate(value)); if (index >= 0) { const waiter = waiters.splice(index, 1)[0]; clearTimeout(waiter.timer); waiter.resolve(value); } else queue.push(value); });
  ws.queue = queue; ws.json = value => ws.send(JSON.stringify(value));
  ws.take = (predicate = () => true) => { const index = queue.findIndex(predicate); if (index >= 0) return Promise.resolve(queue.splice(index, 1)[0]); return new Promise((resolve, reject) => { const waiter = { predicate, resolve, reject, timer: setTimeout(() => { const at = waiters.indexOf(waiter); if (at >= 0) waiters.splice(at, 1); reject(new Error('message_timeout')); }, 2500) }; waiters.push(waiter); }); };
  await new Promise((resolve, reject) => { ws.once('open', resolve); ws.once('error', reject); ws.once('unexpected-response', (_req, response) => { response.resume(); reject(Object.assign(new Error('upgrade_rejected'), { status: response.statusCode })); }); }); return ws;
}
async function fixture(t, capabilities = baseCaps) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32)), sockets = [], logs = [];
  store.addUser({ name: 'fixture-admin', role: 'admin', passwordHash: 'fixture-unused-password-hash' });
  for (const name of ['alice', 'bobby']) store.addUser({ name, role: 'user', passwordHash: 'fixture-unused-password-hash' });
  store.saveSetting('proxmox_initialized', '1', 'fixture');
  const tokens = { admin: newToken(), alice: newToken(), bobby: newToken(), host: newToken() };
  for (const name of ['admin', 'alice', 'bobby']) store.createSession(store.findUser(name === 'admin' ? 'fixture-admin' : name), tokenHash(tokens[name]), Date.now(), 3600000, name === 'admin' ? 'admin' : 'client');
  store.syncProxmox([740, 741].map(vmid => ({ vmid, node: 'fixture-node', name: 'Fake VM ' + vmid, status: 'running' })), 'fixture'); store.assign('alice', 'pve-740', 'fixture'); store.assign('bobby', 'pve-741', 'fixture');
  const code = tokenHash(newToken()), uuid = '12345678-1234-1234-1234-123456789abc'; store.issueEnrollment('pve-740', uuid, code, 'fixture'); store.consumeEnrollment(code, uuid, { hostname: 'Fixture Host', version: '0.2.49', tokenHash: tokenHash(tokens.host) });
  const server = await buildServer({ store, vault, publicOrigin: origin, proxmox: { inventory: async () => [] }, relayOptions: { heartbeatMs: 1000, authorizationMs: 20 }, logger: { error: value => logs.push(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve)); const base = 'http://127.0.0.1:' + server.address().port;
  t.after(async () => { for (const ws of sockets) ws.terminate(); await new Promise(resolve => server.close(resolve)); store.close(); assert.deepEqual(logs, []); });
  const open = async (path, authorization) => { const ws = await socket(base.replace(/^http/, 'ws') + path, authorization); sockets.push(ws); return ws; };
  const call = async (path, { method = 'GET', data, as = 'alice', headers = {} } = {}) => {
    const account = /^\/admin\/users\/([^/]+)(?:\/password|\/enabled)?$/.exec(path);
    if (account && ['PUT', 'DELETE'].includes(method)) data = { ...data, expectedAccountId: store.findUser(decodeURIComponent(account[1])).id };
    const response = await fetch(base + '/api/v1' + path, { method, headers: { Origin: origin, ...(data !== undefined ? { 'Content-Type': 'application/json' } : {}), ...(as === 'admin' ? { Cookie: '__Secure-multitap-admin=' + tokens.admin, 'X-CSRF-Token': vault.csrf(tokens.admin) } : { Authorization: 'Bearer ' + tokens[as] }), ...headers }, body: data === undefined ? undefined : JSON.stringify(data) }); return { status: response.status, body: await response.json() };
  };
  const host = await open('/api/v1/hosts/channel', 'Host ' + tokens.host); host.json({ type: 'host.capabilities', capabilities }); assert.equal((await host.take(v => v.type === 'host.capabilities.ack')).accepted, true);
  return { store, tokens, host, call, open };
}

test('administrator API keeps the account-wide device policy field as an update of every current assignment, defaults deny and keeps clipboard separate', async t => {
  const f = await fixture(t);
  const get = await f.call('/admin/users/alice', { as: 'admin' }); assert.equal(get.status, 200);
  assert.deepEqual(get.body.user.assignments, [{ desktopId: 'pve-740', clipboardPolicy: clipDeny, devicePolicy: deny, soundCardAdd: false }]);
  assert.equal('rdp_redirections' in get.body.user, false); assert.equal('devicePolicy' in get.body.user, false, 'no account-wide value is reported');
  for (const headers of [{ 'X-CSRF-Token': '' }, { Origin: 'https://other.example' }]) assert.equal((await f.call('/admin/users/alice', { method: 'PUT', data: { devicePolicy: all }, as: 'admin', headers })).status, 403);
  assert.equal((await f.call('/admin/users/alice', { method: 'PUT', data: { devicePolicy: all } })).status, 401, 'a client bearer session cannot administer');
  for (const policy of [null, {}, [], true, { drives: true }, { ...deny, drives: 'yes' }, { ...deny, clipboard: true }, { ...deny, audioCapture: false, cameras: false }]) {
    const result = await f.call('/admin/users/alice', { method: 'PUT', data: { devicePolicy: policy }, as: 'admin' }); assert.equal(result.status, 400); assert.equal(result.body.error.code, 'invalid_device_policy');
  }
  const before = { ...f.store.findUser('alice') }, granted = { ...deny, drives: true, serialPorts: true };
  assert.equal((await f.call('/admin/users/alice', { method: 'PUT', data: { devicePolicy: granted }, as: 'admin' })).status, 200);
  assert.deepEqual({ ...f.store.findUser('alice') }, before, 'identity, credentials and the account row are untouched');
  assert.deepEqual((await f.call('/admin/users/alice', { as: 'admin' })).body.user.assignments, [{ desktopId: 'pve-740', clipboardPolicy: clipDeny, devicePolicy: granted, soundCardAdd: false }]);
  assert.deepEqual((await f.call('/admin/inventory', { as: 'admin' })).body.assignments.find(a => a.username === 'alice').devicePolicy, granted);
  assert.deepEqual(f.store.devicePolicy(before.id, 'pve-740'), granted);
  assert.deepEqual(f.store.clipboardPolicy(before.id, 'pve-740'), clipDeny);
  assert.deepEqual(f.store.devicePolicy(f.store.findUser('bobby').id, 'pve-741'), deny, 'another account is unaffected');
  // A new account has no assignment yet, so an account-wide value grants nothing later.
  assert.equal((await f.call('/admin/users', { method: 'POST', data: { username: 'granted-user', password: 'Fixture-password-1234', passwordConfirmation: 'Fixture-password-1234', devicePolicy: { ...deny, printers: true } }, as: 'admin' })).status, 201);
  assert.equal((await f.call('/admin/users', { method: 'POST', data: { username: 'invalid-user', password: 'Fixture-password-1234', passwordConfirmation: 'Fixture-password-1234', devicePolicy: { drives: true } }, as: 'admin' })).body.error.code, 'invalid_device_policy');
  assert.equal(f.store.findUser('invalid-user'), undefined);
  f.store.assign('granted-user', 'pve-741', 'fixture');
  assert.deepEqual(f.store.devicePolicy(f.store.findUser('granted-user').id, 'pve-741'), deny);
  assert.ok(f.store.auditLog().some(event => event.event === 'device_policy_changed' && event.subject === before.id + ':pve-740'));
  // A row written by an earlier version may carry the two bits of no permission:
  // a saved policy leaves them there, and saving the same policy changes nothing.
  const stored = () => f.store.db.prepare('SELECT rdp_redirections FROM assignments WHERE user_id=? AND desktop_id=?').get(before.id, 'pve-740').rdp_redirections;
  assert.equal(stored(), 1 | 64);
  f.store.db.prepare('UPDATE assignments SET rdp_redirections=? WHERE user_id=? AND desktop_id=?').run(1 | 64 | 16 | 32, before.id, 'pve-740');
  assert.deepEqual(f.store.devicePolicy(before.id, 'pve-740'), granted, 'the two bits are not read');
  const events = () => f.store.auditLog().filter(event => event.event === 'device_policy_changed').length, count = events();
  assert.equal((await f.call('/admin/users/alice', { method: 'PUT', data: { devicePolicy: granted }, as: 'admin' })).status, 200);
  assert.equal(stored(), 1 | 64 | 16 | 32); assert.equal(events(), count, 'the same policy is no change');
  assert.equal((await f.call('/admin/users/alice', { method: 'PUT', data: { devicePolicy: { ...deny, printers: true } }, as: 'admin' })).status, 200);
  assert.equal(stored(), 2 | 16 | 32); assert.equal(events(), count + 1);
});
