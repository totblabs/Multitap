import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { setTimeout as delay } from 'node:timers/promises';
import { createServer as createHttpsServer } from 'node:https';
import { createServer as createTcpServer, connect as connectTcp } from 'node:net';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, dirname, resolve, basename } from 'node:path';
import { spawnSync } from 'node:child_process';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword, newToken, tokenHash } from '../src/security.mjs';
import { Relay } from '../src/relay.mjs';

const origin = 'https://relay-fixture.example';
const password = 'Relay-fixture-password-only!';
const machineUuid = '12345678-1234-1234-1234-123456789abc';
const streamInfo = { type: 'stream.info', codec: 'h264', width: 1280, height: 720, sourceWidth: 1920, sourceHeight: 1080, fpsTarget: 60, audio: { sampleRate: 48000, channels: 2, format: 's16le', available: false } };
const streamTelemetry = (sessionId, epoch = 0) => ({
  type: 'stream.stats', sessionId, epoch, capturedFps: 58.2, encodedFps: 58, bitrateKbps: 8560.4, measuredMs: 1012,
  pipeline: { sampleCount: 60, topologyAvgMs: .2, captureAvgMs: 3.5, convertAvgMs: 1.1, encodeAvgMs: 4.4, workAvgMs: 9.2, workP95Ms: 11.5, workMaxMs: 16.1 },
});
async function fixture(t, { aliceAdmin = false, relayOptions = {}, transportOrigin = origin, allowInsecureHttp = false } = {}) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const hash = await hashPassword(password);
  store.addUser({ name: 'admin', passwordHash: hash, role: 'admin' });
  store.addUser({ name: 'alice', passwordHash: hash, role: aliceAdmin ? 'admin' : 'user' });
  store.addUser({ name: 'bobby', passwordHash: hash });
  store.syncProxmox([170, 149].map(vmid => ({ vmid, node: 'pve', name: `VM ${vmid}`, status: 'running' })), 'fixture');
  store.assign('alice', 'pve-170'); store.assign('bobby', 'pve-149');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve-fixture.example', tokenId: 'fixture@pve!broker', secret: 'only-fixture-secret' }), 'fixture');
  const proxmox = { machineIdentity: async (_config, vmid) => { assert.equal(vmid, 170); return { machineUuid }; } };
  const server = await buildServer({ store, vault, proxmox, publicOrigin: transportOrigin, allowInsecureHttp, relayOptions: { authorizationMs: 20, heartbeatMs: 200, ...relayOptions }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const base = `http://127.0.0.1:${server.address().port}`;
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  async function call(path, { method = 'GET', data, token, cookie, csrf } = {}) {
    const response = await fetch(base + '/api/v1' + path, { method,
      headers: { ...(token ? { Authorization: 'Bearer ' + token } : {}), ...(cookie ? { Cookie: cookie } : {}), ...(method !== 'GET' ? { 'Content-Type': 'application/json', Origin: transportOrigin, ...(csrf ? { 'X-CSRF-Token': csrf } : {}) } : {}) },
      body: data === undefined ? undefined : JSON.stringify(data) });
    return { status: response.status, body: await response.json(), headers: response.headers };
  }
  const adminLogin = await call('/admin/login', { method: 'POST', data: { username: 'admin', password } });
  const admin = { cookie: adminLogin.headers.get('set-cookie').split(';')[0], csrf: adminLogin.body.csrfToken };
  const alice = (await call('/auth/login', { method: 'POST', data: { username: 'alice', password } })).body.sessionToken;
  const bobby = (await call('/auth/login', { method: 'POST', data: { username: 'bobby', password } })).body.sessionToken;
  const code = (await call('/admin/desktops/pve-170/enrollment', { ...admin, method: 'POST', data: {} })).body.code;
  async function enroll(uuid = machineUuid) { return call('/hosts/enroll', { method: 'POST', data: { code, hostname: 'Fixture VM', machineUuid: uuid, version: '0.1.0' } }); }
  function wsUrl(path) { return base.replace('http:', 'ws:') + path; }
  return { store, call, admin, alice, bobby, code, enroll, wsUrl };
}
async function socket(url, authorization, options = {}) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url, { headers: { Authorization: authorization }, perMessageDeflate: false, ...options });
    ws.once('error', reject);
    ws.once('unexpected-response', (_req, response) => { response.resume(); reject(Object.assign(new Error('upgrade_rejected'), { status: response.statusCode })); });
    ws.once('open', () => {
      const queue = [], waiters = [];
      ws.on('message', (data, binary) => {
        const value = binary ? data : JSON.parse(data.toString('utf8'));
        const index = waiters.findIndex(waiter => waiter.predicate(value));
        if (index >= 0) { const waiter = waiters.splice(index, 1)[0]; clearTimeout(waiter.timer); waiter.resolve(value); }
        else queue.push(value);
      });
      ws.take = (predicate = () => true) => {
        const index = queue.findIndex(predicate);
        if (index >= 0) return Promise.resolve(queue.splice(index, 1)[0]);
        return new Promise((resolve, reject) => {
          const waiter = { predicate, resolve, timer: setTimeout(() => reject(new Error('message_timeout')), 2000) }; waiters.push(waiter);
        });
      };
      ws.json = value => ws.send(JSON.stringify(value));
      resolve(ws);
    });
  });
}
async function connected(f) {
  const enrolled = await f.enroll(); assert.equal(enrolled.status, 201);
  const host = await socket(f.wsUrl('/api/v1/hosts/channel'), 'Host ' + enrolled.body.hostToken);
  const connection = await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice });
  assert.equal(connection.status, 200);
  const client = await socket(f.wsUrl('/api/v1/sessions/' + connection.body.sessionId), 'Bearer ' + f.alice);
  assert.equal((await host.take(value => value.type === 'session.start')).sessionId, connection.body.sessionId);
  return { host, client, enrolled, session: connection.body };
}

test('Single-use enrollment checks Proxmox VM UUID and stores only token/code hashes', async t => {
  const f = await fixture(t);
  assert.equal((await f.enroll('99999999-9999-9999-9999-999999999999')).status, 403);
  const result = await f.enroll(); assert.equal(result.status, 201);
  assert.equal((await f.enroll()).status, 403);
  const host = f.store.db.prepare('SELECT * FROM hosts').get();
  assert.notEqual(host.token_hash, result.body.hostToken); assert.equal(JSON.stringify(host).includes(result.body.hostToken), false);
  assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM host_enrollments').get().n, 0);
  await assert.rejects(socket(f.wsUrl('/api/v1/hosts/channel'), 'Host wrong-token'), error => error.status === 401);
  assert.equal((await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice })).status, 409);
});

test('Authenticated host heartbeat updates self-reported version without rotating credentials or exposing secrets', async t => {
  const f = await fixture(t), enrolled = await f.enroll();
  const initial = f.store.db.prepare('SELECT id,token_hash,version FROM hosts WHERE desktop_id=?').get('pve-170');
  assert.equal(initial.version, '0.1.0');
  const host = await socket(f.wsUrl('/api/v1/hosts/channel'), 'Host ' + enrolled.body.hostToken);
  host.json({ type: 'host.heartbeat', version: '0.1.2' });
  assert.deepEqual(await host.take(value => value.type === 'host.heartbeat.ack'), { type: 'host.heartbeat.ack' });
  assert.equal((await f.call('/desktops', { token: f.alice })).body.desktops[0].host.version, '0.1.2');
  for (const version of [null, 123, { version: '0.1.3' }, '0.1.2+secret', '0.1.2\n', '1'.repeat(33) + '.2.3', enrolled.body.hostToken]) {
    host.json({ type: 'host.heartbeat', version });
    await host.take(value => value.type === 'host.heartbeat.ack');
  }
  // The 0.1.1 peer's older heartbeat shape remains valid and keeps the latest
  // known display metadata instead of resetting it to the enrollment version.
  host.json({ type: 'host.heartbeat' }); await host.take(value => value.type === 'host.heartbeat.ack');
  const current = f.store.db.prepare('SELECT id,token_hash,version FROM hosts WHERE desktop_id=?').get('pve-170');
  assert.equal(current.id, initial.id); assert.equal(current.token_hash, initial.token_hash); assert.equal(current.version, '0.1.2');
  const inventory = (await f.call('/admin/inventory', f.admin)).body;
  assert.equal(inventory.desktops.find(d => d.id === 'pve-170').host.version, '0.1.2');
  const serialized = JSON.stringify(inventory);
  assert.equal(serialized.includes(enrolled.body.hostToken), false);
  assert.equal(serialized.includes(initial.token_hash), false);
  // Enrollment now preserves its authoritative VM UUID in administrator
  // inventory. Host credentials/claimed identity stay out of public metadata
  // and the assigned-desktop response still exposes no machine UUID.
  const desktop = inventory.desktops.find(d => d.id === 'pve-170');
  assert.equal(desktop.pve_machine_uuid, machineUuid);
  assert.equal(JSON.stringify(desktop.host).includes(machineUuid), false);
  assert.equal(JSON.stringify((await f.call('/desktops', { token: f.alice })).body).includes(machineUuid), false);
  const closed = once(host, 'close'); host.close(); await closed;
  const reconnected = await socket(f.wsUrl('/api/v1/hosts/channel'), 'Host ' + enrolled.body.hostToken);
  reconnected.json({ type: 'host.heartbeat' }); await reconnected.take(value => value.type === 'host.heartbeat.ack');
  assert.equal((await f.call('/desktops', { token: f.alice })).body.desktops[0].host.version, '0.1.2');
});

test('Actual WebSocket relay isolates VMs and credentials, forwards binary/input/RTT and resets on authorization revocation', async t => {
  const f = await fixture(t), { host, client, enrolled, session } = await connected(f);
  assert.equal((await f.call('/desktops', { token: f.alice })).body.desktops[0].canConnect, true);
  assert.equal((await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.bobby })).status, 404);
  assert.equal((await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice })).status, 409);
  await assert.rejects(socket(f.wsUrl('/api/v1/sessions/' + session.sessionId), 'Bearer ' + f.bobby), error => error.status === 401);
  await assert.rejects(socket(f.wsUrl('/api/v1/hosts/channel'), 'Host ' + enrolled.body.hostToken), error => error.status === 409);
  await assert.rejects(socket(f.wsUrl('/api/v1/hosts/channel?token=' + enrolled.body.hostToken), 'Host ' + enrolled.body.hostToken), error => error.status === 403);
  host.json(streamInfo); assert.deepEqual(await client.take(value => value.type === 'stream.info'), streamInfo);
  const video = Buffer.alloc(18); video[0] = 1; video[1] = 1; video.writeBigUInt64LE(12345n, 4); video.set([0, 0, 0, 1, 0x65, 1], 12);
  host.send(video); assert.deepEqual(await client.take(Buffer.isBuffer), video);
  client.json({ type: 'input', event: 'key', vk: 91, scan: 0, down: true, extended: true });
  assert.equal((await host.take(value => value.type === 'input')).vk, 91);
  client.json({ type: 'ping', id: 'full-trip' }); const ping = await host.take(value => value.type === 'ping'); host.json({ type: 'pong', id: ping.id });
  assert.equal((await client.take(value => value.type === 'pong')).id, 'full-trip');
  const closed = once(client, 'close');
  f.store.revoke('alice', 'pve-170');
  assert.equal((await host.take(value => value.type === 'input.reset')).type, 'input.reset');
  assert.equal((await host.take(value => value.type === 'session.stop')).reason, 'authorization_revoked');
  await closed;
});

test('Oversized media closes the host and its client instead of retaining an unbounded frame', async t => {
  const f = await fixture(t), { host, client } = await connected(f);
  host.json(streamInfo); await client.take(value => value.type === 'stream.info');
  const hostClosed = once(host, 'close'), clientClosed = once(client, 'close');
  host.send(Buffer.alloc(4 * 1024 * 1024 + 1));
  await Promise.all([hostClosed, clientClosed]);
  assert.equal((await f.call('/desktops', { token: f.alice })).body.desktops[0].canConnect, false);
});

test('Late frames after disconnect and before a fresh stream header cannot disconnect the host or enter the next decoder', async t => {
  const f = await fixture(t), { host, client } = await connected(f);
  host.json(streamInfo); await client.take(value => value.type === 'stream.info');
  const late = Buffer.alloc(18); late[0] = 1; late[1] = 1; late.writeBigUInt64LE(123n, 4); late.set([0,0,0,1,0x65,1], 12);
  const oldClosed = once(client, 'close'); client.terminate(); await oldClosed;
  await host.take(value => value.type === 'input.reset'); await host.take(value => value.type === 'session.stop');
  host.send(late); host.json({ type: 'host.heartbeat' });
  assert.equal((await host.take(value => value.type === 'host.heartbeat.ack')).type, 'host.heartbeat.ack');
  const next = await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice });
  assert.equal(next.status, 200);
  const nextClient = await socket(f.wsUrl('/api/v1/sessions/' + next.body.sessionId), 'Bearer ' + f.alice);
  await host.take(value => value.type === 'session.start');
  const received = []; nextClient.on('message', (data, binary) => received.push(binary ? data : JSON.parse(data.toString())));
  host.send(late); host.json(streamInfo);
  assert.equal((await nextClient.take(value => value.type === 'stream.info')).type, 'stream.info');
  const fresh = Buffer.from(late); fresh.writeBigUInt64LE(1n, 4); host.send(fresh);
  assert.deepEqual(await nextClient.take(Buffer.isBuffer), fresh);
  assert.equal(received[0].type, 'stream.info');
  assert.equal(received.filter(Buffer.isBuffer).length, 1);
  assert.equal((await f.call('/desktops', { token: f.alice })).body.desktops[0].canConnect, true);
});

test('A stalled actual client socket triggers bounded backpressure cleanup while the host stays reusable', async t => {
  const f = await fixture(t), { host, client } = await connected(f);
  host.json(streamInfo); await client.take(value => value.type === 'stream.info');
  const frame = Buffer.alloc(4 * 1024 * 1024); frame[0] = 1; frame[1] = 1;
  client._socket.pause();
  try {
    for (let i = 0; i < 6; i++) {
      await new Promise((resolve, reject) => host.send(frame, error => error ? reject(error) : resolve()));
    }
    assert.equal((await host.take(value => value.type === 'session.stop')).reason, 'client_backpressure');
    host.json({ type: 'host.heartbeat' });
    assert.equal((await host.take(value => value.type === 'host.heartbeat.ack')).type, 'host.heartbeat.ack');
  } finally { client._socket.resume(); client.terminate(); }
});

test('A host transport restart closes the old controller and permits a fresh channel with the same protected identity', async t => {
  const f = await fixture(t), { host, client, enrolled, session } = await connected(f);
  const controllerClosed = once(client, 'close'); host.terminate(); await controllerClosed;
  assert.equal((await f.call('/desktops', { token: f.alice })).body.desktops[0].canConnect, false);
  await assert.rejects(socket(f.wsUrl('/api/v1/sessions/' + session.sessionId), 'Bearer ' + f.alice), error => error.status === 401);
  const restarted = await socket(f.wsUrl('/api/v1/hosts/channel'), 'Host ' + enrolled.body.hostToken);
  const fresh = await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice });
  assert.equal(fresh.status, 200); assert.notEqual(fresh.body.sessionId, session.sessionId);
  const freshClient = await socket(f.wsUrl('/api/v1/sessions/' + fresh.body.sessionId), 'Bearer ' + f.alice);
  assert.equal((await restarted.take(value => value.type === 'session.start')).sessionId, fresh.body.sessionId);
  restarted.json(streamInfo); assert.equal((await freshClient.take(value => value.type === 'stream.info')).codec, 'h264');
});

test('An isolated stopped controller cannot forward already queued keys or contaminate the next controller', async t => {
  const f = await fixture(t), { host, client } = await connected(f);
  const forwardedKeys = [];
  host.on('message', (data, binary) => { if (!binary) { const value = JSON.parse(data.toString()); if (value.type === 'input' && value.event === 'key') forwardedKeys.push(value); } });
  const stopped = once(client, 'close');
  // Coalesce a stop and malicious trailing keys in one TCP write. This is an
  // in-memory fake host: no Windows or production VM input is ever delivered.
  client._socket.cork();
  client.json({ type: 'session.stop' });
  client.json({ type: 'input', event: 'key', vk: 65, scan: 0, down: true, extended: false });
  client.json({ type: 'input', event: 'key', vk: 65, scan: 0, down: false, extended: false });
  client._socket.uncork();
  await host.take(value => value.type === 'input.reset');
  await host.take(value => value.type === 'session.stop');
  await stopped;
  await new Promise(resolve => setTimeout(resolve, 50));
  assert.deepEqual(forwardedKeys, []);
  const next = await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice });
  assert.equal(next.status, 200);
  const nextClient = await socket(f.wsUrl('/api/v1/sessions/' + next.body.sessionId), 'Bearer ' + f.alice);
  assert.equal((await host.take(value => value.type === 'session.start')).sessionId, next.body.sessionId);
  nextClient.json({ type: 'input.reset' });
  await host.take(value => value.type === 'input.reset');
  assert.deepEqual(forwardedKeys, []);
});

test('An active expired client token resets the fake host and closes its controller on periodic authorization checks', async t => {
  const f = await fixture(t), { host, client } = await connected(f);
  const closed = once(client, 'close');
  f.store.db.prepare("UPDATE sessions SET expires_at=? WHERE token_hash=? AND purpose='client'").run(Date.now() - 1, tokenHash(f.alice));
  await host.take(value => value.type === 'input.reset');
  assert.equal((await host.take(value => value.type === 'session.stop')).reason, 'authorization_revoked');
  await closed;
  assert.equal((await f.call('/auth/me', { token: f.alice })).status, 401);
});

test('Heartbeat failure and excessive control-message rate reset isolated controllers without physical input', async t => {
  const f = await fixture(t), enrolled = await f.enroll();
  const host = await socket(f.wsUrl('/api/v1/hosts/channel'), 'Host ' + enrolled.body.hostToken);
  const first = await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice });
  const silent = await socket(f.wsUrl('/api/v1/sessions/' + first.body.sessionId), 'Bearer ' + f.alice, { autoPong: false });
  await host.take(value => value.type === 'session.start');
  const silentClosed = once(silent, 'close');
  await host.take(value => value.type === 'input.reset');
  assert.equal((await host.take(value => value.type === 'session.stop')).reason, 'client_timeout');
  await silentClosed;
  const second = await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice });
  const noisy = await socket(f.wsUrl('/api/v1/sessions/' + second.body.sessionId), 'Bearer ' + f.alice);
  await host.take(value => value.type === 'session.start');
  const noisyClosed = once(noisy, 'close');
  noisy._socket.cork();
  for (let i = 0; i < 2001; i++) noisy.json({ type: 'ping', id: String(i) });
  noisy._socket.uncork();
  await host.take(value => value.type === 'input.reset');
  assert.equal((await host.take(value => value.type === 'session.stop')).reason, 'invalid_input');
  await noisyClosed;
  host.json({ type: 'host.heartbeat' });
  await host.take(value => value.type === 'host.heartbeat.ack');
});

test('Client password change validates current/confirmed values, preserves account ID and immediately closes all old sessions', async t => {
  const f = await fixture(t, { aliceAdmin: true }), { host, client } = await connected(f);
  const original = f.store.findUser('alice');
  const secondClientToken = (await f.call('/auth/login', { method: 'POST', data: { username: 'alice', password } })).body.sessionToken;
  const aliceAdmin = await f.call('/admin/login', { method: 'POST', data: { username: 'alice', password } });
  assert.equal(aliceAdmin.status, 200);
  const aliceCookie = aliceAdmin.headers.get('set-cookie').split(';')[0];
  assert.equal((await f.call('/auth/password', { method: 'POST', token: f.alice, data: { currentPassword: password, newPassword: 'New-relay-fixture-password!', passwordConfirmation: 'different' } })).status, 400);
  assert.equal((await f.call('/auth/password', { method: 'POST', token: f.alice, data: { currentPassword: 'incorrect', newPassword: 'New-relay-fixture-password!', passwordConfirmation: 'New-relay-fixture-password!' } })).status, 403);
  assert.equal(f.store.findUser('alice').password_hash, original.password_hash);
  const closed = once(client, 'close');
  const changed = await f.call('/auth/password', { method: 'POST', token: f.alice, data: { currentPassword: password, newPassword: 'New-relay-fixture-password!', passwordConfirmation: 'New-relay-fixture-password!' } });
  assert.equal(changed.status, 200); assert.equal(changed.body.requiresLogin, true);
  assert.equal((await host.take(value => value.type === 'input.reset')).type, 'input.reset'); await closed;
  assert.equal(f.store.findUser('alice').id, original.id);
  for (const token of [f.alice, secondClientToken]) assert.equal((await f.call('/auth/me', { token })).status, 401);
  assert.equal((await f.call('/admin/session', { cookie: aliceCookie })).status, 401);
  assert.equal((await f.call('/admin/session', f.admin)).status, 200);
  assert.equal((await f.call('/auth/login', { method: 'POST', data: { username: 'alice', password } })).status, 401);
  assert.equal((await f.call('/auth/login', { method: 'POST', data: { username: 'alice', password: 'New-relay-fixture-password!' } })).status, 200);
});

test('Relay performs actual verified WSS transport through an isolated HTTPS endpoint', async t => {
  if (spawnSync('openssl', ['version']).error?.code === 'ENOENT') { t.skip('OpenSSL unavailable; verified WSS runs in Linux QA.'); return; }
  const directory = await mkdtemp(join(tmpdir(), 'multitap-wss-fixture-'));
  t.after(async () => { assert.equal(dirname(resolve(directory)), resolve(tmpdir())); assert.ok(basename(directory).startsWith('multitap-wss-fixture-')); await rm(directory, { recursive: true }); });
  const generated = spawnSync('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', join(directory, 'key.pem'), '-out', join(directory, 'cert.pem'), '-days', '1', '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1'], { encoding: 'utf8' });
  assert.equal(generated.status, 0);
  const cert = await readFile(join(directory, 'cert.pem')), key = await readFile(join(directory, 'key.pem'));
  const store = new Store(':memory:');
  const id = store.addUser({ name: 'tls-client', passwordHash: await hashPassword(password) });
  store.syncProxmox([{ vmid: 170, name: 'TLS fixture VM', node: 'pve', status: 'running' }], 'fixture');
  store.assign('tls-client', 'pve-170');
  const hostToken = newToken(), clientToken = newToken(), code = newToken();
  store.issueEnrollment('pve-170', machineUuid, tokenHash(code), 'fixture');
  store.consumeEnrollment(tokenHash(code), machineUuid, { hostname: 'TLS fixture VM', version: '0.1.0', tokenHash: tokenHash(hostToken) });
  store.createSession(store.findUser('tls-client'), tokenHash(clientToken), Date.now(), 60000);
  const relay = new Relay({ store, publicOrigin: 'https://localhost', authorizationMs: 20 });
  const server = createHttpsServer({ cert, key }); relay.attach(server);
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  relay.publicOrigin = `https://127.0.0.1:${server.address().port}`;
  const hostUrl = relay.publicOrigin.replace('https:', 'wss:') + '/api/v1/hosts/channel';
  await assert.rejects(socket(hostUrl, 'Host ' + hostToken, { rejectUnauthorized: true }));
  const host = await socket(hostUrl, 'Host ' + hostToken, { ca: cert, rejectUnauthorized: true });
  const session = relay.connect({ id }, tokenHash(clientToken), 'pve-170');
  const client = await socket(session.webSocketUrl, 'Bearer ' + clientToken, { ca: cert, rejectUnauthorized: true });
  await host.take(value => value.type === 'session.start');
  const appliedInfo = { ...streamInfo, bitrate: 12000000, captureBackend: 'wgc-warp' };
  host.json(appliedInfo); assert.deepEqual(await client.take(value => value.type === 'stream.info'), appliedInfo);
  const measured = streamTelemetry(session.sessionId);
  host.json(measured); assert.deepEqual(await client.take(value => value.type === 'stream.stats'), measured);
  client.json({ type: 'ping', id: 'verified-wss' });
  const ping = await host.take(value => value.type === 'ping'); host.json({ type: 'pong', id: ping.id });
  assert.equal((await client.take(value => value.type === 'pong')).id, 'verified-wss');
  client.json({ type: 'session.stop' }); await host.take(value => value.type === 'session.stop');
  host.json({ type: 'host.capabilities', capabilities: serviceCapabilities });
  await host.take(value => value.type === 'host.capabilities.ack');
  host.json({ type: 'host.capabilities', capabilities: resizeCapabilities });await host.take(value => value.type === 'host.capabilities.ack');
  const epochSession = relay.connect({ id }, tokenHash(clientToken), 'pve-170', 'stream', undefined, 'epoch-v1');
  const epochClient = await socket(epochSession.webSocketUrl, 'Bearer ' + clientToken, { ca: cert, rejectUnauthorized: true });
  await host.take(value => value.type === 'session.start');host.json({ ...streamInfo, epoch: 0 });await epochClient.take(value => value.type === 'stream.info');
  const resize = await requestResize(epochClient, host, 'verified-tls-resize');
  host.json(resizedInfo(resize));await epochClient.take(value => value.type === 'stream.info' && value.epoch === resize.epoch);
  const resizedTelemetry = streamTelemetry(epochSession.sessionId, resize.epoch);
  host.json(resizedTelemetry); assert.deepEqual(await epochClient.take(value => value.type === 'stream.stats'), resizedTelemetry);
  host.send(epochFrame(0, 99));const fresh = epochFrame(resize.epoch, 100);host.send(fresh);assert.deepEqual(await epochClient.take(Buffer.isBuffer), fresh);
});

const serviceCapabilities = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true };
async function serviceHost(f, capabilities = serviceCapabilities) {
  const enrolled = await f.enroll(); assert.equal(enrolled.status, 201);
  const host = await socket(f.wsUrl('/api/v1/hosts/channel'), 'Host ' + enrolled.body.hostToken);
  host.json({ type: 'host.capabilities', capabilities });
  assert.equal((await host.take(value => value.type === 'host.capabilities.ack')).accepted, true);
  return { host, enrolled };
}
// Wire compatibility with the released client and Host: what they still say
// about RDP is accepted and not read, and asking for it is refused as before.
test('Only streaming is offered: a released client that asks for RDP gets the refusal it knows, and what a released Host reports about RDP is not read', async t => {
  const f = await fixture(t), enrolled = await f.enroll();
  const host = await socket(f.wsUrl('/api/v1/hosts/channel'), 'Host ' + enrolled.body.hostToken);
  const modes = async () => (await f.call('/desktops', { token: f.alice })).body.desktops[0].connectionModes;
  const rdp = async (extra = {}) => { const response = await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice, data: { mode: 'rdp', ...extra } }); return [response.status, response.body.error.code]; };
  assert.deepEqual(await modes(), { stream: true }); // Old stream-only peer compatibility.
  assert.deepEqual(await rdp(), [409, 'rdp_not_supported']);
  // The request of a released client carries its own `rdp` object.
  assert.deepEqual(await rdp({ rdp: { protocol: 'rdp-relay-v1', availabilityId: 'a'.repeat(32), engineNonce: 'b'.repeat(32), clientPublicKey: '04' + 'c'.repeat(128) } }), [409, 'rdp_not_supported']);
  for (const mode of [null, '', 'direct', {}, 1]) assert.equal((await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice, data: { mode } })).status, 400);
  for (const extra of [{ port: 3389 }, { destination: '192.0.2.1' }, { password: 'not-a-real-password' }, { credentialProtocol: 'host-credentials-v1' }]) {
    assert.equal((await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice, data: { mode: 'rdp', ...extra } })).body.error.code, 'unexpected_field');
  }
  // A released Host reports rdpSupported and rdpEnabled: any value is accepted,
  // the answer names no RDP protocol and the flags are passed on to nobody.
  host.json({ type: 'host.capabilities', capabilities: { ...serviceCapabilities, rdpSupported: true, rdpEnabled: true, streaming: false, interactiveSession: false, virtualDisplayReady: false } });
  const ack = await host.take(value => value.type === 'host.capabilities.ack');
  assert.equal(ack.accepted, true); assert.equal('rdpRelayProtocol' in ack, false);
  assert.deepEqual(await modes(), { stream: false });
  assert.deepEqual(await rdp(), [409, 'rdp_not_supported']);
  assert.equal((await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice, data: { mode: 'stream' } })).body.error.code, 'stream_unavailable');
  host.json({ type: 'host.capabilities', capabilities: { ...serviceCapabilities, rdpEnabled: 'true' } });
  assert.equal((await host.take(value => value.type === 'host.capabilities.ack')).accepted, true);
  // A message of the retired relay is not answered and does not end the Host's connection.
  host.json({ type: 'host.rdp.eligibility', protocol: 'rdp-relay-v1', revision: 1 });
  host.json({ type: 'host.heartbeat', capabilities: { ...serviceCapabilities, rdpSupported: false, rdpEnabled: false } });
  const answered = await host.take(value => value.type === 'host.heartbeat.ack' || value.type === 'host.rdp.eligibility.ack');
  assert.equal(answered.type, 'host.heartbeat.ack', 'the retired message got no answer');
  assert.deepEqual(await modes(), { stream: true });
  const capabilities = (await f.call('/admin/inventory', f.admin)).body.desktops.find(d => d.id === 'pve-170').host.capabilities;
  assert.equal('rdpEnabled' in capabilities, false); assert.equal('rdpSupported' in capabilities, false);
  const listed = (await f.call('/desktops', { token: f.alice })).body.desktops[0];
  assert.equal('rdpAvailability' in listed, false); assert.equal('rdpEnabled' in listed.host.capabilities, false);
  // The address of the retired tunnel is no socket.
  await assert.rejects(socket(f.wsUrl('/api/v1/hosts/sessions/' + '0'.repeat(8) + '-0000-4000-8000-' + '0'.repeat(12) + '/rdp'), 'Host ' + enrolled.body.hostToken));
});

test('Stream options are bounded, returned as applied requests and mapped onto host start fields', async t => {
  const f = await fixture(t), { host } = await serviceHost(f);
  for (const options of [null, [], { maxWidth: 3842 }, { maxHeight: 2162 }, { maxWidth: 1919 }, { fps: 120 }, { bitrate: 20000001 }, { bitrate: 3999999 }, { audio: 'false' }, { destination: 'localhost' }, { monitor: 0 }, { monitor: '16' }]) {
    assert.equal((await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice, data: { mode: 'stream', options } })).body.error.code, 'invalid_stream_options');
  }
  assert.equal((await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice, data: { mode: 'rdp', options: {} } })).body.error.code, 'rdp_not_supported');
  const defaults = await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice });
  assert.deepEqual(defaults.body.options, { maxWidth: 1920, maxHeight: 1080, fps: 60, bitrate: 12000000, audio: true, monitor: '0' });
  const defaultClient = await socket(f.wsUrl('/api/v1/sessions/' + defaults.body.sessionId), 'Bearer ' + f.alice);
  const defaultStart = await host.take(value => value.type === 'session.start');
  assert.equal(defaultStart.width, 1920); assert.equal(defaultStart.height, 1080); assert.equal(defaultStart.bitrate, 12000000);
  defaultClient.json({ type: 'session.stop' }); await host.take(value => value.type === 'session.stop');
  const options = { maxWidth: 1280, maxHeight: 720, fps: 30, bitrate: 4000000, audio: false, monitor: '0' };
  const response = await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice, data: { mode: 'stream', options } });
  assert.deepEqual(response.body.options, options);
  const client = await socket(f.wsUrl('/api/v1/sessions/' + response.body.sessionId), 'Bearer ' + f.alice);
  assert.deepEqual(await host.take(value => value.type === 'session.start'), { type: 'session.start', sessionId: response.body.sessionId, mode: 'stream', width: 1280, height: 720, fps: 30, bitrate: 4000000, audio: false, monitor: '0', clipboardPolicy:{textToRemote:false,textToLocal:false,filesToRemote:false,filesToLocal:false} });
  host.json({ ...streamInfo, sourceWidth: 1584, sourceHeight: 884, width: 1280, height: 714, fpsTarget: 30, bitrate: 4000000 });
  const actual = await client.take(value => value.type === 'stream.info');
  assert.equal(actual.width, 1280); assert.equal(actual.height, 714); assert.equal(actual.sourceWidth, 1584);
});

test('Advertised dual displays permit an independently bounded composite stream without leaking device identifiers', async t => {
  const f = await fixture(t), { host } = await serviceHost(f);
  assert.equal((await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice, data: { options: { monitor: 'all' } } })).body.error.code, 'monitor_unavailable');
  const monitors = [{ id: '0', width: 1920, height: 1080, primary: true, serial: 'do-not-publish-device-serial' }, { id: '1', width: 1920, height: 1080, primary: false }];
  host.json({ type: 'host.capabilities', capabilities: { ...serviceCapabilities, monitors, displayCount: 999, hostToken: 'do-not-publish-host-fields' } });
  assert.equal((await host.take(value => value.type === 'host.capabilities.ack')).accepted, true);
  const capabilities = (await f.call('/desktops', { token: f.alice })).body.desktops[0].host.capabilities;
  assert.equal(capabilities.displayCount, 2); assert.equal(capabilities.monitors.length, 2);
  assert.equal(JSON.stringify(capabilities).includes('do-not-publish'), false);
  for (const invalid of [[monitors[0], monitors[0]], [{ ...monitors[0], width: 0 }], [{ ...monitors[0], id: 'device-serial' }], Array(17).fill(monitors[0])]) {
    host.json({ type: 'host.capabilities', capabilities: { ...serviceCapabilities, monitors: invalid } });
    assert.equal((await host.take(value => value.type === 'host.capabilities.ack')).accepted, false);
  }
  assert.equal((await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice, data: { options: { monitor: '2' } } })).body.error.code, 'monitor_unavailable');
  const connection = await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice, data: { options: { monitor: 'all', maxWidth: 3840, maxHeight: 2160, fps: 30, bitrate: 20000000 } } });
  assert.equal(connection.status, 200);
  const client = await socket(f.wsUrl('/api/v1/sessions/' + connection.body.sessionId), 'Bearer ' + f.alice);
  const start = await host.take(value => value.type === 'session.start');
  assert.equal(start.monitor, 'all'); assert.equal(start.width, 3840); assert.equal(start.height, 2160); assert.equal(start.fps, 30); assert.equal(start.bitrate, 20000000);
  host.json({ ...streamInfo, width: 3840, height: 1080, sourceWidth: 3840, sourceHeight: 1080, fpsTarget: 30 });
  assert.equal((await client.take(value => value.type === 'stream.info')).width, 3840);
});

const resizeCapabilities = { ...serviceCapabilities, dynamicResolution: true, resizeProtocol: 'epoch-v1', monitors: [
  { id: '0', width: 1920, height: 1080, primary: true, modes: [{ width: 1920, height: 1080, refreshRate: 60 }, { width: 1280, height: 720, refreshRate: 60 }] },
  { id: '1', width: 1920, height: 1080, primary: false, modes: [{ width: 1920, height: 1080, refreshRate: 60 }] },
] };
async function epochController(f, host, optIn = true) {
  const connection = await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice, data: { mode: 'stream', ...(optIn ? { resizeProtocol: 'epoch-v1' } : {}) } });
  assert.equal(connection.status, 200);assert.equal(connection.body.transport, optIn ? 'media-epoch-v1' : 'media-v1');
  const client = await socket(f.wsUrl('/api/v1/sessions/' + connection.body.sessionId), 'Bearer ' + f.alice);
  const start = await host.take(value => value.type === 'session.start');
  assert.equal(start.resizeProtocol, optIn ? 'epoch-v1' : undefined);
  host.json({ ...streamInfo, epoch: 0 });await client.take(value => value.type === 'stream.info');
  return { client, start, session: connection.body };
}
function epochFrame(epoch, marker, key = true, kind = 1) {
  const frame = Buffer.alloc(kind === 1 ? 18 : 16);frame[0] = kind;frame[1] = kind === 1 && key ? 1 : 0;frame.writeUInt16LE(epoch, 2);frame[12] = marker;return frame;
}
async function requestResize(client, host, requestId = 'resize-1', extra = {}) {
  client.json({ type: 'stream.resize', width: 1280, height: 720, fps: 30, monitor: '0', requestId, ...extra });
  const resizing = await client.take(value => value.type === 'stream.resizing');
  const request = await host.take(value => value.type === 'stream.resize');
  assert.equal(request.resizeId, resizing.resizeId);assert.equal(request.epoch, resizing.epoch);assert.equal(request.requestId, requestId);
  return request;
}
function resizedInfo(request) {
  return { ...streamInfo, width: request.width, height: request.height, sourceWidth: request.width, sourceHeight: request.height, fpsTarget: request.fps, sessionId: request.sessionId, requestId: request.requestId, resizeId: request.resizeId, epoch: request.epoch };
}

test('Viewport provisioning accepts unlisted bounded modes with one hardware transaction and no input during resize', async t => {
  const f=await fixture(t),{host}=await serviceHost(f,{...resizeCapabilities,viewportProvisioning:true});
  const {client,session}=await epochController(f,host);assert.equal(session.viewportProvisioning,true);
  const seen=[];host.on('message',(data,binary)=>{if(!binary)seen.push(JSON.parse(data.toString()));});
  const request=await requestResize(client,host,'viewport-custom',{width:1346,height:812,fps:60});
  await host.take(value=>value.type==='input.reset');
  client.json({type:'input',event:'move',x:.5,y:.5});
  client.json({type:'stream.resize',width:1548,height:916,fps:60,monitor:'0',requestId:'viewport-busy'});
  const rejected=await client.take(value=>value.type==='stream.resize.failed');assert.equal(rejected.requestId,'viewport-busy');assert.equal(rejected.reason,'resize_in_progress');assert.equal(rejected.unchanged,true);
  client.json({type:'ping',id:'resize-input-barrier'});await host.take(value=>value.type==='ping');
  assert.equal(seen.filter(value=>value.type==='input').length,0);
  assert.equal(seen.filter(value=>value.type==='stream.resize').length,1);
  host.json(resizedInfo(request));const info=await client.take(value=>value.type==='stream.info');assert.equal(info.width,1346);assert.equal(info.sourceWidth,1346);
  host.send(epochFrame(request.epoch,42));assert.equal((await client.take(Buffer.isBuffer))[12],42);
});

test('Viewport provisioning uses a bounded independent deadline and still revalidates authorization',async t=>{
  const f=await fixture(t,{relayOptions:{resizeReadyMs:20,viewportReadyMs:180}}),{host}=await serviceHost(f,{...resizeCapabilities,viewportProvisioning:true});
  const {client}=await epochController(f,host);await requestResize(client,host,'viewport-timeout',{width:1366,height:768});
  await new Promise(resolve=>setTimeout(resolve,65));client.json({type:'ping',id:'still-resizing'});await host.take(value=>value.type==='ping'&&value.id==='still-resizing');
  const failure=await client.take(value=>value.type==='stream.resize.failed');assert.equal(failure.reason,'resize_timeout');assert.equal(failure.unchanged,false);
  await client.take(value=>value.type==='session.ended');await host.take(value=>value.type==='session.stop');
  const next=await epochController(f,host);await requestResize(next.client,host,'viewport-revoked',{width:1366,height:768});
  f.store.revoke('alice','pve-170');assert.equal((await next.client.take(value=>value.type==='session.ended')).reason,'authorization_revoked');
});

test('Viewport capability requires ready owned displays and real source and encoded dimensions must agree',async t=>{
  const f=await fixture(t),{host}=await serviceHost(f,resizeCapabilities);
  for(const capabilities of [{...resizeCapabilities,viewportProvisioning:'yes'},{...resizeCapabilities,viewportProvisioning:true,dynamicResolution:false},{...resizeCapabilities,viewportProvisioning:true,virtualDisplayReady:false},{...resizeCapabilities,viewportProvisioning:true,interactiveSession:false}]){
    host.json({type:'host.capabilities',capabilities});assert.equal((await host.take(value=>value.type==='host.capabilities.ack')).accepted,false);
  }
  host.json({type:'host.capabilities',capabilities:{...resizeCapabilities,viewportProvisioning:true}});assert.equal((await host.take(value=>value.type==='host.capabilities.ack')).accepted,true);
  const {client}=await epochController(f,host),request=await requestResize(client,host,'viewport-scaled',{width:1366,height:768});
  host.json({...resizedInfo(request),width:1280,height:720});assert.equal((await client.take(value=>value.type==='stream.resize.failed')).reason,'invalid_resize_info');
  assert.equal((await client.take(value=>value.type==='session.ended')).reason,'resize_failed');
});

test('Resize protocol is explicitly opted in by both peers; old clients and hosts retain reserved-zero media', async t => {
  const f = await fixture(t), { host } = await serviceHost(f, resizeCapabilities);
  const { client } = await epochController(f, host, false);
  client.json({ type: 'stream.resize', width: 1280, height: 720, fps: 30, monitor: '0', requestId: 'legacy-request' });
  assert.deepEqual(await client.take(value => value.type === 'stream.resize.failed'), { type: 'stream.resize.failed', requestId: 'legacy-request', reason: 'dynamic_resolution_unavailable', unchanged: true });
  const original = epochFrame(0, 1);host.send(original);assert.deepEqual(await client.take(Buffer.isBuffer), original);
  client.json({ type: 'session.stop' });await host.take(value => value.type === 'session.stop');
  host.json({ type: 'host.capabilities', capabilities: serviceCapabilities });await host.take(value => value.type === 'host.capabilities.ack');
  const fallback = await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice, data: { resizeProtocol: 'epoch-v1' } });
  assert.equal(fallback.body.transport, 'media-v1');assert.equal(fallback.body.resizeProtocol, undefined);
  assert.equal((await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice, data: { resizeProtocol: 'invented' } })).status, 400);
});

test('Actual socket resize fences late old media, resets input during changes and requires a fresh keyframe', async t => {
  const f = await fixture(t), { host } = await serviceHost(f, resizeCapabilities), { client } = await epochController(f, host);
  const resets = [], inputs = [];host.on('message', (data, binary) => { if (!binary) { const value=JSON.parse(data.toString());if(value.type==='input.reset')resets.push(1);if(value.type==='input')inputs.push(value); } });
  const packets = [];client.on('message', (data, binary) => { if (binary) packets.push(Buffer.from(data)); });
  const request = await requestResize(client, host);
  assert.equal(request.epoch, 1);
  // These synthetic input events reach only this isolated fixture WebSocket.
  client.json({ type: 'input', event: 'key', vk: 65, scan: 0, down: true, extended: false });
  client.json({type:'ping',id:'pending-input-barrier'});await host.take(value=>value.type==='ping'&&value.id==='pending-input-barrier');
  assert.equal(inputs.length,0);
  host.send(epochFrame(0, 10));host.json({ ...streamInfo, epoch: 0 });
  host.json(resizedInfo(request));await client.take(value => value.type === 'stream.info' && value.epoch === request.epoch);
  host.send(epochFrame(0, 11));host.send(epochFrame(0, 12, false, 2)); // Late old decoder generation after the new header.
  host.send(epochFrame(1, 13, false)); // Delta frame cannot initialize the fresh decoder.
  const fresh = epochFrame(1, 14);host.send(fresh);assert.deepEqual(await client.take(Buffer.isBuffer), fresh);
  const audio = epochFrame(1, 15, false, 2);host.send(audio);assert.deepEqual(await client.take(Buffer.isBuffer), audio);
  host.json({ type: 'pong', id: 'fence-barrier' });await client.take(value => value.type === 'pong');
  assert.deepEqual(packets.map(packet => packet[12]), [14, 15]);
  client.json({ type: 'input', event: 'key', vk: 65, scan: 0, down: false, extended: false });
  assert.equal((await host.take(value => value.type === 'input')).down, false);
  assert.equal(resets.length, 1);
});

test('Resize accepts only a selected advertised mode and two requests per second, without mixing superseded epochs', async t => {
  const f = await fixture(t), { host } = await serviceHost(f, resizeCapabilities), { client } = await epochController(f, host);
  for (const [requestId, extra, reason] of [['absent-mode', { width: 1600, height: 900 }, 'unsupported_display_mode'], ['other-monitor', { monitor: '1' }, 'monitor_not_available'], ['composite', { monitor: 'all' }, 'monitor_not_available']]) {
    client.json({ type: 'stream.resize', width: 1280, height: 720, fps: 30, monitor: '0', requestId, ...extra });
    const failure = await client.take(value => value.type === 'stream.resize.failed');assert.equal(failure.reason, reason);assert.equal(failure.unchanged, true);
  }
  const first = await requestResize(client, host, 'first'), second = await requestResize(client, host, 'second', { width: 1920, height: 1080, fps: 60 });
  assert.equal(second.epoch, 2);
  client.json({ type: 'stream.resize', width: 1280, height: 720, fps: 30, monitor: '0', requestId: 'rate-limited' });
  assert.equal((await client.take(value => value.type === 'stream.resize.failed')).reason, 'resize_rate_limited');
  const packets = [];client.on('message', (data, binary) => { if (binary) packets.push(data[12]); });
  host.json(resizedInfo(first));host.send(epochFrame(first.epoch, 20));
  host.json(resizedInfo(second));assert.equal((await client.take(value => value.type === 'stream.info')).epoch, 2);
  host.send(epochFrame(first.epoch, 21));const current = epochFrame(second.epoch, 22);host.send(current);assert.deepEqual(await client.take(Buffer.isBuffer), current);
  host.json({ type: 'pong', id: 'second-barrier' });await client.take(value => value.type === 'pong');assert.deepEqual(packets, [22]);
});

test('Resize host failure, timeout and assignment revocation terminate the affected stream cleanly while keeping the host online', async t => {
  const f = await fixture(t, { relayOptions: { resizeReadyMs: 100 } }), { host } = await serviceHost(f, resizeCapabilities);
  const first = await epochController(f, host), request = await requestResize(first.client, host);
  host.json({ type: 'stream.resize.failed', sessionId: request.sessionId, requestId: request.requestId, resizeId: request.resizeId, epoch: request.epoch, reason: 'display_change_failed' });
  const failure = await first.client.take(value => value.type === 'stream.resize.failed');assert.equal(failure.unchanged, false);assert.equal(failure.reason, 'display_change_failed');
  assert.equal((await first.client.take(value => value.type === 'session.ended')).reason, 'resize_failed');
  await host.take(value => value.type === 'input.reset');await host.take(value => value.type === 'session.stop');
  const timeout = await epochController(f, host);await requestResize(timeout.client, host);
  assert.equal((await timeout.client.take(value => value.type === 'stream.resize.failed')).reason, 'resize_timeout');await timeout.client.take(value => value.type === 'session.ended');
  await host.take(value => value.type === 'input.reset');await host.take(value => value.type === 'session.stop');
  const revoked = await epochController(f, host);await requestResize(revoked.client, host);
  f.store.revoke('alice', 'pve-170');assert.equal((await revoked.client.take(value => value.type === 'session.ended')).reason, 'authorization_revoked');
  await host.take(value => value.type === 'input.reset');await host.take(value => value.type === 'session.stop');
  host.json({ type: 'host.heartbeat' });await host.take(value => value.type === 'host.heartbeat.ack');
});

test('Explicit capability-protocol hosts stay unavailable until a valid advertisement while headerless legacy hosts retain stream compatibility', async t => {
  const f = await fixture(t), enrolled = await f.enroll();
  const host = await socket(f.wsUrl('/api/v1/hosts/channel'), 'Host ' + enrolled.body.hostToken, { headers: { Authorization: 'Host ' + enrolled.body.hostToken, 'X-Multitap-Host-Protocol': 'capabilities-v1' } });
  const desktop = async () => (await f.call('/desktops', { token: f.alice })).body.desktops[0];
  assert.deepEqual((await desktop()).connectionModes, { stream: false });assert.equal((await desktop()).host.capabilitiesPending, true);
  assert.equal((await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice })).body.error.code, 'stream_unavailable');
  host.json({ type: 'host.capabilities', capabilities: { streaming: true } });
  assert.equal((await host.take(value => value.type === 'host.capabilities.ack')).accepted, false);assert.equal((await desktop()).canConnect, false);
  for (const capabilities of [
    { ...resizeCapabilities, resizeProtocol: 'unknown' },
    { ...resizeCapabilities, monitors: [{ ...resizeCapabilities.monitors[0], modes: Array(65).fill({ width: 1280, height: 720, refreshRate: 60 }) }] },
    { ...resizeCapabilities, monitors: [{ ...resizeCapabilities.monitors[0], modes: [{ width: 1281, height: 720, refreshRate: 60 }] }] },
  ]) {
    host.json({ type: 'host.capabilities', capabilities });
    assert.equal((await host.take(value => value.type === 'host.capabilities.ack')).accepted, false);assert.equal((await desktop()).host.capabilitiesPending, true);
  }
  host.json({ type: 'host.capabilities', capabilities: { ...serviceCapabilities, streaming: false, virtualDisplayReady: false } });
  await host.take(value => value.type === 'host.capabilities.ack');assert.deepEqual((await desktop()).connectionModes, { stream: false });assert.equal((await desktop()).host.capabilitiesPending, false);
  const closed = once(host, 'close');host.close();await closed;
  const legacy = await socket(f.wsUrl('/api/v1/hosts/channel'), 'Host ' + enrolled.body.hostToken);
  assert.deepEqual((await desktop()).connectionModes, { stream: true });legacy.json({ type: 'host.heartbeat' });await legacy.take(value => value.type === 'host.heartbeat.ack');
});

// ---------------------------------------------------------------- Host transport under load
// With large frames (4K video) the Host's WinHTTP can write the pong for a relay
// ping inside a large binary frame it is sending; the relay's ws parser reads
// the rest of that frame as headers and ends every session as host_error.
function maskedFrame(opcode, payload) {
  const length = payload.length;
  const head = length < 126 ? Buffer.from([0x80 | opcode, 0x80 | length]) : Buffer.from([0x80 | opcode, 0x80 | 126, length >> 8, length & 255]);
  return Buffer.concat([head, Buffer.alloc(4), payload]); // zero mask key: payload bytes as written
}
test('A Host frame broken by an interleaved pong ends its sessions as host_error with a framing detail and an audit record', async t => {
  const f = await fixture(t), { host, client } = await connected(f);
  // A video packet (valid 12-byte media header) whose tail, displaced by the
  // pong, is read as the next frame header (0x70: RSV bits set).
  const media = Buffer.alloc(100, 0x70); media.fill(0, 0, 12); media[0] = 1;
  const frame = maskedFrame(2, media), pong = maskedFrame(10, Buffer.alloc(0));
  host._socket.write(Buffer.concat([frame.subarray(0, 6 + 50), pong, frame.subarray(6 + 50)]));
  assert.deepEqual(await client.take(value => value.type === 'session.ended'), { type: 'session.ended', reason: 'host_error', detail: 'host_frame_invalid' });
  await once(host, 'close');
  const row = f.store.db.prepare("SELECT actor,subject FROM audit_events WHERE event='host_channel_closed'").get();
  assert.equal(row.subject, 'pve-170:host_error:host_frame_invalid:1');
  assert.equal(row.actor, f.store.db.prepare('SELECT id FROM hosts WHERE desktop_id=?').get('pve-170').id);
});

test('Oversized and non-UTF-8 Host messages are named in the session end', async t => {
  for (const [send, detail] of [
    [host => host.send(Buffer.alloc(4 * 1024 * 1024 + 1), { binary: true }), 'host_message_too_large'],
    [host => host.send(Buffer.from([0x7b, 0xc0, 0xaf, 0x7d]), { binary: false }), 'host_text_invalid'],
  ]) {
    const f = await fixture(t), { host, client } = await connected(f);
    send(host);
    assert.deepEqual(await client.take(value => value.type === 'session.ended'), { type: 'session.ended', reason: 'host_error', detail });
  }
});

test('A Host that keeps sending is never pinged; a silent one is pinged and expires without a pong', async t => {
  const f = await fixture(t), enrolled = await f.enroll(); // heartbeatMs 200
  const host = await socket(f.wsUrl('/api/v1/hosts/channel'), 'Host ' + enrolled.body.hostToken);
  let pings = 0; host.on('ping', () => ++pings);
  const busy = setInterval(() => host.json({ type: 'host.heartbeat' }), 40);
  try { await delay(250); pings = 0; await delay(800); } finally { clearInterval(busy); }
  assert.equal(pings, 0, 'A Host sending traffic was pinged');
  await delay(700);
  assert.ok(pings >= 1, 'A silent Host was not pinged');
  assert.equal(host.readyState, WebSocket.OPEN, 'Answered pings did not keep the silent Host');
  host.close(); await once(host, 'close');
  const silent = await socket(f.wsUrl('/api/v1/hosts/channel'), 'Host ' + enrolled.body.hostToken, { autoPong: false });
  const started = Date.now(); await once(silent, 'close');
  assert.ok(Date.now() - started < 2000, 'A silent Host without pongs was kept');
});

test('host-closing-v1: the capabilities ACK offers it and a Host that says why it closes ends its sessions with that detail', async t => {
  const f = await fixture(t), enrolled = await f.enroll();
  const host = await socket(f.wsUrl('/api/v1/hosts/channel'), 'Host ' + enrolled.body.hostToken);
  host.json({ type: 'host.capabilities', capabilities: {} });
  const rejected = await host.take(value => value.type === 'host.capabilities.ack');
  assert.equal(rejected.accepted, false); assert.equal(rejected.closingProtocol, undefined);
  host.json({ type: 'host.capabilities', capabilities: serviceCapabilities });
  assert.equal((await host.take(value => value.type === 'host.capabilities.ack')).closingProtocol, 'host-closing-v1');
  const connection = await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice });
  const client = await socket(f.wsUrl('/api/v1/sessions/' + connection.body.sessionId), 'Bearer ' + f.alice);
  await host.take(value => value.type === 'session.start');
  host.json({ type: 'host.closing', reason: 'service_stopping' });
  assert.deepEqual(await client.take(value => value.type === 'session.ended'), { type: 'session.ended', reason: 'host_disconnected', detail: 'host_service_stopping' });
  await once(host, 'close');
  assert.equal(f.store.db.prepare("SELECT subject FROM audit_events WHERE event='host_channel_closed'").get().subject, 'pve-170:host_disconnected:host_service_stopping:1');
});

test('keyframe-v1: a client that falls behind skips video until the IDR the Host is asked for, instead of being disconnected', async t => {
  const f = await fixture(t), { host } = await serviceHost(f, { ...serviceCapabilities, keyFrameProtocol: 'keyframe-v1' });
  const connection = await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice });
  const client = await socket(f.wsUrl('/api/v1/sessions/' + connection.body.sessionId), 'Bearer ' + f.alice);
  const start = await host.take(value => value.type === 'session.start');
  host.json(streamInfo); await client.take(value => value.type === 'stream.info');
  const frame = (key, marker, size) => { const bytes = Buffer.alloc(size); bytes[0] = 1; bytes[1] = key ? 1 : 0; bytes[12] = marker; return bytes; };
  const received = []; client.on('message', (data, binary) => { if (binary) received.push(data[12]); });
  client._socket.pause();
  try {
    for (let n = 0; n < 40; n++) host.send(frame(n === 0, n, 512 * 1024));
    const request = await host.take(value => value.type === 'stream.keyframe');
    assert.equal(request.sessionId, start.sessionId);
    host.send(frame(false, 200, 512 * 1024)); // still behind: skipped, never a P frame without its references
  } finally { client._socket.resume(); }
  for (let n = 0; n < 50 && client.readyState === WebSocket.OPEN && received.length < 3; n++) await delay(20);
  await delay(200);
  assert.equal(client.readyState, WebSocket.OPEN, 'The client was disconnected for its backlog');
  const before = received.length;
  assert.ok(before > 0 && before < 40, 'Video was not skipped');
  assert.ok(!received.includes(200), 'A P frame after the skip reached the client');
  host.send(frame(false, 201, 1000)); // not an IDR: still skipped
  host.send(frame(true, 202, 1000)); host.send(frame(false, 203, 1000));
  for (let n = 0; n < 50 && !received.includes(203); n++) await delay(20);
  assert.deepEqual(received.slice(before), [202, 203], 'Video did not resume exactly at the IDR');
  client.terminate();
});


test('explicit HTTP transport preserves authenticated relay origin and nondefault public port', async t => {
  const publicOrigin = 'http://192.0.2.10:18400';
  const f = await fixture(t, { transportOrigin: publicOrigin, allowInsecureHttp: true });
  const c = await connected(f);
  t.after(() => { c.client.terminate(); c.host.terminate(); });
  assert.equal(c.enrolled.body.serverOrigin, publicOrigin);
  assert.equal(c.session.webSocketUrl, 'ws://192.0.2.10:18400/api/v1/sessions/' + c.session.sessionId);
  assert.equal((await f.call('/auth/me', { token: f.alice })).status, 200);
  const denied = await f.call('/desktops/pve-149/connect', { method: 'POST', token: f.alice });
  assert.equal(denied.status, 404, 'HTTP does not expand assignment authority');
  c.client.close(); c.host.close();
});
