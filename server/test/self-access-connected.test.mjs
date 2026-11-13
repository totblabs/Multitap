// A save of 서버 접속 설정 on a Docker Compose install while people are
// connected: the real server on a loopback listener, a registered Host whose
// control channel connects again the moment it closes, an app with a session
// open and its live channel (both come back by themselves), and the
// administrator's requests over HTTP as the console page sends them. The
// restart itself is counted, not made; what an address answers at 연결 테스트
// is asked of this listener or of a closed loopback port, never the network.
// The HTTPS address the server cannot reach from inside is reached "from
// outside" as a reverse proxy on this machine would pass a browser's request on.
import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm, readdir } from 'node:fs/promises';
import { readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { randomBytes } from 'node:crypto';
import { createServer, request } from 'node:http';
import { setTimeout as delay } from 'node:timers/promises';
import { WebSocket } from 'ws';
import { configuration } from '../src/config.mjs';
import { SelfAccess, probeAddress } from '../src/self-access.mjs';
import { buildServer } from '../src/http.mjs';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { hashPassword, newToken, tokenHash } from '../src/security.mjs';

const secure = 'https://vdi.example.com';
const readAccessFile = path => { try { return readFileSync(path, 'utf8'); } catch (error) { if (error.code === 'ENOENT') return null; throw error; } };
const freePort = () => new Promise(resolve => { const probe = createServer(); probe.listen(0, '127.0.0.1', () => { const port = probe.address().port; probe.close(() => resolve(port)); }); });
const streamInfo = { type: 'stream.info', codec: 'h264', width: 1280, height: 720, sourceWidth: 1920, sourceHeight: 1080, fpsTarget: 60, audio: { sampleRate: 48000, channels: 2, format: 's16le', available: false } };
async function until(predicate, ms = 4000, what = 'fixture_timeout') { const end = Date.now() + ms; while (!predicate()) { assert.ok(Date.now() < end, what); await delay(5); } }

async function connected(t) {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-access-connected-')), port = await freePort(), closedPort = await freePort(), here = '192.0.2.10:' + port, origin = 'http://' + here;
  const settings = { MULTITAP_HOST: '127.0.0.1', MULTITAP_PORT: String(port), MULTITAP_DEPLOYMENT_KIND: 'docker', MULTITAP_DATA_DIR: directory, MULTITAP_PUBLISHED_PORT: '0.0.0.0:' + port, MULTITAP_SERVER_ADDRESS: '192.0.2.10', MULTITAP_HTTP_ORIGIN: origin, MULTITAP_ALLOW_INSECURE_HTTP: '1' };
  const config = configuration(settings, { readAccessFile });
  const f = { directory, port, origin, exits: 0, events: [], hostConnects: 0, live: 0, sessionsOpened: 0, host: null, skew: 0 };
  // An HTTP address is asked of this listener; an HTTPS one of a port nothing listens on.
  const probeRequest = (address, nonce) => { const ask = (options, callback) => request({ ...options, protocol: 'http:', hostname: '127.0.0.1', port: address.startsWith('https://') ? closedPort : port, rejectUnauthorized: undefined }, callback); return probeAddress(address, nonce, { http: ask, https: ask }); };
  const access = SelfAccess.from(config, { exit: () => { f.exits++; f.exitAt = Date.now(); }, load: () => configuration(settings, { readAccessFile }), probeRequest, now: () => Date.now() + f.skew });
  await access.initialize();
  const store = new Store(':memory:'), password = 'Connected-tests-password-42!', sockets = new Set();
  store.addUser({ name: 'owner', role: 'admin', passwordHash: await hashPassword(password) });
  store.addUser({ name: 'alice', role: 'user', passwordHash: 'fixture-unused' });
  store.saveSetting('proxmox_initialized', '1', 'fixture');
  store.syncProxmox([{ vmid: 149, node: 'fixture', name: 'VM149', status: 'running' }], 'fixture');
  store.assign('alice', 'pve-149');
  const tokens = { alice: newToken(), host: newToken() }, code = newToken(), machine = '12345678-1234-1234-1234-123456789abc';
  store.createSession(store.findUser('alice'), tokenHash(tokens.alice), Date.now(), 3600000, 'client');
  store.issueEnrollment('pve-149', machine, tokenHash(code), 'fixture');
  store.consumeEnrollment(tokenHash(code), machine, { hostname: 'Fixture', version: '9.0.0', tokenHash: tokenHash(tokens.host) });
  const server = await buildServer({ store, vault: new Vault(randomBytes(32)), publicOrigin: config.publicOrigin, allowInsecureHttp: config.allowInsecureHttp, accessConfig: config.accessConfig, accessRevision: config.accessRevision, accessInstallationId: config.accessInstallationId,
    accessIdentityFile: config.accessIdentityFile, accessIdentityRead: () => config.selfAccess.read(config.accessIdentityFile, 128), accessClient: access, deploymentKind: 'docker', trustedProxies: config.trustedProxies, initialSetupOptions: { announce: () => {} }, logger: { error: value => assert.fail(String(value)) } });
  await new Promise(resolve => server.listen(port, '127.0.0.1', resolve));
  let closing = false, stopped = false;
  // index.mjs shutdown(): what the restart does to the listener and everything connected.
  f.stop = async () => { if (stopped) return 0; stopped = true; const started = Date.now(); await new Promise(resolve => server.close(resolve)); return Date.now() - started; };
  t.after(async () => { closing = true; for (const ws of sockets) ws.terminate(); server.closeAllConnections(); await f.stop(); access.close(); store.close(); await rm(directory, { recursive: true, force: true }); });
  f.call = (path, { method = 'GET', data, headers = {} } = {}) => new Promise((resolve, reject) => {
    const body = data === undefined ? null : JSON.stringify(data);
    const req = request({ hostname: '127.0.0.1', port, path, method, headers: { Host: here, ...(body ? { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body) } : {}), ...headers } }, res => {
      const chunks = []; res.on('data', chunk => chunks.push(chunk)); res.once('end', () => { let json = null; try { json = JSON.parse(Buffer.concat(chunks).toString('utf8')); } catch { json = null; } resolve({ status: res.statusCode, headers: res.headers, body: json }); });
    });
    req.on('error', reject); req.end(body ?? undefined);
  });
  // A Host that connects again at once whenever its channel closes.
  const openHost = () => {
    if (closing) return;
    const ws = new WebSocket('ws://127.0.0.1:' + port + '/api/v1/hosts/channel', { headers: { Authorization: 'Host ' + tokens.host, Host: here }, perMessageDeflate: false });
    sockets.add(ws); ws.messages = [];
    ws.on('open', () => { f.hostConnects++; f.host = ws; ws.send(JSON.stringify({ type: 'host.capabilities', capabilities: { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true } })); });
    ws.on('message', (value, binary) => { if (!binary) { const message = JSON.parse(value.toString('utf8')); ws.messages.push(message); if (message.type === 'session.stop') f.events.push('host got session.stop ' + message.reason); } });
    ws.on('unexpected-response', (req, res) => { f.events.push('host refused ' + res.statusCode); setTimeout(openHost, 50); });
    ws.on('error', () => {}); ws.on('close', value => { sockets.delete(ws); f.events.push('host closed ' + value); setTimeout(openHost, 0); });
  };
  openHost(); await until(() => f.host?.messages.some(value => value.type === 'host.capabilities.ack'));
  // An app: its live channel and a session; each is opened again as soon as it ends.
  const bearer = { Authorization: 'Bearer ' + tokens.alice };
  const openLive = () => {
    if (closing) return;
    const ws = new WebSocket('ws://127.0.0.1:' + port + '/api/v1/events', { headers: { ...bearer, Host: here, Origin: origin }, perMessageDeflate: false });
    sockets.add(ws); ws.on('open', () => { f.live++; }); ws.on('error', () => {}); ws.on('unexpected-response', (req, res) => { f.events.push('live refused ' + res.statusCode); setTimeout(openLive, 50); });
    ws.on('close', value => { sockets.delete(ws); f.events.push('live closed ' + value); setTimeout(openLive, 0); });
  };
  openLive(); await until(() => f.live === 1);
  const openSession = async () => {
    if (closing) return;
    let reply;
    try { reply = await f.call('/api/v1/desktops/pve-149/connect', { method: 'POST', data: {}, headers: bearer }); } catch { f.events.push('connect failed'); return; }
    if (reply.status !== 200) { f.events.push('connect refused ' + reply.status + ' ' + reply.body?.error?.code); setTimeout(() => { void openSession(); }, 50); return; }
    const id = reply.body.sessionId, ws = new WebSocket('ws://127.0.0.1:' + port + '/api/v1/sessions/' + id, { headers: { ...bearer, Host: here }, perMessageDeflate: false });
    sockets.add(ws); ws.on('error', () => {});
    ws.on('open', () => { f.sessionsOpened++; });
    ws.on('message', (value, binary) => { if (!binary) { const message = JSON.parse(value.toString('utf8')); if (message.type === 'session.ended') f.events.push('app got session.ended ' + message.reason); } });
    ws.on('close', () => { sockets.delete(ws); f.events.push('app session closed'); setTimeout(() => { void openSession(); }, 0); });
    await until(() => f.host.messages.some(value => value.type === 'session.start' && value.sessionId === id));
    f.host.send(JSON.stringify(streamInfo));
    const video = Buffer.alloc(18); video[0] = 1; video[1] = 1; video.set([0, 0, 0, 1, 0x65, 1], 12); f.host.send(video);
  };
  await openSession(); await until(() => f.sessionsOpened === 1); await delay(100);
  // The administrator, over HTTP as the page.
  const login = await f.call('/api/v1/admin/login', { method: 'POST', data: { username: 'owner', password }, headers: { Origin: origin } });
  assert.equal(login.status, 200);
  const cookie = login.headers['set-cookie'][0].split(';')[0];
  f.admin = { Cookie: cookie, Origin: origin, 'X-CSRF-Token': login.body.csrfToken };
  f.status = async () => (await f.call('/api/v1/admin/server-access', { headers: { Cookie: cookie } })).body;
  f.activity = () => access.hooks.activity();
  f.server = server; f.access = access; f.cookie = cookie; f.owner = store.findUser('owner').id;
  f.audit = () => store.db.prepare('SELECT event, actor, subject FROM audit_events WHERE event LIKE ? ORDER BY id').all('server_access_%').filter(row => row.event !== 'server_access_tested');
  // The administrator signs in at another address of the console.
  f.signIn = async headers => {
    const reply = await f.call('/api/v1/admin/login', { method: 'POST', data: { username: 'owner', password }, headers });
    return reply.status === 200 ? { status: 200, setCookie: reply.headers['set-cookie'][0], headers: { ...headers, Cookie: reply.headers['set-cookie'][0].split(';')[0], 'X-CSRF-Token': reply.body.csrfToken } } : { status: reply.status, code: reply.body?.error?.code };
  };
  return f;
}

test('a save while a Host, an app session and a live channel keep connecting again: the connections are ended and the restart is asked for within two seconds', { timeout: 60000 }, async t => {
  const f = await connected(t);
  const first = await f.status();
  assert.deepEqual(first.connections, { people: 1, sessions: 1, consoles: 0 });
  assert.equal(f.activity().activity.hosts.connected, 1); assert.equal(f.activity().activity.live.channels, 1);
  const next = { ...first.applied, http: { ...first.applied.http, extraOrigins: ['http://vdi.example.com:' + f.port] } };
  const tested = await f.call('/api/v1/admin/server-access/test', { method: 'POST', data: { config: next, baseRevision: first.revision }, headers: f.admin });
  assert.equal(tested.status, 200, JSON.stringify(tested.body));
  assert.equal(f.events.length, 0, 'a test ends nothing'); assert.equal(f.activity().activity.sessions.total, 1);
  const started = Date.now();
  const saved = await f.call('/api/v1/admin/server-access/save', { method: 'POST', data: { config: next, testId: tested.body.testId, baseRevision: first.revision, confirm: true }, headers: f.admin });
  assert.equal(saved.status, 202, JSON.stringify(saved.body)); assert.equal(saved.body.state, 'waiting_idle');
  await until(() => f.exits === 1, 2000, 'the restart was not asked for within two seconds');
  const took = f.exitAt - started;
  assert.ok(took < 2000, 'took ' + took + ' ms');
  // Who was told what.
  assert.ok(f.events.includes('host got session.stop server_stopping'), f.events.join(' | '));
  assert.ok(f.events.includes('app got session.ended server_stopping'), f.events.join(' | '));
  assert.equal(f.hostConnects, 1, 'the Host\'s own channel is not a connection to end, and does not hold the save');
  // The app's attempts to come back are refused as a restart, so they do not count as open again.
  await until(() => f.events.some(line => line === 'connect refused 503 server_restarting'), 2000, f.events.join(' | '));
  assert.equal(f.activity().activity.sessions.total, 0);
  const status = await f.status();
  assert.equal(status.pending.state, 'applying'); assert.equal(status.pending.operationId, saved.body.operationId); assert.equal(f.server.accessRuntime.blocked, true);
  assert.deepEqual(JSON.parse(readFileSync(join(f.directory, 'access', 'policy.json'), 'utf8')).config.http.extraOrigins, ['http://vdi.example.com:' + f.port]);
  // The restart itself: the listener closes although a Host and a live channel are still connected.
  const closed = await f.stop();
  assert.ok(closed < 3000, 'closing took ' + closed + ' ms');
});

// The HTTPS address as it reaches this listener from a reverse proxy on this machine.
const outsideAt = { Host: 'vdi.example.com', 'X-Forwarded-Proto': 'https' }, outsidePost = { ...outsideAt, Origin: secure };
const withHttps = applied => ({ ...applied, primaryOrigin: secure, trustedProxies: ['127.0.0.1'], https: { ...applied.https, enabled: true, origin: secure } });

test('an HTTPS address this server cannot reach, with people connected: nobody is disconnected while the administrator proves it from outside, and only that proof lets the save go on', { timeout: 60000 }, async t => {
  const f = await connected(t);
  const first = await f.status(), next = withHttps(first.applied);
  assert.equal((await f.call('/admin/', { headers: outsideAt })).status, 403, 'not an address of this server');
  const tested = await f.call('/api/v1/admin/server-access/test', { method: 'POST', data: { config: next, baseRevision: first.revision }, headers: f.admin });
  assert.equal(tested.status, 200, JSON.stringify(tested.body));
  assert.deepEqual(tested.body.checks.filter(check => check.name === 'address'), [{ name: 'address', origin: secure, status: 'failed', code: 'server_access_address_closed' }, { name: 'address', origin: f.origin, status: 'passed', code: 'server_access_address_reached' }]);
  assert.equal(tested.body.confirmation, 'outside');
  const body = { config: next, testId: tested.body.testId, baseRevision: first.revision, confirm: true };
  // The ordinary save cannot name that test; the save with the proof opens a window and changes nothing.
  const plain = await f.call('/api/v1/admin/server-access/save', { method: 'POST', data: body, headers: f.admin });
  assert.equal(plain.status, 409); assert.equal(plain.body.error.code, 'server_access_test_required');
  const accepted = await f.call('/api/v1/admin/server-access/save', { method: 'POST', data: { ...body, outside: true }, headers: f.admin });
  assert.equal(accepted.status, 202, JSON.stringify(accepted.body)); assert.equal(accepted.body.state, 'awaiting_confirmation');
  const operationId = accepted.body.operationId;
  await delay(300);
  assert.deepEqual(f.events, [], 'nobody was told anything'); assert.equal(f.exits, 0); assert.equal(f.activity().activity.sessions.total, 1);
  assert.equal(f.server.accessRuntime.blocked, false, 'no fence: people go on working while it waits');
  assert.deepEqual(await readdir(join(f.directory, 'access')), ['journal.json'], 'no policy was written');
  const watching = await f.status();
  assert.equal(watching.state, 'awaiting_confirmation'); assert.equal(watching.pending.operationId, operationId); assert.equal(watching.revision, first.revision);
  assert.deepEqual(watching.outside.origins, [{ origin: secure, confirmed: false }]); assert.ok(watching.outside.remainingMs > 290000 && watching.outside.remainingMs <= 300000);
  const code = watching.outside.code; assert.match(code, /^\d{6}$/);
  // The address under proof: the console, and nothing else of the server.
  const consolePage = await f.call('/admin/', { headers: outsideAt });
  assert.equal(consolePage.status, 200); assert.equal(consolePage.headers['strict-transport-security'], undefined, 'no transport policy for a name that is not this server\'s yet');
  for (const path of ['/healthz', '/download', '/api/v1/desktops', '/api/v1/downloads', '/setup/proxmox.py', '/?x=1']) assert.equal((await f.call(path, { headers: outsideAt })).status, 403, path);
  // The address typed by itself (a phone has no link to press) leads to the console page that asks for the code.
  const typed = await f.call('/', { headers: outsideAt });
  assert.equal(typed.status, 302); assert.equal(typed.headers.location, '/admin/#/server-access'); assert.equal(typed.headers['strict-transport-security'], undefined);
  assert.equal((await f.call('/api/v1/admin/server-access', { headers: outsideAt })).status, 401, 'the console asks who is there, as at any address');
  const app = await f.call('/api/v1/desktops/pve-149/connect', { method: 'POST', data: {}, headers: { ...outsideAt, Authorization: 'Bearer nobody' } });
  assert.equal(app.status, 403, 'no app or Host is let in there yet');
  // What is not the proof.
  const confirm = (headers, data) => f.call('/api/v1/admin/server-access/confirm', { method: 'POST', data, headers });
  assert.equal((await confirm(outsidePost, { operationId, code })).status, 401, 'nobody signed in');
  const old = await confirm(f.admin, { operationId, code });
  assert.equal(old.status, 409); assert.equal(old.body.error.code, 'server_access_confirmation_origin', 'the right code at the old address');
  const there = await f.signIn(outsidePost);
  assert.equal(there.status, 200); assert.match(there.setCookie, /^__Secure-[^;]+;.*\bSecure\b/, 'signed in at the new address, over what the browser saw as HTTPS');
  const seen = await f.call('/api/v1/admin/server-access', { headers: { ...outsideAt, Cookie: there.headers.Cookie } });
  assert.equal(seen.status, 200); assert.equal(seen.body.managementOrigin, secure); assert.equal(seen.body.outside.code, undefined, 'the code has to be brought there');
  assert.deepEqual(seen.body.outside.origins, [{ origin: secure, confirmed: false }], 'signing in and looking is not the proof either');
  const wrong = await confirm(there.headers, { operationId, code: code === '000000' ? '000001' : '000000' });
  assert.equal(wrong.status, 409); assert.equal(wrong.body.error.code, 'server_access_confirmation_code');
  assert.equal((await confirm(there.headers, { operationId })).status, 400);
  await delay(100);
  assert.deepEqual(f.events, []); assert.equal(f.exits, 0);
  // The proof: signed in at the new address, with the code.
  const proved = await confirm(there.headers, { operationId, code });
  assert.equal(proved.status, 200, JSON.stringify(proved.body)); assert.deepEqual(proved.body, { accepted: true, operationId, origin: secure, remaining: [] });
  await until(() => f.exits === 1, 3000, 'the save did not go on to its restart');
  assert.ok(f.events.includes('host got session.stop server_stopping'), f.events.join(' | ')); assert.ok(f.events.includes('app got session.ended server_stopping'));
  assert.equal(f.server.accessRuntime.blocked, true, 'behind the fence from the proof on');
  assert.equal(JSON.parse(readFileSync(join(f.directory, 'access', 'policy.json'), 'utf8')).config.https.origin, secure);
  const after = await f.status();
  assert.equal(after.pending.state, 'applying'); assert.equal(after.outside, undefined);
  assert.deepEqual(f.audit().map(row => [row.event, row.actor, row.subject]), [['server_access_outside_started', f.owner, operationId + ':' + secure], ['server_access_outside_confirmed', f.owner, operationId + ':' + secure],
    ['server_access_connections_ended', f.owner, operationId + ':1:1']]);
});

test('the proof does not come: when the five minutes are over nobody has been told anything, the settings are the same and the address is closed again', { timeout: 60000 }, async t => {
  const f = await connected(t);
  const first = await f.status(), next = withHttps(first.applied);
  const tested = await f.call('/api/v1/admin/server-access/test', { method: 'POST', data: { config: next, baseRevision: first.revision }, headers: f.admin });
  const accepted = await f.call('/api/v1/admin/server-access/save', { method: 'POST', data: { config: next, testId: tested.body.testId, baseRevision: first.revision, confirm: true, outside: true }, headers: f.admin });
  assert.equal(accepted.status, 202, JSON.stringify(accepted.body));
  assert.equal((await f.call('/admin/', { headers: outsideAt })).status, 200);
  const there = await f.signIn(outsidePost);
  assert.equal(there.status, 200);
  f.skew += 300001;
  assert.equal((await f.call('/admin/', { headers: outsideAt })).status, 403, 'closed at the minute');
  assert.equal((await f.call('/api/v1/admin/server-access', { headers: { ...outsideAt, Cookie: there.headers.Cookie } })).status, 403, 'also to the administrator who signed in there');
  await f.access.task;
  const status = await f.status();
  assert.equal(status.state, 'failed'); assert.equal(status.errorCode, 'server_access_confirmation_timeout'); assert.equal(status.failedOrigin, secure); assert.equal(status.unchangedVerified, true);
  assert.equal(status.revision, first.revision); assert.equal(status.pending, null); assert.deepEqual(status.applied, first.applied);
  assert.deepEqual(f.events, [], 'nobody was told anything'); assert.equal(f.exits, 0); assert.equal(f.activity().activity.sessions.total, 1); assert.equal(f.server.accessRuntime.blocked, false);
  assert.deepEqual(await readdir(join(f.directory, 'access')), ['journal.json']);
  assert.deepEqual(f.audit().map(row => [row.event, row.actor, row.subject]), [['server_access_outside_started', f.owner, accepted.body.operationId + ':' + secure], ['server_access_outside_expired', f.owner, accepted.body.operationId + ':' + secure]]);
  // The form is free for another try.
  const again = await f.call('/api/v1/admin/server-access/test', { method: 'POST', data: { config: next, baseRevision: first.revision }, headers: f.admin });
  assert.equal(again.status, 200); assert.equal(again.body.confirmation, 'outside');
});

// A fresh install: no policy yet, the console open at the server's own address for its first setup.
test('the first policy of a fresh install, with an HTTPS address this server cannot reach: the same proof from outside, and the first-setup address stays the way in meanwhile', { timeout: 60000 }, async t => {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-access-fresh-')), port = await freePort(), closedPort = await freePort(), here = '192.0.2.10:' + port, origin = 'http://' + here;
  const settings = { MULTITAP_HOST: '127.0.0.1', MULTITAP_PORT: String(port), MULTITAP_DEPLOYMENT_KIND: 'docker', MULTITAP_DATA_DIR: directory, MULTITAP_PUBLISHED_PORT: '0.0.0.0:' + port, MULTITAP_SERVER_ADDRESS: '192.0.2.10', NODE_ENV: 'production', MULTITAP_HTTP_ORIGIN: '', MULTITAP_ALLOW_INSECURE_HTTP: '0' };
  const config = configuration(settings, { readAccessFile });
  assert.equal(config.bootstrapOnly, true); assert.equal(config.setupOrigin, origin); assert.equal(config.accessConfig ?? null, null);
  let exits = 0;
  const probeRequest = (address, nonce) => { const ask = (options, callback) => request({ ...options, protocol: 'http:', hostname: '127.0.0.1', port: address.startsWith('https://') ? closedPort : port, rejectUnauthorized: undefined }, callback); return probeAddress(address, nonce, { http: ask, https: ask }); };
  const access = SelfAccess.from(config, { exit: () => { exits++; }, load: () => configuration(settings, { readAccessFile }), probeRequest });
  await access.initialize();
  const store = new Store(':memory:'), password = 'Fresh-install-password-42!';
  store.addUser({ name: 'owner', role: 'admin', passwordHash: await hashPassword(password) });
  const server = await buildServer({ store, vault: new Vault(randomBytes(32)), publicOrigin: config.publicOrigin, allowInsecureHttp: config.allowInsecureHttp, accessConfig: config.accessConfig, accessRevision: config.accessRevision, accessInstallationId: config.accessInstallationId,
    accessIdentityFile: config.accessIdentityFile, accessIdentityRead: () => config.selfAccess.read(config.accessIdentityFile, 128), accessClient: access, bootstrap: true, setupOrigin: config.setupOrigin, deploymentKind: 'docker', trustedProxies: config.trustedProxies,
    initialSetupOptions: { announce: () => {} }, logger: { error: value => assert.fail(String(value)) } });
  await new Promise(resolve => server.listen(port, '127.0.0.1', resolve));
  t.after(async () => { server.closeAllConnections(); await new Promise(resolve => server.close(resolve)); access.close(); store.close(); await rm(directory, { recursive: true, force: true }); });
  const call = (path, { method = 'GET', data, headers = {} } = {}) => new Promise((resolve, reject) => {
    const body = data === undefined ? null : JSON.stringify(data);
    const req = request({ hostname: '127.0.0.1', port, path, method, headers: { Host: here, ...(body ? { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body) } : {}), ...headers } }, res => {
      const chunks = []; res.on('data', chunk => chunks.push(chunk)); res.once('end', () => { let json = null; try { json = JSON.parse(Buffer.concat(chunks).toString('utf8')); } catch { json = null; } resolve({ status: res.statusCode, headers: res.headers, body: json }); });
    });
    req.on('error', reject); req.end(body ?? undefined);
  });
  const signIn = async headers => { const reply = await call('/api/v1/admin/login', { method: 'POST', data: { username: 'owner', password }, headers }); assert.equal(reply.status, 200, JSON.stringify(reply.body)); return { ...headers, Cookie: reply.headers['set-cookie'][0].split(';')[0], 'X-CSRF-Token': reply.body.csrfToken }; };
  const admin = await signIn({ Origin: origin });
  const first = (await call('/api/v1/admin/server-access', { headers: admin })).body;
  assert.equal(first.applied, null, 'no policy yet'); assert.equal(first.supported, true);
  const next = { protocol: 'server-access-v1', primaryOrigin: secure, trustedProxies: ['127.0.0.1'], https: { enabled: true, origin: secure, mode: 'proxy', listenHost: '0.0.0.0', listenPort: port },
    http: { enabled: true, origin, listenHost: '0.0.0.0', listenPort: port } };
  const tested = await call('/api/v1/admin/server-access/test', { method: 'POST', data: { config: next, baseRevision: first.revision }, headers: admin });
  assert.equal(tested.status, 200, JSON.stringify(tested.body)); assert.equal(tested.body.confirmation, 'outside');
  assert.deepEqual(tested.body.checks.filter(check => check.name === 'address').map(check => [check.origin, check.status]), [[secure, 'failed'], [origin, 'passed']]);
  assert.equal((await call('/admin/', { headers: outsideAt })).status, 403);
  const accepted = await call('/api/v1/admin/server-access/save', { method: 'POST', data: { config: next, testId: tested.body.testId, baseRevision: first.revision, confirm: true, outside: true }, headers: admin });
  assert.equal(accepted.status, 202, JSON.stringify(accepted.body));
  // The first-setup address is still the way in, and shows the code.
  const watching = (await call('/api/v1/admin/server-access', { headers: admin })).body;
  assert.equal(watching.state, 'awaiting_confirmation'); assert.equal(watching.applied, null); assert.match(watching.outside.code, /^\d{6}$/);
  // The new address: the console only, signed in there, with the code.
  assert.equal((await call('/admin/', { headers: outsideAt })).status, 200); assert.equal((await call('/healthz', { headers: outsideAt })).status, 403);
  const there = await signIn(outsidePost);
  const proved = await call('/api/v1/admin/server-access/confirm', { method: 'POST', data: { operationId: accepted.body.operationId, code: watching.outside.code }, headers: there });
  assert.equal(proved.status, 200, JSON.stringify(proved.body));
  await until(() => exits === 1, 3000, 'the save did not go on to its restart');
  const saved = JSON.parse(readFileSync(join(directory, 'access', 'policy.json'), 'utf8'));
  assert.equal(saved.config.https.origin, secure); assert.equal(saved.config.http.origin, origin);
  // The restarted server has its policy, asks itself at the address it can reach, and keeps it.
  const restarted = configuration(settings, { readAccessFile });
  assert.equal(restarted.bootstrapOnly, false); assert.deepEqual([...restarted.allowedOrigins].sort(), [origin, secure].sort());
  const asked = [], second = SelfAccess.from(restarted, { exit: () => assert.fail('no way back is taken'), load: () => configuration(settings, { readAccessFile }),
    publicRequest: async (current, address) => { asked.push(address); return { service: 'multitap-broker', status: 'ok', authentication: 'id-password', version: '1.1.3', access: { protocol: 'server-access-v1', revision: restarted.accessRevision, installationId: restarted.accessInstallationId, bootstrapOnly: false, origin: address } }; } });
  await second.initialize(); await second.resume();
  assert.deepEqual(asked, [origin]); assert.equal(second.status().state, 'succeeded');
});
