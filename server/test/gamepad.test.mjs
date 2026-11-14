import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { gamepadProtocol, gamepadCapability, requestedGamepad, negotiateGamepad, validGamepadState, validGamepadRemove, gamepadRumble } from '../src/gamepad.mjs';

// Gamepads (gamepad-v1): the client sends each pad's whole state and its removal,
// the Host the rumble. Exact negotiation (ordinary streams with a Host that
// advertises it), the relayed messages both ways, released peers unchanged.
// Real broker and WebSockets, fake Host/client.
const origin = 'https://gamepad-fixture.example';
const password = 'Gamepad-fixture-password-only!';
const machineUuid = '12345678-1234-1234-1234-123456789ac6';
const releasedHost = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true };
const padHost = { ...releasedHost, gamepadProtocol: 'gamepad-v1' };
const streamInfo = { type: 'stream.info', codec: 'h264', width: 1920, height: 1080, sourceWidth: 1920, sourceHeight: 1080, fpsTarget: 60, audio: { sampleRate: 48000, channels: 2, format: 's16le', available: true } };
const asked = { gamepadProtocol: 'gamepad-v1' };
const pressed = { type: 'gamepad.state', pad: 0, buttons: 0x1000, lt: 255, rt: 0, lx: -32768, ly: 32767, rx: 0, ry: 0 };

async function fixture(t) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const hash = await hashPassword(password);
  store.addUser({ name: 'admin', passwordHash: hash, role: 'admin' });
  store.addUser({ name: 'alice', passwordHash: hash, role: 'user' });
  store.syncProxmox([{ vmid: 175, node: 'pve', name: 'VM 175', status: 'running' }], 'fixture');
  store.assign('alice', 'pve-175');
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
  const code = (await call('/admin/desktops/pve-175/enrollment', { ...admin, method: 'POST', data: {} })).body.code;
  const wsUrl = path => base.replace('http:', 'ws:') + path;
  async function host(capabilities) {
    const enrolled = await call('/hosts/enroll', { method: 'POST', data: { code, hostname: 'Fixture VM', machineUuid, version: '1.1.10' } });
    assert.equal(enrolled.status, 201);
    const ws = await socket(wsUrl('/api/v1/hosts/channel'), 'Host ' + enrolled.body.hostToken);
    ws.json({ type: 'host.capabilities', capabilities });
    assert.equal((await ws.take(value => value.type === 'host.capabilities.ack')).accepted, true);
    return ws;
  }
  const connect = extra => call('/desktops/pve-175/connect', { method: 'POST', token: alice, data: { mode: 'stream', ...extra } });
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
  assert.equal(gamepadProtocol, 'gamepad-v1');
  assert.equal(gamepadCapability(padHost), true);
  for (const value of [releasedHost, { ...releasedHost, gamepadProtocol: 'gamepad-v2' }, { ...releasedHost, gamepadProtocol: true }, undefined, null])
    assert.equal(gamepadCapability(value), false, JSON.stringify(value));
  assert.equal(requestedGamepad('stream', undefined), false);
  assert.equal(requestedGamepad('stream', 'gamepad-v1'), true);
  for (const [mode, value] of [['rdp', 'gamepad-v1'], ['stream', 'gamepad-v2'], ['stream', ''], ['stream', null], ['stream', true]])
    assert.equal(code(() => requestedGamepad(mode, value)), 'invalid_gamepad_protocol', `${mode} ${value}`);
  assert.equal(negotiateGamepad(true, { capabilities: padHost }, null), true);
  assert.equal(negotiateGamepad(false, { capabilities: padHost }, null), false, 'never granted unrequested');
  assert.equal(negotiateGamepad(true, { capabilities: releasedHost }, null), false, 'a released Host: not granted, not refused');
  assert.equal(negotiateGamepad(true, { capabilities: padHost }, [{ id: '0' }]), false, 'not on independent outputs');
  assert.equal(validGamepadState(pressed), true);
  assert.equal(validGamepadState({ ...pressed, pad: 3, buttons: 0xF7FF, rt: 255, rx: 32767, ry: -32768 }), true);
  for (const value of [{ ...pressed, pad: 4 }, { ...pressed, pad: -1 }, { ...pressed, buttons: 0x0800 }, { ...pressed, buttons: 0x10000 }, { ...pressed, lt: 256 }, { ...pressed, lx: 32768 },
    { ...pressed, ly: 1.5 }, { ...pressed, extra: 1 }, (({ ry, ...rest }) => rest)(pressed), { ...pressed, type: 'gamepad.remove' }, null, []])
    assert.equal(validGamepadState(value), false, JSON.stringify(value));
  assert.equal(validGamepadRemove({ type: 'gamepad.remove', pad: 2 }), true);
  for (const value of [{ type: 'gamepad.remove', pad: 4 }, { type: 'gamepad.remove' }, { type: 'gamepad.remove', pad: 0, x: 1 }, { type: 'gamepad.state', pad: 0 }])
    assert.equal(validGamepadRemove(value), false, JSON.stringify(value));
  assert.deepEqual(gamepadRumble({ type: 'gamepad.rumble', sessionId: 's', pad: 1, low: 128, high: 64 }), { type: 'gamepad.rumble', sessionId: 's', pad: 1, low: 128, high: 64 });
  for (const value of [{ type: 'gamepad.rumble', sessionId: 's', pad: 1, low: 256, high: 0 }, { type: 'gamepad.rumble', pad: 1, low: 1, high: 1 }, { type: 'gamepad.rumble', sessionId: 's', pad: 4, low: 1, high: 1 }, { type: 'gamepad.rumble', sessionId: 's', pad: 0, low: 1, high: 1, x: 1 }])
    assert.equal(gamepadRumble(value), null, JSON.stringify(value));
});

test('Granted: the inventory, start and response carry it, states and removal reach the Host, rumble reaches the client', async t => {
  const f = await fixture(t); const host = await f.host(padHost);
  const inventory = await f.call('/desktops', { token: f.alice });
  assert.equal(inventory.body.desktops[0].host.capabilities.gamepadProtocol, 'gamepad-v1');
  const { response, controller, start, sessionId } = await f.stream(host, asked);
  assert.equal(response.body.gamepadProtocol, 'gamepad-v1');
  assert.equal(start.gamepadProtocol, 'gamepad-v1');
  controller.json(pressed);
  assert.deepEqual(await host.take(value => value.type === 'gamepad.state'), { ...pressed, sessionId });
  controller.json({ type: 'gamepad.remove', pad: 0 });
  assert.deepEqual(await host.take(value => value.type === 'gamepad.remove'), { type: 'gamepad.remove', pad: 0, sessionId });
  host.json({ type: 'gamepad.rumble', sessionId, pad: 0, low: 200, high: 30 });
  assert.deepEqual(await controller.take(value => value.type === 'gamepad.rumble'), { type: 'gamepad.rumble', pad: 0, low: 200, high: 30 });
  // A rumble of another (retired) session is not relayed.
  host.json({ type: 'gamepad.rumble', sessionId: '00000000-0000-4000-8000-000000000000', pad: 0, low: 1, high: 1 });
  host.json({ type: 'gamepad.rumble', sessionId, pad: 1, low: 2, high: 2 });
  assert.equal((await controller.take(value => value.type === 'gamepad.rumble')).pad, 1);
  controller.json({ ...pressed, lt: 300 });
  assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_input');
  await closed(controller);
});

test('A released Host, or a client that did not ask: no grant, no marker, a state ends the session', async t => {
  const f = await fixture(t); const old = await f.host(releasedHost);
  const inventory = await f.call('/desktops', { token: f.alice });
  assert.equal(inventory.body.desktops[0].host.capabilities.gamepadProtocol, undefined);
  const { response, controller, start } = await f.stream(old, asked);
  assert.equal(response.body.gamepadProtocol, undefined, 'not refused, only ungranted');
  assert.equal('gamepadProtocol' in start, false);
  controller.json(pressed);
  assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_input');
  await closed(controller);

  const g = await fixture(t); const capable = await g.host(padHost);
  const plain = await g.stream(capable, {});
  assert.equal(plain.response.body.gamepadProtocol, undefined, 'never granted unrequested');
  assert.equal('gamepadProtocol' in plain.start, false);
  plain.controller.json({ type: 'gamepad.remove', pad: 0 });
  assert.equal((await plain.controller.take(value => value.type === 'session.ended')).reason, 'invalid_input');
  await closed(plain.controller);
});

test('A malformed connect field is refused before any session', async t => {
  const f = await fixture(t); await f.host(padHost);
  for (const gamepadProtocol of ['gamepad-v2', 1, null, '']) {
    const response = await f.connect({ gamepadProtocol });
    assert.equal(response.status, 400, String(gamepadProtocol)); assert.equal(response.body.error.code, 'invalid_gamepad_protocol');
  }
  const plain = await f.connect({});
  assert.equal(plain.status, 200, 'a released client request is unchanged'); assert.equal(plain.body.gamepadProtocol, undefined);
});