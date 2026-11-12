// Independent outputs (tracks-v1, "모든 모니터") with Opus audio
// (audio-opus-tracks-v1) and transparent resume (tracks-resume-v1,
// session-resume.mjs): negotiation, the shared audio format, a lost client
// socket, a lost track tunnel, a lost Host channel, and the refusals that keep
// the released behaviour. Real broker and WebSockets, fake Host and client.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { setTimeout as delay } from 'node:timers/promises';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { audioOpusTracksCapability, negotiateAudioOpus } from '../src/audio-codec.mjs';
import { negotiateSessionResume, trackResumeCapability } from '../src/session-resume.mjs';
import { requests, options, packet, delta, info, acknowledgement, keyboard, resize } from './track-fixture.mjs';

const origin = 'https://tracks-extend-fixture.example';
const password = 'Tracks-extend-fixture-password-only!';
const machineUuid = '12345678-1234-1234-1234-123456789ad1';
const trackHost = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true,
  keyFrameProtocol: 'keyframe-v1', hostResumeProtocol: 'host-resume-v1', videoTrackProtocol: 'tracks-v1', trackViewportProvisioning: true,
  monitors: requests.map(t => ({ id: t.id, width: t.width, height: t.height, primary: t.id === '0' })) };
const resumeHost = { ...trackHost, trackResumeProtocol: 'tracks-resume-v1' };
const opusHost = { ...trackHost, audioCodecProtocol: 'audio-opus-v1', audioCodecTracksProtocol: 'audio-opus-tracks-v1' };
const resume = { sessionResumeProtocol: 'session-resume-v1' };
const tokenPattern = /^[A-Za-z0-9_-]{43}$/;

async function fixture(t, relayOptions = {}) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const hash = await hashPassword(password);
  store.addUser({ name: 'admin', passwordHash: hash, role: 'admin' });
  store.addUser({ name: 'alice', passwordHash: hash });
  store.addUser({ name: 'bobby', passwordHash: hash });
  store.syncProxmox([{ vmid: 170, node: 'pve', name: 'VM 170', status: 'running' }], 'fixture');
  store.assign('alice', 'pve-170');
  store.assign('bobby', 'pve-170');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve-fixture.example', tokenId: 'fixture@pve!broker', secret: 'only-fixture-secret' }), 'fixture');
  const proxmox = { machineIdentity: async () => ({ machineUuid }) };
  const server = await buildServer({ store, vault, proxmox, publicOrigin: origin, relayOptions: { authorizationMs: 20, heartbeatMs: 10000, ...relayOptions }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const base = `http://127.0.0.1:${server.address().port}`;
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  async function call(path, { method = 'GET', data, token, cookie, csrf } = {}) {
    const response = await fetch(base + '/api/v1' + path, { method,
      headers: { ...(token ? { Authorization: 'Bearer ' + token } : {}), ...(cookie ? { Cookie: cookie } : {}), ...(method !== 'GET' ? { 'Content-Type': 'application/json', Origin: origin, ...(csrf ? { 'X-CSRF-Token': csrf } : {}) } : {}) },
      body: data === undefined ? undefined : JSON.stringify(data) });
    return { status: response.status, body: await response.json(), headers: response.headers };
  }
  const adminLogin = await call('/admin/login', { method: 'POST', data: { username: 'admin', password } });
  const admin = { cookie: adminLogin.headers.get('set-cookie').split(';')[0], csrf: adminLogin.body.csrfToken };
  const alice = (await call('/auth/login', { method: 'POST', data: { username: 'alice', password } })).body.sessionToken;
  const bobby = (await call('/auth/login', { method: 'POST', data: { username: 'bobby', password } })).body.sessionToken;
  const code = (await call('/admin/desktops/pve-170/enrollment', { ...admin, method: 'POST', data: {} })).body.code;
  const enrolled = await call('/hosts/enroll', { method: 'POST', data: { code, hostname: 'Fixture VM', machineUuid, version: '0.2.160' } });
  assert.equal(enrolled.status, 201);
  const wsUrl = path => base.replace('http:', 'ws:') + path;
  return { store, server, call, alice, bobby, hostToken: enrolled.body.hostToken, wsUrl };
}
function socket(url, authorization, headers = {}) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url, { headers: { Authorization: authorization, ...headers }, perMessageDeflate: false });
    ws.once('error', reject);
    ws.once('unexpected-response', (_req, response) => { response.resume(); reject(Object.assign(new Error('upgrade_rejected'), { status: response.statusCode })); });
    ws.once('open', () => {
      const queue = [], waiters = [];
      ws.messages = [];
      ws.on('message', (data, binary) => {
        const value = binary ? data : JSON.parse(data.toString('utf8'));
        ws.messages.push(value);
        const index = waiters.findIndex(waiter => waiter.predicate(value));
        if (index >= 0) { const waiter = waiters.splice(index, 1)[0]; clearTimeout(waiter.timer); waiter.resolve(value); }
        else queue.push(value);
      });
      ws.take = (predicate = () => true, ms = 2000) => {
        const index = queue.findIndex(predicate);
        if (index >= 0) return Promise.resolve(queue.splice(index, 1)[0]);
        return new Promise((resolve, reject) => { const waiter = { predicate, resolve, timer: setTimeout(() => { waiters.splice(waiters.indexOf(waiter), 1); reject(new Error('message_timeout')); }, ms) }; waiters.push(waiter); });
      };
      ws.none = async (predicate, ms = 300) => { await delay(ms); assert.equal(queue.some(predicate), false, 'An unexpected message arrived'); };
      ws.json = value => ws.send(JSON.stringify(value));
      resolve(ws);
    });
  });
}
const type = name => value => !Buffer.isBuffer(value) && value.type === name;
const binary = value => Buffer.isBuffer(value);
const video = (track, pts, key = true) => packet({ track, pts, ...(key ? {} : { flags: 0, payload: delta }) });
// kind 2, connector 255: PCM is whole 4-byte frames; Opus is a u16 sequence + a packet of any length.
const pcm = pts => packet({ kind: 2, track: 255, epoch: 0, flags: 0, pts, payload: Buffer.alloc(1920) });
const opus = (sequence, pts) => { const payload = Buffer.alloc(163); payload.writeUInt16LE(sequence, 0); payload[2] = 0xfc; return packet({ kind: 2, track: 255, epoch: 0, flags: 0, pts, payload }); };
const audits = (f, event) => f.store.db.prepare('SELECT subject FROM audit_events WHERE event=?').all(event).map(row => JSON.parse(row.subject));

async function host(f, capabilities) {
  const ws = await socket(f.wsUrl('/api/v1/hosts/channel'), 'Host ' + f.hostToken);
  ws.json({ type: 'host.capabilities', capabilities });
  const ack = await ws.take(type('host.capabilities.ack'));
  assert.equal(ack.accepted, true);
  return ws;
}
// A tracks-v1 session with its track tunnel open.
async function tracks(f, hostWs, extra = {}) {
  const connection = await f.call('/desktops/pve-170/connect', { method: 'POST', token: f.alice, data: { mode: 'stream', options, mediaProtocol: 'tracks-v1', tracks: requests, ...extra } });
  assert.equal(connection.status, 200, JSON.stringify(connection.body));
  const url = f.wsUrl('/api/v1/sessions/' + connection.body.sessionId);
  const client = await socket(url, 'Bearer ' + f.alice);
  const start = await hostWs.take(type('session.start'));
  const tunnelUrl = f.wsUrl(new URL(start.tunnelUrl).pathname);
  const tunnel = await socket(tunnelUrl, 'Host ' + f.hostToken);
  return { connection: connection.body, id: start.sessionId, url, client, start, tunnelUrl, tunnel };
}
// Metadata, both first IDRs and the client's ready acknowledgement.
async function ready(s, metadata = info(s.id)) {
  s.tunnel.json(metadata); await s.client.take(type('stream.tracks.info'));
  for (const track of ['0', '1']) { s.tunnel.send(video(track, 1)); await s.client.take(binary); }
  s.client.json(acknowledgement(metadata)); await s.tunnel.take(type('stream.tracks.ready'));
  return metadata;
}
// Everything sent on `from` before this point has been relayed to `to`.
async function barrier(from, to, id) { from.json({ type: 'pong', id }); await to.take(value => value.type === 'pong' && value.id === id); }
async function inputBarrier(client, tunnel, id) { client.json({ type: 'ping', id }); await tunnel.take(value => value.type === 'ping' && value.id === id); }
// Video restarts exactly at an IDR on every output: deltas before it are dropped.
async function restartsAtIdr(tunnel, client, pts) {
  const before = client.messages.filter(binary).length;
  tunnel.send(video('0', pts, false)); tunnel.send(video('1', pts, false));
  await barrier(tunnel, client, 'deltas-' + pts);
  assert.equal(client.messages.filter(binary).length, before, 'Video before the restart IDR reached the client');
  for (const track of ['0', '1']) { tunnel.send(video(track, pts + 1)); assert.equal((await client.take(binary))[6], Number(track)); }
  tunnel.send(video('0', pts + 2, false)); assert.equal((await client.take(binary))[6], 0);
}

// ------------------------------------------------------------- audio-opus-tracks-v1
test('Opus for independent outputs needs both Host markers and the client\'s request', () => {
  assert.equal(audioOpusTracksCapability(opusHost), true);
  for (const value of [trackHost, { ...trackHost, audioCodecProtocol: 'audio-opus-v1' }, { ...trackHost, audioCodecTracksProtocol: 'audio-opus-tracks-v1' }, { ...opusHost, audioCodecTracksProtocol: 'audio-opus-tracks-v2' }, undefined])
    assert.equal(audioOpusTracksCapability(value), false, JSON.stringify(value));
  const tracksRequest = [{ id: '0' }, { id: '1' }];
  assert.equal(negotiateAudioOpus(true, { capabilities: opusHost }, tracksRequest), true);
  assert.equal(negotiateAudioOpus(false, { capabilities: opusHost }, tracksRequest), false, 'never granted unrequested');
  assert.equal(negotiateAudioOpus(true, { capabilities: { ...opusHost, audioCodecTracksProtocol: undefined } }, tracksRequest), false, 'a Host whose track worker sends PCM');
  assert.equal(negotiateAudioOpus(true, { capabilities: { ...opusHost, audioCodecTracksProtocol: undefined } }, null), true, 'one output is unchanged');
});

test('Granted for tracks: inventory, response and session.start carry it; Opus metadata and odd-length packets reach the client', async t => {
  const f = await fixture(t), hostWs = await host(f, opusHost);
  assert.equal((await f.call('/desktops', { token: f.alice })).body.desktops[0].host.capabilities.audioCodecTracksProtocol, 'audio-opus-tracks-v1');
  const s = await tracks(f, hostWs, { audioCodecProtocol: 'audio-opus-v1' });
  assert.equal(s.connection.audioCodecProtocol, 'audio-opus-v1');
  assert.equal(s.start.audioCodec, 'opus');
  const metadata = info(s.id); metadata.audio = { ...metadata.audio, format: 'opus' };
  assert.equal((await ready(s, metadata)).audio.format, 'opus');
  for (let n = 0; n < 20; n++) s.tunnel.send(opus(n, 10 + n));
  await barrier(s.tunnel, s.client, 'opus');
  const heard = s.client.messages.filter(value => binary(value) && value[4] === 2);
  assert.deepEqual(heard.map(value => value.readUInt16LE(28)), [...Array(20).keys()], 'Every Opus packet reached the client in order');
  assert.equal(heard[0].length, 28 + 163);
  assert.equal(s.tunnel.readyState, WebSocket.OPEN);
});

test('A granted tracks session may still hear PCM when the Host says s16le, and PCM stays whole frames', async t => {
  const f = await fixture(t), hostWs = await host(f, opusHost);
  const s = await tracks(f, hostWs, { audioCodecProtocol: 'audio-opus-v1' });
  await ready(s);
  s.tunnel.send(pcm(10)); await s.client.take(value => binary(value) && value[4] === 2);
  s.tunnel.send(opus(0, 11));
  assert.equal((await s.client.take(type('session.ended'))).reason, 'invalid_track_media', 'An Opus payload in a PCM tracks session was relayed');
});

test('Not granted for tracks: a Host without the tracks marker or a client that did not ask keeps PCM; Opus metadata ends it', async t => {
  for (const [capabilities, extra] of [[{ ...opusHost, audioCodecTracksProtocol: undefined }, { audioCodecProtocol: 'audio-opus-v1' }], [opusHost, {}]]) {
    const f = await fixture(t), hostWs = await host(f, capabilities);
    const s = await tracks(f, hostWs, extra);
    assert.equal(s.connection.audioCodecProtocol, undefined);
    assert.equal('audioCodec' in s.start, false, 'session.start is the released one');
    s.tunnel.json({ ...info(s.id), audio: { sampleRate: 48000, channels: 2, format: 'opus', available: true } });
    assert.equal((await s.client.take(type('session.ended'))).reason, 'invalid_track_info');
  }
});

// ------------------------------------------------------------- tracks-resume-v1
test('Resume for tracks is granted only with the Host\'s tracks-resume-v1 and on request, and session.start says so', async t => {
  assert.equal(trackResumeCapability(resumeHost), true);
  assert.equal(trackResumeCapability(trackHost), false);
  assert.equal(negotiateSessionResume(true, { capabilities: resumeHost }, requests), true);
  assert.equal(negotiateSessionResume(true, { capabilities: trackHost }, requests), false, 'a Host that cannot restart its outputs');
  assert.equal(negotiateSessionResume(true, { capabilities: { ...resumeHost, keyFrameProtocol: undefined } }, requests), false);
  assert.equal(negotiateSessionResume(false, { capabilities: resumeHost }, requests), false);
  {
    const f = await fixture(t), hostWs = await host(f, trackHost);
    assert.equal((await f.call('/desktops', { token: f.alice })).body.desktops[0].host.capabilities.trackResumeProtocol, undefined);
    const s = await tracks(f, hostWs, resume);
    assert.equal(s.connection.sessionResumeProtocol, undefined); assert.equal(s.connection.resumeGraceMs, undefined);
    assert.equal('trackResumeProtocol' in s.start, false);
    await s.client.none(type('session.resume.token'));
  }
  {
    const f = await fixture(t), hostWs = await host(f, resumeHost);
    assert.equal((await f.call('/desktops', { token: f.alice })).body.desktops[0].host.capabilities.trackResumeProtocol, 'tracks-resume-v1');
    const s = await tracks(f, hostWs, resume);
    assert.equal(s.connection.sessionResumeProtocol, 'session-resume-v1'); assert.equal(s.connection.resumeGraceMs, 15000);
    assert.equal(s.start.trackResumeProtocol, 'tracks-resume-v1');
    assert.match((await s.client.take(type('session.resume.token'))).token, tokenPattern);
  }
});

test('A lost client socket keeps both outputs; the client resumes and every output restarts at an IDR', async t => {
  const f = await fixture(t, { resumeGraceMs: 3000 });
  const hostWs = await host(f, resumeHost), s = await tracks(f, hostWs, resume);
  await ready(s);
  const first = await s.client.take(type('session.resume.token'));
  s.client.terminate();
  assert.deepEqual(await s.tunnel.take(type('input.reset')), { type: 'input.reset', sessionId: s.id });
  s.tunnel.send(video('0', 2, false)); // nobody to receive it
  await hostWs.none(type('session.stop'));
  const back = await socket(s.url, 'Bearer ' + f.alice, { 'X-Multitap-Resume': first.token });
  const resumed = await back.take(type('session.resumed'));
  assert.match(resumed.token, tokenPattern); assert.notEqual(resumed.token, first.token);
  assert.equal((await s.tunnel.take(type('stream.keyframe'))).sessionId, s.id);
  // Input waits for the restarted pictures.
  back.json(keyboard()); await inputBarrier(back, s.tunnel, 'before-keys');
  assert.equal(s.tunnel.messages.filter(type('input')).length, 0, 'Input reached the Host before every output restarted');
  await restartsAtIdr(s.tunnel, back, 10);
  back.json(keyboard()); assert.equal((await s.tunnel.take(type('input'))).event, 'key');
  assert.equal(s.tunnel.messages.filter(type('input.reset')).length, 1, 'A detached tracks resume sent a second reset');
  const [record] = audits(f, 'session_resumed');
  assert.equal(record.sessionId, s.id); assert.equal(record.side, 'client'); assert.equal(record.reason, 'client_disconnected');
});

test('A silent tracks client replacement releases held keys exactly once on its track tunnel', async t => {
  const f = await fixture(t), hostWs = await host(f, resumeHost), s = await tracks(f, hostWs, resume);
  await ready(s);
  const token = (await s.client.take(type('session.resume.token'))).token;
  const session = f.server.relay.sessions.get(s.id), retired = session.ws;
  const key = (vk, down) => keyboard({ vk, down });
  const ctrl = key(17, true);
  s.client.json(ctrl);
  assert.deepEqual(await s.tunnel.take(type('input')), { ...ctrl, sessionId: s.id });
  for (const bearer of [f.bobby, randomBytes(32).toString('base64url')])
    await assert.rejects(socket(s.url, 'Bearer ' + bearer, { 'X-Multitap-Resume': token }), error => error.status === 401);
  await inputBarrier(s.client, s.tunnel, 'tracks-auth-denied');
  assert.equal(session.ws, retired);
  assert.equal(s.tunnel.messages.filter(type('input.reset')).length, 0);

  const oldClosed = once(s.client, 'close');
  const back = await socket(s.url, 'Bearer ' + f.alice, { 'X-Multitap-Resume': token });
  await back.take(type('session.resumed')); await oldClosed;
  await s.tunnel.take(type('stream.keyframe'));
  await restartsAtIdr(s.tunnel, back, 10);
  // The old socket may still have a buffered callback; it no longer owns input.
  retired.emit('message', Buffer.from(JSON.stringify(key(66, true))), false);
  back.json(key(65, true)); back.json(key(65, false));
  await inputBarrier(back, s.tunnel, 'tracks-replacement-input');
  const controls = s.tunnel.messages.filter(value => value.type === 'input' || value.type === 'input.reset');
  assert.deepEqual(controls, [
    { ...ctrl, sessionId: s.id }, { type: 'input.reset', sessionId: s.id },
    { ...key(65, true), sessionId: s.id }, { ...key(65, false), sessionId: s.id },
  ]);
  const held = new Set();
  for (const value of controls) {
    if (value.type === 'input.reset') held.clear();
    else if (value.down) held.add(value.vk); else held.delete(value.vk);
  }
  assert.equal(held.size, 0, 'Ctrl stayed pressed on the track worker');
  assert.equal(hostWs.messages.some(type('input.reset')), false, 'Reset belongs on the active track tunnel');
  assert.equal(session.ws.readyState, WebSocket.OPEN);
  assert.equal(hostWs.messages.some(type('session.stop')), false);
});

test('An output late with its IDR is asked again, and one that never sends it ends the session', async t => {
  const f = await fixture(t, { resumeGraceMs: 3000, resizeReadyMs: 1500 });
  const hostWs = await host(f, resumeHost), s = await tracks(f, hostWs, resume);
  await ready(s);
  const token = (await s.client.take(type('session.resume.token'))).token;
  s.client.terminate(); await s.tunnel.take(type('input.reset'));
  const back = await socket(s.url, 'Bearer ' + f.alice, { 'X-Multitap-Resume': token });
  await back.take(type('session.resumed')); await s.tunnel.take(type('stream.keyframe'));
  s.tunnel.send(video('0', 5)); await back.take(binary);
  await delay(600);
  s.tunnel.send(video('1', 6, false));
  assert.equal((await s.tunnel.take(type('stream.keyframe'))).sessionId, s.id, 'A late output was not asked again');
  assert.equal((await back.take(type('session.ended'), 3000)).reason, 'tracks_ready_timeout');
});

test('A lost track tunnel keeps the session; the Host dials it again and every output restarts at an IDR', async t => {
  const f = await fixture(t, { hostResumeGraceMs: 3000 });
  const hostWs = await host(f, resumeHost), s = await tracks(f, hostWs, resume);
  await ready(s); await s.client.take(type('session.resume.token'));
  s.tunnel.terminate();
  assert.deepEqual(await s.client.take(type('session.resuming')), { type: 'session.resuming', sessionId: s.id });
  // While the tunnel is away the client's input has nowhere to go; it is not an error.
  s.client.json(keyboard()); s.client.json({ type: 'ping', id: 'away' });
  await s.client.none(type('session.ended'));
  await hostWs.none(type('session.stop'));
  const again = await socket(s.tunnelUrl, 'Host ' + f.hostToken);
  assert.equal((await again.take(type('stream.keyframe'))).sessionId, s.id);
  assert.deepEqual(await s.client.take(type('session.resumed')), { type: 'session.resumed', sessionId: s.id });
  await restartsAtIdr(again, s.client, 20);
  s.client.json(keyboard()); assert.equal((await again.take(type('input'))).event, 'key');
  assert.equal(again.messages.filter(value => value.type === 'input' || value.id === 'away').length, 1, 'What the client sent while the tunnel was away reached the Host');
  const record = audits(f, 'session_resumed')[0];
  assert.equal(record.side, 'tracks'); assert.equal(record.reason, 'tracks_host_disconnected');
});

test('A Host tunnel the broker still holds is replaced by the Host\'s next one', async t => {
  const f = await fixture(t, { hostResumeGraceMs: 3000 });
  const hostWs = await host(f, resumeHost), s = await tracks(f, hostWs, resume);
  await ready(s);
  const oldClosed = once(s.tunnel, 'close');
  const again = await socket(s.tunnelUrl, 'Host ' + f.hostToken);
  await oldClosed;
  await again.take(type('stream.keyframe'));
  await restartsAtIdr(again, s.client, 30);
  await s.client.none(type('session.ended'));
  assert.equal(audits(f, 'session_resumed')[0].reason, 'replaced');
});

test('The client can resume while the session waits for its track tunnel', async t => {
  const f = await fixture(t, { resumeGraceMs: 3000, hostResumeGraceMs: 3000 });
  const hostWs = await host(f, resumeHost), s = await tracks(f, hostWs, resume);
  await ready(s);
  const token = (await s.client.take(type('session.resume.token'))).token;
  s.tunnel.terminate(); await s.client.take(type('session.resuming'));
  s.client.terminate();
  const back = await socket(s.url, 'Bearer ' + f.alice, { 'X-Multitap-Resume': token });
  await back.take(type('session.resumed')); await back.take(type('session.resuming'));
  const again = await socket(s.tunnelUrl, 'Host ' + f.hostToken);
  await again.take(type('stream.keyframe')); await back.take(type('session.resumed'));
  await restartsAtIdr(again, back, 40);
});

test('host-resume-v1 for tracks: a lost Host channel keeps the session and its tunnel; the returning Host restarts every output', async t => {
  const f = await fixture(t, { hostResumeGraceMs: 3000 });
  const first = await host(f, resumeHost), s = await tracks(f, first, resume);
  await ready(s); await s.client.take(type('session.resume.token'));
  first.terminate();
  assert.deepEqual(await s.client.take(type('session.resuming')), { type: 'session.resuming', sessionId: s.id });
  await delay(100);
  const second = await host(f, resumeHost);
  second.json({ type: 'host.session.resume', sessions: [s.id] });
  assert.deepEqual(await second.take(type('host.session.resumed')), { type: 'host.session.resumed', sessionId: s.id });
  assert.equal((await s.tunnel.take(type('stream.keyframe'))).sessionId, s.id);
  await s.client.take(type('session.resumed'));
  await restartsAtIdr(s.tunnel, s.client, 50);
  s.client.json(keyboard()); assert.equal((await s.tunnel.take(type('input'))).event, 'key');
});

test('Tracks keep the released ending: a deliberate tunnel close, no grant, a released Host, a resize in flight and a missed window', async t => {
  {
    const f = await fixture(t, { hostResumeGraceMs: 3000 });
    const hostWs = await host(f, resumeHost), s = await tracks(f, hostWs, resume);
    await ready(s); s.tunnel.close(1000);
    assert.equal((await s.client.take(type('session.ended'))).reason, 'tracks_host_disconnected');
  }
  for (const [capabilities, extra] of [[resumeHost, {}], [trackHost, resume]]) {
    const f = await fixture(t, { hostResumeGraceMs: 3000, resumeGraceMs: 3000 });
    const hostWs = await host(f, capabilities), s = await tracks(f, hostWs, extra);
    await ready(s); s.tunnel.terminate();
    assert.equal((await s.client.take(type('session.ended'))).reason, 'tracks_host_disconnected');
    // and a second tunnel is refused as before
    await assert.rejects(socket(s.tunnelUrl, 'Host ' + f.hostToken), error => error.status === 401 || error.status === 409);
  }
  {
    const f = await fixture(t, { hostResumeGraceMs: 3000, resumeGraceMs: 3000 });
    const hostWs = await host(f, trackHost), s = await tracks(f, hostWs, resume);
    await ready(s); s.client.terminate();
    assert.equal((await hostWs.take(type('session.stop'))).reason, 'client_disconnected', 'A released Host\'s tracks session waited for the client');
  }
  {
    const f = await fixture(t, { hostResumeGraceMs: 3000, resumeGraceMs: 3000 });
    const hostWs = await host(f, resumeHost), s = await tracks(f, hostWs, resume);
    await ready(s); s.client.json(resize()); await s.tunnel.take(type('stream.tracks.resize'));
    s.tunnel.terminate();
    assert.equal((await s.client.take(type('session.ended'))).reason, 'tracks_host_disconnected', 'A tunnel lost during a resize was held');
  }
  {
    const f = await fixture(t, { hostResumeGraceMs: 3000, resumeGraceMs: 3000 });
    const hostWs = await host(f, resumeHost), s = await tracks(f, hostWs, resume);
    await ready(s); s.client.json(resize()); await s.tunnel.take(type('stream.tracks.resize'));
    s.client.terminate();
    assert.equal((await hostWs.take(type('session.stop'))).reason, 'client_disconnected', 'A client lost during a resize was held');
  }
  {
    const f = await fixture(t, { hostResumeGraceMs: 300 });
    const hostWs = await host(f, resumeHost), s = await tracks(f, hostWs, resume);
    await ready(s); s.tunnel.terminate(); await s.client.take(type('session.resuming'));
    assert.equal((await s.client.take(type('session.ended'))).reason, 'tracks_host_disconnected');
    const expired = audits(f, 'session_resume_expired')[0];
    assert.equal(expired.sessionId, s.id); assert.equal(expired.side, 'tracks');
    await assert.rejects(socket(s.tunnelUrl, 'Host ' + f.hostToken), error => error.status === 401, 'An ended session took a tunnel');
  }
});
