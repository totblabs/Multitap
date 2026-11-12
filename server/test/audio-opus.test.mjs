import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { audioOpusProtocol, audioOpusCodec, audioOpusCapability, requestedAudioOpus, negotiateAudioOpus, validStreamAudio, validAudioPayload } from '../src/audio-codec.mjs';

// Opus audio (audio-opus-v1): the Host's sound as 10 ms Opus packets of any
// length instead of 4-byte PCM frames. Exact negotiation (a capable Host, a
// client that asked, one output), the session.start codec, stream.info's
// format, and released peers unchanged: PCM sessions never see Opus bytes, a
// Host without the capability is still held to whole PCM frames. Real broker
// and WebSockets, fake Host/client.
const origin = 'https://audio-opus-fixture.example';
const password = 'Audio-opus-fixture-password-only!';
const machineUuid = '12345678-1234-1234-1234-123456789ac7';
const releasedHost = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true };
const opusHost = { ...releasedHost, audioCodecProtocol: 'audio-opus-v1' };
const info = format => ({ type: 'stream.info', codec: 'h264', width: 1920, height: 1080, sourceWidth: 1920, sourceHeight: 1080, fpsTarget: 60, audio: { sampleRate: 48000, channels: 2, format, available: true } });
const asked = { audioCodecProtocol: 'audio-opus-v1' };

function header(kind, length, { epoch = 0, pts = 1000, flags = 0 } = {}) {
  const data = Buffer.alloc(12 + length);
  data[0] = kind; data[1] = flags; data.writeUInt16LE(epoch, 2); data.writeBigUInt64LE(BigInt(pts), 4);
  return data;
}
// u16 sequence + an Opus packet: 163 bytes, not whole PCM frames.
const opus = (sequence, pts) => { const data = header(2, 163, { pts }); data.writeUInt16LE(sequence, 12); data[14] = 0xfc; return data; };
const pcm = pts => header(2, 1920, { pts });
const video = () => { const data = header(1, 5, { flags: 1 }); Buffer.from([0, 0, 1, 0x65, 1]).copy(data, 12); return data; };

async function fixture(t) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const hash = await hashPassword(password);
  store.addUser({ name: 'admin', passwordHash: hash, role: 'admin' });
  store.addUser({ name: 'alice', passwordHash: hash, role: 'user' });
  store.syncProxmox([{ vmid: 173, node: 'pve', name: 'VM 173', status: 'running' }], 'fixture');
  store.assign('alice', 'pve-173');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve-fixture.example', tokenId: 'fixture@pve!broker', secret: 'only-fixture-secret' }), 'fixture');
  const proxmox = { machineIdentity: async () => ({ machineUuid }) };
  const server = await buildServer({ store, vault, proxmox, publicOrigin: origin, relayOptions: { authorizationMs: 20, heartbeatMs: 200 }, logger: { error: value => assert.fail(value) } });
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
  const code = (await call('/admin/desktops/pve-173/enrollment', { ...admin, method: 'POST', data: {} })).body.code;
  const wsUrl = path => base.replace('http:', 'ws:') + path;
  async function host(capabilities) {
    const enrolled = await call('/hosts/enroll', { method: 'POST', data: { code, hostname: 'Fixture VM', machineUuid, version: '0.2.150' } });
    assert.equal(enrolled.status, 201);
    const ws = await socket(wsUrl('/api/v1/hosts/channel'), 'Host ' + enrolled.body.hostToken);
    ws.json({ type: 'host.capabilities', capabilities });
    assert.equal((await ws.take(value => value.type === 'host.capabilities.ack')).accepted, true);
    return ws;
  }
  const connect = extra => call('/desktops/pve-173/connect', { method: 'POST', token: alice, data: { mode: 'stream', ...extra } });
  const client = sessionId => socket(wsUrl('/api/v1/sessions/' + sessionId), 'Bearer ' + alice);
  async function stream(hostSocket, extra = {}) {
    const response = await connect(extra);
    assert.equal(response.status, 200, JSON.stringify(response.body));
    const controller = await client(response.body.sessionId);
    const start = await hostSocket.take(value => value.type === 'session.start');
    return { response, controller, start, sessionId: response.body.sessionId };
  }
  return { call, alice, host, connect, stream };
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
async function barrier(host, controller, id) { host.json({ type: 'pong', id }); await controller.take(value => value.type === 'pong' && value.id === id); }
const audioPackets = controller => controller.queue.filter(value => Buffer.isBuffer(value) && value[0] === 2);

test('Capability, request, negotiation and format rules are exact', () => {
  assert.equal(audioOpusProtocol, 'audio-opus-v1');
  assert.equal(audioOpusCodec, 'opus');
  assert.equal(audioOpusCapability(opusHost), true);
  for (const value of [releasedHost, { ...releasedHost, audioCodecProtocol: 'audio-opus-v2' }, { ...releasedHost, audioCodecProtocol: true }, undefined, null])
    assert.equal(audioOpusCapability(value), false, JSON.stringify(value));
  assert.equal(requestedAudioOpus('stream', 'audio-opus-v1'), true);
  for (const [mode, protocol] of [['stream', undefined], ['stream', 'audio-opus-v2'], ['stream', 'opus'], ['stream', true], ['rdp', 'audio-opus-v1']])
    assert.equal(requestedAudioOpus(mode, protocol), false, `${mode} ${protocol}`);
  assert.equal(negotiateAudioOpus(true, { capabilities: opusHost }, null), true);
  assert.equal(negotiateAudioOpus(false, { capabilities: opusHost }, null), false, 'never granted unrequested');
  assert.equal(negotiateAudioOpus(true, { capabilities: releasedHost }, null), false, 'a released Host: not granted, not refused');
  assert.equal(negotiateAudioOpus(true, { capabilities: opusHost }, [{ id: '0' }]), false, 'independent outputs keep PCM');
  const audio = format => ({ sampleRate: 48000, channels: 2, format, available: true });
  assert.equal(validStreamAudio(audio('s16le'), false), true);
  assert.equal(validStreamAudio(audio('s16le'), true), true, 'a granted Host whose encoder did not start sends PCM');
  assert.equal(validStreamAudio(audio('opus'), true), true);
  assert.equal(validStreamAudio(audio('opus'), false), false, 'Opus only where granted');
  for (const value of [{ ...audio('opus'), sampleRate: 44100 }, { ...audio('opus'), channels: 1 }, audio('f32le'), undefined, null])
    assert.equal(validStreamAudio(value, true), false, JSON.stringify(value));
  assert.equal(validAudioPayload(1920, false), true);
  assert.equal(validAudioPayload(163, false), false);
  assert.equal(validAudioPayload(163, true), true);
});

test('Granted: inventory, response and session.start carry it; Opus info and odd-length packets reach the client', async t => {
  const f = await f_host(t, opusHost);
  const inventory = await f.call('/desktops', { token: f.alice });
  assert.equal(inventory.body.desktops[0].host.capabilities.audioCodecProtocol, 'audio-opus-v1');
  const { response, controller, start } = await f.stream(f.hostSocket, asked);
  assert.equal(response.body.audioCodecProtocol, 'audio-opus-v1');
  assert.equal(start.audioCodec, 'opus');
  assert.equal(start.audio, true);
  f.hostSocket.json(info('opus'));
  assert.equal((await controller.take(value => value.type === 'stream.info')).audio.format, 'opus');
  f.hostSocket.send(video()); await controller.take(value => Buffer.isBuffer(value) && value[0] === 1);
  for (let n = 0; n < 50; n++) f.hostSocket.send(opus(n, 2000 + n * 10000));
  await barrier(f.hostSocket, controller, 'opus');
  const heard = audioPackets(controller);
  assert.equal(heard.length, 50, 'every Opus packet reached the client');
  assert.deepEqual(heard.map(packet => packet.readUInt16LE(12)), [...Array(50).keys()]);
  assert.equal(heard[0].length, 175);
  assert.equal(f.hostSocket.readyState, WebSocket.OPEN);
});

test('A granted session may still hear PCM: the Host says s16le when its encoder did not start', async t => {
  const f = await f_host(t, opusHost);
  const { controller } = await f.stream(f.hostSocket, asked);
  f.hostSocket.json(info('s16le'));
  assert.equal((await controller.take(value => value.type === 'stream.info')).audio.format, 's16le');
  f.hostSocket.send(video()); await controller.take(value => Buffer.isBuffer(value) && value[0] === 1);
  f.hostSocket.send(pcm(2000));
  await barrier(f.hostSocket, controller, 'pcm');
  assert.equal(audioPackets(controller).length, 1);
});

test('A client that did not ask (released): PCM session; an Opus packet is dropped, not relayed, and the Host stays', async t => {
  const f = await f_host(t, opusHost);
  const { response, controller, start } = await f.stream(f.hostSocket, {});
  assert.equal(response.body.audioCodecProtocol, undefined, 'never granted unrequested');
  assert.equal('audioCodec' in start, false, 'session.start is the released one');
  f.hostSocket.json(info('s16le')); await controller.take(value => value.type === 'stream.info');
  f.hostSocket.send(video()); await controller.take(value => Buffer.isBuffer(value) && value[0] === 1);
  f.hostSocket.send(opus(0, 2000)); f.hostSocket.send(pcm(12000));
  await barrier(f.hostSocket, controller, 'released-client');
  const heard = audioPackets(controller);
  assert.equal(heard.length, 1, 'only the PCM packet reached the PCM client');
  assert.equal(heard[0].length, 12 + 1920);
  assert.equal(f.hostSocket.readyState, WebSocket.OPEN, 'a late Opus packet does not drop the Host');
});

test('A PCM session refuses Opus stream info', async t => {
  const f = await f_host(t, opusHost);
  const { controller } = await f.stream(f.hostSocket, {});
  f.hostSocket.json(info('opus'));
  await closed(f.hostSocket);
  assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_stream_info');
});

test('A released Host: not granted, and its audio is still held to whole PCM frames', async t => {
  const f = await f_host(t, releasedHost);
  const inventory = await f.call('/desktops', { token: f.alice });
  assert.equal(inventory.body.desktops[0].host.capabilities.audioCodecProtocol, undefined);
  const { response, controller, start } = await f.stream(f.hostSocket, asked);
  assert.equal(response.status, 200, 'asking a released Host is not a refusal');
  assert.equal(response.body.audioCodecProtocol, undefined);
  assert.equal('audioCodec' in start, false);
  f.hostSocket.json(info('s16le')); await controller.take(value => value.type === 'stream.info');
  f.hostSocket.send(video()); await controller.take(value => Buffer.isBuffer(value) && value[0] === 1);
  f.hostSocket.send(opus(0, 2000));
  await closed(f.hostSocket);
});

test('Other values of the connect field are neither refused nor granted', async t => {
  for (const audioCodecProtocol of ['audio-opus-v2', 'opus', 1, null, '']) {
    const f = await f_host(t, opusHost);
    const { response, start } = await f.stream(f.hostSocket, { audioCodecProtocol });
    assert.equal(response.body.audioCodecProtocol, undefined, String(audioCodecProtocol));
    assert.equal('audioCodec' in start, false, String(audioCodecProtocol));
  }
});
async function f_host(t, capabilities) { const f = await fixture(t); f.hostSocket = await f.host(capabilities); return f; }
