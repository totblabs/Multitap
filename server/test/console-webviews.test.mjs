// Real loopback HTTP/WebSocket, memory Store and synthetic PVE sockets only.
// No production credentials, desktop, device, WebView installation or input.
import test from 'node:test';
import assert from 'node:assert/strict';
import { createServer, request } from 'node:http';
import { once, EventEmitter } from 'node:events';
import { randomBytes } from 'node:crypto';
import WebSocket from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { consoleCookieName } from '../src/browser-console.mjs';
import { ConsoleWebviews, consoleWebViewProtocol } from '../src/console-webviews.mjs';
import { normalizeServerAccess } from '../update/server-access-policy.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { enrollFixtureHosts } from './fixtures/admin-assignment-input.mjs';

const origin = 'https://webview-tests.invalid';
const machine = id => '12345678-1234-1234-1234-' + String(id).padStart(12, '0');
const pause = async () => { for (let i = 0; i < 25; i++) await Promise.resolve(); };
async function collected(relay) {
  assert.ok([...relay.console.grants.values()].every(grant => grant.closed), 'authorization is already retired before waiting for transport cleanup');
  for (let i = 0; i < 100 && relay.console.grants.size; i++) await new Promise(resolve => setTimeout(resolve, 5));
  assert.equal(relay.console.grants.size, 0, 'actual server-side transport closure releases its retained cleanup owner');
}
class Wire extends EventEmitter {
  readyState = 1; sent = [];
  send(bytes, _options, done) { this.sent.push(JSON.parse(String(bytes))); done?.(); }
  terminate() { if (this.readyState === 3) return; this.readyState = 3; this.emit('close'); }
  close() { this.terminate(); }
  pause() {} resume() {} ping() {}
  packet(value) { this.emit('message', Buffer.from(JSON.stringify(value)), false); }
}
async function fixture(t, { childMs = 3600000, authorizationMs = 3600000 } = {}) {
  const reservation = createServer(); await new Promise(resolve => reservation.listen(0, '127.0.0.1', resolve));
  const port = reservation.address().port; await new Promise(resolve => reservation.close(resolve));
  const access = normalizeServerAccess({ protocol: 'server-access-v1', primaryOrigin: origin,
    https: { enabled: true, origin, mode: 'proxy', listenHost: '127.0.0.1', listenPort: port },
    http: { enabled: false, origin: null, listenHost: '127.0.0.1', listenPort: port }, trustedProxies: ['127.0.0.1'] });
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32)), tokens = {}, clock = { time: Date.now() };
  for (const name of ['alice', 'bobby']) store.addUser({ name, passwordHash: 'synthetic-only' });
  for (const [key, name] of [['alice', 'alice'], ['other', 'alice'], ['bobby', 'bobby']]) {
    tokens[key] = newToken(); store.createSession(store.findUser(name), tokenHash(tokens[key]), Date.now(), 3600000, 'client');
  }
  store.syncProxmox([149, 150].map(vmid => ({ vmid, node: 'fixture', name: 'Memory VM', status: 'running', machineUuid: machine(vmid) })), 'fixture');
  for (const name of ['alice', 'bobby']) for (const id of ['pve-149', 'pve-150']) store.assign(name, id);
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://memory.invalid', tokenId: 'fixture@pve!fixture', secret: 'SYNTHETIC_ONLY' }), 'fixture');
  const proxmox = { consoleSnapshot: async (_config, vmid) => ({ node: 'fixture', vmid, machineUuid: machine(vmid) }),
    openConsole: async () => ({ socket: new Wire(), password: 'SYNTHETIC_VNC_ONLY' }) };
  const server = await buildServer({ store, vault, proxmox, publicOrigin: origin, accessConfig: access, trustedProxies: access.trustedProxies,
    initialSetupOptions: { announce: () => {} }, browserConsoleOptions: { webviewOptions: { now: () => clock.time, childMs } },
    relayOptions: { authorizationMs, heartbeatMs: 3600000, consoleOptions: { now: () => clock.time }, qemuConsoleOptions: { now: () => clock.time } },
    logger: { error: value => assert.fail(JSON.stringify(value)) } });
  await new Promise(resolve => server.listen(port, '127.0.0.1', resolve));
  t.after(async () => { server.closeAllConnections(); await new Promise(resolve => server.close(resolve)); store.close(); });
  function call(path, { method = 'GET', data, form, as, cookie, csrf, browserOrigin = origin, proxy = true, headers = {} } = {}) {
    return new Promise((resolve, reject) => {
      const bytes = form !== undefined ? Buffer.from(form) : data !== undefined ? Buffer.from(JSON.stringify(data)) : null;
      const req = request({ hostname: '127.0.0.1', port, path, method, headers: { Host: new URL(origin).host,
        ...(proxy ? { 'X-Forwarded-Proto': 'https' } : {}), ...(browserOrigin ? { Origin: browserOrigin } : {}),
        ...(as ? { Authorization: 'Bearer ' + tokens[as] } : {}), ...(cookie ? { Cookie: cookie } : {}), ...(csrf ? { 'X-Multitap-Console-CSRF': csrf } : {}),
        ...(bytes ? { 'Content-Type': form !== undefined ? 'application/x-www-form-urlencoded' : 'application/json', 'Content-Length': bytes.length } : {}), ...headers } }, res => {
        const chunks = []; res.on('data', bytes => chunks.push(bytes)); res.on('end', () => { const text = Buffer.concat(chunks).toString(); resolve({ status: res.statusCode, headers: res.headers, text, body: text && res.headers['content-type']?.startsWith('application/json') ? JSON.parse(text) : null }); });
      }); req.on('error', reject); req.end(bytes);
    });
  }
  const issue = (id = 'pve-149', as = 'alice', extra = {}) => call('/api/v1/desktops/' + id + '/console-webview', { method: 'POST', data: {}, as, ...extra });
  const attach = (reply, extra = {}) => call(reply.body.path, { method: 'POST', form: 'code=' + reply.body.code, ...extra });
  async function child(id = 'pve-149', as = 'alice') {
    const issued = await issue(id, as); assert.equal(issued.status, 200);
    const response = await attach(issued); assert.equal(response.status, 303);
    const cookie = response.headers['set-cookie'][0].split(';')[0];
    const session = await call('/console/api/session', { cookie }); assert.equal(session.status, 200);
    return { issued, response, cookie, csrf: session.body.csrfToken, session };
  }
  const connect = (auth, id = 'pve-149') => call('/console/api/desktops/' + id + '/connect', { method: 'POST', data: {}, cookie: auth.cookie, csrf: auth.csrf });
  function socket(id, auth) { return new WebSocket(`ws://127.0.0.1:${port}/console/ws/${id}`, { headers: { Host: new URL(origin).host, 'X-Forwarded-Proto': 'https', Origin: origin, Cookie: auth.cookie } }); }
  return { store, server, relay: server.relay, tokens, clock, call, issue, attach, child, connect, socket };
}

test('Native login hands off through one-use POST into a scoped secure cookie without a second login or Store session', async t => {
  const f = await fixture(t), before = f.store.db.prepare('SELECT count(*) count FROM sessions').get().count;
  enrollFixtureHosts(f.store,['pve-149']);
  const list = await f.call('/api/v1/desktops', { as: 'alice' });
  assert.equal(list.body.desktops[0].consoleWebViewProtocol, consoleWebViewProtocol);
  const child = await f.child(); const body = child.issued.body;
  assert.deepEqual(Object.keys(body).sort(), ['code', 'expiresAt', 'path', 'protocol']); assert.equal(body.protocol, consoleWebViewProtocol);
  assert.equal(body.path, '/console/pve-149/attach'); assert.match(body.code, /^[A-Za-z0-9_-]{43}$/);
  assert.equal(child.response.headers.location, '/console/pve-149'); assert.equal(child.response.text, '');
  assert.equal(child.response.headers['cache-control'], 'no-store'); assert.equal(child.response.headers['referrer-policy'], 'no-referrer');
  assert.match(child.response.headers['set-cookie'][0], new RegExp('^' + consoleCookieName(origin) + '=[A-Za-z0-9_-]{43}; Path=/console; HttpOnly; Secure; SameSite=Strict$'));
  assert.equal(JSON.stringify(child.response.headers).includes(f.tokens.alice), false); assert.equal(JSON.stringify(child.response.headers).includes(body.code), false);
  assert.equal(f.store.db.prepare('SELECT count(*) count FROM sessions').get().count, before);
  assert.equal(child.session.body.consoleWebView.desktopId, 'pve-149'); assert.equal(child.session.body.user.username, 'alice');
  assert.equal((await f.attach(child.issued)).status, 403, 'the exact handoff cannot be replayed');
  assert.equal((await f.connect(child)).status, 200);
  assert.ok(f.relay.protectedSessionHashes(f.store.findUser('alice').id).has(tokenHash(f.tokens.alice)), 'a live derived console protects the exact parent from session-count eviction without renewing it');
});

test('Issuance requires exact native authority, assigned VM, HTTPS and an empty bounded JSON body', async t => {
  const f = await fixture(t);
  assert.equal((await f.issue('pve-149', undefined, { as: undefined })).status, 401);
  assert.equal((await f.issue('pve-149', 'alice', { proxy: false })).status, 403);
  assert.equal((await f.issue('pve-149', 'alice', { data: { desktopId: 'pve-150' } })).status, 400);
  assert.equal((await f.issue('pve-149', 'alice', { browserOrigin: 'https://foreign.invalid' })).status, 403);
  f.store.revoke('alice', 'pve-149'); assert.equal((await f.issue()).status, 404);
  assert.equal((await f.issue('pve-150', 'alice', { browserOrigin: null })).status, 200, 'native bearer transport needs no browser Origin');
});

test('Attach rejects wrong Origin, VM, method, format, query, oversized and expired codes without minting authority', async t => {
  const f = await fixture(t), code = await f.issue();
  for (const extra of [{ browserOrigin: null }, { browserOrigin: 'https://foreign.invalid' }, { proxy: false }]) assert.equal((await f.attach(code, extra)).status, 403);
  assert.equal((await f.call('/console/pve-150/attach', { method: 'POST', form: 'code=' + code.body.code })).status, 403);
  assert.equal((await f.call(code.body.path, { method: 'GET' })).status, 405);
  assert.equal((await f.call(code.body.path, { method: 'POST', data: { code: code.body.code } })).status, 415);
  assert.equal((await f.call(code.body.path + '?code=' + code.body.code, { method: 'POST', form: 'code=' + code.body.code })).status, 400);
  assert.equal((await f.call(code.body.path, { method: 'POST', form: 'code=' + code.body.code + '&extra=1' })).status, 400);
  assert.equal((await f.call(code.body.path, { method: 'POST', form: 'code=' + 'a'.repeat(100) })).status, 413);
  assert.equal((await f.attach(code)).status, 303, 'foreign attempts did not spend the legitimate owner code');
  const next = await f.issue(); f.clock.time += 30001; assert.equal((await f.attach(next)).status, 403);
});

test('A child cannot enter another VM, use native/admin APIs, or manage account factors', async t => {
  const f = await fixture(t), child = await f.child();
  assert.equal((await f.connect(child, 'pve-150')).status, 404);
  assert.equal((await f.call('/console/pve-150', { cookie: child.cookie })).status, 404);
  // The deployed schema16 baseline has no account-factor page.
  assert.equal((await f.call('/console/account/security', { cookie: child.cookie })).status, 404);
  assert.ok([403, 404].includes((await f.call('/console/api/mfa', { cookie: child.cookie })).status));
  assert.ok([403, 404].includes((await f.call('/console/api/mfa/setup', { method: 'POST', data: {}, cookie: child.cookie, csrf: child.csrf })).status));
  assert.equal((await f.call('/api/v1/desktops', { cookie: child.cookie })).status, 401);
  const raw = child.cookie.split('=')[1]; assert.equal((await f.call('/api/v1/desktops', { headers: { Authorization: 'Bearer ' + raw } })).status, 401);
  assert.equal((await f.call('/console/api/session', { cookie: child.cookie, browserOrigin: 'https://foreign.invalid' })).status, 403);
});

test('Child reads never renew its parent and fixed child expiry retires an already issued console grant', async t => {
  const f = await fixture(t, { childMs: 1000 }), child = await f.child();
  const parentHash = tokenHash(f.tokens.alice), before = f.store.authenticate(parentHash).expires_at;
  const connected = await f.connect(child); assert.equal(connected.status, 200); assert.equal(f.relay.console.grants.size, 1);
  assert.equal((await f.call('/console/api/session', { cookie: child.cookie })).status, 200); assert.equal(f.store.authenticate(parentHash).expires_at, before);
  f.clock.time += 1001; f.relay.revalidate(); assert.equal(f.relay.console.grants.size, 0);
  assert.equal((await f.call('/console/api/session', { cookie: child.cookie })).status, 401); assert.equal(f.store.authenticate(parentHash).expires_at, before);
});

for (const revoke of ['logout', 'assignment', 'disabled', 'expired']) test(`Parent ${revoke} revokes an attached WebSocket and every derived grant`, async t => {
  const f = await fixture(t), child = await f.child(), grant = await f.connect(child);
  const ws = f.socket(grant.body.id, child); await once(ws, 'open', { signal: AbortSignal.timeout(2000) }); const closed = once(ws, 'close', { signal: AbortSignal.timeout(2000) });
  if (revoke === 'logout') await f.call('/api/v1/auth/logout', { method: 'POST', data: {}, as: 'alice' });
  if (revoke === 'assignment') f.store.revoke('alice', 'pve-149');
  if (revoke === 'disabled') f.store.setUserEnabled('alice', false, 'fixture');
  if (revoke === 'expired') f.store.db.prepare('UPDATE sessions SET expires_at=? WHERE token_hash=?').run(Date.now() - 1, tokenHash(f.tokens.alice));
  f.relay.revalidate(); await closed; await collected(f.relay);
  assert.equal(f.relay.console.grants.size, 0); assert.equal((await f.call('/console/api/session', { cookie: child.cookie })).status, 401);
  assert.ok(f.store.authenticate(tokenHash(f.tokens.bobby)), 'an unrelated account session is unchanged');
});

test('The normal authorization sweep closes an expired child socket without a later HTTP request', async t => {
  const f = await fixture(t, { childMs: 1000, authorizationMs: 10 }), child = await f.child(), grant = await f.connect(child);
  const ws = f.socket(grant.body.id, child); await once(ws, 'open', { signal: AbortSignal.timeout(2000) });
  const closed = once(ws, 'close', { signal: AbortSignal.timeout(2000) });
  f.clock.time += 1001; await closed; await collected(f.relay);
  assert.ok(f.store.authenticate(tokenHash(f.tokens.alice)), 'the launcher login is still valid');
});

test('Scoped close retires only the same parent/VM/origin children and unused codes', async t => {
  const f = await fixture(t), first = await f.child(), second = await f.child('pve-150'), other = await f.child('pve-149', 'other');
  const grant = await f.connect(first), ws = f.socket(grant.body.id, first); await once(ws, 'open', { signal: AbortSignal.timeout(2000) });
  const socketClosed = once(ws, 'close', { signal: AbortSignal.timeout(2000) });
  const pendingCode = await f.issue();
  const closed = await f.call('/api/v1/desktops/pve-149/console-webviews/close', { method: 'POST', data: {}, as: 'alice' });
  await socketClosed; await collected(f.relay);
  assert.deepEqual(closed.body, { closed: true }); assert.equal((await f.call('/console/api/session', { cookie: first.cookie })).status, 401);
  assert.equal((await f.attach(pendingCode)).status, 403);
  for (const child of [second, other]) assert.equal((await f.call('/console/api/session', { cookie: child.cookie })).status, 200);
  assert.ok(f.store.authenticate(tokenHash(f.tokens.alice)));
  assert.equal((await f.call('/api/v1/console-webviews/close', { method: 'POST', data: {}, as: 'alice' })).status, 200);
  assert.equal((await f.call('/console/api/session', { cookie: second.cookie })).status, 401);
  assert.equal((await f.call('/console/api/session', { cookie: other.cookie })).status, 200);
});

test('Child logout and child replacement never log out the launcher or an independent browser login', async t => {
  const f = await fixture(t), first = await f.child(), ordinaryCookie = consoleCookieName(origin) + '=' + f.tokens.other;
  const issued = await f.issue(), replaced = await f.attach(issued, { cookie: first.cookie }); assert.equal(replaced.status, 303);
  assert.equal((await f.call('/console/api/session', { cookie: first.cookie })).status, 401);
  assert.ok(f.store.authenticate(tokenHash(f.tokens.alice))); assert.ok(f.store.authenticate(tokenHash(f.tokens.other)));
  const next = await f.issue(), independent = await f.attach(next, { cookie: ordinaryCookie }); assert.equal(independent.status, 303);
  assert.ok(f.store.authenticate(tokenHash(f.tokens.other)), 'replacing cookie does not revoke independent browser authority');
  const cookie = independent.headers['set-cookie'][0].split(';')[0], session = await f.call('/console/api/session', { cookie });
  assert.equal((await f.call('/console/api/logout', { method: 'POST', data: {}, cookie, csrf: session.body.csrfToken })).status, 204);
  assert.ok(f.store.authenticate(tokenHash(f.tokens.alice))); assert.ok(f.store.authenticate(tokenHash(f.tokens.other)));
});

test('A late exact-owner close cannot revoke a fresh handoff or child for the same parent and VM', async t => {
  const f = await fixture(t), old = await f.child(), oldOwner = tokenHash(old.issued.body.code), fresh = await f.issue();
  const path = '/api/v1/desktops/pve-149/console-webviews/close';
  const close = (ownerId, extra = {}) => f.call(path, { method: 'POST', data: { ownerId }, as: 'alice', ...extra });
  assert.equal((await close(oldOwner)).status, 200);
  assert.equal((await f.call('/console/api/session', { cookie: old.cookie })).status, 401);
  const attached = await f.attach(fresh); assert.equal(attached.status, 303, 'a late old close preserves the newer unused code');
  const cookie = attached.headers['set-cookie'][0].split(';')[0], session = await f.call('/console/api/session', { cookie });
  const current = { cookie, csrf: session.body.csrfToken }, grant = await f.connect(current); assert.equal(grant.status, 200);
  const newOwner = tokenHash(fresh.body.code);
  for (const response of [await close(oldOwner), await close('a'.repeat(64)), await close(newOwner, { as: 'bobby' }),
    await f.call('/api/v1/desktops/pve-150/console-webviews/close', { method: 'POST', data: { ownerId: newOwner }, as: 'alice' })]) assert.equal(response.status, 200);
  assert.equal((await close(newOwner, { browserOrigin: 'https://foreign.invalid' })).status, 403);
  assert.equal((await f.call('/console/api/session', { cookie })).status, 200); assert.equal(f.relay.console.grants.size, 1);
  assert.equal((await close(newOwner)).status, 200); assert.equal((await f.call('/console/api/session', { cookie })).status, 401);
  assert.equal(f.relay.console.grants.size, 0); assert.ok(f.store.authenticate(tokenHash(f.tokens.alice)));
});

test('Native exact-owner close accepts only a lowercase SHA256 owner and keeps all-parent body empty', async t => {
  const f = await fixture(t), child = await f.child(), path = '/api/v1/desktops/pve-149/console-webviews/close';
  for (const data of [{ ownerId: null }, { ownerId: 123 }, { ownerId: 'A'.repeat(64) }, { ownerId: 'a'.repeat(63) }, { ownerId: 'a'.repeat(64), extra: true }]) {
    assert.equal((await f.call(path, { method: 'POST', data, as: 'alice' })).status, 400);
  }
  assert.equal((await f.call('/api/v1/console-webviews/close', { method: 'POST', data: { ownerId: tokenHash(child.issued.body.code) }, as: 'alice' })).status, 400);
  assert.equal((await f.call('/console/api/session', { cookie: child.cookie })).status, 200);
});

test('Parent revocation cancels an in-flight QEMU preparation while retaining its worker cleanup lock', async t => {
  const f = await fixture(t), child = await f.child(), enrollment = newToken(), hostToken = newToken();
  f.store.issueEnrollment('pve-149', machine(149), tokenHash(enrollment), 'fixture');
  f.store.consumeEnrollment(tokenHash(enrollment), machine(149), { hostname: 'SyntheticHost', version: '9.0.0', tokenHash: tokenHash(hostToken) });
  const wire = new Wire(); f.relay.attachHost(wire, f.store.authenticateHost(tokenHash(hostToken)), tokenHash(hostToken), true);
  wire.packet({ type: 'host.capabilities', capabilities: { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true, qemuConsoleProtocol: 'qemu-console-v2' } });
  f.relay.proxmox.consoleSnapshot = async (_config, vmid) => ({ node: 'fixture', vmid, machineUuid: machine(vmid), model: 'std' });
  const connecting = f.connect(child); let request;
  for (let i = 0; i < 100; i++) { request = wire.sent.find(value => value.type === 'host.qemu-console.prepare'); if (request) break; await new Promise(resolve => setTimeout(resolve, 5)); }
  assert.ok(request); assert.equal(f.relay.displayBusy.size, 1);
  await f.call('/api/v1/auth/logout', { method: 'POST', data: {}, as: 'alice' });
  assert.equal((await connecting).status, 409); assert.equal(f.relay.displayBusy.size, 1);
  wire.packet({ type: 'host.qemu-console.result', protocol: 'qemu-console-v2', operationId: request.operationId, nonce: request.nonce, ok: false, attempted: false, workerQuiescent: true, reason: 'qemu_console_cancelled' });
  await pause(); assert.equal(f.relay.displayBusy.size, 0); assert.equal(f.relay.console.grants.size, 0);
});

test('In-memory origin scoping, issuance bounds and exact parent expiry are enforced without Store writes', async t => {
  const store = new Store(':memory:'); t.after(() => store.close());
  store.addUser({ name: 'alice', passwordHash: 'synthetic-only' }); store.syncProxmox([{ vmid: 149, node: 'fixture', name: 'VM', status: 'running', machineUuid: machine(149) }], 'fixture'); store.assign('alice', 'pve-149');
  const token = newToken(), hash = tokenHash(token); store.createSession(store.findUser('alice'), hash, Date.now(), 10000, 'client');
  const manager = new ConsoleWebviews(store), user = store.authenticate(hash), issued = manager.create(user, hash, 'pve-149', origin), child = manager.consume(issued.code, 'pve-149', origin);
  assert.equal(manager.authenticate(child.hash, { origin: 'https://other.invalid' }), null);
  assert.ok(manager.authenticate(child.hash, { origin, desktopId: 'pve-149' })); assert.equal(manager.authenticate(child.hash, { origin, desktopId: 'pve-150' }), null);
  assert.ok(child.expiresAt <= user.expires_at);
  for (let i = 0; i < 9; i++) manager.create(user, hash, 'pve-149', origin);
  assert.throws(() => manager.create(user, hash, 'pve-149', origin), error => error.code === 'console_webview_limit');
  assert.equal(manager.codes.size, 1); assert.equal(store.db.prepare('SELECT count(*) count FROM sessions').get().count, 1);
});

test('Derived preparation reads are scoped to the exact child and VM, and never renew the parent or retire grants', async t => {
  const f = await fixture(t, { childMs: 10000 }), child = await f.child(), foreignChild = await f.child('pve-150'), otherParent = await f.child('pve-149', 'other');
  const issued = await f.connect(child), grant = f.relay.console.grants.get(issued.body.id), path = `/console/api/desktops/pve-149/sessions/${issued.body.id}`;
  const sessions = f.store.db.prepare('SELECT * FROM sessions ORDER BY token_hash').all();
  f.server.relay.proxmox.consoleSnapshot = async () => assert.fail('a derived preparation read cannot query PVE');
  assert.deepEqual((await f.call(path, { cookie: child.cookie })).body, { qemuConsolePreparation: { status: 'unknown' } });
  assert.equal((await f.call(path, { cookie: foreignChild.cookie })).status, 404);
  assert.equal((await f.call(path, { cookie: otherParent.cookie })).status, 404);
  f.clock.time += 10001;
  assert.equal((await f.call(path, { cookie: child.cookie })).status, 401);
  assert.deepEqual(f.store.db.prepare('SELECT * FROM sessions ORDER BY token_hash').all(), sessions);
  assert.equal(grant.closed, false); assert.equal(f.relay.console.grants.get(grant.id), grant);
});

test('Read-only derived authentication observes expiry and parent revocation without deleting or renewing authority', t => {
  const store = new Store(':memory:'); t.after(() => store.close());
  store.addUser({ name: 'alice', passwordHash: 'synthetic-only' });
  store.syncProxmox([{ vmid: 149, node: 'fixture', name: 'VM', status: 'running', machineUuid: machine(149) }], 'fixture'); store.assign('alice', 'pve-149');
  const token = newToken(), hash = tokenHash(token); store.createSession(store.findUser('alice'), hash, Date.now(), 10000, 'client');
  const clock = { time: Date.now() }, manager = new ConsoleWebviews(store, { now: () => clock.time, childMs: 1000 }), user = store.authenticate(hash);
  const issue = () => manager.consume(manager.create(user, hash, 'pve-149', origin).code, 'pve-149', origin);
  const expired = issue(), expiredBinding = manager.children.get(expired.hash); clock.time += 1001;
  assert.equal(manager.authenticate(expired.hash, { origin, readOnly: true }), null);
  assert.equal(manager.children.get(expired.hash), expiredBinding);
  assert.equal(manager.authenticate(expired.hash, { origin }), null); assert.equal(manager.children.has(expired.hash), false, 'normal cleanup is preserved');
  const revoked = issue(), revokedBinding = manager.children.get(revoked.hash); store.logout(hash);
  assert.equal(manager.authenticate(revoked.hash, { origin, readOnly: true }), null);
  assert.equal(manager.children.get(revoked.hash), revokedBinding);
  assert.equal(manager.authenticate(revoked.hash), null); assert.equal(manager.children.has(revoked.hash), false);
});
