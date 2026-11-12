// UDP media of a Docker Compose install that names no relay list: each leg is
// given the address it already reaches the broker at, on the UDP port Docker
// publishes. The configuration cases, the list of one leg, and the real
// broker (HTTP, WebSocket, relay and UDP worker) with a synthetic Host and
// client that connect at different addresses. No Docker and no network path:
// whether a datagram reaches that address is not shown here.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { createServer, request } from 'node:http';
import { once } from 'node:events';
import { setTimeout as delay } from 'node:timers/promises';
import { WebSocket } from 'ws';
import { configuration } from '../src/config.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { Store } from '../src/store.mjs';
import { UdpRelay } from '../src/udp-relay.mjs';
import { udpMediaProtocol } from '../src/udp-media.mjs';
import { Vault } from '../src/vault.mjs';

const noFile = { readAccessFile: () => null };
const docker = extra => ({ MULTITAP_HOST: '0.0.0.0', MULTITAP_PORT: '18480', MULTITAP_DEPLOYMENT_KIND: 'docker', MULTITAP_HTTP_ORIGIN: 'http://192.0.2.10:18480', MULTITAP_ALLOW_INSECURE_HTTP: '1',
  MULTITAP_PUBLISHED_PORT: '0.0.0.0:18480', MULTITAP_UDP_PORT: '18481', MULTITAP_UDP_PUBLISHED_PORT: '18481', ...extra });
const udpOf = extra => configuration(docker(extra), noFile).udp;

test('A Compose install without relay lists derives them from the published UDP port and sends media over UDP; explicit .env values win', () => {
  const derived = udpOf({});
  assert.equal(derived.derivedPort, 18481); assert.equal(derived.media, true); assert.deepEqual(derived.relays, { host: [], client: [] }); assert.equal(derived.port, 18481);
  // The port Docker publishes, not the one the relay listens on inside the container.
  assert.equal(udpOf({ MULTITAP_UDP_PUBLISHED_PORT: '0.0.0.0:28481' }).derivedPort, 28481);
  assert.equal(udpOf({ MULTITAP_UDP_PUBLISHED_PORT: '192.0.2.10:28481' }).derivedPort, 28481);
  // MULTITAP_UDP_MEDIA still decides when it is set: a desktop list, or anything that is not `on`.
  assert.deepEqual([...udpOf({ MULTITAP_UDP_MEDIA: 'pve-101,pve-102' }).media], ['pve-101', 'pve-102']);
  // `off` is off, not a desktop named "off": the console is told so (describe()).
  assert.equal(udpOf({ MULTITAP_UDP_MEDIA: 'off' }).media, false); assert.equal(udpOf({ MULTITAP_UDP_MEDIA: 'on' }).media, true);
  assert.equal(relayOf({ derivedPort: 18481, media: udpOf({ MULTITAP_UDP_MEDIA: 'off' }).media }).describe().media, 'off');
  assert.equal(relayOf({ derivedPort: 18481, media: udpOf({ MULTITAP_UDP_MEDIA: 'pve-101' }).media }).describe().media, 'desktops');
  assert.equal(relayOf({ derivedPort: 18481, media: udpOf({}).media }).describe().media, 'on');
  // The same on an install with its own lists.
  for (const [value, media] of [['off', false], ['', false], ['on', true]]) assert.equal(udpOf({ MULTITAP_UDP_HOST_RELAYS: 'vdi.example.com:18481', MULTITAP_UDP_CLIENT_RELAYS: 'vdi.example.com:18481', MULTITAP_UDP_MEDIA: value }).media, media, value);
  assert.equal(udpOf({ MULTITAP_UDP_MEDIA: 'off' }).derivedPort, 18481, 'the reachability probe still runs');

  // Lists in .env: used as they are, with media as MULTITAP_UDP_MEDIA says (off when unset).
  const lists = { MULTITAP_UDP_HOST_RELAYS: '192.0.2.10:18481,vdi.example.com:18481', MULTITAP_UDP_CLIENT_RELAYS: 'vdi.example.com:18481' };
  const explicit = udpOf(lists);
  assert.equal(explicit.derivedPort, undefined); assert.equal(explicit.media, false);
  assert.deepEqual(explicit.relays, { host: [{ host: '192.0.2.10', port: 18481 }, { host: 'vdi.example.com', port: 18481 }], client: [{ host: 'vdi.example.com', port: 18481 }] });
  assert.equal(udpOf({ ...lists, MULTITAP_UDP_MEDIA: 'on' }).media, true);
  // One list alone is an unfinished .env setting, not a reason to derive the other.
  for (const key of Object.keys(lists)) { const half = udpOf({ [key]: lists[key] }); assert.equal(half.derivedPort, undefined, key); assert.equal(half.media, false, key); }

  // Not derived where the published port is not told, is no port, or is open on loopback only: sessions stay on WebSocket.
  for (const value of [undefined, '', '0', '70000', 'udp', '127.0.0.1:18481', '[::1]:18481', '[::]:18481']) {
    const off = udpOf({ MULTITAP_UDP_PUBLISHED_PORT: value });
    assert.equal(off.derivedPort, undefined, String(value)); assert.equal(off.media, false, String(value));
  }
  // No relay at all without its port.
  assert.equal(udpOf({ MULTITAP_UDP_PORT: '' }), null);
});

test('An install with an updater, or outside Docker, never derives relay lists: empty lists keep sessions on WebSocket as before', () => {
  const production = { NODE_ENV: 'production', MULTITAP_DATA_DIR: '/var/lib/multitap', MULTITAP_PUBLIC_ORIGIN: 'https://vdi.example.com', MULTITAP_UDP_PORT: '18481', MULTITAP_UDP_PUBLISHED_PORT: '18481' };
  for (const settings of [
    { ...production, MULTITAP_DEPLOYMENT_KIND: 'systemd', MULTITAP_UPDATE_SOCKET: '/run/multitap-update/control.sock' },
    { ...production, MULTITAP_DEPLOYMENT_KIND: 'docker', MULTITAP_UPDATE_SOCKET: '/run/multitap-docker-update/control.sock' },
    { ...production, MULTITAP_DEPLOYMENT_KIND: 'systemd' }, { ...production, MULTITAP_DEPLOYMENT_KIND: 'external' },
  ]) {
    const udp = configuration(settings, { readAccessFile: path => path.endsWith('server-access.identity') ? 'c'.repeat(64) : null }).udp;
    assert.equal(udp.derivedPort, undefined, settings.MULTITAP_DEPLOYMENT_KIND); assert.equal(udp.media, false); assert.deepEqual(udp.relays, { host: [], client: [] });
    // Their own lists and switch, exactly as given.
    const own = configuration({ ...settings, MULTITAP_UDP_HOST_RELAYS: 'vdi.example.com:18481', MULTITAP_UDP_CLIENT_RELAYS: 'vdi.example.com:18481', MULTITAP_UDP_MEDIA: 'on' }, { readAccessFile: path => path.endsWith('server-access.identity') ? 'c'.repeat(64) : null }).udp;
    assert.equal(own.derivedPort, undefined); assert.equal(own.media, true); assert.deepEqual(own.relays.client, [{ host: 'vdi.example.com', port: 18481 }]);
  }
});

// The relay's own rules, without its worker.
const relayOf = values => Object.create(UdpRelay.prototype, Object.getOwnPropertyDescriptors({ running: true, derivedPort: null, hostRoute: null, relays: { host: [], client: [] }, media: false, limits: { port: 18481 }, ...values }));
test('A derived list names the host of the address that leg connects to; no IPv4 endpoint, no list', () => {
  const relay = relayOf({ derivedPort: 18481, media: true });
  assert.equal(relay.available, true);
  // The same server at whatever address each side uses: the LAN address inside, the public one outside.
  assert.deepEqual(relay.relaysFor('client', 'http://192.0.2.10:18480'), [{ host: '192.0.2.10', port: 18481 }]);
  assert.deepEqual(relay.relaysFor('client', 'http://203.0.113.7:8080'), [{ host: '203.0.113.7', port: 18481 }], 'the HTTP port of the address is not the UDP port');
  assert.deepEqual(relay.relaysFor('host', 'https://VDI.Example.com'), [{ host: 'vdi.example.com', port: 18481 }]);
  assert.deepEqual(relay.relaysFor('client', 'http://name.iptime.org:18480'), [{ host: 'name.iptime.org', port: 18481 }]);
  // The relay and its legs are IPv4: an IPv6 address gives no endpoint, and that session stays on WebSocket.
  for (const origin of ['http://[2001:db8::10]:18480', 'https://[::1]', 'not an address', '']) assert.deepEqual(relay.relaysFor('client', origin), [], origin);
  // host-route-v1: a Host inside the network is first given the address it reaches the server at there; a client never is.
  const routed = relayOf({ derivedPort: 18481, hostRoute: '192.0.2.10' });
  assert.deepEqual(routed.relaysFor('host', 'https://vdi.example.com'), [{ host: '192.0.2.10', port: 18481 }, { host: 'vdi.example.com', port: 18481 }]);
  assert.deepEqual(routed.relaysFor('client', 'https://vdi.example.com'), [{ host: 'vdi.example.com', port: 18481 }]);
  assert.deepEqual(routed.relaysFor('host', 'http://192.0.2.10:18480'), [{ host: '192.0.2.10', port: 18481 }], 'the same address once');
  assert.deepEqual(routed.relaysFor('host', 'http://[2001:db8::10]:18480'), [{ host: '192.0.2.10', port: 18481 }]);
  assert.deepEqual(routed.describe(), { running: true, port: 18481, derivedPort: 18481, media: 'off', relays: { host: [], client: [] }, hostRoute: '192.0.2.10' });

  // Configured lists are everyone's, whatever address they connect to.
  const lists = { host: [{ host: '192.0.2.121', port: 18481 }], client: [{ host: 'vdi.example.com', port: 18482 }] };
  const configured = relayOf({ relays: lists, hostRoute: '192.0.2.10', media: new Set(['pve-101']) });
  assert.equal(configured.available, true);
  assert.deepEqual(configured.relaysFor('client', 'http://192.0.2.10:18480'), lists.client); assert.deepEqual(configured.relaysFor('host', 'http://[2001:db8::10]:18480'), lists.host);
  assert.deepEqual(configured.describe(), { running: true, port: 18481, derivedPort: null, media: 'desktops', relays: lists, hostRoute: null });
  // Neither derived nor both lists: no grant is possible.
  assert.equal(relayOf({}).available, false); assert.equal(relayOf({ relays: { ...lists, client: [] } }).available, false);
  assert.equal(relayOf({ derivedPort: 18481, running: false }).available, false);
});

async function until(predicate, ms = 4000) {
  const end = Date.now() + ms;
  while (!predicate()) { assert.ok(Date.now() < end, 'fixture_timeout'); await delay(2); }
}
const freePort = () => new Promise(resolve => { const probe = createServer(); probe.listen(0, '127.0.0.1', () => { const port = probe.address().port; probe.close(() => resolve(port)); }); });
const streamInfo = { type: 'stream.info', codec: 'h264', width: 1280, height: 720, sourceWidth: 1920, sourceHeight: 1080, fpsTarget: 60, audio: { sampleRate: 48000, channels: 2, format: 's16le', available: false } };

// A Compose install with a LAN address and a public one. The Host connects at
// `hostAt`, the client at `clientAt`; each is one of the configured addresses.
async function broker(t, { hostAt, clientAt, extra = {}, hostRoute = null }) {
  const port = await freePort(), lan = 'lan.example.test:' + port, outside = '203.0.113.7:' + port, six = '[2001:db8::10]:' + port;
  const config = configuration(docker({ MULTITAP_HOST: '127.0.0.1', MULTITAP_PORT: String(port), MULTITAP_PUBLISHED_PORT: '0.0.0.0:' + port, MULTITAP_HTTP_ORIGIN: ['http://' + lan, 'http://' + outside, 'http://' + six].join(','), ...extra }), noFile);
  const store = new Store(':memory:'), sockets = [];
  store.addUser({ name: 'alice', role: 'user', passwordHash: 'fixture-unused' });
  store.syncProxmox([{ vmid: 149, node: 'fixture', name: 'VM149', status: 'running' }], 'fixture');
  store.assign('alice', 'pve-149');
  const tokens = { alice: newToken(), host: newToken() }, code = newToken(), machine = '12345678-1234-1234-1234-123456789abc';
  store.createSession(store.findUser('alice'), tokenHash(tokens.alice), Date.now(), 3600000, 'client');
  store.issueEnrollment('pve-149', machine, tokenHash(code), 'fixture');
  store.consumeEnrollment(tokenHash(code), machine, { hostname: 'Fixture', version: '9.0.0', tokenHash: tokenHash(tokens.host) });
  // The relay's own socket is a free loopback port here; what the legs are told is the published port.
  const server = await buildServer({ store, vault: new Vault(randomBytes(32)), publicOrigin: config.publicOrigin, allowInsecureHttp: config.allowInsecureHttp, accessConfig: config.accessConfig, accessRevision: config.accessRevision, trustedProxies: config.trustedProxies,
    deploymentKind: 'docker', relayOptions: { udp: config.udp && { ...config.udp, host: '127.0.0.1', port: 0, logger: { warn: () => {} } }, hostRoute }, initialSetupOptions: { announce: () => {} }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(port, '127.0.0.1', resolve));
  t.after(async () => { for (const ws of sockets) ws.terminate(); await new Promise(resolve => server.close(resolve)); store.close(); });
  const at = { lan, outside, six };
  async function open(path, authorization, where) {
    const ws = new WebSocket('ws://127.0.0.1:' + port + path, { headers: { Authorization: authorization, Host: at[where] }, perMessageDeflate: false });
    Object.assign(ws, { messages: [], frames: [] });
    ws.on('message', (value, binary) => binary ? ws.frames.push(value) : ws.messages.push(JSON.parse(value.toString('utf8'))));
    ws.on('error', () => {});
    await once(ws, 'open'); sockets.push(ws);
    return ws;
  }
  const call = (path, method = 'GET', data) => new Promise((resolve, reject) => {
    const body = data ? JSON.stringify(data) : null;
    const req = request({ hostname: '127.0.0.1', port, path: '/api/v1' + path, method, headers: { Host: at[clientAt], Authorization: 'Bearer ' + tokens.alice, ...(body ? { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body) } : {}) } }, res => {
      const chunks = []; res.on('data', chunk => chunks.push(chunk)); res.once('end', () => resolve({ status: res.statusCode, body: JSON.parse(Buffer.concat(chunks).toString('utf8')) }));
    });
    req.on('error', reject); req.end(body ?? undefined);
  });
  const host = await open('/api/v1/hosts/channel', 'Host ' + tokens.host, hostAt);
  host.send(JSON.stringify({ type: 'host.capabilities', capabilities: { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true, udpMediaProtocol } }));
  await until(() => host.messages.some(value => value.type === 'host.capabilities.ack'));
  if (config.udp) await until(() => server.relay.udp.running);
  const start = async () => {
    const reply = await call('/desktops/pve-149/connect', 'POST', { udpMediaProtocol });
    assert.equal(reply.status, 200);
    const id = reply.body.sessionId, client = await open('/api/v1/sessions/' + id, 'Bearer ' + tokens.alice, clientAt);
    await until(() => host.messages.some(value => value.type === 'session.start' && value.sessionId === id));
    host.send(JSON.stringify(streamInfo));
    await until(() => client.messages.some(value => value.type === 'stream.info'));
    const video = Buffer.alloc(18); video[0] = 1; video[1] = 1; video.set([0, 0, 0, 1, 0x65, 1], 12);
    host.send(video);
    await until(() => client.frames.length === 1);
    return { client, id };
  };
  const caps = async () => (await call('/desktops')).body.desktops[0].host.capabilities;
  return { server, host, start, caps, config, port };
}
const grants = ws => ws.messages.filter(value => value.type === 'udp.grant');

test('The real broker: a Host inside and a client outside each get the address they connect to, with keys, and never the other one\'s', { timeout: 20000 }, async t => {
  const f = await broker(t, { hostAt: 'lan', clientAt: 'outside' });
  assert.equal(f.config.udp.derivedPort, 18481);
  assert.equal((await f.caps()).udpMediaProtocol, udpMediaProtocol, 'a client is offered UDP without any relay list in .env');
  const { client } = await f.start();
  await until(() => grants(client).length === 1 && grants(f.host).length === 1);
  assert.deepEqual(grants(client)[0].relays, [{ host: '203.0.113.7', port: 18481 }]);
  assert.deepEqual(grants(f.host)[0].relays, [{ host: 'lan.example.test', port: 18481 }]);
  assert.equal(grants(client)[0].keys, true, 'media, not only the reachability probe'); assert.equal(grants(f.host)[0].keys, true);
  assert.equal(JSON.stringify(client.messages).includes('lan.example.test'), false); assert.equal(JSON.stringify(f.host.messages).includes('203.0.113.7'), false);
  // The console's read-only view.
  assert.deepEqual(f.server.relay.udp.describe(), { running: true, port: 0, derivedPort: 18481, media: 'on', relays: { host: [], client: [] }, hostRoute: null });
});

test('The real broker: both inside at the LAN address, with the Host first given its route; MULTITAP_UDP_MEDIA=off leaves only the probe', { timeout: 20000 }, async t => {
  const f = await broker(t, { hostAt: 'lan', clientAt: 'lan', hostRoute: '192.0.2.5', extra: { MULTITAP_UDP_MEDIA: 'off' } });
  const { client } = await f.start();
  await until(() => grants(client).length === 1 && grants(f.host).length === 1);
  assert.deepEqual(grants(client)[0].relays, [{ host: 'lan.example.test', port: 18481 }]);
  assert.deepEqual(grants(f.host)[0].relays, [{ host: '192.0.2.5', port: 18481 }, { host: 'lan.example.test', port: 18481 }]);
  assert.equal(Object.hasOwn(grants(client)[0], 'keys'), false, 'no keys: the picture stays on WebSocket');
});

test('The real broker: a leg whose address gives no IPv4 endpoint gets no grant, and the session runs on WebSocket', { timeout: 20000 }, async t => {
  const f = await broker(t, { hostAt: 'lan', clientAt: 'six' });
  const { client } = await f.start();
  await delay(150);
  assert.equal(grants(client).length, 0); assert.equal(grants(f.host).length, 0, 'nor is a ticket sent to the other leg');
  assert.equal(client.frames.length, 1); assert.equal(client.readyState, WebSocket.OPEN); assert.equal(f.server.relay.udp.grants.size, 0);
});

test('The real broker: without the published UDP port nothing is derived and no client is offered UDP', { timeout: 20000 }, async t => {
  const f = await broker(t, { hostAt: 'lan', clientAt: 'outside', extra: { MULTITAP_UDP_PUBLISHED_PORT: '' } });
  assert.equal(f.config.udp.derivedPort, undefined);
  assert.equal(Object.hasOwn(await f.caps(), 'udpMediaProtocol'), false);
  const { client } = await f.start();
  await delay(100);
  assert.equal(grants(client).length, 0); assert.equal(grants(f.host).length, 0);
});
