// Real HTTP, WebSocket, relay and UDP worker with synthetic Host and client.
// This proves the Broker's side of the probe contract, not the Host, the
// client or any network path.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { createSocket } from 'node:dgram';
import { once } from 'node:events';
import { setTimeout as delay } from 'node:timers/promises';
import { WebSocket } from 'ws';
import { configuration } from '../src/config.mjs';
import { buildServer } from '../src/http.mjs';
import { ackDatagram, bindDatagram, dataDatagram } from '../src/media-udp.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { Store } from '../src/store.mjs';
import { maxUdpRelays, negotiateUdpMedia, parseUdpRelays, requestedUdpMedia, udpMediaProtocol } from '../src/udp-media.mjs';
import { Vault } from '../src/vault.mjs';

const relays = { host: [{ host: '192.0.2.121', port: 18481 }, { host: 'vdi.example.com', port: 18481 }], client: [{ host: 'vdi.example.com', port: 18481 }] };
const noLists = { host: [], client: [] };
const streamInfo = { type: 'stream.info', codec: 'h264', width: 1280, height: 720, sourceWidth: 1920, sourceHeight: 1080, fpsTarget: 60, audio: { sampleRate: 48000, channels: 2, format: 's16le', available: false } };
const baseCapabilities = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true };
async function until(predicate, ms = 4000) {
  const end = Date.now() + ms;
  while (!predicate()) { assert.ok(Date.now() < end, 'fixture_timeout'); await delay(2); }
}
// A Host offers the key, or does not; `udp: null` is a broker with no UDP port.
async function broker(t, { hostOffers = true, directOffers = false, inputOffers = false, capability = udpMediaProtocol, udp = {} } = {}) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32)), sockets = [], warnings = [];
  store.addUser({ name: 'alice', role: 'user', passwordHash: 'fixture-unused' });
  store.syncProxmox([{ vmid: 149, node: 'fixture', name: 'VM149', status: 'running' }], 'fixture');
  store.assign('alice', 'pve-149');
  const tokens = { alice: newToken(), host: newToken() }, code = newToken(), machine = '12345678-1234-1234-1234-123456789abc';
  store.createSession(store.findUser('alice'), tokenHash(tokens.alice), Date.now(), 3600000, 'client');
  store.issueEnrollment('pve-149', machine, tokenHash(code), 'fixture');
  store.consumeEnrollment(tokenHash(code), machine, { hostname: 'Fixture', version: '9.0.0', tokenHash: tokenHash(tokens.host) });
  const options = udp && { host: '127.0.0.1', port: 0, relays, logger: { warn: line => warnings.push(line) }, ...udp };
  const server = await buildServer({ store, vault, publicOrigin: 'https://relay-fixture.example', relayOptions: { udp: options }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { for (const ws of sockets) ws.terminate(); await new Promise(resolve => server.close(resolve)); store.close(); });
  const base = '127.0.0.1:' + server.address().port;
  async function open(path, authorization) {
    const ws = new WebSocket('ws://' + base + path, { headers: { Authorization: authorization }, perMessageDeflate: false });
    Object.assign(ws, { messages: [], frames: [] });
    ws.on('message', (value, binary) => binary ? ws.frames.push(value) : ws.messages.push(JSON.parse(value.toString('utf8'))));
    ws.on('error', () => {});
    await once(ws, 'open'); sockets.push(ws);
    return ws;
  }
  const call = async (path, method = 'GET', data) => {
    const response = await fetch('http://' + base + '/api/v1' + path, { method, headers: { Authorization: 'Bearer ' + tokens.alice, ...(data ? { 'Content-Type': 'application/json' } : {}) }, body: data ? JSON.stringify(data) : undefined });
    return { status: response.status, body: await response.json() };
  };
  const host = await open('/api/v1/hosts/channel', 'Host ' + tokens.host);
  host.send(JSON.stringify({ type: 'host.capabilities', capabilities: { ...baseCapabilities, ...(hostOffers ? { udpMediaProtocol: capability } : {}), ...(directOffers ? {udpDirectHostProtocol:'udp-direct-v1'} : {}), ...(inputOffers ? {inputLatencyProtocol:'input-latency-v1'} : {}) } }));
  await until(() => host.messages.some(value => value.type === 'host.capabilities.ack'));
  // Connect, attach, start the stream, and prove the session runs on WebSocket.
  const start = async (body = { udpMediaProtocol }, afterConnect = async () => {}) => {
    const reply = await call('/desktops/pve-149/connect', 'POST', body);
    assert.equal(reply.status, 200);
    await afterConnect();
    const client = await open('/api/v1/sessions/' + reply.body.sessionId, 'Bearer ' + tokens.alice), id = reply.body.sessionId;
    await until(() => host.messages.some(value => value.type === 'session.start' && value.sessionId === id));
    host.send(JSON.stringify(streamInfo));
    await until(() => client.messages.some(value => value.type === 'stream.info'));
    const video = Buffer.alloc(18); video[0] = 1; video[1] = 1; video.set([0, 0, 0, 1, 0x65, 1], 12);
    host.send(video);
    await until(() => client.frames.length === 1);
    return { reply: reply.body, client, id };
  };
  const caps = async () => (await call('/desktops')).body.desktops[0].host.capabilities;
  if (udp) await until(() => server.relay.udp.running);
  return { store, server, relay: server.relay, udp: server.relay.udp, host, call, start, caps, warnings };
}
const grants = ws => ws.messages.filter(value => value.type === 'udp.grant');
const secret = grant => ({ id: Buffer.from(grant.id, 'base64'), ticket: Buffer.from(grant.ticket, 'base64') });
async function udpPeer(t, f) {
  const socket = createSocket('udp4'), value = { received: [] };
  socket.on('message', data => value.received.push(data));
  await new Promise(resolve => socket.bind(0, '127.0.0.1', resolve));
  value.port = socket.address().port;
  t.after(() => new Promise(resolve => socket.close(resolve)));
  value.send = bytes => new Promise(resolve => socket.send(bytes, f.udp.port, '127.0.0.1', resolve));
  return value;
}
async function bind(peer, leg) {
  const proof = bindDatagram(leg.ticket, leg.id), ack = ackDatagram(leg.ticket, leg.id), end = Date.now() + 4000;
  while (!peer.received.some(data => data.equals(ack))) { assert.ok(Date.now() < end, 'bind_timeout'); await peer.send(proof); await delay(5); }
  peer.received.length = 0;
}

test('relay-only policy: composite capable peers downgrade to relay without capability or peer offers', async t => {
  const f = await broker(t, { directOffers: true, udp: { media: true } });
  const caps = await f.caps(); assert.equal(caps.udpDirectProtocol, undefined); assert.equal(caps.udpDirectHostProtocol, undefined); assert.equal(caps.udpMediaProtocol, udpMediaProtocol);
  const malformed = await f.call('/desktops/pve-149/connect', 'POST', { udpDirectProtocol: 'other' }); assert.equal(malformed.status, 400);
  const { client, id } = await f.start({ udpMediaProtocol, udpDirectProtocol: 'udp-direct-v1' });
  await until(() => grants(client).length === 1 && grants(f.host).length === 1);
  assert.equal(f.relay.sessions.get(id).udpDirect, false);
  for (const grant of [grants(client)[0], grants(f.host)[0]]) { assert.equal(grant.keys, true); assert.equal(grant.directProtocol, undefined); assert.equal(grant.stun, undefined); }
  const c = await udpPeer(t, f), h = await udpPeer(t, f); await bind(c, secret(grants(client)[0])); await bind(h, secret(grants(f.host)[0]));
  const packet = dataDatagram(secret(grants(client)[0]).id, Buffer.from([0x91, 1])); await c.send(packet); await until(() => h.received.some(value => value.equals(packet)));
  assert.equal(f.udp.direct.size, 0); assert.equal(client.messages.some(v => v.type === 'udp.peer'), false); assert.equal(f.host.messages.some(v => v.type === 'udp.peer'), false);
  client.send(JSON.stringify({ type: 'input', event: 'key', vk: 65, scan: 30, down: false, extended: false })); await until(() => f.host.messages.some(v => v.type === 'input' && v.vk === 65));
  client.send(JSON.stringify({ type: 'input.reset' })); await until(() => f.host.messages.some(v => v.type === 'input.reset'));
  assert.equal(client.readyState, WebSocket.OPEN);
});

test('relay-only policy: forged composite direct state cannot grant peers or forward direct input', async t => {
  const f = await broker(t, { directOffers: true, udp: { media: true } }), { client, id } = await f.start({ udpMediaProtocol });
  await until(() => grants(client).length === 1); const session = f.relay.sessions.get(id);
  session.udpDirect = true; session.udpGranted = false; f.relay.grantUdpMedia(session); await until(() => grants(client).length === 2);
  assert.equal(grants(client)[1].directProtocol, undefined); assert.equal(f.udp.direct.size, 0);
  f.udp.onPeer(id, 'host', [{ address: '192.168.1.2', port: 1234 }]); await delay(20);
  assert.equal(client.messages.some(v => v.type === 'udp.peer'), false);
  let locals = 0; const local = f.udp.local.bind(f.udp); f.udp.local = (...args) => { locals++; return local(...args); };
  client.send(JSON.stringify({ type: 'udp.local', protocol: 'udp-direct-v1', address: '192.168.1.2', port: 1234 }));
  f.host.send(JSON.stringify({ type: 'udp.local', protocol: 'udp-direct-v1', sessionId: id, address: '192.168.1.2', port: 1234 })); await delay(20); assert.equal(locals, 0);
  client.send(JSON.stringify({ type: 'input.direct', protocol: 'udp-direct-v1', sequence: 1, epoch: 0, input: { type: 'input.reset' } }));
  await until(() => !f.relay.sessions.has(id)); assert.equal(f.host.messages.some(v => v.type === 'input.direct'), false);
});

for (const [name, options, request, granted] of [
  ['both offered', {}, true, true],
  ['Host only', {}, false, false],
  ['client only', { hostOffers: false }, true, false],
  ['neither', { hostOffers: false }, false, false],
  ['no relay lists', { udp: { relays: noLists } }, true, false],
  ['no Host list', { udp: { relays: { ...relays, host: [] } } }, true, false],
  ['no client list', { udp: { relays: { ...relays, client: [] } } }, true, false],
  ['no UDP port', { udp: null }, true, false],
]) test(`Grant matrix, ${name}: ${granted ? 'each leg gets one grant' : 'nothing new reaches either peer'}`, async t => {
  const f = await broker(t, options), { reply, client } = await f.start(request ? { udpMediaProtocol } : {});
  await delay(30);
  assert.equal(grants(client).length, granted ? 1 : 0);
  assert.equal(grants(f.host).length, granted ? 1 : 0);
  if (!granted) assert.equal(/udp/i.test(JSON.stringify([reply, client.messages, f.host.messages])), false);
  assert.equal(client.readyState, WebSocket.OPEN);
});

test('Grant matrix, relay down: a stale request is accepted, gets nothing and the session runs', async t => {
  const f = await broker(t);
  await f.udp.close();
  const { reply, client } = await f.start();
  await delay(30);
  assert.equal(/udp/i.test(JSON.stringify([reply, client.messages, f.host.messages])), false);
  assert.equal(client.readyState, WebSocket.OPEN);
});

test('A relay that fails between connect and stream start sends nothing and ends nothing', async t => {
  const f = await broker(t, { udp: { restartMs: 3600000 } });
  const { id, client } = await f.start({ udpMediaProtocol }, async () => { await f.udp.worker.terminate(); await until(() => !f.udp.running); });
  await delay(30);
  assert.equal(f.relay.sessions.get(id).udpMedia, true, 'negotiated at connect, while the relay ran');
  assert.equal(grants(client).length, 0); assert.equal(grants(f.host).length, 0);
  assert.equal(client.readyState, WebSocket.OPEN);
});

test('Clients see the capability only while a grant is possible', async t => {
  const offered = await broker(t, { udp: { restartMs: 40 } });
  assert.equal((await offered.caps()).udpMediaProtocol, udpMediaProtocol);
  const worker = offered.udp.worker, exited = new Promise(resolve => worker.once('exit', resolve));
  worker.postMessage({ type: 'grant' });
  await exited;
  assert.equal('udpMediaProtocol' in await offered.caps(), false, 'relay down');
  await until(() => offered.udp.running);
  assert.equal((await offered.caps()).udpMediaProtocol, udpMediaProtocol, 'back with the relay');
  await offered.udp.close();
  assert.equal('udpMediaProtocol' in await offered.caps(), false, 'relay closed');
  for (const options of [{ udp: null }, { udp: { relays: noLists } }, { udp: { relays: { ...relays, host: [] } } }, { udp: { relays: { ...relays, client: [] } } }, { hostOffers: false }, { capability: 'udp-media-v2' }]) {
    const f = await broker(t, options), capabilities = await f.caps();
    assert.equal('udpMediaProtocol' in capabilities, false, JSON.stringify(options));
    assert.equal(capabilities.streaming, true, 'the other capabilities stay valid');
  }
});

test('Each leg gets only its own ticket, in the contract shape, once per session', async t => {
  const f = await broker(t), { reply, client } = await f.start();
  await until(() => grants(client).length === 1 && grants(f.host).length === 1);
  const [mine] = grants(client), [theirs] = grants(f.host), before = Date.now();
  assert.deepEqual(mine.relays, relays.client); assert.deepEqual(theirs.relays, relays.host);
  for (const grant of [mine, theirs]) {
    assert.deepEqual(Object.keys(grant).sort(), ['expiresAt', 'id', 'protocol', 'relays', 'ticket', 'type']);
    assert.equal(grant.protocol, 'udp-media-v1');
    assert.equal(secret(grant).id.length, 8); assert.equal(secret(grant).ticket.length, 32);
    assert.equal(secret(grant).id.toString('base64'), grant.id); assert.equal(secret(grant).ticket.toString('base64'), grant.ticket);
    assert.ok(Number.isSafeInteger(grant.expiresAt) && grant.expiresAt > before);
  }
  assert.notEqual(mine.ticket, theirs.ticket); assert.notEqual(mine.id, theirs.id);
  const seenByClient = JSON.stringify([reply, client.messages]), seenByHost = JSON.stringify(f.host.messages);
  assert.equal(seenByClient.includes(theirs.ticket), false); assert.equal(seenByClient.includes(theirs.id), false);
  assert.equal(seenByHost.includes(mine.ticket), false); assert.equal(seenByHost.includes(mine.id), false);
  f.host.send(JSON.stringify(streamInfo));
  await until(() => client.messages.filter(value => value.type === 'stream.info').length === 2);
  await delay(30);
  assert.equal(grants(client).length, 1); assert.equal(grants(f.host).length, 1);
});

test('Each leg receives only its own relay list, and a client never sees a Host-only address', async t => {
  const lists = { host: [{ host: '192.0.2.121', port: 18481 }], client: [{ host: 'vdi.example.com', port: 18482 }] };
  const f = await broker(t, { udp: { relays: lists } }), { reply, client } = await f.start();
  await until(() => grants(client).length === 1 && grants(f.host).length === 1);
  assert.deepEqual(grants(client)[0].relays, lists.client); assert.deepEqual(grants(f.host)[0].relays, lists.host);
  const seenByClient = JSON.stringify([reply, client.messages, await f.call('/desktops')]), seenByHost = JSON.stringify(f.host.messages);
  assert.equal(seenByClient.includes('192.0.2.121'), false);
  assert.equal(seenByHost.includes('vdi.example.com'), false);
});

test('The grant comes after the stream has started, and nothing is in the connect reply', async t => {
  const f = await broker(t), { reply, client } = await f.start();
  await until(() => grants(client).length === 1 && grants(f.host).length === 1);
  assert.equal(/udp/i.test(JSON.stringify(reply)), false);
  const index = (ws, type) => ws.messages.findIndex(value => value.type === type);
  assert.ok(index(client, 'stream.info') < index(client, 'udp.grant'));
  assert.ok(index(f.host, 'session.start') < index(f.host, 'udp.grant'));
});

test('Granted tickets bind real legs and carry the contract probe both ways; one leg cannot use the other', async t => {
  const f = await broker(t), { client } = await f.start();
  await until(() => grants(client).length === 1 && grants(f.host).length === 1);
  const clientLeg = secret(grants(client)[0]), hostLeg = secret(grants(f.host)[0]), near = await udpPeer(t, f), far = await udpPeer(t, f);
  for (const crossed of [bindDatagram(clientLeg.ticket, hostLeg.id), bindDatagram(hostLeg.ticket, clientLeg.id)]) {
    const before = f.udp.stats().dropped.bad_proof;
    await near.send(crossed);
    await until(() => f.udp.stats().dropped.bad_proof === before + 1);
  }
  assert.equal(near.received.length, 0); assert.equal(f.udp.stats().bindings, 0);
  await bind(near, clientLeg); await bind(far, hostLeg);
  const payload = Buffer.alloc(16); payload[0] = 1; payload.writeUInt32BE(7, 4); payload.writeBigUInt64BE(123456789n, 8);
  await near.send(dataDatagram(clientLeg.id, payload));
  await until(() => far.received.length === 1);
  assert.deepEqual(far.received[0], dataDatagram(clientLeg.id, payload));
  const pong = Buffer.from(payload); pong[0] = 2;
  await far.send(dataDatagram(hostLeg.id, pong));
  await until(() => near.received.length === 1);
  assert.deepEqual(near.received[0], dataDatagram(hostLeg.id, pong));
});

for (const [name, end] of [
  ['Reconnect', ({ client }) => client.terminate()],
  ['Takeover', (_, f) => f.call('/desktops/pve-149/takeover', 'POST', {})],
]) test(`${name} revokes the old grant and the next session gets a new one`, async t => {
  const f = await broker(t), first = await f.start();
  await until(() => grants(first.client).length === 1 && grants(f.host).length === 1);
  const old = secret(grants(first.client)[0]), near = await udpPeer(t, f);
  await bind(near, old);
  assert.equal(f.udp.stats().bindings, 1);
  await end(first, f);
  await until(() => f.relay.sessions.size === 0 && f.udp.stats().bindings === 0);
  const second = await f.start();
  await until(() => grants(second.client).length === 1 && grants(f.host).length === 2);
  assert.notEqual(grants(second.client)[0].ticket, grants(first.client)[0].ticket);
  assert.notEqual(grants(f.host)[1].ticket, grants(f.host)[0].ticket);
  const before = f.udp.stats().dropped.unknown_id;
  await near.send(bindDatagram(old.ticket, old.id));
  await until(() => f.udp.stats().dropped.unknown_id === before + 1);
  await delay(30);
  assert.equal(near.received.length, 0, 'the old ticket is dead');
  await bind(await udpPeer(t, f), secret(grants(second.client)[0]));
});

test('Tickets reach no log, audit row or relay counter', async t => {
  const lines = [], original = {};
  for (const name of ['log', 'info', 'warn', 'error', 'debug']) { original[name] = console[name]; console[name] = (...values) => lines.push(values.join(' ')); }
  t.after(() => Object.assign(console, original));
  const f = await broker(t), { client } = await f.start();
  await until(() => grants(client).length === 1 && grants(f.host).length === 1);
  client.terminate();
  await until(() => f.relay.sessions.size === 0);
  const stored = JSON.stringify(f.store.db.prepare('SELECT * FROM audit_events').all()), counters = JSON.stringify(f.udp.stats());
  assert.ok(stored.length > 2, 'the check reads a table that has rows');
  for (const { ticket } of [grants(client)[0], grants(f.host)[0]]) for (const text of [lines.join('\n'), stored, f.warnings.join('\n'), counters]) assert.equal(text.includes(ticket), false);
});

test('A malformed or misplaced request field is ignored, never refused', async t => {
  const f = await broker(t);
  for (const value of ['udp-media-v2', 'UDP-MEDIA-V1', 7, true, null, {}, []]) {
    const reply = await f.call('/desktops/pve-149/connect', 'POST', { udpMediaProtocol: value });
    assert.equal(reply.status, 200, JSON.stringify(value));
    assert.equal(f.relay.sessions.get(reply.body.sessionId).udpMedia, false);
    await f.call('/sessions/' + reply.body.sessionId, 'DELETE');
  }
  // RDP is not offered: the connect is refused for its mode, not for this field.
  const rdp = await f.call('/desktops/pve-149/connect', 'POST', { mode: 'rdp', udpMediaProtocol });
  assert.equal(rdp.status, 409); assert.equal(rdp.body.error.code, 'rdp_not_supported');
});

test('Negotiation is exact: stream only, no independent outputs, a marker that matches', () => {
  const host = { capabilities: { udpMediaProtocol } }, udp = { available: true };
  assert.equal(requestedUdpMedia('stream', udpMediaProtocol), true);
  assert.equal(requestedUdpMedia('rdp', udpMediaProtocol), false);
  assert.equal(negotiateUdpMedia(true, host, null, udp), true);
  assert.equal(negotiateUdpMedia(false, host, null, udp), false);
  assert.equal(negotiateUdpMedia(true, host, [{ id: '0' }], udp), false);
  assert.equal(negotiateUdpMedia(true, host, null, { available: false }), false);
  assert.equal(negotiateUdpMedia(true, host, null, null), false);
  assert.equal(negotiateUdpMedia(true, { capabilities: {} }, null, udp), false);
  assert.equal(negotiateUdpMedia(true, { capabilities: { udpMediaProtocol: 'udp-media-v2' } }, null, udp), false);
});

test('Relay lists are strictly validated, bounded, per leg and keep their order', () => {
  assert.deepEqual(parseUdpRelays('192.0.2.121:18481,vdi.example.com:18481'), relays.host);
  assert.deepEqual(parseUdpRelays('VDI.Example.Com:1,127.0.0.1:65535'), [{ host: 'vdi.example.com', port: 1 }, { host: '127.0.0.1', port: 65535 }]);
  assert.deepEqual(parseUdpRelays('relay:443'), [{ host: 'relay', port: 443 }]);
  assert.deepEqual(parseUdpRelays(undefined), []); assert.deepEqual(parseUdpRelays(''), []);
  assert.equal(parseUdpRelays(Array.from({ length: maxUdpRelays }, (_, index) => `relay${index}:1`).join(',')).length, maxUdpRelays);
  for (const bad of [Array.from({ length: maxUdpRelays + 1 }, (_, index) => `relay${index}:1`).join(','), 'a:1,a:1', 'a:1,A:1', 'a:1,', ',a:1', ' a:1', 'a:1 ', 'a', 'a:', ':1', 'a:0', 'a:65536', 'a:01', 'a:1:2', 'a:1e3', '[::1]:1', '::1:1', 'https://a:1', 'a/b:1', 'user@a:1', '-a:1', 'a-:1', 'a..b:1', 'a.:1', '.a:1', '1.2.3:1', '999.1.1.1:1', '1.2.3.4.5:1', 'x'.repeat(64) + ':1', Array.from({ length: 50 }, () => 'abcde').join('.') + ':1']) {
    assert.throws(() => parseUdpRelays(bad), /invalid_udp_relays/, bad);
  }
  const env = { MULTITAP_UDP_PORT: '18481', MULTITAP_UDP_HOST_RELAYS: '192.0.2.121:18481,vdi.example.com:18481', MULTITAP_UDP_CLIENT_RELAYS: 'vdi.example.com:18481' };
  assert.deepEqual(configuration(env).udp.relays, relays);
  assert.deepEqual(configuration({ MULTITAP_UDP_PORT: '18481' }).udp.relays, noLists);
  assert.deepEqual(configuration({ ...env, MULTITAP_UDP_CLIENT_RELAYS: '' }).udp.relays, { ...relays, client: [] });
  assert.deepEqual(configuration({ ...env, MULTITAP_UDP_HOST_RELAYS: '' }).udp.relays, { ...relays, host: [] });
  for (const name of ['MULTITAP_UDP_HOST_RELAYS', 'MULTITAP_UDP_CLIENT_RELAYS']) {
    assert.throws(() => configuration({ ...env, [name]: 'bad list' }), /invalid_udp_relays/, name);
    assert.equal(configuration({ [name]: 'bad list' }).udp, null, 'off stays off and cannot fail startup');
  }
});

test('udp.key passes between the legs of a granted session and nothing else does', async t => {
  const f = await broker(t, { udp: { media: true } }), s = await f.start(), publicKey = randomBytes(32).toString('base64');
  await until(() => grants(s.client).length === 1);
  const key = (extra = {}) => JSON.stringify({ type: 'udp.key', protocol: udpMediaProtocol, publicKey, ...extra });
  const hostKeys = () => f.host.messages.filter(value => value.type === 'udp.key'), clientKeys = () => s.client.messages.filter(value => value.type === 'udp.key');
  s.client.send(key());
  await until(() => hostKeys().length === 1);
  assert.deepEqual(hostKeys()[0], { type: 'udp.key', protocol: udpMediaProtocol, publicKey, sessionId: s.id });
  f.host.send(key({ sessionId: s.id }));
  await until(() => clientKeys().length === 1);
  assert.deepEqual(clientKeys()[0], { type: 'udp.key', protocol: udpMediaProtocol, publicKey });
  // Refused and ignored: the session stays up and a good key still passes.
  for (const bad of [{ publicKey: 'short' }, { publicKey: 'A'.repeat(44) }, { protocol: 'other' }, { extra: 1 }]) s.client.send(key(bad));
  f.host.send(key({ sessionId: 'another-session' }));
  f.host.send(key());
  s.client.send(key());
  await until(() => hostKeys().length === 2);
  await delay(20);
  assert.equal(hostKeys().length, 2);
  assert.equal(clientKeys().length, 1);
  assert.equal(s.client.readyState, WebSocket.OPEN);
});

test('udp.key does nothing in a session without a grant', async t => {
  const f = await broker(t), s = await f.start({}), publicKey = randomBytes(32).toString('base64');
  s.client.send(JSON.stringify({ type: 'udp.key', protocol: udpMediaProtocol, publicKey }));
  f.host.send(JSON.stringify({ type: 'udp.key', protocol: udpMediaProtocol, publicKey, sessionId: s.id }));
  await delay(40);
  assert.equal(f.host.messages.filter(value => value.type === 'udp.key').length, 0);
  assert.equal(s.client.messages.filter(value => value.type === 'udp.key').length, 0);
  assert.equal(s.client.readyState, WebSocket.OPEN);
});
test('The grant says a key exchange is possible only while it is', async t => {
  for (const media of [false, true]) {
    const f = await broker(t, { udp: { media } }), s = await f.start();
    await until(() => grants(s.client).length === 1 && grants(f.host).length === 1);
    assert.equal(grants(s.client)[0].keys, media ? true : undefined);
    assert.equal(grants(f.host)[0].keys, media ? true : undefined);
    assert.equal(Object.hasOwn(grants(s.client)[0], 'keys'), media);
  }
});

test('A desktop list turns media on for those desktops alone', async t => {
  assert.deepEqual(configuration({ MULTITAP_UDP_PORT: '18481', MULTITAP_UDP_MEDIA: 'pve-245,pve-149' }).udp.media, new Set(['pve-245', 'pve-149']));
  for (const [media, allowed] of [[new Set(['pve-149']), true], [new Set(['pve-245']), false]]) {
    const f = await broker(t, { udp: { media } }), s = await f.start(), publicKey = randomBytes(32).toString('base64');
    await until(() => grants(s.client).length === 1 && grants(f.host).length === 1);
    assert.equal(Object.hasOwn(grants(s.client)[0], 'keys'), allowed);
    assert.equal(Object.hasOwn(grants(f.host)[0], 'keys'), allowed);
    s.client.send(JSON.stringify({ type: 'udp.key', protocol: udpMediaProtocol, publicKey }));
    f.host.send(JSON.stringify({ type: 'udp.key', protocol: udpMediaProtocol, publicKey, sessionId: s.id }));
    await delay(40);
    assert.equal(f.host.messages.filter(value => value.type === 'udp.key').length, allowed ? 1 : 0);
    assert.equal(s.client.messages.filter(value => value.type === 'udp.key').length, allowed ? 1 : 0);
  }
});

test('udp.key stays off until MULTITAP_UDP_MEDIA is on', async t => {
  assert.equal(configuration({ MULTITAP_UDP_PORT: '18481', MULTITAP_UDP_MEDIA: 'on' }).udp.media, true);
  assert.equal(configuration({ MULTITAP_UDP_PORT: '18481' }).udp.media, false);
  const f = await broker(t), s = await f.start(), publicKey = randomBytes(32).toString('base64');
  await until(() => grants(s.client).length === 1);
  s.client.send(JSON.stringify({ type: 'udp.key', protocol: udpMediaProtocol, publicKey }));
  f.host.send(JSON.stringify({ type: 'udp.key', protocol: udpMediaProtocol, publicKey, sessionId: s.id }));
  await delay(40);
  assert.equal(f.host.messages.filter(value => value.type === 'udp.key').length, 0);
  assert.equal(s.client.messages.filter(value => value.type === 'udp.key').length, 0);
});

for(const [directOffers,requested,enabled] of [[true,true,true],[true,false,false],[false,true,false]]) test('trusted constructor-only dormant direct negotiation '+[directOffers,requested,enabled],async t=>{
  const f=await broker(t,{directOffers,udp:{media:true,allowPeerDirect:true}}),{client,id}=await f.start({udpMediaProtocol,...(requested?{udpDirectProtocol:'udp-direct-v1'}:{})});
  await until(()=>grants(client).length===1&&grants(f.host).length===1);
  for(const grant of [grants(client)[0],grants(f.host)[0]])assert.equal(grant.directProtocol,enabled?'udp-direct-v1':undefined);
  if(!enabled)return;
  const c=await udpPeer(t,f),h=await udpPeer(t,f);await bind(c,secret(grants(client)[0]));await bind(h,secret(grants(f.host)[0]));
  await until(()=>client.messages.some(v=>v.type==='udp.peer')&&f.host.messages.some(v=>v.type==='udp.peer'));
  assert.equal(client.messages.find(v=>v.type==='udp.peer').sessionId,id);
  assert.deepEqual(client.messages.find(v=>v.type==='udp.peer').candidates,[{address:'127.0.0.1',port:h.port}]);
  client.send(JSON.stringify({type:'udp.local',protocol:'udp-direct-v1',address:'192.168.1.2',port:1234}));
  await until(()=>f.host.messages.some(v=>v.type==='udp.peer'&&v.candidates.some(c=>c.address==='192.168.1.2')));
});

test('trusted constructor-only dormant direct input retains validation and holds early ACK until WSS sample',async t=>{
  const f=await broker(t,{directOffers:true,inputOffers:true,udp:{media:true,allowPeerDirect:true}}),{client,id}=await f.start({udpMediaProtocol,udpDirectProtocol:'udp-direct-v1',inputLatencyProtocol:'input-latency-v1'});
  await until(()=>grants(client).length===1);
  assert.equal(f.relay.visibleCapabilities({udpMediaProtocol,udpDirectHostProtocol:'udp-direct-v1'},'alice','pve-149').udpDirectProtocol,'udp-direct-v1');
  assert.equal(f.relay.visibleCapabilities({udpMediaProtocol,udpDirectProtocol:'udp-direct-v1'},'alice','pve-149').udpDirectProtocol,undefined,'raw connect markers cannot attest broker support');
  const malformed=await f.call('/desktops/pve-149/connect','POST',{udpDirectProtocol:'other'});assert.equal(malformed.status,400);assert.equal(malformed.body.error.code,'invalid_udp_direct_protocol');
  const ack={type:'input.ack',sessionId:id,sample:1,ok:true,injectUs:100,holdUs:150};f.host.send(JSON.stringify(ack));
  await until(()=>f.relay.sessions.get(id)?.inputLatency?.earlyDirect?.size===1);
  client.send(JSON.stringify({type:'input.direct',protocol:'udp-direct-v1',sequence:1,epoch:0,input:{type:'input',event:'key',vk:65,scan:30,down:true,extended:false,sample:1}}));
  await until(()=>client.messages.some(v=>v.type==='input.ack')&&f.host.messages.some(v=>v.type==='input.direct'));
  const forwarded=f.host.messages.find(v=>v.type==='input.direct');assert.equal(forwarded.sessionId,id);assert.equal(forwarded.sequence,1);assert.equal(forwarded.input.vk,65);
  assert.deepEqual(client.messages.filter(v=>v.type==='input.ack'),[ack]);
  f.host.send(JSON.stringify(ack));await delay(20);assert.equal(client.messages.filter(v=>v.type==='input.ack').length,1);assert.equal(f.relay.hosts.size,1);
  const fence=sequence=>({type:'input.direct',protocol:'udp-direct-v1',sequence,epoch:0,input:{type:'input.fence'}});
  client.send(JSON.stringify(fence(2)));await until(()=>f.host.messages.some(v=>v.type==='input.direct'&&v.sequence===2));
  f.relay.sessions.get(id).streamReady=false;client.send(JSON.stringify(fence(3)));
  client.send(JSON.stringify({...fence(4),input:{type:'input.reset'}}));await until(()=>f.host.messages.some(v=>v.type==='input.direct'&&v.sequence===4));
  assert.equal(f.host.messages.some(v=>v.type==='input.direct'&&v.sequence===3),false,'a fence must not reopen fast input before output readiness');
  f.relay.sessions.get(id).streamReady=true;
  client.send(JSON.stringify({type:'input.direct',protocol:'udp-direct-v1',sequence:5,epoch:0,input:{type:'host.power.request',action:'shutdown'}}));
  await until(()=>!f.relay.sessions.has(id));assert.equal(f.host.messages.filter(v=>v.type==='host.power.request').length,0);
});
