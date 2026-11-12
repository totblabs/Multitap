// Actual Relay/Store, in-memory SQLite and EventEmitter wires only. No listener,
// remote request, native API or fixture data files. This tests failure routing,
// not a claim that the fake Host can create any real display or encode frames.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { Relay } from '../src/relay.mjs';
import { newToken, tokenHash } from '../src/security.mjs';

const v2 = 'stream-size-v2', v1 = 'stream-size-v1';
const reasons = ['frame_layout_unsupported', 'frame_memory_unavailable', 'frame_memory_insufficient', 'encoder_format_unsupported', 'unsupported_display_mode'];
const limits = { maxWidth: 3840, maxHeight: 3840, maxPixels: 8294400, maxPixelRate: 0 };
const options = { maxWidth: 1280, maxHeight: 720, fps: 60, bitrate: 8000000, audio: false, monitor: '0' };
const machineUuid = '12345678-1234-1234-1234-123456789abc';
const caps = {
  service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true,
  dynamicResolution: true,
  viewportProvisioning: true, virtualMonitorControl: true, resizeProtocol: 'epoch-v1',
  initialViewportProtocol: 'epoch-v1', viewportEncoderLimitsProtocol: 'separate-v1',
  streamSizeProtocol: v2, videoTrackProtocol: 'tracks-v1', trackViewportProvisioning: true,
  monitors: [{ id: '0', width: 1920, height: 1080, primary: true,
    modes: [{ width: 1920, height: 1080, refreshRate: 60 }] }],
};

class MemoryWire extends EventEmitter {
  readyState = WebSocket.OPEN;
  bufferedAmount = 0;
  messages = [];
  send(payload, { binary }, complete) {
    assert.equal(this.readyState, WebSocket.OPEN, 'no send after memory wire closure');
    this.messages.push(binary ? Buffer.from(payload) : JSON.parse(String(payload)));
    complete?.();
  }
  packet(value) { this.emit('message', Buffer.from(JSON.stringify(value)), false); }
  close(_code, reason) {
    if (this.readyState === WebSocket.CLOSED) return;
    this.closeReason = reason;
    this.readyState = WebSocket.CLOSED;
    this.emit('close');
  }
  terminate() { this.close(); }
  ping() {}
}

function fixture(t) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  store.addUser({ name: 'alice', passwordHash: 'memory-only-failure-routing' });
  store.syncProxmox([170, 149].map(vmid => ({ vmid, node: 'fixture', name: `VM${vmid}`, status: 'running' })), 'fixture');
  for (const vmid of [170, 149]) store.assign('alice', `pve-${vmid}`);
  const token = newToken(), hash = tokenHash(token);
  store.createSession(store.findUser('alice'), hash, Date.now(), 3600000, 'client');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://memory.invalid', tokenId: 'fixture@pve!fixture', secret: 'synthetic-only' }), 'fixture');
  const relay = new Relay({ store, vault, publicOrigin: 'https://failure-routing.invalid',
    authorizationMs: 3600000, heartbeatMs: 3600000 });
  const hosts = new Map(), dispatches = [];
  const dispatch = relay.dispatchSessionStart.bind(relay);
  relay.dispatchSessionStart = (...args) => { const result = dispatch(...args); dispatches.push(result); return result; };
  for (const vmid of [170, 149]) {
    const enrollment = newToken(), hostHash = tokenHash(newToken());
    store.issueEnrollment(`pve-${vmid}`, machineUuid, tokenHash(enrollment), 'fixture');
    store.consumeEnrollment(tokenHash(enrollment), machineUuid, { hostname: 'MemoryHost', version: '9.0.0', tokenHash: hostHash });
    const wire = new MemoryWire();
    relay.attachHost(wire, store.authenticateHost(hostHash), hostHash, true);
    wire.packet({ type: 'host.capabilities', capabilities: caps });
    assert.equal(wire.messages.at(-1)?.accepted, true, 'actual capability parser accepts fixture');
    hosts.set(vmid, { wire, host: relay.hosts.get(`pve-${vmid}`) });
  }
  async function open({ route = 'host', protocol = v2, vmid = 170 } = {}) {
    const tracks = route === 'track';
    const request = tracks ? {
      mediaProtocol: 'tracks-v1', tracks: [{ id: '0', width: 1280, height: 720, x: 0, y: 0 }],
      ...(protocol === v2 ? { streamSizeProtocol: v2, decoderLimits: limits } : {}),
    } : protocol ? {
      initialViewportProtocol: 'epoch-v1', initialViewport: { monitor: '0', width: 1280, height: 720, fps: 60 },
      viewportEncoderLimitsProtocol: 'separate-v1', streamSizeProtocol: protocol, decoderLimits: limits,
    } : {};
    const ticket = relay.connect(store.findUser('alice'), hash, `pve-${vmid}`, 'stream', options,
      !tracks && protocol ? 'epoch-v1' : undefined, undefined, request);
    const session = relay.sessions.get(ticket.sessionId), client = new MemoryWire();
    const at = dispatches.length;
    relay.attachClient(client, session);
    await dispatches[at];
    assert.equal(relay.sessions.get(session.id), session, 'fixture must remain admitted before failure');
    assert.equal(session.streamSizeVersion, tracks && protocol !== v2 ? null : protocol);
    let wire = hosts.get(vmid).wire;
    if (tracks) { wire = new MemoryWire(); relay.attachTrackTunnel(wire, session); }
    return { session, client, wire };
  }
  t.after(async () => { relay.close(); await Promise.allSettled(dispatches); store.close(); });
  return { relay, hosts, open };
}

for (const route of ['host', 'track']) {
  for (const reason of reasons) {
    test(`Negotiated v2 ${route} forwards only the current fixed failure: ${reason}`, async t => {
      const f = fixture(t), other = await f.open({ vmid: 149, protocol: null });
      const c = await f.open({ route });
      c.wire.packet({ type: 'stream.unavailable', sessionId: c.session.id, reason });
      assert.deepEqual(c.client.messages.find(m => m.type === 'stream.unavailable'), {
        type: 'stream.unavailable', sessionId: c.session.id, reason,
      });
      assert.equal(f.relay.sessions.has(c.session.id), false);
      assert.equal(f.hosts.get(170).wire.readyState, WebSocket.OPEN, 'valid failure preserves reusable Host wire');
      assert.equal(f.relay.sessions.get(other.session.id), other.session, 'other VM is not stopped');
      assert.equal(other.client.readyState, WebSocket.OPEN);
    });
  }

  test(`Legacy ${route} refuses every new v2 reason`, async t => {
    for (const reason of reasons) {
      const f = fixture(t), c = await f.open({ route, protocol: route === 'host' ? v1 : null });
      c.wire.packet({ type: 'stream.unavailable', sessionId: c.session.id, reason });
      assert.equal(c.client.messages.some(m => m.type === 'stream.unavailable'), false);
      assert.equal(c.client.messages.find(m => m.type === 'session.ended')?.reason,
        route === 'host' ? 'invalid_control' : 'invalid_track_control');
    }
  });

  test(`V2 ${route} refuses unknown and malformed reasons without forwarding raw text`, async t => {
    for (const reason of ['driver detail / synthetic secret', 'FRAME_MEMORY_INSUFFICIENT', 'frame_memory_insufficient\n', null, [], {}, true]) {
      const f = fixture(t), c = await f.open({ route });
      c.wire.packet({ type: 'stream.unavailable', sessionId: c.session.id, reason });
      assert.equal(c.client.messages.some(m => m.type === 'stream.unavailable'), false);
      assert.equal(c.client.messages.find(m => m.type === 'session.ended')?.reason,
        route === 'host' ? 'invalid_control' : 'invalid_track_control');
      assert.equal(JSON.stringify(c.client.messages).includes('synthetic secret'), false);
    }
  });

  test(`Generic failure remains compatible on legacy ${route}`, async t => {
    const f = fixture(t), c = await f.open({ route, protocol: null });
    c.wire.packet({ type: 'stream.unavailable', sessionId: c.session.id, reason: 'worker_failed' });
    assert.equal(c.client.messages.find(m => m.type === 'stream.unavailable')?.reason, 'worker_failed');
    assert.equal(f.hosts.get(170).wire.readyState, WebSocket.OPEN);
  });
}

test('A late unavailable from a retired Host session cannot end the current v2 controller', async t => {
  const f = fixture(t), c = await f.open();
  c.wire.packet({ type: 'stream.unavailable', sessionId: '00000000-0000-0000-0000-000000000000', reason: 'unsupported_display_mode' });
  assert.equal(f.relay.sessions.get(c.session.id), c.session);
  assert.equal(c.client.messages.some(m => m.type === 'stream.unavailable'), false);
});

test('An unknown internal size marker cannot authorize a new failure reason', async t => {
  for (const route of ['host', 'track']) {
    const f = fixture(t), c = await f.open({ route });
    c.session.streamSizeVersion = 'stream-size-v3';
    c.wire.packet({ type: 'stream.unavailable', sessionId: c.session.id, reason: 'frame_layout_unsupported' });
    assert.equal(c.client.messages.some(m => m.type === 'stream.unavailable'), false);
    assert.equal(c.client.messages.find(m => m.type === 'session.ended')?.reason,
      route === 'host' ? 'invalid_control' : 'invalid_track_control');
  }
});

test('Blocked-mode admission uses the same v2 predicate and legacy fallback', async t => {
  for (const protocol of [v2, v1, null]) {
    for (const reason of [...reasons, 'driver detail / synthetic secret']) {
      const f = fixture(t), c = await f.open({ protocol });
      // Only the readiness observation is replaced; actual current authority,
      // per-VM idle guard, unavailable() and stop() still run in production.
      f.relay.modes = () => ({ stream: false });
      f.relay.unavailableReason = () => reason;
      assert.equal(f.relay.admitSessionStart(c.session), false);
      assert.equal(c.client.messages.find(m => m.type === 'stream.unavailable')?.reason,
        protocol === v2 && reasons.includes(reason) ? reason : 'host_maintenance');
      assert.equal(f.hosts.get(170).wire.readyState, WebSocket.OPEN);
    }
  }
});
