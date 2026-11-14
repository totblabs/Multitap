import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { backgroundVideoProtocol, backgroundVideoCapability, requestedBackgroundVideo, negotiateBackgroundVideo, validBackgroundVideo } from '../src/background-video.mjs';

// Background video (background-video-v1): while the client's view is hidden it
// tells the Host, which sends one picture a second. Exact negotiation (ordinary
// streams with a Host that advertises it), the relayed request, released peers
// unchanged. Real broker and WebSockets, fake Host/client.
const origin = 'https://background-video-fixture.example';
const password = 'Background-video-fixture-password-only!';
const machineUuid = '12345678-1234-1234-1234-123456789ac5';
const releasedHost = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true };
const backgroundHost = { ...releasedHost, backgroundVideoProtocol: 'background-video-v1' };
const streamInfo = { type: 'stream.info', codec: 'h264', width: 1920, height: 1080, sourceWidth: 1920, sourceHeight: 1080, fpsTarget: 60, audio: { sampleRate: 48000, channels: 2, format: 's16le', available: true } };
const asked = { backgroundVideoProtocol: 'background-video-v1' };

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
  assert.equal(backgroundVideoProtocol, 'background-video-v1');
  assert.equal(backgroundVideoCapability(backgroundHost), true);
  for (const value of [releasedHost, { ...releasedHost, backgroundVideoProtocol: 'background-video-v2' }, { ...releasedHost, backgroundVideoProtocol: true }, undefined, null])
    assert.equal(backgroundVideoCapability(value), false, JSON.stringify(value));
  assert.equal(requestedBackgroundVideo('stream', undefined), false);
  assert.equal(requestedBackgroundVideo('stream', 'background-video-v1'), true);
  for (const [mode, value] of [['rdp', 'background-video-v1'], ['stream', 'background-video-v2'], ['stream', ''], ['stream', null], ['stream', true], ['stream', 1]])
    assert.equal(code(() => requestedBackgroundVideo(mode, value)), 'invalid_background_video', `${mode} ${value}`);
  assert.equal(negotiateBackgroundVideo(true, { capabilities: backgroundHost }, null), true);
  assert.equal(negotiateBackgroundVideo(false, { capabilities: backgroundHost }, null), false, 'never granted unrequested');
  assert.equal(negotiateBackgroundVideo(true, { capabilities: releasedHost }, null), false, 'a released Host: not granted, not refused');
  assert.equal(negotiateBackgroundVideo(true, { capabilities: backgroundHost }, [{ id: '0' }]), false, 'independent outputs keep every frame');
  assert.equal(validBackgroundVideo({ type: 'stream.background', on: true }), true);
  assert.equal(validBackgroundVideo({ type: 'stream.background', on: false }), true);
  for (const value of [{ type: 'stream.background' }, { type: 'stream.background', on: 1 }, { type: 'stream.background', on: 'true' }, { type: 'stream.background', on: true, x: 2 }, { type: 'cursor.compose', on: true }, null, [], 'x'])
    assert.equal(validBackgroundVideo(value), false, JSON.stringify(value));
});

test('Granted: the inventory, start and response carry it, on and off reach the Host, a malformed request ends the session', async t => {
  const f = await fixture(t); const host = await f.host(backgroundHost);
  const inventory = await f.call('/desktops', { token: f.alice });
  assert.equal(inventory.body.desktops[0].host.capabilities.backgroundVideoProtocol, 'background-video-v1');
  const { response, controller, start, sessionId } = await f.stream(host, asked);
  assert.equal(response.body.backgroundVideoProtocol, 'background-video-v1');
  assert.equal(start.backgroundVideoProtocol, 'background-video-v1');
  controller.json({ type: 'stream.background', on: true });
  assert.deepEqual(await host.take(value => value.type === 'stream.background'), { type: 'stream.background', sessionId, on: true });
  controller.json({ type: 'stream.background', on: false });
  assert.deepEqual(await host.take(value => value.type === 'stream.background'), { type: 'stream.background', sessionId, on: false });
  controller.json({ type: 'stream.background', on: 'yes' });
  assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_input');
  await closed(controller);
});

test('A released Host, or a client that did not ask: no grant, no marker, the request ends the session', async t => {
  const f = await fixture(t); const old = await f.host(releasedHost);
  const inventory = await f.call('/desktops', { token: f.alice });
  assert.equal(inventory.body.desktops[0].host.capabilities.backgroundVideoProtocol, undefined);
  const { response, controller, start } = await f.stream(old, asked);
  assert.equal(response.body.backgroundVideoProtocol, undefined, 'not refused, only ungranted');
  assert.equal('backgroundVideoProtocol' in start, false);
  controller.json({ type: 'stream.background', on: true });
  assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_input');
  await closed(controller);

  const g = await fixture(t); const capable = await g.host(backgroundHost);
  const plain = await g.stream(capable, {});
  assert.equal(plain.response.body.backgroundVideoProtocol, undefined, 'never granted unrequested');
  assert.equal('backgroundVideoProtocol' in plain.start, false);
  plain.controller.json({ type: 'stream.background', on: true });
  assert.equal((await plain.controller.take(value => value.type === 'session.ended')).reason, 'invalid_input');
  await closed(plain.controller);
});

test('A malformed connect field is refused before any session', async t => {
  const f = await fixture(t); await f.host(backgroundHost);
  for (const backgroundVideoProtocol of ['background-video-v2', 1, null, '']) {
    const response = await f.connect({ backgroundVideoProtocol });
    assert.equal(response.status, 400, String(backgroundVideoProtocol)); assert.equal(response.body.error.code, 'invalid_background_video');
  }
  const plain = await f.connect({});
  assert.equal(plain.status, 200, 'a released client request is unchanged'); assert.equal(plain.body.backgroundVideoProtocol, undefined);
});
