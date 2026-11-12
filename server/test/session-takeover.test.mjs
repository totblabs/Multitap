import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';

const origin = 'https://takeover-fixture.example';
const password = 'Takeover-fixture-password-only!';
const machineUuid = '12345678-1234-1234-1234-123456789abc';

async function fixture(t, { aliceAdmin = false } = {}) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const hash = await hashPassword(password);
  store.addUser({ name: 'admin', passwordHash: hash, role: 'admin' });
  store.addUser({ name: 'alice', passwordHash: hash, role: aliceAdmin ? 'admin' : 'user' });
  store.addUser({ name: 'bobby', passwordHash: hash });
  store.syncProxmox([170, 149].map(vmid => ({ vmid, node: 'pve', name: `VM ${vmid}`, status: 'running' })), 'fixture');
  store.assign('alice', 'pve-170'); store.assign('bobby', 'pve-170');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve-fixture.example', tokenId: 'fixture@pve!broker', secret: 'only-fixture-secret' }), 'fixture');
  const proxmox = { machineIdentity: async () => ({ machineUuid }) };
  const server = await buildServer({ store, vault, proxmox, publicOrigin: origin, relayOptions: { authorizationMs: 20, heartbeatMs: 200 }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const base = `http://127.0.0.1:${server.address().port}`;
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  async function call(path, { method = 'GET', data, token, cookie, csrf } = {}) {
    const response = await fetch(base + '/api/v1' + path, { method,
      headers: { ...(token ? { Authorization: 'Bearer ' + token } : {}), ...(cookie ? { Cookie: cookie } : {}), ...(method !== 'GET' ? { 'Content-Type': 'application/json', Origin: origin, ...(csrf ? { 'X-CSRF-Token': csrf } : {}) } : {}) },
      body: data === undefined ? undefined : JSON.stringify(data) });
    return { status: response.status, body: await response.json() };
  }
  const login = async name => (await call('/auth/login', { method: 'POST', data: { username: name, password } })).body.sessionToken;
  const adminLogin = await fetch(base + '/api/v1/admin/login', { method: 'POST', headers: { 'Content-Type': 'application/json', Origin: origin }, body: JSON.stringify({ username: 'admin', password }) });
  const admin = { cookie: adminLogin.headers.get('set-cookie').split(';')[0], csrf: (await adminLogin.json()).csrfToken };
  const code = (await call('/admin/desktops/pve-170/enrollment', { ...admin, method: 'POST', data: {} })).body.code;
  const enrolled = await call('/hosts/enroll', { method: 'POST', data: { code, hostname: 'Fixture VM', machineUuid, version: '0.1.0' } });
  assert.equal(enrolled.status, 201);
  const wsUrl = path => base.replace('http:', 'ws:') + path;
  const host = await socket(wsUrl('/api/v1/hosts/channel'), 'Host ' + enrolled.body.hostToken);
  return { store, call, login, wsUrl, host };
}

async function socket(url, authorization) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url, { headers: { Authorization: authorization }, perMessageDeflate: false });
    ws.once('error', reject);
    ws.once('unexpected-response', (_req, response) => { response.resume(); reject(Object.assign(new Error('upgrade_rejected'), { status: response.statusCode })); });
    ws.once('open', () => {
      const queue = [], waiters = [];
      ws.on('message', (data, binary) => {
        const value = binary ? data : JSON.parse(data.toString('utf8'));
        const index = waiters.findIndex(waiter => waiter.predicate(value));
        if (index >= 0) { const waiter = waiters.splice(index, 1)[0]; clearTimeout(waiter.timer); waiter.resolve(value); }
        else queue.push(value);
      });
      ws.take = (predicate = () => true) => {
        const index = queue.findIndex(predicate);
        if (index >= 0) return Promise.resolve(queue.splice(index, 1)[0]);
        return new Promise((resolve, reject) => {
          const waiter = { predicate, resolve, timer: setTimeout(() => reject(new Error('message_timeout')), 2000) }; waiters.push(waiter);
        });
      };
      resolve(ws);
    });
  });
}

async function attach(f, token) {
  const connection = await f.call('/desktops/pve-170/connect', { method: 'POST', token });
  assert.equal(connection.status, 200);
  const client = await socket(f.wsUrl('/api/v1/sessions/' + connection.body.sessionId), 'Bearer ' + token);
  assert.equal((await f.host.take(value => value.type === 'session.start')).sessionId, connection.body.sessionId);
  return { client, sessionId: connection.body.sessionId };
}

test('Explicit duplicate login retires the same account controller before the new login connects', async t => {
  const f = await fixture(t);
  const first = await f.login('alice');
  const old = await attach(f, first);
  const pending = await f.call('/auth/login', { method: 'POST', data: { username: 'alice', password } });
  assert.equal(pending.status, 409); assert.equal(pending.body.error.code, 'login_confirmation_required');
  assert.equal((await f.call('/auth/me', { token: first })).status, 200, 'pending confirmation preserves the existing login');
  assert.equal(old.client.readyState, WebSocket.OPEN);
  const admitted = await f.call('/auth/login/confirm', { method: 'POST', data: { challengeToken: pending.body.error.details.challengeToken, confirm: true } });
  assert.equal(admitted.status, 200); const second = admitted.body.sessionToken;
  assert.equal((await f.call('/auth/me', { token: first })).body.error.code, 'pc_login_replaced');
  assert.deepEqual(await old.client.take(value => value.type === 'session.ended'), { type: 'session.ended', reason: 'pc_login_replaced' });
  const stop = await f.host.take(value => value.type === 'session.stop');
  assert.equal(stop.sessionId, old.sessionId); assert.equal(stop.reason, 'pc_login_replaced');
  // A legacy Host without stop-v1 has no cleanup barrier: admission is immediate.
  await attach(f, second);
  assert.equal((await f.call('/auth/login/confirm', { method: 'POST', data: { challengeToken: pending.body.error.details.challengeToken, confirm: true } })).status, 409, 'confirmation is single use');
});

// Any account assigned to the VM may end another
// account's controller after confirming (left running at home, then connect).
test('Another assigned user may end the controller; it is offered and then released', async t => {
  const f = await fixture(t);
  const alice = await f.login('alice'), bobby = await f.login('bobby');
  await attach(f, alice);
  const busy = await f.call('/desktops/pve-170/connect', { method: 'POST', token: bobby });
  assert.deepEqual(busy.body.error.details, { reason: 'controller_in_use', takeover: true });
  const released = await f.call('/desktops/pve-170/takeover', { method: 'POST', token: bobby });
  assert.equal(released.status, 200); assert.equal(released.body.released, true);
  assert.equal((await f.host.take(value => value.type === 'session.stop')).reason, 'taken_over');
});

test('An administrator assigned to the VM may end another user\'s controller', async t => {
  const f = await fixture(t, { aliceAdmin: true });
  const alice = await f.login('alice'), bobby = await f.login('bobby');
  const old = await attach(f, bobby);
  assert.equal((await f.call('/desktops/pve-170/connect', { method: 'POST', token: alice })).body.error.details.takeover, true);
  assert.equal((await f.call('/desktops/pve-170/takeover', { method: 'POST', token: alice })).body.released, true);
  // The ended stream is told which account took it,
  // as the takeover offer names the controller to the taker.
  const ended = await old.client.take(value => value.type === 'session.ended');
  assert.deepEqual({ reason: ended.reason, detail: ended.detail, by: ended.by }, { reason: 'taken_over', detail: 'other_account', by: 'alice' });
  await attach(f, alice);
});

// Who is using the desktop: the account name,
// the same one the takeover card shows; nothing else about that person or connection.
test('The desktop list says the VM is in use, whether by the viewer\'s own account, and otherwise by which account', async t => {
  const f = await fixture(t);
  const alice = await f.login('alice'), bobby = await f.login('bobby');
  const listed = async token => (await f.call('/desktops', { token })).body.desktops.find(desktop => desktop.id === 'pve-170');
  assert.equal('activeSession' in await listed(alice), false);
  const old = await attach(f, alice);
  assert.deepEqual((await listed(alice)).activeSession, { mine: true });
  assert.deepEqual((await listed(bobby)).activeSession, { mine: false, account: 'alice' }, 'who is using it');
  assert.deepEqual((await f.call('/desktops/pve-170', { token: bobby })).body.desktop.activeSession, { mine: false, account: 'alice' });
  assert.deepEqual(Object.keys((await listed(bobby)).activeSession).sort(), ['account', 'mine'], 'only the account name, no time, address or device');
  old.client.close();
  await f.host.take(value => value.type === 'session.stop');
  assert.equal('activeSession' in await listed(bobby), false);
});

test('Takeover is idempotent, assignment-bound and rejects request fields', async t => {
  const f = await fixture(t);
  const alice = await f.login('alice');
  const idle = await f.call('/desktops/pve-170/takeover', { method: 'POST', token: alice });
  assert.equal(idle.status, 200); assert.equal(idle.body.released, false); assert.equal(idle.body.sessionCleanup.pending, false);
  assert.equal((await f.call('/desktops/pve-149/takeover', { method: 'POST', token: alice })).status, 404);
  assert.equal((await f.call('/desktops/pve-170/takeover', { method: 'POST', token: alice, data: { force: true } })).body.error.code, 'unexpected_field');
  assert.equal((await f.call('/desktops/pve-170/takeover', { token: alice })).status, 405);
  assert.equal((await f.call('/desktops/pve-170/takeover', { method: 'POST' })).status, 401);
});
