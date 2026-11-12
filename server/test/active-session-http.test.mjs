// Full actual HTTP handler without listen/connect: request/response are memory
// EventEmitters. Only the source static assets are read by buildServer.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { randomBytes } from 'node:crypto';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword, tokenHash, newToken } from '../src/security.mjs';

const password = 'synthetic-login-capacity-only-42!';
async function fixture(t) {
  const store = new Store(':memory:');
  store.addUser({ name: 'primary', role: 'admin', passwordHash: 'memory-primary-only' });
  store.addUser({ name: 'alice', role: 'admin', passwordHash: await hashPassword(password) });
  const machine = vmid => '12345678-1234-1234-1234-' + String(vmid).padStart(12, '0');
  store.syncProxmox([149, 150, 151, 152, 153].map(vmid => ({ vmid, node: 'memory', name: 'Memory VM', status: 'running', machineUuid: machine(vmid) })), 'fixture');
  for (let vmid = 149; vmid < 154; ++vmid) store.assign('alice', 'pve-' + vmid);
  const server = await buildServer({ store, vault: new Vault(randomBytes(32)), publicOrigin: 'https://capacity.invalid', initialSetupOptions: { announce: () => {} },
    relayOptions: { authorizationMs: 3600000, heartbeatMs: 3600000 }, logger: { error: value => assert.fail('Unexpected fixed handler error: ' + value) } });
  t.after(async () => {
    await new Promise(resolve => server.close(() => resolve())); server.relay.close(); store.close();
  });
  assert.equal(server.listening, false);
  async function call(path, input) {
    return new Promise(resolve => {
      const bytes = Buffer.from(JSON.stringify(input)), req = new EventEmitter(), headers = new Map();
      Object.assign(req, { method: 'POST', url: path, headers: { 'content-type': 'application/json', 'content-length': String(bytes.length), origin: 'https://capacity.invalid' },
        rawHeaders: ['Content-Type', 'application/json', 'Origin', 'https://capacity.invalid'], socket: { remoteAddress: '127.0.0.1' }, resume() {} });
      const on = req.on.bind(req); let bodyScheduled = false;
      req.on = (event, listener) => { on(event, listener); if (event === 'data' && !bodyScheduled) { bodyScheduled = true; queueMicrotask(() => { req.emit('data', bytes); req.emit('end'); }); } return req; };
      const res = new EventEmitter(); Object.assign(res, { writableEnded: false, headersSent: false,
        setHeader(name, value) { headers.set(name.toLowerCase(), value); }, getHeader(name) { return headers.get(name.toLowerCase()); },
        writeHead(status, values) { this.statusCode = status; this.headersSent = true; for (const [key, value] of Object.entries(values)) this.setHeader(key, value); },
        end(body) { this.writableEnded = true; resolve({ status: this.statusCode, body: JSON.parse(String(body)), headers }); } });
      server.emit('request', req, res);
    });
  }
  const login = (admin = false, extra = {}) => call(admin ? '/api/v1/admin/login' : '/api/v1/auth/login', { username: 'alice', password, ...extra });
  function reserve(token, vmid) {
    const enrollment = newToken(), secret = newToken(), wire = new EventEmitter();
    Object.assign(wire, { readyState: 1, bufferedAmount: 0, send(_bytes, _options, done) { done?.(); }, ping() {},
      close() { if (this.readyState !== 3) { this.readyState = 3; this.emit('close'); } }, terminate() { this.close(); } });
    store.issueEnrollment('pve-' + vmid, machine(vmid), tokenHash(enrollment), 'fixture');
    store.consumeEnrollment(tokenHash(enrollment), machine(vmid), { hostname: 'MemoryHost', version: '9.0.0', tokenHash: tokenHash(secret) });
    server.relay.attachHost(wire, store.authenticateHost(tokenHash(secret)), tokenHash(secret), true);
    wire.emit('message', Buffer.from(JSON.stringify({ type: 'host.capabilities', capabilities: { service: true, interactiveSession: true,
      streaming: true, virtualDisplayReady: true, virtualMonitorControl: true,
      monitors: [{ id: '0', width: 1280, height: 720, primary: true, modes: [{ width: 1280, height: 720, refreshRate: 60 }] }] } })), false);
    return server.relay.connect(store.findUser('alice'), tokenHash(token), 'pve-' + vmid, 'stream',
      { maxWidth: 1280, maxHeight: 720, fps: 60, bitrate: 8000000, audio: false, monitor: '0' });
  }
  const seedClient = () => { const token = newToken(); assert.equal(store.createSession(store.findUser('alice'),tokenHash(token),Date.now(),3600000,'client'),true); return token; };
  const confirm = challengeToken => call('/api/v1/auth/login/confirm',{challengeToken,confirm:true});
  return { store, server, login, reserve, seedClient, confirm };
}

test('Actual client/admin login returns fixed409 at five live owners and emits no new bearer or success audit', async t => {
  const f = await fixture(t), tokens = [];
  for (let n = 0; n < 5; ++n) tokens.push(f.seedClient());
  const user = f.store.findUser('alice'), originalProtection = f.server.relay.protectedSessionHashes.bind(f.server.relay);
  // Actual Relay.connect creates five independent authorized pending tickets.
  // Hosts are EventEmitters and no native session.start/transport is invoked.
  for (let n = 0; n < tokens.length; ++n) assert.ok(f.reserve(tokens[n], 149 + n).sessionId);
  const rows = f.store.db.prepare('SELECT * FROM sessions ORDER BY rowid').all();
  const audits = f.store.db.prepare("SELECT count(*) AS n FROM audit_events WHERE event='login_succeeded'").get().n;
  for (const admin of [false, true]) {
    const response = await f.login(admin); assert.equal(response.status, 409); assert.equal(response.body.error.code, admin ? 'active_session_limit' : 'login_confirmation_required');
    assert.equal(Object.hasOwn(response.body, 'sessionToken'), false); assert.equal(response.headers.has('set-cookie'), false);
    for (const token of tokens) { assert.ok(f.store.authenticate(tokenHash(token))); assert.equal(JSON.stringify(response.body).includes(token), false); }
  }
  assert.equal(originalProtection(user.id).size, 5); assert.deepEqual(f.store.db.prepare('SELECT * FROM sessions ORDER BY rowid').all(), rows);
  assert.equal(f.store.db.prepare("SELECT count(*) AS n FROM audit_events WHERE event='login_succeeded'").get().n, audits);
});

test('Client input cannot choose protected hashes and only an explicit confirmation replaces the bounded client group', async t => {
  const f = await fixture(t), first = f.seedClient();
  const forged = await f.login(false, { protectedHashes: [tokenHash(first)] }); assert.equal(forged.status, 400); assert.equal(forged.body.error.code, 'unexpected_field');
  for (let n = 0; n < 4; ++n) f.seedClient();
  const pending=await f.login();assert.equal(pending.status,409);assert.equal(pending.body.error.code,'login_confirmation_required');
  assert.ok(f.store.authenticate(tokenHash(first)));assert.equal(f.store.db.prepare('SELECT count(*) n FROM sessions WHERE user_id=?').get(f.store.findUser('alice').id).n,5);
  assert.equal((await f.confirm(pending.body.error.details.challengeToken)).status,200);
  assert.equal(f.store.authenticate(tokenHash(first)), undefined);
  assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM sessions WHERE user_id=?').get(f.store.findUser('alice').id).n, 1);
});
