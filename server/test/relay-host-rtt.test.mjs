import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { hostPingSent, hostPongReceived, withHostRtt } from '../src/stream-observability.mjs';
import { safeClientDiagnosticReport } from '../src/client-diagnostics.mjs';

// relayHostRttMaxMs: the relay's own measurement of the Host leg of the client's
// ping (relay -> Host -> relay), handed to the client with the Host's next
// stream.stats. Real broker and WebSockets, fake host/client.
const origin = 'https://host-rtt-fixture.example';
const password = 'Host-rtt-fixture-password-only!';
const machineUuid = '12345678-1234-1234-1234-123456789abe';
const capabilities = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true };
const streamInfo = { type: 'stream.info', codec: 'h264', width: 1920, height: 1080, sourceWidth: 1920, sourceHeight: 1080, fpsTarget: 60, audio: { sampleRate: 48000, channels: 2, format: 's16le', available: false } };
const stats = { type: 'stream.stats', capturedFps: 30, encodedFps: 10, bitrateKbps: 900 };

test('Host leg timing: matched by id, the largest since the last statistics, bounded', () => {
  const session = {};
  assert.equal(withHostRtt(session, stats), stats, 'nothing measured: the statistics are the same object');
  hostPongReceived(session, 'never-sent', 5);
  assert.equal(withHostRtt(session, stats), stats, 'a pong without its ping measures nothing');
  hostPingSent(session, 'a', 1000); hostPingSent(session, 'b', 2000); hostPingSent(session, 'c', 2500);
  hostPongReceived(session, 'b', 2040.4); hostPongReceived(session, 'a', 4200); hostPongReceived(session, 'c', 2507);
  hostPongReceived(session, 'a', 9000);
  assert.deepEqual(withHostRtt(session, stats), { ...stats, relayHostRttMaxMs: 3200 }, 'the largest; a repeated pong counts once');
  assert.equal(withHostRtt(session, stats), stats, 'handed over once');
  hostPingSent(session, 'd', 10000); hostPongReceived(session, 'd', 10000);
  assert.equal(withHostRtt(session, stats).relayHostRttMaxMs, 0, 'zero is a measurement');
  hostPingSent(session, 'late', 0); hostPongReceived(session, 'late', 90000);
  assert.equal(withHostRtt(session, stats).relayHostRttMaxMs, 60000, 'bounded');
  hostPingSent(session, 'back', 500); hostPongReceived(session, 'back', 400);
  assert.equal(withHostRtt(session, stats), stats, 'a clock that went back measures nothing');
  // Unanswered pings do not grow: only the newest 8 ids wait.
  for (let n = 0; n < 100; n++) hostPingSent(session, 'p' + n, n);
  assert.equal(session.hostPings.size, 8);
  hostPongReceived(session, 'p0', 500); hostPongReceived(session, 'p91', 500);
  assert.equal(withHostRtt(session, stats), stats, 'a dropped id is not measured');
  hostPongReceived(session, 'p99', 500);
  assert.equal(withHostRtt(session, stats).relayHostRttMaxMs, 401);
  // The member is only ever the relay's own.
  assert.deepEqual(withHostRtt(session, { ...stats, relayHostRttMaxMs: 7 }), stats);
  hostPingSent(session, 'e', 0); hostPongReceived(session, 'e', 12);
  assert.deepEqual(withHostRtt(session, { ...stats, relayHostRttMaxMs: 7 }), { ...stats, relayHostRttMaxMs: 12 });
});

test('The client diagnostics keep rttHostMaxMs of a streamStats row as a bounded integer', () => {
  const row = values => ({ protocol: 'client-diagnostics-v1', runId: 'a'.repeat(32), sequence: 1, version: '1.1.26', events: [{ time: '2026-11-17T01:02:03.004Z', event: 'streamStats', values }] });
  assert.deepEqual(safeClientDiagnosticReport(row({ rttMaxMs: 3214, rttHostMaxMs: 3190 })).events[0].values, { rttMaxMs: 3214, rttHostMaxMs: 3190 });
  for (const bad of [-1, 1.5, '3190', 2 ** 32]) assert.deepEqual(safeClientDiagnosticReport(row({ rttHostMaxMs: bad })).events[0].values, {}, String(bad));
});

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
  const enrolled = await call('/hosts/enroll', { method: 'POST', data: { code, hostname: 'Fixture VM', machineUuid, version: '0.2.50' } });
  assert.equal(enrolled.status, 201);
  const host = await socket(wsUrl('/api/v1/hosts/channel'), 'Host ' + enrolled.body.hostToken);
  host.json({ type: 'host.capabilities', capabilities });
  assert.equal((await host.take(value => value.type === 'host.capabilities.ack')).accepted, true);
  // An attached, ready stream: connect, attach the controller, deliver stream.info.
  const response = await call('/desktops/pve-171/connect', { method: 'POST', token: alice, data: { mode: 'stream' } });
  assert.equal(response.status, 200);
  const controller = await socket(wsUrl('/api/v1/sessions/' + response.body.sessionId), 'Bearer ' + alice);
  await host.take(value => value.type === 'session.start');
  host.json(streamInfo); await controller.take(value => value.type === 'stream.info');
  return { host, controller };
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
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));

test('A slow Host answer to the client ping reaches the client with the next stream.stats, once', async t => {
  const { host, controller } = await fixture(t);
  host.json(stats);
  assert.deepEqual(await controller.take(value => value.type === 'stream.stats'), stats, 'no ping yet: the statistics are unchanged');
  // A quick ping, then one the Host answers 300 ms late: the largest is handed over.
  controller.json({ type: 'ping', id: 'quick' });
  assert.deepEqual(await host.take(value => value.type === 'ping'), { type: 'ping', id: 'quick' }, 'the ping itself is unchanged');
  host.json({ type: 'pong', id: 'quick' });
  assert.deepEqual(await controller.take(value => value.type === 'pong'), { type: 'pong', id: 'quick' }, 'the pong itself is unchanged');
  controller.json({ type: 'ping', id: 'slow' }); await host.take(value => value.type === 'ping' && value.id === 'slow');
  const started = performance.now();
  await wait(300);
  host.json({ type: 'pong', id: 'slow' });
  await controller.take(value => value.type === 'pong' && value.id === 'slow');
  const elapsed = performance.now() - started;
  host.json(stats);
  const measured = await controller.take(value => value.type === 'stream.stats');
  assert.ok(Number.isInteger(measured.relayHostRttMaxMs), JSON.stringify(measured));
  // Timers may fire a little early; the relay's span is inside this test's send -> receive span.
  assert.ok(measured.relayHostRttMaxMs >= 280 && measured.relayHostRttMaxMs <= Math.ceil(elapsed) + 20, `${measured.relayHostRttMaxMs} ms of ${elapsed} ms`);
  assert.deepEqual({ ...measured, relayHostRttMaxMs: undefined }, { ...stats, relayHostRttMaxMs: undefined }, 'the host statistics are otherwise unchanged');
  host.json(stats);
  assert.deepEqual(await controller.take(value => value.type === 'stream.stats'), stats, 'no new pong: the member is absent');
  // A pong the client never asked for is forwarded as before and measures nothing.
  host.json({ type: 'pong', id: 'unasked' });
  await controller.take(value => value.type === 'pong' && value.id === 'unasked');
  host.json(stats);
  assert.deepEqual(await controller.take(value => value.type === 'stream.stats'), stats);
  controller.close();
});
