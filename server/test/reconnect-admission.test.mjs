// Actual broker admission/dispatch with memory SQLite, EventEmitter wires and
// held promises only. No socket/listener/remote request, Windows or file I/O.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { randomBytes, randomUUID } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { Relay } from '../src/relay.mjs';
import { HttpError } from '../src/errors.mjs';
import { newToken, tokenHash } from '../src/security.mjs';

const origin = 'https://reconnect-admission-fixture.invalid';
const machineUuid = '12345678-1234-1234-1234-123456789abc';
const options = { maxWidth: 1280, maxHeight: 720, fps: 60, bitrate: 8000000, audio: false, monitor: '0' };
const viewportRequest = { initialViewportProtocol: 'epoch-v1', initialViewport: { monitor: '0', width: 2560, height: 1440, fps: 60 } };
const capabilities = (profile = false) => ({
  service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true,
  sessionStopProtocol: 'stop-v1',
  dynamicResolution: true, viewportProvisioning: true, virtualMonitorControl: true,
  resizeProtocol: 'epoch-v1', initialViewportProtocol: 'epoch-v1',
  ...(profile ? { streamProfileProtocol: 'console-profile-v1', streamProfileViewportProtocol: 'epoch-v1' } : {}),
  monitors: [{ id: '0', width: 1920, height: 1080, primary: !profile,
    modes: [{ width: 1920, height: 1080, refreshRate: 60 }] }],
});

class MemoryWire extends EventEmitter {
  readyState = WebSocket.OPEN;
  bufferedAmount = 0;
  messages = [];
  send(payload, { binary }, complete) {
    assert.equal(this.readyState, WebSocket.OPEN, 'fixture sent on a closed memory wire');
    this.messages.push(binary ? Buffer.from(payload) : JSON.parse(String(payload)));
    complete?.();
  }
  packet(value) { this.emit('message', Buffer.from(JSON.stringify(value)), false); }
  close() {
    if (this.readyState === WebSocket.CLOSED) return;
    this.readyState = WebSocket.CLOSED;
    this.emit('close');
  }
  terminate() { this.close(); }
  ping() {}
}

function fixture(t, { profile = false, holdProof = false } = {}) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  for (const name of ['alice', 'bobby']) store.addUser({ name, passwordHash: 'unused-memory-fixture' });
  store.syncProxmox([170, 149].map(vmid => ({ vmid, node: 'fixture', name: `VM${vmid}`, status: 'running' })), 'fixture');
  store.assign('alice', 'pve-170'); store.assign('bobby', 'pve-170'); store.assign('bobby', 'pve-149');
  const tokens = { alice: newToken(), bobby: newToken() };
  for (const name of ['alice', 'bobby']) store.createSession(store.findUser(name), tokenHash(tokens[name]), Date.now(), 3600000, 'client');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://memory-fixture.invalid', tokenId: 'fixture@pve!fixture', secret: 'synthetic-only' }), 'fixture');
  let resolveProof, proofCalls = 0;
  const proof = { machineUuid, model: 'std' };
  const heldProof = new Promise(resolve => { resolveProof = () => resolve(proof); });
  const proxmox = { consoleIdentity: () => { ++proofCalls; return holdProof ? heldProof : Promise.resolve(proof); } };
  // Intervals never fire during these promise-only cases and are cleared below.
  const relay = new Relay({ store, vault, proxmox, publicOrigin: origin, authorizationMs: 3600000, heartbeatMs: 3600000 });
  const dispatches = [], productionDispatch = relay.dispatchSessionStart.bind(relay);
  relay.dispatchSessionStart = (...args) => { const actual = productionDispatch(...args); dispatches.push(actual); return actual; };
  const hosts = new Map();
  for (const vmid of [170, 149]) {
    const secret = newToken(), hash = tokenHash(secret), enrollment = newToken();
    store.issueEnrollment(`pve-${vmid}`, machineUuid, tokenHash(enrollment), 'fixture');
    store.consumeEnrollment(tokenHash(enrollment), machineUuid, { hostname: 'MemoryHost', version: '9.0.0', tokenHash: hash });
    const wire = new MemoryWire();
    relay.attachHost(wire, store.authenticateHost(hash), hash, true);
    hosts.set(vmid, { wire, host: relay.hosts.get(`pve-${vmid}`) });
  }
  function advertise(value = capabilities(profile), vmid = 170) {
    const wire = hosts.get(vmid).wire;
    wire.packet({ type: 'host.capabilities', capabilities: value });
    assert.equal(wire.messages.at(-1)?.accepted, true, 'fixture capabilities must pass the actual broker parser');
  }
  advertise(); advertise(capabilities(), 149);
  function create({ initial = false, who = 'alice', vmid = 170 } = {}) {
    return relay.connect(store.findUser(who), tokenHash(tokens[who]), `pve-${vmid}`, 'stream', options,
      initial ? 'epoch-v1' : undefined, undefined, initial ? viewportRequest : {});
  }
  function attach(ticket) {
    const session = relay.sessions.get(ticket.sessionId), client = new MemoryWire();
    const before = dispatches.length;
    relay.attachClient(client, session);
    return { session, client, done: dispatches[before] ?? Promise.resolve() };
  }
  const starts = (vmid = 170) => hosts.get(vmid).wire.messages.filter(value => value.type === 'session.start');
  t.after(async () => {
    // Finish only fixture-owned promises before closing its memory store.
    resolveProof(); relay.close(); await Promise.allSettled(dispatches); store.close();
  });
  return { store, relay, tokens, hosts, advertise, create, attach, starts, resolveProof, get proofCalls() { return proofCalls; } };
}

for (const kind of ['plain', 'initial', 'profile']) {
  test(`Unchanged ${kind} admission dispatches the exact ticket once`, async t => {
    const f = fixture(t, { profile: kind === 'profile' });
    const ticket = f.create({ initial: kind !== 'plain' }), attached = f.attach(ticket);
    await attached.done;
    assert.equal(f.starts().length, 1);
    assert.equal(f.starts()[0].sessionId, ticket.sessionId);
    assert.equal(f.relay.sessions.get(ticket.sessionId), attached.session);
    assert.equal(attached.session.hostDispatched, true);
    if (kind !== 'plain') assert.deepEqual(f.starts()[0].initialViewport, ticket.initialViewport);
    if (kind === 'profile') assert.equal(f.starts()[0].consoleProfile.sessionId, ticket.sessionId);
  });
}

for (const initial of [false, true]) {
  test(`A ticket cannot dispatch after maintenance with positive viewport flags (${initial ? 'initial' : 'plain'})`, async t => {
    const f = fixture(t), other = f.create({ who: 'bobby', vmid: 149 });
    const otherAttached = f.attach(other); await otherAttached.done;
    const ticket = f.create({ initial });
    f.advertise({ ...capabilities(), viewportFailureReason: 'host_maintenance' });
    // The parser accepted positive flags; the authoritative mode is still blocked.
    assert.equal(f.hosts.get(170).host.capabilities.streaming, true);
    assert.equal(f.relay.modes(f.hosts.get(170).host).stream, false);
    const attached = f.attach(ticket); await attached.done;
    assert.equal(f.starts().length, 0, 'maintenance must not reach Host session.start');
    assert.equal(f.relay.sessions.has(ticket.sessionId), false);
    assert.equal(attached.client.messages.find(value => value.type === 'stream.unavailable')?.reason, 'host_maintenance');
    assert.equal(f.relay.sessions.get(other.sessionId), otherAttached.session);
    assert.equal(f.starts(149).length, 1, 'another assigned VM is unaffected');
  });
}

for (const barrier of ['display', 'cleanup', 'update', 'capabilities-pending']) {
  test(`A ${barrier} barrier introduced after ticket issuance blocks Host dispatch`, async t => {
    const f = fixture(t), ticket = f.create();
    let predecessor;
    if (barrier === 'display') f.relay.displayBusy.set('pve-170', {
      desktopId: 'pve-170', host: f.hosts.get(170).host, id: randomUUID(), nonce: 'memory-only',
      status: 'pending', checkedAt: Date.now(), deadline: Date.now() + 60000,
    });
    if (barrier === 'cleanup') predecessor = f.relay.sessionStop.begin({
      desktopId: 'pve-170', id: randomUUID(), host: f.hosts.get(170).host,
      mode: 'stream', hostDispatched: true, stopProtocol: 'stop-v1',
    });
    if (barrier === 'update') f.relay.hostUpdate = { guard: () => { throw new HttpError(409, 'host_update_in_progress'); } };
    if (barrier === 'capabilities-pending') f.hosts.get(170).host.capabilitiesPending = true;
    const attached = f.attach(ticket); await attached.done;
    assert.equal(f.starts().length, 0, 'the new authoritative barrier must be observed');
    assert.equal(f.relay.sessions.has(ticket.sessionId), false);
    assert.equal(attached.client.messages.find(value => value.type === 'stream.unavailable')?.reason, 'host_maintenance');
    if (predecessor) assert.equal(f.relay.sessionStop.pending.get('pve-170'), predecessor, 'a rejected unstarted ticket cannot clear predecessor cleanup');
  });
}

for (const change of ['maintenance', 'display', 'cleanup', 'assignment', 'logout', 'update', 'capabilities-pending']) {
  test(`A held console proof cannot dispatch after ${change} changes`, async t => {
    const f = fixture(t, { profile: true, holdProof: true });
    const ticket = f.create({ initial: true }), attached = f.attach(ticket);
    assert.equal(f.proofCalls, 1, 'the production proof must actually be pending');
    if (change === 'maintenance') f.advertise({ ...capabilities(true), viewportFailureReason: 'host_maintenance' });
    if (change === 'display') f.relay.displayBusy.set('pve-170', {
      desktopId: 'pve-170', host: f.hosts.get(170).host, id: randomUUID(), nonce: 'memory-only',
      status: 'pending', checkedAt: Date.now(), deadline: Date.now() + 60000,
    });
    if (change === 'cleanup') f.relay.sessionStop.begin({
      desktopId: 'pve-170', id: randomUUID(), host: f.hosts.get(170).host,
      mode: 'stream', hostDispatched: true, stopProtocol: 'stop-v1',
    });
    if (change === 'assignment') f.store.revoke('alice', 'pve-170');
    if (change === 'logout') f.store.logout(tokenHash(f.tokens.alice));
    if (change === 'update') f.relay.hostUpdate = { guard: () => { throw new HttpError(409, 'host_update_in_progress'); } };
    if (change === 'capabilities-pending') f.hosts.get(170).host.capabilitiesPending = true;
    f.resolveProof(); await attached.done;
    assert.equal(f.starts().length, 0, 'late proof does not renew admission authority');
    assert.equal(f.relay.sessions.has(ticket.sessionId), false);
    if (change === 'assignment' || change === 'logout') assert.equal(attached.client.messages.find(value => value.type === 'session.ended')?.reason, 'authorization_revoked');
    else assert.equal(attached.client.messages.find(value => value.type === 'stream.unavailable')?.reason, 'host_maintenance');
  });
}

test('A stale held proof cannot stop a newer ticket or bypass explicit takeover', async t => {
  const f = fixture(t, { profile: true, holdProof: true });
  const first = f.create({ initial: true }), old = f.attach(first);
  assert.equal(f.proofCalls, 1);
  assert.throws(() => f.create({ who: 'bobby', initial: true }), error => error.code === 'desktop_busy' && error.details.reason === 'controller_in_use');
  // Exact-own cancellation, never an implicit takeover of somebody else.
  f.relay.cancel(f.store.findUser('alice'), tokenHash(f.tokens.alice), first.sessionId);
  const newer = f.create({ initial: true }), current = f.relay.sessions.get(newer.sessionId);
  f.resolveProof(); await old.done;
  assert.equal(f.starts().length, 0);
  assert.equal(f.relay.sessions.get(newer.sessionId), current);
  assert.equal(current.hostDispatched, undefined);
  assert.throws(() => f.create({ who: 'bobby', initial: true }), error => error.code === 'desktop_busy');
});
