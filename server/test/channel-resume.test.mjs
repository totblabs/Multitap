// channel-resume-v1 (channel-resume.mjs): the side channels of a stream that
// resumed (session-resume-v1 / host-resume-v1) pair again in the same session,
// with the same checks as a first pair, and every other mix of peers keeps the
// released lifecycle (a closed pair is final, a dial while away is 401).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { setTimeout as delay } from 'node:timers/promises';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { devicePolicyKeys } from '../src/device-policy.mjs';
import { clipboardPolicyKeys } from '../src/clipboard-policy.mjs';
import { ChannelResume, channelResumeProtocol } from '../src/channel-resume.mjs';
import { captureAssignmentInput } from './fixtures/admin-assignment-input.mjs';

const origin = 'https://channel-resume-fixture.example';
const password = 'Channel-fixture-password-only!';
const machineUuid = '12345678-1234-1234-1234-123456789abc';
const streamInfo = { type: 'stream.info', codec: 'h264', width: 1280, height: 720, sourceWidth: 1920, sourceHeight: 1080, fpsTarget: 60, audio: { sampleRate: 48000, channels: 2, format: 's16le', available: false } };
const releasedHost = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true, keyFrameProtocol: 'keyframe-v1', hostResumeProtocol: 'host-resume-v1', clipboard: true, printProtocol: 'print-v1', driveProtocol: 'drive-v1', usbProtocol: 'usb-v1' };
const newHost = { ...releasedHost, hostChannelResumeProtocol: channelResumeProtocol };
const kinds = ['clipboard', 'print', 'drive', 'usb'];
const deny = Object.fromEntries(devicePolicyKeys.map(key => [key, false]));
const devices = { ...deny, drives: true, printers: true, usbDevices: true };
const clipboardAll = Object.fromEntries(clipboardPolicyKeys.map(key => [key, true]));
const channelHeader = { 'X-Multitap-Channel-Resume': channelResumeProtocol };

async function fixture(t, relayOptions = {}) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const hash = await hashPassword(password);
  store.addUser({ name: 'admin', passwordHash: hash, role: 'admin' });
  store.addUser({ name: 'alice', passwordHash: hash });
  store.syncProxmox([{ vmid: 170, node: 'pve', name: 'VM 170', status: 'running' }], 'fixture');
  store.assign('alice', 'pve-170');
  store.updateAssignmentPolicy('alice', 'pve-170', { devicePolicy: devices, clipboardPolicy: clipboardAll }, 'fixture');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve-fixture.example', tokenId: 'fixture@pve!broker', secret: 'only-fixture-secret' }), 'fixture');
  const proxmox = { machineIdentity: async () => ({ machineUuid }) };
  const server = await buildServer({ store, vault, proxmox, publicOrigin: origin, relayOptions: { authorizationMs: 20, heartbeatMs: 200, ...relayOptions }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const base = `http://127.0.0.1:${server.address().port}`;
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  async function call(path, { method = 'GET', data, token, cookie, csrf } = {}) {
    data = captureAssignmentInput(store, path, data);
    const response = await fetch(base + '/api/v1' + path, { method,
      headers: { ...(token ? { Authorization: 'Bearer ' + token } : {}), ...(cookie ? { Cookie: cookie } : {}), ...(method !== 'GET' ? { 'Content-Type': 'application/json', Origin: origin, ...(csrf ? { 'X-CSRF-Token': csrf } : {}) } : {}) },
      body: data === undefined ? undefined : JSON.stringify(data) });
    return { status: response.status, body: await response.json(), headers: response.headers };
  }
  const adminLogin = await call('/admin/login', { method: 'POST', data: { username: 'admin', password } });
  const admin = { cookie: adminLogin.headers.get('set-cookie').split(';')[0], csrf: adminLogin.body.csrfToken };
  const alice = (await call('/auth/login', { method: 'POST', data: { username: 'alice', password } })).body.sessionToken;
  const code = (await call('/admin/desktops/pve-170/enrollment', { ...admin, method: 'POST', data: {} })).body.code;
  const enrolled = await call('/hosts/enroll', { method: 'POST', data: { code, hostname: 'Fixture VM', machineUuid, version: '0.2.0' } });
  assert.equal(enrolled.status, 201);
  const wsUrl = path => base.replace('http:', 'ws:') + path;
  // The administrator changes alice's permissions for this desktop.
  const grant = async policy => assert.equal((await call('/admin/assignments/batch', { ...admin, method: 'PUT', data: { username: 'alice', assignments: [{ desktopId: 'pve-170', ...policy }] } })).status, 200);
  return { store, server, call, alice, admin, grant, hostToken: enrolled.body.hostToken, wsUrl };
}
function socket(url, authorization, headers = {}) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url, { headers: { Authorization: authorization, ...headers }, perMessageDeflate: false });
    ws.once('error', reject);
    ws.once('unexpected-response', (_req, response) => { response.resume(); reject(Object.assign(new Error('upgrade_rejected'), { status: response.statusCode })); });
    ws.once('open', () => {
      const queue = [], waiters = [];
      ws.on('message', (data, binary) => {
        const value = binary ? data : JSON.parse(data.toString('utf8'));
        const index = waiters.findIndex(waiter => waiter.predicate(value));
        if (index >= 0) { const waiter = waiters.splice(index, 1)[0]; clearTimeout(waiter.timer); waiter.resolve(value); }
        else queue.push(value);
      });
      ws.take = (predicate = () => true, ms = 2000) => {
        const index = queue.findIndex(predicate);
        if (index >= 0) return Promise.resolve(queue.splice(index, 1)[0]);
        return new Promise((resolve, reject) => { const waiter = { predicate, resolve, timer: setTimeout(() => reject(new Error('message_timeout')), ms) }; waiters.push(waiter); });
      };
      ws.none = async (predicate, ms = 200) => { await delay(ms); assert.equal(queue.some(predicate), false, 'An unexpected message arrived'); };
      ws.json = value => ws.send(JSON.stringify(value));
      ws.ended = new Promise(done => ws.once('close', (code, reason) => done({ code, reason: reason.toString() })));
      resolve(ws);
    });
  });
}
const type = name => value => !Buffer.isBuffer(value) && value.type === name;
const frame = (key, marker) => { const bytes = Buffer.alloc(64); bytes[0] = 1; bytes[1] = key ? 1 : 0; bytes[12] = marker; return bytes; };
const status = async promise => { try { const ws = await promise; ws.terminate(); return 101; } catch (error) { return error.status; } };
async function host(f, capabilities = newHost) {
  const ws = await socket(f.wsUrl('/api/v1/hosts/channel'), 'Host ' + f.hostToken);
  ws.json({ type: 'host.capabilities', capabilities });
  assert.equal((await ws.take(type('host.capabilities.ack'))).accepted, true);
  return ws;
}
// A streaming session with every side channel granted that already showed its first picture.
async function streaming(f, hostWs, { header = channelHeader, resume = true } = {}) {
  const connection = await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice, data: { mode: 'stream', printProtocol: 'print-v1', driveProtocol: 'drive-v1', usbProtocol: 'usb-v1', ...(resume ? { sessionResumeProtocol: 'session-resume-v1' } : {}) } });
  assert.equal(connection.status, 200, JSON.stringify(connection.body));
  const url = f.wsUrl('/api/v1/sessions/' + connection.body.sessionId);
  const client = await socket(url, 'Bearer ' + f.alice, header);
  const token = resume ? await client.take(type('session.resume.token')) : null;
  const start = await hostWs.take(type('session.start'));
  hostWs.json(streamInfo);
  await client.take(type('stream.info'));
  hostWs.send(frame(true, 1));
  await client.take(Buffer.isBuffer);
  return { id: start.sessionId, url, client, token, start };
}
const dialClient = (f, s, kind) => socket(f.wsUrl(`/api/v1/sessions/${s.id}/${kind}`), 'Bearer ' + f.alice);
const dialHost = (f, s, kind) => socket(f.wsUrl(`/api/v1/hosts/sessions/${s.id}/${kind}`), 'Host ' + f.hostToken);
// Both peers dial and the relay pairs them.
async function pair(f, s, kind) {
  const client = await dialClient(f, s, kind), hostSide = await dialHost(f, s, kind);
  assert.equal((await client.take(type(kind + '.ready'))).sessionId, s.id);
  assert.equal((await hostSide.take(type(kind + '.ready'))).sessionId, s.id);
  return { client, host: hostSide };
}
async function pairs(f, s) { const all = {}; for (const kind of kinds) all[kind] = await pair(f, s, kind); return all; }
// Every socket of every pair hears why it ended, then closes.
async function ended(all, suffix) {
  for (const kind of kinds) for (const role of ['client', 'host']) {
    assert.equal((await all[kind][role].take(type(kind + '.error'))).reason, kind + suffix, `${kind} ${role}`);
    await all[kind][role].ended;
  }
}
async function resumeClient(f, s, token) {
  const client = await socket(s.url, 'Bearer ' + f.alice, { 'X-Multitap-Resume': token, ...channelHeader });
  return { client, resumed: await client.take(type('session.resumed')) };
}

test('channel-resume-v1 is granted only where the client asked, the Host redials and the session resumes', async t => {
  const f = await fixture(t);
  let hostWs = await host(f);
  let s = await streaming(f, hostWs);
  assert.equal(s.token.channelResumeProtocol, channelResumeProtocol);
  assert.equal(s.start.channelResumeProtocol, channelResumeProtocol);
  s.client.close(1000); await hostWs.take(type('session.stop'));
  // A released client: no header, so nothing changes for it or for the Host.
  s = await streaming(f, hostWs, { header: {} });
  assert.equal(s.token.channelResumeProtocol, undefined);
  assert.equal(s.start.channelResumeProtocol, undefined);
  s.client.close(1000); await hostWs.take(type('session.stop'));
  // A wrong marker is not the protocol.
  s = await streaming(f, hostWs, { header: { 'X-Multitap-Channel-Resume': 'channel-resume-v2' } });
  assert.equal(s.token.channelResumeProtocol, undefined); assert.equal(s.start.channelResumeProtocol, undefined);
  s.client.close(1000); await hostWs.take(type('session.stop'));
  // A session that does not resume has nothing to come back after.
  s = await streaming(f, hostWs, { resume: false });
  assert.equal(s.start.channelResumeProtocol, undefined);
  await s.client.none(value => !Buffer.isBuffer(value) && value.channelResumeProtocol !== undefined, 100);
  s.client.close(1000); await hostWs.take(type('session.stop'));
  hostWs.close(1000); await delay(50);
  // A released Host: the marker is not in its capabilities.
  hostWs = await host(f, releasedHost);
  s = await streaming(f, hostWs);
  assert.equal(s.token.channelResumeProtocol, undefined); assert.equal(s.start.channelResumeProtocol, undefined);
  for (const value of [{ ...releasedHost, hostChannelResumeProtocol: 'channel-resume-v2' }, { ...releasedHost, hostChannelResumeProtocol: true }]) {
    const relay = new ChannelResume({});
    assert.equal(relay.negotiate({ mode: 'stream', resume: true, host: { capabilities: value } }, channelResumeProtocol), false);
  }
});

test('a session that did not negotiate keeps the released lifecycle: a closed pair is final and a dial while away is refused', async t => {
  const f = await fixture(t);
  const hostWs = await host(f, releasedHost);
  const s = await streaming(f, hostWs);
  const all = await pairs(f, s);
  s.client.terminate();
  // The released reason, and nothing told to the Host.
  for (const kind of kinds) assert.equal((await all[kind].host.take(type(kind + '.error'))).reason, kind + '_unavailable', kind);
  for (const kind of kinds) assert.equal(await status(dialHost(f, s, kind)), 401, kind);
  await hostWs.none(type('session.channels'), 100);
  const { client } = await resumeClient(f, s, s.token.token);
  await hostWs.none(type('session.channels'), 100);
  // Printing, folders and USB stay closed for the session; the clipboard pairs again, as before.
  for (const kind of ['print', 'drive', 'usb']) assert.equal(await status(dialClient(f, s, kind)), 409, kind);
  await pair(f, s, 'clipboard');
  client.close(1000);
});

test('after the client was away every side channel pairs again in the same session', async t => {
  const f = await fixture(t);
  const hostWs = await host(f);
  const s = await streaming(f, hostWs);
  const all = await pairs(f, s);
  s.client.terminate();
  // The pairs end at once with the reason both peers wait on, and the Host hears the client left.
  await ended(all, '_stream_away');
  assert.deepEqual(await hostWs.take(type('session.channels')), { type: 'session.channels', sessionId: s.id, state: 'away' });
  // A dial now is "not now", never the refusal that used to end the channel for good.
  for (const kind of kinds) assert.equal(await status(dialHost(f, s, kind)), 503, kind);
  for (const kind of kinds) assert.equal(await status(dialClient(f, s, kind)), 503, kind);
  const { client, resumed } = await resumeClient(f, s, s.token.token);
  assert.equal(resumed.channelResumeProtocol, channelResumeProtocol);
  assert.deepEqual(await hostWs.take(type('session.channels')), { type: 'session.channels', sessionId: s.id, state: 'back' });
  const again = await pairs(f, s);
  // The new pairs are ordinary pairs: a second resume brings them back too.
  client.terminate();
  await ended(again, '_stream_away');
  assert.equal((await hostWs.take(type('session.channels'))).state, 'away');
  const second = await resumeClient(f, s, resumed.token);
  assert.equal((await hostWs.take(type('session.channels'))).state, 'back');
  await pairs(f, s);
  second.client.close(1000);
});

test('after the Host was away every side channel pairs again, and the Host is told first whether the client is there', async t => {
  const f = await fixture(t);
  let hostWs = await host(f);
  const s = await streaming(f, hostWs);
  const all = await pairs(f, s);
  hostWs.terminate();
  await s.client.take(type('session.resuming'));
  // Not "authorization_revoked", and the held session is not stopped by a pair that outlived the Host's channel.
  await ended(all, '_stream_away');
  for (const kind of kinds) assert.equal(await status(dialClient(f, s, kind)), 503, kind);
  hostWs = await host(f);
  hostWs.json({ type: 'host.session.resume', sessions: [s.id] });
  assert.deepEqual(await hostWs.take(), { type: 'session.channels', sessionId: s.id, state: 'back' });
  assert.deepEqual(await hostWs.take(), { type: 'host.session.resumed', sessionId: s.id });
  await s.client.take(type('session.resumed'));
  await pairs(f, s);
  // The Host returns while the client is away: its channels wait for the client.
  s.client.terminate();
  assert.equal((await hostWs.take(type('session.channels'))).state, 'away');
  hostWs.terminate(); await delay(80);
  hostWs = await host(f);
  hostWs.json({ type: 'host.session.resume', sessions: [s.id] });
  assert.equal((await hostWs.take(type('session.channels'))).state, 'away');
  await hostWs.take(type('host.session.resumed'));
  for (const kind of kinds) assert.equal(await status(dialHost(f, s, kind)), 503, kind);
  const { client } = await resumeClient(f, s, s.token.token);
  assert.equal((await hostWs.take(type('session.channels'))).state, 'back');
  await pairs(f, s);
  client.close(1000);
});

test('an outage longer than the hold ends the session and no channel can be dialled', async t => {
  const f = await fixture(t, { resumeGraceMs: 150 });
  const hostWs = await host(f);
  const s = await streaming(f, hostWs);
  const all = await pairs(f, s);
  s.client.terminate();
  await ended(all, '_stream_away');
  await hostWs.take(type('session.stop'), 3000);
  for (const kind of kinds) { assert.equal(await status(dialHost(f, s, kind)), 401, kind); assert.equal(await status(dialClient(f, s, kind)), 401, kind); }
  assert.equal(await status(socket(s.url, 'Bearer ' + f.alice, { 'X-Multitap-Resume': s.token.token, ...channelHeader })), 401);
});

test('a permission the administrator turned off during the outage stays off for the session', async t => {
  const f = await fixture(t);
  const hostWs = await host(f);
  const s = await streaming(f, hostWs);
  const all = await pairs(f, s);
  s.client.terminate();
  await ended(all, '_stream_away');
  // Printers and the clipboard's files are taken away; folders and USB stay.
  await f.grant({ devicePolicy: { ...devices, printers: false }, clipboardPolicy: { ...clipboardAll, filesToRemote: false } });
  // Turned on again before the client is back: still off for this session, as for a pair that was open.
  await f.grant({ devicePolicy: devices, clipboardPolicy: clipboardAll });
  const { client } = await resumeClient(f, s, s.token.token);
  for (const kind of ['print', 'clipboard']) { assert.equal(await status(dialClient(f, s, kind)), 409, kind); assert.equal(await status(dialHost(f, s, kind)), 409, kind); }
  const drive = await pair(f, s, 'drive'), usb = await pair(f, s, 'usb');
  // Turned off while the pair is open: that end is final too.
  await f.grant({ devicePolicy: { ...devices, drives: false } });
  assert.equal((await drive.client.take(type('drive.error'))).reason, 'drive_policy_changed');
  await f.grant({ devicePolicy: devices });
  assert.equal(await status(dialClient(f, s, 'drive')), 409);
  // A permission that is simply off at the dial is the released refusal.
  await f.grant({ devicePolicy: { ...devices, usbDevices: false } });
  assert.equal((await usb.host.take(type('usb.error'))).reason, 'usb_policy_changed');
  assert.equal(await status(dialHost(f, s, 'usb')), 409);
  client.close(1000);
});

test('a pair that ended for a reason of its own stays closed, a transport end may be dialled again', async t => {
  const f = await fixture(t);
  const hostWs = await host(f);
  const s = await streaming(f, hostWs);
  // A violation is final, as before.
  let print = await pair(f, s, 'print');
  print.client.json({ type: 'print.nonsense' });
  assert.equal((await print.host.take(type('print.error'))).reason, 'print_invalid_message');
  assert.equal(await status(dialClient(f, s, 'print')), 409);
  // A lost socket is not: both peers dial a fresh pair.
  let usb = await pair(f, s, 'usb');
  usb.client.terminate();
  assert.equal((await usb.host.take(type('usb.error'))).reason, 'usb_transport_closed');
  usb = await pair(f, s, 'usb');
  // The same peer dials again while the broker still holds its old socket (a network change the broker has
  // not noticed): the old pair is gone, the other peer hears a transport end and dials too.
  const drive = await pair(f, s, 'drive');
  const fresh = await dialClient(f, s, 'drive');
  assert.equal((await drive.host.take(type('drive.error'))).reason, 'drive_transport_closed');
  await drive.client.ended;
  const hostSide = await dialHost(f, s, 'drive');
  await fresh.take(type('drive.ready')); await hostSide.take(type('drive.ready'));
  s.client.close(1000);
  await hostWs.take(type('session.stop'));
  // The session ended: nothing can be dialled.
  for (const kind of kinds) assert.equal(await status(dialClient(f, s, kind)), 401, kind);
});

test('new pairs are bounded per channel and session', async t => {
  const f = await fixture(t);
  f.server.relay.channels.maxPairs = 2;
  const hostWs = await host(f);
  const s = await streaming(f, hostWs);
  for (let n = 0; n < 2; n++) { const usb = await pair(f, s, 'usb'); usb.client.terminate(); await usb.host.take(type('usb.error')); await usb.host.ended; }
  assert.equal(await status(dialClient(f, s, 'usb')), 503);
  // Another channel has its own count.
  await pair(f, s, 'print');
  // The window passes.
  f.server.relay.channels.windowMs = 1; await delay(20);
  await pair(f, s, 'usb');
  s.client.close(1000);
});
