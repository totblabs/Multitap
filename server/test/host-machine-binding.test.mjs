// host-machine-v1: a full clone of a registered VM carries the original's Host token. The Host names the
// SMBIOS UUID it runs on when it opens its channel; the broker refuses a UUID other than the one the Host
// was approved on (403, X-Multitap-Host-Error: host_machine_changed) before the copy can take the channel,
// and records it once. A Host that sends no UUID (released versions) connects as before.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { eventNames } from '../admin/js/labels.js';

const origin = 'https://host-machine-fixture.example';
const password = 'Host-machine-fixture-password!';
const machineUuid = '12345678-1234-1234-1234-123456789ae1';
const cloneUuid = '87654321-4321-4321-4321-cba987654321';

async function fixture(t) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  store.addUser({ name: 'admin', passwordHash: await hashPassword(password), role: 'admin' });
  store.syncProxmox([{ vmid: 182, node: 'pve', name: 'VM 182', status: 'running' }], 'fixture');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve-fixture.example', tokenId: 'fixture@pve!broker', secret: 'only-fixture-secret' }), 'fixture');
  const proxmox = { machineIdentity: async () => ({ machineUuid }) };
  const server = await buildServer({ store, vault, proxmox, publicOrigin: origin, relayOptions: { authorizationMs: 20, heartbeatMs: 200 }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const base = `http://127.0.0.1:${server.address().port}`;
  const sockets = [];
  t.after(async () => { for (const ws of sockets) ws.terminate(); await new Promise(resolve => server.close(resolve)); store.close(); });
  async function call(path, { method = 'GET', data, cookie, csrf } = {}) {
    const response = await fetch(base + '/api/v1' + path, { method,
      headers: { ...(cookie ? { Cookie: cookie } : {}), ...(method !== 'GET' ? { 'Content-Type': 'application/json', Origin: origin, ...(csrf ? { 'X-CSRF-Token': csrf } : {}) } : {}) },
      body: data === undefined ? undefined : JSON.stringify(data) });
    return { status: response.status, body: await response.json() };
  }
  const login = await fetch(base + '/api/v1/admin/login', { method: 'POST', headers: { 'Content-Type': 'application/json', Origin: origin }, body: JSON.stringify({ username: 'admin', password }) });
  const admin = { cookie: login.headers.get('set-cookie').split(';')[0], csrf: (await login.json()).csrfToken };
  async function enroll() {
    const code = (await call('/admin/desktops/pve-182/enrollment', { ...admin, method: 'POST', data: {} })).body.code;
    const enrolled = await call('/hosts/enroll', { method: 'POST', data: { code, hostname: 'Fixture VM', machineUuid, version: '1.0.60' } });
    assert.equal(enrolled.status, 201);
    return enrolled.body.hostToken;
  }
  // Resolves { ws } when the channel opens, { status, error } when the upgrade is refused.
  function channel(token, machine) {
    return new Promise((resolve, reject) => {
      const headers = { Authorization: 'Host ' + token, ...(machine === undefined ? {} : { 'X-Multitap-Host-Machine': machine }) };
      const ws = new WebSocket(base.replace('http:', 'ws:') + '/api/v1/hosts/channel', { headers, perMessageDeflate: false });
      ws.once('open', () => { sockets.push(ws); resolve({ ws }); });
      ws.once('unexpected-response', (request, response) => { resolve({ status: response.statusCode, error: response.headers['x-multitap-host-error'] ?? null }); request.destroy(); });
      ws.once('error', error => { if (ws.readyState !== WebSocket.CLOSED) reject(error); });
    });
  }
  const refusals = () => store.db.prepare("SELECT actor,subject FROM audit_events WHERE event='host_machine_changed' ORDER BY id").all().map(row => ({ ...row }));
  return { store, enroll, channel, refusals };
}

test('the machine the Host was approved on connects', async t => {
  const f = await fixture(t);
  const token = await f.enroll();
  const opened = await f.channel(token, machineUuid.toUpperCase());
  assert.ok(opened.ws, JSON.stringify(opened));
  assert.deepEqual(f.refusals(), []);
});

test('a copy on another machine is refused by name, recorded once, and cannot lock out the original', async t => {
  const f = await fixture(t);
  const token = await f.enroll();
  const hostId = f.store.db.prepare('SELECT id FROM hosts').get().id;

  // The copy dials first: refused before it can hold the channel (no 409 for the original afterwards).
  const copy = await f.channel(token, cloneUuid);
  assert.deepEqual(copy, { status: 403, error: 'host_machine_changed' });
  assert.deepEqual(f.refusals(), [{ actor: hostId, subject: 'pve-182' }]);

  const original = await f.channel(token, machineUuid);
  assert.ok(original.ws, 'the original still connects');

  // While the original is connected the copy still gets its own answer, not 409.
  const again = await f.channel(token, cloneUuid);
  assert.deepEqual(again, { status: 403, error: 'host_machine_changed' });
  assert.equal(f.refusals().length, 1, 'one row per Host per 10 minutes');
  assert.equal(original.ws.readyState, WebSocket.OPEN, 'the refusal leaves the original alone');

  // The admin console names it in both languages.
  assert.ok(Object.hasOwn(eventNames, 'host_machine_changed'));
});

test('a Host that sends no machine (released versions) keeps today\'s behaviour', async t => {
  const f = await fixture(t);
  const token = await f.enroll();
  const first = await f.channel(token);
  assert.ok(first.ws, JSON.stringify(first));
  // Today's behaviour includes the second connection's 409.
  const second = await f.channel(token);
  assert.deepEqual(second, { status: 409, error: null });
  assert.deepEqual(f.refusals(), []);
});

test('a malformed machine value is refused without an audit row', async t => {
  const f = await fixture(t);
  const token = await f.enroll();
  assert.deepEqual(await f.channel(token, 'not-a-uuid'), { status: 400, error: null });
  assert.deepEqual(f.refusals(), []);
});

test('re-enrollment after deletion binds the new registration to its own machine', async t => {
  const f = await fixture(t);
  const first = await f.enroll();
  const second = await f.enroll();
  assert.deepEqual(await f.channel(first, machineUuid), { status: 401, error: null }, 'the replaced token is gone');
  const opened = await f.channel(second, machineUuid);
  assert.ok(opened.ws);
});

test('hostMachineUuid reads the approved machine for local PCs as well', () => {
  const store = new Store(':memory:');
  try {
    assert.equal(store.hostMachineUuid('missing'), null);
    store.db.prepare("INSERT INTO desktops(id,name,kind,description,os,enabled,created_at) VALUES('pc-fixture','PC','pc','fixture','Windows',1,?)").run(Date.now());
    store.db.prepare('INSERT INTO hosts(id,desktop_id,token_hash,hostname,machine_uuid,version,created_at) VALUES(?,?,?,?,?,?,?)')
      .run('00000000-0000-4000-8000-000000000001', 'pc-fixture', 'h'.repeat(64), 'PC', 'ABCDEF01-2345-6789-ABCD-EF0123456789', '1.0.60', Date.now());
    assert.equal(store.hostMachineUuid('00000000-0000-4000-8000-000000000001'), 'abcdef01-2345-6789-abcd-ef0123456789');
  } finally { store.close(); }
});
