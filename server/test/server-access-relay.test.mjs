import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes, randomUUID, X509Certificate } from 'node:crypto';
import { createServer as createTcpServer } from 'node:net';
import { request as httpRequest } from 'node:http';
import { request as httpsRequest } from 'node:https';
import { readFile, lstat, realpath } from 'node:fs/promises';
import { join, resolve, dirname, basename } from 'node:path';
import { tmpdir } from 'node:os';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { loadTlsListener } from '../src/tls-listener.mjs';
import { normalizeServerAccess, serverAccessHash } from '../update/server-access-policy.mjs';
import { devicePolicyKeys } from '../src/device-policy.mjs';

// Real TCP HTTP/TLS and authenticated ws/wss. Synthetic accounts/VM/media only:
// this cannot establish Windows/Android capture, decode, injection or display.
// Existing relay.test.mjs service capabilities / 18-byte media framing and
// input-latency.test.mjs negotiation + ACK fields are reused unchanged.
const password = 'Server-access-network-fixture-only-42!';
const vmid = 4187, desktopId = 'pve-4187';
const machineUuid = '12345678-1234-4234-8234-000000004187';
const streamInfo = { type: 'stream.info', codec: 'h264', width: 1280, height: 720,
  sourceWidth: 1920, sourceHeight: 1080, fpsTarget: 60,
  audio: { sampleRate: 48000, channels: 2, format: 's16le', available: false } };
const serviceCapabilities = { service: true, interactiveSession: true, streaming: true,
  virtualDisplayReady: true };
const tlsDirectory = process.env.MULTITAP_TEST_TLS_DIRECTORY;
const tlsCase = { timeout: 20000, skip: !tlsDirectory ? 'Root-owned ephemeral localhost TLS fixture required' : false };
const lookupLoopback = (_hostname, options, done) => options?.all
  ? done(null, [{ address: '127.0.0.1', family: 4 }]) : done(null, '127.0.0.1', 4);
let passwordHash;
const hash = () => passwordHash ??= hashPassword(password);

async function tlsMaterial() {
  // No generation, install, global trust mutation or repository key lookup.
  const directory = resolve(tlsDirectory);
  assert.equal(dirname(directory), resolve(tmpdir()));
  assert.match(basename(directory), /^multitap-tls-fixture-[a-f0-9]{32}$/);
  assert.equal(await realpath(directory), directory);
  const folder = await lstat(directory);
  assert.ok(folder.isDirectory() && !folder.isSymbolicLink());
  if (process.platform !== 'win32') { assert.equal(folder.uid, process.getuid()); assert.equal(folder.mode & 0o077, 0); }
  for (const name of ['cert.pem', 'key.pem']) {
    const file = await lstat(join(directory, name));
    assert.ok(file.isFile() && !file.isSymbolicLink() && file.size > 0 && file.size <= 65536);
    if (process.platform !== 'win32') { assert.equal(file.uid, process.getuid()); assert.equal(file.mode & 0o022, 0); }
  }
  const ca = await readFile(join(directory, 'cert.pem'));
  const certificate = new X509Certificate(ca);
  assert.equal(certificate.checkHost('localhost'), 'localhost');
  assert.ok(Date.parse(certificate.validFrom) <= Date.now() && Date.parse(certificate.validTo) > Date.now());
  return { directory, ca };
}
async function listen(server, port) {
  await new Promise((done, fail) => {
    const failed = error => { server.off('listening', ready); fail(error); };
    const ready = () => { server.off('error', failed); done(); };
    server.once('error', failed); server.once('listening', ready); server.listen(port, '127.0.0.1');
  });
}
async function reservePorts() {
  const reservations = [createTcpServer(), createTcpServer()];
  try { for (const server of reservations) await listen(server, 0); }
  catch (error) { for (const server of reservations) if (server.listening) server.close(); throw error; }
  const ports = reservations.map(server => server.address().port); let released = false;
  return { ports, async release() {
    if (released) return; released = true;
    await Promise.all(reservations.map(server => new Promise(done => server.close(done))));
  } };
}
const wsUrl = (origin, path) => { const url = new URL(path, origin); url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:'; return url.href; };
const failureCode = reply => typeof reply.body?.error === 'string' ? reply.body.error : reply.body?.error?.code;
function assertPeerUrl(value, origin, path) {
  const url = new URL(value), peer = new URL(origin);
  assert.equal(url.protocol, peer.protocol === 'https:' ? 'wss:' : 'ws:');
  assert.equal(url.host, peer.host); assert.equal(url.pathname, path);
  assert.equal(url.username, ''); assert.equal(url.password, ''); assert.equal(url.search, ''); assert.equal(url.hash, '');
}
async function fixture(t, { mode = 'both', primary = 'https', legacy = false, bootstrap = false, disabledHttpListener = false } = {}) {
  const reservation = await reservePorts(), [httpPort, httpsPort] = reservation.ports;
  t.after(() => reservation.release()); // also owns setup-failure cleanup before TLS loading
  const origins = { http: 'http://localhost:' + httpPort, https: 'https://localhost:' + httpsPort };
  const hasTls = mode !== 'http';
  const material = hasTls ? await tlsMaterial() : null;
  const tls = material ? await loadTlsListener({ certFile: join(material.directory, 'cert.pem'), keyFile: join(material.directory, 'key.pem') }) : null;
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  let server; const sockets = new Set(), peers = new Set(), requestCounts = { http: 0, https: 0 };
  const cleanup = async () => {
    for (const peer of peers) peer.terminate();
    for (const socket of sockets) socket.destroy();
    try { if (server) await new Promise(done => server.close(done)); }
    finally { await reservation.release(); store.close(); }
  };
  t.after(cleanup);
  try {
    const digest = await hash();
    for (const [name, role] of [['admin', 'admin'], ['alice', 'user'], ['bobby', 'user']]) store.addUser({ name, role, passwordHash: digest });
    store.syncProxmox([{ vmid, node: 'fixture', name: 'Synthetic access VM', status: 'running' }], 'network-fixture');
    store.assign('alice', desktopId);
    store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve-fixture.invalid', tokenId: 'fixture@pve!broker', secret: 'synthetic-only' }), 'fixture');
    const primaryScheme = mode === 'both' ? primary : mode;
    const config = bootstrap ? null : normalizeServerAccess({ protocol: 'server-access-v1', primaryOrigin: origins[primaryScheme],
      https: { enabled: hasTls, origin: origins.https, mode: 'direct', listenHost: '127.0.0.1', listenPort: httpsPort,
        // Canonical Linux agent metadata only; buildServer consumes the ACTUAL
        // protected ephemeral options above, never these synthetic file names.
        ...(hasTls ? { certFile: '/fixture/cert.pem', keyFile: '/fixture/key.pem' } : {}) },
      http: { enabled: mode !== 'https', origin: origins.http, listenHost: '127.0.0.1', listenPort: httpPort }, trustedProxies: [] });
    const proxmox = { machineIdentity: async (_settings, id) => { assert.equal(id, vmid); return { machineUuid }; } };
    server = await buildServer({ store, vault, proxmox,
      publicOrigin: bootstrap ? null : origins[primaryScheme], allowInsecureHttp: mode !== 'https',
      tls: primaryScheme === 'https' ? tls : null, trustedProxies: [],
      ...(legacy ? {} : { accessConfig: bootstrap ? undefined : config,
        accessRevision: config ? serverAccessHash(config) : null, accessInstallationId: randomUUID(), bootstrap }),
      relayOptions: { authorizationMs: 60000, heartbeatMs: 60000 }, logger: { error: value => assert.fail(value) } });
    const listeners = [{ server, scheme: primaryScheme }];
    if (mode === 'both') listeners.push({ server: server.createAdditionalListener(primaryScheme === 'https' ? null : tls), scheme: primaryScheme === 'https' ? 'http' : 'https' });
    if (disabledHttpListener) listeners.push({ server: server.createAdditionalListener(null), scheme: 'http' });
    for (const item of listeners) {
      item.server.on('connection', socket => { sockets.add(socket); socket.once('close', () => sockets.delete(socket)); });
      item.server.on('request', () => { requestCounts[item.scheme]++; });
    }
    await reservation.release();
    // Fixed canonical ports were reserved at0 then closed before config/bind;
    // unexpected bind failure is a setup failure, never a retry/pass waiver.
    for (const item of listeners) await listen(item.server, item.scheme === 'http' ? httpPort : httpsPort);
  } finally { tls?.key.fill(0); }
  async function request(origin, path, { method = 'GET', data, token, cookie, csrf, headers = {}, ca = material?.ca } = {}) {
    const url = new URL(path, origin), secure = url.protocol === 'https:';
    return new Promise((done, fail) => {
      const request = (secure ? httpsRequest : httpRequest)(url, {
        lookup: lookupLoopback, agent: false, timeout: 3000,
        ...(secure ? { ca, servername: 'localhost', rejectUnauthorized: true, minVersion: 'TLSv1.2' } : {}),
        method, headers: { ...(method !== 'GET' ? { Origin: origin } : {}),
          ...(data === undefined ? {} : { 'Content-Type': 'application/json' }),
          ...(token ? { Authorization: 'Bearer ' + token } : {}), ...(cookie ? { Cookie: cookie } : {}),
          ...(csrf ? { 'X-CSRF-Token': csrf } : {}), ...headers },
      }, response => {
        const chunks = []; let size = 0;
        response.on('data', chunk => { size += chunk.length; if (size > 65536) request.destroy(Error('fixture_body_limit')); else chunks.push(chunk); });
        response.once('error', fail);
        response.once('end', () => { let body = null; try { body = JSON.parse(Buffer.concat(chunks).toString('utf8')); } catch { /* caller asserts expected result */ }
          done({ status: response.statusCode, headers: response.headers, body }); });
      });
      request.once('error', fail); request.once('timeout', () => request.destroy(Error('fixture_request_timeout')));
      request.end(data === undefined ? undefined : JSON.stringify(data));
    });
  }
  async function socket(url, authorization, headers = {}) {
    return new Promise((done, fail) => {
      const parsed = new URL(url), secure = parsed.protocol === 'wss:';
      const peer = new WebSocket(url, { lookup: lookupLoopback, handshakeTimeout: 3000,
        perMessageDeflate: false, maxPayload: 4 * 1024 * 1024,
        ...(secure ? { ca: material.ca, servername: 'localhost', rejectUnauthorized: true, minVersion: 'TLSv1.2' } : {}),
        headers: { Authorization: authorization, ...headers } });
      peers.add(peer); peer.once('close', () => peers.delete(peer));
      const queue = [], waiters = [];
      const end = () => { for (const waiter of waiters.splice(0)) { clearTimeout(waiter.timer); waiter.fail(Error('fixture_socket_closed')); } };
      peer.on('error', () => {}); peer.once('error', fail); peer.once('close', end);
      peer.once('unexpected-response', (_request, response) => { response.resume();
        fail(Object.assign(Error('fixture_upgrade_rejected'), { status: response.statusCode })); peer.terminate(); });
      peer.on('message', (bytes, binary) => {
        const value = binary ? bytes : JSON.parse(bytes.toString('utf8'));
        const index = waiters.findIndex(waiter => waiter.match(value));
        if (index < 0) { if (queue.length >= 64) { peer.terminate(); return; } queue.push(value); }
        else { const waiter = waiters.splice(index, 1)[0]; clearTimeout(waiter.timer); waiter.done(value); }
      });
      peer.take = (match = () => true) => {
        const index = queue.findIndex(match); if (index >= 0) return Promise.resolve(queue.splice(index, 1)[0]);
        return new Promise((resolve, reject) => { const waiter = { match, done: resolve, fail: reject,
          timer: setTimeout(() => { const index = waiters.indexOf(waiter); if (index >= 0) waiters.splice(index, 1); reject(Error('fixture_message_timeout')); }, 3000) };
          waiters.push(waiter); });
      };
      peer.json = value => peer.send(JSON.stringify(value)); peer.once('open', () => done(peer));
    });
  }
  async function login(origin, name = 'alice', admin = false) {
    const result = await request(origin, admin ? '/api/v1/admin/login' : '/api/v1/auth/login', { method: 'POST', data: { username: name, password } });
    assert.equal(result.status, 200, 'synthetic account login must succeed at its own allowed origin');
    return admin ? { cookie: result.headers['set-cookie'][0].split(';')[0], rawCookie: result.headers['set-cookie'][0], csrf: result.body.csrfToken } : result.body.sessionToken;
  }
  return { server, store, origins, request, socket, login, ca: material?.ca, requestCount: scheme => requestCounts[scheme] };
}

for (const mode of ['http', 'https', 'both']) test('real enabled listener health: ' + mode, mode === 'http' ? { timeout: 20000 } : tlsCase, async t => {
  const f = await fixture(t, { mode });
  for (const scheme of mode === 'both' ? ['http', 'https'] : [mode]) {
    const reply = await f.request(f.origins[scheme], '/healthz');
    assert.equal(reply.status, 200); assert.equal(reply.body.service, 'multitap-broker');
    assert.deepEqual(reply.body.transport, { protocol: 'broker-transport-v1', publicOrigin: f.origins[scheme], scheme,
      allowInsecureHttp: scheme === 'http', directTls: scheme === 'https' });
    assert.equal(reply.body.access.protocol, 'server-access-v1');
    assert.equal(reply.body.access.origin, f.origins[scheme]);
    assert.equal(Object.hasOwn(reply.body.access, 'primaryOrigin'), false);
    assert.equal(Object.hasOwn(reply.body.access, 'allowedOrigins'), false);
    assert.ok(Buffer.byteLength(JSON.stringify(reply.body)) <= 8192, 'Host existing bounded health reader remains sufficient');
  }
});

for (const [clientScheme, hostScheme] of [['https', 'http'], ['http', 'https']]) {
  test('real relay preserves separate client/Host origins: ' + clientScheme + ' client / ' + hostScheme + ' Host', tlsCase, async t => {
    const f = await fixture(t, { mode: 'both', primary: clientScheme });
    const clientOrigin = f.origins[clientScheme], hostOrigin = f.origins[hostScheme];
    const admin = await f.login(clientOrigin, 'admin', true), token = await f.login(clientOrigin);
    const enrollment = await f.request(clientOrigin, '/api/v1/admin/desktops/' + desktopId + '/enrollment', { ...admin, method: 'POST', data: {} });
    assert.equal(enrollment.status, 201);
    const enrolled = await f.request(hostOrigin, '/api/v1/hosts/enroll', { method: 'POST', data: {
      code: enrollment.body.code, hostname: 'Synthetic network Host', machineUuid, version: '0.2.99' } });
    assert.equal(enrolled.status, 201);
    // Existing Store API seeds assignment permission; no relay origin/session mutation.
    f.store.updateAssignmentPolicy('alice', desktopId, { devicePolicy: Object.fromEntries(devicePolicyKeys.map(key => [key, key === 'drives'])) }, 'network-fixture');
    const host = await f.socket(wsUrl(hostOrigin, '/api/v1/hosts/channel'), 'Host ' + enrolled.body.hostToken,
      { 'X-Multitap-Host-Protocol': 'capabilities-v1' });
    host.json({ type: 'host.capabilities', capabilities: { ...serviceCapabilities,
      inputLatencyProtocol: 'input-latency-v1', driveProtocol: 'drive-v1', driveFs: true } });
    assert.equal((await host.take(value => value.type === 'host.capabilities.ack')).accepted, true);
    const connected = await f.request(clientOrigin, '/api/v1/desktops/' + desktopId + '/connect', { method: 'POST', token,
      data: { mode: 'stream', inputLatencyProtocol: 'input-latency-v1', driveProtocol: 'drive-v1', driveFs: true } });
    assert.equal(connected.status, 200); const grant = connected.body;
    assert.equal(grant.inputLatencyProtocol, 'input-latency-v1');
    assertPeerUrl(grant.webSocketUrl, clientOrigin, '/api/v1/sessions/' + grant.sessionId);
    assertPeerUrl(grant.driveWebSocketUrl, clientOrigin, '/api/v1/sessions/' + grant.sessionId + '/drive');
    const client = await f.socket(grant.webSocketUrl, 'Bearer ' + token);
    const start = await host.take(value => value.type === 'session.start');
    assert.equal(start.sessionId, grant.sessionId);
    assertPeerUrl(start.driveUrl, hostOrigin, '/api/v1/hosts/sessions/' + grant.sessionId + '/drive');
    await assert.rejects(f.socket(wsUrl(hostOrigin, '/api/v1/sessions/' + grant.sessionId), 'Bearer ' + token), error => error.status === 403);
    await assert.rejects(f.socket(wsUrl(clientOrigin, '/api/v1/hosts/sessions/' + grant.sessionId + '/drive'), 'Host ' + enrolled.body.hostToken), error => error.status === 403);
    host.json(streamInfo); assert.deepEqual(await client.take(value => value.type === 'stream.info'), streamInfo);
    // Same valid legacy media-v1 synthetic framing as relay.test.mjs, not H.264 decode proof.
    const bytes = Buffer.alloc(18); bytes[0] = 1; bytes[1] = 1; bytes.writeBigUInt64LE(12345n, 4);
    bytes.set([0, 0, 0, 1, 0x65, 1], 12); host.send(bytes);
    assert.deepEqual(await client.take(Buffer.isBuffer), bytes);
    client.json({ type: 'input', event: 'move', x: 0.25, y: 0.75, sample: 1 });
    assert.equal((await host.take(value => value.type === 'input')).sample, 1);
    const ack = { type: 'input.ack', sessionId: grant.sessionId, sample: 1, ok: true, injectUs: 100, holdUs: 200 };
    host.json(ack); assert.deepEqual(await client.take(value => value.type === 'input.ack'), ack);
    const hostDrive = await f.socket(start.driveUrl, 'Host ' + enrolled.body.hostToken);
    const clientDrive = await f.socket(grant.driveWebSocketUrl, 'Bearer ' + token);
    assert.equal((await hostDrive.take(value => value.type === 'drive.ready')).sessionId, grant.sessionId);
    assert.equal((await clientDrive.take(value => value.type === 'drive.ready')).sessionId, grant.sessionId);
    clientDrive.json({ type: 'drive.shares', shares: [] });
    assert.deepEqual(await hostDrive.take(value => value.type === 'drive.shares'), { type: 'drive.shares', shares: [] });
    const other = await f.login(clientOrigin, 'bobby');
    assert.equal((await f.request(clientOrigin, '/api/v1/desktops/' + desktopId + '/connect', { method: 'POST', token: other, data: { mode: 'stream' } })).status, 404);
    client.json({ type: 'session.stop' });
    assert.equal((await host.take(value => value.type === 'session.stop')).sessionId, grant.sessionId);
  });
}

test('strict origin-scoped cookies and CSRF use actual two-listener admin requests', tlsCase, async t => {
  const f = await fixture(t), http = await f.login(f.origins.http, 'admin', true), https = await f.login(f.origins.https, 'admin', true);
  assert.match(http.cookie, /^multitap-admin-http-[a-f0-9]{16}=/);
  assert.match(https.cookie, /^__Secure-multitap-admin-[a-f0-9]{16}=/);
  assert.equal(http.rawCookie.includes('; Secure;'), false); assert.equal(https.rawCookie.includes('; Secure;'), true);
  assert.notEqual(http.cookie.split('=')[0], https.cookie.split('=')[0]);
  assert.equal((await f.request(f.origins.https, '/api/v1/admin/server-info', { cookie: http.cookie })).status, 401);
  assert.equal((await f.request(f.origins.http, '/api/v1/admin/server-info', { cookie: https.cookie })).status, 401);
  assert.equal((await f.request(f.origins.https, '/api/v1/admin/logout', { method: 'POST', cookie: https.cookie, data: {} })).status, 403);
  assert.equal((await f.request(f.origins.https, '/api/v1/admin/logout', { method: 'POST', cookie: https.cookie, csrf: http.csrf, data: {} })).status, 403);
  assert.equal((await f.request(f.origins.https, '/api/v1/admin/logout', { method: 'POST', ...https, headers: { Origin: f.origins.http }, data: {} })).status, 403);
  assert.equal((await f.request(f.origins.https, '/api/v1/admin/logout', { method: 'POST', ...https, data: {} })).status, 200);
});

test('public Host/Origin/forwarded metadata cannot select an unregistered endpoint', tlsCase, async t => {
  const f = await fixture(t);
  // Via is NOT trusted authority: it only excludes the intentional loopback
  // legacy-updater health shortcut so these are PUBLIC admission negatives.
  const via = { Via: '1.1 multitap-network-fixture' };
  for (const headers of [
    { ...via, Host: 'unregistered.invalid:12345' },
    { ...via, Origin: f.origins.https },
    { ...via, Host: new URL(f.origins.https).host, 'X-Forwarded-Proto': 'https', 'X-Forwarded-Host': new URL(f.origins.https).host },
  ]) {
    const result = await f.request(f.origins.http, '/healthz', { headers });
    assert.equal(result.status, 403); assert.equal(failureCode(result), 'origin_not_allowed');
  }
  const ignored = await f.request(f.origins.http, '/healthz', { headers: { ...via,
    'X-Forwarded-Proto': 'https', 'X-Forwarded-Host': 'unregistered.invalid:443' } });
  assert.equal(ignored.status, 200); assert.equal(ignored.body.transport.publicOrigin, f.origins.http);
  await assert.rejects(f.socket(wsUrl(f.origins.http, '/api/v1/hosts/channel'), 'Host synthetic-invalid',
    { Origin: f.origins.https }), error => error.status === 403);
});

test('HTTP-off public route refuses even an accidentally attached plain listener', tlsCase, async t => {
  const f = await fixture(t, { mode: 'https', disabledHttpListener: true });
  const reply = await f.request(f.origins.http, '/healthz', { headers: { Via: '1.1 multitap-network-fixture' } });
  assert.equal(reply.status, 403); assert.equal(failureCode(reply), 'http_disabled');
  const login = await f.request(f.origins.http, '/api/v1/auth/login', { method: 'POST', data: { username: 'alice', password } });
  assert.equal(login.status, 403); assert.equal(failureCode(login), 'http_disabled');
});

test('legacy HTTPS buildServer mode retains verified TLS and old cookie/health shape', tlsCase, async t => {
  const f = await fixture(t, { mode: 'https', legacy: true });
  const reply = await f.request(f.origins.https, '/healthz');
  assert.equal(reply.status, 200); assert.equal(reply.body.access, undefined);
  assert.equal(reply.body.transport.publicOrigin, f.origins.https); assert.equal(reply.body.transport.directTls, true);
  const admin = await f.login(f.origins.https, 'admin', true);
  assert.ok(admin.cookie.startsWith('__Secure-multitap-admin='));
  assert.equal((await f.request(f.origins.https, '/api/v1/admin/server-info', admin)).status, 200);
});

test('bootstrap is management-only, not a ready client or Host relay', { timeout: 20000 }, async t => {
  const f = await fixture(t, { mode: 'http', bootstrap: true });
  const reply = await f.request(f.origins.http, '/healthz');
  assert.equal(reply.status, 200); assert.equal(reply.body.access.bootstrapOnly, true);
  assert.equal(reply.body.transport.publicOrigin, f.origins.http);
  assert.equal(reply.body.transport.allowInsecureHttp, false);
  const admin = await f.login(f.origins.http, 'admin', true);
  assert.equal((await f.request(f.origins.http, '/api/v1/admin/server-info', admin)).status, 200);
  for (const path of ['/api/v1/auth/login', '/api/v1/hosts/enroll']) {
    const result = await f.request(f.origins.http, path, { method: 'POST', data: { username: 'alice', password } });
    assert.equal(result.status, 403); assert.equal(failureCode(result), 'http_disabled');
  }
  await assert.rejects(f.socket(wsUrl(f.origins.http, '/api/v1/hosts/channel'), 'Host synthetic-invalid'), error => error.status === 403);
});

test('TLS trust failure never retries through the HTTP listener', tlsCase, async t => {
  const f = await fixture(t);
  const health = await f.request(f.origins.http, '/healthz'); assert.equal(health.status, 200);
  const beforeHttp = f.requestCount('http'), beforeHttps = f.requestCount('https');
  // null deliberately uses the system CA set for this negative only. All
  // successful TLS requests supply Root's CA; rejectUnauthorized stays true.
  await assert.rejects(f.request(f.origins.https, '/healthz', { ca: null }), error =>
    ['DEPTH_ZERO_SELF_SIGNED_CERT', 'SELF_SIGNED_CERT_IN_CHAIN', 'UNABLE_TO_VERIFY_LEAF_SIGNATURE', 'UNABLE_TO_GET_ISSUER_CERT_LOCALLY'].includes(error.code));
  assert.equal(f.requestCount('http'), beforeHttp, 'TLS failure cannot silently dial HTTP');
  assert.equal(f.requestCount('https'), beforeHttps, 'certificate rejection precedes every HTTP request');
});
