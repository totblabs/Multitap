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
  pause() { this.paused = true; }
  resume() { this.paused = false; }
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
  const restores = [];
  function inject(target, name, replacement) {
    const previous = target[name]; target[name] = replacement;
    const restore = () => { target[name] = previous; };
    restores.push(restore); return restore;
  }
  let beforeDispatch = () => {};
  const dispatches = [], productionDispatch = relay.dispatchSessionStart.bind(relay);
  relay.dispatchSessionStart = (...args) => { beforeDispatch(...args); const actual = productionDispatch(...args); dispatches.push(actual); return actual; };
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
    for (const restore of restores.reverse()) restore();
    resolveProof(); relay.close(); await Promise.allSettled(dispatches); store.close();
  });
  return { store, relay, tokens, hosts, advertise, create, attach, starts, resolveProof, inject,
    beforeDispatch: callback => { beforeDispatch = callback; }, get proofCalls() { return proofCalls; } };
}

// Only own Store/transport methods are faulted. The actual dispatch Promise is
// observed directly, so no global unhandledRejection handler is installed.
for (const method of ['authenticateHost', 'authenticate', 'assignedDesktop']) {
  test(`Entry ${method} read failure is contained without changing another controller`, async t => {
    const f = fixture(t), other = f.create({ who: 'bobby', vmid: 149 });
    const otherAttached = f.attach(other); await otherAttached.done;
    const ticket = f.create(); let readCalls = 0;
    f.beforeDispatch(() => f.inject(f.store, method, () => { ++readCalls; throw new Error('synthetic-private-read-fault'); }));
    const attached = f.attach(ticket);
    await assert.doesNotReject(attached.done);
    assert.equal(readCalls, 1, 'abort must not retry failed Store authority reads');
    assert.equal(f.starts().length, 0);
    assert.equal(f.relay.sessions.has(ticket.sessionId), false);
    assert.equal(attached.client.messages.find(value => value.type === 'session.ended')?.reason, 'host_error');
    assert.equal(f.relay.sessions.get(other.sessionId), otherAttached.session);
    assert.equal(otherAttached.client.readyState, WebSocket.OPEN);
    assert.equal(f.hosts.get(170).wire.readyState, WebSocket.OPEN, 'do not drop the shared Host transport');
    assert.equal(f.hosts.get(149).wire.readyState, WebSocket.OPEN);
  });
}

for (const method of ['authenticateHost', 'authenticate', 'assignedDesktop']) {
  test(`Held-proof ${method} read failure is contained without another Store read`, async t => {
    const f = fixture(t, { profile: true, holdProof: true });
    const ticket = f.create({ initial: true }), attached = f.attach(ticket);
    assert.equal(f.proofCalls, 1);
    let readCalls = 0;
    f.inject(f.store, method, () => { ++readCalls; throw new Error('synthetic-private-read-fault'); });
    f.resolveProof(); await assert.doesNotReject(attached.done);
    assert.equal(readCalls, 1, 'unexpected proof errors must go directly to DB-free abort');
    assert.equal(f.starts().length, 0);
    assert.equal(f.relay.sessions.has(ticket.sessionId), false);
    assert.equal(attached.client.messages.find(value => value.type === 'session.ended')?.reason, 'host_error');
    assert.equal(attached.session.profilePreparing, false);
    assert.equal(f.hosts.get(170).wire.readyState, WebSocket.OPEN);
  });
}

test('Throwing stop cannot retain an unstarted exact ticket or reject its start Promise', async t => {
  const f = fixture(t), ticket = f.create(); let stopCalls = 0;
  f.beforeDispatch(() => {
    f.inject(f.store, 'authenticateHost', () => { throw new Error('synthetic-private-read-fault'); });
    f.inject(f.relay, 'stop', () => { ++stopCalls; throw new Error('synthetic-private-stop-fault'); });
  });
  const attached = f.attach(ticket); await assert.doesNotReject(attached.done);
  assert.ok(stopCalls > 0);
  assert.equal(f.starts().length, 0);
  assert.equal(f.relay.sessions.has(ticket.sessionId), false);
  assert.equal(f.relay.sessionStop.pending.has('pve-170'), false);
});

test('Throwing close and terminate remain observed failures without claiming the socket closed', async t => {
  const f = fixture(t), ticket = f.create(); let closeCalls = 0, terminateCalls = 0;
  f.beforeDispatch(session => {
    f.inject(f.store, 'authenticateHost', () => { throw new Error('synthetic-private-read-fault'); });
    f.inject(session.ws, 'close', () => { ++closeCalls; throw new Error('synthetic-private-close-fault'); });
    f.inject(session.ws, 'terminate', () => { ++terminateCalls; throw new Error('synthetic-private-terminate-fault'); });
  });
  const attached = f.attach(ticket); await assert.doesNotReject(attached.done);
  assert.ok(closeCalls > 0); assert.ok(terminateCalls > 0);
  assert.equal(attached.client.readyState, WebSocket.OPEN, 'injected closure failure must not become fabricated closed proof');
  assert.equal(f.starts().length, 0);
  assert.equal(f.relay.sessions.has(ticket.sessionId), false);
  assert.equal(f.hosts.get(170).wire.readyState, WebSocket.OPEN);
});

test('A channel cleanup throw cannot leak the outer start rejection or affect another VM', async t => {
  const f = fixture(t), other = f.create({ who: 'bobby', vmid: 149 });
  const otherAttached = f.attach(other); await otherAttached.done;
  const ticket = f.create(); let cleanupCalls = 0;
  f.beforeDispatch(() => {
    f.inject(f.store, 'authenticateHost', () => { throw new Error('synthetic-private-read-fault'); });
    f.inject(f.relay.clipboard, 'closeSession', () => { ++cleanupCalls; throw new Error('synthetic-private-cleanup-fault'); });
  });
  const attached = f.attach(ticket); await assert.doesNotReject(attached.done);
  assert.ok(cleanupCalls > 0);
  assert.equal(f.starts().length, 0);
  assert.equal(f.relay.sessions.has(ticket.sessionId), false);
  assert.equal(f.relay.sessions.get(other.sessionId), otherAttached.session);
  assert.equal(otherAttached.client.readyState, WebSocket.OPEN);
});

test('DB-failure abort preserves an older pending stop-v1 obligation exactly', async t => {
  const f = fixture(t), ticket = f.create();
  const predecessor = f.relay.sessionStop.begin({ desktopId: 'pve-170', id: randomUUID(),
    host: f.hosts.get(170).host, mode: 'stream', hostDispatched: true, stopProtocol: 'stop-v1' });
  assert.ok(predecessor);
  f.beforeDispatch(() => f.inject(f.store, 'authenticateHost', () => { throw new Error('synthetic-private-read-fault'); }));
  const attached = f.attach(ticket); await assert.doesNotReject(attached.done);
  assert.equal(f.starts().length, 0);
  assert.equal(f.relay.sessions.has(ticket.sessionId), false);
  assert.equal(f.relay.sessionStop.pending.get('pve-170'), predecessor);
  assert.equal(predecessor.completionVerified, false);
});

test('A failing old read cannot release a new ticket created by exact-own cancellation', async t => {
  const f = fixture(t), ticket = f.create(); let newer;
  f.beforeDispatch(() => {
    let restore;
    restore = f.inject(f.store, 'authenticateHost', () => {
      restore(); // Restore only this memory method before explicit own cleanup.
      f.relay.cancel(f.store.findUser('alice'), tokenHash(f.tokens.alice), ticket.sessionId);
      newer = f.create();
      throw new Error('synthetic-late-private-read-fault');
    });
  });
  const attached = f.attach(ticket); await assert.doesNotReject(attached.done);
  assert.ok(newer); assert.notEqual(newer.sessionId, ticket.sessionId);
  assert.equal(f.relay.sessions.has(ticket.sessionId), false);
  assert.equal(f.relay.sessions.has(newer.sessionId), true);
  assert.equal(f.relay.sessions.get(newer.sessionId).hostDispatched, undefined);
  assert.equal(f.starts().length, 0);
  assert.equal(f.hosts.get(170).wire.readyState, WebSocket.OPEN);
});

test('Client-close failure cannot strand its own track tunnel or close a foreign track wire', async t => {
  const f = fixture(t), caps = { ...capabilities(), videoTrackProtocol: 'tracks-v1', trackViewportProvisioning: true };
  f.advertise(caps); f.advertise(caps, 149);
  const tracks = { mediaProtocol: 'tracks-v1', tracks: [{ id: '0', width: 1280, height: 720, x: 0, y: 0 }] };
  const createTrack = (who, vmid) => f.relay.connect(f.store.findUser(who), tokenHash(f.tokens[who]), `pve-${vmid}`, 'stream', options,
    undefined, undefined, tracks);
  const foreign = createTrack('bobby', 149), foreignAttached = f.attach(foreign); await foreignAttached.done;
  const foreignWire = new MemoryWire(); f.relay.attachTrackTunnel(foreignWire, foreignAttached.session);
  const ticket = createTrack('alice', 170); let ownWire, realClose, terminateCalls = 0;
  f.beforeDispatch(session => {
    ownWire = new MemoryWire(); realClose = ownWire.close.bind(ownWire);
    f.relay.attachTrackTunnel(ownWire, session);
    f.inject(session.ws, 'close', () => { throw new Error('synthetic-private-client-close-fault'); });
    f.inject(ownWire, 'close', () => { throw new Error('synthetic-private-track-close-fault'); });
    f.inject(ownWire, 'terminate', () => { ++terminateCalls; realClose(); });
    f.inject(f.store, 'authenticateHost', () => { throw new Error('synthetic-private-read-fault'); });
  });
  try {
    const attached = f.attach(ticket); await assert.doesNotReject(attached.done);
    assert.equal(ownWire.readyState, WebSocket.CLOSED);
    assert.ok(terminateCalls > 0);
    assert.equal(foreignWire.readyState, WebSocket.OPEN);
    assert.equal(foreignAttached.client.readyState, WebSocket.OPEN);
    assert.equal(f.relay.sessions.get(foreign.sessionId), foreignAttached.session);
    assert.equal(f.relay.sessions.has(ticket.sessionId), false);
    assert.equal(f.starts().length, 0);
    assert.equal(f.hosts.get(170).wire.readyState, WebSocket.OPEN);
    assert.equal(f.hosts.get(149).wire.readyState, WebSocket.OPEN);
  } finally { realClose?.(); }
});
