import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { cursorCapability, cursorTracksCapability, requestedCursor, requestedCursorTracks, negotiateCursor, cursorState, parseCursorRecord, cursorPacket, trackCursorPacket, cursorSource, cursorMessagesPerSecond, cursorBytesPerSecond } from '../src/cursor.mjs';
import { maximumGeometrySide } from '../src/video-geometry.mjs';
import { requests as trackRequests, options as trackOptions, info as trackInfo, packet as trackPacket, delta as trackDelta } from './track-fixture.mjs';

// Negotiated cursor separation (cursor-v1): exact negotiation, strict relay
// validation of binary kind-3 records, and released peers that keep exactly
// today's behaviour. Real broker and WebSockets, fake host/client.
const origin = 'https://cursor-fixture.example';
const password = 'Cursor-fixture-password-only!';
const machineUuid = '12345678-1234-1234-1234-123456789abe';
const releasedHost = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true };
const cursorHost = { ...releasedHost, cursorProtocol: 'cursor-v1' };
const trackHost = { ...cursorHost, videoTrackProtocol: 'tracks-v1', trackViewportProvisioning: true, monitors: trackRequests.map(t => ({ id: t.id, width: t.width, height: t.height, primary: t.id === '0' })) };
const cursorTracksHost = { ...trackHost, cursorTracksProtocol: 'cursor-tracks-v1' };
const streamInfo = { type: 'stream.info', codec: 'h264', width: 1920, height: 1080, sourceWidth: 1920, sourceHeight: 1080, fpsTarget: 60, audio: { sampleRate: 48000, channels: 2, format: 's16le', available: true } };

// Wire builders (docs/remote-session-protocol.md, cursor-v1).
function header(kind, length, { epoch = 0, pts = 1000, flags = 0 } = {}) {
  const data = Buffer.alloc(12 + length);
  data[0] = kind; data[1] = flags; data.writeUInt16LE(epoch, 2); data.writeBigUInt64LE(BigInt(pts), 4);
  return data;
}
function shape({ id = 1, format = 1, width = 32, height = 32, hotspotX = 0, hotspotY = 0, ...envelope } = {}) {
  const stride = ((width + 31) >>> 5) * 4, rows = format === 1 ? height : 2 * height;
  const data = header(3, 16 + stride * rows + (format === 1 ? width * height * 4 : 0), envelope);
  data[12] = 1; data[13] = format; data.writeUInt32LE(id, 16);
  data.writeUInt16LE(width, 20); data.writeUInt16LE(height, 22); data.writeUInt16LE(hotspotX, 24); data.writeUInt16LE(hotspotY, 26);
  // Every mask bit inside the width set, every padding bit zero.
  const used = (width + 7) >>> 3, last = width % 8 ? (0xff << (8 - width % 8)) & 0xff : 0xff;
  for (let row = 0; row < rows; row++) { const at = 28 + row * stride; data.fill(0xff, at, at + used - 1); data[at + used - 1] = last; }
  if (format === 1) for (let n = 28 + stride * rows; n < data.length; n += 4) { data[n] = 40; data[n + 1] = 80; data[n + 2] = 120; data[n + 3] = 255; }
  return data;
}
function state({ id = 1, visible = true, x, y, flags, ...envelope } = {}) {
  const data = header(3, 16, envelope);
  data[12] = 2; data[13] = flags ?? ((visible ? 1 : 0) | (x !== undefined ? 2 : 0));
  data.writeUInt32LE(visible ? id : 0, 16); data.writeInt32LE(x ?? 0, 20); data.writeInt32LE(y ?? 0, 24);
  return data;
}
// cursor-tracks-v1: the same record in its TVT1 envelope (kind 3, connector 255, track epoch 0).
const trackCursor = (record, topology = 1) => trackPacket({ kind: 3, track: 255, epoch: 0, flags: 0, topology, pts: 1000, payload: record.subarray(12) });
const video = (key = true, epoch = 0) => { const data = header(1, 5, { epoch, flags: key ? 1 : 0 }); Buffer.from([0, 0, 1, key ? 0x65 : 0x41, 1]).copy(data, 12); return data; };
const audio = () => { const data = header(2, 8, { pts: 2000 }); data.fill(7, 12); return data; };

async function fixture(t, { heartbeatMs = 200 } = {}) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const hash = await hashPassword(password);
  store.addUser({ name: 'admin', passwordHash: hash, role: 'admin' });
  store.addUser({ name: 'alice', passwordHash: hash, role: 'user' });
  store.syncProxmox([{ vmid: 172, node: 'pve', name: 'VM 172', status: 'running' }], 'fixture');
  store.assign('alice', 'pve-172');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve-fixture.example', tokenId: 'fixture@pve!broker', secret: 'only-fixture-secret' }), 'fixture');
  const proxmox = { machineIdentity: async () => ({ machineUuid }) };
  const server = await buildServer({ store, vault, proxmox, publicOrigin: origin, relayOptions: { authorizationMs: 20, heartbeatMs }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const base = `http://127.0.0.1:${server.address().port}`;
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  async function call(path, { method = 'GET', data, token, cookie, csrf } = {}) {
    const response = await fetch(base + '/api/v1' + path, { method,
      headers: { ...(token ? { Authorization: 'Bearer ' + token } : {}), ...(cookie ? { Cookie: cookie } : {}), ...(method !== 'GET' ? { 'Content-Type': 'application/json', Origin: origin, ...(csrf ? { 'X-CSRF-Token': csrf } : {}) } : {}) },
      body: data === undefined ? undefined : JSON.stringify(data) });
    return { status: response.status, body: await response.json() };
  }
  const adminLogin = await fetch(base + '/api/v1/admin/login', { method: 'POST', headers: { 'Content-Type': 'application/json', Origin: origin }, body: JSON.stringify({ username: 'admin', password }) });
  const admin = { cookie: adminLogin.headers.get('set-cookie').split(';')[0], csrf: (await adminLogin.json()).csrfToken };
  const alice = (await call('/auth/login', { method: 'POST', data: { username: 'alice', password } })).body.sessionToken;
  const code = (await call('/admin/desktops/pve-172/enrollment', { ...admin, method: 'POST', data: {} })).body.code;
  const wsUrl = path => base.replace('http:', 'ws:') + path;
  let hostToken;
  async function host(capabilities) {
    const enrolled = await call('/hosts/enroll', { method: 'POST', data: { code, hostname: 'Fixture VM', machineUuid, version: '0.2.50' } });
    assert.equal(enrolled.status, 201); hostToken = enrolled.body.hostToken;
    const ws = await socket(wsUrl('/api/v1/hosts/channel'), 'Host ' + hostToken);
    ws.json({ type: 'host.capabilities', capabilities });
    assert.equal((await ws.take(value => value.type === 'host.capabilities.ack')).accepted, true);
    return ws;
  }
  const connect = extra => call('/desktops/pve-172/connect', { method: 'POST', token: alice, data: { mode: 'stream', ...extra } });
  const client = sessionId => socket(wsUrl('/api/v1/sessions/' + sessionId), 'Bearer ' + alice);
  async function attached(hostSocket, extra = {}) {
    const response = await connect(extra);
    assert.equal(response.status, 200, JSON.stringify(response.body));
    const controller = await client(response.body.sessionId);
    const start = await hostSocket.take(value => value.type === 'session.start');
    return { response, controller, start, sessionId: response.body.sessionId };
  }
  // An attached, ready stream: connect, attach the controller, deliver stream.info.
  async function stream(hostSocket, extra = {}) {
    const result = await attached(hostSocket, extra);
    hostSocket.json(streamInfo); await result.controller.take(value => value.type === 'stream.info');
    return result;
  }
  const tunnel = start => socket(wsUrl(new URL(start.tunnelUrl).pathname), 'Host ' + hostToken);
  return { call, alice, host, connect, client, attached, stream, tunnel };
}
async function socket(url, authorization) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url, { headers: { Authorization: authorization }, perMessageDeflate: false });
    ws.once('error', reject);
    ws.once('open', () => {
      const queue = [], waiters = [];
      ws.on('message', (data, binary) => {
        const value = binary ? data : JSON.parse(data.toString('utf8'));
        const index = waiters.findIndex(waiter => waiter.predicate(value));
        if (index >= 0) { const waiter = waiters.splice(index, 1)[0]; clearTimeout(waiter.timer); waiter.resolve(value); } else queue.push(value);
      });
      ws.queue = queue;
      ws.take = (predicate = () => true) => {
        const index = queue.findIndex(predicate);
        if (index >= 0) return Promise.resolve(queue.splice(index, 1)[0]);
        return new Promise((done, fail) => { const waiter = { predicate, resolve: done, timer: setTimeout(() => fail(new Error('message_timeout')), 2000) }; waiters.push(waiter); });
      };
      ws.json = value => ws.send(JSON.stringify(value));
      resolve(ws);
    });
  });
}
const closed = ws => new Promise(resolve => { if (ws.readyState === WebSocket.CLOSED) resolve(); else ws.once('close', resolve); });
const code = fn => { try { fn(); return 'accepted'; } catch (error) { return error.code; } };
// Everything the host sent before the pong has reached the controller.
async function barrier(host, controller, id) { host.json({ type: 'pong', id }); await controller.take(value => value.type === 'pong' && value.id === id); }
async function hostOnline(host) { host.json({ type: 'host.heartbeat' }); await host.take(value => value.type === 'host.heartbeat.ack'); assert.equal(host.readyState, WebSocket.OPEN); }
const binaries = ws => ws.queue.filter(Buffer.isBuffer);

test('Capability, request and negotiation are exact; separation never refuses a connection', () => {
  assert.equal(cursorCapability(cursorHost), true);
  for (const value of [releasedHost, { ...releasedHost, cursorProtocol: 'cursor-v2' }, { ...releasedHost, cursorProtocol: true }, undefined, null])
    assert.equal(cursorCapability(value), false);
  assert.equal(requestedCursor('stream', undefined), false);
  assert.equal(requestedCursor('stream', 'cursor-v1'), true);
  for (const [mode, protocol] of [['rdp', 'cursor-v1'], ['stream', 'cursor-v2'], ['stream', ''], ['stream', 1], ['stream', null], ['stream', true]])
    assert.equal(code(() => requestedCursor(mode, protocol)), 'invalid_cursor_protocol', `${mode} ${protocol}`);
  const capable = { capabilities: cursorHost }, released = { capabilities: releasedHost };
  assert.equal(negotiateCursor(true, capable, null), true);
  assert.equal(negotiateCursor(false, capable, null), false, 'never granted unrequested');
  assert.equal(negotiateCursor(true, released, null), false, 'released host: composed, not refused');
  assert.equal(negotiateCursor(true, { capabilities: undefined }, null), false);
  assert.equal(negotiateCursor(true, capable, [{ id: '0' }]), false, 'tracks-v1 keeps the composed cursor');
});

test('cursor-tracks-v1: capability, request and negotiation are exact; never a refusal', () => {
  assert.equal(cursorTracksCapability(cursorTracksHost), true);
  for (const value of [trackHost, { ...trackHost, cursorTracksProtocol: 'cursor-tracks-v2' }, { ...releasedHost, cursorTracksProtocol: 'cursor-tracks-v1' }, undefined])
    assert.equal(cursorTracksCapability(value), false);
  const tracks = [{ id: '0' }];
  assert.equal(requestedCursorTracks('stream', 'cursor-v1', undefined, tracks), false);
  assert.equal(requestedCursorTracks('stream', undefined, undefined, null), false);
  assert.equal(requestedCursorTracks('stream', 'cursor-v1', 'cursor-tracks-v1', tracks), true);
  for (const [mode, protocol, outputs, requested] of [['stream', undefined, 'cursor-tracks-v1', tracks], ['stream', 'cursor-v1', 'cursor-tracks-v2', tracks], ['stream', 'cursor-v1', 'cursor-tracks-v1', null],
    ['rdp', 'cursor-v1', 'cursor-tracks-v1', tracks], ['stream', 'cursor-v1', null, tracks], ['stream', 'cursor-v1', true, tracks]])
    assert.equal(code(() => requestedCursorTracks(mode, protocol, outputs, requested)), 'invalid_cursor_protocol', `${mode} ${protocol} ${outputs} ${!!requested}`);
  const outputs = { capabilities: cursorTracksHost }, single = { capabilities: trackHost };
  assert.equal(negotiateCursor(true, outputs, tracks, true), true);
  assert.equal(negotiateCursor(true, outputs, tracks, false), false, 'cursor-v1 alone keeps the composed cursor of independent outputs');
  assert.equal(negotiateCursor(true, single, tracks, true), false, 'a Host without the marker: composed, not refused');
  assert.equal(negotiateCursor(false, outputs, tracks, true), false);
  assert.equal(negotiateCursor(true, outputs, null, false), true, 'a single output is unchanged');
});

test('cursor-tracks-v1 relay decisions: own TVT1 kind, granted session only, topology/key gates, no positions', () => {
  const keyed = () => new Map([['0', { key: true }], ['1', { key: true }]]);
  const ready = () => ({ mode: 'stream', cursorTracks: true, cursor: cursorState(), trackState: { info: { topologyEpoch: 2 }, pending: null, video: keyed() } });
  const edit = (value, change) => { const copy = Buffer.from(value); change(copy); return copy; };
  const good = trackCursor(shape({ id: 1 }), 2);
  for (const [name, value] of Object.entries({
    'TVT1 magic': edit(good, d => { d[0] = 0x58; }), 'flags': edit(good, d => { d[5] = 1; }), 'connector 0': edit(good, d => { d[6] = 0; }), 'byte 7': edit(good, d => { d[7] = 1; }),
    'topology 0': trackCursor(shape({ id: 1 }), 0), 'track epoch': edit(good, d => { d.writeUInt32LE(1, 12); }), 'pts beyond 2^53': edit(good, d => { d.writeBigUInt64LE(2n ** 53n, 16); }),
    'length': Buffer.concat([good, Buffer.alloc(1)]), 'malformed record': trackCursor(shape({ hotspotX: 40 }), 2), 'position': trackCursor(state({ id: 1, x: 1, y: 1 }), 2),
    'hidden position': trackCursor(state({ visible: false, x: 0, y: 0 }), 2), 'legacy envelope': shape(), 'not a buffer': [...good] }))
    assert.equal(trackCursorPacket(ready(), value), 'invalid', name);
  for (const [name, change] of [['ungranted', s => { s.cursor = null; }], ['single-output grant', s => { s.cursorTracks = false; }], ['no tracks', s => { s.trackState = null; }]])
    { const session = ready(); change(session); assert.equal(trackCursorPacket(session, good), 'invalid', name); }
  for (const [name, change] of [['before info', s => { s.trackState.info = null; }], ['resize pending', s => { s.trackState.pending = { topologyEpoch: 3 }; }],
    ['older topology', s => { s.trackState.info.topologyEpoch = 3; }], ['before a key frame', s => { s.trackState.video.get('1').key = false; }]])
    { const session = ready(); change(session); assert.equal(trackCursorPacket(session, good), 'discard', name); assert.equal(session.cursor.lastShapeId, 0, name + ': ids do not advance'); }
  { const session = ready(); session.trackState.info.topologyEpoch = 1; assert.equal(trackCursorPacket(session, good), 'invalid', 'an unannounced topology'); }
  const session = ready();
  assert.equal(trackCursorPacket(session, trackCursor(state({ id: 1 }), 2)), 'invalid', 'a visible state cannot name an unsent shape');
  assert.equal(trackCursorPacket(session, trackCursor(state({ visible: false }), 2)), 'forward');
  assert.equal(trackCursorPacket(session, trackCursor(shape({ id: 3 }), 2)), 'forward');
  assert.equal(trackCursorPacket(session, trackCursor(state({ id: 2 }), 2)), 'forward', 'an older id of this session stays nameable');
  assert.equal(trackCursorPacket(session, trackCursor(shape({ id: 3 }), 2)), 'invalid', 'shape ids strictly increase');
  // The ids continue across a resize of the same session.
  session.trackState.info.topologyEpoch = 3;
  assert.equal(trackCursorPacket(session, trackCursor(shape({ id: 3 }), 3)), 'invalid'); assert.equal(trackCursorPacket(session, trackCursor(shape({ id: 4 }), 3)), 'forward');
  const bounded = ready(); bounded.trackState.pending = { topologyEpoch: 3 };
  for (let n = 0; n < cursorMessagesPerSecond; n++) assert.equal(trackCursorPacket(bounded, trackCursor(state({ visible: false }), 2), 5000 + n), 'discard');
  assert.equal(trackCursorPacket(bounded, trackCursor(state({ visible: false }), 2), 5500), 'rate');
  assert.equal(trackCursorPacket(bounded, trackCursor(state({ visible: false }), 2), 6000), 'discard', 'the next second starts a new window');
  // The legacy host-channel path never accepts records for a track session.
  assert.equal(cursorPacket(ready(), shape()), 'discard');
});

test('Record structure: every documented shape/state accepted, every malformed field rejected', () => {
  for (const value of [shape(), shape({ format: 2 }), shape({ width: 1, height: 1 }), shape({ width: 33, height: 19, hotspotX: 32, hotspotY: 18 }),
    shape({ width: 256, height: 256 }), shape({ width: 256, height: 256, format: 2 }), shape({ id: 2147483647 }), shape({ epoch: 65535, pts: 2 ** 53 })])
    assert.equal(parseCursorRecord(value)?.type, 'shape');
  assert.deepEqual(parseCursorRecord(shape({ id: 9, width: 33, height: 19, hotspotX: 4, hotspotY: 5, format: 2 })), { type: 'shape', format: 2, id: 9, width: 33, height: 19, hotspotX: 4, hotspotY: 5 });
  assert.deepEqual(parseCursorRecord(state({ id: 4 })), { type: 'state', id: 4, visible: true, position: false, x: 0, y: 0 });
  assert.deepEqual(parseCursorRecord(state({ visible: false })), { type: 'state', id: 0, visible: false, position: false, x: 0, y: 0 });
  assert.deepEqual(parseCursorRecord(state({ id: 4, x: 15, y: 16000 })), { type: 'state', id: 4, visible: true, position: true, x: 15, y: 16000 });
  assert.equal(parseCursorRecord(state({ visible: false, x: 0, y: 0 }))?.position, true, 'a hidden cursor may still be warped');
  const edit = (value, change) => { const copy = Buffer.from(value); change(copy); return copy; };
  const bad = {
    'kind 1': edit(shape(), d => { d[0] = 1; }), 'header flags': edit(shape(), d => { d[1] = 1; }), 'empty payload': header(3, 0), 'short state': header(3, 15),
    'record type 0': edit(state(), d => { d[12] = 0; }), 'record type 3': edit(state(), d => { d[12] = 3; }),
    'shape format 0': edit(shape(), d => { d[13] = 0; }), 'shape format 3': edit(shape(), d => { d[13] = 3; }), 'reserved byte 2': edit(shape(), d => { d[14] = 1; }), 'reserved byte 3': edit(state(), d => { d[15] = 1; }),
    'shape id 0': shape({ id: 0 }), 'shape id 2^31': shape({ id: 2147483648 }), 'width 0': edit(shape(), d => { d.writeUInt16LE(0, 20); }), 'height 257': edit(shape(), d => { d.writeUInt16LE(257, 22); }),
    'hotspot x = width': shape({ hotspotX: 32 }), 'hotspot y = height': shape({ hotspotY: 32 }),
    'one byte short': shape().subarray(0, shape().length - 1), 'one byte long': Buffer.concat([shape(), Buffer.alloc(1)]),
    'colour length for monochrome': edit(shape({ format: 1 }), d => { d[13] = 2; }),
    'padding bit set': edit(shape({ width: 33 }), d => { d[28 + 4] |= 0x40; }), 'padding byte set': edit(shape({ width: 33 }), d => { d[28 + 7] = 1; }),
    'state flag bit 2': state({ flags: 5 }), 'visible id 0': edit(state(), d => { d.writeUInt32LE(0, 16); }), 'hidden id 1': edit(state({ visible: false }), d => { d.writeUInt32LE(1, 16); }),
    'x without position': edit(state(), d => { d.writeInt32LE(1, 20); }), 'y without position': edit(state(), d => { d.writeInt32LE(1, 24); }),
    'negative x': state({ x: -1, y: 0 }), 'y beyond source ABI': state({ x: 0, y: maximumGeometrySide }), 'state length 17': Buffer.concat([state(), Buffer.alloc(1)]),
    'not a buffer': [...state()],
  };
  for (const [name, value] of Object.entries(bad)) assert.equal(parseCursorRecord(value), null, name);
});

test('Cursor coordinates above16K retain the shared signed source ABI', () => {
  for (const [x,y] of [[16384,0],[0,16384],[32767,32767],[maximumGeometrySide-1,0],[0,maximumGeometrySide-1]]) {
    const parsed = parseCursorRecord(state({x,y}));
    assert.deepEqual(parsed, {type:'state',id:1,visible:true,position:true,x,y});
  }
  for (const [x,y] of [[-1,0],[0,-1],[maximumGeometrySide,0],[0,maximumGeometrySide],[0x7fffffff,0]]) {
    assert.equal(parseCursorRecord(state({x,y})), null, 'negative or outside shared signed source ABI');
  }
  const wrapped=state({x:0,y:0});wrapped.writeUInt32LE(0x80000000,20);
  assert.equal(parseCursorRecord(wrapped),null,'unsigned high bit cannot become a signed positive coordinate');
});

test('Large-source cursor relay remains bounded by the current confirmed source and epoch', () => {
  const session = {mode:'stream',streamReady:true,resize:null,awaitingKeyFrame:false,epoch:1,trackState:null,cursor:cursorState()};
  cursorSource(session,{...streamInfo,sourceWidth:32768,sourceHeight:2160});
  assert.equal(cursorPacket(session,shape({epoch:1})), 'forward');
  assert.equal(cursorPacket(session,state({x:32767,y:2159,epoch:1})), 'forward');
  assert.equal(cursorPacket(session,state({x:32768,y:0,epoch:1})), 'invalid','x == observed width is still outside');
  assert.equal(cursorPacket(session,state({x:0,y:2160,epoch:1})), 'invalid','y == observed height is still outside');
  session.resize={epoch:2};
  assert.equal(cursorPacket(session,state({x:32767,y:0,epoch:1})), 'discard','pending resize has no current cursor target');
  session.resize=null;session.epoch=2;
  cursorSource(session,{...streamInfo,sourceWidth:5120,sourceHeight:1440});
  assert.equal(cursorPacket(session,state({x:32767,y:0,epoch:1})), 'discard','late old epoch cannot target the new source');
  assert.equal(cursorPacket(session,state({x:32767,y:0,epoch:2})), 'invalid','new epoch cannot use old larger source bounds');
  assert.equal(cursorPacket(session,state({x:5119,y:1439,epoch:2})), 'forward');
});

test('Portrait cursor source above16K keeps its own independent source bounds', () => {
  const session={mode:'stream',streamReady:true,resize:null,awaitingKeyFrame:false,epoch:0,trackState:null,cursor:cursorState()};
  cursorSource(session,{...streamInfo,sourceWidth:2160,sourceHeight:32768});
  assert.equal(cursorPacket(session,shape()),'forward');
  assert.equal(cursorPacket(session,state({x:2159,y:32767})),'forward');
  assert.equal(cursorPacket(session,state({x:2160,y:32767})),'invalid');
  assert.equal(cursorPacket(session,state({x:0,y:32768})),'invalid');
});

test('Relay decisions: discard outside a current granted decoder, stateful ids/positions, per-second bounds', () => {
  const ready = () => ({ mode: 'stream', streamReady: true, resize: null, awaitingKeyFrame: false, epoch: 0, trackState: null, cursor: cursorState() });
  for (const [name, change] of [['ungranted', s => { s.cursor = null; }], ['rdp', s => { s.mode = 'rdp'; }], ['tracks', s => { s.trackState = {}; }], ['before stream.info', s => { s.streamReady = false; }],
    ['resize pending', s => { s.resize = { epoch: 1 }; }], ['before the key frame', s => { s.awaitingKeyFrame = true; }], ['other epoch', s => { s.epoch = 2; }]]) {
    const session = ready(); change(session);
    assert.equal(cursorPacket(session, shape()), 'discard', name);
    assert.equal(cursorPacket(session, shape({ format: 3 })), 'invalid', name + ': malformed is always invalid');
  }
  assert.equal(cursorPacket(null, shape()), 'discard', 'no bound session');
  const session = ready(); cursorSource(session, streamInfo);
  assert.equal(cursorPacket(session, state({ id: 1 })), 'invalid', 'a visible state cannot name an unsent shape');
  assert.equal(cursorPacket(session, state({ visible: false })), 'forward');
  assert.equal(cursorPacket(session, shape({ id: 3 })), 'forward');
  assert.equal(cursorPacket(session, state({ id: 3 })), 'forward');
  assert.equal(cursorPacket(session, state({ id: 2 })), 'forward', 'an older id of this session stays nameable');
  assert.equal(cursorPacket(session, state({ id: 4 })), 'invalid');
  assert.equal(cursorPacket(session, shape({ id: 3 })), 'invalid', 'shape ids strictly increase');
  assert.equal(cursorPacket(session, shape({ id: 2 })), 'invalid');
  assert.equal(cursorPacket(session, state({ id: 3, x: 1919, y: 1079 })), 'forward');
  assert.equal(cursorPacket(session, state({ id: 3, x: 1920, y: 0 })), 'invalid', 'position outside the source');
  assert.equal(cursorPacket(session, state({ id: 3, x: 0, y: 1080 })), 'invalid');
  // Discarded records never advance the session's ids.
  const gated = ready(); gated.streamReady = false;
  assert.equal(cursorPacket(gated, shape({ id: 7 })), 'discard'); gated.streamReady = true;
  assert.equal(cursorPacket(gated, shape({ id: 1 })), 'forward');
  // A resize keeps the session's ids: the next epoch continues above them.
  gated.epoch = 1; assert.equal(cursorPacket(gated, shape({ id: 1, epoch: 1 })), 'invalid'); assert.equal(cursorPacket(gated, shape({ id: 2, epoch: 1 })), 'forward');
  // Message bound per second (also counts discarded records of the session).
  const bounded = ready(); bounded.streamReady = false;
  for (let n = 0; n < cursorMessagesPerSecond; n++) assert.equal(cursorPacket(bounded, state({ visible: false }), 5000 + n), 'discard');
  assert.equal(cursorPacket(bounded, state({ visible: false }), 5500), 'rate');
  assert.equal(cursorPacket(bounded, state({ visible: false }), 6000), 'discard', 'the next second starts a new window');
  // Payload bound per second: fifteen maximal colour shapes exceed 4 MiB.
  const big = ready(), maximal = shape({ width: 256, height: 256 });
  let decision, sent = 0;
  for (let id = 1; id <= 20 && decision !== 'rate'; id++) { decision = cursorPacket(big, shape({ id, width: 256, height: 256 }), 9000); if (decision === 'forward') sent += maximal.length - 12; }
  assert.equal(decision, 'rate'); assert.ok(sent <= cursorBytesPerSecond && sent + maximal.length - 12 > cursorBytesPerSecond);
});

test('Inventory passes the capability exactly; others stay valid', async t => {
  const f = await fixture(t); await f.host({ ...releasedHost, cursorProtocol: 'cursor-v2', clipboard: true });
  const desktop = (await f.call('/desktops', { token: f.alice })).body.desktops[0];
  assert.equal('cursorProtocol' in desktop.host.capabilities, false, 'an unknown value is omitted, not passed');
  assert.equal(desktop.host.capabilities.streaming, true, 'and never invalidates the other capabilities');
  const g = await fixture(t); await g.host(cursorHost);
  assert.equal((await g.call('/desktops', { token: g.alice })).body.desktops[0].host.capabilities.cursorProtocol, 'cursor-v1');
});

test('Malformed requests are refused before any session; RDP never carries the marker', async t => {
  const f = await fixture(t); await f.host({ ...cursorHost });
  for (const value of ['cursor-v2', 1, '', null]) {
    const response = await f.connect({ cursorProtocol: value });
    assert.equal(response.status, 400); assert.equal(response.body.error.code, 'invalid_cursor_protocol', String(value));
  }
  // RDP is not offered: refused as such, so it never carries the marker.
  const rdp = await f.call('/desktops/pve-172/connect', { method: 'POST', token: f.alice, data: { mode: 'rdp', cursorProtocol: 'cursor-v1' } });
  assert.equal(rdp.status, 409); assert.equal(rdp.body.error.code, 'rdp_not_supported'); assert.equal(rdp.body.cursorProtocol, undefined);
  const plain = await f.connect({});
  assert.equal(plain.status, 200, 'a released client request is unchanged'); assert.equal(plain.body.cursorProtocol, undefined);
});

test('Released host: requested but not granted; cursor records are never forwarded, kinds 1/2 exactly as before', async t => {
  const f = await fixture(t), host = await f.host(releasedHost);
  const { response, controller, start } = await f.stream(host, { cursorProtocol: 'cursor-v1' });
  assert.equal(response.body.cursorProtocol, undefined); assert.equal('cursorProtocol' in start, false);
  host.send(video()); assert.deepEqual(await controller.take(Buffer.isBuffer), video());
  host.send(shape()); host.send(state());
  host.send(audio()); assert.deepEqual(await controller.take(Buffer.isBuffer), audio());
  await barrier(host, controller, 'released-host'); await hostOnline(host);
  assert.equal(binaries(controller).length, 0, 'well-formed records of an ungranted session are discarded');
  // A released media envelope violation still drops the host as before.
  host.send(header(4, 8)); await closed(host);
  assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_media');
});

test('Released client with a separating host: no grant, the host is never asked to separate', async t => {
  const f = await fixture(t), host = await f.host(cursorHost);
  const { response, controller, start } = await f.stream(host);
  assert.equal(response.body.cursorProtocol, undefined); assert.equal('cursorProtocol' in start, false);
  host.send(shape()); host.send(video(false)); assert.deepEqual(await controller.take(Buffer.isBuffer), video(false));
  await barrier(host, controller, 'released-client'); await hostOnline(host);
  assert.equal(binaries(controller).length, 0);
});

test('Granted session: marker in response and session.start, exact records forwarded after stream.info only', async t => {
  const f = await fixture(t), host = await f.host(cursorHost);
  const { response, controller, start } = await f.attached(host, { cursorProtocol: 'cursor-v1' });
  assert.equal(response.body.cursorProtocol, 'cursor-v1'); assert.equal(start.cursorProtocol, 'cursor-v1');
  // Before stream.info nothing belongs to a decoder and the ids do not advance.
  host.send(shape({ id: 5 })); host.json(streamInfo); await controller.take(value => value.type === 'stream.info');
  await barrier(host, controller, 'before-ready'); assert.equal(binaries(controller).length, 0);
  const sequence = [video(), shape({ id: 1, width: 33, height: 19, hotspotX: 3, hotspotY: 4 }), state({ id: 1 }), shape({ id: 2, format: 2 }), state({ id: 2, x: 1919, y: 1079 }), state({ visible: false }), video(false)];
  for (const value of sequence) host.send(value);
  for (const value of sequence) assert.deepEqual(await controller.take(Buffer.isBuffer), value, 'order and bytes are unchanged');
  // A stale epoch (media-v1 has epoch 0) is discarded without dropping the host.
  host.send(state({ id: 2, epoch: 1 })); await barrier(host, controller, 'stale-epoch'); await hostOnline(host);
  assert.equal(binaries(controller).length, 0);
});

async function dropped(t, packets, reason) {
  const f = await fixture(t), host = await f.host(cursorHost);
  const { controller } = await f.stream(host, { cursorProtocol: 'cursor-v1' });
  for (const value of packets) host.send(value);
  await closed(host);
  assert.equal((await controller.take(value => value.type === 'session.ended')).reason, reason);
}
test('Granted session: a malformed record drops the host channel', async t => { await dropped(t, [shape({ hotspotX: 40 })], 'invalid_media'); });
test('Granted session: a non-increasing shape id drops the host channel', async t => { await dropped(t, [shape({ id: 2 }), shape({ id: 2 })], 'invalid_media'); });
test('Granted session: a state naming an unsent shape drops the host channel', async t => { await dropped(t, [shape({ id: 1 }), state({ id: 2 })], 'invalid_media'); });
test('Granted session: a position outside the stream.info source drops the host channel', async t => { await dropped(t, [shape({ id: 1 }), state({ id: 1, x: 1920, y: 5 })], 'invalid_media'); });
test('Granted session: more than 100 cursor messages per second drop the host channel', async t => {
  await dropped(t, [shape({ id: 1 }), ...Array.from({ length: cursorMessagesPerSecond }, () => state({ id: 1 }))], 'host_rate_limit');
});

test('tracks-v1 without cursor-tracks-v1 never grants cursor-v1; legacy kind-3 records cannot reach its controller', async t => {
  const f = await fixture(t, { heartbeatMs: 10000 }), host = await f.host(trackHost);
  assert.equal((await f.connect({ options: trackOptions, mediaProtocol: 'tracks-v1', tracks: trackRequests, cursorProtocol: 'cursor-v2' })).body.error.code, 'invalid_cursor_protocol');
  const { response, controller, start, sessionId } = await f.attached(host, { options: trackOptions, mediaProtocol: 'tracks-v1', tracks: trackRequests, cursorProtocol: 'cursor-v1' });
  assert.equal(response.body.transport, 'media-tracks-v1'); assert.equal(response.body.cursorProtocol, undefined); assert.equal('cursorProtocol' in start, false);
  const source = await f.tunnel(start);
  source.json(trackInfo(sessionId)); await controller.take(value => value.type === 'stream.tracks.info');
  // The reusable host channel of a track session discards it and stays online.
  host.send(shape()); await hostOnline(host);
  source.json({ type: 'pong', id: 'track-barrier' }); await controller.take(value => value.type === 'pong');
  assert.equal(binaries(controller).length, 0);
  // The session-bound track source accepts only TVT1 media.
  source.send(shape());
  assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_track_media');
  assert.equal(binaries(controller).length, 0);
});

const outputs = { options: trackOptions, mediaProtocol: 'tracks-v1', tracks: trackRequests };
const bothMarkers = { cursorProtocol: 'cursor-v1', cursorTracksProtocol: 'cursor-tracks-v1' };

test('cursor-tracks-v1: inventory passes the marker only with cursor-v1; malformed requests are refused', async t => {
  const f = await fixture(t, { heartbeatMs: 10000 }); await f.host(cursorTracksHost);
  const capabilities = (await f.call('/desktops', { token: f.alice })).body.desktops[0].host.capabilities;
  assert.equal(capabilities.cursorTracksProtocol, 'cursor-tracks-v1'); assert.equal(capabilities.cursorProtocol, 'cursor-v1');
  for (const extra of [{ ...outputs, cursorTracksProtocol: 'cursor-tracks-v1' }, { ...outputs, ...bothMarkers, cursorTracksProtocol: 'cursor-tracks-v2' }, bothMarkers]) {
    const response = await f.connect(extra);
    assert.equal(response.status, 400); assert.equal(response.body.error.code, 'invalid_cursor_protocol', JSON.stringify(extra));
  }
  const g = await fixture(t, { heartbeatMs: 10000 }); await g.host({ ...trackHost, cursorProtocol: undefined, cursorTracksProtocol: 'cursor-tracks-v1' });
  assert.equal('cursorTracksProtocol' in (await g.call('/desktops', { token: g.alice })).body.desktops[0].host.capabilities, false, 'never without cursor-v1');
});

test('cursor-tracks-v1 granted: markers in response and session.start, records forwarded unchanged after both key frames', async t => {
  const f = await fixture(t, { heartbeatMs: 10000 }), host = await f.host(cursorTracksHost);
  const { response, controller, start, sessionId } = await f.attached(host, { ...outputs, ...bothMarkers });
  assert.equal(response.body.cursorProtocol, 'cursor-v1'); assert.equal(response.body.cursorTracksProtocol, 'cursor-tracks-v1');
  assert.equal(start.cursorProtocol, 'cursor-v1'); assert.equal(start.cursorTracksProtocol, 'cursor-tracks-v1');
  const source = await f.tunnel(start);
  source.json(trackInfo(sessionId)); await controller.take(value => value.type === 'stream.tracks.info');
  // Until every output's first key frame nothing belongs to a decoder, and the ids do not advance.
  source.send(trackCursor(shape({ id: 9 })));
  source.send(trackPacket({ track: '0' })); assert.deepEqual(await controller.take(Buffer.isBuffer), trackPacket({ track: '0' }));
  source.send(trackCursor(shape({ id: 10 })));
  const sequence = [trackPacket({ track: '1' }), trackCursor(shape({ id: 1, width: 33, height: 19, hotspotX: 3, hotspotY: 4 })), trackCursor(state({ id: 1 })),
    trackCursor(state({ visible: false })), trackPacket({ track: '0', pts: 1, flags: 0, payload: trackDelta })];
  for (const value of sequence) source.send(value);
  for (const value of sequence) assert.deepEqual(await controller.take(Buffer.isBuffer), value, 'order and bytes are unchanged');
  assert.equal(binaries(controller).length, 0, 'the records before the key frames were discarded');
  // Independent outputs never carry a position.
  source.send(trackCursor(state({ id: 1, x: 5, y: 5 })));
  assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_track_media');
});

test('cursor-tracks-v1 with a Host that lacks it: requested but not granted; its records still end the session', async t => {
  const f = await fixture(t, { heartbeatMs: 10000 }), host = await f.host(trackHost);
  const { response, controller, start, sessionId } = await f.attached(host, { ...outputs, ...bothMarkers });
  assert.equal(response.body.cursorProtocol, undefined); assert.equal(response.body.cursorTracksProtocol, undefined);
  assert.equal('cursorProtocol' in start, false); assert.equal('cursorTracksProtocol' in start, false);
  const source = await f.tunnel(start);
  source.json(trackInfo(sessionId)); await controller.take(value => value.type === 'stream.tracks.info');
  source.send(trackPacket({ track: '0' })); source.send(trackPacket({ track: '1' }));
  await controller.take(Buffer.isBuffer); await controller.take(Buffer.isBuffer);
  source.send(trackCursor(shape({ id: 1 })));
  assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_track_media');
  assert.equal(binaries(controller).length, 0);
});

// 게임 마우스 on independent outputs (모든 모니터): relative-mouse-v1 and
// cursor-compose-v1 over the tracks connection, only for a Host that takes them
// there (relative-mouse-tracks-v1), with the composed cursor, when every
// monitor is a streamed track.
const gameHost = { ...cursorTracksHost, relativeMouseProtocol: 'relative-mouse-v1', relativeMouseTracksProtocol: 'relative-mouse-tracks-v1', cursorComposeProtocol: 'cursor-compose-v1' };
const gameMarkers = { ...bothMarkers, relativeMouseProtocol: 'relative-mouse-v1', cursorComposeProtocol: 'cursor-compose-v1' };
async function readyTracks(f, host, extra) {
  const result = await f.attached(host, { ...outputs, ...extra });
  const source = await f.tunnel(result.start);
  const metadata = trackInfo(result.sessionId);
  source.json(metadata); await result.controller.take(value => value.type === 'stream.tracks.info');
  source.send(trackPacket({ track: '0' })); source.send(trackPacket({ track: '1' }));
  await result.controller.take(Buffer.isBuffer); await result.controller.take(Buffer.isBuffer);
  result.controller.json({ type: 'stream.tracks.ready', topologyEpoch: 1, tracks: metadata.tracks.map(({ id, epoch }) => ({ id, epoch })) });
  await source.take(value => value.type === 'stream.tracks.ready');
  return { ...result, source };
}

test('relative-mouse-tracks-v1: negotiation needs the Host marker, the composed cursor and every monitor as a track', async () => {
  const { negotiateRelativeMouse, relativeMouseTracksCapability, validRelativeTrackMouse } = await import('../src/relative-mouse.mjs');
  const { negotiateCursorCompose } = await import('../src/cursor.mjs');
  const tracks = trackRequests.map(t => ({ id: t.id })), game = { capabilities: gameHost };
  assert.equal(relativeMouseTracksCapability(gameHost), true);
  assert.equal(relativeMouseTracksCapability({ ...gameHost, relativeMouseProtocol: undefined }), false, 'never without relative-mouse-v1');
  assert.equal(negotiateCursorCompose(true, true, game, tracks), true);
  assert.equal(negotiateCursorCompose(true, true, { capabilities: { ...gameHost, relativeMouseTracksProtocol: undefined } }, tracks), false, 'a released Host keeps the tracks cursor as it is');
  assert.equal(negotiateRelativeMouse(true, game, tracks, true), true);
  assert.equal(negotiateRelativeMouse(true, game, tracks, false), false, 'never without the composed cursor');
  assert.equal(negotiateRelativeMouse(true, { capabilities: { ...gameHost, relativeMouseTracksProtocol: 'relative-mouse-tracks-v2' } }, tracks, true), false);
  assert.equal(negotiateRelativeMouse(true, game, [tracks[0]], true), false, 'a monitor outside the tracks: movement could leave them');
  assert.equal(negotiateRelativeMouse(false, game, tracks, true), false);
  assert.equal(negotiateRelativeMouse(true, { capabilities: { ...gameHost, monitors: [gameHost.monitors[0]] } }, null), true, 'one output of one monitor is unchanged');
  assert.equal(validRelativeTrackMouse({ type: 'input', event: 'moveRelative', dx: 3, dy: -4, topologyEpoch: 1 }), true);
  for (const value of [{ type: 'input', event: 'moveRelative', dx: 3, dy: -4 }, { type: 'input', event: 'moveRelative', dx: 3, dy: -4, topologyEpoch: 1, sample: 1 }, { type: 'input', event: 'moveRelative', dx: 0, dy: 0, topologyEpoch: 1 },
    { type: 'input', event: 'button', button: 'left', down: true, topologyEpoch: 1, track: '0' }, null])
    assert.equal(validRelativeTrackMouse(value), false, JSON.stringify(value));
});

test('relative-mouse-tracks-v1 granted: markers in response and session.start; movement, buttons, wheel and cursor.compose reach the tracks connection', async t => {
  const f = await fixture(t, { heartbeatMs: 10000 }), host = await f.host(gameHost);
  const { response, controller, start, sessionId, source } = await readyTracks(f, host, gameMarkers);
  assert.equal(response.body.relativeMouseProtocol, 'relative-mouse-v1'); assert.equal(response.body.cursorComposeProtocol, 'cursor-compose-v1');
  assert.equal(start.relativeMouseProtocol, 'relative-mouse-v1'); assert.equal(start.relativeMouseTracksProtocol, 'relative-mouse-tracks-v1');
  assert.equal(start.cursorComposeProtocol, 'cursor-compose-v1'); assert.equal(start.cursorTracksProtocol, 'cursor-tracks-v1');
  controller.json({ type: 'cursor.compose', on: true });
  assert.deepEqual(await source.take(value => value.type === 'cursor.compose'), { type: 'cursor.compose', sessionId, on: true });
  for (const value of [{ type: 'input', event: 'moveRelative', dx: 3, dy: -4, topologyEpoch: 1 }, { type: 'input', event: 'button', button: 'x1', down: true, topologyEpoch: 1 }, { type: 'input', event: 'wheel', delta: -120, topologyEpoch: 1 }]) {
    controller.json(value);
    assert.deepEqual(await source.take(item => item.type === 'input'), { ...value, sessionId });
  }
  const position = { type: 'input', event: 'move', track: '1', epoch: 1, topologyEpoch: 1, x: 0.25, y: 0.75 };
  controller.json(position); assert.deepEqual(await source.take(item => item.type === 'input'), { ...position, sessionId }, 'positions keep working');
  controller.json({ type: 'input', event: 'moveRelative', dx: 1, dy: 1, topologyEpoch: 1, sample: 1 });
  assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_input');
});

test('relative-mouse-tracks-v1 with a released Host: requested but not granted; its messages end the session', async t => {
  for (const message of [{ type: 'cursor.compose', on: true }, { type: 'input', event: 'moveRelative', dx: 3, dy: -4, topologyEpoch: 1 }]) {
    const f = await fixture(t, { heartbeatMs: 10000 }), host = await f.host({ ...gameHost, relativeMouseTracksProtocol: undefined });
    const { response, controller, start, source } = await readyTracks(f, host, gameMarkers);
    assert.equal(response.body.relativeMouseProtocol, undefined); assert.equal(response.body.cursorComposeProtocol, undefined); assert.equal(response.body.cursorTracksProtocol, 'cursor-tracks-v1');
    for (const key of ['relativeMouseProtocol', 'relativeMouseTracksProtocol', 'cursorComposeProtocol']) assert.equal(key in start, false, key);
    controller.json(message);
    assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_input', JSON.stringify(message));
    assert.equal(source.queue.some(value => value.type === 'cursor.compose' || value.type === 'input'), false, 'nothing reached the Host');
  }
});
