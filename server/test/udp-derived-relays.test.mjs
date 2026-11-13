// UDP media of a Docker Compose install that names no relay list: each leg is
// given the address it already reaches the broker at, on the UDP port Docker
// publishes. Media goes over it only where it is turned on: by the switch the
// install saves with its access settings, or by MULTITAP_UDP_MEDIA in .env.
// The configuration cases, the list of one leg, and the real
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
import { mkdtemp, rm, readFile } from 'node:fs/promises';
import { readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { configuration } from '../src/config.mjs';
import { SelfAccess, accessProbeProtocol } from '../src/self-access.mjs';
import { main as accessCli } from '../src/access-cli.mjs';
import { selfAccessStorage } from '../src/config.mjs';
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

test('A Compose install without relay lists derives them from the published UDP port; media over UDP is off until it is turned on, and explicit .env values win', () => {
  const derived = udpOf({});
  assert.equal(derived.derivedPort, 18481); assert.equal(derived.media, false, 'not said anywhere: off'); assert.deepEqual(derived.relays, { host: [], client: [] }); assert.equal(derived.port, 18481);
  assert.deepEqual(configuration(docker({}), noFile).selfAccess.udpSwitch, { relay: true, env: null }); assert.equal(configuration(docker({}), noFile).selfAccess.udpMedia, false);
  for (const [value, env] of [['on', 'on'], ['off', 'off'], ['pve-101,pve-102', 'desktops']]) assert.equal(configuration(docker({ MULTITAP_UDP_MEDIA: value }), noFile).selfAccess.udpSwitch.env, env);
  assert.deepEqual(configuration(docker({ MULTITAP_UDP_PORT: '' }), noFile).selfAccess.udpSwitch, { relay: false, env: null });
  // The port Docker publishes, not the one the relay listens on inside the container.
  assert.equal(udpOf({ MULTITAP_UDP_PUBLISHED_PORT: '0.0.0.0:28481' }).derivedPort, 28481);
  assert.equal(udpOf({ MULTITAP_UDP_PUBLISHED_PORT: '192.0.2.10:28481' }).derivedPort, 28481);
  // MULTITAP_UDP_MEDIA still decides when it is set: a desktop list, or anything that is not `on`.
  assert.deepEqual([...udpOf({ MULTITAP_UDP_MEDIA: 'pve-101,pve-102' }).media], ['pve-101', 'pve-102']);
  // `off` is off, not a desktop named "off": the console is told so (describe()).
  assert.equal(udpOf({ MULTITAP_UDP_MEDIA: 'off' }).media, false); assert.equal(udpOf({ MULTITAP_UDP_MEDIA: 'on' }).media, true);
  assert.equal(relayOf({ derivedPort: 18481, media: udpOf({ MULTITAP_UDP_MEDIA: 'off' }).media }).describe().media, 'off');
  assert.equal(relayOf({ derivedPort: 18481, media: udpOf({ MULTITAP_UDP_MEDIA: 'pve-101' }).media }).describe().media, 'desktops');
  assert.equal(relayOf({ derivedPort: 18481, media: udpOf({}).media }).describe().media, 'off');
  assert.equal(relayOf({ derivedPort: 18481, media: udpOf({ MULTITAP_UDP_MEDIA: 'on' }).media }).describe().media, 'on');
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
const relayOf = values => Object.create(UdpRelay.prototype, Object.getOwnPropertyDescriptors({ running: true, derivedPort: null, hostRoute: null, relays: { host: [], client: [] }, media: false, limits: { port: 18481 }, grants: new Set(), counts: new Float64Array(16), ...values }));
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
  assert.deepEqual(routed.describe(), { running: true, port: 18481, derivedPort: 18481, media: 'off', sessions: 0, bindings: 0, relays: { host: [], client: [] }, hostRoute: '192.0.2.10' });

  // Configured lists are everyone's, whatever address they connect to.
  const lists = { host: [{ host: '192.0.2.121', port: 18481 }], client: [{ host: 'vdi.example.com', port: 18482 }] };
  const configured = relayOf({ relays: lists, hostRoute: '192.0.2.10', media: new Set(['pve-101']) });
  assert.equal(configured.available, true);
  assert.deepEqual(configured.relaysFor('client', 'http://192.0.2.10:18480'), lists.client); assert.deepEqual(configured.relaysFor('host', 'http://[2001:db8::10]:18480'), lists.host);
  assert.deepEqual(configured.describe(), { running: true, port: 18481, derivedPort: null, media: 'desktops', sessions: 0, bindings: 0, relays: lists, hostRoute: null });
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
// `volume`: a data folder, so the install keeps its access settings itself
// (src/self-access.mjs) as a real one does; `f.access` is that client and
// `f.exits` counts the restarts it asks for.
async function broker(t, { hostAt, clientAt, extra = {}, hostRoute = null, volume = null }) {
  const port = await freePort(), lan = 'lan.example.test:' + port, outside = '203.0.113.7:' + port, six = '[2001:db8::10]:' + port;
  const settings = docker({ MULTITAP_HOST: '127.0.0.1', MULTITAP_PORT: String(port), MULTITAP_PUBLISHED_PORT: '0.0.0.0:' + port, MULTITAP_HTTP_ORIGIN: ['http://' + lan, 'http://' + outside, 'http://' + six].join(','), ...(volume ? { MULTITAP_DATA_DIR: volume } : {}), ...extra });
  const reader = volume ? { readAccessFile: plainFile } : noFile, config = configuration(settings, reader), run = { exits: 0 };
  const access = volume ? SelfAccess.from(config, { exit: () => { run.exits++; }, load: () => configuration(settings, reader), sleep: () => new Promise(resolve => setImmediate(resolve)), pollMs: 1,
    probeRequest: async (origin, nonce) => ({ answer: { protocol: accessProbeProtocol, nonce, admitted: true } }) }) : null;
  await access?.initialize();
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
    ...(access ? { accessClient: access, accessInstallationId: config.accessInstallationId, accessIdentityFile: config.accessIdentityFile, accessIdentityRead: () => config.selfAccess.read(config.accessIdentityFile, 128) } : {}),
    deploymentKind: 'docker', relayOptions: { udp: config.udp && { ...config.udp, host: '127.0.0.1', port: 0, logger: { warn: () => {} } }, hostRoute }, initialSetupOptions: { announce: () => {} }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(port, '127.0.0.1', resolve));
  t.after(async () => { for (const ws of sockets) ws.terminate(); await new Promise(resolve => server.close(resolve)); access?.close(); store.close(); });
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
  return { server, host, start, caps, config, port, access, run, settings, lan: 'http://' + lan };
}
const plainFile = path => { try { return readFileSync(path, 'utf8'); } catch (error) { if (error.code === 'ENOENT') return null; throw error; } };
const grants = ws => ws.messages.filter(value => value.type === 'udp.grant');

test('The real broker, media over UDP turned on in .env: a Host inside and a client outside each get the address they connect to, with keys, and never the other one\'s', { timeout: 20000 }, async t => {
  const f = await broker(t, { hostAt: 'lan', clientAt: 'outside', extra: { MULTITAP_UDP_MEDIA: 'on' } });
  assert.equal(f.config.udp.derivedPort, 18481);
  assert.equal((await f.caps()).udpMediaProtocol, udpMediaProtocol, 'a client is offered UDP without any relay list in .env');
  const { client } = await f.start();
  await until(() => grants(client).length === 1 && grants(f.host).length === 1);
  assert.deepEqual(grants(client)[0].relays, [{ host: '203.0.113.7', port: 18481 }]);
  assert.deepEqual(grants(f.host)[0].relays, [{ host: 'lan.example.test', port: 18481 }]);
  assert.equal(grants(client)[0].keys, true, 'media, not only the reachability probe'); assert.equal(grants(f.host)[0].keys, true);
  assert.equal(JSON.stringify(client.messages).includes('lan.example.test'), false); assert.equal(JSON.stringify(f.host.messages).includes('203.0.113.7'), false);
  // The console's read-only view.
  assert.deepEqual(f.server.relay.udp.describe(), { running: true, port: 0, derivedPort: 18481, media: 'on', sessions: 1, bindings: 0, relays: { host: [], client: [] }, hostRoute: null });
});

test('The real broker, nothing said about media: the addresses are still given (the port can be measured), without keys, so picture and sound stay on WebSocket', { timeout: 20000 }, async t => {
  const f = await broker(t, { hostAt: 'lan', clientAt: 'outside' });
  assert.equal(f.config.udp.derivedPort, 18481); assert.equal(f.config.udp.media, false);
  assert.equal((await f.caps()).udpMediaProtocol, udpMediaProtocol, 'the probe is still offered');
  const { client } = await f.start();
  await until(() => grants(client).length === 1 && grants(f.host).length === 1);
  assert.deepEqual(grants(client)[0].relays, [{ host: '203.0.113.7', port: 18481 }]); assert.deepEqual(grants(f.host)[0].relays, [{ host: 'lan.example.test', port: 18481 }]);
  assert.equal(Object.hasOwn(grants(client)[0], 'keys'), false); assert.equal(Object.hasOwn(grants(f.host)[0], 'keys'), false);
  // A key a peer sends anyway is not passed on.
  client.send(JSON.stringify({ type: 'udp.key', protocol: udpMediaProtocol, publicKey: Buffer.alloc(32, 7).toString('base64') }));
  await delay(100);
  assert.equal(f.host.messages.some(value => value.type === 'udp.key'), false);
  assert.equal(f.server.relay.udp.describe().media, 'off');
});

// The switch on the server access page, as the console's 연결 테스트 and 변경 저장 reach the server.
const withSwitch = (applied, on) => { const { udpMedia: _was, ...config } = applied; return on ? { ...config, udpMedia: true } : config; };
async function saveSwitch(f, on) {
  const runtime = f.server.accessRuntime, context = { managementOrigin: f.lan }, status = await runtime.status(context), config = withSwitch(status.applied, on);
  const tested = await runtime.test({ config, baseRevision: status.revision }, context);
  const saved = await runtime.save({ config, testId: tested.testId, baseRevision: status.revision, confirm: true }, context, { actor: 'owner' });
  await f.access.task;
  return { tested, saved, status: await runtime.status({ ...context, udpMedia: f.server.relay.udp.describe() }) };
}

test('The real broker, the saved switch: off on a fresh install; turned on and off by a test and a save without a restart or a disconnection; sessions that connect afterwards get keys or not', { timeout: 30000 }, async t => {
  const volume = await mkdtemp(join(tmpdir(), 'multitap-udp-switch-'));
  t.after(() => rm(volume, { recursive: true, force: true }));
  const f = await broker(t, { hostAt: 'lan', clientAt: 'lan', volume });
  const before = await f.server.accessRuntime.status({ managementOrigin: f.lan });
  assert.deepEqual(before.udpSwitch, { saved: false, relay: true, env: null }); assert.equal(Object.hasOwn(before.applied, 'udpMedia'), false);
  const first = await f.start();
  await until(() => grants(first.client).length === 1);
  assert.equal(Object.hasOwn(grants(first.client)[0], 'keys'), false, 'a fresh install: the probe only');
  // Turned on. Only the switch changes: no address is asked, nothing restarts, the session that runs stays as it is.
  const on = await saveSwitch(f, true);
  assert.equal(on.tested.addressesUnchanged, true); assert.deepEqual(on.tested.checks.map(check => check.name), ['configuration', 'managed_installation']);
  assert.equal(on.status.state, 'applied'); assert.equal(on.status.applied.udpMedia, true); assert.deepEqual(on.status.udpSwitch, { saved: true, relay: true, env: null });
  assert.equal(f.run.exits, 0, 'no restart'); assert.equal(first.client.readyState, WebSocket.OPEN, 'nobody was disconnected');
  assert.equal(f.server.relay.sessions.has(first.id), true); assert.equal(f.server.relay.udp.describe().media, 'on');
  assert.equal(JSON.parse(await readFile(join(volume, 'access', 'policy.json'), 'utf8')).udpMedia, true);
  assert.equal(grants(first.client).length, 1, 'the session already running is not re-granted');
  await delay(2200);
  assert.equal(f.server.accessRuntime.blocked, false, 'the save\'s fence is given back');
  // The next session gets keys.
  first.client.close(); await until(() => !f.server.relay.sessions.has(first.id));
  const second = await f.start();
  await until(() => grants(second.client).length === 1 && grants(f.host).length === 2);
  assert.equal(grants(second.client)[0].keys, true); assert.equal(grants(f.host)[1].keys, true);
  // The command line reads the same state, and a start of the server would use it.
  let printed = '';
  assert.equal(await accessCli(['show'], { env: f.settings, stdout: { write: chunk => { printed += chunk; } }, configure: value => configuration(value, { readAccessFile: plainFile }), storage: value => selfAccessStorage(value, { readAccessFile: plainFile }) }), 0);
  assert.match(printed, /^udpMedia=1$/m); assert.match(printed, /^udpMediaEnv=$/m); assert.match(printed, /^udpMediaInUse=on$/m);
  assert.equal(configuration(f.settings, { readAccessFile: plainFile }).udp.media, true);
  // Turned off again: the same, the other way.
  const off = await saveSwitch(f, false);
  assert.equal(off.status.state, 'applied'); assert.equal(Object.hasOwn(off.status.applied, 'udpMedia'), false); assert.equal(off.status.udpSwitch.saved, false);
  assert.equal(f.run.exits, 0); assert.equal(second.client.readyState, WebSocket.OPEN); assert.equal(f.server.relay.udp.describe().media, 'off');
  assert.equal(Object.hasOwn(JSON.parse(await readFile(join(volume, 'access', 'policy.json'), 'utf8')), 'udpMedia'), false, 'a policy with the switch off is the policy an earlier version wrote');
  await delay(2200);
  second.client.close(); await until(() => !f.server.relay.sessions.has(second.id));
  const third = await f.start();
  await until(() => grants(third.client).length === 1);
  assert.equal(Object.hasOwn(grants(third.client)[0], 'keys'), false);
  assert.equal(configuration(f.settings, { readAccessFile: plainFile }).udp.media, false);
  // Turned on from the command line under the running server: taken over at its next look at the volume, again without a restart.
  let said = '';
  assert.equal(await accessCli(['udp-media', 'on'], { env: f.settings, stdout: { write: chunk => { said += chunk; } }, configure: value => configuration(value, { readAccessFile: plainFile }), storage: value => selfAccessStorage(value, { readAccessFile: plainFile }) }), 0, said);
  assert.equal(f.server.relay.udp.describe().media, 'off');
  await f.access.look();
  assert.equal(f.server.relay.udp.describe().media, 'on'); assert.equal(f.run.exits, 0); assert.equal(third.client.readyState, WebSocket.OPEN);
  assert.equal((await f.server.accessRuntime.status({ managementOrigin: f.lan })).udpSwitch.saved, true);
});

test('The real broker, .env decides: the saved switch is kept but does not change what runs, and the console is told the value that is fixed', { timeout: 30000 }, async t => {
  for (const [value, env, media] of [['off', 'off', 'off'], ['on', 'on', 'on'], ['pve-149', 'desktops', 'desktops']]) {
    const volume = await mkdtemp(join(tmpdir(), 'multitap-udp-env-'));
    t.after(() => rm(volume, { recursive: true, force: true }));
    const f = await broker(t, { hostAt: 'lan', clientAt: 'lan', volume, extra: { MULTITAP_UDP_MEDIA: value } });
    const status = await f.server.accessRuntime.status({ managementOrigin: f.lan });
    assert.deepEqual(status.udpSwitch, { saved: false, relay: true, env }, value); assert.equal(f.server.relay.udp.describe().media, media, value);
    // A save that asks for the other value changes nothing: there is nothing to save.
    const context = { managementOrigin: f.lan }, config = withSwitch(status.applied, value !== 'on');
    const tested = await f.server.accessRuntime.test({ config, baseRevision: status.revision }, context);
    await assert.rejects(f.server.accessRuntime.save({ config, testId: tested.testId, baseRevision: status.revision, confirm: true }, context, { actor: 'owner' }), error => error.code === 'server_access_change_required', value);
    assert.equal(f.server.relay.udp.describe().media, media, value);
  }
});

test('An install updated from the version that sent media over UDP unasked: its saved policy says nothing about it, so it is off; the command line turns the saved switch on and off', { timeout: 30000 }, async t => {
  const volume = await mkdtemp(join(tmpdir(), 'multitap-udp-upgrade-'));
  t.after(() => rm(volume, { recursive: true, force: true }));
  const settings = docker({ MULTITAP_DATA_DIR: volume }), read = { readAccessFile: plainFile };
  const cli = async (...argv) => { let text = ''; const code = await accessCli(argv, { env: settings, stdout: { write: chunk => { text += chunk; } }, configure: value => configuration(value, read), storage: value => selfAccessStorage(value, read) });
    return { code, values: Object.fromEntries(text.split('\n').filter(Boolean).map(line => [line.slice(0, line.indexOf('=')), line.slice(line.indexOf('=') + 1)])) }; };
  // The policy that version saved (no `udpMedia`), beside the same .env.
  assert.equal((await cli('set', 'http://192.0.2.10:18480', 'http://vdi.example.com:18480')).code, 0);
  assert.equal(Object.hasOwn(JSON.parse(await readFile(join(volume, 'access', 'policy.json'), 'utf8')), 'udpMedia'), false);
  const upgraded = configuration(settings, read);
  assert.equal(upgraded.selfAccess.saved, true); assert.equal(upgraded.udp.derivedPort, 18481); assert.equal(upgraded.udp.media, false, 'back on WebSocket without anybody doing anything');
  const shown = await cli('udp-media', 'status');
  assert.deepEqual([shown.code, shown.values.udpMedia, shown.values.udpMediaEnv, shown.values.udpMediaInUse], [0, '0', '', 'off']);
  // On, from the command line: saved in the policy, the addresses untouched.
  const revision = shown.values.revision, on = await cli('udp-media', 'on');
  assert.deepEqual([on.code, on.values.changed, on.values.udpMedia, on.values.udpMediaInUse, on.values.http], [0, '1', '1', 'on', 'http://192.0.2.10:18480,http://vdi.example.com:18480']);
  assert.notEqual(on.values.revision, revision);
  assert.equal(configuration(settings, read).udp.media, true); assert.equal(configuration(settings, read).accessRevision, upgraded.accessRevision, 'the addresses, and with them the health check, are the same');
  assert.equal((await cli('udp-media', 'on')).values.changed, '0');
  // Other changes keep it.
  assert.equal((await cli('set', 'http://192.0.2.10:18480')).values.udpMedia, '1'); assert.equal((await cli('public-ip', 'off')).values.udpMedia, '1');
  const off = await cli('udp-media', 'off');
  assert.deepEqual([off.code, off.values.changed, off.values.udpMedia, off.values.udpMediaInUse], [0, '1', '0', 'off']);
  // .env wins over the saved switch, and the output says so.
  await cli('udp-media', 'on');
  const fixed = await accessOutput(['show'], { ...settings, MULTITAP_UDP_MEDIA: 'off' }, read);
  assert.deepEqual([fixed.udpMedia, fixed.udpMediaEnv, fixed.udpMediaInUse], ['1', 'off', 'off']);
  for (const argv of [['udp-media'], ['udp-media', 'maybe'], ['udp-media', 'on', 'extra'], ['udp-media', 'status', '--force']]) assert.equal((await cli(...argv)).code, 64, argv.join(' '));
  // Without any addresses there is no policy to keep the switch in.
  const empty = await mkdtemp(join(tmpdir(), 'multitap-udp-empty-'));
  t.after(() => rm(empty, { recursive: true, force: true }));
  const fresh = { ...docker({ MULTITAP_DATA_DIR: empty, MULTITAP_HTTP_ORIGIN: '', MULTITAP_ALLOW_INSECURE_HTTP: '0' }), NODE_ENV: 'production' };
  let text = '';
  assert.equal(await accessCli(['udp-media', 'on'], { env: fresh, stdout: { write: chunk => { text += chunk; } }, configure: value => configuration(value, read), storage: value => selfAccessStorage(value, read) }), 2);
  assert.match(text, /^ok=0\ncode=server_access_udp_media_unavailable\n$/);
});
async function accessOutput(argv, env, read) {
  let text = '';
  await accessCli(argv, { env, stdout: { write: chunk => { text += chunk; } }, configure: value => configuration(value, read), storage: value => selfAccessStorage(value, read) });
  return Object.fromEntries(text.split('\n').filter(Boolean).map(line => [line.slice(0, line.indexOf('=')), line.slice(line.indexOf('=') + 1)]));
}

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
