import test from 'node:test';
import assert from 'node:assert/strict';
import { once } from 'node:events';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword, tokenHash, newToken } from '../src/security.mjs';

const origin = 'https://session-access.fixture.invalid', password = 'Only-a-synthetic-session-fixture-42!';
let passwordHash;
async function fixture(t, { shortIdle = false } = {}) {
  passwordHash ??= await hashPassword(password);
  const store = new Store(':memory:'), vault = new Vault(Buffer.alloc(32, 19)), sockets = [];
  const userId = store.addUser({ name: 'fixture-user', passwordHash });
  const adminId = store.addUser({ name: 'fixture-admin', passwordHash, role: 'admin' });
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://fixture.invalid', tokenId: 'fixture@pve!test', secret: 'synthetic' }), adminId);
  store.saveSetting('proxmox_initialized', '1', adminId);
  let clock = Date.now();
  const server = await buildServer({ store, vault, publicOrigin: origin, sessionAccessOptions: { now: () => clock, ...(shortIdle ? { policy: { idleTimeoutMs: 60000, warningMs: 10000 } } : {}) },
    desktopEventOptions: { tickMs: 10, authorizationMs: 10 }, relayOptions: { authorizationMs: 10 }, logger: { error: error => assert.fail(error) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { for (const ws of sockets) ws.terminate(); await new Promise(resolve => server.close(resolve)); store.close(); });
  const base = 'http://127.0.0.1:' + server.address().port;
  const call = async (path, { data, token, intent, method, requestOrigin = origin } = {}) => {
    const response = await fetch(base + '/api/v1' + path, { method: method ?? (data === undefined ? 'GET' : 'POST'), headers: { Origin: requestOrigin,
      ...(data === undefined ? {} : { 'Content-Type': 'application/json' }), ...(token ? { Authorization: 'Bearer ' + token } : {}),
      ...(intent ? { 'X-Multitap-Session-Policy': 'windows-pc-v1', 'X-Multitap-Login-Intent': intent } : {}) }, body: data === undefined ? undefined : JSON.stringify(data) });
    return { status: response.status, body: await response.json(), headers: response.headers };
  };
  const login = (intent = 'confirmable-v1') => call('/auth/login', { data: { username: 'fixture-user', password }, intent });
  const confirm = challengeToken => call('/auth/login/confirm', { data: { challengeToken, confirm: true }, intent: 'confirmable-v1' });
  async function watch(token) {
    const ws = new WebSocket(base.replace('http:', 'ws:') + '/api/v1/events', { headers: { Origin: origin, Authorization: 'Bearer ' + token } });
    sockets.push(ws); await once(ws, 'open'); return ws;
  }
  return { store, vault, userId, adminId, server, call, login, confirm, watch, advance: ms => { clock += ms; }, now: () => clock };
}
const challengeOf = response => { assert.equal(response.status, 409); assert.equal(response.body.error.code, 'login_confirmation_required'); return response.body.error.details.challengeToken; };

test('actual HTTP first login returns a persisted idle policy and duplicate cancellation leaves the existing login alive', async t => {
  const f = await fixture(t), first = await f.login(); assert.equal(first.status, 200);
  assert.equal(first.body.sessionAccess.protocol, 'session-access-v1'); assert.equal(first.body.sessionAccess.idleTimeoutMs, 1800000);
  assert.ok(Number.isFinite(Date.parse(first.body.sessionAccess.idleDeadline)));
  challengeOf(await f.login());
  assert.equal((await f.call('/auth/me', { token: first.body.sessionToken })).status, 200);
  assert.equal(f.store.db.prepare("SELECT count(*) n FROM sessions WHERE purpose='client'").get().n, 1);
});

test('confirmed takeover atomically replaces only this account client session and carries the real reason on an existing socket', async t => {
  const f = await fixture(t), first = await f.login(), ws = await f.watch(first.body.sessionToken);
  const adminToken = newToken(); f.store.createSession(f.store.findUser('fixture-admin'), tokenHash(adminToken), Date.now(), 3600000, 'admin');
  const challenge = challengeOf(await f.login()); assert.equal(ws.readyState, WebSocket.OPEN);
  const closed = once(ws, 'close'), next = await f.confirm(challenge); assert.equal(next.status, 200);
  const [code, reason] = await closed; assert.equal(code, 1008); assert.equal(reason.toString(), 'pc_login_replaced');
  assert.equal((await f.call('/auth/me', { token: first.body.sessionToken })).body.error.code, 'pc_login_replaced');
  assert.equal((await f.call('/auth/me', { token: next.body.sessionToken })).status, 200);
  assert.equal(f.store.authenticate(tokenHash(adminToken), Date.now(), 'admin')?.id, f.adminId);
  assert.equal((await f.confirm(challenge)).status, 409);
});

test('stale confirmation after another server-owned session appears never evicts either current session', async t => {
  const f = await fixture(t), first = await f.login(), challenge = challengeOf(await f.login());
  const extra = newToken(); f.store.createSession(f.store.findUser('fixture-user'), tokenHash(extra), Date.now(), 3600000, 'client');
  const before = f.store.db.prepare('SELECT token_hash FROM sessions ORDER BY token_hash').all();
  assert.equal((await f.confirm(challenge)).body.error.code, 'login_confirmation_changed');
  assert.deepEqual(f.store.db.prepare('SELECT token_hash FROM sessions ORDER BY token_hash').all(), before);
  assert.equal((await f.call('/auth/me', { token: first.body.sessionToken })).status, 200);
});

test('polling and renewal do not count as activity, including an attached authenticated watcher', async t => {
  const f = await fixture(t, { shortIdle: true }), first = await f.login(), token = first.body.sessionToken, ws = await f.watch(token);
  f.advance(59000);
  assert.equal((await f.call('/auth/me', { token })).status, 200);
  const renewed = await fetch('http://127.0.0.1:' + f.server.address().port + '/api/v1/auth/renew', { method: 'POST', headers: { Origin: origin, Authorization: 'Bearer ' + token } });
  assert.equal(renewed.status, 200); await renewed.json();
  const closed = once(ws, 'close'); f.advance(1001);
  const ended = await f.call('/auth/me', { token }); assert.equal(ended.status, 401); assert.equal(ended.body.error.code, 'session_idle_timeout');
  const [code, reason] = await closed; assert.equal(code, 1008); assert.equal(reason.toString(), 'session_idle_timeout');
});

test('accepted authenticated UI input moves the real persisted deadline; activity arriving after expiry cannot revive it', async t => {
  const f = await fixture(t, { shortIdle: true }), first = await f.login(), token = first.body.sessionToken;
  f.advance(50000);
  const activity = await f.call('/auth/activity', { data: {}, token }); assert.equal(activity.status, 200);
  assert.equal(Date.parse(activity.body.idleDeadline), f.now() + 60000);
  assert.equal(f.store.db.prepare('SELECT last_input_at FROM session_activity WHERE token_hash=?').get(tokenHash(token)).last_input_at, f.now());
  f.advance(50000); assert.equal((await f.call('/auth/me', { token })).status, 200);
  f.advance(10001);
  const late = await f.call('/auth/activity', { data: {}, token }); assert.equal(late.status, 401); assert.equal(late.body.error.code, 'session_idle_timeout');
  assert.equal(f.store.authenticate(tokenHash(token)), undefined);
});

test('a saved-password recovery attempt cannot silently undo an idle logout', async t => {
  const f = await fixture(t, { shortIdle: true }), first = await f.login();
  f.advance(60001); assert.equal((await f.call('/auth/me', { token: first.body.sessionToken })).body.error.code, 'session_idle_timeout');
  const automatic = await f.login('recovery-v1'); assert.equal(automatic.status, 409); assert.equal(automatic.body.error.code, 'session_reauthentication_required');
  assert.equal((await f.login()).status, 200, 'a fresh explicit password login remains possible');
});

test('the authenticated idle toggle disables countdown and re-enabling starts a fresh server deadline', async t => {
  const f = await fixture(t, { shortIdle: true }), first = await f.login(), token = first.body.sessionToken;
  const off = await f.call('/auth/idle', { method: 'PUT', data: { enabled: false }, token });
  assert.equal(off.status, 200); assert.equal(off.body.idleEnabled, false); assert.equal(off.body.idleDeadline, null); assert.equal(off.body.warningAt, null);
  f.advance(3600000); assert.equal((await f.call('/auth/me', { token })).status, 200);
  const on = await f.call('/auth/idle', { method: 'PUT', data: { enabled: true }, token });
  assert.equal(on.status, 200); assert.equal(on.body.idleEnabled, true); assert.equal(Date.parse(on.body.idleDeadline), f.now() + 60000);
  f.advance(60001); assert.equal((await f.call('/auth/me', { token })).body.error.code, 'session_idle_timeout');
});

test('disabled idle logout still permits explicit login replacement and never bypasses the absolute expiry', async t => {
  const f = await fixture(t), first = await f.login(), token = first.body.sessionToken;
  assert.equal((await f.call('/auth/idle', { method: 'PUT', data: { enabled: false }, token })).status, 200);
  const next = await f.confirm(challengeOf(await f.login())); assert.equal(next.status, 200);
  assert.equal((await f.call('/auth/me', { token })).body.error.code, 'pc_login_replaced');
  const current = next.body.sessionToken;
  assert.equal((await f.call('/auth/idle', { method: 'PUT', data: { enabled: false }, token: current })).status, 200);
  f.advance(9 * 3600000); assert.equal((await f.call('/auth/me', { token: current })).body.error.code, 'session_expired');
});
