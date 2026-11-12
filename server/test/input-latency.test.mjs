import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { inputLatencyCapability, requestedInputLatency, negotiateInputLatency, inputLatencyState, admitInputSample, inputLatencyHostMessage, inputLatencySamplesPerSecond, inputLatencyTimedPacket, recordInputLatencyForward, inputLatencyForwardStats, holdEarlyDirectInputReport, takeEarlyDirectInputReports } from '../src/input-latency.mjs';

// Negotiated input latency measurement (input-latency-v1): exact negotiation,
// strict relay validation of the new field/messages, and released peers that
// keep exactly today's behaviour. Real broker and WebSockets, fake host/client.
const origin = 'https://latency-fixture.example';
const password = 'Latency-fixture-password-only!';
const machineUuid = '12345678-1234-1234-1234-123456789abd';
const releasedHost = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true };
const measuringHost = { ...releasedHost, inputLatencyProtocol: 'input-latency-v1' };
const streamInfo = { type: 'stream.info', codec: 'h264', width: 1920, height: 1080, sourceWidth: 1920, sourceHeight: 1080, fpsTarget: 60, audio: { sampleRate: 48000, channels: 2, format: 's16le', available: false } };
const move = { type: 'input', event: 'move', x: 0.25, y: 0.75 };

test('Early direct timing waits for admission, expires, and never crosses a session', () => {
  const session = { id: 'direct-one', mode: 'stream', udpDirect: true, streamReady: true, epoch: 0, inputLatency: inputLatencyState() };
  const ack = sample => ({ type: 'input.ack', sessionId: session.id, sample, ok: true, injectUs: 10, holdUs: 20 });
  assert.equal(holdEarlyDirectInputReport({ ...session, udpDirect: false }, ack(1), 1000), undefined);
  assert.equal(holdEarlyDirectInputReport(session, { ...ack(1), sessionId: 'retired' }, 1000), true);
  assert.equal(session.inputLatency.earlyDirect, undefined);
  assert.equal(holdEarlyDirectInputReport(session, { ...ack(1), holdUs: 9 }, 1000), null);
  assert.equal(holdEarlyDirectInputReport(session, ack(1), 1000), true);
  admitInputSample(session, { ...move, sample: 1 }, 1100);
  assert.deepEqual(takeEarlyDirectInputReports(session, 1, 1100), [ack(1)]);
  assert.deepEqual(takeEarlyDirectInputReports(session, 1, 1100), []);
  holdEarlyDirectInputReport(session, ack(2), 1200);
  admitInputSample(session, { ...move, sample: 2 }, 3301);
  assert.deepEqual(takeEarlyDirectInputReports(session, 2, 3301), []);
  for (let sample = 3; sample < 100; sample++) holdEarlyDirectInputReport(session, ack(sample), 4000);
  assert.equal(session.inputLatency.earlyDirect.size, 64);
});
const click = { type: 'input', event: 'button', button: 'left', down: true, x: 0.25, y: 0.75 };

async function fixture(t) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const hash = await hashPassword(password);
  store.addUser({ name: 'admin', passwordHash: hash, role: 'admin' });
  store.addUser({ name: 'alice', passwordHash: hash, role: 'user' });
  store.syncProxmox([{ vmid: 171, node: 'pve', name: 'VM 171', status: 'running' }], 'fixture');
  store.assign('alice', 'pve-171');
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
  const code = (await call('/admin/desktops/pve-171/enrollment', { ...admin, method: 'POST', data: {} })).body.code;
  const wsUrl = path => base.replace('http:', 'ws:') + path;
  async function host(capabilities) {
    const enrolled = await call('/hosts/enroll', { method: 'POST', data: { code, hostname: 'Fixture VM', machineUuid, version: '0.2.50' } });
    assert.equal(enrolled.status, 201);
    const ws = await socket(wsUrl('/api/v1/hosts/channel'), 'Host ' + enrolled.body.hostToken);
    ws.json({ type: 'host.capabilities', capabilities });
    assert.equal((await ws.take(value => value.type === 'host.capabilities.ack')).accepted, true);
    return ws;
  }
  const connect = extra => call('/desktops/pve-171/connect', { method: 'POST', token: alice, data: { mode: 'stream', ...extra } });
  const client = sessionId => socket(wsUrl('/api/v1/sessions/' + sessionId), 'Bearer ' + alice);
  // An attached, ready stream: connect, attach the controller, deliver stream.info.
  async function stream(hostSocket, extra = {}) {
    const response = await connect(extra);
    assert.equal(response.status, 200);
    const controller = await client(response.body.sessionId);
    const start = await hostSocket.take(value => value.type === 'session.start');
    hostSocket.json(streamInfo); await controller.take(value => value.type === 'stream.info');
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

test('Capability, request and negotiation are exact; measurement never refuses a connection', () => {
  assert.equal(inputLatencyCapability(measuringHost), true);
  for (const value of [releasedHost, { ...releasedHost, inputLatencyProtocol: 'input-latency-v2' }, { ...releasedHost, inputLatencyProtocol: true }, undefined, null])
    assert.equal(inputLatencyCapability(value), false);
  assert.equal(requestedInputLatency('stream', undefined), false);
  assert.equal(requestedInputLatency('stream', 'input-latency-v1'), true);
  assert.equal(code(() => requestedInputLatency('rdp', 'input-latency-v1')), 'invalid_input_latency_protocol');
  assert.equal(code(() => requestedInputLatency('stream', 'input-latency-v2')), 'invalid_input_latency_protocol');
  assert.equal(code(() => requestedInputLatency('stream', '')), 'invalid_input_latency_protocol');
  const capable = { capabilities: measuringHost }, released = { capabilities: releasedHost };
  assert.equal(negotiateInputLatency(true, capable, null), true);
  assert.equal(negotiateInputLatency(false, capable, null), false, 'never granted unrequested');
  assert.equal(negotiateInputLatency(true, released, null), false, 'released host: unmeasured, not refused');
  assert.equal(negotiateInputLatency(true, { capabilities: undefined }, null), false);
  assert.equal(negotiateInputLatency(true, capable, [{ id: '0' }]), false, 'tracks-v1 is not measured by this protocol');
});

test('Client sample admission: unchanged without the field, strict with it, bounded per second', () => {
  const session = { inputLatency: inputLatencyState() }, unmeasured = { inputLatency: null };
  assert.equal(admitInputSample(unmeasured, move), move, 'released clients keep the exact forwarded value');
  assert.equal(admitInputSample(session, move), move);
  assert.equal(admitInputSample(unmeasured, { ...move, sample: 1 }), null, 'unnegotiated sample is invalid input');
  for (const sample of [0, -1, 1.5, '2', 2147483648, null, true]) assert.equal(admitInputSample({ inputLatency: inputLatencyState() }, { ...move, sample }), null, String(sample));
  assert.equal(admitInputSample({ inputLatency: inputLatencyState() }, { type: 'ping', id: 'x', sample: 1 }), null, 'only input may carry a sample');
  assert.equal(admitInputSample({ inputLatency: inputLatencyState() }, { type: 'input.reset', sample: 1 }), null);
  const sampled = { ...click, sample: 7 };
  assert.equal(admitInputSample(session, sampled, 1000), sampled);
  assert.equal(admitInputSample(session, { ...click, sample: 7 }, 1001), null, 'samples strictly increase');
  assert.equal(admitInputSample(session, { ...click, sample: 6 }, 1002), null);
  // Rate bound: beyond 20 per second the input is still delivered, unmeasured.
  const bounded = { inputLatency: inputLatencyState() };
  const forwarded = Array.from({ length: inputLatencySamplesPerSecond + 5 }, (_, n) => admitInputSample(bounded, { ...move, sample: n + 1 }, 5000 + n));
  assert.equal(forwarded.filter(value => 'sample' in value).length, inputLatencySamplesPerSecond);
  assert.deepEqual(forwarded.at(-1), move, 'over-rate input loses only its sample field');
  assert.equal(bounded.inputLatency.outstanding.size, inputLatencySamplesPerSecond, 'stripped samples are never awaited');
  assert.equal('sample' in admitInputSample(bounded, { ...move, sample: 100 }, 6100), true, 'the next second measures again');
});

test('Host reports: exact shape, own session, once per forwarded sample, tags only for current media', () => {
  const state = inputLatencyState();
  const session = { id: 'session-a', mode: 'stream', inputLatency: state, streamReady: true, resize: null, epoch: 0 };
  admitInputSample(session, { ...move, sample: 3 }, 1); admitInputSample(session, { ...click, sample: 4 }, 2);
  const ack = { type: 'input.ack', sessionId: 'session-a', sample: 3, ok: true, injectUs: 180, holdUs: 950 };
  assert.deepEqual(inputLatencyHostMessage(session, ack), ack);
  assert.equal(inputLatencyHostMessage(session, ack), false, 'a duplicate acknowledgement is not forwarded');
  assert.equal(inputLatencyHostMessage(session, { ...ack, sample: 4, sessionId: 'session-old' }), false, 'a retired session cannot report into its successor');
  for (const bad of [{ ...ack, sample: 4, extra: 1 }, { ...ack, sample: 5 }, { ...ack, sample: 4, ok: 'yes' }, { ...ack, sample: 4, injectUs: 951 }, { ...ack, sample: 4, holdUs: 10000001, injectUs: 0 }, { ...ack, sample: 4, injectUs: -1 }, { ...ack, sample: 4, holdUs: 1.5 }, { type: 'input.ack', sessionId: 'session-a', sample: 4, ok: true, injectUs: 1 }])
    assert.equal(inputLatencyHostMessage(session, bad), null, JSON.stringify(bad));
  assert.equal(inputLatencyHostMessage({ ...session, inputLatency: null }, { ...ack, sample: 4 }), null, 'an unnegotiated session never relays reports');
  const frame = { type: 'input.frame', sessionId: 'session-a', epoch: 0, sample: 4, pts: 1234567 };
  assert.equal(inputLatencyHostMessage({ ...session, streamReady: false }, frame), false);
  assert.equal(inputLatencyHostMessage({ ...session, resize: { epoch: 1 } }, frame), false);
  assert.equal(inputLatencyHostMessage(session, { ...frame, epoch: 1 }), false, 'old/new epoch media is not this decoder');
  for (const bad of [{ ...frame, pts: -1 }, { ...frame, epoch: 65536 }, { ...frame, sample: 9 }, { ...frame, x: 0.5 }, { type: 'input.frame', sessionId: 'session-a', sample: 4, pts: 1 }])
    assert.equal(inputLatencyHostMessage(session, bad), null, JSON.stringify(bad));
  assert.deepEqual(inputLatencyHostMessage(session, frame), frame);
  assert.equal(inputLatencyHostMessage(session, frame), false, 'each sample tags at most one frame');
  assert.equal(inputLatencyHostMessage(session, { ...frame, sample: 3 }), false, 'tags follow injection order');
  // frame-timing-v1: all three host durations (integers, 0..10 s) or none.
  const timedSession = { ...session, inputLatency: inputLatencyState() };
  admitInputSample(timedSession, { ...click, sample: 7 }, 3);
  const timed = { type: 'input.frame', sessionId: 'session-a', epoch: 0, sample: 7, pts: 2000000, captureUs: 18310, encodeUs: 5520, queueUs: 640 };
  for (const bad of [{ ...timed, queueUs: undefined }, { ...timed, captureUs: -1 }, { ...timed, encodeUs: 10000001 }, { ...timed, queueUs: 1.5 }, { ...timed, captureUs: '1' }, { ...timed, extra: 1 }])
    assert.equal(inputLatencyHostMessage(timedSession, JSON.parse(JSON.stringify(bad))), null, JSON.stringify(bad));
  assert.deepEqual(inputLatencyHostMessage(timedSession, timed), timed);
});

const videoPacket = (pts, size = 64, key = false) => { const packet = Buffer.alloc(12 + size); packet[0] = 1; packet[1] = key ? 1 : 0; packet.writeBigUInt64LE(BigInt(pts), 4); return packet; };

test('Relay forward timing: only the packet the last tag named, bounded, nearest-rank, last 30 s', () => {
  const session = { id: 'session-a', mode: 'stream', inputLatency: inputLatencyState(), streamReady: true, resize: null, epoch: 0 };
  const state = session.inputLatency;
  assert.equal(inputLatencyTimedPacket(state, videoPacket(100)), false, 'nothing is timed before a tag');
  admitInputSample(session, { ...click, sample: 1 }, 1);
  assert.ok(inputLatencyHostMessage(session, { type: 'input.frame', sessionId: 'session-a', epoch: 0, sample: 1, pts: 200 }));
  assert.equal(inputLatencyTimedPacket(state, videoPacket(199)), false, 'an earlier packet (a tile at pts-1) is not timed');
  assert.equal(inputLatencyTimedPacket(state, Buffer.from([2, 0, 0, 0, 200, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0])), false, 'audio is not timed');
  assert.equal(inputLatencyTimedPacket(state, videoPacket(200)), true);
  assert.equal(inputLatencyTimedPacket(state, videoPacket(200)), false, 'one packet per tag');
  admitInputSample(session, { ...click, sample: 2 }, 2);
  assert.ok(inputLatencyHostMessage(session, { type: 'input.frame', sessionId: 'session-a', epoch: 0, sample: 2, pts: 300 }));
  assert.equal(inputLatencyTimedPacket(state, videoPacket(316)), false, 'a later packet ends the wait');
  assert.equal(inputLatencyTimedPacket(state, videoPacket(300)), false);
  assert.equal(inputLatencyForwardStats(state, 1000), null);
  for (const [ms, at] of [[4, 1000], [1, 1000], [9, 1000], [2, 1000], [40000, 1000], [-1, 1000], [Number.NaN, 1000]]) recordInputLatencyForward(state, ms, at);
  assert.deepEqual(inputLatencyForwardStats(state, 2000), { samples: 5, p50Ms: 4, p95Ms: 10000 }, 'invalid values ignored, large ones bounded');
  assert.equal(inputLatencyForwardStats(state, 1000 + 30001), null, 'older than 30 s');
  for (let n = 0; n < 100; n++) recordInputLatencyForward(state, 1, 50000);
  assert.equal(inputLatencyForwardStats(state, 50000).samples, 64);
  assert.equal(inputLatencyForwardStats(null), null);
});

test('Negotiated session: stream.stats gains the relay share of tagged packets; released sessions keep exact statistics', async t => {
  const f = await fixture(t), host = await f.host(measuringHost);
  const stats = { type: 'stream.stats', capturedFps: 30, encodedFps: 10, bitrateKbps: 900 };
  const plain = await f.stream(host);
  host.json(stats);
  assert.deepEqual(await plain.controller.take(value => value.type === 'stream.stats'), stats, 'an unmeasured session is unchanged');
  plain.controller.close(); await host.take(value => value.type === 'session.stop');
  const { controller, sessionId } = await f.stream(host, { inputLatencyProtocol: 'input-latency-v1' });
  host.json(stats);
  assert.deepEqual(await controller.take(value => value.type === 'stream.stats'), stats, 'no member before a timed packet');
  controller.json({ ...click, sample: 1 }); await host.take(value => value.type === 'input');
  host.json({ type: 'input.frame', sessionId, epoch: 0, sample: 1, pts: 33333 });
  await controller.take(value => value.type === 'input.frame');
  host.send(videoPacket(33333, 50000, true));
  assert.equal(Buffer.isBuffer(await controller.take(value => Buffer.isBuffer(value))), true);
  host.json(stats);
  const measured = await controller.take(value => value.type === 'stream.stats');
  assert.deepEqual(Object.keys(measured.relayForward).sort(), ['p50Ms', 'p95Ms', 'samples']);
  assert.equal(measured.relayForward.samples, 1);
  assert.ok(measured.relayForward.p50Ms >= 0 && measured.relayForward.p50Ms < 2000);
  assert.deepEqual({ ...measured, relayForward: undefined }, { ...stats, relayForward: undefined }, 'the host statistics are otherwise unchanged');
});

test('Released host: no capability, no grant, no session.start field; sampled input is invalid', async t => {
  const f = await fixture(t), host = await f.host(releasedHost);
  const desktop = (await f.call('/desktops', { token: f.alice })).body.desktops[0];
  assert.equal(desktop.host.capabilities.inputLatencyProtocol, undefined);
  const { response, controller, start } = await f.stream(host, { inputLatencyProtocol: 'input-latency-v1' });
  assert.equal(response.body.inputLatencyProtocol, undefined, 'requested but not granted');
  assert.equal('inputLatencyProtocol' in start, false);
  controller.json(move);
  assert.deepEqual(await host.take(value => value.type === 'input'), move);
  controller.json({ ...move, sample: 1 });
  assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_input');
  await closed(controller);
});

test('Malformed requests are refused before any session; RDP never carries the marker', async t => {
  const f = await fixture(t); await f.host({ ...measuringHost });
  assert.equal((await f.connect({ inputLatencyProtocol: 'input-latency-v2' })).body.error.code, 'invalid_input_latency_protocol');
  assert.equal((await f.connect({ inputLatencyProtocol: 1 })).body.error.code, 'invalid_input_latency_protocol');
  // RDP is not offered: refused as such, so it never carries the marker.
  const rdp = await f.call('/desktops/pve-171/connect', { method: 'POST', token: f.alice, data: { mode: 'rdp', inputLatencyProtocol: 'input-latency-v1' } });
  assert.equal(rdp.status, 409); assert.equal(rdp.body.error.code, 'rdp_not_supported'); assert.equal(rdp.body.inputLatencyProtocol, undefined);
  const plain = await f.connect({});
  assert.equal(plain.status, 200, 'a released client request is unchanged'); assert.equal(plain.body.inputLatencyProtocol, undefined);
});

test('Released client with a measuring host: exact released input; an unrequested sample is invalid', async t => {
  const f = await fixture(t), host = await f.host(measuringHost);
  const desktop = (await f.call('/desktops', { token: f.alice })).body.desktops[0];
  assert.equal(desktop.host.capabilities.inputLatencyProtocol, 'input-latency-v1');
  const { response, controller, start } = await f.stream(host);
  assert.equal(response.body.inputLatencyProtocol, undefined);
  assert.equal('inputLatencyProtocol' in start, false, 'the host is never asked to measure');
  assert.equal('inputFrameTimingProtocol' in start, false);
  for (const value of [move, click, { type: 'input', event: 'key', vk: 65, scan: 30, down: true, extended: false }])
    { controller.json(value); assert.deepEqual(await host.take(item => item.type === 'input'), value); }
  controller.json({ ...click, sample: 1 });
  assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_input');
  await host.take(value => value.type === 'session.stop'); await closed(controller);
  assert.equal(host.queue.some(value => value.type === 'input' && 'sample' in value), false, 'an unrequested sample never reaches the host');
  // Reports of an unnegotiated session are a host protocol violation.
  const second = await f.stream(host);
  host.json({ type: 'input.ack', sessionId: second.sessionId, sample: 1, ok: true, injectUs: 1, holdUs: 2 });
  await closed(host);
  assert.equal((await second.controller.take(value => value.type === 'session.ended')).reason, 'invalid_control');
});

test('Negotiated session: sampled input reaches the host in order, acknowledgements and frame tags reach the controller', async t => {
  const f = await fixture(t), host = await f.host(measuringHost);
  const { response, controller, start, sessionId } = await f.stream(host, { inputLatencyProtocol: 'input-latency-v1' });
  assert.equal(response.body.inputLatencyProtocol, 'input-latency-v1');
  assert.equal(start.inputLatencyProtocol, 'input-latency-v1');
  assert.equal(start.inputFrameTimingProtocol, 'frame-timing-v1', 'this broker accepts timed tags');
  const sequence = [move, { ...click, sample: 1 }, { ...move, x: 0.5 }, { type: 'input', event: 'key', vk: 65, scan: 30, down: true, extended: false, sample: 2 }];
  for (const value of sequence) controller.json(value);
  for (const value of sequence) assert.deepEqual(await host.take(item => item.type === 'input'), value, 'order and content are unchanged');
  host.json({ type: 'input.ack', sessionId, sample: 1, ok: true, injectUs: 210, holdUs: 1400 });
  assert.deepEqual(await controller.take(value => value.type === 'input.ack'), { type: 'input.ack', sessionId, sample: 1, ok: true, injectUs: 210, holdUs: 1400 });
  host.json({ type: 'input.frame', sessionId, epoch: 0, sample: 1, pts: 5016700 });
  assert.deepEqual(await controller.take(value => value.type === 'input.frame'), { type: 'input.frame', sessionId, epoch: 0, sample: 1, pts: 5016700 });
  // A late report of another session is ignored without dropping the host.
  host.json({ type: 'input.ack', sessionId: '00000000-0000-4000-8000-000000000000', sample: 2, ok: true, injectUs: 1, holdUs: 2 });
  host.json({ type: 'input.ack', sessionId, sample: 2, ok: false, injectUs: 90, holdUs: 300 });
  assert.equal((await controller.take(value => value.type === 'input.ack')).sample, 2);
  assert.equal(controller.queue.some(value => value.type === 'input.ack'), false, 'the foreign report was not forwarded');
  // The over-rate tail is delivered without its sample and never acknowledged.
  for (let n = 0; n < inputLatencySamplesPerSecond + 3; n++) controller.json({ ...move, sample: 10 + n });
  const delivered = [];
  for (let n = 0; n < inputLatencySamplesPerSecond + 3; n++) delivered.push(await host.take(item => item.type === 'input'));
  const stripped = delivered.filter(value => !('sample' in value));
  assert.ok(stripped.length >= 3, 'samples beyond the per-second bound lose only their field');
  assert.ok(stripped.every(value => JSON.stringify(value) === JSON.stringify(move)));
  // A malformed report drops the host channel like any invalid control.
  host.json({ type: 'input.ack', sessionId, sample: 10, ok: true, injectUs: 5, holdUs: 4 });
  await closed(host);
  assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_control');
});
