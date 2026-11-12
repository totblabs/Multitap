import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { relativeMouseCapability, requestedRelativeMouse, negotiateRelativeMouse, relativeMouseInput, validRelativeMouse, relativeMouseMaxDelta } from '../src/relative-mouse.mjs';

// Negotiated relative mouse input (relative-mouse-v1): exact negotiation, strict
// relay validation and bounds, and released peers that keep exactly today's
// behaviour. Real broker and WebSockets, fake Host/client.
const origin = 'https://relative-fixture.example';
const password = 'Relative-fixture-password-only!';
const machineUuid = '12345678-1234-1234-1234-123456789ac2';
const releasedHost = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true };
const relativeHost = { ...releasedHost, relativeMouseProtocol: 'relative-mouse-v1' };
const twoMonitors = [{ id: '0', width: 1920, height: 1080, primary: true }, { id: '1', width: 1920, height: 1080, primary: false }];
const streamInfo = { type: 'stream.info', codec: 'h264', width: 1920, height: 1080, sourceWidth: 1920, sourceHeight: 1080, fpsTarget: 60, audio: { sampleRate: 48000, channels: 2, format: 's16le', available: false } };
const move = (dx, dy) => ({ type: 'input', event: 'moveRelative', dx, dy });
const button = down => ({ type: 'input', event: 'button', button: 'left', down });
const wheel = { type: 'input', event: 'wheel', delta: -120 };

async function fixture(t) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const hash = await hashPassword(password);
  store.addUser({ name: 'admin', passwordHash: hash, role: 'admin' });
  store.addUser({ name: 'alice', passwordHash: hash, role: 'user' });
  store.syncProxmox([{ vmid: 173, node: 'pve', name: 'VM 173', status: 'running' }], 'fixture');
  store.assign('alice', 'pve-173');
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
  const code = (await call('/admin/desktops/pve-173/enrollment', { ...admin, method: 'POST', data: {} })).body.code;
  const wsUrl = path => base.replace('http:', 'ws:') + path;
  async function host(capabilities) {
    const enrolled = await call('/hosts/enroll', { method: 'POST', data: { code, hostname: 'Fixture VM', machineUuid, version: '0.2.70' } });
    assert.equal(enrolled.status, 201);
    const ws = await socket(wsUrl('/api/v1/hosts/channel'), 'Host ' + enrolled.body.hostToken);
    ws.json({ type: 'host.capabilities', capabilities });
    assert.equal((await ws.take(value => value.type === 'host.capabilities.ack')).accepted, true);
    return ws;
  }
  const connect = extra => call('/desktops/pve-173/connect', { method: 'POST', token: alice, data: { mode: 'stream', ...extra } });
  const client = sessionId => socket(wsUrl('/api/v1/sessions/' + sessionId), 'Bearer ' + alice);
  async function stream(hostSocket, extra = {}) {
    const response = await connect(extra);
    assert.equal(response.status, 200, JSON.stringify(response.body));
    const controller = await client(response.body.sessionId);
    const start = await hostSocket.take(value => value.type === 'session.start');
    hostSocket.json({ ...streamInfo, ...(start.resizeProtocol ? { epoch: 0 } : {}) }); await controller.take(value => value.type === 'stream.info');
    return { response, controller, start, sessionId: response.body.sessionId };
  }
  return { call, alice, host, connect, client, stream };
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
const ended = async controller => (await controller.take(value => value.type === 'session.ended')).reason;

test('Capability, request, negotiation and message rules are exact', () => {
  assert.equal(relativeMouseCapability(relativeHost), true);
  for (const value of [releasedHost, { ...releasedHost, relativeMouseProtocol: 'relative-mouse-v2' }, { ...releasedHost, relativeMouseProtocol: true }, undefined, null])
    assert.equal(relativeMouseCapability(value), false);
  assert.equal(requestedRelativeMouse('stream', undefined), false);
  assert.equal(requestedRelativeMouse('stream', 'relative-mouse-v1'), true);
  for (const [mode, protocol] of [['rdp', 'relative-mouse-v1'], ['stream', 'relative-mouse-v2'], ['stream', ''], ['stream', 1], ['stream', null], ['stream', true]])
    assert.equal(code(() => requestedRelativeMouse(mode, protocol)), 'invalid_relative_mouse_protocol', `${mode} ${protocol}`);
  assert.equal(negotiateRelativeMouse(true, { capabilities: relativeHost }, null), true);
  assert.equal(negotiateRelativeMouse(true, { capabilities: { ...relativeHost, monitors: [twoMonitors[0]] } }, null), true);
  assert.equal(negotiateRelativeMouse(false, { capabilities: relativeHost }, null), false, 'never granted unrequested');
  assert.equal(negotiateRelativeMouse(true, { capabilities: releasedHost }, null), false, 'released Host: positions, not refused');
  assert.equal(negotiateRelativeMouse(true, { capabilities: relativeHost }, [{ id: '0' }]), false, 'independent outputs keep positions');
  assert.equal(negotiateRelativeMouse(true, { capabilities: { ...relativeHost, monitors: twoMonitors } }, null), false, 'two monitors keep positions');

  for (const value of [move(12, -3), move(1, 0), move(0, -1), move(relativeMouseMaxDelta, -relativeMouseMaxDelta), { ...move(1, 1), sample: 7 }, button(true), button(false), { ...button(true), sample: 7 }, { ...button(true), button: 'right' }, { ...button(false), button: 'middle' }, { ...button(true), button: 'x1' }, { ...button(false), button: 'x2' }, wheel, { ...wheel, delta: 12000 }, { ...wheel, sample: 7 }])
    assert.equal(validRelativeMouse(value), true, JSON.stringify(value));
  for (const value of [move(0, 0), move(relativeMouseMaxDelta + 1, 0), move(0, -relativeMouseMaxDelta - 1), move(1.5, 0), move(0, 0.5), move('1', 1), move(1, null), move(NaN, 1), move(Infinity, 1), { type: 'input', event: 'moveRelative', dx: 1 }, { type: 'input', event: 'moveRelative', dy: 1 },
    { ...move(1, 1), x: 0.5 }, { ...move(1, 1), y: 0.5 }, { ...move(1, 1), extra: 1 }, { ...move(1, 1), type: 'input.reset' }, { ...button(true), button: 'x3' }, { ...button(true), down: 'yes' }, { ...button(true), extra: 1 }, { ...wheel, delta: 12001 }, { ...wheel, delta: 0.5 }, { ...wheel, extra: 1 }, { type: 'input', event: 'text', text: 'a' }, null, [], 'a'])
    assert.equal(validRelativeMouse(value), false, JSON.stringify(value));
  assert.equal(relativeMouseInput(move(1, 1)), true);
  assert.equal(relativeMouseInput(button(true)), true);
  assert.equal(relativeMouseInput(wheel), true);
  for (const value of [{ ...button(true), x: 0.5, y: 0.5 }, { ...wheel, x: 0.5, y: 0.5 }, { ...button(true), x: 0.5 }, { type: 'input', event: 'move', x: 0.5, y: 0.5 }, { type: 'input', event: 'key' }, { type: 'ping', id: 'a' }])
    assert.equal(relativeMouseInput(value), false, JSON.stringify(value));
});

test('A capable Host advertises it; the grant reaches the client and session.start; input is forwarded exactly', async t => {
  const f = await fixture(t); const host = await f.host(relativeHost);
  const inventory = await f.call('/desktops', { token: f.alice });
  assert.equal(inventory.body.desktops[0].host.capabilities.relativeMouseProtocol, 'relative-mouse-v1');
  const { response, controller, start } = await f.stream(host, { relativeMouseProtocol: 'relative-mouse-v1' });
  assert.equal(response.body.relativeMouseProtocol, 'relative-mouse-v1');
  assert.equal(start.relativeMouseProtocol, 'relative-mouse-v1');
  for (const value of [move(12, -3), move(-16384, 16384), button(true), button(false), { ...button(true), button: 'x1' }, { ...button(false), button: 'x1' }, { ...button(true), button: 'x2' }, { ...button(false), button: 'x2' }, wheel]) {
    controller.json(value);
    assert.deepEqual(await host.take(item => item.type === 'input'), value);
  }
  // Positioned input keeps working in the same session, in order.
  const position = { type: 'input', event: 'move', x: 0.5, y: 0.5 };
  controller.json(position); assert.deepEqual(await host.take(item => item.type === 'input'), position);
  // Both zero ends the session.
  controller.json(move(0, 0));
  assert.equal(await ended(controller), 'invalid_input');
  await closed(controller);
  assert.equal(host.queue.some(item => item.type === 'input' && item.event === 'moveRelative'), false);
});

test('Every malformed message ends a negotiated session before the Host', async t => {
  const f = await fixture(t); const host = await f.host(relativeHost);
  for (const bad of [move(16385, 0), { ...move(1, 1), x: 0.5 }, { ...button(true), button: 'x3' }, { ...wheel, delta: 12001 }, { ...move(1, 1), sample: 0 }]) {
    const { controller } = await f.stream(host, { relativeMouseProtocol: 'relative-mouse-v1' });
    controller.json(bad);
    assert.equal(await ended(controller), 'invalid_input', JSON.stringify(bad));
    await host.take(value => value.type === 'session.stop'); await closed(controller);
  }
  assert.equal(host.queue.some(item => item.type === 'input'), false);
});

test('Ungranted sessions never carry it: released Host, unrequested, RDP, two monitors', async t => {
  const f = await fixture(t); const host = await f.host(releasedHost);
  const inventory = await f.call('/desktops', { token: f.alice });
  assert.equal(inventory.body.desktops[0].host.capabilities.relativeMouseProtocol, undefined);
  for (const value of [move(1, 1), button(true), wheel]) {
    const released = await f.stream(host, { relativeMouseProtocol: 'relative-mouse-v1' });
    assert.equal(released.response.body.relativeMouseProtocol, undefined, 'requested from a released Host: not granted, not refused');
    assert.equal(released.start.relativeMouseProtocol, undefined);
    released.controller.json(value);
    assert.equal(await ended(released.controller), 'invalid_input');
    await host.take(item => item.type === 'session.stop'); await closed(released.controller);
  }
  assert.equal(host.queue.some(item => item.type === 'input'), false, 'ungranted input never reaches the Host');
  assert.equal((await f.connect({ relativeMouseProtocol: 'relative-mouse-v2' })).body.error.code, 'invalid_relative_mouse_protocol');
  // RDP is not offered: refused as such, so it never carries relative input.
  const rdp = await f.call('/desktops/pve-173/connect', { method: 'POST', token: f.alice, data: { mode: 'rdp', relativeMouseProtocol: 'relative-mouse-v1' } });
  assert.equal(rdp.status, 409); assert.equal(rdp.body.error.code, 'rdp_not_supported'); assert.equal(rdp.body.relativeMouseProtocol, undefined);
});

test('A capable Host without the client request keeps positions', async t => {
  const f = await fixture(t); const host = await f.host(relativeHost);
  const { response, controller, start } = await f.stream(host, {});
  assert.equal(response.body.relativeMouseProtocol, undefined);
  assert.equal(start.relativeMouseProtocol, undefined);
  controller.json(move(1, 1));
  assert.equal(await ended(controller), 'invalid_input');
});

test('Two monitors are not negotiated', async t => {
  const f = await fixture(t); const host = await f.host({ ...relativeHost, monitors: twoMonitors });
  const { response, start } = await f.stream(host, { relativeMouseProtocol: 'relative-mouse-v1' });
  assert.equal(response.body.relativeMouseProtocol, undefined);
  assert.equal(start.relativeMouseProtocol, undefined);
});

test('Sampled like move, and dropped while the output changes', async t => {
  const f = await fixture(t);
  const host = await f.host({ ...relativeHost, inputLatencyProtocol: 'input-latency-v1', dynamicResolution: true, resizeProtocol: 'epoch-v1', monitors: [
    { id: '0', width: 1920, height: 1080, primary: true, modes: [{ width: 1920, height: 1080, refreshRate: 60 }, { width: 1280, height: 720, refreshRate: 60 }] }] });
  const { controller } = await f.stream(host, { relativeMouseProtocol: 'relative-mouse-v1', inputLatencyProtocol: 'input-latency-v1', resizeProtocol: 'epoch-v1' });
  controller.json({ ...move(3, 4), sample: 1 });
  assert.deepEqual(await host.take(item => item.type === 'input'), { ...move(3, 4), sample: 1 });
  controller.json({ type: 'stream.resize', width: 1280, height: 720, fps: 60, monitor: '0', requestId: 'relative-resize' });
  await controller.take(value => value.type === 'stream.resizing');
  await host.take(value => value.type === 'input.reset');
  const request = await host.take(value => value.type === 'stream.resize');
  controller.json(move(5, 6)); controller.json(button(true));
  controller.json({ type: 'ping', id: 'relative-barrier' }); await host.take(value => value.type === 'ping');
  assert.equal(host.queue.some(item => item.type === 'input'), false, 'dropped during the change, the session stays');
  host.json({ ...streamInfo, width: 1280, height: 720, sourceWidth: 1280, sourceHeight: 720, sessionId: request.sessionId, requestId: request.requestId, resizeId: request.resizeId, epoch: request.epoch });
  await controller.take(value => value.type === 'stream.info');
  controller.json(move(7, 8));
  assert.deepEqual(await host.take(item => item.type === 'input'), move(7, 8));
});
