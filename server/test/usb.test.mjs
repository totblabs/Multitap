import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { usbCapability, requestedUsb, negotiateUsb, usbLimits, parseUsbDescriptors, usbDevicePolicy, usbStatuses } from '../src/usb.mjs';
import { descriptors, vectorFile, vectors } from './usb-descriptors.mjs';
import { safeClientDiagnosticReport } from '../src/client-diagnostics.mjs';
import { requests as trackRequests, options as trackOptions } from './track-fixture.mjs';

// Streaming USB redirection (usb-v1): exact negotiation on the per-assignment
// `usbDevices` permission, the descriptor policy (shared vectors with the Host
// and client), the relayed URB framing, the window, class permissions,
// revocation that ends USB but not the stream, and released peers that keep
// exactly today's behaviour. Real broker and WebSockets, fake Host/client.
const origin = 'https://usb-fixture.example';
const password = 'Usb-fixture-password-only!';
const machineUuid = '12345678-1234-1234-1234-123456789ad3';
const releasedHost = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true };
const usbHost = { ...releasedHost, usbProtocol: 'usb-v1' };
const streamInfo = { type: 'stream.info', codec: 'h264', width: 1920, height: 1080, sourceWidth: 1920, sourceHeight: 1080, fpsTarget: 60, audio: { sampleRate: 48000, channels: 2, format: 's16le', available: false } };
const deny = { drives: false, printers: false, usbDevices: false, smartCards: false, serialPorts: false };

async function fixture(t, grant = { usbDevices: true }) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const hash = await hashPassword(password);
  store.addUser({ name: 'admin', passwordHash: hash, role: 'admin' });
  store.addUser({ name: 'alice', passwordHash: hash, role: 'user' });
  store.syncProxmox([{ vmid: 183, node: 'pve', name: 'VM 183', status: 'running' }], 'fixture');
  store.assign('alice', 'pve-183');
  const setPolicy = value => store.updateAssignmentPolicy('alice', 'pve-183', { devicePolicy: { ...deny, ...value } }, 'fixture');
  setPolicy(grant);
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
  const code = (await call('/admin/desktops/pve-183/enrollment', { ...admin, method: 'POST', data: {} })).body.code;
  const wsUrl = path => base.replace('http:', 'ws:') + path;
  let hostToken;
  async function host(capabilities) {
    const enrolled = await call('/hosts/enroll', { method: 'POST', data: { code, hostname: 'Fixture VM', machineUuid, version: '0.2.72' } });
    assert.equal(enrolled.status, 201);
    hostToken = enrolled.body.hostToken;
    const ws = await socket(wsUrl('/api/v1/hosts/channel'), 'Host ' + hostToken);
    ws.json({ type: 'host.capabilities', capabilities });
    assert.equal((await ws.take(value => value.type === 'host.capabilities.ack')).accepted, true);
    return ws;
  }
  const connect = extra => call('/desktops/pve-183/connect', { method: 'POST', token: alice, data: { mode: 'stream', ...extra } });
  async function stream(hostSocket, extra = {}) {
    const response = await connect(extra);
    assert.equal(response.status, 200, JSON.stringify(response.body));
    const controller = await socket(wsUrl('/api/v1/sessions/' + response.body.sessionId), 'Bearer ' + alice);
    const start = await hostSocket.take(value => value.type === 'session.start');
    hostSocket.json({ ...streamInfo }); await controller.take(value => value.type === 'stream.info');
    return { response, controller, start, sessionId: response.body.sessionId };
  }
  async function usbPair(sessionId) {
    const usbHostSocket = await socket(wsUrl(`/api/v1/hosts/sessions/${sessionId}/usb`), 'Host ' + hostToken);
    const usbClient = await socket(wsUrl(`/api/v1/sessions/${sessionId}/usb`), 'Bearer ' + alice);
    const ready = await usbClient.take(value => value.type === 'usb.ready');
    assert.deepEqual(await usbHostSocket.take(value => value.type === 'usb.ready'), ready);
    return { usbHost: usbHostSocket, usbClient, ready };
  }
  const refused = (path, authorization) => new Promise(resolve => {
    const ws = new WebSocket(wsUrl(path), { headers: { Authorization: authorization }, perMessageDeflate: false });
    ws.once('unexpected-response', (_, response) => { resolve(response.statusCode); ws.terminate(); });
    ws.once('open', () => { resolve('open'); ws.close(); });
    ws.once('error', () => {});
  });
  const client = sessionId => socket(wsUrl('/api/v1/sessions/' + sessionId), 'Bearer ' + alice);
  return { store, call, alice, admin, host, connect, client, stream, usbPair, refused, setPolicy, relay: () => server.relay, token: () => hostToken };
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
const isText = value => !Buffer.isBuffer(value);
const isFrame = value => Buffer.isBuffer(value);

// One 24-byte header (+ payload) as the wire carries it.
function frame({ type, device = 1, endpoint = 0x81, transfer = 2, seq = 1, length = 0, status = 0, setup = Buffer.alloc(8), payload = Buffer.alloc(0) }) {
  const header = Buffer.alloc(24);
  header[0] = type; header[1] = device; header[2] = endpoint; header[3] = transfer;
  header.writeUInt32LE(seq, 4); header.writeUInt32LE(length, 8); header.writeInt32LE(status, 12); Buffer.from(setup).copy(header, 16);
  return Buffer.concat([header, payload]);
}
const parse = data => ({ type: data[0], device: data[1], endpoint: data[2], transfer: data[3], seq: data.readUInt32LE(4), length: data.readUInt32LE(8), status: data.readInt32LE(12), payload: data.subarray(24) });
const stick = descriptors({ configs: [{ interfaces: [{ number: 0, class: 8, subclass: 6, protocol: 0x50, endpoints: [[0x81, 2, 512], [0x02, 2, 512]] }] }] });
const serial = descriptors({ deviceClass: 2, configs: [{ interfaces: [{ number: 0, class: 255, endpoints: [[0x81, 2, 64], [0x02, 2, 64]] }] }] });
const attach = (device, bytes, speed = 'high') => ({ type: 'usb.attach', device, descriptors: bytes.toString('base64'), speed });

test('Capability, request and negotiation rules are exact', () => {
  assert.equal(usbCapability(usbHost), true);
  for (const value of [releasedHost, { ...releasedHost, usbProtocol: 'usb-v2' }, { ...releasedHost, usbProtocol: true }, undefined, null]) assert.equal(usbCapability(value), false);
  assert.equal(requestedUsb('stream', undefined), false);
  assert.equal(requestedUsb('stream', 'usb-v1'), true);
  for (const [mode, protocol] of [['rdp', 'usb-v1'], ['stream', 'usb-v2'], ['stream', ''], ['stream', 1], ['stream', null], ['stream', true]])
    assert.equal(code(() => requestedUsb(mode, protocol)), 'invalid_usb_protocol', `${mode} ${protocol}`);
  const allowed = { ...deny, usbDevices: true };
  assert.equal(negotiateUsb(true, { capabilities: usbHost }, allowed), true);
  assert.equal(negotiateUsb(false, { capabilities: usbHost }, allowed), false, 'never granted unrequested');
  assert.equal(negotiateUsb(true, { capabilities: releasedHost }, allowed), false, 'released Host: not granted, not refused');
  assert.equal(negotiateUsb(true, { capabilities: usbHost }, deny), false, 'default denied');
  assert.equal(negotiateUsb(true, { capabilities: usbHost }, { ...deny, drives: true, printers: true, serialPorts: true }), false, 'only usbDevices grants');
});

test('The shared descriptor vectors are what the builder makes, and the policy decides each one as recorded', () => {
  const file = JSON.parse(readFileSync(new URL('./fixtures/usb-descriptor-vectors.json', import.meta.url), 'utf8'));
  assert.deepEqual(file, vectorFile(), 'regenerate fixtures/usb-descriptor-vectors.json from usb-descriptors.mjs');
  for (const [name, entry] of Object.entries(file)) {
    const decision = usbDevicePolicy(parseUsbDescriptors(new Uint8Array(Buffer.from(entry.descriptors, 'hex'))));
    assert.equal(decision.allowed, entry.expect.allowed, name);
    if (decision.allowed) { assert.deepEqual(decision.requires, entry.expect.requires, name); assert.deepEqual(decision.classes, entry.expect.classes, name); }
    else assert.equal(decision.reason, entry.expect.reason, name);
  }
  assert.ok(Object.keys(vectors).length >= 15);
});

test('Malformed descriptor sets are refused, never partly read', () => {
  const good = Buffer.from(stick);
  const bad = {
    'too short': good.subarray(0, 20),
    'wrong device length': Buffer.concat([Buffer.from([17]), good.subarray(1)]),
    'config count 0': (() => { const copy = Buffer.from(good); copy[17] = 0; return copy; })(),
    'config count 5': (() => { const copy = Buffer.from(good); copy[17] = 5; return copy; })(),
    'bad max packet': (() => { const copy = Buffer.from(good); copy[7] = 7; return copy; })(),
    'config total beyond data': (() => { const copy = Buffer.from(good); copy[20] = 255; return copy; })(),
    'trailing bytes': Buffer.concat([good, Buffer.from([0])]),
    'descriptor length 1': (() => { const copy = Buffer.from(good); copy[18 + 9] = 1; return copy; })(),
    'endpoint count mismatch': (() => { const copy = Buffer.from(good); copy[18 + 9 + 4] = 3; return copy; })(),
    'oversized': Buffer.alloc(usbLimits.descriptorBytes + 1),
  };
  for (const [name, bytes] of Object.entries(bad)) assert.equal(code(() => parseUsbDescriptors(new Uint8Array(bytes))), undefined, name);
  assert.doesNotThrow(() => parseUsbDescriptors(new Uint8Array(good)));
});

test('A granted session carries usb-v1 to the client and the Host', async t => {
  const f = await fixture(t); const host = await f.host(usbHost);
  const inventory = await f.call('/desktops', { token: f.alice });
  assert.equal(inventory.body.desktops[0].host.capabilities.usbProtocol, 'usb-v1');
  const { response, start, sessionId } = await f.stream(host, { usbProtocol: 'usb-v1' });
  assert.equal(response.body.usbProtocol, 'usb-v1');
  assert.equal(response.body.usbWebSocketUrl, `wss://usb-fixture.example/api/v1/sessions/${sessionId}/usb`);
  assert.equal(start.usbProtocol, 'usb-v1');
  assert.equal(start.usbUrl, `wss://usb-fixture.example/api/v1/hosts/sessions/${sessionId}/usb`);
  assert.deepEqual(start.usbGrant, { drives: false, printers: false, smartCards: false, serialPorts: false }, 'usbDevices alone grants no class');
});

test('Independent outputs (tracks-v1) carry usb-v1 to the Host as well, marked usb-tracks-v1 for the client', async t => {
  const trackUsbHost = { ...usbHost, videoTrackProtocol: 'tracks-v1', trackViewportProvisioning: true, monitors: trackRequests.map(item => ({ id: item.id, width: item.width, height: item.height, primary: item.id === '0' })) };
  const f = await fixture(t); const host = await f.host(trackUsbHost);
  const response = await f.connect({ options: trackOptions, mediaProtocol: 'tracks-v1', tracks: trackRequests, usbProtocol: 'usb-v1' });
  assert.equal(response.status, 200, JSON.stringify(response.body));
  const { sessionId } = response.body;
  assert.equal(response.body.transport, 'media-tracks-v1');
  assert.equal(response.body.usbProtocol, 'usb-v1');
  assert.equal(response.body.usbTracksProtocol, 'usb-tracks-v1');
  assert.equal(response.body.usbWebSocketUrl, `wss://usb-fixture.example/api/v1/sessions/${sessionId}/usb`);
  await f.client(sessionId);
  const start = await host.take(value => value.type === 'session.start');
  assert.equal(start.mediaProtocol, 'tracks-v1');
  assert.equal(start.usbProtocol, 'usb-v1');
  assert.equal(start.usbUrl, `wss://usb-fixture.example/api/v1/hosts/sessions/${sessionId}/usb`);
  assert.deepEqual(start.usbGrant, { drives: false, printers: false, smartCards: false, serialPorts: false });
  await f.usbPair(sessionId);
  // An ordinary session never carries the independent-outputs marker.
  const g = await fixture(t); const single = await g.host(usbHost);
  assert.equal((await g.stream(single, { usbProtocol: 'usb-v1' })).response.body.usbTracksProtocol, undefined);
});

test('Without the request, the permission or a capable Host nothing is granted and nothing is refused', async t => {
  for (const [grant, capabilities, extra] of [[{ usbDevices: true }, usbHost, {}], [{}, usbHost, { usbProtocol: 'usb-v1' }], [{ drives: true }, usbHost, { usbProtocol: 'usb-v1' }], [{ usbDevices: true }, releasedHost, { usbProtocol: 'usb-v1' }]]) {
    const f = await fixture(t, grant); const host = await f.host(capabilities);
    const { response, start } = await f.stream(host, extra);
    assert.equal(response.body.usbProtocol, undefined); assert.equal(response.body.usbWebSocketUrl, undefined);
    assert.equal(start.usbProtocol, undefined); assert.equal(start.usbUrl, undefined); assert.equal(start.usbGrant, undefined);
  }
  const f = await fixture(t);
  await f.host(usbHost);
  assert.equal((await f.connect({ usbProtocol: 'usb-v2' })).body.error.code, 'invalid_usb_protocol');
  // RDP is not offered: refused as such before its USB field is read.
  const rdp = await f.call('/desktops/pve-183/connect', { method: 'POST', token: f.alice, data: { mode: 'rdp', usbProtocol: 'usb-v1' } });
  assert.equal(rdp.status, 409); assert.equal(rdp.body.error.code, 'rdp_not_supported');
});

test('Only the session\'s own client and Host can open the channel, and only when granted', async t => {
  const f = await fixture(t); const host = await f.host(usbHost);
  const { sessionId } = await f.stream(host, { usbProtocol: 'usb-v1' });
  assert.equal(await f.refused(`/api/v1/sessions/${sessionId}/usb`, 'Bearer ' + 'a'.repeat(43)), 401);
  assert.equal(await f.refused(`/api/v1/hosts/sessions/${sessionId}/usb`, 'Host ' + 'a'.repeat(43)), 401);
  assert.equal(await f.refused(`/api/v1/sessions/${sessionId}/usb`, 'Host ' + f.token()), 401);
  const pair = await f.usbPair(sessionId);
  assert.equal(await f.refused(`/api/v1/sessions/${sessionId}/usb`, 'Bearer ' + f.alice), 409, 'one client socket');
  assert.deepEqual(pair.ready.limits, usbLimits);
  const ungranted = await fixture(t, {}); const ungrantedHost = await ungranted.host(usbHost);
  const second = await ungranted.stream(ungrantedHost, { usbProtocol: 'usb-v1' });
  assert.equal(await ungranted.refused(`/api/v1/sessions/${second.sessionId}/usb`, 'Bearer ' + ungranted.alice), 401);
});

test('A device attaches, requests cross with exact bounds, and returns release the window', async t => {
  const f = await fixture(t, { usbDevices: true, drives: true }); const host = await f.host(usbHost);
  const { sessionId } = await f.stream(host, { usbProtocol: 'usb-v1' });
  const { usbHost: h, usbClient: c } = await f.usbPair(sessionId);
  c.json(attach(1, stick));
  assert.equal((await h.take(isText)).type, 'usb.attach');
  h.json({ type: 'usb.status', state: 'ready' }); assert.deepEqual(await c.take(isText), { type: 'usb.status', state: 'ready' });
  h.json({ type: 'usb.attached', device: 1 }); assert.deepEqual(await c.take(isText), { type: 'usb.attached', device: 1 });
  // Bulk OUT with a payload, bulk IN, a control IN transfer.
  const out = Buffer.alloc(4096, 7);
  h.send(frame({ type: 1, endpoint: 0x02, seq: 10, length: out.length, payload: out }));
  const submitted = parse(await c.take(isFrame)); assert.equal(submitted.seq, 10); assert.equal(submitted.payload.length, 4096);
  c.send(frame({ type: 2, endpoint: 0x02, seq: 10, length: out.length }));
  assert.equal(parse(await h.take(isFrame)).length, 4096);
  h.send(frame({ type: 1, endpoint: 0x81, seq: 11, length: 512 }));
  await c.take(isFrame);
  const data = Buffer.alloc(100, 9);
  c.send(frame({ type: 2, endpoint: 0x81, seq: 11, length: data.length, payload: data }));
  const back = parse(await h.take(isFrame)); assert.equal(back.payload.length, 100); assert.equal(back.status, 0);
  const setup = Buffer.from([0x80, 6, 0, 1, 0, 0, 18, 0]);
  h.send(frame({ type: 1, endpoint: 0x80, transfer: 0, seq: 12, length: 18, setup }));
  assert.deepEqual(parse(await c.take(isFrame)).seq, 12);
  c.send(frame({ type: 2, endpoint: 0x80, transfer: 0, seq: 12, status: -32 }));
  assert.equal(parse(await h.take(isFrame)).status, -32, 'a stall is a normal return');
  // Unlink and its answer.
  h.send(frame({ type: 1, endpoint: 0x81, seq: 13, length: 64 })); await c.take(isFrame);
  h.send(frame({ type: 3, seq: 14, length: 13, endpoint: 0, transfer: 0 })); assert.equal(parse(await c.take(isFrame)).type, 3);
  c.send(frame({ type: 4, seq: 14, endpoint: 0, transfer: 0, status: -104 })); assert.equal(parse(await h.take(isFrame)).type, 4);
  c.send(frame({ type: 2, endpoint: 0x81, seq: 13, status: -104 })); assert.equal(parse(await h.take(isFrame)).status, -104);
  assert.equal(f.relay().sessions.get(sessionId).usbPair.flightBytes, 0);
  assert.equal(f.relay().sessions.get(sessionId).usbPair.flight.size, 0);
});

test('The exchange USBSTOR makes to start a bulk-only stick crosses the relay unchanged, including class requests, stalls, timeouts and a cancelled request', async t => {
  // What the VM's USBSTOR needs in order to start. None of its steps may be refused by the relay (that would
  // close the channel), and a request the client gave up on is released by the client's own answer.
  const f = await fixture(t, { usbDevices: true, drives: true }); const host = await f.host(usbHost);
  const { sessionId } = await f.stream(host, { usbProtocol: 'usb-v1' });
  const { usbHost: h, usbClient: c } = await f.usbPair(sessionId);
  c.json(attach(1, stick)); await h.take(isText);
  h.json({ type: 'usb.status', state: 'ready' }); await c.take(isText);
  h.json({ type: 'usb.attached', device: 1 }); await c.take(isText);
  let seq = 100;
  const control = async (bm, request, value, index, length, answer) => {
    const setup = Buffer.from([bm, request, value & 255, value >> 8, index & 255, index >> 8, length & 255, length >> 8]), id = ++seq, input = (bm & 0x80) !== 0;
    h.send(frame({ type: 1, endpoint: input ? 0x80 : 0, transfer: 0, seq: id, length, setup }));
    const raw = await c.take(isFrame), seen = parse(raw); assert.equal(seen.seq, id); assert.deepEqual([...raw.subarray(16, 24)], [...setup]);
    c.send(frame({ type: 2, endpoint: input ? 0x80 : 0, transfer: 0, seq: id, ...answer }));
    return parse(await h.take(isFrame));
  };
  assert.equal((await control(0x80, 6, 0x0303, 0x0409, 255, { length: 26, payload: Buffer.alloc(26, 3) })).payload.length, 26, 'the serial number string');
  assert.equal((await control(0x00, 9, 1, 0, 0, {})).status, 0, 'SET_CONFIGURATION');
  assert.equal((await control(0xa1, 0xfe, 0, 0, 1, { status: -32 })).status, -32, 'GET_MAX_LUN stalled');
  assert.equal((await control(0xa1, 0xfe, 0, 0, 1, { length: 1, payload: Buffer.from([0]) })).payload.length, 1, 'GET_MAX_LUN answered');
  assert.equal((await control(0x21, 0xff, 0, 0, 0, {})).status, 0, 'Bulk-Only Mass Storage Reset');
  assert.equal((await control(0x02, 1, 0, 0x81, 0, {})).status, 0, 'CLEAR_FEATURE(ENDPOINT_HALT)');
  assert.equal((await control(0xa1, 0xfe, 0, 0, 1, { status: -110 })).status, -110, 'a control request the client timed out');
  // CBW out (31), data in (36, short answer), CSW in (13).
  h.send(frame({ type: 1, endpoint: 0x02, seq: 200, length: 31, payload: Buffer.alloc(31, 1) })); assert.equal(parse(await c.take(isFrame)).payload.length, 31);
  c.send(frame({ type: 2, endpoint: 0x02, seq: 200, length: 31 })); assert.equal(parse(await h.take(isFrame)).length, 31);
  h.send(frame({ type: 1, endpoint: 0x81, seq: 201, length: 36 })); await c.take(isFrame);
  c.send(frame({ type: 2, endpoint: 0x81, seq: 201, length: 36, payload: Buffer.alloc(36, 2) })); assert.equal(parse(await h.take(isFrame)).payload.length, 36);
  h.send(frame({ type: 1, endpoint: 0x81, seq: 202, length: 13 })); await c.take(isFrame);
  c.send(frame({ type: 2, endpoint: 0x81, seq: 202, length: 13, payload: Buffer.alloc(13, 3) })); assert.equal(parse(await h.take(isFrame)).payload.length, 13);
  // A request the device never answers: the VM cancels it; the client confirms (-104) and answers the request itself (-104, no data)
  // although the device has not let go of it. The relay's window is free again and the channel stays.
  const stuck = Buffer.from([0xa1, 0xfe, 0, 0, 0, 0, 1, 0]);
  h.send(frame({ type: 1, endpoint: 0x80, transfer: 0, seq: 300, length: 1, setup: stuck })); await c.take(isFrame);
  h.send(frame({ type: 3, seq: 301, length: 300, endpoint: 0, transfer: 0 })); assert.equal(parse(await c.take(isFrame)).type, 3);
  c.send(frame({ type: 4, seq: 301, endpoint: 0, transfer: 0, status: -104 })); assert.equal(parse(await h.take(isFrame)).type, 4);
  c.send(frame({ type: 2, endpoint: 0x80, transfer: 0, seq: 300, status: -104 })); assert.equal(parse(await h.take(isFrame)).status, -104);
  const pair = f.relay().sessions.get(sessionId).usbPair;
  assert.equal(pair.flight.size, 0); assert.equal(pair.flightBytes, 0); assert.equal(pair.closed, false, 'the channel stayed open through the whole start');
});
const violations = {
  'client submits': (h, c) => c.send(frame({ type: 1, seq: 1, length: 0 })),
  'Host returns': (h, c) => h.send(frame({ type: 2, seq: 1 })),
  'short frame': (h, c) => h.send(Buffer.alloc(23)),
  'unknown type': (h, c) => h.send(frame({ type: 9, seq: 1 })),
  'iso transfer': (h, c) => h.send(frame({ type: 1, transfer: 1, seq: 1, length: 8 })),
  'bulk on endpoint 0': (h, c) => h.send(frame({ type: 1, endpoint: 0x80, transfer: 2, seq: 1, length: 8 })),
  'reserved endpoint bits': (h, c) => h.send(frame({ type: 1, endpoint: 0x41, seq: 1, length: 8 })),
  'control with a wrong length': (h, c) => h.send(frame({ type: 1, endpoint: 0x80, transfer: 0, seq: 1, length: 18, setup: Buffer.from([0x80, 6, 0, 1, 0, 0, 17, 0]) })),
  'control direction disagrees': (h, c) => h.send(frame({ type: 1, endpoint: 0x00, transfer: 0, seq: 1, length: 0, setup: Buffer.from([0x80, 6, 0, 1, 0, 0, 0, 0]), payload: Buffer.alloc(0) })),
  'setup on bulk': (h, c) => h.send(frame({ type: 1, seq: 1, length: 8, setup: Buffer.from([1, 0, 0, 0, 0, 0, 0, 0]) })),
  'OUT without its payload': (h, c) => h.send(frame({ type: 1, endpoint: 0x02, seq: 1, length: 8 })),
  'IN with a payload': (h, c) => h.send(frame({ type: 1, endpoint: 0x81, seq: 1, length: 8, payload: Buffer.alloc(8) })),
  'too large': (h, c) => h.send(frame({ type: 1, endpoint: 0x81, seq: 1, length: usbLimits.maxUrbBytes + 1 })),
  'submit with a status': (h, c) => h.send(frame({ type: 1, seq: 1, length: 8, status: -1 })),
  'unknown device': (h, c) => h.send(frame({ type: 1, device: 9, seq: 1, length: 8 })),
  'unlink of nothing carries data': (h, c) => h.send(frame({ type: 3, endpoint: 0, transfer: 0, seq: 1, length: 0, payload: Buffer.alloc(1) })),
  'return of nothing': async (h, c) => { c.send(frame({ type: 2, seq: 77 })); },
  'return with the wrong endpoint': async (h, c) => { h.send(frame({ type: 1, endpoint: 0x81, seq: 5, length: 8 })); await c.take(isFrame); c.send(frame({ type: 2, endpoint: 0x82, seq: 5 })); },
  'return longer than asked': async (h, c) => { h.send(frame({ type: 1, endpoint: 0x81, seq: 5, length: 8 })); await c.take(isFrame); c.send(frame({ type: 2, endpoint: 0x81, seq: 5, length: 9, payload: Buffer.alloc(9) })); },
  'return payload disagrees with its length': async (h, c) => { h.send(frame({ type: 1, endpoint: 0x81, seq: 5, length: 8 })); await c.take(isFrame); c.send(frame({ type: 2, endpoint: 0x81, seq: 5, length: 4, payload: Buffer.alloc(5) })); },
  'return with an unknown status': async (h, c) => { h.send(frame({ type: 1, endpoint: 0x81, seq: 5, length: 8 })); await c.take(isFrame); c.send(frame({ type: 2, endpoint: 0x81, seq: 5, status: -1234 })); },
  'return twice': async (h, c) => { h.send(frame({ type: 1, endpoint: 0x81, seq: 5, length: 8 })); await c.take(isFrame); c.send(frame({ type: 2, endpoint: 0x81, seq: 5 })); await h.take(isFrame); c.send(frame({ type: 2, endpoint: 0x81, seq: 5 })); },
  'seq reused in flight': async (h, c) => { h.send(frame({ type: 1, endpoint: 0x81, seq: 5, length: 8 })); h.send(frame({ type: 1, endpoint: 0x81, seq: 5, length: 8 })); },
  'window of bytes exceeded': async (h, c) => { for (let n = 1; n <= 5; n++) h.send(frame({ type: 1, endpoint: 0x81, seq: n, length: usbLimits.maxUrbBytes })); },
  'too many requests in flight': async (h, c) => { for (let n = 1; n <= usbLimits.maxInFlight + 1; n++) h.send(frame({ type: 1, endpoint: 0x81, seq: n, length: 8 })); },
  'text on the binary path with a binary header-less message': (h, c) => c.send(Buffer.from('not a frame, but long enough to pass the header size check!!')),
};
for (const [name, act] of Object.entries(violations)) {
  test(`Frame violation closes only the USB channel: ${name}`, async t => {
    const f = await fixture(t, { usbDevices: true, drives: true }); const host = await f.host(usbHost);
    const { sessionId, controller } = await f.stream(host, { usbProtocol: 'usb-v1' });
    const { usbHost: h, usbClient: c } = await f.usbPair(sessionId);
    c.json(attach(1, stick)); await h.take(isText); h.json({ type: 'usb.attached', device: 1 }); await c.take(isText);
    await act(h, c);
    await Promise.all([closed(h), closed(c)]);
    assert.equal(controller.readyState, WebSocket.OPEN, 'the stream stays');
    assert.equal(f.relay().sessions.has(sessionId), true);
  });
}

test('Attach rules: descriptors, policy denials, class permissions and limits are answered by the relay itself', async t => {
  const f = await fixture(t); const host = await f.host(usbHost);   // usbDevices only, no drives
  const { sessionId } = await f.stream(host, { usbProtocol: 'usb-v1' });
  const { usbHost: h, usbClient: c } = await f.usbPair(sessionId);
  c.json(attach(1, stick));
  assert.deepEqual(await c.take(isText), { type: 'usb.rejected', device: 1, reason: 'usb_class_not_granted' });
  const keyboard = Buffer.from(vectorFile()['boot keyboard'].descriptors, 'hex');
  c.json(attach(2, keyboard)); assert.deepEqual(await c.take(isText), { type: 'usb.rejected', device: 2, reason: 'usb_hid_boot_denied' });
  c.json(attach(3, Buffer.from(vectorFile()['hub'].descriptors, 'hex'))); assert.equal((await c.take(isText)).reason, 'usb_class_denied');
  c.json(attach(4, Buffer.from(vectorFile()['webcam (video)'].descriptors, 'hex'))); assert.equal((await c.take(isText)).reason, 'usb_isochronous_denied');
  c.json(attach(5, Buffer.from('not descriptors, but base64 decodable'))); assert.equal((await c.take(isText)).reason, 'usb_descriptor_invalid');
  assert.equal(h.queue.length, 0, 'the Host never sees a refused device');
  // A serial chip needs usbDevices only: accepted, and the Host is asked.
  c.json(attach(6, serial)); assert.equal((await h.take(isText)).device, 6);
  // Four at a time at most.
  for (const id of [7, 8, 9]) { c.json(attach(id, serial)); assert.equal((await h.take(isText)).device, id); }
  c.json(attach(10, serial)); assert.deepEqual(await c.take(isText), { type: 'usb.rejected', device: 10, reason: 'usb_limit_reached' });
  // An id is used once per session, even after a refusal.
  c.json(attach(1, serial));
  await Promise.all([closed(h), closed(c)]);
});

test('Malformed control messages close the channel; the Host cannot invent devices or answers', async t => {
  const cases = {
    'client attach with extra field': (h, c) => c.json({ ...attach(1, serial), note: 'x' }),
    'client attach id 0': (h, c) => c.json(attach(0, serial)),
    'client attach bad speed': (h, c) => c.json({ ...attach(1, serial), speed: 'super' }),
    'client attach non-canonical base64': (h, c) => c.json({ ...attach(1, serial), descriptors: serial.toString('base64').replace(/=+$/, '') + '!' }),
    'client detach free text': (h, c) => c.json({ type: 'usb.detach', device: 1, reason: 'my own words' }),
    'client detach of an unknown device': (h, c) => c.json({ type: 'usb.detach', device: 9, reason: 'usb_user_stopped' }),
    'client unknown message': (h, c) => c.json({ type: 'usb.format' }),
    'Host attached an unrequested device': (h, c) => h.json({ type: 'usb.attached', device: 1 }),
    'Host attached twice': async (h, c) => { c.json(attach(1, serial)); await h.take(isText); h.json({ type: 'usb.attached', device: 1 }); h.json({ type: 'usb.attached', device: 1 }); },
    'Host free-text rejection': async (h, c) => { c.json(attach(1, serial)); await h.take(isText); h.json({ type: 'usb.rejected', device: 1, reason: 'because' }); },
    'Host status with a free-text reason': (h, c) => h.json({ type: 'usb.status', state: 'unavailable', reason: 'because' }),
    'Host status ready with a reason': (h, c) => h.json({ type: 'usb.status', state: 'ready', reason: 'usb_setup_failed' }),
    'Host attach message': (h, c) => h.json(attach(1, serial)),
    'binary text mix': (h, c) => c.send(Buffer.alloc(3)),
    'oversized text': (h, c) => c.send(JSON.stringify({ type: 'usb.attach', pad: 'x'.repeat(17000) })),
  };
  for (const [name, act] of Object.entries(cases)) {
    const f = await fixture(t); const host = await f.host(usbHost);
    const { sessionId, controller } = await f.stream(host, { usbProtocol: 'usb-v1' });
    const { usbHost: h, usbClient: c } = await f.usbPair(sessionId);
    await act(h, c);
    await Promise.all([closed(h), closed(c)]);
    assert.equal(controller.readyState, WebSocket.OPEN, name);
  }
});

test('Rejections, Host detaches and client detaches update both ends and retire the id', async t => {
  const f = await fixture(t); const host = await f.host(usbHost);
  const { sessionId } = await f.stream(host, { usbProtocol: 'usb-v1' });
  const { usbHost: h, usbClient: c } = await f.usbPair(sessionId);
  c.json(attach(1, serial)); await h.take(isText);
  h.json({ type: 'usb.rejected', device: 1, reason: 'usb_attach_failed' });
  assert.deepEqual(await c.take(isText), { type: 'usb.rejected', device: 1, reason: 'usb_attach_failed' });
  c.json(attach(2, serial)); await h.take(isText); h.json({ type: 'usb.attached', device: 2 }); await c.take(isText);
  h.send(frame({ type: 1, device: 2, endpoint: 0x81, seq: 1, length: 64 })); await c.take(isFrame);
  // The Host detaches (the VM removed it); a late return from the client is dropped, not an error.
  h.json({ type: 'usb.detach', device: 2, reason: 'usb_vm_detached' });
  assert.deepEqual(await c.take(isText), { type: 'usb.detach', device: 2, reason: 'usb_vm_detached' });
  c.send(frame({ type: 2, device: 2, endpoint: 0x81, seq: 1 }));
  c.json({ type: 'usb.detach', device: 2, reason: 'usb_device_removed' });
  c.json(attach(3, serial)); assert.equal((await h.take(isText)).device, 3);
  assert.equal(h.queue.length, 0);
  h.json({ type: 'usb.attached', device: 3 }); await c.take(isText);
  c.json({ type: 'usb.detach', device: 3, reason: 'usb_user_stopped' });
  assert.deepEqual(await h.take(isText), { type: 'usb.detach', device: 3, reason: 'usb_user_stopped' });
  assert.equal(f.relay().sessions.get(sessionId).usbPair.devices.size, 0);
});

test('Revoking usbDevices closes the channel but not the stream; revoking a class permission detaches only that device', async t => {
  const f = await fixture(t, { usbDevices: true, drives: true }); const host = await f.host(usbHost);
  const { sessionId, controller } = await f.stream(host, { usbProtocol: 'usb-v1' });
  const { usbHost: h, usbClient: c } = await f.usbPair(sessionId);
  c.json(attach(1, stick)); await h.take(isText); h.json({ type: 'usb.attached', device: 1 }); await c.take(isText);
  c.json(attach(2, serial)); await h.take(isText); h.json({ type: 'usb.attached', device: 2 }); await c.take(isText);
  h.send(frame({ type: 1, device: 1, endpoint: 0x81, seq: 1, length: 64 })); await c.take(isFrame);
  f.setPolicy({ usbDevices: true });                  // drives revoked
  f.relay().devicePolicyChanged(f.store.findUser('alice').id);
  const detach = { type: 'usb.detach', device: 1, reason: 'usb_policy_changed' };
  assert.deepEqual(await c.take(isText), detach); assert.deepEqual(await h.take(isText), detach);
  assert.equal(h.readyState, WebSocket.OPEN, 'the serial device stays');
  assert.equal(f.relay().sessions.get(sessionId).usbPair.flightBytes, 0, 'the removed device\'s requests no longer hold the window');
  f.setPolicy({});                                     // usbDevices revoked
  const detached = Promise.all([closed(h), closed(c)]);
  f.relay().devicePolicyChanged(f.store.findUser('alice').id);
  await detached;
  assert.equal(controller.readyState, WebSocket.OPEN, 'the stream stays');
});

test('A second channel generation is refused after close, and ending the session ends the channel', async t => {
  const f = await fixture(t); const host = await f.host(usbHost);
  const { sessionId, controller } = await f.stream(host, { usbProtocol: 'usb-v1' });
  const { usbHost: h, usbClient: c } = await f.usbPair(sessionId);
  c.json({ type: 'usb.format' }); await Promise.all([closed(h), closed(c)]);
  assert.equal(await f.refused(`/api/v1/sessions/${sessionId}/usb`, 'Bearer ' + f.alice), 409, 'a closed pair is not reopened within the session');
  controller.close();
  await new Promise(resolve => setTimeout(resolve, 100));
});

test('A burst of honest traffic stays inside the relay queue cap', async t => {
  const f = await fixture(t); const host = await f.host(usbHost);
  const { sessionId } = await f.stream(host, { usbProtocol: 'usb-v1' });
  const { usbHost: h, usbClient: c } = await f.usbPair(sessionId);
  c.json(attach(1, serial)); await h.take(isText); h.json({ type: 'usb.attached', device: 1 }); await c.take(isText);
  // The full window (1 MiB in 4 x 256 KiB) in flight at once, then returned.
  const block = Buffer.alloc(usbLimits.maxUrbBytes, 3);
  for (let n = 1; n <= 4; n++) h.send(frame({ type: 1, endpoint: 0x02, seq: n, length: block.length, payload: block }));
  for (let n = 1; n <= 4; n++) assert.equal(parse(await c.take(isFrame)).seq, n);
  for (let n = 1; n <= 4; n++) c.send(frame({ type: 2, endpoint: 0x02, seq: n, length: block.length }));
  for (let n = 1; n <= 4; n++) assert.equal(parse(await h.take(isFrame)).seq, n);
  assert.equal(h.readyState, WebSocket.OPEN);
  assert.ok(usbStatuses.includes(-32));
});

test('USB diagnostics keep fixed codes and counts and drop anything that identifies the hardware', () => {
  const report = {
    protocol: 'client-diagnostics-v1', runId: 'a'.repeat(32), sequence: 1, version: '0.4.85',
    events: [{ time: '2026-10-26T10:00:00.000Z', event: 'usb', mode: 'stream', code: 'usb_class_denied',
      values: { usbDevices: 1, usbUrbs: 12, usbKib: 340, usbClass: 3, vendorId: 4660, productId: 22136, serial: 'ABC', name: 'SanDisk Cruzer' } },
    { time: '2026-10-26T10:00:01.000Z', event: 'usb', code: 'free text from a device', values: {} }],
  };
  const safe = safeClientDiagnosticReport(report);
  assert.deepEqual(safe.events[0].values, { usbDevices: 1, usbUrbs: 12, usbKib: 340, usbClass: 3 });
  assert.equal(safe.events[0].code, 'usb_class_denied');
  assert.equal(safe.events[1].code, 'unknown_failure');
  assert.equal(JSON.stringify(safe).includes('SanDisk'), false);
  // The raw USBD status of the first failed request is a fixed usb.h number.
  const summary = safeClientDiagnosticReport({ ...report, events: [{ time: '2026-11-02T07:40:10.012Z', event: 'usb', values: { stage: 9, ok: true, error: 71, usbReq: 32774, usbUsbd: 0x80000800 } }] });
  assert.deepEqual(summary.events[0].values, { stage: 9, ok: true, error: 71, usbReq: 32774, usbUsbd: 0x80000800 });
  // Codes of a device another remote window holds (이 창으로 옮기기) are kept as sent.
  for (const code of ['usb_device_redirected', 'usb_move_timeout']) {
    const kept = safeClientDiagnosticReport({ ...report, events: [{ time: '2026-11-02T08:00:00.000Z', event: 'usb', code, values: { stage: 13, ok: false } }] });
    assert.equal(kept.events[0].code, code);
  }
});
test('The Host is told which class permissions came with the grant', async t => {
  const f = await fixture(t, { usbDevices: true, drives: true, serialPorts: true }); const host = await f.host(usbHost);
  const { start } = await f.stream(host, { usbProtocol: 'usb-v1' });
  assert.deepEqual(start.usbGrant, { drives: true, printers: false, smartCards: false, serialPorts: true });
});
test('A largest request arrives as it is sent from the Host in 8 KiB fragments (WinHTTP sends no larger pieces)', async t => {
  const f = await fixture(t); const host = await f.host(usbHost);
  const { sessionId } = await f.stream(host, { usbProtocol: 'usb-v1' });
  const { usbHost: h, usbClient: c } = await f.usbPair(sessionId);
  c.json(attach(1, serial)); await h.take(isText); h.json({ type: 'usb.attached', device: 1 }); await c.take(isText);
  const block = Buffer.alloc(usbLimits.maxUrbBytes, 5);
  const whole = frame({ type: 1, endpoint: 0x02, seq: 1, length: block.length, payload: block });
  const pieces = Math.ceil(whole.length / 8192);
  assert.ok(pieces > 32);
  for (let n = 0; n < pieces; n++) h.send(whole.subarray(n * 8192, (n + 1) * 8192), { binary: true, fin: n === pieces - 1 });
  const arrived = await c.take(isFrame);
  assert.equal(arrived.length, whole.length);
  assert.equal(h.readyState, WebSocket.OPEN);
});