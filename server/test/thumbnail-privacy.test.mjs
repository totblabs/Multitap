import { test } from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { Relay } from '../src/relay.mjs';
import { desktopFingerprint } from '../src/desktop-events.mjs';
import { thumbnailPrivacyProtocol, validThumbnailPrivacy, observeThumbnailPrivacy, freshThumbnailPrivacy } from '../src/thumbnail-privacy.mjs';

const first = { protocol: thumbnailPrivacyProtocol, epoch: '1234567890abcdef0000000000000001', allowed: true };
const second = { ...first, epoch: '1234567890abcdef0000000000000002' };
const caps = privacy => ({ service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true, thumbnailPrivacy: privacy });
function host(privacy = first) {
  const value = { identity: { id: 'synthetic-host', desktop_id: 'pve-710', hostname: 'fixture', version: '0.0.0' }, tokenHash: 'synthetic-token-hash', ws: { readyState: 1 }, lastSeen: Date.now(), capabilitiesPending: false, capabilitiesAt: Date.now(), capabilities: caps(privacy) };
  observeThumbnailPrivacy(value, value.capabilities);
  return value;
}
function report(value, capabilities) { observeThumbnailPrivacy(value, capabilities); value.capabilities = capabilities; value.capabilitiesAt = Date.now(); }
// Exercise actual Relay authentication, decoration, capability filtering and
// modes. No Relay constructor, timer, socket, Store, service or network is run.
function memoryRelay(value) {
  const context = {
    hosts: new Map([['pve-710', value]]), displayBusy: new Map(), heartbeatMs: 10000,
    store: { authenticateHost: token => token === 'synthetic-token-hash' ? { id: 'synthetic-host' } : null, clipboardPolicy: () => null, devicePolicy: () => ({}) },
    sessionStop: { blocked: () => false, view: () => null }, consolePreparation: { view: () => ({ supported: false }) },
    activeSession: () => null, displayView: () => ({ supported: false }),
  };
  for (const name of ['hostAuthenticated', 'modes', 'visibleCapabilities', 'decorate']) context[name] = Relay.prototype[name];
  return context;
}
const row = { id: 'pve-710', kind: 'vm', name: 'synthetic assigned VM' };
function snapshot(context) { return context.decorate([row], 'synthetic-account'); }
function exposed(context) { return snapshot(context)[0].host.thumbnailPrivacy; }

test('thumbnail privacy accepts exact readonly shape and returns a detached copy', () => {
  assert.deepEqual(validThumbnailPrivacy(first), first);
  assert.deepEqual(validThumbnailPrivacy({ ...first, allowed: false }), { ...first, allowed: false });
  const detached = validThumbnailPrivacy(first); detached.allowed = false;
  assert.equal(first.allowed, true);
});

test('thumbnail privacy rejects malformed, extra, unknown-protocol and nonboolean fields', () => {
  for (const bad of [null, undefined, [], true, 'x', {}, { ...first, extra: true },
    { ...first, protocol: 'future' }, { ...first, epoch: first.epoch.toUpperCase() },
    { ...first, epoch: 'a'.repeat(31) }, { ...first, epoch: 'a'.repeat(33) },
    { ...first, epoch: first.epoch.slice(0, 16) + '0'.repeat(16) },
    { ...first, epoch: 'g'.repeat(32) }, { ...first, allowed: 1 }, { ...first, allowed: 'false' },
    { protocol: first.protocol, epoch: first.epoch }]) assert.equal(validThumbnailPrivacy(bad), null);
});

test('fresh privacy has exact age boundaries and never infers permission from unknown time/auth', () => {
  const value = host(); value.capabilitiesAt = 1000;
  assert.deepEqual(freshThumbnailPrivacy(value, true, 30000, 31000), first);
  assert.equal(freshThumbnailPrivacy(value, true, 30000, 31001), null);
  for (const now of [999, NaN, Infinity]) assert.equal(freshThumbnailPrivacy(value, true, 30000, now), null);
  for (const auth of [false, undefined, null, 1]) assert.equal(freshThumbnailPrivacy(value, auth, 30000, 1000), null);
  value.capabilitiesPending = true;
  assert.equal(freshThumbnailPrivacy(value, true, 30000, 1000), null);
  assert.equal(freshThumbnailPrivacy(undefined, true, 30000, 1000), null);
  value.capabilitiesPending = false; delete value.thumbnailPrivacyEpochState;
  assert.equal(freshThumbnailPrivacy(value, true, 30000, 1000), null, 'Unobserved capability was trusted');
});

test('invalid new report hides only privacy immediately and requires a valid new observation', () => {
  const value = host(); value.capabilitiesAt = 1000;
  const ordinary = value.capabilities;
  observeThumbnailPrivacy(value, null);
  assert.equal(freshThumbnailPrivacy(value, true, 30000, 1000), null);
  assert.equal(value.capabilities, ordinary, 'Existing stream admission was mutated');
  observeThumbnailPrivacy(value, value.capabilities);
  assert.deepEqual(freshThumbnailPrivacy(value, true, 30000, 1000), first);
});

test('explicit denied epoch remains visible; contradictory or absent permission stays unknown', () => {
  const value = host({ ...first, allowed: false }); value.capabilitiesAt = 1000;
  value.capabilities.interactiveSession = false;
  assert.deepEqual(freshThumbnailPrivacy(value, true, 30000, 1000), { ...first, allowed: false });
  value.capabilities.thumbnailPrivacy = first;
  assert.equal(freshThumbnailPrivacy(value, true, 30000, 1000), null);
  delete value.capabilities.thumbnailPrivacy;
  assert.equal(freshThumbnailPrivacy(value, true, 30000, 1000), null);
});

test('actual assigned-desktop decorator preserves fresh epoch and omits pending/stale/auth-unknown fields', () => {
  const value = host(), context = memoryRelay(value);
  assert.deepEqual(exposed(context), first);
  value.capabilitiesPending = true;
  assert.equal(exposed(context), undefined);
  value.capabilitiesPending = false; value.capabilitiesAt = Date.now() - 30001;
  assert.equal(exposed(context), undefined);
  value.capabilitiesAt = Date.now(); value.tokenHash = 'revoked';
  assert.equal(snapshot(context)[0].host.connected, false);
  assert.equal(exposed(context), undefined);
});

test('actual decorator masks invalid new report and closed/replaced Host without disturbing ordinary caps', () => {
  const value = host(), context = memoryRelay(value);
  observeThumbnailPrivacy(value, null);
  assert.equal(exposed(context), undefined);
  assert.equal(snapshot(context)[0].host.capabilities.streaming, true);
  value.ws.readyState = 3;
  assert.equal(snapshot(context)[0].host.connected, false);
  value.ws.readyState = 1;
  context.hosts.set('pve-710', { ...value, identity: { ...value.identity, id: 'successor' } });
  assert.equal(context.hostAuthenticated(value), false, 'Old Host object kept successor authority');
  assert.equal(exposed(context), undefined);
});

test('idle lock/unlock changes actual assigned-list fingerprint even with no active stream', () => {
  const value = host(), context = memoryRelay(value);
  const before = snapshot(context);
  const lockedCaps = caps({ ...second, allowed: false });
  lockedCaps.interactiveSession = false; lockedCaps.streaming = false;
  report(value, lockedCaps);
  const locked = snapshot(context);
  assert.notEqual(desktopFingerprint(before), desktopFingerprint(locked));
  assert.deepEqual(locked[0].host.thumbnailPrivacy, { ...second, allowed: false });
  assert.equal(locked[0].host.capabilities.thumbnailPrivacy, undefined);
  report(value, caps({ ...first, epoch: '1234567890abcdef0000000000000003' }));
  const unlocked = snapshot(context);
  assert.deepEqual(before[0].host.capabilities, unlocked[0].host.capabilities, 'Ordinary capability changes contaminated the wrapper-only fingerprint proof');
  assert.equal(unlocked[0].host.capabilities.thumbnailPrivacy, undefined);
  assert.notEqual(desktopFingerprint(before), desktopFingerprint(unlocked), 'Lock/unlock returned to the old fingerprint');
  assert.notEqual(desktopFingerprint(locked), desktopFingerprint(unlocked));
});

test('privacy never grants streaming or virtual display readiness and remains per desktop', () => {
  const value = host(), context = memoryRelay(value);
  value.capabilities.streaming = false; value.capabilities.virtualDisplayReady = false;
  assert.deepEqual(exposed(context), first);
  assert.equal(snapshot(context)[0].connectionModes.stream, false);
  const other = context.decorate([{ ...row, id: 'pve-711' }], 'synthetic-account')[0];
  assert.equal(other.host.connected, false);
  assert.equal(other.host.capabilities, undefined);
});

test('physical-PC filtering preserves fresh privacy while still removing VM-only profile claims', () => {
  const value = host(), context = memoryRelay(value);
  value.capabilities.streamProfileProtocol = 'synthetic-profile';
  const view = context.visibleCapabilities(value.capabilities, 'synthetic-account', 'pve-710', true);
  assert.equal(view.thumbnailPrivacy, undefined, 'Raw Host privacy must never be forwarded as broker attestation');
  assert.equal(view.streamProfileProtocol, undefined);
  const projected = context.decorate([{ ...row, kind: 'pc' }], 'synthetic-account')[0];
  assert.deepEqual(projected.host.thumbnailPrivacy, first);
  assert.equal(projected.host.capabilities.thumbnailPrivacy, undefined);
  assert.equal(projected.host.capabilities.streamProfileProtocol, undefined);
});

test('older computed capability cannot roll the accepted epoch back on the same authenticated channel', () => {
  const value = host(), context = memoryRelay(value);
  report(value, caps(second));
  assert.deepEqual(exposed(context), second);
  report(value, caps({ ...first, allowed: false }));
  assert.equal(exposed(context), undefined, 'Old denial was exposed as a current source');
  report(value, caps(second));
  assert.deepEqual(exposed(context), second, 'Old denial burned the newer accepted epoch');
  report(value, caps(first));
  assert.equal(exposed(context), undefined);
  assert.equal(snapshot(context)[0].connectionModes.stream, true, 'Rejected privacy changed stream admission');
  assert.equal(value.thumbnailPrivacyEpochState.sequence, 2n);
  report(value, caps(second));
  assert.deepEqual(exposed(context), second);
});

test('same-epoch denial is terminal until a newer event epoch, without blocking streaming', () => {
  const value = host(), context = memoryRelay(value);
  report(value, caps({ ...first, allowed: false }));
  assert.equal(exposed(context).allowed, false);
  report(value, caps(first));
  assert.equal(exposed(context), undefined, 'Old true revived a denied source');
  assert.equal(snapshot(context)[0].connectionModes.stream, true);
  report(value, caps(second));
  assert.deepEqual(exposed(context), second);
});

test('one fixed prefix per channel rejects foreign and retired owners; fresh channel requires new observation', () => {
  const value = host(), context = memoryRelay(value);
  const foreign = { ...second, epoch: 'abcdef12345678900000000000000001' };
  report(value, caps(foreign));
  assert.equal(exposed(context), undefined);
  assert.equal(value.thumbnailPrivacyEpochState.prefix, first.epoch.slice(0, 16));
  const successor = host(foreign);
  successor.identity.id = 'successor';
  const current = memoryRelay(successor); current.store.authenticateHost = () => ({ id: 'successor' });
  assert.deepEqual(exposed(current), foreign);
  assert.equal(context.hostAuthenticated(successor), false, 'Foreign channel inherited current Host identity');

  // The actual attach/message/close/reconnect hooks over EventEmitter bytes,
  // not a WebSocket/Relay constructor or a manually reset epoch-state object.
  class MemorySocket extends EventEmitter {
    readyState = 1; bufferedAmount = 0; sent = [];
    send(payload, _options, callback) { this.sent.push(JSON.parse(payload)); callback?.(); }
    terminate() { this.readyState = 3; }
    close() { this.readyState = 3; this.emit('close'); }
    report(capabilities) { this.emit('message', Buffer.from(JSON.stringify({ type: 'host.capabilities', capabilities })), false); }
  }
  const actual = memoryRelay(undefined); actual.hosts.clear(); actual.sessions = new Map(); actual.hostCloseAudits = new Map();
  actual.store.hostSeen = () => {};
  actual.sessionStop.capabilities = () => {}; actual.sessionStop.hostLost = () => {};
  actual.qemuConsole = { hostLost() {}, capabilities() { return true; }, resume() {} };
  for (const name of ['attachHost', 'hostSession', 'dropHost', 'send', 'hostEvent']) actual[name] = Relay.prototype[name];
  const identity = { id: 'synthetic-host', desktop_id: 'pve-710', hostname: 'fixture', version: '0.0.0' };
  const oldSocket = new MemorySocket(); actual.attachHost(oldSocket, identity, 'synthetic-token-hash', true);
  assert.equal(exposed(actual), undefined, 'Attached pending Host inherited a prior channel proof');
  oldSocket.report(caps(first)); assert.deepEqual(exposed(actual), first);
  oldSocket.report(caps({ ...first, allowed: false })); assert.equal(exposed(actual).allowed, false);
  oldSocket.report(caps(first));
  assert.equal(exposed(actual), undefined);
  assert.equal(oldSocket.sent.at(-1).accepted, true, 'Rejected privacy rejected otherwise valid Host capabilities');
  assert.equal(snapshot(actual)[0].connectionModes.stream, true);
  oldSocket.report(caps(second)); assert.deepEqual(exposed(actual), second);
  oldSocket.report(caps({ ...second, extra: true }));
  assert.equal(exposed(actual), undefined, 'Malformed optional privacy kept a prior permission');
  assert.equal(oldSocket.sent.at(-1).accepted, true, 'Malformed optional privacy changed ordinary capability acceptance');
  oldSocket.report(caps(second)); assert.deepEqual(exposed(actual), second);
  oldSocket.close(); assert.equal(actual.hosts.has('pve-710'), false);
  const newSocket = new MemorySocket(); actual.attachHost(newSocket, identity, 'synthetic-token-hash', true);
  const newOwner = actual.hosts.get('pve-710');
  assert.equal(newOwner.thumbnailPrivacyEpochState, undefined);
  assert.equal(exposed(actual), undefined);
  newSocket.report(caps(foreign)); assert.deepEqual(exposed(actual), foreign);
  oldSocket.report(caps(first));
  assert.equal(actual.hosts.get('pve-710'), newOwner, 'Late old Host retired its successor');
  assert.deepEqual(exposed(actual), foreign, 'Late old socket changed the successor epoch');
});
