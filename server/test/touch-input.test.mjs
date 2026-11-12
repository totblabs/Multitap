import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { touchInputCapability, requestedTouchInput, negotiateTouchInput, validTouchInput, validTouchContact, touchInputMaxContacts } from '../src/touch-input.mjs';
import { safeClientDiagnosticReport } from '../src/client-diagnostics.mjs';

// Negotiated touch contacts (touch-input-v1): exact negotiation, strict relay
// validation and bounds, no injection while the output changes, and released
// peers that keep exactly today's behaviour. Real broker and WebSockets, fake
// Host/client.
const origin = 'https://touch-fixture.example';
const password = 'Touch-fixture-password-only!';
const machineUuid = '12345678-1234-1234-1234-123456789ad1';
const releasedHost = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true };
const touchHost = { ...releasedHost, touchInputProtocol: 'touch-input-v1' };
const resizingHost = { ...touchHost, dynamicResolution: true, resizeProtocol: 'epoch-v1', monitors: [
  { id: '0', width: 1920, height: 1080, primary: true, modes: [{ width: 1920, height: 1080, refreshRate: 60 }, { width: 1280, height: 720, refreshRate: 60 }] }] };
const streamInfo = { type: 'stream.info', codec: 'h264', width: 1920, height: 1080, sourceWidth: 1920, sourceHeight: 1080, fpsTarget: 60, audio: { sampleRate: 48000, channels: 2, format: 's16le', available: false } };
const contact = (id, phase, x = 0.5, y = 0.5, extra = {}) => ({ id, phase, x, y, ...extra });
const touch = (...contacts) => ({ type: 'input', event: 'touch', contacts });

async function fixture(t) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const hash = await hashPassword(password);
  store.addUser({ name: 'admin', passwordHash: hash, role: 'admin' });
  store.addUser({ name: 'alice', passwordHash: hash, role: 'user' });
  store.syncProxmox([{ vmid: 181, node: 'pve', name: 'VM 181', status: 'running' }], 'fixture');
  store.assign('alice', 'pve-181');
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
  const code = (await call('/admin/desktops/pve-181/enrollment', { ...admin, method: 'POST', data: {} })).body.code;
  const wsUrl = path => base.replace('http:', 'ws:') + path;
  async function host(capabilities) {
    const enrolled = await call('/hosts/enroll', { method: 'POST', data: { code, hostname: 'Fixture VM', machineUuid, version: '0.2.70' } });
    assert.equal(enrolled.status, 201);
    const ws = await socket(wsUrl('/api/v1/hosts/channel'), 'Host ' + enrolled.body.hostToken);
    ws.json({ type: 'host.capabilities', capabilities });
    assert.equal((await ws.take(value => value.type === 'host.capabilities.ack')).accepted, true);
    return ws;
  }
  const connect = extra => call('/desktops/pve-181/connect', { method: 'POST', token: alice, data: { mode: 'stream', ...extra } });
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
const isTouch = item => item.type === 'input' && item.event === 'touch';

test('Capability, request, negotiation and frame rules are exact', () => {
  assert.equal(touchInputCapability(touchHost), true);
  for (const value of [releasedHost, { ...releasedHost, touchInputProtocol: 'touch-input-v2' }, { ...releasedHost, touchInputProtocol: true }, undefined, null])
    assert.equal(touchInputCapability(value), false);
  assert.equal(requestedTouchInput('stream', undefined), false);
  assert.equal(requestedTouchInput('stream', 'touch-input-v1'), true);
  for (const [mode, protocol] of [['rdp', 'touch-input-v1'], ['stream', 'touch-input-v2'], ['stream', ''], ['stream', 1], ['stream', null], ['stream', true]])
    assert.equal(code(() => requestedTouchInput(mode, protocol)), 'invalid_touch_input_protocol', `${mode} ${protocol}`);
  assert.equal(negotiateTouchInput(true, { capabilities: touchHost }, null), true);
  assert.equal(negotiateTouchInput(false, { capabilities: touchHost }, null), false, 'never granted unrequested');
  assert.equal(negotiateTouchInput(true, { capabilities: releasedHost }, null), false, 'released Host: pointer only, not refused');
  assert.equal(negotiateTouchInput(true, { capabilities: touchHost }, [{ id: '0' }]), false, 'independent outputs keep pointer input');

  // Valid frames: every phase, the edges of the coordinate space, optional
  // pressure and size, ten contacts, large client ids.
  const ten = Array.from({ length: touchInputMaxContacts }, (_, n) => contact(n, 'move', n / 10, 1 - n / 10));
  for (const value of [touch(contact(0, 'down')), touch(contact(1, 'move', 0, 0)), touch(contact(2, 'up', 1, 1)), touch(contact(3, 'cancel')),
    touch(contact(4, 'down', 0.25, 0.75, { pressure: 0 }), contact(5, 'move', 0.5, 0.5, { pressure: 1, w: 0.01, h: 0.02 })),
    touch(...ten), touch(contact(0x7fffffff, 'down', 0.1, 0.2, { w: 1, h: 1 }))])
    assert.equal(validTouchInput(value), true, JSON.stringify(value));
  // Invalid frames: bounds, duplicates, extra or missing fields, sampling.
  for (const value of [touch(), touch(...ten, contact(10, 'move')), touch(contact(1, 'down'), contact(1, 'move')),
    touch(contact(-1, 'down')), touch(contact(0x80000000, 'down')), touch(contact(1.5, 'down')), touch(contact('1', 'down')),
    touch(contact(1, 'press')), touch(contact(1, 'DOWN')), touch(contact(1, 'down', -0.01, 0.5)), touch(contact(1, 'down', 0.5, 1.01)),
    touch(contact(1, 'down', Infinity, 0.5)), touch(contact(1, 'down', '0.5', 0.5)), touch(contact(1, 'down', 0.5, 0.5, { pressure: 1.5 })),
    touch(contact(1, 'down', 0.5, 0.5, { pressure: -0.1 })), touch(contact(1, 'down', 0.5, 0.5, { w: 0.1 })), touch(contact(1, 'down', 0.5, 0.5, { h: 0.1 })),
    touch(contact(1, 'down', 0.5, 0.5, { w: 0, h: 0.1 })), touch(contact(1, 'down', 0.5, 0.5, { w: 0.1, h: 1.5 })), touch(contact(1, 'down', 0.5, 0.5, { orientation: 90 })),
    touch({ id: 1, phase: 'down', x: 0.5 }), touch(null), touch([1, 'down', 0.5, 0.5]),
    { ...touch(contact(1, 'down')), sample: 1 }, { ...touch(contact(1, 'down')), sessionId: 'x' }, { type: 'input', event: 'touch', contacts: {} },
    { type: 'input', event: 'touch' }, { type: 'input', event: 'move', contacts: [contact(1, 'down')] }, { type: 'touch', event: 'touch', contacts: [contact(1, 'down')] }, null, [], 'touch'])
    assert.equal(validTouchInput(value), false, JSON.stringify(value));
  assert.equal(validTouchContact(contact(7, 'up', 0.3, 0.4, { pressure: 0.5, w: 0.05, h: 0.05 })), true);
  // The largest valid frame stays far below the relay's 8 KiB control limit.
  const largest = touch(...Array.from({ length: touchInputMaxContacts }, (_, n) => contact(0x7fffffff - n, 'move', 0.123456789012345, 0.987654321098765, { pressure: 0.123456789012345, w: 0.123456789012345, h: 0.123456789012345 })));
  assert.equal(validTouchInput(largest), true);
  assert.ok(Buffer.byteLength(JSON.stringify(largest)) < 2048);
});

test('A capable Host advertises it; the grant reaches the client and session.start; frames are forwarded exactly', async t => {
  const f = await fixture(t); const host = await f.host(touchHost);
  const inventory = await f.call('/desktops', { token: f.alice });
  assert.equal(inventory.body.desktops[0].host.capabilities.touchInputProtocol, 'touch-input-v1');
  const { response, controller, start } = await f.stream(host, { touchInputProtocol: 'touch-input-v1' });
  assert.equal(response.body.touchInputProtocol, 'touch-input-v1');
  assert.equal(start.touchInputProtocol, 'touch-input-v1');
  const frames = [touch(contact(3, 'down', 0.2, 0.3, { pressure: 0.6, w: 0.01, h: 0.012 })),
    touch(contact(3, 'move', 0.25, 0.35), contact(9, 'down', 0.7, 0.7)),
    touch(contact(3, 'up', 0.25, 0.35), contact(9, 'cancel', 0.7, 0.7))];
  for (const frame of frames) {
    controller.json(frame);
    assert.deepEqual(await host.take(isTouch), frame, 'forwarded unchanged, without a sample or sessionId');
  }
  // Mouse and keys keep working in the same session, in order.
  const move = { type: 'input', event: 'move', x: 0.5, y: 0.5 };
  controller.json(move); assert.deepEqual(await host.take(item => item.type === 'input'), move);
  // A malformed frame ends the session and reaches nothing.
  controller.json(touch(contact(1, 'down', 2, 0.5)));
  assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_input');
  await host.take(value => value.type === 'input.reset');
  await closed(controller);
  assert.equal(host.queue.some(item => isTouch(item) && item.contacts[0].x === 2), false);
});

test('Touch frames are not injected while the output changes; the change starts with input.reset', async t => {
  const f = await fixture(t); const host = await f.host(resizingHost);
  const { controller, start } = await f.stream(host, { touchInputProtocol: 'touch-input-v1', resizeProtocol: 'epoch-v1' });
  assert.equal(start.touchInputProtocol, 'touch-input-v1');
  controller.json(touch(contact(0, 'down')));
  assert.equal((await host.take(isTouch)).contacts[0].phase, 'down');
  controller.json({ type: 'stream.resize', width: 1280, height: 720, fps: 60, monitor: '0', requestId: 'touch-resize' });
  await controller.take(value => value.type === 'stream.resizing');
  await host.take(value => value.type === 'input.reset');
  const request = await host.take(value => value.type === 'stream.resize');
  controller.json(touch(contact(0, 'move', 0.6, 0.6)));
  controller.json({ type: 'ping', id: 'touch-barrier' }); await host.take(value => value.type === 'ping');
  assert.equal(host.queue.some(isTouch), false, 'dropped during the change, the session stays');
  host.json({ ...streamInfo, width: 1280, height: 720, sourceWidth: 1280, sourceHeight: 720, sessionId: request.sessionId, requestId: request.requestId, resizeId: request.resizeId, epoch: request.epoch });
  await controller.take(value => value.type === 'stream.info');
  controller.json(touch(contact(1, 'down', 0.4, 0.4)));
  assert.deepEqual(await host.take(isTouch), touch(contact(1, 'down', 0.4, 0.4)));
});

test('Ungranted sessions never carry touch: released Host, unrequested, malformed, RDP', async t => {
  const f = await fixture(t); const host = await f.host(releasedHost);
  const inventory = await f.call('/desktops', { token: f.alice });
  assert.equal(inventory.body.desktops[0].host.capabilities.touchInputProtocol, undefined);
  const released = await f.stream(host, { touchInputProtocol: 'touch-input-v1' });
  assert.equal(released.response.body.touchInputProtocol, undefined, 'requested from a released Host: not granted, not refused');
  assert.equal(released.start.touchInputProtocol, undefined);
  released.controller.json(touch(contact(0, 'down')));
  assert.equal((await released.controller.take(value => value.type === 'session.ended')).reason, 'invalid_input');
  await host.take(value => value.type === 'session.stop'); await closed(released.controller);
  assert.equal(host.queue.some(isTouch), false, 'ungranted touch never reaches the Host');
  assert.equal((await f.connect({ touchInputProtocol: 'touch-input-v2' })).body.error.code, 'invalid_touch_input_protocol');
  // RDP is not offered: refused as such, so it never carries touch.
  const rdp = await f.call('/desktops/pve-181/connect', { method: 'POST', token: f.alice, data: { mode: 'rdp', touchInputProtocol: 'touch-input-v1' } });
  assert.equal(rdp.status, 409); assert.equal(rdp.body.error.code, 'rdp_not_supported'); assert.equal(rdp.body.touchInputProtocol, undefined);
});

test('A capable Host without the client request keeps today\'s session.start', async t => {
  const f = await fixture(t); const host = await f.host(touchHost);
  const { response, controller, start } = await f.stream(host, {});
  assert.equal(response.body.touchInputProtocol, undefined);
  assert.equal(start.touchInputProtocol, undefined);
  controller.json(touch(contact(0, 'down')));
  assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_input');
  await closed(controller);
  assert.equal(host.queue.some(isTouch), false);
});

test('Touch diagnostics pass the client diagnostics projection; positions never do', () => {
  const report = safeClientDiagnosticReport({ protocol: 'client-diagnostics-v1', runId: 'c'.repeat(32), sequence: 1, version: null,
    events: [{ event: 'streamStats', time: '2026-10-26T12:00:00.000Z', values: { samples: 10, touchMode: 0, touchInput: true, touchFrames: 240, touchContacts: 2, touchX: 0.5, futureField: 7 } }] });
  assert.deepEqual(report.events[0].values, { samples: 10, touchMode: 0, touchInput: true, touchFrames: 240, touchContacts: 2 });
});
