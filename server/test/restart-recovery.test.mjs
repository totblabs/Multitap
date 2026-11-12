// Restart recovery: a broker update restarts the process and ends
// every stream as server_stopping. The Host channel closes with 1012
// Service Restart (the Host reconnects at once), and a client waits for its
// desktop's Host with GET /api/v1/desktops/:id/host-ready (host-ready-wait-v1)
// instead of reading the whole list every half second.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { setTimeout as delay } from 'node:timers/promises';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { HostReadyWaits } from '../src/host-ready-wait.mjs';

const origin = 'https://restart-fixture.example';
const password = 'Restart-fixture-password-only!';
const machineUuid = '12345678-1234-1234-1234-123456789abc';
const capabilities = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true };
const streamInfo = { type: 'stream.info', codec: 'h264', width: 1280, height: 720, sourceWidth: 1280, sourceHeight: 720, fpsTarget: 60, audio: { sampleRate: 48000, channels: 2, format: 's16le', available: false } };

async function enrolledStore() {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const hash = await hashPassword(password);
  store.addUser({ name: 'admin', passwordHash: hash, role: 'admin' });
  store.addUser({ name: 'alice', passwordHash: hash });
  store.addUser({ name: 'bobby', passwordHash: hash });
  store.syncProxmox([{ vmid: 170, node: 'pve', name: 'VM 170', status: 'running' }], 'fixture');
  store.assign('alice', 'pve-170');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve-fixture.example', tokenId: 'fixture@pve!broker', secret: 'only-fixture-secret' }), 'fixture');
  return { store, vault };
}
// One broker process: a server on `port` (0: any) over a store that outlives it, as the SQLite file does.
async function broker({ store, vault }, port = 0, relayOptions = {}, inventoryMs = 0) {
  const proxmox = { machineIdentity: async () => ({ machineUuid }), inventory: async () => { await delay(inventoryMs); return [{ vmid: 170, node: 'pve', name: 'VM 170', status: 'running' }]; } };
  const server = await buildServer({ store, vault, proxmox, publicOrigin: origin, relayOptions: { authorizationMs: 20, heartbeatMs: 200, ...relayOptions }, logger: { error: value => assert.fail(value) } });
  await new Promise((resolve, reject) => { server.once('error', reject); server.listen(port, '127.0.0.1', resolve); });
  const base = `http://127.0.0.1:${server.address().port}`;
  async function call(path, { method = 'GET', data, token, cookie, csrf, signal } = {}) {
    const response = await fetch(base + '/api/v1' + path, { method, signal,
      headers: { ...(token ? { Authorization: 'Bearer ' + token } : {}), ...(cookie ? { Cookie: cookie } : {}), ...(method !== 'GET' ? { 'Content-Type': 'application/json', Origin: origin, ...(csrf ? { 'X-CSRF-Token': csrf } : {}) } : {}) },
      body: data === undefined ? undefined : JSON.stringify(data) });
    return { status: response.status, body: await response.json(), headers: response.headers };
  }
  const closed = () => new Promise(resolve => server.close(resolve));
  return { server, base, call, closed, port: server.address().port, wsUrl: path => base.replace('http:', 'ws:') + path };
}
async function signIn(b) {
  const adminLogin = await b.call('/admin/login', { method: 'POST', data: { username: 'admin', password } });
  const admin = { cookie: adminLogin.headers.get('set-cookie').split(';')[0], csrf: adminLogin.body.csrfToken };
  const alice = (await b.call('/auth/login', { method: 'POST', data: { username: 'alice', password } })).body.sessionToken;
  const bobby = (await b.call('/auth/login', { method: 'POST', data: { username: 'bobby', password } })).body.sessionToken;
  const code = (await b.call('/admin/desktops/pve-170/enrollment', { ...admin, method: 'POST', data: {} })).body.code;
  const enrolled = await b.call('/hosts/enroll', { method: 'POST', data: { code, hostname: 'Fixture VM', machineUuid, version: '0.2.0' } });
  assert.equal(enrolled.status, 201);
  return { alice, bobby, hostToken: enrolled.body.hostToken };
}
function socket(url, authorization, { signal, onSocket } = {}) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url, { headers: { Authorization: authorization }, perMessageDeflate: false, handshakeTimeout: 3000 });
    onSocket?.(ws);
    const abort = () => ws.terminate();
    signal?.addEventListener('abort', abort, { once: true });
    ws.once('close', () => signal?.removeEventListener('abort', abort));
    ws.once('error', reject);
    ws.once('unexpected-response', (_req, response) => { response.resume(); reject(Object.assign(new Error('upgrade_rejected'), { status: response.statusCode })); });
    ws.once('open', () => {
      const queue = [], waiters = [];
      ws.once('close', () => {
        for (const waiter of waiters.splice(0)) { clearTimeout(waiter.timer); waiter.reject(new Error('socket_closed')); }
      });
      ws.on('message', (data, binary) => {
        const value = binary ? data : JSON.parse(data.toString('utf8'));
        const index = waiters.findIndex(waiter => waiter.predicate(value));
        if (index >= 0) { const waiter = waiters.splice(index, 1)[0]; clearTimeout(waiter.timer); waiter.resolve(value); } else queue.push(value);
      });
      ws.take = (predicate = () => true, ms = 3000) => {
        const index = queue.findIndex(predicate);
        if (index >= 0) return Promise.resolve(queue.splice(index, 1)[0]);
        if (ws.readyState !== WebSocket.OPEN) return Promise.reject(new Error('socket_closed'));
        return new Promise((resolve, reject) => { const waiter = { predicate, resolve, reject, timer: setTimeout(() => reject(new Error('message_timeout')), ms) }; waiters.push(waiter); });
      };
      ws.json = value => ws.send(JSON.stringify(value));
      resolve(ws);
    });
    if (signal?.aborted) abort();
  });
}
const type = name => value => !Buffer.isBuffer(value) && value.type === name;
async function host(b, token, options) {
  const ws = await socket(b.wsUrl('/api/v1/hosts/channel'), 'Host ' + token, options);
  ws.json({ type: 'host.capabilities', capabilities });
  assert.equal((await ws.take(type('host.capabilities.ack'))).accepted, true);
  return ws;
}
// A synthetic relay frame with an optional fresh marker, not decodable H.264.
const keyFrame = (epoch, marker = Buffer.alloc(0)) => { const bytes = Buffer.alloc(64); bytes[0] = 1; bytes[1] = 1; bytes.writeUInt16LE(epoch, 2); marker.copy(bytes, 8); return bytes; };
async function stream(b, token, hostWs, { signal, onSocket, marker, unauthorizedToken } = {}) {
  const connection = await b.call('/desktops/pve-170/connect', { method: 'POST', token, data: { mode: 'stream' }, signal });
  assert.equal(connection.status, 200, JSON.stringify(connection.body));
  const url = connection.body.webSocketUrl.replace(origin.replace('https:', 'wss:'), b.base.replace('http:', 'ws:'));
  const client = await socket(url, 'Bearer ' + token, { signal, onSocket });
  if (unauthorizedToken) await assert.rejects(socket(url, 'Bearer ' + unauthorizedToken, { signal, onSocket }), error => error.status === 401, 'An unassigned account attached to the fresh session');
  const start = await hostWs.take(type('session.start'));
  assert.equal(start.sessionId, connection.body.sessionId);
  hostWs.json({ ...streamInfo, sessionId: start.sessionId, epoch: start.epoch ?? 0 });
  await client.take(type('stream.info'));
  const sentFrame = keyFrame(start.epoch ?? 0, marker);
  hostWs.send(sentFrame);
  const receivedFrame = await client.take(value => Buffer.isBuffer(value) && value[0] === 1);
  assert.deepEqual(receivedFrame, sentFrame, 'The authorized Client did not receive the exact fresh scripted frame');
  return { client, sessionId: start.sessionId, sentFrame, receivedFrame };
}

test('A broker restart closes each Host channel with 1012 after its session.stop, without delaying the shutdown', async () => {
  const state = await enrolledStore();
  const b = await broker(state); const { alice, hostToken } = await signIn(b);
  const hostWs = await host(b, hostToken);
  const { client } = await stream(b, alice, hostWs);
  const closedAt = once(hostWs, 'close'), ended = client.take(type('session.ended'));
  const began = performance.now();
  await b.closed();
  const shutdownMs = performance.now() - began;
  const [code, reason] = await closedAt;
  assert.equal(code, 1012, 'The Host channel did not close as Service Restart');
  assert.equal(reason.toString(), 'server_stopping');
  assert.equal((await ended).reason, 'server_stopping');
  // The stop reached the Host before the close frame, on the same channel.
  assert.equal((await hostWs.take(type('session.stop'))).reason, 'server_stopping');
  assert.ok(shutdownMs < 500, `Shutdown waited ${shutdownMs.toFixed(0)} ms for answering Hosts`);
  state.store.close();
});

test('A Host that never answers the close frame delays the shutdown by at most the relay close timeout', async () => {
  const state = await enrolledStore();
  const b = await broker(state); const { hostToken } = await signIn(b);
  const hostWs = await host(b, hostToken);
  hostWs._socket.pause(); // reads nothing more, so it never answers the close
  const began = performance.now();
  await b.closed();
  const shutdownMs = performance.now() - began;
  assert.ok(shutdownMs < 2000, `Shutdown took ${shutdownMs.toFixed(0)} ms`);
  hostWs.terminate(); state.store.close();
});

test('host-ready answers the moment the Host reports, after the hold without it, and only to assigned accounts', async () => {
  const state = await enrolledStore();
  const b = await broker(state, 0, { hostReadyOptions: { holdMs: 300 } }); const { alice, bobby, hostToken } = await signIn(b);
  try {
    const list = await b.call('/desktops', { token: alice });
    assert.equal(list.body.desktops[0].hostReadyProtocol, 'host-ready-wait-v1');
    // No Host: the hold ends with ready:false.
    let began = performance.now();
    let answer = await b.call('/desktops/pve-170/host-ready', { token: alice });
    assert.deepEqual(answer.body, { protocol: 'host-ready-wait-v1', ready: false });
    assert.ok(performance.now() - began >= 250, 'The wait did not hold');
    // Others: unassigned, unknown, unauthenticated, wrong method, a query.
    assert.equal((await b.call('/desktops/pve-170/host-ready', { token: bobby })).body.error.code, 'desktop_not_found');
    assert.equal((await b.call('/desktops/pve-999/host-ready', { token: alice })).status, 404);
    assert.equal((await b.call('/desktops/pve-170/host-ready')).status, 401);
    assert.equal((await b.call('/desktops/pve-170/host-ready', { method: 'POST', token: alice, data: {} })).status, 405);
    // The Host arrives during a wait: answered with its capabilities, not at the hold's end.
    const slow = await broker(state, 0, { hostReadyOptions: { holdMs: 5000 } });
    try {
      const waiting = slow.call('/desktops/pve-170/host-ready', { token: alice });
      await delay(100);
      began = performance.now();
      const hostWs = await host(slow, hostToken);
      answer = await waiting;
      const lateMs = performance.now() - began;
      assert.deepEqual(answer.body, { protocol: 'host-ready-wait-v1', ready: true });
      assert.ok(lateMs < 1000, `Answered ${lateMs.toFixed(0)} ms after the Host connected`);
      // Already reported: at once.
      began = performance.now();
      assert.equal((await slow.call('/desktops/pve-170/host-ready', { token: alice })).body.ready, true);
      assert.ok(performance.now() - began < 250);
      hostWs.close();
    } finally { await slow.closed(); }
  } finally { await b.closed(); state.store.close(); }
});

test('A held host-ready wait ends at once when the broker closes', async () => {
  const state = await enrolledStore();
  const b = await broker(state, 0, { hostReadyOptions: { holdMs: 10000 } }); const { alice } = await signIn(b);
  const waiting = b.call('/desktops/pve-170/host-ready', { token: alice });
  await delay(100);
  const began = performance.now();
  await b.closed();
  assert.deepEqual((await waiting).body, { protocol: 'host-ready-wait-v1', ready: false });
  assert.ok(performance.now() - began < 1000, 'A held wait delayed the shutdown');
  state.store.close();
});

test('A request still running at shutdown does not keep the process for the keep-alive time', async () => {
  const state = await enrolledStore();
  const b = await broker(state, 0, {}, 300); const { alice } = await signIn(b);
  await delay(2100); // past the power cache: the next list read asks the (slow) Proxmox stub
  const reading = b.call('/desktops', { token: alice });
  await delay(50);
  const began = performance.now();
  await b.closed();
  assert.equal((await reading).status, 200);
  const shutdownMs = performance.now() - began;
  assert.ok(shutdownMs < 1000, `Shutdown waited ${shutdownMs.toFixed(0)} ms for an answered request's connection`);
  state.store.close();
});

test('Waits are bounded per account and in total, and a client that leaves ends its wait', async () => {
  let reported = false;
  const waits = new HostReadyWaits({ reported: () => reported, holdMs: 60000, maxWaits: 3, maxPerUser: 2 });
  const a = waits.wait('pve-170', 'u1'), b = waits.wait('pve-170', 'u1');
  assert.throws(() => waits.wait('pve-170', 'u1'), error => error.status === 429 && error.code === 'too_many_host_ready_waits');
  const c = waits.wait('pve-171', 'u2');
  assert.throws(() => waits.wait('pve-170', 'u3'), error => error.status === 429);
  waits.wake('pve-170'); assert.equal(waits.size, 3, 'A wake before the Host reported ended a wait');
  reported = true; waits.wake('pve-170');
  assert.deepEqual(await Promise.all([a, b]), [true, true]);
  // The request's response closing ends its wait.
  const { EventEmitter } = await import('node:events');
  const response = new EventEmitter();
  reported = false;
  const left = waits.wait('pve-172', 'u2', response);
  response.emit('close');
  assert.equal(await left, false); assert.equal(waits.size, 1);
  waits.close(); assert.equal(await c, false); assert.equal(waits.size, 0);
  assert.equal(await waits.wait('pve-170', 'u1'), false, 'A closed broker held a wait');
});

// Real broker listeners with scripted Host/Client peers and synthetic relay
// bytes. This verifies recovery, authentication and freshness, not H.264
// decoding or field performance. Startup duration and discrete retry phases
// differ between runs, so one schedule is not required to always be faster.
function assertDrillRecovery(value) {
  assert.equal(value.hostCloseCode, 1012, 'Host close must announce Service Restart');
  assert.equal(value.hostCloseReason, 'server_stopping', 'Host close reason must identify broker shutdown');
  assert.equal(value.oldSessionEndReason, 'server_stopping', 'The old session must end at shutdown');
  assert.equal(typeof value.oldSessionId, 'string'); assert.ok(value.oldSessionId);
  assert.equal(typeof value.newSessionId, 'string'); assert.ok(value.newSessionId);
  assert.notEqual(value.newSessionId, value.oldSessionId, 'Recovery must create a fresh session');
  assert.ok(Buffer.isBuffer(value.sentFrame) && Buffer.isBuffer(value.receivedFrame) && Buffer.isBuffer(value.oldFrame));
  assert.notDeepEqual(value.sentFrame, value.oldFrame, 'Recovery must send a fresh frame marker');
  assert.deepEqual(value.receivedFrame, value.sentFrame, 'Recovery must relay the exact fresh frame');
  for (const name of ['brokerReadyMs', 'hostMs', 'frameMs', 'deadlineMs']) assert.ok(Number.isFinite(value[name]) && value[name] >= 0, 'Recovery timestamps must be observed and finite');
  assert.ok(value.hostMs >= value.brokerReadyMs, 'Host readiness must follow the new broker listener');
  assert.ok(value.frameMs >= value.hostMs, 'The fresh frame must follow authenticated Host readiness');
  assert.ok(value.frameMs <= value.deadlineMs, 'Recovery must finish within its explicit deadline');
}
async function drill({ fast, downMs, startupDelayMs = 0, deadlineMs = 15000, onCleanup }) {
  const state = await enrolledStore();
  const controller = new AbortController(), { signal } = controller;
  const sockets = new Set(), brokers = new Set(), jobs = [];
  const options = { signal, onSocket: ws => sockets.add(ws) };
  // Attach rejection handlers immediately; cleanup joins every asynchronous job.
  const observe = promise => { const job = promise.then(value => ({ value }), error => ({ error })); jobs.push(job); return job; };
  const result = async job => { const settled = await job; if (settled.error) throw settled.error; return settled.value; };
  let deadline;
  try {
    const original = await broker(state); brokers.add(original);
    const { alice, bobby, hostToken } = await signIn(original), port = original.port;
    let hostWs = await host(original, hostToken, options);
    const oldMarker = randomBytes(16), old = await stream(original, alice, hostWs, { ...options, marker: oldMarker });
    const stoppedAt = performance.now();
    deadline = setTimeout(() => controller.abort(new Error('restart_drill_deadline')), deadlineMs); deadline.unref();
    const hostClosed = observe(once(hostWs, 'close')), oldEnded = observe(old.client.take(type('session.ended')));
    let brokerReadyMs;
    const restarted = observe((async () => {
      await original.closed(); await delay(downMs, undefined, { signal }); await delay(startupDelayMs, undefined, { signal });
      const fresh = await broker(state, port); brokers.add(fresh); brokerReadyMs = performance.now() - stoppedAt;
      signal.throwIfAborted(); return fresh;
    })());
    const [code, reason] = await result(hostClosed), oldEnd = await result(oldEnded);
    assert.equal(code, 1012); assert.equal(reason.toString(), 'server_stopping'); assert.equal(oldEnd.reason, 'server_stopping');
    const steps = fast ? [250, 500, 1000, 1000, 1000, 1000] : [];
    const hostBack = observe((async () => {
      for (let attempt = 0; ; ++attempt) {
        await delay(steps[attempt] ?? 2000, undefined, { signal });
        try { hostWs = await host({ wsUrl: path => `ws://127.0.0.1:${port}${path}` }, hostToken, options); return performance.now() - stoppedAt; }
        catch (error) { if (signal.aborted) throw error; /* broker still down */ }
      }
    })());
    const get = async path => {
      try { const r = await fetch(`http://127.0.0.1:${port}/api/v1${path}`, { headers: { Authorization: 'Bearer ' + alice }, signal }); return { status: r.status, body: await r.json() }; }
      catch (error) { if (signal.aborted) throw error; return { status: 0 }; }
    };
    let reads = 0;
    for (;;) {
      signal.throwIfAborted(); const list = await get('/desktops'); ++reads;
      const desktop = list.status === 200 ? list.body.desktops[0] : null;
      if (desktop?.host?.connected && desktop.canConnect) break;
      if (fast && desktop?.hostReadyProtocol === 'host-ready-wait-v1') { await get('/desktops/pve-170/host-ready'); continue; }
      await delay(fast ? 250 : 500, undefined, { signal });
    }
    const fresh = await result(restarted), hostMs = await result(hostBack);
    await assert.rejects(socket(fresh.wsUrl('/api/v1/sessions/' + old.sessionId), 'Bearer ' + alice, options), error => error.status === 401, 'A session from the stopped broker remained attachable');
    let marker; do { marker = randomBytes(16); } while (marker.equals(oldMarker));
    const next = await stream(fresh, alice, hostWs, { ...options, marker, unauthorizedToken: bobby });
    const frameMs = performance.now() - stoppedAt;
    assertDrillRecovery({ hostCloseCode: code, hostCloseReason: reason.toString(), oldSessionEndReason: oldEnd.reason,
      oldSessionId: old.sessionId, newSessionId: next.sessionId, oldFrame: old.receivedFrame, sentFrame: next.sentFrame, receivedFrame: next.receivedFrame,
      brokerReadyMs, hostMs, frameMs, deadlineMs });
    return { hostMs, frameMs, brokerReadyMs, listReads: reads, freshSession: true, exactFreshFrame: true, oldSessionRejected: true, unassignedClientRejected: true };
  } finally {
    clearTimeout(deadline); controller.abort();
    for (const ws of sockets) if (ws.readyState !== WebSocket.CLOSED) ws.terminate();
    await Promise.all(jobs);
    // No reconnect job may create another socket now. Join close events too,
    // including refused HTTP upgrades that had not completed their handshake.
    await Promise.all([...sockets].map(ws => ws.readyState === WebSocket.CLOSED ? Promise.resolve() : new Promise(resolve => { ws.once('close', resolve); ws.terminate(); })));
    for (const b of brokers) await b.closed();
    state.store.close();
    onCleanup?.({ openSockets: [...sockets].filter(ws => ws.readyState !== WebSocket.CLOSED).length, openListeners: [...brokers].filter(b => b.server.listening).length, joinedJobs: jobs.length });
  }
}

test('Restart drill (synthetic relay): both reconnect schedules recover a fresh authorized session and exact frame', { timeout: 65000 }, async () => {
  const rows = [];
  for (const downMs of [300, 1200]) {
    const before = await drill({ fast: false, downMs }), after = await drill({ fast: true, downMs });
    rows.push({ downMs, before, after });
  }
  console.log(JSON.stringify({ simulated: true, syntheticRelayFrames: true, actualH264Decoded: false, alwaysFasterClaim: false,
    restartDrill: rows.map(({ downMs, before, after }) => ({ downMs, before, after })) }));
});

test('Restart drill assertions reject stale sessions/frames, wrong shutdown codes and invalid readiness/deadline ordering', () => {
  const good = { hostCloseCode: 1012, hostCloseReason: 'server_stopping', oldSessionEndReason: 'server_stopping', oldSessionId: 'old-fixture', newSessionId: 'fresh-fixture',
    oldFrame: keyFrame(0, Buffer.alloc(16, 1)), sentFrame: keyFrame(0, Buffer.alloc(16, 2)), receivedFrame: keyFrame(0, Buffer.alloc(16, 2)), brokerReadyMs: 2000, hostMs: 2750, frameMs: 2800, deadlineMs: 15000 };
  assertDrillRecovery(good);
  for (const changed of [{ hostCloseCode: 1000 }, { hostCloseReason: 'other' }, { oldSessionEndReason: 'other' }, { newSessionId: good.oldSessionId },
    { sentFrame: good.oldFrame, receivedFrame: good.oldFrame }, { receivedFrame: good.oldFrame }, { brokerReadyMs: 3000 }, { frameMs: 2500 }, { deadlineMs: 2799 }, { brokerReadyMs: NaN }]) {
    assert.throws(() => assertDrillRecovery({ ...good, ...changed }), assert.AssertionError);
  }
});

test('Restart drill tolerates controlled slower startup and cleans up after a real deadline abort', { timeout: 25000 }, async () => {
  const delayed = await drill({ fast: true, downMs: 1200, startupDelayMs: 700 });
  console.log(JSON.stringify({ simulated: true, controlledStartupDelayMs: 700, actualH264Decoded: false, alwaysFasterClaim: false, delayed }));
  let cleanup;
  await assert.rejects(drill({ fast: true, downMs: 1200, startupDelayMs: 700, deadlineMs: 100, onCleanup: value => { cleanup = value; } }), error => error.name === 'AbortError' || error.message === 'restart_drill_deadline');
  assert.equal(cleanup?.openSockets, 0, 'An aborted drill leaked a WebSocket');
  assert.equal(cleanup?.openListeners, 0, 'An aborted drill leaked a broker listener');
  assert.ok(cleanup.joinedJobs >= 2, 'An aborted drill did not join its restart/reconnect jobs');
});
