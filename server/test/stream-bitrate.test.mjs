import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { bitrateCapability, validBitrateFeedback, startBitrateFields, negotiateBitrate } from '../src/stream-bitrate.mjs';

// Adaptive bitrate negotiation (bitrate-adaptive-v1, docs/adaptive-bitrate.md):
// old/new host x fixed/auto x ceilings, the arrival-feedback path and the
// raised statistics bound. Real broker, real WebSockets, fake host/client.
const origin = 'https://bitrate-fixture.example';
const password = 'Bitrate-fixture-password-only!';
const machineUuid = '12345678-1234-1234-1234-123456789abc';
const legacyHost = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true };
const adaptiveHost = { ...legacyHost, bitrateProtocol: 'bitrate-adaptive-v1', maxBitrate: 100000000 };
const streamInfo = { type: 'stream.info', codec: 'h264', width: 1920, height: 1080, sourceWidth: 1920, sourceHeight: 1080, fpsTarget: 60, audio: { sampleRate: 48000, channels: 2, format: 's16le', available: false } };

async function fixture(t) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const hash = await hashPassword(password);
  store.addUser({ name: 'admin', passwordHash: hash, role: 'admin' });
  store.addUser({ name: 'alice', passwordHash: hash, role: 'user' });
  store.syncProxmox([{ vmid: 170, node: 'pve', name: 'VM 170', status: 'running' }], 'fixture');
  store.assign('alice', 'pve-170');
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
  const code = (await call('/admin/desktops/pve-170/enrollment', { ...admin, method: 'POST', data: {} })).body.code;
  const wsUrl = path => base.replace('http:', 'ws:') + path;
  async function host(capabilities) {
    const enrolled = await call('/hosts/enroll', { method: 'POST', data: { code, hostname: 'Fixture VM', machineUuid, version: '0.2.50' } });
    assert.equal(enrolled.status, 201);
    const ws = await socket(wsUrl('/api/v1/hosts/channel'), 'Host ' + enrolled.body.hostToken);
    ws.json({ type: 'host.capabilities', capabilities });
    assert.equal((await ws.take(value => value.type === 'host.capabilities.ack')).accepted, true);
    return ws;
  }
  const connect = options => call('/desktops/pve-170/connect', { method: 'POST', token: alice, data: { mode: 'stream', ...(options === undefined ? {} : { options }) } });
  const client = sessionId => socket(wsUrl('/api/v1/sessions/' + sessionId), 'Bearer ' + alice);
  return { call, alice, host, connect, client };
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
const feedback = { type: 'stream.feedback', epoch: 0, frames: [[16667, 5000123], [33334, 5016790]], bytes: 41000, intervalUs: 100000 };

test('Bitrate capability and feedback shapes are exact; malformed values are omitted, not fatal', () => {
  assert.deepEqual(bitrateCapability(adaptiveHost), { bitrateProtocol: 'bitrate-adaptive-v1', maxBitrate: 100000000 });
  for (const value of [legacyHost, { ...adaptiveHost, maxBitrate: 19999999 }, { ...adaptiveHost, maxBitrate: 100000001 }, { ...adaptiveHost, maxBitrate: '100000000' }, { ...adaptiveHost, bitrateProtocol: 'bitrate-adaptive-v2' }, { bitrateProtocol: 'bitrate-adaptive-v1' }])
    assert.equal(bitrateCapability(value), null);
  assert.equal(validBitrateFeedback(feedback), true);
  for (const value of [{ ...feedback, epoch: 65536 }, { ...feedback, frames: [[1]] }, { ...feedback, frames: Array(65).fill([1, 2]) }, { ...feedback, frames: [[-1, 2]] }, { ...feedback, intervalUs: 0 }, { ...feedback, bytes: 1.5 }, { ...feedback, sessionId: 'x' }, { ...feedback, relayQueuedBytes: 0 }])
    assert.equal(validBitrateFeedback(value), false);
  assert.deepEqual(startBitrateFields({ bitrate: 100000000 }, true), { bitrate: 20000000, bitrateMode: 'auto', bitrateMax: 100000000 });
  assert.deepEqual(startBitrateFields({ bitrate: 12000000 }, true), { bitrate: 12000000, bitrateMode: 'auto', bitrateMax: 12000000 });
  assert.deepEqual(startBitrateFields({ bitrate: 60000000 }, false), { bitrate: 60000000 });
  // Negotiation matrix without sockets: host capability x mode x ceiling x tracks.
  const adaptive = { capabilities: adaptiveHost }, legacy = { capabilities: legacyHost };
  assert.equal(negotiateBitrate({ bitrate: 20000000 }, legacy, null), false);
  assert.equal(negotiateBitrate({ bitrate: 20000000 }, adaptive, null), false);
  assert.equal(negotiateBitrate({ bitrate: 100000000, bitrateMode: 'auto' }, adaptive, null), true);
  assert.equal(negotiateBitrate({ bitrate: 100000000 }, adaptive, [{ id: '0' }]), false);
  const code = fn => { try { fn(); return 'accepted'; } catch (error) { return error.code; } };
  assert.equal(code(() => negotiateBitrate({ bitrate: 100000000, bitrateMode: 'auto' }, adaptive, [{ id: '0' }])), 'invalid_stream_options');
  assert.equal(code(() => negotiateBitrate({ bitrate: 100000000, bitrateMode: 'auto' }, legacy, null)), 'stream_bitrate_unavailable');
  assert.equal(code(() => negotiateBitrate({ bitrate: 30000000 }, legacy, null)), 'invalid_stream_options');
  assert.equal(code(() => negotiateBitrate({ bitrate: 20000000, bitrateMode: 'auto' }, { capabilities: undefined }, null)), 'stream_bitrate_unavailable');
});

test('Released host: today\'s fixed 4-20 Mbps behaviour; auto and higher ceilings are refused before any session', async t => {
  const f = await fixture(t), host = await f.host(legacyHost);
  const desktop = (await f.call('/desktops', { token: f.alice })).body.desktops[0];
  assert.equal(desktop.host.capabilities.bitrateProtocol, undefined); assert.equal(desktop.host.capabilities.maxBitrate, undefined);
  assert.equal((await f.connect({ bitrate: 100000000, bitrateMode: 'auto' })).body.error.code, 'stream_bitrate_unavailable');
  assert.equal((await f.connect({ bitrate: 20000001 })).body.error.code, 'invalid_stream_options');
  assert.equal((await f.connect({ bitrate: 60000000, bitrateMode: 'fixed' })).body.error.code, 'invalid_stream_options');
  const legacy = { maxWidth: 1920, maxHeight: 1080, fps: 60, bitrate: 20000000, audio: true, monitor: '0' };
  const response = await f.connect(legacy);
  assert.equal(response.status, 200); assert.deepEqual(response.body.options, legacy); assert.equal(response.body.bitrateProtocol, undefined);
  const client = await f.client(response.body.sessionId);
  const start = await host.take(value => value.type === 'session.start');
  assert.equal(start.bitrate, 20000000); assert.equal('bitrateMode' in start, false); assert.equal('bitrateMax' in start, false);
  // A fixed session has no feedback path: it is still invalid input.
  client.json(feedback);
  assert.equal((await client.take(value => value.type === 'session.ended')).reason, 'invalid_input');
});

test('Adaptive host: auto with a 100 Mbps ceiling starts at 20 Mbps, forwards arrival feedback and wider statistics', async t => {
  const f = await fixture(t), host = await f.host(adaptiveHost);
  const desktop = (await f.call('/desktops', { token: f.alice })).body.desktops[0];
  assert.equal(desktop.host.capabilities.bitrateProtocol, 'bitrate-adaptive-v1'); assert.equal(desktop.host.capabilities.maxBitrate, 100000000);
  assert.equal((await f.connect({ bitrate: 100000001, bitrateMode: 'auto' })).body.error.code, 'invalid_stream_options');
  assert.equal((await f.connect({ bitrate: 20000000, bitrateMode: 'turbo' })).body.error.code, 'invalid_stream_options');
  const response = await f.connect({ maxWidth: 3840, maxHeight: 2160, fps: 60, bitrate: 100000000, bitrateMode: 'auto', audio: true, monitor: '0' });
  assert.equal(response.status, 200); assert.equal(response.body.bitrateProtocol, 'bitrate-adaptive-v1');
  assert.deepEqual(response.body.options, { maxWidth: 3840, maxHeight: 2160, fps: 60, bitrate: 100000000, bitrateMode: 'auto', audio: true, monitor: '0' });
  const client = await f.client(response.body.sessionId);
  const start = await host.take(value => value.type === 'session.start');
  assert.equal(start.bitrate, 20000000); assert.equal(start.bitrateMode, 'auto'); assert.equal(start.bitrateMax, 100000000);
  host.json({ ...streamInfo, bitrate: 20000000, bitrateMode: 'auto', bitrateMax: 100000000 }); await client.take(value => value.type === 'stream.info');
  client.json(feedback);
  const forwarded = await host.take(value => value.type === 'stream.feedback');
  assert.deepEqual({ ...forwarded, relayQueuedBytes: 0 }, { ...feedback, sessionId: response.body.sessionId, relayQueuedBytes: 0 });
  assert.equal(Number.isInteger(forwarded.relayQueuedBytes) && forwarded.relayQueuedBytes >= 0, true);
  // 150 Mbps measured output (above the released 100000 kbps bound) is relayed.
  host.json({ type: 'stream.stats', capturedFps: 60, encodedFps: 60, bitrateKbps: 150000, bitrateTarget: 100000000, bitrateMode: 'auto', networkDelayMs: 12.5 });
  const stats = await client.take(value => value.type === 'stream.stats');
  assert.equal(stats.bitrateTarget, 100000000); assert.equal(stats.networkDelayMs, 12.5);
  // Malformed feedback ends the session like any other invalid input.
  client.json({ ...feedback, frames: [[1, 2, 3]] });
  assert.equal((await client.take(value => value.type === 'session.ended')).reason, 'invalid_input');
  await closed(client);
});

test('Adaptive host: manual fixed ceilings up to the advertised maximum; auto start follows a low ceiling', async t => {
  const f = await fixture(t), host = await f.host({ ...adaptiveHost, maxBitrate: 50000000 });
  assert.equal((await f.connect({ bitrate: 60000000 })).body.error.code, 'stream_bitrate_unavailable');
  assert.equal((await f.connect({ bitrate: 60000000, bitrateMode: 'auto' })).body.error.code, 'stream_bitrate_unavailable');
  const fixed = await f.connect({ bitrate: 50000000 });
  assert.equal(fixed.status, 200); assert.equal(fixed.body.bitrateProtocol, undefined); assert.equal(fixed.body.options.bitrateMode, undefined);
  let client = await f.client(fixed.body.sessionId);
  let start = await host.take(value => value.type === 'session.start');
  assert.equal(start.bitrate, 50000000); assert.equal('bitrateMode' in start, false);
  client.json({ type: 'session.stop' }); await host.take(value => value.type === 'session.stop'); await closed(client);
  const low = await f.connect({ bitrate: 12000000, bitrateMode: 'auto' });
  assert.equal(low.status, 200); assert.equal(low.body.bitrateProtocol, 'bitrate-adaptive-v1');
  client = await f.client(low.body.sessionId);
  start = await host.take(value => value.type === 'session.start');
  assert.equal(start.bitrate, 12000000); assert.equal(start.bitrateMax, 12000000); assert.equal(start.bitrateMode, 'auto');
  client.json({ type: 'session.stop' }); await host.take(value => value.type === 'session.stop'); await closed(client);
  const explicitFixed = await f.connect({ bitrate: 20000000, bitrateMode: 'fixed' });
  assert.equal(explicitFixed.status, 200); assert.equal(explicitFixed.body.options.bitrateMode, 'fixed'); assert.equal(explicitFixed.body.bitrateProtocol, undefined);
  client = await f.client(explicitFixed.body.sessionId);
  start = await host.take(value => value.type === 'session.start');
  assert.equal(start.bitrate, 20000000); assert.equal('bitrateMode' in start, false);
});

test('A malformed bitrate capability keeps the host usable at the released fixed rate', async t => {
  const f = await fixture(t);
  await f.host({ ...legacyHost, bitrateProtocol: 'bitrate-adaptive-v1', maxBitrate: 500000000 });
  const desktop = (await f.call('/desktops', { token: f.alice })).body.desktops[0];
  assert.equal(desktop.canConnect, true); assert.equal(desktop.host.capabilities.bitrateProtocol, undefined);
  assert.equal((await f.connect({ bitrate: 100000000, bitrateMode: 'auto' })).body.error.code, 'stream_bitrate_unavailable');
  assert.equal((await f.connect({ bitrate: 20000000 })).status, 200);
});
