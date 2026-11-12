// Real isolated HTTP/WS/SQLite. Host messages are synthetic protocol evidence,
// not proof of installed Windows input, USB or display restoration.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createServer, request } from 'node:http';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword, newToken, tokenHash } from '../src/security.mjs';
import { normalizeServerAccess } from '../update/server-access-policy.mjs';

const origin = 'https://pc-cleanup.fixture.invalid', password = 'Only-PC-cleanup-fixture-password-48!';
const hashed = hashPassword(password);
const policy = { 'X-Multitap-Session-Policy': 'windows-pc-v1', 'X-Multitap-Login-Intent': 'confirmable-v1' };
const caps = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true, virtualMonitorControl: true,   sessionStopProtocol: 'stop-v1', monitors: [{ id: '0', width: 1920, height: 1080, primary: true }] };
async function until(predicate) { const deadline = Date.now() + 4000; while (!predicate()) { assert.ok(Date.now() < deadline, 'bounded fixture wait'); await new Promise(resolve => setTimeout(resolve, 5)); } }
async function fixture(t, { browserFirst = false } = {}) {
  const reservation = createServer(); await new Promise(resolve => reservation.listen(0, '127.0.0.1', resolve));
  const port = reservation.address().port; await new Promise(resolve => reservation.close(resolve));
  const config = normalizeServerAccess({ protocol: 'server-access-v1', primaryOrigin: origin,
    https: { enabled: true, origin, mode: 'proxy', listenHost: '127.0.0.1', listenPort: port },
    http: { enabled: false, origin: null, listenHost: '127.0.0.1', listenPort: port }, trustedProxies: ['127.0.0.1'] });
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32)), sockets = [], errors = [];
  store.addUser({ name: 'alice', passwordHash: await hashed });
  store.syncProxmox([{ vmid: 149, node: 'fixture', name: 'Synthetic VM', status: 'running' }], 'fixture'); store.assign('alice', 'pve-149');
  const hostToken = newToken(), code = newToken(), uuid = '12345678-1234-1234-1234-123456789abc';
  store.issueEnrollment('pve-149', uuid, tokenHash(code), 'fixture');
  store.consumeEnrollment(tokenHash(code), uuid, { hostname: 'Fixture', version: '9.0.0', tokenHash: tokenHash(hostToken) });
  const server = await buildServer({ store, vault, publicOrigin: origin, accessConfig: config, trustedProxies: config.trustedProxies,
    relayOptions: { authorizationMs: 10, heartbeatMs: 10000, stopPollMs: 20 }, desktopEventOptions: { authorizationMs: 10, tickMs: 20 },
    logger: { error: value => errors.push(value) } });
  await new Promise(resolve => server.listen(port, '127.0.0.1', resolve));
  t.after(async () => { for (const ws of sockets) ws.terminate(); server.closeAllConnections(); await new Promise(resolve => server.close(resolve)); store.close(); });
  const base = 'http://127.0.0.1:' + port;
  const common = { Host: new URL(origin).host, Origin: origin, 'X-Forwarded-Proto': 'https' };
  function call(path, { method = 'GET', token, headers = {}, data, raw } = {}) {
    const payload = raw ?? (data === undefined ? undefined : JSON.stringify(data));
    return new Promise((resolve, reject) => {
      const req = request(base + path, { method, headers: { ...common, ...headers,
        ...(token ? { Authorization: 'Bearer ' + token } : {}), ...(data === undefined ? {} : { 'Content-Type': 'application/json' }),
        ...(payload === undefined ? {} : { 'Content-Length': Buffer.byteLength(payload) }) } }, res => {
        const chunks = []; res.on('data', bytes => chunks.push(bytes)); res.on('end', () => {
          try { const text = Buffer.concat(chunks).toString(); resolve({ status: res.statusCode, body: text ? JSON.parse(text) : null, cookie: res.headers['set-cookie']?.[0]?.split(';')[0] }); }
          catch (error) { reject(error); }
        });
      });
      req.on('error', reject); req.setTimeout(5000, () => req.destroy(Error('fixture request timeout'))); req.end(payload);
    });
  }
  async function open(path, token, host = false) {
    const ws = new WebSocket(base.replace('http:', 'ws:') + path, { headers: { ...common, Authorization: (host ? 'Host ' : 'Bearer ') + token }, perMessageDeflate: false });
    sockets.push(ws); ws.messages = [];
    ws.on('message', bytes => ws.messages.push(JSON.parse(bytes.toString()))); ws.on('error', () => {});
    ws.closed = new Promise(resolve => ws.once('close', (code, reason) => resolve({ code, reason: reason.toString() })));
    await new Promise((resolve, reject) => { ws.once('open', resolve); ws.once('error', reject); ws.once('unexpected-response', (_req, res) => { res.resume(); reject(Object.assign(Error('upgrade refused'), { status: res.statusCode })); }); });
    return ws;
  }
  const rawLogin = () => call('/api/v1/auth/login', { method: 'POST', headers: policy, data: { username: 'alice', password } });
  const confirm = async offer => {
    assert.equal(offer.status, 409); assert.equal(offer.body.error.code, 'login_confirmation_required');
    const result = await call('/api/v1/auth/login/confirm', { method: 'POST', headers: policy, data: { challengeToken: offer.body.error.details.challengeToken, confirm: true } });
    assert.equal(result.status, 200, result.body?.error?.code); return result.body.sessionToken;
  };
  // The explicit name makes every replacement opt-in at its test call site;
  // rawLogin never auto-confirms and remains usable for no-eviction controls.
  const confirmedLogin = async () => { const offer = await rawLogin(); return offer.status === 200 ? offer.body.sessionToken : confirm(offer); };
  let independent = null;
  if (browserFirst) {
    independent = await call('/console/api/login', { method: 'POST', headers: policy, data: { username: 'alice', password } });
    assert.equal(independent.status, 200); assert.ok(independent.cookie);
    assert.equal(store.db.prepare("SELECT purpose FROM sessions WHERE issuer='browser-console'").get().purpose, 'client');
  }
  const initial = await rawLogin();
  let a;
  if (browserFirst) {
    assert.equal(initial.status, 409);
    assert.equal((await call('/console/api/session', { headers: { Cookie: independent.cookie } })).status, 200, 'browser remains valid before explicit yes');
    a = await confirm(initial);
  } else { assert.equal(initial.status, 200); a = initial.body.sessionToken; }
  const host = await open('/api/v1/hosts/channel', hostToken, true);
  async function advertise() {
    const count = host.messages.filter(value => value.type === 'host.capabilities.ack').length;
    host.send(JSON.stringify({ type: 'host.capabilities', capabilities: caps }));
    await until(() => host.messages.filter(value => value.type === 'host.capabilities.ack').length > count);
    assert.equal(host.messages.filter(value => value.type === 'host.capabilities.ack').at(-1).accepted, true);
  }
  await advertise();
  const ticket = token => call('/api/v1/desktops/pve-149/connect', { method: 'POST', token, data: { mode: 'stream' } });
  return { store, server, call, open, rawLogin, confirmedLogin, a, independent, host, ticket, advertise, common, base, errors };
}

test('Replacement ends only old stream/watch and preserves exact Host stop/display readiness fence', { timeout: 15000 }, async t => {
  const f = await fixture(t), watch = await f.open('/api/v1/events', f.a);
  await until(() => watch.messages.some(value => value.type === 'desktops.ready'));
  const ticket = await f.ticket(f.a); assert.equal(ticket.status, 200);
  const client = await f.open('/api/v1/sessions/' + ticket.body.sessionId, f.a);
  await until(() => f.host.messages.some(value => value.type === 'session.start'));
  const offer = await f.rawLogin();
  assert.equal(offer.status, 409); assert.equal(offer.body.error.code, 'login_confirmation_required');
  assert.equal(f.host.messages.some(value => value.type === 'session.stop'), false, 'asking for confirmation does not stop an active stream');
  assert.equal((await f.call('/api/v1/auth/me', { token: f.a })).status, 200);
  const b = await f.confirmedLogin();
  await until(() => f.host.messages.some(value => value.type === 'session.stop' && value.sessionId === ticket.body.sessionId));
  const stop = f.host.messages.find(value => value.type === 'session.stop' && value.sessionId === ticket.body.sessionId);
  assert.equal(stop.reason, 'pc_login_replaced'); assert.equal(stop.sessionStopProtocol, 'stop-v1');
  assert.ok(f.host.messages.some(value => value.type === 'input.reset' && value.sessionId === ticket.body.sessionId));
  assert.equal((await watch.closed).reason, 'pc_login_replaced');
  await client.closed; assert.equal(client.messages.find(value => value.type === 'session.ended').reason, 'pc_login_replaced');
  assert.equal((await f.ticket(b)).body.error.code, 'session_cleanup_pending');
  f.host.send(JSON.stringify({ type: 'host.session.stopped', sessionId: stop.sessionId, sessionStopProtocol: 'stop-v1', stopRequestId: stop.stopRequestId,
    workerQuiescent: true, profileRequired: true, profileRestored: false }));
  await until(() => f.host.messages.some(value => value.type === 'host.session.stop.status'));
  assert.equal((await f.ticket(b)).body.error.code, 'session_cleanup_pending');
  f.host.send(JSON.stringify({ type: 'host.session.stopped', sessionId: stop.sessionId, sessionStopProtocol: 'stop-v1', stopRequestId: stop.stopRequestId,
    workerQuiescent: true, profileRequired: true, profileRestored: true }));
  await new Promise(resolve => setTimeout(resolve, 20));
  assert.equal((await f.ticket(b)).body.error.code, 'session_cleanup_pending', 'completion alone is insufficient');
  await f.advertise(); assert.equal((await f.ticket(b)).status, 200); assert.equal(f.errors.length, 0);
});

test('An unspent old ticket cannot attach after replacement and does not block successor', async t => {
  const f = await fixture(t), old = await f.ticket(f.a); assert.equal(old.status, 200);
  const b = await f.confirmedLogin();
  await assert.rejects(f.open('/api/v1/sessions/' + old.body.sessionId, f.a), error => error.status === 401);
  assert.equal(f.host.messages.some(value => value.type === 'session.start'), false);
  assert.equal((await f.ticket(b)).status, 200); assert.equal(f.errors.length, 0);
});

test('A connect body already waiting at bearer admission cannot issue a retired ticket', async t => {
  const f = await fixture(t), payload = JSON.stringify({ mode: 'stream' });
  let admittedResolve; const admitted = new Promise(resolve => { admittedResolve = resolve; });
  const authenticate = f.store.authenticate;
  f.store.authenticate = function (...args) {
    const current = authenticate.apply(this, args);
    if (args[0] === tokenHash(f.a)) { f.store.authenticate = authenticate; admittedResolve(); }
    return current;
  };
  let writer;
  const response = new Promise((resolve, reject) => {
    writer = request(f.base + '/api/v1/desktops/pve-149/connect', { method: 'POST', headers: { ...f.common, Authorization: 'Bearer ' + f.a,
      'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(payload) } }, res => {
      const chunks = []; res.on('data', bytes => chunks.push(bytes)); res.on('end', () => resolve({ status: res.statusCode, body: JSON.parse(Buffer.concat(chunks).toString()) }));
    });
    writer.on('error', reject); writer.setTimeout(5000, () => writer.destroy(Error('fixture timeout'))); writer.write('{');
  });
  void response.catch(() => {}); t.after(() => writer.destroy());
  await admitted; const b = await f.confirmedLogin(); writer.end(payload.slice(1));
  const result = await response; assert.equal(result.status, 401); assert.equal(result.body.error.code, 'pc_login_replaced');
  assert.equal(f.host.messages.some(value => value.type === 'session.start'), false);
  assert.equal((await f.ticket(b)).status, 200); assert.equal(f.errors.length, 0);
});

test('Independent browser is client-purpose and requires confirmation; derived console ends only with its confirmed parent replacement', async t => {
  const f = await fixture(t, { browserFirst: true });
  assert.equal((await f.call('/console/api/session', { headers: { Cookie: f.independent.cookie } })).status, 401, 'explicit Windows confirmation retires browser client authority too');
  const handoff = await f.call('/api/v1/desktops/pve-149/console-webview', { method: 'POST', token: f.a, data: {} }); assert.equal(handoff.status, 200);
  const child = await f.call(handoff.body.path, { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, raw: 'code=' + handoff.body.code });
  assert.equal(child.status, 303); assert.ok(child.cookie);
  assert.equal((await f.call('/console/api/session', { headers: { Cookie: child.cookie } })).status, 200);
  const pendingBrowser = await f.call('/console/api/login', { method: 'POST', headers: policy, data: { username: 'alice', password } });
  assert.equal(pendingBrowser.status, 409); assert.equal(pendingBrowser.body.error.code, 'login_confirmation_required');
  assert.equal((await f.call('/console/api/session', { headers: { Cookie: child.cookie } })).status, 200, 'an unconfirmed browser login does not close the derived viewer');
  await f.confirmedLogin();
  assert.equal((await f.call('/console/api/session', { headers: { Cookie: child.cookie } })).status, 401);
  const stale = await f.call('/api/v1/auth/login/confirm', { method: 'POST', headers: policy, data: { challengeToken: pendingBrowser.body.error.details.challengeToken, confirm: true } });
  assert.equal(stale.status, 409); assert.equal(stale.body.error.code, 'login_confirmation_changed');
  assert.equal(f.errors.length, 0);
});
