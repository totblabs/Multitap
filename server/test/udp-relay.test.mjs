// Real loopback UDP and the real worker thread, with synthetic peers. This
// proves the relay only: no Host, client, NAT, router or internet path is here.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { randomBytes } from 'node:crypto';
import { createSocket } from 'node:dgram';
import { once } from 'node:events';
import { mkdtemp, rm } from 'node:fs/promises';
import { createServer } from 'node:net';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { WebSocket } from 'ws';
import { configuration } from '../src/config.mjs';
import { buildServer } from '../src/http.mjs';
import { ACK, BIND, BIND_BYTES, DATA, HEADER_BYTES, MAX_DATAGRAM, ackDatagram, bindDatagram, dataDatagram } from '../src/media-udp.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { Store } from '../src/store.mjs';
import { UdpRelay, kernelSocket } from '../src/udp-relay.mjs';
import { activeSessions } from '../src/admin-monitoring.mjs';
import { Vault } from '../src/vault.mjs';

const quiet = { warn() {} };
const loopback = { host: '127.0.0.1', port: 0, logger: quiet };
async function until(predicate, ms = 4000) {
  const end = Date.now() + ms;
  while (!predicate()) { assert.ok(Date.now() < end, 'fixture_timeout'); await delay(2); }
}
async function peer(t, relay, address = '127.0.0.1', port = 0) {
  const socket = createSocket('udp4'), value = { received: [], lastAt: 0, sent: 0 };
  socket.on('message', data => { value.received.push(data); value.lastAt = performance.now(); });
  await new Promise(resolve => socket.bind(port, address, resolve));
  t.after(() => new Promise(resolve => socket.close(resolve)));
  value.port = socket.address().port;
  value.send = bytes => new Promise((resolve, reject) => socket.send(bytes, relay.port, '127.0.0.1', error => error ? reject(error) : (value.sent++, resolve())));
  return value;
}
// Like a real peer: the grant or the ack may lose a race or a datagram, so
// the proof is repeated until the ack arrives.
async function bind(from, leg) {
  const proof = bindDatagram(leg.ticket, leg.id), ack = ackDatagram(leg.ticket, leg.id), end = Date.now() + 4000;
  while (!from.received.some(data => data.equals(ack))) { assert.ok(Date.now() < end, 'bind_timeout'); await from.send(proof); await delay(5); }
  from.received.length = 0;
}
async function udpRelay(t, options = {}, sessions = new Set(['one', 'two'])) {
  const relay = new UdpRelay({ ...loopback, live: id => sessions.has(id), ...options });
  t.after(() => relay.close());
  await until(() => relay.running);
  return Object.assign(relay, { sessions });
}
async function pair(t, relay, session = 'one') {
  const grant = relay.issue(session), client = await peer(t, relay), host = await peer(t, relay);
  assert.ok(grant);
  await bind(client, grant.client); await bind(host, grant.host);
  return { grant, client, host };
}
async function counted(relay, read, send) {
  const before = read(relay.stats());
  await send();
  await until(() => read(relay.stats()) > before);
}

// A real Relay with a real attached WebSocket session: only the Host, client
// and VM are synthetic.
async function broker(t, { udp = {}, relayOptions = {} } = {}) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32)), sockets = [];
  store.addUser({ name: 'alice', role: 'user', passwordHash: 'fixture-unused' });
  store.syncProxmox([{ vmid: 149, node: 'fixture', name: 'VM149', status: 'running' }], 'fixture');
  store.assign('alice', 'pve-149');
  const tokens = { alice: newToken(), host: newToken() }, code = newToken(), machine = '12345678-1234-1234-1234-123456789abc';
  store.createSession(store.findUser('alice'), tokenHash(tokens.alice), Date.now(), 3600000, 'client');
  store.issueEnrollment('pve-149', machine, tokenHash(code), 'fixture');
  store.consumeEnrollment(tokenHash(code), machine, { hostname: 'Fixture', version: '9.0.0', tokenHash: tokenHash(tokens.host) });
  const server = await buildServer({ store, vault, publicOrigin: 'https://relay-fixture.example', relayOptions: { ...relayOptions, ...(udp ? { udp: { ...loopback, ...udp } } : {}) }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { for (const ws of sockets) ws.terminate(); await new Promise(resolve => server.close(resolve)); store.close(); });
  const base = '127.0.0.1:' + server.address().port;
  async function open(path, authorization) {
    const ws = new WebSocket('ws://' + base + path, { headers: { Authorization: authorization }, perMessageDeflate: false });
    ws.messages = []; ws.on('message', value => ws.messages.push(JSON.parse(value.toString('utf8')))); ws.on('error', () => {});
    await once(ws, 'open'); sockets.push(ws);
    return ws;
  }
  const call = async (path, method = 'GET', data) => {
    const response = await fetch('http://' + base + '/api/v1' + path, { method, headers: { Authorization: 'Bearer ' + tokens.alice, ...(data ? { 'Content-Type': 'application/json' } : {}) }, body: data ? JSON.stringify(data) : undefined });
    return { status: response.status, body: await response.json() };
  };
  const host = await open('/api/v1/hosts/channel', 'Host ' + tokens.host);
  host.send(JSON.stringify({ type: 'host.capabilities', capabilities: { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true } }));
  await until(() => host.messages.some(value => value.type === 'host.capabilities.ack'));
  const connect = async () => { const reply = await call('/desktops/pve-149/connect', 'POST', {}); assert.equal(reply.status, 200); return reply.body; };
  const attach = async ticket => { const ws = await open('/api/v1/sessions/' + ticket.sessionId, 'Bearer ' + tokens.alice); await until(() => host.messages.some(value => value.type === 'session.start')); return ws; };
  if (udp) await until(() => server.relay.udp.running);
  return { store, server, relay: server.relay, udp: server.relay.udp, host, call, connect, attach };
}

const dropped = reason => stats => stats.dropped[reason];
const handles = () => { const report = process.report.getReport(); return { main: report.libuv.filter(handle => handle.type === 'udp').length, workers: report.workers.length, worker: report.workers.flatMap(worker => worker.libuv).filter(handle => handle.type === 'udp').length }; };

test('Framing is fixed: known answers pin the bytes a Host and client must produce', () => {
  const ticket = Buffer.alloc(32, 0x11), id = Buffer.from('0123456789abcdef', 'hex'), proof = bindDatagram(ticket, id), ack = ackDatagram(ticket, id);
  // Computed with .NET HMACSHA256, not with the code under test.
  assert.equal(proof.toString('hex'), '01' + '0123456789abcdef' + 'dee943b5a3bee007c23f4a8aee397ecc5fd2e4de1cda09797f261075f1c79b48');
  assert.equal(ack.toString('hex'), '02' + '0123456789abcdef' + '918d801de29f11d8372b339a618bf37c067632d18c8e54b3aff63c025124a950');
  assert.equal(proof.length, BIND_BYTES); assert.equal(proof[0], BIND); assert.equal(ack[0], ACK);
  const data = dataDatagram(id, Buffer.from([0xff, 0]));
  assert.deepEqual([...data.subarray(0, 1)], [DATA]); assert.deepEqual([...data.subarray(HEADER_BYTES)], [0xff, 0]);
});

test('The port setting is explicit, validated and off by default', () => {
  assert.equal(configuration({}).udp, null);
  assert.equal(configuration({ MULTITAP_UDP_PORT: '' }).udp, null);
  assert.equal(configuration({ MULTITAP_UDP_HOST: '127.0.0.1' }).udp, null);
  assert.deepEqual(configuration({ MULTITAP_UDP_PORT: '18443' }).udp, { port: 18443, host: '0.0.0.0', allowPeerDirect: false, stun: [], media: false, relays: { host: [], client: [] } });
  assert.deepEqual(configuration({ MULTITAP_UDP_PORT: '18443', MULTITAP_UDP_HOST: '127.0.0.1' }).udp, { port: 18443, host: '127.0.0.1', allowPeerDirect: false, stun: [], media: false, relays: { host: [], client: [] } });
  for (const port of ['0', '65536', '-1', '1.5', 'abc', ' 443', '443 ', '0x10', '1e3']) assert.throws(() => configuration({ MULTITAP_UDP_PORT: port }), /invalid_udp_port/, port);
  for (const host of ['::1', 'localhost', '1.2.3', '1.2.3.4.5']) assert.throws(() => configuration({ MULTITAP_UDP_PORT: '443', MULTITAP_UDP_HOST: host }), /invalid_udp_host/, host);
});

test('Disabled mode opens no socket and starts no worker; the check itself sees an enabled relay', async t => {
  const off = await broker(t, { udp: null });
  assert.equal(off.relay.udp, null);
  assert.deepEqual(handles(), { main: 0, workers: 0, worker: 0 });
  await off.attach(await off.connect());
  assert.deepEqual(handles(), { main: 0, workers: 0, worker: 0 });
  const on = await udpRelay(t);
  assert.deepEqual(handles(), { main: 0, workers: 1, worker: 1 });
  await on.close();
  await until(() => handles().workers === 0);
});

test('Nothing advertises the relay to a client or a Host', async t => {
  const f = await broker(t), ticket = await f.connect();
  await f.attach(ticket);
  const seen = JSON.stringify([ticket, f.host.messages, (await f.call('/desktops')).body]);
  assert.equal(/udp/i.test(seen), false);
});

test('Opaque datagrams cross in both directions, unchanged, up to the size limit', async t => {
  const relay = await udpRelay(t), { grant, client, host } = await pair(t, relay);
  const up = dataDatagram(grant.client.id, randomBytes(MAX_DATAGRAM - HEADER_BYTES)), down = dataDatagram(grant.host.id, randomBytes(64));
  await client.send(up); await host.send(down);
  await until(() => host.received.length === 1 && client.received.length === 1);
  assert.deepEqual(host.received[0], up); assert.deepEqual(client.received[0], down);
  const stats = relay.stats();
  assert.equal(stats.forwarded, 2); assert.equal(stats.forwardedBytes, up.length + down.length); assert.equal(stats.bindings, 2); assert.equal(stats.sessions, 1);
  assert.equal(Object.values(stats.dropped).reduce((sum, value) => sum + value, 0), 0);
});

test('A wrong HMAC binds nothing, gets no reply and does not spend the ticket', async t => {
  const relay = await udpRelay(t), grant = relay.issue('one'), attacker = await peer(t, relay), client = await peer(t, relay);
  const flipped = bindDatagram(grant.client.ticket, grant.client.id); flipped[flipped.length - 1] ^= 1;
  for (const forged of [bindDatagram(randomBytes(32), grant.client.id), flipped, bindDatagram(grant.client.ticket, grant.client.id).subarray(0, BIND_BYTES - 1), Buffer.concat([bindDatagram(grant.client.ticket, grant.client.id), Buffer.of(0)])]) {
    await counted(relay, dropped('bad_proof'), () => attacker.send(forged));
  }
  await delay(30);
  assert.equal(attacker.received.length, 0); assert.equal(relay.stats().bindings, 0);
  await bind(client, grant.client);
  assert.equal(relay.stats().bindings, 1);
});

test('A ticket binds once: a replay elsewhere is refused and only the pinned peer gets its ack again', async t => {
  const relay = await udpRelay(t), { grant, client, host } = await pair(t, relay), thief = await peer(t, relay);
  const proof = bindDatagram(grant.client.ticket, grant.client.id), ack = ackDatagram(grant.client.ticket, grant.client.id);
  await counted(relay, dropped('ticket_reused'), () => thief.send(proof));
  await client.send(proof);
  await until(() => client.received.length === 1);
  assert.deepEqual(client.received[0], ack);
  assert.ok(ack.length <= proof.length, 'no reply is larger than its request');
  assert.equal(relay.stats().bindings, 2);
  // The binding did not move: traffic still goes to the original address.
  const payload = dataDatagram(grant.host.id, randomBytes(32));
  client.received.length = 0;
  await host.send(payload);
  await until(() => client.received.length === 1);
  await delay(30);
  assert.equal(thief.received.length, 0);
});

test('An expired ticket binds nothing and gets no reply', async t => {
  const relay = await udpRelay(t, { ticketMs: 25 }), grant = relay.issue('one'), client = await peer(t, relay);
  await delay(50);
  await counted(relay, dropped('ticket_expired'), () => client.send(bindDatagram(grant.client.ticket, grant.client.id)));
  await delay(30);
  assert.equal(client.received.length, 0); assert.equal(relay.stats().bindings, 0);
});

test('After pinning, the same connection ID from another source is not forwarded', async t => {
  const relay = await udpRelay(t), { grant, client, host } = await pair(t, relay);
  // Each half of the pin alone: another port, then the very same port elsewhere.
  for (const spoofer of [await peer(t, relay), await peer(t, relay, '127.0.0.2', client.port)]) {
    await counted(relay, dropped('wrong_source'), () => spoofer.send(dataDatagram(grant.client.id, randomBytes(32))));
    await delay(30);
    assert.equal(spoofer.received.length, 0);
  }
  assert.equal(host.received.length, 0); assert.equal(relay.stats().forwarded, 0);
  await client.send(dataDatagram(grant.client.id, randomBytes(32)));
  await until(() => host.received.length === 1);
});

test('A source port of 0 pins nothing, and the worker and every other grant live on', async () => {
  const fixture = fileURLToPath(new URL('./udp-zero-port-fixture.mjs', import.meta.url));
  const child = spawn(process.execPath, ['--import', pathToFileURL(fixture).href, fixture], { stdio: ['ignore', 'pipe', 'inherit'] });
  let output = '';
  child.stdout.on('data', chunk => { output += chunk; });
  const [code] = await once(child, 'exit');
  assert.equal(code, 0);
  assert.deepEqual(JSON.parse(output), { running: true, restarts: 0, other: true, issued: true, bindings: 0, malformed: 1 });
});

test('Unknown datagrams never get a reply', async t => {
  const relay = await udpRelay(t), grant = relay.issue('one'), stranger = await peer(t, relay), unknown = randomBytes(8);
  const cases = [
    ['malformed', Buffer.of(DATA)], ['malformed', randomBytes(HEADER_BYTES - 1)], ['malformed', Buffer.concat([Buffer.of(0), grant.client.id])], ['malformed', Buffer.concat([Buffer.of(9), grant.client.id, randomBytes(100)])],
    // An ack sent back at the relay must not start a reflection loop.
    ['malformed', ackDatagram(grant.client.ticket, grant.client.id)],
    ['unknown_id', dataDatagram(unknown, randomBytes(100))], ['unknown_id', bindDatagram(randomBytes(32), unknown)],
    ['unbound', dataDatagram(grant.client.id, randomBytes(100))],
  ];
  for (const [reason, datagram] of cases) await counted(relay, dropped(reason), () => stranger.send(datagram));
  await delay(50);
  assert.equal(stranger.received.length, 0);
  const client = await peer(t, relay);
  await bind(client, grant.client);
  await counted(relay, dropped('unbound'), () => client.send(dataDatagram(grant.client.id, randomBytes(8))));
  await delay(30);
  assert.equal(client.received.length, 0, 'a bound leg with no peer yet gets nothing back');
});

test('Datagrams over 1200 bytes are dropped; exactly 1200 passes', async t => {
  const relay = await udpRelay(t), { grant, client, host } = await pair(t, relay);
  await counted(relay, dropped('oversize'), () => client.send(dataDatagram(grant.client.id, randomBytes(MAX_DATAGRAM - HEADER_BYTES + 1))));
  await delay(30);
  assert.equal(host.received.length, 0); assert.equal(client.received.length, 0);
  await client.send(dataDatagram(grant.client.id, randomBytes(MAX_DATAGRAM - HEADER_BYTES)));
  await until(() => host.received.length === 1);
  assert.equal(host.received[0].length, MAX_DATAGRAM);
});

// Upper bound only: `forwarded` may be anything below what the bucket allows.
const wireBytes = stats => stats.forwardedBytes + stats.forwarded * 64;
test('The per-session bucket limits one session and leaves another alone', async t => {
  const rate = 40000, burstMs = 100, relay = await udpRelay(t, { sessionBytesPerSecond: rate, burstMs });
  const one = await pair(t, relay, 'one'), two = await pair(t, relay, 'two'), started = performance.now();
  const payload = dataDatagram(one.grant.client.id, randomBytes(300));
  for (let index = 0; index < 200; index++) await one.client.send(payload);
  await until(() => relay.stats().dropped.rate_session + relay.stats().forwarded === 200);
  const stats = relay.stats(), elapsed = performance.now() - started;
  assert.ok(stats.dropped.rate_session > 0);
  assert.ok(wireBytes(stats) <= rate * burstMs / 1000 + rate * elapsed / 1000, 'forwarded at most burst plus rate');
  await two.client.send(dataDatagram(two.grant.client.id, randomBytes(300)));
  await until(() => two.host.received.length === 1);
  assert.equal(relay.stats().dropped.rate_global, 0);
});

test('The global bucket limits the sum of all sessions', async t => {
  const rate = 60000, burstMs = 100, relay = await udpRelay(t, { globalBytesPerSecond: rate, burstMs });
  const one = await pair(t, relay, 'one'), two = await pair(t, relay, 'two'), started = performance.now();
  const first = dataDatagram(one.grant.client.id, randomBytes(300)), second = dataDatagram(two.grant.client.id, randomBytes(300));
  for (let index = 0; index < 100; index++) { await one.client.send(first); await two.client.send(second); }
  await until(() => relay.stats().dropped.rate_global + relay.stats().forwarded === 200);
  const stats = relay.stats(), elapsed = performance.now() - started;
  assert.ok(stats.dropped.rate_global > 0); assert.equal(stats.dropped.rate_session, 0);
  assert.ok(wireBytes(stats) <= rate * burstMs / 1000 + rate * elapsed / 1000, 'forwarded at most burst plus rate');
});

test('A new ticket for a session drops the old binding', async t => {
  const relay = await udpRelay(t), { grant, client } = await pair(t, relay), next = relay.issue('one');
  await until(() => relay.stats().bindings === 0);
  assert.notDeepEqual(next.client.ticket, grant.client.ticket);
  await counted(relay, dropped('unknown_id'), () => client.send(dataDatagram(grant.client.id, randomBytes(8))));
  const newcomer = await peer(t, relay);
  await bind(newcomer, next.client);
  // Old ACKs may already be in the kernel queue. Inspect rejection of this
  // new attempt instead of mistaking a delayed ACK for a successful rebind.
  await counted(relay, dropped('unknown_id'), () => client.send(bindDatagram(grant.client.ticket, grant.client.id)));
  assert.equal(relay.stats().bindings, 1, 'only the replacement ticket is bound');
});

test('No ticket is issued for a session that may not hold one, or while the relay is down', async t => {
  const relay = await udpRelay(t, { live: () => { throw Error('store_failed'); } });
  assert.equal(relay.issue('one'), null);
  await relay.close();
  const stopped = await udpRelay(t);
  await stopped.close();
  assert.equal(stopped.issue('one'), null);
});

test('Counters are bounded and expose no ticket, key or address', async t => {
  const relay = await udpRelay(t), { grant, client } = await pair(t, relay), before = JSON.stringify(relay.stats());
  for (let index = 0; index < 64; index++) await client.send(dataDatagram(randomBytes(8), randomBytes(8)));
  await until(() => relay.stats().dropped.unknown_id === 64);
  const text = JSON.stringify(relay.stats());
  assert.equal(Object.keys(JSON.parse(text).dropped).length, Object.keys(JSON.parse(before).dropped).length);
  assert.ok(text.length < 512);
  for (const secret of [grant.client.ticket, grant.host.ticket, grant.client.id, grant.host.id]) for (const encoding of ['hex', 'base64', 'latin1']) assert.equal(text.includes(secret.toString(encoding)), false);
  assert.equal(text.includes('127.0.0.1'), false);
});

test('The socket buffers the kernel granted are reported, with the started log line', async t => {
  const lines = [], relay = await udpRelay(t, { logger: { warn() {}, info: line => lines.push(JSON.parse(line)) } }), stats = relay.stats();
  assert.ok(Number.isSafeInteger(stats.recvBuffer) && stats.recvBuffer > 0, 'recvBuffer');
  assert.ok(Number.isSafeInteger(stats.sendBuffer) && stats.sendBuffer > 0, 'sendBuffer');
  assert.deepEqual(lines, [{ event: 'udp_relay_started', port: relay.port, recvBuffer: stats.recvBuffer, sendBuffer: stats.sendBuffer }]);
  // Only Linux has /proc/net/udp; elsewhere the kernel view is absent, never invented.
  if (process.platform !== 'linux') assert.equal(stats.kernel, null);
  else assert.ok(Number.isSafeInteger(stats.kernel.drops) && Number.isSafeInteger(stats.kernel.rxQueue));
  await relay.close();
  assert.equal(relay.stats().recvBuffer, null); assert.equal(relay.stats().kernel, null);
});

test('The kernel view reads only the relay port\'s unconnected row of /proc/net/udp', () => {
  const table = [
    '   sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode ref pointer drops',
    '  101: 0100007F:4831 0A000001:1F90 01 00000000:00000000 00:00000000 00000000     0        0 11 2 0000000000000000 9',
    '  102: 00000000:4831 00000000:0000 07 00000000:00000A00 00:00000000 00000000     0        0 12 2 0000000000000000 17',
    '  103: 00000000:4832 00000000:0000 07 00000000:00000000 00:00000000 00000000     0        0 13 2 0000000000000000 99',
  ].join('\n');
  assert.deepEqual(kernelSocket(0x4831, () => table), { rxQueue: 0xa00, drops: 17 });
  assert.equal(kernelSocket(0x4833, () => table), null);
  assert.equal(kernelSocket(0x4831, () => { throw Object.assign(Error('missing'), { code: 'ENOENT' }); }), null);
  assert.equal(kernelSocket(0x4831, () => table.replace(' 17', ' x')), null);
});

test('The administrator sessions view carries the relay counters and buffers', async t => {
  const f = await broker(t), view = activeSessions(f.store, f.relay);
  assert.equal(view.udpRelay.port, f.udp.port); assert.equal(view.udpRelay.recvBuffer, f.udp.stats().recvBuffer);
  assert.equal('udpRelay' in activeSessions(f.store, { sessions: new Map() }), false);
});

// Each revocation path alone, with the other two held off. These call the
// relay directly: every API response runs revalidate(), which would hide a
// broken stop() hook.
const off = 3600000;
test('Ending a session drops its binding through stop(), without the sweeps', async t => {
  const f = await broker(t, { udp: { sweepMs: off }, relayOptions: { authorizationMs: off } }), ticket = await f.connect();
  await f.attach(ticket);
  const { grant, client, host } = await pair(t, f.udp, ticket.sessionId);
  f.relay.stop(f.relay.sessions.get(ticket.sessionId), 'client_stopped');
  await until(() => f.udp.stats().bindings === 0);
  await counted(f.udp, dropped('unknown_id'), () => client.send(dataDatagram(grant.client.id, randomBytes(8))));
  assert.equal(host.received.length, 0);
});

test('revalidate() drops a binding whose session left without stop()', async t => {
  const f = await broker(t, { udp: { sweepMs: off }, relayOptions: { authorizationMs: off } }), ticket = await f.connect();
  await pair(t, f.udp, ticket.sessionId);
  // An unattached ticket is released this way (cancel, takeover), never through stop().
  f.relay.sessions.delete(ticket.sessionId);
  await delay(50);
  assert.equal(f.udp.stats().bindings, 2);
  f.relay.revalidate();
  await until(() => f.udp.stats().bindings === 0);
});

test('The UDP sweep drops a binding on permission loss without the WebSocket tick', async t => {
  const f = await broker(t, { relayOptions: { authorizationMs: off } }), ticket = await f.connect();
  await f.attach(ticket);
  await pair(t, f.udp, ticket.sessionId);
  f.store.revoke('alice', 'pve-149');
  await until(() => f.udp.stats().bindings === 0, 240);
  assert.ok(f.relay.sessions.has(ticket.sessionId), 'the WebSocket session keeps its own 250 ms tick');
  f.relay.revalidate();
  assert.equal(f.relay.sessions.has(ticket.sessionId), false);
});

test('Host loss or revocation drops the binding', async t => {
  const f = await broker(t, { relayOptions: { authorizationMs: off } }), ticket = await f.connect();
  await pair(t, f.udp, ticket.sessionId);
  f.store.db.prepare('UPDATE hosts SET revoked=1').run();
  await until(() => f.udp.stats().bindings === 0, 240);
});

// Measured at the far peer, under traffic: the last datagram to arrive dates
// the moment the worker really stopped, whatever its counters say.
async function lastForwardedMs(t, f, end) {
  const ticket = await f.connect();
  await f.attach(ticket);
  const { grant, client, host } = await pair(t, f.udp, ticket.sessionId), payload = dataDatagram(grant.client.id, randomBytes(1000));
  let pumping = true;
  const pump = (async () => { while (pumping) { await client.send(payload); await client.send(payload); await delay(1); } })();
  await until(() => host.received.length > 10);
  // Any phase of the relay's own tick.
  await delay(Math.random() * 250);
  const ended = performance.now();
  await end(ticket);
  await delay(350);
  const last = host.lastAt, forwarded = f.udp.stats().forwarded;
  assert.ok(host.received.length > 10);
  await delay(50);
  assert.equal(f.udp.stats().forwarded, forwarded, 'traffic was still arriving and no longer crossed');
  pumping = false; await pump;
  return last - ended;
}

test('Under traffic, forwarding stops within 250 ms of session end and of permission loss, at default settings', async t => {
  const f = await broker(t), times = [];
  for (let round = 0; round < 4; round++) {
    times.push(await lastForwardedMs(t, f, ticket => f.call('/sessions/' + ticket.sessionId, 'DELETE')));
    await until(() => !f.relay.sessionStop.blocked('pve-149'));
    times.push(await lastForwardedMs(t, f, async () => f.store.revoke('alice', 'pve-149')));
    await until(() => f.relay.sessions.size === 0 && !f.relay.sessionStop.blocked('pve-149'));
    f.store.assign('alice', 'pve-149');
  }
  t.diagnostic('last forwarded after end (ms): ' + times.map(value => value.toFixed(1)).join(' '));
  assert.ok(Math.max(...times) <= 250, times.join(' '));
});

test('A worker crash leaves the broker and its WebSocket session running, and spends every ticket', async t => {
  const f = await broker(t, { udp: { restartMs: 40 } }), ticket = await f.connect(), ws = await f.attach(ticket);
  const { grant, client } = await pair(t, f.udp, ticket.sessionId), worker = f.udp.worker;
  // Not events.once(): it would reject on the 'error' this test provokes.
  const exited = new Promise(resolve => worker.once('exit', resolve));
  // A grant without legs is a real uncaught TypeError inside the worker.
  worker.postMessage({ type: 'grant' });
  assert.equal(await exited, 1);
  assert.equal(f.udp.stats().sessions, 0); assert.equal(f.udp.stats().bindings, 0);
  assert.equal(f.udp.issue(ticket.sessionId), null, 'no ticket while the relay is down');
  assert.equal((await f.call('/desktops')).status, 200);
  assert.ok(f.relay.sessions.has(ticket.sessionId));
  assert.equal(ws.readyState, WebSocket.OPEN);
  assert.equal(f.host.messages.some(value => value.type === 'session.stop'), false);
  await until(() => f.udp.running);
  assert.equal(f.udp.stats().restarts, 1);
  await counted(f.udp, dropped('unknown_id'), () => client.send(bindDatagram(grant.client.ticket, grant.client.id)));
  await delay(30);
  assert.equal(client.received.length, 0, 'an old ticket does not survive the restart');
  const next = await pair(t, f.udp, ticket.sessionId);
  await next.client.send(dataDatagram(next.grant.client.id, randomBytes(8)));
  await until(() => next.host.received.length === 1);
});

test('A port that is already taken is reported once and costs the broker nothing', async t => {
  const holder = createSocket('udp4'), lines = [];
  await new Promise(resolve => holder.bind(0, '127.0.0.1', resolve));
  const port = holder.address().port, f = await broker(t, { udp: false });
  const relay = new UdpRelay({ host: '127.0.0.1', port, restartMs: 10, live: () => true, logger: { warn: line => lines.push(line) } });
  t.after(() => relay.close());
  await until(() => relay.stats().restarts >= 3);
  assert.deepEqual(lines, [JSON.stringify({ event: 'udp_relay_failed', code: 'EADDRINUSE' })]);
  assert.equal(relay.issue('one'), null);
  assert.equal((await f.call('/desktops')).status, 200);
  await new Promise(resolve => holder.close(resolve));
  await until(() => relay.running);
});

test('The real entry point opens the configured port', async t => {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-udp-relay-'));
  const reserve = async make => { const socket = make(); await new Promise(resolve => socket.bind?.(0, '127.0.0.1', resolve) ?? socket.listen(0, '127.0.0.1', resolve)); const { port } = socket.address(); await new Promise(resolve => socket.close(resolve)); return port; };
  const udpPort = await reserve(() => createSocket('udp4')), port = await reserve(createServer);
  const env = { ...process.env, NODE_ENV: 'development', MULTITAP_DATA_DIR: directory, MULTITAP_HOST: '127.0.0.1', MULTITAP_PORT: String(port), MULTITAP_PUBLIC_ORIGIN: `http://127.0.0.1:${port}`, MULTITAP_ALLOW_INSECURE_HTTP: '1', MULTITAP_TLS_MODE: 'proxy', MULTITAP_UDP_PORT: String(udpPort), MULTITAP_UDP_HOST: '127.0.0.1' };
  for (const key of ['MULTITAP_SERVER_ACCESS_FILE', 'MULTITAP_TLS_CERT_FILE', 'MULTITAP_TLS_KEY_FILE', 'MULTITAP_UPDATE_SOCKET']) delete env[key];
  const child = spawn(process.execPath, [fileURLToPath(new URL('../src/index.mjs', import.meta.url))], { env, stdio: 'ignore' }), exited = once(child, 'exit');
  t.after(async () => { child.kill(); await exited.catch(() => {}); await rm(directory, { recursive: true, maxRetries: 8, retryDelay: 100 }); });
  // The relay never answers, so the only outside evidence is that the port is held.
  const taken = () => new Promise((resolve, reject) => {
    const socket = createSocket('udp4');
    socket.once('error', error => { socket.close(); error.code === 'EADDRINUSE' ? resolve(true) : reject(error); });
    socket.bind(udpPort, '127.0.0.1', () => socket.close(() => resolve(false)));
  });
  const end = Date.now() + 8000;
  while (!await taken()) { assert.ok(Date.now() < end, 'udp_port_not_opened'); await delay(250); }
});

test('Closing the server stops the worker and releases the port', async t => {
  const f = await broker(t), { port } = f.udp;
  await new Promise(resolve => f.server.close(resolve));
  await until(() => handles().workers === 0);
  const again = createSocket('udp4');
  await new Promise((resolve, reject) => { again.once('error', reject); again.bind(port, '127.0.0.1', resolve); });
  await new Promise(resolve => again.close(resolve));
});
