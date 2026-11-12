import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { cursorComposeProtocol, cursorComposeCapability, requestedCursorCompose, negotiateCursorCompose, validCursorCompose } from '../src/cursor.mjs';

// Cursor in the video (cursor-compose-v1): while the client holds the mouse
// (게임 마우스) it asks the Host to draw the VM's real cursor into the video and
// shows none of its own. Exact negotiation (only with cursor-v1, one output),
// the relayed request, released peers unchanged. Real broker and WebSockets,
// fake Host/client.
const origin = 'https://cursor-compose-fixture.example';
const password = 'Cursor-compose-fixture-password-only!';
const machineUuid = '12345678-1234-1234-1234-123456789ac4';
const releasedHost = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true };
const cursorHost = { ...releasedHost, cursorProtocol: 'cursor-v1' };
const echoHost = { ...cursorHost, cursorEchoProtocol: 'cursor-echo-v1' };
const composeHost = { ...echoHost, cursorComposeProtocol: 'cursor-compose-v1' };
const streamInfo = { type: 'stream.info', codec: 'h264', width: 1920, height: 1080, sourceWidth: 1920, sourceHeight: 1080, fpsTarget: 60, audio: { sampleRate: 48000, channels: 2, format: 's16le', available: true } };
// What a new client asks where the inventory shows both.
const asked = { cursorProtocol: 'cursor-v1', cursorEchoProtocol: 'cursor-echo-v1', cursorComposeProtocol: 'cursor-compose-v1' };

async function fixture(t) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const hash = await hashPassword(password);
  store.addUser({ name: 'admin', passwordHash: hash, role: 'admin' });
  store.addUser({ name: 'alice', passwordHash: hash, role: 'user' });
  store.syncProxmox([{ vmid: 174, node: 'pve', name: 'VM 174', status: 'running' }], 'fixture');
  store.assign('alice', 'pve-174');
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
  const adminLogin = await fetch(base + '/api/v1/admin/login', { method: 'POST', headers: { 'Content-Type': 'application/json', Origin: origin }, body: JSON.stringify({ username: 'admin', password }) });
  const admin = { cookie: adminLogin.headers.get('set-cookie').split(';')[0], csrf: (await adminLogin.json()).csrfToken };
  const alice = (await call('/auth/login', { method: 'POST', data: { username: 'alice', password } })).body.sessionToken;
  const code = (await call('/admin/desktops/pve-174/enrollment', { ...admin, method: 'POST', data: {} })).body.code;
  const wsUrl = path => base.replace('http:', 'ws:') + path;
  async function host(capabilities) {
    const enrolled = await call('/hosts/enroll', { method: 'POST', data: { code, hostname: 'Fixture VM', machineUuid, version: '0.2.145' } });
    assert.equal(enrolled.status, 201);
    const ws = await socket(wsUrl('/api/v1/hosts/channel'), 'Host ' + enrolled.body.hostToken);
    ws.json({ type: 'host.capabilities', capabilities });
    assert.equal((await ws.take(value => value.type === 'host.capabilities.ack')).accepted, true);
    return ws;
  }
  const connect = extra => call('/desktops/pve-174/connect', { method: 'POST', token: alice, data: { mode: 'stream', ...extra } });
  const client = sessionId => socket(wsUrl('/api/v1/sessions/' + sessionId), 'Bearer ' + alice);
  async function stream(hostSocket, extra = {}) {
    const response = await connect(extra);
    assert.equal(response.status, 200, JSON.stringify(response.body));
    const controller = await client(response.body.sessionId);
    const start = await hostSocket.take(value => value.type === 'session.start');
    hostSocket.json(streamInfo); await controller.take(value => value.type === 'stream.info');
    return { response, controller, start, sessionId: response.body.sessionId };
  }
  return { call, alice, host, connect, stream };
}
async function socket(url, authorization) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url, { headers: { Authorization: authorization }, perMessageDeflate: false });
    ws.once('error', reject);
    ws.once('open', () => {
      const queue = [], waiters = [];
      ws.on('message', (data, binary) => {
        const value = binary ? data : JSON.parse(data.toString('utf8'));
        const index = waiters.findIndex(waiter => waiter.predicate(value));
        if (index >= 0) { const waiter = waiters.splice(index, 1)[0]; clearTimeout(waiter.timer); waiter.resolve(value); } else queue.push(value);
      });
      ws.queue = queue;
      ws.take = (predicate = () => true) => {
        const index = queue.findIndex(predicate);
        if (index >= 0) return Promise.resolve(queue.splice(index, 1)[0]);
        return new Promise((done, fail) => { const waiter = { predicate, resolve: done, timer: setTimeout(() => fail(new Error('message_timeout')), 2000) }; waiters.push(waiter); });
      };
      ws.json = value => ws.send(JSON.stringify(value));
      resolve(ws);
    });
  });
}
const closed = ws => new Promise(resolve => { if (ws.readyState === WebSocket.CLOSED) resolve(); else ws.once('close', resolve); });
const code = fn => { try { fn(); return 'accepted'; } catch (error) { return error.code; } };

test('Capability, request, negotiation and message rules are exact', () => {
  assert.equal(cursorComposeProtocol, 'cursor-compose-v1');
  assert.equal(cursorComposeCapability(composeHost), true);
  assert.equal(cursorComposeCapability({ ...cursorHost, cursorComposeProtocol: 'cursor-compose-v1' }), true, 'without the echo too');
  for (const value of [cursorHost, echoHost, { ...releasedHost, cursorComposeProtocol: 'cursor-compose-v1' }, { ...composeHost, cursorComposeProtocol: 'cursor-compose-v2' }, { ...composeHost, cursorComposeProtocol: true }, undefined, null])
    assert.equal(cursorComposeCapability(value), false, JSON.stringify(value));
  assert.equal(requestedCursorCompose('stream', 'cursor-v1', undefined), false);
  assert.equal(requestedCursorCompose('stream', undefined, undefined), false);
  assert.equal(requestedCursorCompose('stream', 'cursor-v1', 'cursor-compose-v1'), true);
  for (const [mode, protocol, compose] of [['rdp', 'cursor-v1', 'cursor-compose-v1'], ['stream', undefined, 'cursor-compose-v1'], ['stream', 'cursor-v1', 'cursor-compose-v2'], ['stream', 'cursor-v1', 'cursor-echo-v1'], ['stream', 'cursor-v1', ''], ['stream', 'cursor-v1', null], ['stream', 'cursor-v1', true]])
    assert.equal(code(() => requestedCursorCompose(mode, protocol, compose)), 'invalid_cursor_protocol', `${mode} ${protocol} ${compose}`);
  assert.equal(negotiateCursorCompose(true, true, { capabilities: composeHost }, null), true);
  assert.equal(negotiateCursorCompose(false, true, { capabilities: composeHost }, null), false, 'never granted unrequested');
  assert.equal(negotiateCursorCompose(true, false, { capabilities: composeHost }, null), false, 'never without cursor-v1 granted');
  assert.equal(negotiateCursorCompose(true, true, { capabilities: echoHost }, null), false, 'an echo-only Host: not granted, not refused');
  assert.equal(negotiateCursorCompose(true, true, { capabilities: composeHost }, [{ id: '0' }]), false, 'independent outputs have no compose');
  assert.equal(validCursorCompose({ type: 'cursor.compose', on: true }), true);
  assert.equal(validCursorCompose({ type: 'cursor.compose', on: false }), true);
  for (const value of [{ type: 'cursor.compose' }, { type: 'cursor.compose', on: 1 }, { type: 'cursor.compose', on: 'true' }, { type: 'cursor.compose', on: true, x: 2 }, { type: 'cursor.echo', on: true }, null, [], 'x'])
    assert.equal(validCursorCompose(value), false, JSON.stringify(value));
});

test('Granted: the inventory, start and response carry it, on and off reach the Host, a malformed request ends the session', async t => {
  const f = await fixture(t); const host = await f.host(composeHost);
  const inventory = await f.call('/desktops', { token: f.alice });
  assert.equal(inventory.body.desktops[0].host.capabilities.cursorComposeProtocol, 'cursor-compose-v1');
  assert.equal(inventory.body.desktops[0].host.capabilities.cursorEchoProtocol, 'cursor-echo-v1');
  const { response, controller, start, sessionId } = await f.stream(host, asked);
  assert.equal(response.body.cursorProtocol, 'cursor-v1');
  assert.equal(response.body.cursorComposeProtocol, 'cursor-compose-v1');
  assert.equal(response.body.cursorEchoProtocol, 'cursor-echo-v1', 'both may be granted; the client picks compose');
  assert.equal(start.cursorProtocol, 'cursor-v1');
  assert.equal(start.cursorComposeProtocol, 'cursor-compose-v1');
  controller.json({ type: 'cursor.compose', on: true });
  assert.deepEqual(await host.take(value => value.type === 'cursor.compose'), { type: 'cursor.compose', sessionId, on: true });
  controller.json({ type: 'cursor.compose', on: false });
  assert.deepEqual(await host.take(value => value.type === 'cursor.compose'), { type: 'cursor.compose', sessionId, on: false });
  // The echo of the same session still passes on its own.
  controller.json({ type: 'cursor.echo', on: false });
  assert.deepEqual(await host.take(value => value.type === 'cursor.echo'), { type: 'cursor.echo', sessionId, on: false });
  controller.json({ type: 'cursor.compose', on: 'yes' });
  assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_input');
  await closed(controller);
});

test('An echo-only Host, or a client that did not ask: no grant, no marker, the request ends the session', async t => {
  const f = await fixture(t); const old = await f.host(echoHost);
  const inventory = await f.call('/desktops', { token: f.alice });
  assert.equal(inventory.body.desktops[0].host.capabilities.cursorComposeProtocol, undefined);
  const { response, controller, start } = await f.stream(old, asked);
  assert.equal(response.body.cursorProtocol, 'cursor-v1', 'cursor-v1 itself is unchanged');
  assert.equal(response.body.cursorEchoProtocol, 'cursor-echo-v1', 'the echo is still granted: the client falls back to it');
  assert.equal(response.body.cursorComposeProtocol, undefined, 'not refused, only ungranted');
  assert.equal('cursorComposeProtocol' in start, false);
  controller.json({ type: 'cursor.compose', on: true });
  assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_input');
  await closed(controller);

  const g = await fixture(t); const capable = await g.host(composeHost);
  const plain = await g.stream(capable, { cursorProtocol: 'cursor-v1', cursorEchoProtocol: 'cursor-echo-v1' });
  assert.equal(plain.response.body.cursorComposeProtocol, undefined, 'never granted unrequested');
  assert.equal(plain.response.body.cursorEchoProtocol, 'cursor-echo-v1', 'a released client keeps its echo');
  assert.equal('cursorComposeProtocol' in plain.start, false);
  plain.controller.json({ type: 'cursor.compose', on: true });
  assert.equal((await plain.controller.take(value => value.type === 'session.ended')).reason, 'invalid_input');
  await closed(plain.controller);
});

test('A malformed connect field is refused before any session', async t => {
  const f = await fixture(t); await f.host(composeHost);
  for (const cursorComposeProtocol of ['cursor-compose-v2', 1, null, '']) {
    const response = await f.connect({ cursorProtocol: 'cursor-v1', cursorComposeProtocol });
    assert.equal(response.status, 400, String(cursorComposeProtocol)); assert.equal(response.body.error.code, 'invalid_cursor_protocol');
  }
  const alone = await f.connect({ cursorComposeProtocol: 'cursor-compose-v1' });
  assert.equal(alone.status, 400, 'compose needs cursor-v1 in the same request'); assert.equal(alone.body.error.code, 'invalid_cursor_protocol');
  const plain = await f.connect({});
  assert.equal(plain.status, 200, 'a released client request is unchanged'); assert.equal(plain.body.cursorComposeProtocol, undefined);
});
