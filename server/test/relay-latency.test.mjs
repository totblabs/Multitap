import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { Socket } from 'node:net';
import { WebSocket } from 'ws';
import { Relay } from '../src/relay.mjs';
import { Store } from '../src/store.mjs';
import { newToken, tokenHash } from '../src/security.mjs';

const info = { type: 'stream.info', codec: 'h264', width: 1280, height: 720, sourceWidth: 1280, sourceHeight: 720, fpsTarget: 60, audio: { sampleRate: 48000, channels: 2, format: 's16le', available: false } };
const frame = sequence => { const bytes = Buffer.alloc(64); bytes[0] = 1; bytes[1] = 1; bytes.writeUInt32LE(sequence, 12); return bytes; };

async function socket(url, authorization) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url, { headers: { Authorization: authorization }, perMessageDeflate: false }), queue = [], waiters = [];
    ws.on('message', (bytes, binary) => {
      const value = binary ? bytes : JSON.parse(bytes.toString());
      const index = waiters.findIndex(w => w.test(value));
      if (index < 0) queue.push(value); else { const w = waiters.splice(index, 1)[0]; clearTimeout(w.timer); w.resolve(value); }
    });
    ws.on('error', () => {}); ws.once('error', reject);
    ws.once('unexpected-response', (_req, response) => { response.resume(); reject(Object.assign(new Error('upgrade_rejected'), { status: response.statusCode })); });
    ws.json = value => ws.send(JSON.stringify(value)); ws.queue = queue;
    ws.take = (check = () => true) => {
      const index = queue.findIndex(check); if (index >= 0) return Promise.resolve(queue.splice(index, 1)[0]);
      return new Promise((resolve, reject) => { const w = { test: check, resolve, timer: setTimeout(() => reject(new Error('latency_fixture_timeout')), 2000) }; waiters.push(w); });
    };
    ws.once('open', () => resolve(ws));
  });
}

async function fixture(t) {
  const store = new Store(':memory:');
  const user = store.addUser({ name: 'alice', passwordHash: 'fixture-only' }), token = newToken();
  store.createSession(store.findUser('alice'), tokenHash(token), Date.now(), 3600000);
  store.syncProxmox([{ vmid: 820, node: 'fixture', name: 'Latency 820', status: 'running' }], 'fixture');
  store.assign('alice', 'pve-820');
  const code = newToken(), hostToken = newToken(), uuid = '12345678-1234-1234-1234-000000000820';
  store.issueEnrollment('pve-820', uuid, tokenHash(code), 'fixture');
  store.consumeEnrollment(tokenHash(code), uuid, { hostname: 'Latency host', version: '0.2.3', tokenHash: tokenHash(hostToken) });
  // Long sweeps keep periodic work out of the per-packet assertions below.
  const relay = new Relay({ store, publicOrigin: 'https://latency-fixture.example', authorizationMs: 60000, heartbeatMs: 60000 });
  const server = createServer(); relay.attach(server);
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const port = server.address().port, base = 'ws://127.0.0.1:' + port;
  relay.publicOrigin = 'https://127.0.0.1:' + port;
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  const host = () => socket(base + '/api/v1/hosts/channel', 'Host ' + hostToken);
  async function session(hostSocket) {
    const connection = relay.connect({ id: user }, tokenHash(token), 'pve-820', 'stream');
    const client = await socket(base + new URL(connection.webSocketUrl).pathname, 'Bearer ' + token);
    const start = await hostSocket.take(v => v.type === 'session.start');
    assert.equal(start.sessionId, connection.sessionId);
    hostSocket.json(info); await client.take(v => v.type === 'stream.info');
    return { connection, client };
  }
  return { store, relay, port, host, session, token, hostToken };
}

test('Relay host and controller upgrades disable Nagle on the server-side sockets', async t => {
  const calls = [], original = Socket.prototype.setNoDelay;
  Socket.prototype.setNoDelay = function (...args) { calls.push({ port: this.localPort, enable: args.length === 0 ? true : args[0] }); return original.apply(this, args); };
  try {
    const f = await fixture(t), host = await f.host();
    const { client } = await f.session(host);
    const serverSide = calls.filter(call => call.port === f.port);
    assert.ok(serverSide.length >= 2, 'both relay-side sockets (host channel and controller) must set TCP_NODELAY');
    assert.ok(serverSide.every(call => call.enable !== false), 'no relay-side socket may re-enable Nagle');
    client.close(); host.close();
  } finally { Socket.prototype.setNoDelay = original; }
});

test('Host packets reuse the single attached controller without scanning sessions and follow a replacement controller', async t => {
  const f = await fixture(t), host = await f.host();
  const first = await f.session(host);
  let scans = 0; const values = f.relay.sessions.values;
  f.relay.sessions.values = function () { scans++; return values.call(this); };
  for (let n = 0; n < 40; n++) host.send(frame(n), { binary: true });
  for (let n = 0; n < 40; n++) assert.equal((await first.client.take(v => Buffer.isBuffer(v))).readUInt32LE(12), n, 'media order is preserved');
  host.json({ type: 'pong', id: 'p1' }); assert.equal((await first.client.take(v => v.type === 'pong')).id, 'p1');
  assert.ok(scans <= 1, `per-packet host lookup must not rescan sessions (scans=${scans})`);
  f.relay.sessions.values = values;
  // A retired controller is never selected again; its successor is.
  first.client.json({ type: 'session.stop' });
  assert.equal((await host.take(v => v.type === 'session.stop')).sessionId, first.connection.sessionId);
  host.send(frame(99), { binary: true });
  const second = await f.session(host);
  host.send(frame(100), { binary: true });
  assert.equal((await second.client.take(v => Buffer.isBuffer(v))).readUInt32LE(12), 100);
  assert.equal(first.client.queue.some(v => Buffer.isBuffer(v) && v.readUInt32LE(12) >= 99), false);
  // Client input still reaches the host after the controller change.
  second.client.json({ type: 'input', event: 'move', x: 0.25, y: 0.75 });
  assert.deepEqual(await host.take(v => v.type === 'input'), { type: 'input', event: 'move', x: 0.25, y: 0.75 });
  second.client.close(); host.close();
});

test('Relay send serializes a control value once and keeps the exact UTF-8 queue bound', () => {
  const relay = Object.create(Relay.prototype);
  let serialized = 0; const value = { toJSON() { serialized++; return { type: 'session.ended', reason: '연결 종료' }; } };
  const sent = [], ws = { readyState: WebSocket.OPEN, bufferedAmount: 0, terminated: false, send(payload, options) { sent.push([payload, options.binary]); }, terminate() { this.terminated = true; } };
  assert.equal(relay.send(ws, value), true);
  assert.equal(serialized, 1);
  assert.deepEqual(sent, [[JSON.stringify({ type: 'session.ended', reason: '연결 종료' }), false]]);
  const bytes = Buffer.byteLength(sent[0][0]);
  ws.bufferedAmount = 8 * 1024 * 1024 - bytes; assert.equal(relay.send(ws, value), true); assert.equal(ws.terminated, false);
  ws.bufferedAmount = 8 * 1024 * 1024 - bytes + 1; assert.equal(relay.send(ws, value), false); assert.equal(ws.terminated, true);
  assert.equal(sent.length, 2);
});

test('Reused authorization statements observe every live revocation', t => {
  const store = new Store(':memory:'); t.after(() => store.close());
  store.addUser({ name: 'alice', passwordHash: 'fixture-only' });
  const hash = tokenHash(newToken()), alice = store.findUser('alice');
  store.createSession(alice, hash, Date.now(), 3600000);
  store.syncProxmox([{ vmid: 821, node: 'fixture', name: 'Latency 821', status: 'running' }], 'fixture');
  store.assign('alice', 'pve-821');
  const code = newToken(), hostHash = tokenHash(newToken()), uuid = '12345678-1234-1234-1234-000000000821';
  store.issueEnrollment('pve-821', uuid, tokenHash(code), 'fixture');
  store.consumeEnrollment(tokenHash(code), uuid, { hostname: 'Latency host', version: '0.2.3', tokenHash: hostHash });
  let prepared = 0; const prepare = store.db.prepare;
  store.db.prepare = function (...args) { prepared++; return prepare.apply(this, args); };
  for (let n = 0; n < 50; n++) { assert.equal(store.authenticate(hash)?.id, alice.id); assert.equal(store.assignedDesktop(alice.id, 'pve-821')?.id, 'pve-821'); assert.ok(store.authenticateHost(hostHash)); store.clipboardPolicy(alice.id, 'pve-821'); store.devicePolicy(alice.id, 'pve-821'); }
  assert.ok(prepared <= 5, `per-message authorization must not recompile its SQL (prepared=${prepared})`);
  store.db.prepare = prepare;
  store.revoke('alice', 'pve-821'); assert.equal(store.assignedDesktop(alice.id, 'pve-821'), undefined);
  store.assign('alice', 'pve-821'); assert.equal(store.assignedDesktop(alice.id, 'pve-821')?.id, 'pve-821');
  store.setUserEnabled('alice', false); assert.equal(store.authenticate(hash), undefined); assert.equal(store.assignedDesktop(alice.id, 'pve-821'), undefined);
  assert.equal(store.authenticate(hash, Date.now() + 7200000), undefined);
  store.db.prepare('UPDATE hosts SET revoked=1').run(); assert.equal(store.authenticateHost(hostHash), undefined);
});
