// Actual Relay/ConsoleRelay/Store with memory SQLite and in-process wires.
// No VM, display API, listener, device or external credentials are touched.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { randomBytes, randomUUID } from 'node:crypto';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { Relay } from '../src/relay.mjs';
import { qemuConsoleProtocol, legacyQemuConsoleProtocol } from '../src/qemu-console-preparation.mjs';
import { newToken, tokenHash } from '../src/security.mjs';

const machine = '12345678-1234-1234-1234-000000000149';
const settle = async () => { for (let n = 0; n < 25; n++) await Promise.resolve(); };
class Wire extends EventEmitter {
  readyState = 1; sent = [];
  send(bytes, _options, done) { this.sent.push(JSON.parse(String(bytes))); done?.(); }
  terminate() { if (this.readyState === 3) return; this.readyState = 3; this.emit('close'); }
  close() { this.terminate(); }
  packet(value) { this.emit('message', Buffer.from(JSON.stringify(value)), false); }
  ping() {}
}
function fixture(t, { model = 'std', supported = true, attach = true } = {}) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  store.addUser({ name: 'alice', passwordHash: 'synthetic-fixture' });
  store.syncProxmox([{ vmid: 149, node: 'memory', name: 'Memory VM', status: 'running', machineUuid: machine }], 'fixture');
  store.assign('alice', 'pve-149');
  const token = newToken(), hash = tokenHash(token);
  store.createSession(store.findUser('alice'), hash, Date.now(), 3600000, 'client');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://memory.invalid', tokenId: 'fixture@pve!fixture', secret: 'SYNTHETIC_SERVER_ONLY' }), 'fixture');
  const clock = { time: Date.now() }, calls = [];
  const proxmox = { consoleSnapshot: async () => { calls.push('snapshot'); return { node: 'memory', vmid: 149, machineUuid: machine, ...(model ? { model } : {}) }; } };
  const relay = new Relay({ store, vault, proxmox, publicOrigin: 'https://broker.invalid', authorizationMs: 3600000, heartbeatMs: 3600000,
    consoleOptions: { now: () => clock.time }, qemuConsoleOptions: { now: () => clock.time, timeoutMs: 25000 } });
  const wire = new Wire(); let host;
  const capabilities = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true,     ...(supported ? { qemuConsoleProtocol } : {}) };
  if (attach) {
    const enrollment = newToken(), secret = newToken();
    store.issueEnrollment('pve-149', machine, tokenHash(enrollment), 'fixture');
    store.consumeEnrollment(tokenHash(enrollment), machine, { hostname: 'MemoryHost', version: '9.0.0', tokenHash: tokenHash(secret) });
    relay.attachHost(wire, store.authenticateHost(tokenHash(secret)), tokenHash(secret), true);
    host = relay.hosts.get('pve-149'); wire.packet({ type: 'host.capabilities', capabilities });
  }
  const grant = options => relay.console.create(store.findUser('alice'), hash, 'pve-149', options);
  const request = () => wire.sent.find(value => value.type === 'host.qemu-console.prepare');
  const reply = (extra = {}, req = request(), target = wire) => target.packet({ type: 'host.qemu-console.result', protocol: qemuConsoleProtocol,
    operationId: req.operationId, nonce: req.nonce, ok: true, attempted: false, workerQuiescent: true, reason: 'qemu_console_active', ...extra });
  const acknowledge = (extra = {}, target = wire) => {
    const req = wire.sent.findLast(value => value.type === 'host.qemu-console.renew');
    target.packet({ type: 'host.qemu-console.held', protocol: qemuConsoleProtocol, operationId: req.operationId, nonce: req.nonce, sequence: req.sequence, leaseMs: 30000, ...extra });
  };
  t.after(() => { relay.close(); store.close(); });
  return { store, relay, wire, host, clock, calls, capabilities, grant, request, reply, acknowledge };
}

for (const reason of ['qemu_console_active', 'qemu_console_restored']) test(`A v2 Host ${reason} strict proof precedes ticket issue and retains the exact grant lock`, async t => {
  const f = fixture(t), pending = f.grant(); await settle(); const request = f.request();
  assert.deepEqual(Object.keys(request).sort(), ['machineUuid', 'model', 'nonce', 'operationId', 'protocol', 'type']);
  assert.equal(request.machineUuid, machine); assert.equal(request.model, 'std'); assert.equal(request.protocol, qemuConsoleProtocol);
  const operation = f.relay.displayBusy.get('pve-149'); assert.equal(operation.kind, 'qemu-console');
  assert.equal([...f.relay.console.grants.values()][0].state, 'preparing');
  assert.throws(() => f.relay.assertPowerAllowed('pve-149'), error => error.code === 'display_in_progress');
  assert.throws(() => f.relay.assertDisplayIdle('pve-149'), error => error.code === 'console_busy');
  assert.throws(() => f.relay.connect(f.store.findUser('alice'), operation.tokenHash, 'pve-149'), error => error.code === 'console_busy');
  await assert.rejects(f.grant(), error => error.code === 'console_busy');
  f.reply({ reason, attempted: reason === 'qemu_console_restored' }); const grant = await pending;
  assert.equal(grant.qemuConsolePreparation.status, 'isolated');
  assert.equal(f.relay.displayBusy.get('pve-149'), operation); assert.equal(operation.status, 'held');
  assert.equal(f.relay.qemuConsole.operations.size, 1);
  assert.doesNotThrow(() => f.relay.console.current(operation.grant), 'only the exact held grant passes its own reservation');
  assert.throws(() => f.relay.assertPowerAllowed('pve-149'), error => error.code === 'display_in_progress');
  assert.deepEqual(f.calls, ['snapshot', 'snapshot']);
  assert.equal(JSON.stringify(grant).includes(request.nonce), false);
  const deadline = operation.leaseDeadline;
  f.reply(); assert.equal(operation.leaseDeadline, deadline, 'replayed completion cannot renew or release the held owner');
});

for (const mode of ['offline', 'unsupported', 'legacy-v1', 'nonstd', 'stale']) test(`${mode} keeps original Proxmox emergency console without a readiness assertion`, async t => {
  const f = fixture(t, { attach: mode !== 'offline', supported: mode !== 'unsupported', model: mode === 'nonstd' ? undefined : 'std' });
  if (mode === 'nonstd') f.relay.proxmox.consoleSnapshot = async () => ({ node: 'memory', vmid: 149, machineUuid: machine });
  if (mode === 'legacy-v1') {
    f.wire.packet({ type: 'host.capabilities', capabilities: { ...f.capabilities, qemuConsoleProtocol: legacyQemuConsoleProtocol } });
    assert.equal(f.host.capabilities.qemuConsoleProtocol, legacyQemuConsoleProtocol);
    assert.equal(f.wire.sent.at(-1).accepted, true);
  }
  if (mode === 'stale') f.host.capabilitiesAt = f.clock.time - f.relay.heartbeatMs * 3 - 1;
  const grant = await f.grant(); assert.equal(Object.hasOwn(grant, 'qemuConsolePreparation'), false); assert.equal(f.request(), undefined);
  assert.equal(f.relay.qemuConsole.operations.size, 0); assert.equal(f.relay.displayBusy.size, 0);
});

test('No attempted mutation on locked/unavailable Windows preserves emergency access as unknown', async t => {
  const f = fixture(t), pending = f.grant(); await settle(); f.reply({ ok: false, reason: 'qemu_console_unavailable' });
  const grant = await pending; assert.deepEqual(grant.qemuConsolePreparation, { status: 'unknown' });
  assert.equal(f.relay.displayBusy.size, 0);
});

// A park (signed out, locked, a standard user, a UAC prompt) stops
// the Host's own virtual outputs. When that stop fails the Host answers
// unavailable with attempted=true. The console is not refused then
// (console_display_unavailable); it opens like every other "could not prepare".
test('A preparation the Host attempted and could not finish still opens the console as unknown', async t => {
  const f = fixture(t), pending = f.grant(); await settle();
  f.reply({ ok: false, attempted: true, reason: 'qemu_console_unavailable' });
  const grant = await pending; assert.deepEqual(grant.qemuConsolePreparation, { status: 'unknown' });
  assert.equal(f.relay.displayBusy.size, 0); assert.equal(f.relay.qemuConsole.operations.size, 0);
  assert.equal(f.relay.console.grants.size, 1, 'the console grant is issued');
  assert.equal(f.wire.sent.filter(value => value.type === 'host.qemu-console.cancel').length, 0, 'nothing is held, so nothing is released');
  // The attempted change still withdraws what the Host advertised until it reports again.
  assert.equal(f.host.capabilitiesPending, true); assert.equal(f.relay.qemuConsole.supported(f.host), false);
  f.wire.packet({ type: 'host.capabilities', capabilities: f.capabilities });
  assert.equal(f.relay.qemuConsole.supported(f.host), true);
});

test('An unattempted unavailable answer leaves the advertised Host state as it is', async t => {
  const f = fixture(t), pending = f.grant(); await settle(); f.reply({ ok: false, reason: 'qemu_console_unavailable' });
  await pending; assert.equal(f.host.capabilitiesPending, false); assert.equal(f.relay.qemuConsole.supported(f.host), true);
});

test('An unavailable answer whose worker is still running is a timeout, never an open console', async t => {
  const f = fixture(t), pending = f.grant(), rejected = assert.rejects(pending, error => error.code === 'console_timeout'); await settle();
  f.reply({ ok: false, attempted: true, reason: 'qemu_console_unavailable', workerQuiescent: false }); await rejected;
  assert.equal(f.relay.console.grants.size, 0);
});

for (const reason of ['qemu_console_state_missing', 'qemu_console_state_invalid', 'qemu_console_identity_changed', 'qemu_console_foreign_changed', 'qemu_console_busy', 'qemu_console_inactive']) test(`${reason} cannot issue a ready ticket`, async t => {
  const f = fixture(t), pending = f.grant(), rejected = assert.rejects(pending, error => error.code === 'console_display_' + reason.slice('qemu_console_'.length));
  await settle(); f.reply({ ok: false, attempted: true, reason }); await rejected;
  assert.equal(f.relay.console.grants.size, 0); assert.equal(f.relay.displayBusy.size, 0);
});

test('Wrong nonce, Host instance or operation cannot complete another mutation', async t => {
  const f = fixture(t), pending = f.grant(); await settle(); const operation = f.relay.displayBusy.get('pve-149');
  f.reply({ nonce: newToken() }); f.reply({ operationId: randomUUID() });
  f.relay.qemuConsole.result({ ...f.host }, { ...f.request(), type: 'host.qemu-console.result', ok: true, attempted: false, workerQuiescent: true, reason: 'qemu_console_active' });
  assert.equal(f.relay.displayBusy.get('pve-149'), operation); assert.equal(operation.status, 'pending');
  f.reply(); await pending;
});

test('Timeout never unlocks power or retries mutation; exact late worker completion releases without reviving ticket', async t => {
  const f = fixture(t), pending = f.grant(), rejected = assert.rejects(pending, error => error.code === 'console_timeout'); await settle();
  const operation = f.relay.displayBusy.get('pve-149'); f.clock.time += 25001; f.relay.qemuConsole.revalidate(); await rejected;
  assert.equal(operation.status, 'releasing'); assert.equal(f.relay.displayBusy.get('pve-149'), operation);
  assert.throws(() => f.relay.assertPowerAllowed('pve-149'), error => error.code === 'display_in_progress');
  assert.equal(f.wire.sent.filter(value => value.type === 'host.qemu-console.prepare').length, 1);
  assert.equal(f.wire.sent.filter(value => value.type === 'host.qemu-console.cancel').length, 1);
  assert.equal(f.wire.sent.filter(value => value.type === 'host.qemu-console.status').length, 1);
  f.reply({ ok: false, reason: 'qemu_console_timeout', workerQuiescent: false }); assert.equal(f.relay.displayBusy.get('pve-149'), operation);
  f.clock.time += 5001; f.relay.qemuConsole.revalidate(); assert.equal(f.wire.sent.filter(value => value.type === 'host.qemu-console.status').length, 2);
  f.reply(); assert.equal(f.relay.displayBusy.get('pve-149'), operation, 'late success does not acknowledge hold release');
  f.reply({ ok: false, reason: 'qemu_console_timeout' });
  assert.equal(f.relay.displayBusy.size, 0); assert.equal(f.relay.console.grants.size, 0); assert.equal(operation.status, 'releasing');
});

test('Browser abort removes its grant but retains mutation ownership until worker exit', async t => {
  const f = fixture(t), abort = new AbortController(), pending = f.grant({ signal: abort.signal }), rejected = assert.rejects(pending, error => error.code === 'console_cancelled');
  await settle(); const operation = f.relay.displayBusy.get('pve-149'); abort.abort(); await rejected;
  assert.equal(f.relay.console.grants.size, 0); assert.equal(f.relay.displayBusy.get('pve-149'), operation);
  f.reply({ ok: false, reason: 'qemu_console_cancelled' }); assert.equal(f.relay.displayBusy.size, 0);
});

test('Assignment revocation cancels; exact Host loss clears the independent cleanup owner', async t => {
  const f = fixture(t), pending = f.grant(), rejected = assert.rejects(pending); await settle(); const operation = f.relay.displayBusy.get('pve-149');
  f.store.revoke('alice', 'pve-149'); f.relay.revalidate(); await rejected; assert.equal(f.relay.displayBusy.get('pve-149'), operation);
  f.relay.dropHost(f.host, 'host_disconnected'); assert.equal(f.relay.qemuConsole.operations.size, 0); assert.equal(f.relay.displayBusy.size, 0);
});

test('Malformed result fails closed and actual model change after recovery invalidates the grant', async t => {
  const f = fixture(t), pending = f.grant(), rejected = assert.rejects(pending); await settle();
  f.reply({ extra: 'unexpected' }); await rejected; assert.equal(f.relay.hosts.size, 0); assert.equal(f.relay.displayBusy.size, 0);
  const second = fixture(t), next = second.grant(), invalid = assert.rejects(next, error => error.code === 'console_identity_changed'); await settle();
  second.relay.proxmox.consoleSnapshot = async () => ({ node: 'memory', vmid: 149, machineUuid: machine }); second.reply(); await invalid;
});

test('Existing capture cleanup barrier is not bypassed by new capability or by QEMU operation ownership', async t => {
  const f = fixture(t), ended = { id: randomUUID(), desktopId: 'pve-149', host: f.host, mode: 'stream', hostDispatched: true, stopProtocol: 'stop-v1' };
  const record = f.relay.sessionStop.begin(ended);
  f.relay.sessionStop.result(f.host, { type: 'host.session.stopped', sessionId: ended.id, sessionStopProtocol: 'stop-v1', stopRequestId: record.requestId, workerQuiescent: true, profileRequired: true, profileRestored: false });
  await assert.rejects(f.grant(), error => error.code === 'console_busy'); assert.equal(f.request(), undefined);
});

test('QEMU capability is accepted only as the exact service protocol', t => {
  const f = fixture(t); assert.equal(f.host.capabilities.qemuConsoleProtocol, qemuConsoleProtocol);
  for (const extra of [{ qemuConsoleProtocol: 'qemu-console-v3' }, { qemuConsoleProtocol, service: false }]) {
    f.wire.packet({ type: 'host.capabilities', capabilities: { ...f.capabilities, ...extra } });
    assert.equal(f.wire.sent.at(-1).accepted, false);
  }
});

test('Contradictory success, non-quiescent success and unknown reasons cannot assert restoration', async t => {
  for (const extra of [{ attempted: true }, { reason: 'qemu_console_restored', attempted: false }, { workerQuiescent: false }, { reason: 'user-controlled-secret' }, { protocol: legacyQemuConsoleProtocol }]) {
    const f = fixture(t), pending = f.grant(), rejected = assert.rejects(pending); await settle(); f.reply(extra); await rejected;
    assert.equal(f.relay.hosts.size, 0); assert.equal(f.relay.displayBusy.size, 0); assert.equal(f.relay.console.grants.size, 0);
  }
});

test('An expired unrelated display operation keeps its existing power policy', t => {
  const f = fixture(t); f.relay.displayBusy.set('pve-149', { kind: 'console', status: 'unknown', deadline: Date.now() - 1 });
  assert.doesNotThrow(() => f.relay.assertPowerAllowed('pve-149')); f.relay.displayBusy.clear();
});

test('A bounded Host preparation gets its own deadline after initial PVE reads, then ordinary ticket expiry resumes', async t => {
  const f = fixture(t), initial = f.clock.time; let snapshots = 0;
  f.relay.proxmox.consoleSnapshot = async () => { if (++snapshots === 1) f.clock.time += 15000; return { node: 'memory', vmid: 149, machineUuid: machine, model: 'std' }; };
  const pending = f.grant(); await settle(); f.clock.time += 24000; f.relay.console.revalidate();
  assert.ok(f.clock.time > initial + 30000); assert.equal(f.relay.displayBusy.size, 1);
  assert.equal([...f.relay.console.grants.values()][0].closed, false, 'the expired initial-read budget is not the Host operation deadline');
  f.reply(); const grant = await pending; assert.equal(Date.parse(grant.expiresAt), f.clock.time + f.relay.console.grantMs);
});

async function heldFixture(t) {
  const f = fixture(t), pending = f.grant(); await settle(); f.reply();
  f.issued = await pending; f.operation = f.relay.displayBusy.get('pve-149'); return f;
}

test('Exact retirement releases once, late prepare is insufficient, and only post-quiet capabilities reopen stream admission', async t => {
  const f = await heldFixture(t), op = f.operation;
  f.relay.console.retire(op.grant, 'console_cancelled'); f.relay.console.retire(op.grant, 'console_cancelled');
  assert.equal(op.status, 'releasing'); assert.equal(f.relay.console.grants.size, 0);
  assert.equal(f.wire.sent.filter(value => value.type === 'host.qemu-console.cancel').length, 1);
  assert.equal(f.host.capabilitiesPending, true); assert.equal(f.host.capabilitiesAt, 0);
  assert.equal(f.relay.qemuConsole.preparation(op.grant).status, 'unknown');
  f.wire.packet({ type: 'host.capabilities', capabilities: f.capabilities });
  assert.equal(f.host.capabilitiesPending, true, 'pre-quiet capabilities are stale for release');
  f.reply(); f.reply({ ok: false, reason: 'qemu_console_cancelled', workerQuiescent: false });
  assert.equal(f.relay.displayBusy.get('pve-149'), op);
  assert.throws(() => f.relay.connect(f.store.findUser('alice'), op.tokenHash, 'pve-149'), error => error.code === 'display_in_progress');
  f.reply({ ok: false, reason: 'qemu_console_cancelled', nonce: newToken() });
  assert.equal(f.relay.displayBusy.get('pve-149'), op);
  f.reply({ ok: false, reason: 'qemu_console_cancelled' });
  assert.equal(f.relay.displayBusy.size, 0); assert.equal(f.relay.qemuConsole.operations.size, 0);
  assert.equal(f.host.capabilitiesPending, true);
  assert.throws(() => f.relay.connect(f.store.findUser('alice'), op.tokenHash, 'pve-149'), error => error.code === 'stream_unavailable' && error.details.reason === 'capabilities_pending');
  f.wire.packet({ type: 'host.heartbeat', capabilities: f.capabilities });
  assert.equal(f.host.capabilitiesPending, false);
  assert.doesNotThrow(() => f.relay.connect(f.store.findUser('alice'), op.tokenHash, 'pve-149'));
});

test('Explicit ordered renewals extend only the exact current hold, measured from dispatch', async t => {
  const f = await heldFixture(t), op = f.operation, initial = op.leaseDeadline;
  f.clock.time += 9999; f.relay.qemuConsole.revalidate();
  assert.equal(f.wire.sent.some(value => value.type === 'host.qemu-console.renew'), false);
  f.clock.time++; f.relay.qemuConsole.revalidate();
  const renewal = f.wire.sent.at(-1);
  assert.deepEqual(renewal, { type: 'host.qemu-console.renew', protocol: qemuConsoleProtocol, operationId: op.id, nonce: op.nonce, sequence: 1 });
  f.acknowledge({ nonce: newToken() }); f.acknowledge({ operationId: randomUUID() });
  f.relay.qemuConsole.held({ ...f.host }, { ...renewal, type: 'host.qemu-console.held', leaseMs: 30000 });
  f.acknowledge({ sequence: 2 }); assert.equal(op.leaseDeadline, initial, 'future ACK has no effect');
  f.clock.time += 4000; f.acknowledge();
  assert.equal(op.ackSequence, 1); assert.equal(op.leaseDeadline, initial + 10000, 'receive delay cannot lengthen lease');
  f.clock.time += 6000; f.relay.qemuConsole.revalidate();
  assert.equal(f.wire.sent.at(-1).sequence, 2);
  f.acknowledge({ sequence: 1 }); assert.equal(op.ackSequence, 1, 'reversed ACK does not acknowledge seq2');
  f.acknowledge(); assert.equal(op.ackSequence, 2); assert.equal(op.leaseDeadline, initial + 20000);
  f.reply(); assert.equal(op.leaseDeadline, initial + 20000, 'status result never extends hold');
});

test('A delayed initial result does not add transport delay to the initial Host lease', async t => {
  const f = fixture(t), pending = f.grant(); await settle(); const op = f.relay.displayBusy.get('pve-149');
  f.clock.time += 24000; f.reply(); await pending;
  assert.equal(op.leaseDeadline, op.submittedAt + 30000);
  f.relay.qemuConsole.revalidate(); assert.equal(f.wire.sent.at(-1).type, 'host.qemu-console.renew');
  f.acknowledge(); assert.equal(op.leaseDeadline, f.clock.time + 30000);
});

test('The pure proof getter becomes unknown before the timer handles expired lease or outstanding ACK', async t => {
  for (const expiry of ['lease', 'ack']) {
    const f = await heldFixture(t), op = f.operation;
    if (expiry === 'ack') { f.clock.time += 10000; f.relay.qemuConsole.revalidate(); f.clock.time += 5000; }
    else f.clock.time += 30000;
    const packets = f.wire.sent.length, lease = op.leaseDeadline;
    assert.deepEqual(f.relay.qemuConsole.preparation(op.grant), { status: 'unknown' });
    assert.equal(op.status, 'held'); assert.equal(op.grant.closed, false);
    assert.equal(f.wire.sent.length, packets); assert.equal(op.leaseDeadline, lease);
  }
});

test('A final PVE identity read cannot turn a lease that expired during the await into an isolated initial response', async t => {
  const f = fixture(t); let snapshots = 0;
  f.relay.proxmox.consoleSnapshot = async () => {
    if (++snapshots === 2) f.clock.time += 30001;
    return { node: 'memory', vmid: 149, machineUuid: machine, model: 'std' };
  };
  const pending = f.grant(); await settle(); f.reply();
  assert.deepEqual((await pending).qemuConsolePreparation, { status: 'unknown' });
  assert.equal(f.relay.displayBusy.get('pve-149').status, 'held', 'the getter does not mutate the existing bounded teardown policy');
});

for (const mode of ['missing-ack', 'late-ack', 'ttl', 'overflow']) test(`${mode} retires the grant without converting timeout to release proof`, async t => {
  const f = await heldFixture(t), op = f.operation;
  if (mode === 'overflow') op.sequence = op.ackSequence = 0xffffffff;
  f.clock.time += mode === 'ttl' ? 30000 : 10000; f.relay.qemuConsole.revalidate();
  if (mode === 'missing-ack' || mode === 'late-ack') {
    f.clock.time += 5000;
    if (mode === 'late-ack') f.acknowledge(); else f.relay.qemuConsole.revalidate();
  }
  assert.equal(op.status, 'releasing'); assert.equal(op.grant.closed, true);
  assert.equal(f.wire.sent.filter(value => value.type === 'host.qemu-console.cancel').length, 1);
  assert.equal(f.relay.displayBusy.get('pve-149'), op);
  f.clock.time += 100000; f.relay.qemuConsole.revalidate();
  assert.equal(f.relay.displayBusy.get('pve-149'), op, 'lease expiry is not worker exit proof');
  assert.equal(f.wire.sent.filter(value => value.type === 'host.qemu-console.prepare').length, 1);
  f.reply({ ok: false, reason: 'qemu_console_timeout' }); assert.equal(f.relay.displayBusy.size, 0);
});

for (const extra of [{ sequence: 0 }, { sequence: -1 }, { sequence: 0x100000000 }, { sequence: 1.5 }, { leaseMs: 30001 }, { extra: true }, { protocol: legacyQemuConsoleProtocol }]) test(`Malformed hold ACK ${JSON.stringify(extra)} cannot extend or release the display`, async t => {
  const f = await heldFixture(t), op = f.operation, initial = op.leaseDeadline;
  f.clock.time += 10000; f.relay.qemuConsole.revalidate(); f.acknowledge(extra);
  assert.equal(op.status, 'releasing'); assert.equal(op.leaseDeadline, initial);
  assert.equal(f.relay.displayBusy.get('pve-149'), op);
});

test('Held authorization revocation retires the exact grant, stops renewal, and retains release ownership', async t => {
  const f = await heldFixture(t), op = f.operation;
  f.store.revoke('alice', 'pve-149'); f.clock.time += 10000; f.relay.revalidate();
  assert.equal(op.status, 'releasing'); assert.equal(op.grant.closed, true);
  assert.equal(f.wire.sent.filter(value => value.type === 'host.qemu-console.renew').length, 0);
  assert.equal(f.relay.displayBusy.get('pve-149'), op);
  f.reply({ ok: false, reason: 'qemu_console_identity_changed' }); assert.equal(f.relay.displayBusy.size, 0);
});

test('Host hold failure requires exact quiet release, and status polling never sends another prepare or renew', async t => {
  const f = await heldFixture(t), op = f.operation;
  f.reply({ ok: false, reason: 'qemu_console_identity_changed', workerQuiescent: false });
  f.clock.time += 1000; f.relay.qemuConsole.revalidate();
  const poll = f.wire.sent.at(-1);
  assert.deepEqual(poll, { type: 'host.qemu-console.status', protocol: qemuConsoleProtocol, operationId: op.id, nonce: op.nonce });
  assert.equal(f.wire.sent.filter(value => value.type === 'host.qemu-console.renew').length, 0);
  f.reply({ ok: false, reason: 'qemu_console_state_invalid' }); assert.equal(f.relay.displayBusy.get('pve-149'), op, 'not a release ACK');
  f.reply({ ok: false, reason: 'qemu_console_identity_changed' }); assert.equal(f.relay.displayBusy.size, 0);
});

test('Host loss downgrades the initial proof and frees dead Host ownership while keeping the PVE grant', async t => {
  const f = await heldFixture(t), op = f.operation;
  f.relay.dropHost(f.host, 'host_disconnected');
  assert.equal(op.grant.qemuConsolePreparation.status, 'unknown'); assert.equal(f.relay.qemuConsole.preparation(op.grant).status, 'unknown'); assert.equal(f.relay.displayBusy.size, 0);
  assert.equal(f.relay.qemuConsole.operations.size, 0); assert.equal(op.grant.closed, false);
  assert.doesNotThrow(() => f.relay.console.current(op.grant));
  f.relay.console.retire(op.grant, 'console_cancelled');
  const fallback = await f.grant(); assert.equal(Object.hasOwn(fallback, 'qemuConsolePreparation'), false);
  f.reply({ ok: false, reason: 'qemu_console_cancelled' });
  assert.equal(f.relay.console.grants.get(fallback.id)?.closed, false, 'dead Host reply cannot retire a later offline grant');
});

test('Unrelated stop barrier and foreign reservations remain strict even for the exact held grant', async t => {
  const f = await heldFixture(t), op = f.operation;
  const other = { kind: 'unrelated', host: f.host }; f.relay.displayBusy.set('pve-149', other);
  assert.throws(() => f.relay.console.current(op.grant), error => error.code === 'console_busy');
  f.relay.displayBusy.set('pve-149', op);
  f.relay.sessionStop.pending.set('pve-149', { host: f.host, desktopId: 'pve-149', workerQuiescent: false });
  assert.throws(() => f.relay.console.current(op.grant), error => error.code === 'console_busy');
  f.relay.sessionStop.pending.clear();
});

test('An old exact release or ACK cannot remove a later grant on the same authenticated Host', async t => {
  const f = await heldFixture(t), old = f.operation, oldRequest = f.request();
  f.relay.console.retire(old.grant, 'console_cancelled'); f.reply({ ok: false, reason: 'qemu_console_cancelled' });
  f.wire.packet({ type: 'host.capabilities', capabilities: f.capabilities });
  const pending = f.grant(); await settle(); const request = f.wire.sent.findLast(value => value.type === 'host.qemu-console.prepare');
  f.reply({}, request); await pending;
  const current = f.relay.displayBusy.get('pve-149'); assert.notEqual(current.id, old.id);
  f.reply({ ok: false, reason: 'qemu_console_cancelled' }, oldRequest);
  f.wire.packet({ type: 'host.qemu-console.held', protocol: qemuConsoleProtocol, operationId: old.id, nonce: old.nonce, sequence: 1, leaseMs: 30000 });
  assert.equal(f.relay.displayBusy.get('pve-149'), current); assert.equal(current.status, 'held');
});

// A Windows restart under an open console: the Host returns on a new connection and knows nothing of the console.
function returned(f) {
  const wire = new Wire(), hash = f.host.tokenHash;
  f.relay.dropHost(f.host, 'host_disconnected');
  f.relay.attachHost(wire, f.store.authenticateHost(hash), hash, true);
  const report = (extra = {}) => wire.packet({ type: 'host.capabilities', capabilities: { ...f.capabilities, ...extra } });
  const prepares = () => wire.sent.filter(value => value.type === 'host.qemu-console.prepare');
  return { wire, report, prepares, host: f.relay.hosts.get('pve-149') };
}
test('A Host that comes back under an open console is asked again once it reports a signed-in session, and the console is isolated again', async t => {
  const f = await heldFixture(t), first = f.operation, grant = first.grant, expiresAt = grant.expiresAt;
  const back = returned(f);
  assert.equal(grant.closed, false); assert.equal(f.relay.qemuConsole.preparation(grant).status, 'unknown');
  // At the sign-in screen the Host could only park its own outputs: nothing is asked yet.
  back.report({ interactiveSession: false, streaming: false, virtualDisplayReady: false });
  assert.deepEqual(back.prepares(), []);
  back.report();
  const [request] = back.prepares();
  assert.notEqual(request.operationId, first.id); assert.notEqual(request.nonce, first.nonce);
  assert.deepEqual([request.machineUuid, request.model, request.protocol], [machine, 'std', qemuConsoleProtocol]);
  assert.equal(grant.expiresAt, expiresAt, 'an issued ticket keeps its own deadline');
  const operation = f.relay.displayBusy.get('pve-149');
  assert.equal(operation.grant, grant); assert.equal(operation.host, back.host); assert.equal(operation.status, 'pending');
  assert.equal(f.relay.qemuConsole.preparation(grant).status, 'unknown', 'not isolated before the Host says so');
  back.report(); assert.equal(back.prepares().length, 1, 'one request at a time');
  f.reply({ attempted: true, reason: 'qemu_console_restored' }, request, back.wire); await settle();
  assert.equal(operation.status, 'held'); assert.equal(f.relay.qemuConsole.preparation(grant).status, 'isolated'); assert.equal(grant.closed, false);
  back.report(); assert.equal(back.prepares().length, 1, 'a held console is not asked again');
  // The old connection's operation cannot answer for the new one.
  f.reply({ ok: false, reason: 'qemu_console_cancelled' }, f.request(), f.wire);
  assert.equal(operation.status, 'held'); assert.equal(grant.closed, false);
});
test('A repeat that is refused or unanswered never closes the open console; it is asked again five seconds apart and stops after six requests', async t => {
  const f = await heldFixture(t), grant = f.operation.grant, back = returned(f);
  for (let attempt = 1; attempt <= 6; attempt++) {
    back.report(); assert.equal(back.prepares().length, attempt);
    f.reply({ ok: false, reason: 'qemu_console_busy' }, back.prepares().at(-1), back.wire); await settle();
    assert.equal(f.relay.displayBusy.size, 0); assert.equal(grant.closed, false); assert.equal(f.relay.qemuConsole.preparation(grant).status, 'unknown');
    assert.doesNotThrow(() => f.relay.console.current(grant));
    // A Host that just started reports several times in a row: one request per five seconds.
    f.clock.time += 4999; back.report(); assert.equal(back.prepares().length, attempt);
    f.clock.time += 1;
  }
  back.report(); assert.equal(back.prepares().length, 6);
  // Another restart is another Host connection: asked again, and no answer within the deadline ends only the request.
  f.host = back.host; const again = returned(f);
  again.report(); assert.equal(again.prepares().length, 1);
  const operation = f.relay.displayBusy.get('pve-149');
  f.clock.time += 25001; f.relay.qemuConsole.revalidate();
  assert.equal(operation.status, 'releasing'); assert.equal(grant.closed, false, 'the console was open without the proof and stays open');
  assert.equal(again.wire.sent.filter(value => value.type === 'host.qemu-console.cancel').length, 1);
  f.reply({ ok: false, reason: 'qemu_console_timeout' }, again.prepares()[0], again.wire);
  assert.equal(f.relay.displayBusy.size, 0); assert.equal(grant.closed, false);
});
test('Nothing is asked again of the Host a console was opened on, of an unsupported Host or for another desktop state', async t => {
  // The same connection: a console that opened as unknown stays as it was opened.
  const same = fixture(t), opening = same.grant(); await settle();
  same.reply({ ok: false, reason: 'qemu_console_unavailable' }); await opening;
  same.wire.packet({ type: 'host.capabilities', capabilities: same.capabilities });
  assert.equal(same.wire.sent.filter(value => value.type === 'host.qemu-console.prepare').length, 1);
  // A returned Host without the protocol, and one while the display is held by something else.
  const old = await heldFixture(t), oldBack = returned(old);
  oldBack.report({ qemuConsoleProtocol: undefined }); assert.deepEqual(oldBack.prepares(), []);
  const busy = await heldFixture(t), busyBack = returned(busy);
  busy.relay.displayBusy.set('pve-149', { kind: 'display', desktopId: 'pve-149' });
  busyBack.report(); assert.deepEqual(busyBack.prepares(), []);
  busy.relay.displayBusy.delete('pve-149');
  // A closed console is not prepared for.
  busy.relay.console.retire(busy.operation.grant, 'console_cancelled');
  busyBack.report(); assert.deepEqual(busyBack.prepares(), []);
});
