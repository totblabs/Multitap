// Transparent resume (session-resume-v1 / host-resume-v1, session-resume.mjs)
// and the relay's WebSocket transport settings (tuning backlog #32, #43).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { Socket } from 'node:net';
import { setTimeout as delay } from 'node:timers/promises';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';

const origin = 'https://resume-fixture.example';
const password = 'Resume-fixture-password-only!';
const machineUuid = '12345678-1234-1234-1234-123456789abc';
const streamInfo = { type: 'stream.info', codec: 'h264', width: 1280, height: 720, sourceWidth: 1920, sourceHeight: 1080, fpsTarget: 60, audio: { sampleRate: 48000, channels: 2, format: 's16le', available: false } };
const serviceCapabilities = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true, keyFrameProtocol: 'keyframe-v1' };
const resumeCapabilities = { ...serviceCapabilities, hostResumeProtocol: 'host-resume-v1' };
const tokenPattern = /^[A-Za-z0-9_-]{43}$/;

async function fixture(t, relayOptions = {}) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const hash = await hashPassword(password);
  store.addUser({ name: 'admin', passwordHash: hash, role: 'admin' });
  store.addUser({ name: 'alice', passwordHash: hash });
  store.addUser({ name: 'bobby', passwordHash: hash });
  store.syncProxmox([{ vmid: 170, node: 'pve', name: 'VM 170', status: 'running' }], 'fixture');
  store.assign('alice', 'pve-170'); store.assign('bobby', 'pve-170');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve-fixture.example', tokenId: 'fixture@pve!broker', secret: 'only-fixture-secret' }), 'fixture');
  const proxmox = { machineIdentity: async () => ({ machineUuid }) };
  const server = await buildServer({ store, vault, proxmox, publicOrigin: origin, relayOptions: { authorizationMs: 20, heartbeatMs: 200, ...relayOptions }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const base = `http://127.0.0.1:${server.address().port}`;
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  async function call(path, { method = 'GET', data, token, cookie, csrf } = {}) {
    const response = await fetch(base + '/api/v1' + path, { method,
      headers: { ...(token ? { Authorization: 'Bearer ' + token } : {}), ...(cookie ? { Cookie: cookie } : {}), ...(method !== 'GET' ? { 'Content-Type': 'application/json', Origin: origin, ...(csrf ? { 'X-CSRF-Token': csrf } : {}) } : {}) },
      body: data === undefined ? undefined : JSON.stringify(data) });
    return { status: response.status, body: await response.json(), headers: response.headers };
  }
  const adminLogin = await call('/admin/login', { method: 'POST', data: { username: 'admin', password } });
  const admin = { cookie: adminLogin.headers.get('set-cookie').split(';')[0], csrf: adminLogin.body.csrfToken };
  const alice = (await call('/auth/login', { method: 'POST', data: { username: 'alice', password } })).body.sessionToken;
  const bobby = (await call('/auth/login', { method: 'POST', data: { username: 'bobby', password } })).body.sessionToken;
  const code = (await call('/admin/desktops/pve-170/enrollment', { ...admin, method: 'POST', data: {} })).body.code;
  const enrolled = await call('/hosts/enroll', { method: 'POST', data: { code, hostname: 'Fixture VM', machineUuid, version: '0.2.0' } });
  assert.equal(enrolled.status, 201);
  const wsUrl = path => base.replace('http:', 'ws:') + path;
  return { store, server, call, alice, bobby, hostToken: enrolled.body.hostToken, wsUrl };
}
function socket(url, authorization, headers = {}, options = {}) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url, { headers: { Authorization: authorization, ...headers }, perMessageDeflate: false, ...options });
    ws.once('error', reject);
    ws.once('unexpected-response', (_req, response) => { response.resume(); reject(Object.assign(new Error('upgrade_rejected'), { status: response.statusCode })); });
    ws.once('open', () => {
      const queue = [], waiters = [];
      ws.messages = [];
      ws.on('message', (data, binary) => {
        const value = binary ? data : JSON.parse(data.toString('utf8'));
        ws.messages.push(value);
        const index = waiters.findIndex(waiter => waiter.predicate(value));
        if (index >= 0) { const waiter = waiters.splice(index, 1)[0]; clearTimeout(waiter.timer); waiter.resolve(value); }
        else queue.push(value);
      });
      ws.take = (predicate = () => true, ms = 2000) => {
        const index = queue.findIndex(predicate);
        if (index >= 0) return Promise.resolve(queue.splice(index, 1)[0]);
        return new Promise((resolve, reject) => { const waiter = { predicate, resolve, timer: setTimeout(() => reject(new Error('message_timeout')), ms) }; waiters.push(waiter); });
      };
      // Nothing matching arrives within `ms`.
      ws.none = async (predicate, ms = 300) => { await delay(ms); assert.equal(queue.some(predicate), false, 'An unexpected message arrived'); };
      ws.json = value => ws.send(JSON.stringify(value));
      resolve(ws);
    });
  });
}
const type = name => value => !Buffer.isBuffer(value) && value.type === name;
const frame = (key, marker, size = 64) => { const bytes = Buffer.alloc(size); bytes[0] = 1; bytes[1] = key ? 1 : 0; bytes[12] = marker; return bytes; };
async function host(f, capabilities = serviceCapabilities) {
  const ws = await socket(f.wsUrl('/api/v1/hosts/channel'), 'Host ' + f.hostToken);
  ws.json({ type: 'host.capabilities', capabilities });
  const ack = await ws.take(type('host.capabilities.ack'));
  assert.equal(ack.accepted, true);
  return Object.assign(ws, { ack });
}
// A streaming session that already showed its first picture.
async function streaming(f, hostWs, { resume = true } = {}) {
  const connection = await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice, data: { mode: 'stream', ...(resume ? { sessionResumeProtocol: 'session-resume-v1' } : {}) } });
  assert.equal(connection.status, 200, JSON.stringify(connection.body));
  const url = f.wsUrl('/api/v1/sessions/' + connection.body.sessionId);
  const client = await socket(url, 'Bearer ' + f.alice);
  const start = await hostWs.take(type('session.start'));
  hostWs.json(streamInfo);
  await client.take(type('stream.info'));
  hostWs.send(frame(true, 1));
  assert.equal((await client.take(Buffer.isBuffer))[12], 1);
  return { connection: connection.body, id: start.sessionId, url, client };
}
const received = (ws, markers) => { ws.on('message', (data, binary) => { if (binary) markers.push(data[12]); }); return markers; };
async function arrived(markers, count) { for (let n = 0; n < 100 && markers.length < count; n++) await delay(20); return markers; }
const audits = (f, event) => f.store.db.prepare('SELECT subject FROM audit_events WHERE event=?').all(event).map(row => row.subject);

test('session-resume-v1 is offered only where the Host restarts pictures, and granted only on request', async t => {
  const f = await fixture(t);
  const plain = await host(f, { ...serviceCapabilities, keyFrameProtocol: undefined });
  const listed = async () => (await f.call('/desktops', { token: f.alice })).body.desktops[0].host.capabilities;
  assert.equal((await listed()).sessionResumeProtocol, undefined);
  const refused = await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice, data: { mode: 'stream', sessionResumeProtocol: 'session-resume-v1' } });
  assert.equal(refused.status, 200); assert.equal(refused.body.sessionResumeProtocol, undefined); assert.equal(refused.body.resumeGraceMs, undefined);
  await f.call('/sessions/' + refused.body.sessionId, { method: 'DELETE', token: f.alice });
  plain.close(); await once(plain, 'close');
  const capable = await host(f);
  assert.equal(capable.ack.hostResumeProtocol, 'host-resume-v1');
  assert.equal((await listed()).sessionResumeProtocol, 'session-resume-v1');
  const ordinary = await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice, data: { mode: 'stream' } });
  assert.equal(ordinary.body.sessionResumeProtocol, undefined);
  const client = await socket(f.wsUrl('/api/v1/sessions/' + ordinary.body.sessionId), 'Bearer ' + f.alice);
  await capable.take(type('session.start'));
  await client.none(type('session.resume.token'));
  client.close(); await capable.take(type('session.stop'));
  for (let n = 0; n < 50 && f.server.relay.sessions.size; n++) await delay(20);
  const granted = await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice, data: { mode: 'stream', sessionResumeProtocol: 'session-resume-v1' } });
  assert.equal(granted.status, 200, JSON.stringify(granted.body));
  assert.equal(granted.body.sessionResumeProtocol, 'session-resume-v1'); assert.equal(granted.body.resumeGraceMs, 15000);
});

test('An abrupt client loss keeps the stream; the same client resumes with its token and the picture restarts at an IDR', async t => {
  const f = await fixture(t, { resumeGraceMs: 3000 });
  const hostWs = await host(f), { id, url, client } = await streaming(f, hostWs);
  const first = await client.take(type('session.resume.token'));
  assert.match(first.token, tokenPattern); assert.equal(first.graceMs, 3000); assert.equal(first.sessionId, id);
  client.terminate();
  assert.deepEqual(await hostWs.take(type('input.reset')), { type: 'input.reset', sessionId: id });
  hostWs.send(frame(false, 10)); // nobody to receive it
  await hostWs.none(type('session.stop'));
  assert.equal(f.server.relay.sessions.has(id), true);
  const back = await socket(url, 'Bearer ' + f.alice, { 'X-Multitap-Resume': first.token });
  const markers = received(back, []);
  const resumed = await back.take(type('session.resumed'));
  assert.match(resumed.token, tokenPattern); assert.notEqual(resumed.token, first.token); assert.equal(resumed.graceMs, 3000);
  assert.equal((await hostWs.take(type('stream.keyframe'))).sessionId, id);
  hostWs.send(frame(false, 11)); hostWs.send(frame(true, 12)); hostWs.send(frame(false, 13));
  assert.deepEqual(await arrived(markers, 2), [12, 13], 'Video did not restart exactly at the IDR');
  back.json({ type: 'input', event: 'move', x: .5, y: .5 });
  assert.equal((await hostWs.take(type('input'))).event, 'move');
  assert.equal(hostWs.messages.filter(type('input.reset')).length, 1, 'A detached resume sent a second reset');
  const [record] = audits(f, 'session_resumed');
  const detail = JSON.parse(record);
  assert.equal(detail.sessionId, id); assert.equal(detail.side, 'client'); assert.equal(detail.reason, 'client_disconnected'); assert.ok(detail.gapMs >= 0);
  const everything = JSON.stringify(f.store.db.prepare('SELECT * FROM audit_events').all());
  assert.equal(everything.includes(first.token) || everything.includes(resumed.token), false, 'A resume token reached the audit log');
});

test('A resume token is single-use and bound to the account; a refused resume ends the session at once', async t => {
  const f = await fixture(t, { resumeGraceMs: 3000 });
  const hostWs = await host(f), { id, url, client } = await streaming(f, hostWs);
  const first = (await client.take(type('session.resume.token'))).token;
  // Another account cannot use the URL, with or without the token.
  await assert.rejects(socket(url, 'Bearer ' + f.bobby, { 'X-Multitap-Resume': first }), error => error.status === 401);
  await hostWs.none(type('session.stop'));
  client.terminate(); await hostWs.take(type('input.reset'));
  const back = await socket(url, 'Bearer ' + f.alice, { 'X-Multitap-Resume': first });
  await back.take(type('session.resumed'));
  back.terminate(); await hostWs.take(type('input.reset'));
  // The first token was spent by the resume that used it.
  await assert.rejects(socket(url, 'Bearer ' + f.alice, { 'X-Multitap-Resume': first }), error => error.status === 401);
  assert.equal((await hostWs.take(type('session.stop'))).reason, 'client_disconnected');
  assert.equal(f.server.relay.sessions.has(id), false);
});

test('An unresumed client ends with its own reason after the grace window', async t => {
  const f = await fixture(t, { resumeGraceMs: 300 });
  const hostWs = await host(f), { id, client } = await streaming(f, hostWs);
  await client.take(type('session.resume.token'));
  client.terminate(); await hostWs.take(type('input.reset'));
  const started = Date.now(), stop = await hostWs.take(type('session.stop'));
  assert.equal(stop.reason, 'client_disconnected'); assert.ok(Date.now() - started >= 200, 'The session did not wait');
  const expired = JSON.parse(audits(f, 'session_resume_expired')[0]);
  assert.equal(expired.sessionId, id); assert.equal(expired.side, 'client');
});

test('A deliberate close, a client that did not ask and a loss during a resize end the session at once', async t => {
  const f = await fixture(t, { resumeGraceMs: 5000 });
  const hostWs = await host(f);
  const closing = await streaming(f, hostWs);
  closing.client.close(1000);
  assert.equal((await hostWs.take(type('session.stop'), 1000)).reason, 'client_disconnected');
  for (let n = 0; n < 50 && f.server.relay.sessions.size; n++) await delay(20);
  const released = await streaming(f, hostWs, { resume: false });
  released.client.terminate();
  assert.equal((await hostWs.take(type('session.stop'), 1000)).reason, 'client_disconnected');
  for (let n = 0; n < 50 && f.server.relay.sessions.size; n++) await delay(20);
  hostWs.close(); await once(hostWs, 'close');
  // A resize in flight has no stable picture to come back to.
  const resizing = await host(f, { ...serviceCapabilities, dynamicResolution: true, resizeProtocol: 'epoch-v1', monitors: [{ id: '0', width: 1920, height: 1080, primary: true, modes: [{ width: 1920, height: 1080, refreshRate: 60 }, { width: 1280, height: 720, refreshRate: 60 }] }] });
  const connection = await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice, data: { mode: 'stream', resizeProtocol: 'epoch-v1', sessionResumeProtocol: 'session-resume-v1' } });
  const client = await socket(f.wsUrl('/api/v1/sessions/' + connection.body.sessionId), 'Bearer ' + f.alice);
  await resizing.take(type('session.start'));
  resizing.json({ ...streamInfo, epoch: 0 }); await client.take(type('stream.info'));
  client.json({ type: 'stream.resize', width: 1280, height: 720, fps: 60, monitor: '0', requestId: 'r1' });
  await resizing.take(type('stream.resize'));
  client.terminate();
  assert.equal((await resizing.take(type('session.stop'), 1000)).reason, 'client_disconnected');
});

test('A client whose old socket the broker still holds replaces it on resume', async t => {
  const f = await fixture(t, { resumeGraceMs: 3000 });
  const hostWs = await host(f), { url, client } = await streaming(f, hostWs);
  const token = (await client.take(type('session.resume.token'))).token;
  const oldClosed = once(client, 'close');
  const back = await socket(url, 'Bearer ' + f.alice, { 'X-Multitap-Resume': token });
  await back.take(type('session.resumed'));
  await oldClosed;
  await hostWs.take(type('stream.keyframe'));
  await hostWs.none(type('session.stop'));
  const markers = received(back, []);
  hostWs.send(frame(true, 20));
  assert.deepEqual(await arrived(markers, 1), [20]);
});

test('A silent client replacement releases held keys exactly once before new input', async t => {
  const f = await fixture(t, { heartbeatMs: 10000 });
  const hostWs = await host(f), { id, url, client } = await streaming(f, hostWs);
  const token = (await client.take(type('session.resume.token'))).token;
  const session = f.server.relay.sessions.get(id), retired = session.ws;
  const key = (vk, down) => ({ type: 'input', event: 'key', vk, scan: 0, down, extended: false });
  const ctrl = key(17, true);
  client.json(ctrl);
  assert.deepEqual(await hostWs.take(type('input')), ctrl);

  // Neither another assigned account nor an invalid bearer can replace/reset
  // this controller or spend its resume token.
  for (const bearer of [f.bobby, randomBytes(32).toString('base64url')])
    await assert.rejects(socket(url, 'Bearer ' + bearer, { 'X-Multitap-Resume': token }), error => error.status === 401);
  client.json({ type: 'ping', id: 'auth-denied' });
  await hostWs.take(value => value.type === 'ping' && value.id === 'auth-denied');
  assert.equal(session.ws, retired);
  assert.equal(hostWs.messages.filter(type('input.reset')).length, 0);

  const oldClosed = once(client, 'close');
  const back = await socket(url, 'Bearer ' + f.alice, { 'X-Multitap-Resume': token });
  await back.take(type('session.resumed')); await oldClosed;
  await hostWs.take(type('stream.keyframe'));
  hostWs.send(frame(true, 20)); await back.take(Buffer.isBuffer);
  // Model an already-buffered message from ws after the old controller retired.
  retired.emit('message', Buffer.from(JSON.stringify(key(66, true))), false);
  back.json(key(65, true)); back.json(key(65, false));
  back.json({ type: 'ping', id: 'replacement-input' });
  await hostWs.take(value => value.type === 'ping' && value.id === 'replacement-input');
  const controls = hostWs.messages.filter(value => value.type === 'input' || value.type === 'input.reset');
  assert.deepEqual(controls, [ctrl, { type: 'input.reset', sessionId: id }, key(65, true), key(65, false)]);
  const held = new Set();
  for (const value of controls) {
    if (value.type === 'input.reset') held.clear();
    else if (value.down) held.add(value.vk); else held.delete(value.vk);
  }
  assert.equal(held.size, 0, 'Ctrl stayed pressed without a key-up from the lost client');
  assert.equal(session.ws.readyState, WebSocket.OPEN);
  assert.equal(hostWs.messages.some(type('session.stop')), false);
});

test('A failed replacement reset cannot attach a client after losing its Host', async t => {
  const f = await fixture(t, { heartbeatMs: 10000 });
  const hostWs = await host(f), { id, url, client } = await streaming(f, hostWs);
  const token = (await client.take(type('session.resume.token'))).token;
  const session = f.server.relay.sessions.get(id);
  // Exercise the production bounded-send failure deterministically, without
  // flooding the network or depending on an operating-system queue size.
  Object.defineProperty(session.host.ws, 'bufferedAmount', { get: () => 0x7fffffff });
  const back = new WebSocket(url, { headers: { Authorization: 'Bearer ' + f.alice, 'X-Multitap-Resume': token } });
  const closed = once(back, 'close');
  await once(back, 'open');
  const [code, reason] = await closed;
  assert.equal(code, 1008); assert.equal(reason.toString(), 'resume_refused');
  assert.equal(f.server.relay.sessions.has(id), false);
  assert.equal(session.ws, null, 'An orphan replacement became the current controller');
});

test('A heartbeat timeout detaches the client instead of ending the stream', async t => {
  const f = await fixture(t, { resumeGraceMs: 3000 }); // heartbeatMs 200
  const hostWs = await host(f), { url, client } = await streaming(f, hostWs, { resume: true });
  const token = (await client.take(type('session.resume.token'))).token;
  // A silent peer: answers no ping (a stalled path).
  client._socket.pause(); client.pong = () => {};
  await hostWs.take(type('input.reset'), 3000);
  await hostWs.none(type('session.stop'));
  client.terminate();
  const back = await socket(url, 'Bearer ' + f.alice, { 'X-Multitap-Resume': token });
  await back.take(type('session.resumed'));
  assert.equal(JSON.parse(audits(f, 'session_resumed')[0]).reason, 'client_timeout');
});

test('Cursor shapes sent while the client was away reach it on resume', async t => {
  const f = await fixture(t, { resumeGraceMs: 3000 });
  const hostWs = await host(f, { ...serviceCapabilities, cursorProtocol: 'cursor-v1' });
  const connection = await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice, data: { mode: 'stream', cursorProtocol: 'cursor-v1', sessionResumeProtocol: 'session-resume-v1' } });
  const url = f.wsUrl('/api/v1/sessions/' + connection.body.sessionId);
  const client = await socket(url, 'Bearer ' + f.alice);
  await hostWs.take(type('session.start'));
  hostWs.json(streamInfo); await client.take(type('stream.info'));
  hostWs.send(frame(true, 1)); await client.take(Buffer.isBuffer);
  const token = (await client.take(type('session.resume.token'))).token;
  client.terminate(); await hostWs.take(type('input.reset'));
  // A 1x1 monochrome shape (id 7), then a visible state on it.
  const shape = Buffer.alloc(12 + 16 + 8); shape[0] = 3; shape[12] = 1; shape[13] = 2; shape.writeUInt32LE(7, 16); shape.writeUInt16LE(1, 20); shape.writeUInt16LE(1, 22);
  const state = Buffer.alloc(12 + 16); state[0] = 3; state[12] = 2; state[13] = 1; state.writeUInt32LE(7, 16);
  hostWs.send(shape); hostWs.send(state);
  await delay(100);
  const back = await socket(url, 'Bearer ' + f.alice, { 'X-Multitap-Resume': token });
  await back.take(type('session.resumed'));
  const cursor = value => Buffer.isBuffer(value) && value[0] === 3;
  const records = [(await back.take(cursor))[12], (await back.take(cursor))[12]];
  assert.deepEqual(records, [1, 2], 'The shape and state of the absence were not replayed in order');
});

test('host-resume-v1: a lost Host channel keeps the session; the returning Host resumes it and the client continues', async t => {
  const f = await fixture(t, { hostResumeGraceMs: 3000 });
  const first = await host(f, resumeCapabilities), { id, client } = await streaming(f, first);
  await client.take(type('session.resume.token'));
  first.terminate();
  assert.deepEqual(await client.take(type('session.resuming')), { type: 'session.resuming', sessionId: id });
  await client.none(type('session.ended'));
  // While the Host is away the client's input has nowhere to go; it is not an error.
  client.json({ type: 'input', event: 'move', x: .5, y: .5 });
  const second = await host(f, resumeCapabilities);
  second.json({ type: 'host.session.resume', sessions: [id] });
  assert.deepEqual(await second.take(type('host.session.resumed')), { type: 'host.session.resumed', sessionId: id });
  assert.equal((await second.take(type('stream.keyframe'))).sessionId, id);
  assert.deepEqual(await client.take(type('session.resumed')), { type: 'session.resumed', sessionId: id });
  const markers = received(client, []);
  second.send(frame(false, 30)); second.send(frame(true, 31)); second.send(frame(false, 32));
  assert.deepEqual(await arrived(markers, 2), [31, 32]);
  client.json({ type: 'input', event: 'move', x: .25, y: .25 });
  assert.equal((await second.take(type('input'))).x, .25);
  const record = JSON.parse(audits(f, 'session_resumed')[0]);
  assert.equal(record.side, 'host'); assert.equal(record.reason, 'host_disconnected');
});

test('A Host that returns without the session, closes deliberately, cannot resume or misses the window ends it', async t => {
  {
    const f = await fixture(t, { hostResumeGraceMs: 3000 });
    const first = await host(f, resumeCapabilities), { id, client } = await streaming(f, first);
    first.terminate(); await client.take(type('session.resuming'));
    const second = await host(f, { ...resumeCapabilities, sessionStopProtocol: 'stop-v1' });
    second.json({ type: 'host.session.resume', sessions: [] });
    assert.equal((await client.take(type('session.ended'))).reason, 'host_disconnected');
    assert.equal(f.server.relay.sessions.has(id), false);
    // A stream kept for a session this broker no longer has is stopped on the
    // Host through stop-v1, and the next session waits for that cleanup.
    second.json({ type: 'host.session.resume', sessions: [id] });
    const stop = await second.take(type('session.stop'));
    assert.equal(stop.sessionId, id); assert.equal(stop.reason, 'resume_refused'); assert.equal(stop.sessionStopProtocol, 'stop-v1'); assert.match(stop.stopRequestId, /^[0-9a-f-]{36}$/);
    assert.equal((await f.call('/desktops', { token: f.alice })).body.desktops[0].sessionCleanup.pending, true);
    assert.equal((await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice, data: { mode: 'stream' } })).body.error.code, 'session_cleanup_pending');
  }
  {
    const f = await fixture(t, { hostResumeGraceMs: 1000 });
    const first = await host(f, resumeCapabilities), { client } = await streaming(f, first);
    first.terminate(); await client.take(type('session.resuming'));
    const second = await host(f, resumeCapabilities);
    second.json({ type: 'host.session.resume', sessions: ['00000000-0000-0000-0000-00000000000z'] });
    await once(second, 'close'); // a malformed offer is a protocol error
    assert.equal((await client.take(type('session.ended'))).reason, 'host_disconnected');
  }
  {
    const f = await fixture(t, { hostResumeGraceMs: 3000 });
    const first = await host(f, resumeCapabilities), { client } = await streaming(f, first);
    first.json({ type: 'host.closing', reason: 'service_stopping' });
    assert.deepEqual(await client.take(type('session.ended'), 1000), { type: 'session.ended', reason: 'host_disconnected', detail: 'host_service_stopping' });
  }
  {
    const f = await fixture(t, { hostResumeGraceMs: 3000 });
    const first = await host(f), { client } = await streaming(f, first); // released Host: no host-resume-v1
    first.terminate();
    assert.equal((await client.take(type('session.ended'), 1000)).reason, 'host_disconnected');
  }
  {
    const f = await fixture(t, { hostResumeGraceMs: 300 });
    const first = await host(f, resumeCapabilities), { client } = await streaming(f, first);
    first.terminate(); await client.take(type('session.resuming'));
    assert.equal((await client.take(type('session.ended'))).reason, 'host_disconnected');
    assert.equal(JSON.parse(audits(f, 'session_resume_expired')[0]).side, 'host');
  }
});

test('The client can resume while its session waits for the Host', async t => {
  const f = await fixture(t, { resumeGraceMs: 3000, hostResumeGraceMs: 3000 });
  const first = await host(f, resumeCapabilities), { id, url, client } = await streaming(f, first);
  const token = (await client.take(type('session.resume.token'))).token;
  first.terminate(); await client.take(type('session.resuming'));
  client.terminate();
  const back = await socket(url, 'Bearer ' + f.alice, { 'X-Multitap-Resume': token });
  await back.take(type('session.resumed'));
  await back.take(type('session.resuming'));
  const second = await host(f, resumeCapabilities);
  second.json({ type: 'host.session.resume', sessions: [id] });
  await second.take(type('stream.keyframe'));
  await back.take(type('session.resumed'));
});

// ------------------------------------------------------------------ #43 transport
test('Relay WebSockets refuse permessage-deflate and run without Nagle on every hop the broker owns', async t => {
  const delayed = new Set(), original = Socket.prototype.setNoDelay;
  Socket.prototype.setNoDelay = function (noDelay = true) { if (noDelay) delayed.add(this); else delayed.delete(this); return original.call(this, noDelay); };
  t.after(() => { Socket.prototype.setNoDelay = original; });
  const f = await fixture(t);
  const hostWs = await socket(f.wsUrl('/api/v1/hosts/channel'), 'Host ' + f.hostToken, {}, { perMessageDeflate: true });
  hostWs.json({ type: 'host.capabilities', capabilities: serviceCapabilities }); await hostWs.take(type('host.capabilities.ack'));
  const connection = await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice, data: { mode: 'stream' } });
  const client = await socket(f.wsUrl('/api/v1/sessions/' + connection.body.sessionId), 'Bearer ' + f.alice, {}, { perMessageDeflate: true });
  await hostWs.take(type('session.start'));
  assert.equal(hostWs.extensions, '', 'The Host channel negotiated compression');
  assert.equal(client.extensions, '', 'The client session negotiated compression');
  const session = f.server.relay.sessions.get(connection.body.sessionId), hostRecord = f.server.relay.hosts.get('pve-170');
  assert.ok(delayed.has(session.ws._socket), 'Nagle is on for the client session socket');
  assert.ok(delayed.has(hostRecord.ws._socket), 'Nagle is on for the Host channel socket');
});

test('gamepad-v1: a Host hold forgets the kept rumble, and a client that went away during it has its pads set neutral when the Host is back', async t => {
  // A rumble the game stops while the Host is away never reaches the broker: replaying the kept level to a resuming
  // client would make its pad vibrate without end. And the input.reset of a client that left during the hold could
  // not be sent then.
  const f = await fixture(t, { hostResumeGraceMs: 3000, resumeGraceMs: 3000 });
  const padCapabilities = { ...resumeCapabilities, gamepadProtocol: 'gamepad-v1' };
  const first = await host(f, padCapabilities);
  const connection = await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice, data: { mode: 'stream', sessionResumeProtocol: 'session-resume-v1', gamepadProtocol: 'gamepad-v1' } });
  assert.equal(connection.status, 200, JSON.stringify(connection.body)); assert.equal(connection.body.gamepadProtocol, 'gamepad-v1');
  const id = connection.body.sessionId, url = f.wsUrl('/api/v1/sessions/' + id);
  const client = await socket(url, 'Bearer ' + f.alice);
  await first.take(type('session.start'));
  first.json(streamInfo); await client.take(type('stream.info'));
  first.send(frame(true, 1)); await client.take(Buffer.isBuffer);
  const token = (await client.take(type('session.resume.token'))).token;
  client.json({ type: 'gamepad.state', pad: 0, buttons: 0x1000, lt: 0, rt: 255, lx: 0, ly: 0, rx: 0, ry: 0 });
  await first.take(type('gamepad.state'));
  first.json({ type: 'gamepad.rumble', sessionId: id, pad: 0, low: 255, high: 255 });
  assert.equal((await client.take(type('gamepad.rumble'))).low, 255);
  first.terminate();
  await client.take(type('session.resuming'));
  client.terminate();                               // the client goes away during the Host's hold
  await delay(100);
  const second = await host(f, padCapabilities);
  second.json({ type: 'host.session.resume', sessions: [id] });
  await second.take(type('host.session.resumed'));
  assert.deepEqual(await second.take(type('input.reset')), { type: 'input.reset', sessionId: id });
  assert.deepEqual(await second.take(type('gamepad.state')), { type: 'gamepad.state', pad: 0, buttons: 0, lt: 0, rt: 0, lx: 0, ly: 0, rx: 0, ry: 0, sessionId: id });
  const back = await socket(url, 'Bearer ' + f.alice, { 'X-Multitap-Resume': token });
  t.after(() => back.terminate());
  await back.take(type('session.resumed'));
  await back.none(type('gamepad.rumble'));
});
