// Actual Store/Relay/ConsoleRelay, memory SQLite and owned EventEmitter wires.
// No listener, remote request, credential lookup, Windows API or fixture files.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { randomBytes, randomUUID } from 'node:crypto';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { Relay } from '../src/relay.mjs';
import { newToken, tokenHash } from '../src/security.mjs';

const settle = async () => { for (let n = 0; n < 20; ++n) await Promise.resolve(); };
const machine = vmid => '12345678-1234-1234-1234-' + String(vmid).padStart(12, '0');
class Wire extends EventEmitter {
  readyState = 1; messages = []; bufferedAmount = 0;
  send(bytes, options, done) { this.messages.push(options.binary ? Buffer.from(bytes) : JSON.parse(String(bytes))); done?.(); }
  close() { if (this.readyState === 3) return; this.readyState = 3; this.emit('close'); }
  terminate() { this.close(); }
  packet(value) { this.emit('message', Buffer.from(JSON.stringify(value)), false); }
  ping() {} pause() {} resume() {}
}
function fixture(t) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32)), now = Date.now();
  store.addUser({ name: 'primary', role: 'admin', passwordHash: 'memory-primary' });
  store.addUser({ name: 'alice', passwordHash: 'memory-alice' }); store.addUser({ name: 'bobby', passwordHash: 'memory-bobby' });
  store.syncProxmox([149, 150, 151, 152, 153, 154].map(vmid => ({ vmid, node: 'memory', name: 'Memory VM', status: 'running', machineUuid: machine(vmid) })), 'fixture');
  for (const name of ['alice', 'bobby']) for (let vmid = 149; vmid <= 154; ++vmid) store.assign(name, 'pve-' + vmid);
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://memory.invalid', tokenId: 'fixture@pve!fixture', secret: 'SYNTHETIC_ONLY' }), 'fixture');
  const proxmox = { consoleSnapshot: async (_config, vmid) => ({ node: 'memory', vmid, machineUuid: machine(vmid) }) };
  const relay = new Relay({ store, vault, proxmox, publicOrigin: 'https://broker.invalid', authorizationMs: 3600000, heartbeatMs: 3600000 });
  t.after(() => { relay.close(); store.close(); });
  const user = () => store.findUser('alice'); let serial = 0;
  function seed(name = 'alice', purpose = 'client') {
    const hash = tokenHash('synthetic-token-' + (++serial));
    assert.equal(store.createSession(store.findUser(name), hash, now + serial, 3600000, purpose), true); return hash;
  }
  const binding = (hash, vmid = 149, name = 'alice') => ({ id: randomUUID(), userId: store.findUser(name).id, tokenHash: hash, desktopId: 'pve-' + vmid });
  function login(protectedHashes, purpose = 'client') {
    const hash = tokenHash('synthetic-new-' + (++serial));
    const result = store.createSession(user(), hash, now + 1000 + serial, 3600000, purpose, { protectedHashes }); return { hash, result };
  }
  function host(vmid = 149) {
    const secret = newToken(), enrollment = newToken();
    store.issueEnrollment('pve-' + vmid, machine(vmid), tokenHash(enrollment), 'fixture');
    store.consumeEnrollment(tokenHash(enrollment), machine(vmid), { hostname: 'MemoryHost', version: '9.0.0', tokenHash: tokenHash(secret) });
    const wire = new Wire(); relay.attachHost(wire, store.authenticateHost(tokenHash(secret)), tokenHash(secret), true);
    wire.packet({ type: 'host.capabilities', capabilities: { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true,
      sessionStopProtocol: 'stop-v1', virtualMonitorControl: true, dynamicResolution: true,
      viewportProvisioning: true, resizeProtocol: 'epoch-v1', initialViewportProtocol: 'epoch-v1',
      monitors: [{ id: '0', width: 1280, height: 720, primary: true, modes: [{ width: 1280, height: 720, refreshRate: 60 }] }] } });
    assert.equal(wire.messages.at(-1).accepted, true, 'real capability parser accepts fixture'); return wire;
  }
  function ticket(hash) { host(); return relay.connect(user(), hash, 'pve-149', 'stream', { maxWidth: 1280, maxHeight: 720, fps: 60, bitrate: 8000000, audio: false, monitor: '0' }); }
  return { store, vault, now, relay, proxmox, user, seed, binding, login, ticket };
}

test('An oldest live token survives a sixth login while the oldest inactive token is evicted', t => {
  const f = fixture(t), hashes = Array.from({ length: 5 }, () => f.seed());
  // This Store boundary is also usable unchanged against the old implementation
  // (which ignores the extra server-owned argument and deletes hashes[0]).
  const issued = f.login(new Set([hashes[0]])); assert.equal(issued.result, true);
  assert.ok(f.store.authenticate(hashes[0], f.now + 2000)); assert.equal(f.store.authenticate(hashes[1], f.now + 2000), undefined);
  assert.ok(f.store.authenticate(issued.hash, f.now + 2000));
  assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM sessions WHERE user_id=?').get(f.user().id).n, 5);
});

test('All five live tokens refuse the new login atomically, preserving rows, expiry and success audit', t => {
  const f = fixture(t), hashes = Array.from({ length: 5 }, () => f.seed());
  const rows = f.store.db.prepare('SELECT * FROM sessions ORDER BY rowid').all();
  const audits = f.store.db.prepare("SELECT count(*) AS n FROM audit_events WHERE event='login_succeeded'").get().n;
  assert.throws(() => f.login(new Set(hashes)), error => error.code === 'active_session_limit' && error.message === 'active_session_limit');
  assert.deepEqual(f.store.db.prepare('SELECT * FROM sessions ORDER BY rowid').all(), rows);
  assert.equal(f.store.db.prepare("SELECT count(*) AS n FROM audit_events WHERE event='login_succeeded'").get().n, audits);
  for (const hash of hashes) assert.ok(f.store.authenticate(hash, f.now + 2000));
});

test('Administrator and client purposes retain the same total5 cap without evicting any live client', t => {
  const f = fixture(t), hashes = Array.from({ length: 5 }, () => f.seed());
  assert.throws(() => f.login(new Set(hashes), 'admin'), error => error.code === 'active_session_limit');
  const issued = f.login(new Set(hashes.slice(0, 4)), 'admin');
  assert.ok(f.store.authenticate(issued.hash, f.now + 2000, 'admin')); assert.equal(f.store.authenticate(issued.hash, f.now + 2000), undefined);
  for (const hash of hashes.slice(0, 4)) assert.ok(f.store.authenticate(hash, f.now + 2000));
  assert.equal(f.store.authenticate(hashes[4], f.now + 2000), undefined);
  assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM sessions WHERE user_id=?').get(f.user().id).n, 5);
});

test('Default Store callers preserve bounded inactive eviction and stale-password false return compatibility', t => {
  const f = fixture(t), first = f.seed(); for (let n = 0; n < 5; ++n) f.seed();
  assert.equal(f.store.authenticate(first, f.now + 2000), undefined);
  const stale = f.user(); f.store.db.prepare('UPDATE users SET password_hash=? WHERE id=?').run('changed-memory', stale.id);
  assert.equal(f.store.createSession(stale, tokenHash('stale-password-attempt'), f.now + 2000, 60000), false);
});

for (const attached of [false, true]) test(`Actual ${attached ? 'attached stream' : 'pending ticket'} authorization is protected by the fresh Relay observation`, async t => {
  const f = fixture(t), hashes = Array.from({ length: 5 }, () => f.seed()), grant = f.ticket(hashes[0]);
  const session = f.relay.sessions.get(grant.sessionId);
  if (attached) { f.relay.attachClient(new Wire(), session); await settle(); assert.equal(session.hostDispatched, true); }
  const protection = f.relay.protectedSessionHashes(f.user().id, f.now + 2000);
  assert.equal(protection.has(hashes[0]), true); f.login(protection);
  assert.equal(f.relay.authorized(session), true); f.relay.revalidate(); assert.equal(f.relay.sessions.get(session.id), session);
});

test('A session, an active console and an unresolved display owner all protect their own current tokens', async t => {
  const f = fixture(t), hashes = Array.from({ length: 5 }, () => f.seed());
  const session = { ...f.binding(hashes[0]), mode: 'stream' }; f.relay.sessions.set(session.id, session);
  const consoleGrant = await f.relay.console.create(f.user(), hashes[1], 'pve-150');
  const operation = { ...f.binding(hashes[2], 151), status: 'unknown' }; f.relay.displayBusy.set(operation.desktopId, operation);
  try {
    const protection = f.relay.protectedSessionHashes(f.user().id, f.now + 2000);
    assert.deepEqual([...protection].sort(), hashes.slice(0, 3).sort());
    f.login(protection); for (const hash of hashes.slice(0, 3)) assert.ok(f.store.authenticate(hash, f.now + 2000));
  } finally { f.relay.sessions.delete(session.id); f.relay.displayBusy.delete(operation.desktopId); f.relay.console.cancel(f.user(), hashes[1], 'pve-150', consoleGrant.id); }
});

test('Actual stop-v1 cleanup preserves its token privately until the pending record is removed', async t => {
  const f = fixture(t), hashes = Array.from({ length: 5 }, () => f.seed()), grant = f.ticket(hashes[0]);
  const session = f.relay.sessions.get(grant.sessionId); f.relay.attachClient(new Wire(), session); await settle();
  assert.equal(session.hostDispatched, true); f.relay.stop(session, 'disconnected');
  const record = f.relay.sessionStop.pending.get('pve-149'); assert.ok(record); assert.equal(f.relay.sessions.has(session.id), false);
  assert.equal(Object.hasOwn(record, 'tokenHash'), false); assert.equal(Object.hasOwn(record, 'userId'), false);
  assert.equal(JSON.stringify(f.relay.sessionStop.view('pve-149')).includes(hashes[0]), false);
  assert.equal(JSON.stringify(f.relay.sessionStop.fields(record)).includes(hashes[0]), false);
  const protection = f.relay.protectedSessionHashes(f.user().id, f.now + 2000); assert.equal(protection.has(hashes[0]), true);
  f.login(protection); assert.ok(f.store.authenticate(hashes[0], f.now + 2000));
  f.relay.sessionStop.pending.delete('pve-149');
  assert.equal(f.relay.protectedSessionHashes(f.user().id, f.now + 2000).has(hashes[0]), false);
});

test('Retired console still protects its live token until its actual held identity query returns', async t => {
  const f = fixture(t), hashes = Array.from({ length: 5 }, () => f.seed()); let resolve;
  f.proxmox.consoleSnapshot = () => new Promise(done => { resolve = done; });
  const abort = new AbortController(), pending = f.relay.console.create(f.user(), hashes[0], 'pve-149', { signal: abort.signal });
  const rejected = assert.rejects(pending, error => error.code === 'console_cancelled'); await settle(); abort.abort();
  assert.equal(f.relay.console.grants.size, 1);
  const protection = f.relay.protectedSessionHashes(f.user().id, f.now + 2000); assert.equal(protection.has(hashes[0]), true); f.login(protection);
  resolve({ node: 'memory', vmid: 149, machineUuid: machine(149) }); await rejected;
  assert.equal(f.relay.console.grants.size, 0); assert.equal(f.relay.protectedSessionHashes(f.user().id, f.now + 2000).has(hashes[0]), false);
});

test('Internal live UI entries are protected only while open and belonging to the current authenticated user', t => {
  const f = fixture(t), hashes = Array.from({ length: 5 }, () => f.seed()), foreign = f.seed('bobby'), wire = new Wire();
  const clients = [{ hash: hashes[0], userId: f.user().id, ws: wire }, { hash: foreign, userId: f.user().id, ws: new Wire() }];
  assert.deepEqual([...f.relay.protectedSessionHashes(f.user().id, f.now + 2000, clients)], [hashes[0]]);
  wire.close(); assert.equal(f.relay.protectedSessionHashes(f.user().id, f.now + 2000, clients).size, 0);
});

test('Expiry, explicit logout, password change, disable and assignment revocation still defeat protection', t => {
  const f = fixture(t), hash = f.seed(), binding = f.binding(hash), id = binding.id; f.relay.sessions.set(id, binding);
  try {
    assert.equal(f.relay.protectedSessionHashes(f.user().id, f.now + 2000).has(hash), true);
    f.store.revoke('alice', 'pve-149'); assert.equal(f.relay.authorized(binding), false);
    assert.equal(f.relay.protectedSessionHashes(f.user().id, f.now + 2000).has(hash), false);
    f.store.assign('alice', 'pve-149'); f.store.logout(hash);
    assert.equal(f.relay.protectedSessionHashes(f.user().id, f.now + 2000).has(hash), false);
    const expired = f.seed(); f.store.db.prepare('UPDATE sessions SET expires_at=? WHERE token_hash=?').run(f.now + 100, expired);
    binding.tokenHash = expired; assert.equal(f.relay.protectedSessionHashes(f.user().id, f.now + 2000).has(expired), false);
    const passwordChanged = f.seed(); binding.tokenHash = passwordChanged; f.store.changePassword('alice', 'changed-memory-password', 'fixture');
    assert.equal(f.store.authenticate(passwordChanged, f.now + 2000), undefined);
    assert.equal(f.relay.protectedSessionHashes(f.user().id, f.now + 2000).size, 0);
    const disabled = f.seed(); binding.tokenHash = disabled; f.store.setUserEnabled('alice', false, 'fixture');
    assert.equal(f.relay.protectedSessionHashes(f.user().id, f.now + 2000).size, 0);
  } finally { f.relay.sessions.delete(id); }
});

test('Live-owner observation failure aborts instead of producing an empty protection snapshot', t => {
  const f = fixture(t), hash = f.seed(), binding = f.binding(hash); f.relay.sessions.set(binding.id, binding);
  const original = f.store.authenticate; f.store.authenticate = () => { throw new Error('fixed memory read failure'); };
  try { assert.throws(() => f.relay.protectedSessionHashes(f.user().id, f.now + 2000)); }
  finally { f.store.authenticate = original; f.relay.sessions.delete(binding.id); }
  assert.ok(f.store.authenticate(hash, f.now + 2000));
});
