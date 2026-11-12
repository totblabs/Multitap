import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { cursorState, cursorPacket, cursorSource, cursorMessagesPerSecond, cursorEchoProtocol, cursorEchoMessagesPerSecond, cursorEchoCapability, requestedCursorEcho, negotiateCursorEcho, validCursorEcho } from '../src/cursor.mjs';

// Cursor echo (cursor-echo-v1): while the client holds the mouse (게임 마우스) it
// asks the Host for the VM's real cursor position as it changes. Exact
// negotiation (only with cursor-v1, one output), the relayed request, the
// higher record bound of a granted session, and released peers unchanged.
// Real broker and WebSockets, fake Host/client.
const origin = 'https://cursor-echo-fixture.example';
const password = 'Cursor-echo-fixture-password-only!';
const machineUuid = '12345678-1234-1234-1234-123456789ac3';
const releasedHost = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true };
const cursorHost = { ...releasedHost, cursorProtocol: 'cursor-v1' };
const echoHost = { ...cursorHost, cursorEchoProtocol: 'cursor-echo-v1' };
const streamInfo = { type: 'stream.info', codec: 'h264', width: 1920, height: 1080, sourceWidth: 1920, sourceHeight: 1080, fpsTarget: 60, audio: { sampleRate: 48000, channels: 2, format: 's16le', available: true } };
const asked = { cursorProtocol: 'cursor-v1', cursorEchoProtocol: 'cursor-echo-v1' };

function header(kind, length, { epoch = 0, pts = 1000, flags = 0 } = {}) {
  const data = Buffer.alloc(12 + length);
  data[0] = kind; data[1] = flags; data.writeUInt16LE(epoch, 2); data.writeBigUInt64LE(BigInt(pts), 4);
  return data;
}
function shape({ id = 1, width = 8, height = 8 } = {}) {
  const stride = ((width + 31) >>> 5) * 4, rows = 2 * height;
  const data = header(3, 16 + stride * rows);
  data[12] = 1; data[13] = 2; data.writeUInt32LE(id, 16); data.writeUInt16LE(width, 20); data.writeUInt16LE(height, 22);
  for (let row = 0; row < rows; row++) data[28 + row * stride] = 0x80;
  return data;
}
function state({ id = 1, visible = true, x, y } = {}) {
  const data = header(3, 16);
  data[12] = 2; data[13] = (visible ? 1 : 0) | (x !== undefined ? 2 : 0);
  data.writeUInt32LE(visible ? id : 0, 16); data.writeInt32LE(x ?? 0, 20); data.writeInt32LE(y ?? 0, 24);
  return data;
}
const video = () => { const data = header(1, 5, { flags: 1 }); Buffer.from([0, 0, 1, 0x65, 1]).copy(data, 12); return data; };

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
    const enrolled = await call('/hosts/enroll', { method: 'POST', data: { code, hostname: 'Fixture VM', machineUuid, version: '0.2.141' } });
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
async function barrier(host, controller, id) { host.json({ type: 'pong', id }); await controller.take(value => value.type === 'pong' && value.id === id); }

test('Capability, request, negotiation and message rules are exact', () => {
  assert.equal(cursorEchoProtocol, 'cursor-echo-v1');
  assert.equal(cursorEchoCapability(echoHost), true);
  for (const value of [cursorHost, { ...releasedHost, cursorEchoProtocol: 'cursor-echo-v1' }, { ...echoHost, cursorEchoProtocol: 'cursor-echo-v2' }, { ...echoHost, cursorEchoProtocol: true }, undefined, null])
    assert.equal(cursorEchoCapability(value), false, JSON.stringify(value));
  assert.equal(requestedCursorEcho('stream', 'cursor-v1', undefined), false);
  assert.equal(requestedCursorEcho('stream', undefined, undefined), false);
  assert.equal(requestedCursorEcho('stream', 'cursor-v1', 'cursor-echo-v1'), true);
  for (const [mode, protocol, echo] of [['rdp', 'cursor-v1', 'cursor-echo-v1'], ['stream', undefined, 'cursor-echo-v1'], ['stream', 'cursor-v1', 'cursor-echo-v2'], ['stream', 'cursor-v1', ''], ['stream', 'cursor-v1', null], ['stream', 'cursor-v1', true]])
    assert.equal(code(() => requestedCursorEcho(mode, protocol, echo)), 'invalid_cursor_protocol', `${mode} ${protocol} ${echo}`);
  assert.equal(negotiateCursorEcho(true, true, { capabilities: echoHost }, null), true);
  assert.equal(negotiateCursorEcho(false, true, { capabilities: echoHost }, null), false, 'never granted unrequested');
  assert.equal(negotiateCursorEcho(true, false, { capabilities: echoHost }, null), false, 'never without cursor-v1 granted');
  assert.equal(negotiateCursorEcho(true, true, { capabilities: cursorHost }, null), false, 'a Host without it: not granted, not refused');
  assert.equal(negotiateCursorEcho(true, true, { capabilities: echoHost }, [{ id: '0' }]), false, 'independent outputs have no echo');
  assert.equal(validCursorEcho({ type: 'cursor.echo', on: true }), true);
  assert.equal(validCursorEcho({ type: 'cursor.echo', on: false }), true);
  for (const value of [{ type: 'cursor.echo' }, { type: 'cursor.echo', on: 1 }, { type: 'cursor.echo', on: 'true' }, { type: 'cursor.echo', on: true, rate: 2 }, { type: 'caret.watch', on: true }, null, [], 'x'])
    assert.equal(validCursorEcho(value), false, JSON.stringify(value));
});

test('A granted session raises the record bound to the echo bound; others keep 100 a second', () => {
  const ready = echo => ({ mode: 'stream', streamReady: true, resize: null, awaitingKeyFrame: false, epoch: 0, trackState: null, cursor: cursorState(), ...(echo ? { cursorEcho: true } : {}) });
  for (const echo of [false, true]) {
    const session = ready(echo); cursorSource(session, streamInfo);
    assert.equal(cursorPacket(session, shape(), 5000), 'forward');
    const limit = echo ? cursorEchoMessagesPerSecond : cursorMessagesPerSecond;
    let decision;
    for (let n = 1; n < limit; n++) { decision = cursorPacket(session, state({ x: n % 1920, y: 7 }), 5000 + n % 900); assert.equal(decision, 'forward', `${echo} ${n}`); }
    assert.equal(cursorPacket(session, state({ x: 1, y: 1 }), 5950), 'rate', `beyond ${limit}`);
    assert.equal(cursorPacket(session, state({ x: 1, y: 1 }), 6000), 'forward', 'the next second starts a new window');
  }
  assert.ok(cursorEchoMessagesPerSecond >= 2 * (50 + 667), 'twice the Host bound while echoing');
});

test('Granted: the start and response carry it, the request reaches the Host, 600 positions in a second reach the client', async t => {
  const f = await fixture(t); const host = await f.host(echoHost);
  const inventory = await f.call('/desktops', { token: f.alice });
  assert.equal(inventory.body.desktops[0].host.capabilities.cursorEchoProtocol, 'cursor-echo-v1');
  const { response, controller, start, sessionId } = await f.stream(host, asked);
  assert.equal(response.body.cursorProtocol, 'cursor-v1');
  assert.equal(response.body.cursorEchoProtocol, 'cursor-echo-v1');
  assert.equal(start.cursorProtocol, 'cursor-v1');
  assert.equal(start.cursorEchoProtocol, 'cursor-echo-v1');
  controller.json({ type: 'cursor.echo', on: true });
  assert.deepEqual(await host.take(value => value.type === 'cursor.echo'), { type: 'cursor.echo', sessionId, on: true });
  host.send(video()); await controller.take(Buffer.isBuffer);
  host.send(shape());
  for (let n = 0; n < 600; n++) host.send(state({ x: 100 + n, y: 200 }));
  await barrier(host, controller, 'echo-flood');
  const records = controller.queue.filter(Buffer.isBuffer);
  assert.equal(records.length, 601, 'the shape and every echoed position reached the client');
  assert.equal(records.at(-1).readInt32LE(20), 699);
  assert.equal(host.readyState, WebSocket.OPEN, 'a granted echo rate does not drop the Host');
  controller.json({ type: 'cursor.echo', on: false });
  assert.deepEqual(await host.take(value => value.type === 'cursor.echo'), { type: 'cursor.echo', sessionId, on: false });
  // A malformed request ends the session.
  controller.json({ type: 'cursor.echo', on: 'yes' });
  assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_input');
  await closed(controller);
});

test('A Host without it, or a client that did not ask: no grant, no marker, the request ends the session, the old bound drops a flood', async t => {
  const f = await fixture(t); const old = await f.host(cursorHost);
  const inventory = await f.call('/desktops', { token: f.alice });
  assert.equal(inventory.body.desktops[0].host.capabilities.cursorEchoProtocol, undefined);
  const { response, controller, start } = await f.stream(old, asked);
  assert.equal(response.body.cursorProtocol, 'cursor-v1', 'cursor-v1 itself is unchanged');
  assert.equal(response.body.cursorEchoProtocol, undefined, 'not refused, only ungranted');
  assert.equal('cursorEchoProtocol' in start, false);
  controller.json({ type: 'cursor.echo', on: true });
  assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_input');
  await closed(controller);

  const g = await fixture(t); const capable = await g.host(echoHost);
  const plain = await g.stream(capable, { cursorProtocol: 'cursor-v1' });
  assert.equal(plain.response.body.cursorEchoProtocol, undefined, 'never granted unrequested');
  assert.equal('cursorEchoProtocol' in plain.start, false);
  capable.send(video()); await plain.controller.take(Buffer.isBuffer);
  capable.send(shape());
  for (let n = 0; n < cursorMessagesPerSecond + 5; n++) capable.send(state({ x: n, y: 1 }));
  await closed(capable);
  assert.equal((await plain.controller.take(value => value.type === 'session.ended')).reason, 'host_rate_limit');
});

test('A malformed connect field is refused before any session', async t => {
  const f = await f_host(t);
  for (const cursorEchoProtocol of ['cursor-echo-v2', 1, null, '']) {
    const response = await f.connect({ cursorProtocol: 'cursor-v1', cursorEchoProtocol });
    assert.equal(response.status, 400, String(cursorEchoProtocol)); assert.equal(response.body.error.code, 'invalid_cursor_protocol');
  }
  const alone = await f.connect({ cursorEchoProtocol: 'cursor-echo-v1' });
  assert.equal(alone.status, 400, 'the echo needs cursor-v1 in the same request'); assert.equal(alone.body.error.code, 'invalid_cursor_protocol');
  const plain = await f.connect({});
  assert.equal(plain.status, 200, 'a released client request is unchanged'); assert.equal(plain.body.cursorEchoProtocol, undefined);
});
async function f_host(t) { const f = await fixture(t); await f.host(echoHost); return f; }
