import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes, randomUUID } from 'node:crypto';
import { once } from 'node:events';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';

// A physical PC's CPU/memory/uptime on its launcher card: the Host's own
// heartbeat report (host-resources-v1, uptime with v2) through the real broker
// to the desktop list and the live push. The "Host" is this test's WebSocket.
const origin = 'https://pc-resources.invalid';
const pcUuid = 'b1234567-89ab-cdef-8123-0123456789ab', pcId = 'pc-' + pcUuid;
const gib = 1024 ** 3;
const caps = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true, monitors: [{ id: '0', width: 1920, height: 1080, primary: true }] };
const report = (extra = {}) => ({ cpuPercent: 37.5, cpuCount: 8, memoryUsedBytes: 6 * gib, memoryTotalBytes: 16 * gib, ...extra });
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const ackTypes = new Set(['host.capabilities.ack', 'host.heartbeat.ack']);

async function fixture(t) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32)), tokens = {};
  for (const name of ['alice', 'carol']) {
    store.addUser({ name, passwordHash: 'fixture', role: 'user' });
    tokens[name] = newToken(); store.createSession(store.findUser(name), tokenHash(tokens[name]), Date.now(), 3600000, 'client');
  }
  store.syncProxmox([{ vmid: 149, node: 'fixture', name: 'VM', status: 'running' }], 'fixture');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve.invalid', tokenId: 'fixture@pve!fixture', secret: 'fixture' }), 'fixture');
  // An approved physical PC exactly as local-pc.mjs records one, assigned to alice only.
  const hostToken = newToken(), hostId = randomUUID(), now = Date.now();
  store.db.prepare("INSERT INTO desktops(id,name,kind,description,os,enabled,created_at) VALUES(?,?,'pc','fixture','Windows',1,?)").run(pcId, 'TOWER-PC', now);
  store.db.prepare('INSERT INTO hosts(id,desktop_id,token_hash,hostname,machine_uuid,version,created_at) VALUES(?,?,?,?,?,?,?)').run(hostId, pcId, tokenHash(hostToken), 'TOWER-PC', pcUuid, '0.2.146', now);
  store.db.prepare('INSERT INTO local_pc_approvals(desktop_id,host_id,machine_uuid,approved_at,actor) VALUES(?,?,?,?,?)').run(pcId, hostId, pcUuid, now, 'fixture');
  store.replaceVmAssignments('alice', [pcId], 'fixture');
  const server = await buildServer({ store, vault, publicOrigin: origin, proxmox: { inventory: async () => [{ vmid: 149, node: 'fixture', name: 'VM', status: 'running', cpuUsage: 0.1, vcpus: 2, memoryUsedBytes: gib, memoryBytes: 2 * gib, uptimeSeconds: 5 }] },
    relayOptions: { authorizationMs: 20, heartbeatMs: 1000 }, desktopEventOptions: { tickMs: 20, authorizationMs: 20, heartbeatMs: 1000 }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const sockets = [];
  t.after(async () => { for (const ws of sockets) ws.terminate(); await new Promise(resolve => server.close(resolve)); store.close(); });
  const base = `http://127.0.0.1:${server.address().port}`;
  const list = async (as = 'alice') => (await (await fetch(base + '/api/v1/desktops', { headers: { Authorization: 'Bearer ' + tokens[as] } })).json()).desktops;
  const pc = async () => (await list()).find(d => d.id === pcId);
  async function host({ capabilities = true } = {}) {
    const ws = new WebSocket(base.replace('http:', 'ws:') + '/api/v1/hosts/channel', { headers: { Authorization: 'Host ' + hostToken, 'X-Multitap-Host-Protocol': 'capabilities-v1' }, perMessageDeflate: false });
    sockets.push(ws); ws.received = [];
    ws.on('message', data => ws.received.push(JSON.parse(data.toString())));
    await once(ws, 'open');
    ws.json = value => ws.send(JSON.stringify(value));
    ws.beat = resources => ws.json({ type: 'host.heartbeat', version: '0.2.146', ...(resources === undefined ? {} : { resources }) });
    if (capabilities) { ws.json({ type: 'host.capabilities', capabilities: caps }); await until(() => ws.received.some(v => v.type === 'host.capabilities.ack'), 'capabilities ack'); }
    return ws;
  }
  async function watch(as = 'alice', live = 'resources,catalog') {
    const ws = new WebSocket(base.replace('http:', 'ws:') + '/api/v1/events', { headers: { Authorization: 'Bearer ' + tokens[as], Origin: origin, ...(live ? { 'X-ToTB-Live': live } : {}) }, perMessageDeflate: false });
    sockets.push(ws); ws.queue = [];
    ws.on('message', bytes => ws.queue.push(JSON.parse(bytes.toString())));
    await once(ws, 'open');
    await until(() => ws.queue.some(m => m.type === 'desktops.ready'), 'ready');
    return ws;
  }
  return { list, pc, host, watch };
}
async function until(predicate, label) {
  for (let i = 0; i < 200; i++) { if (await predicate()) return; await delay(20); }
  assert.fail('timeout: ' + label);
}
// The pushed samples of the PC, oldest first (null: cleared).
const pushed = ws => ws.queue.filter(m => m.type === 'resources').flatMap(m => m.desktops).filter(d => d.id === pcId).map(d => d.resources);

test('a PC Host report reaches the desktop list and the live push in the VM fields (source host, uptime from v2)', async t => {
  const f = await fixture(t), h = await f.host();
  // The broker offers host-resources-v2 to a Host that sends capabilities.
  assert.equal(h.received.find(v => v.type === 'host.capabilities.ack').resourcesProtocol, 'host-resources-v2');
  const alice = await f.watch();
  assert.equal((await f.pc()).resources, null, 'no report yet: nothing to show');
  h.beat(report({ uptimeSeconds: 3600 }));
  await until(() => pushed(alice).length === 1, 'pushed PC sample');
  const sample = pushed(alice)[0];
  assert.equal(typeof sample.checkedAt, 'string');
  const { checkedAt, ...rest } = sample;
  assert.ok(Math.abs(Date.parse(checkedAt) - Date.now()) < 5000);
  assert.deepEqual(rest, { source: 'host', measuredBy: 'host', stale: false, cpuUsagePercent: 37.5, vcpus: 8, memoryUsedBytes: 6 * gib, memoryTotalBytes: 16 * gib, uptimeSeconds: 3600 });
  const listed = await f.pc();
  assert.deepEqual({ ...listed.resources, checkedAt: undefined }, { ...rest, checkedAt: undefined });
  assert.equal(Object.hasOwn(listed, 'powerState'), false, 'still no VM power fields on a PC');
  // A v1 report (a Host before uptime) still fills CPU and memory.
  h.beat(report({ cpuPercent: 12 }));
  await until(() => pushed(alice).length === 2, 'second sample');
  assert.deepEqual([pushed(alice)[1].cpuUsagePercent, pushed(alice)[1].uptimeSeconds], [12, null]);
  // Unchanged values are not pushed again.
  h.beat(report({ cpuPercent: 12 })); await delay(150);
  assert.equal(pushed(alice).length, 2);
  // The Host goes away: the card's sample is cleared.
  h.close();
  await until(() => pushed(alice).length === 3, 'cleared');
  assert.equal(pushed(alice)[2], null);
  assert.equal((await f.pc()).resources, null);
});

test('bad PC reports are dropped; a bad uptime drops only the uptime', async t => {
  const f = await fixture(t), h = await f.host(), alice = await f.watch();
  h.beat(report({ uptimeSeconds: 60 }));
  await until(() => pushed(alice).length === 1, 'baseline');
  for (const bad of [report({ cpuPercent: 101 }), report({ cpuPercent: -0.1 }), report({ cpuPercent: '50' }), report({ cpuCount: 0 }), report({ cpuCount: 2.5 }),
    report({ memoryUsedBytes: 17 * gib }), report({ memoryTotalBytes: 0 }), report({ memoryUsedBytes: -1 }), [], 'resources', null]) h.beat(bad);
  await delay(200);
  assert.equal(pushed(alice).length, 1, 'no invalid report reached the card');
  assert.equal((await f.pc()).resources.cpuUsagePercent, 37.5, 'the last valid report stays');
  for (const [index, uptimeSeconds] of [-1, 1.5, '60', 100 * 366 * 86400 + 1].entries()) {
    h.beat(report({ cpuPercent: 40 + index, uptimeSeconds }));
    await until(() => pushed(alice).length === 2 + index, 'sample without uptime');
    assert.deepEqual([pushed(alice).at(-1).cpuUsagePercent, pushed(alice).at(-1).uptimeSeconds], [40 + index, null]);
  }
});

test('a Host before host-resources-v1 keeps the empty card and is sent nothing new', async t => {
  const f = await fixture(t), h = await f.host(), alice = await f.watch();
  h.beat(); h.beat(); await delay(200);
  assert.equal((await f.pc()).resources, null);
  assert.equal(pushed(alice).length, 0, 'nothing pushed for the PC');
  assert.deepEqual([...new Set(h.received.map(v => v.type))].sort(), [...ackTypes].sort(), 'only the usual acknowledgements');
});

test('without a connected client the broker sends nothing for PC resources; a client that connects gets the current sample at once', async t => {
  const f = await fixture(t), h = await f.host();
  for (let i = 0; i < 5; i++) { h.beat(report({ cpuPercent: 10 + i, uptimeSeconds: 100 + i })); await delay(40); }
  await delay(150);
  assert.ok(h.received.every(v => ackTypes.has(v.type)), 'the Host is never asked for anything');
  const alice = await f.watch();
  await until(() => pushed(alice).length === 1, 'current sample');
  assert.deepEqual([pushed(alice)[0].cpuUsagePercent, pushed(alice)[0].uptimeSeconds], [14, 104]);
  assert.ok(h.received.every(v => ackTypes.has(v.type)), 'a watching client does not make the broker ask the Host either');
  // Another account's watch never carries this PC.
  const carol = await f.watch('carol');
  h.beat(report({ cpuPercent: 77 }));
  await until(() => pushed(alice).length === 2, 'alice update');
  await delay(100);
  assert.equal(pushed(carol).length, 0);
});

test('a released client watch (no live resources) keeps list invalidations only', async t => {
  const f = await fixture(t), h = await f.host(), legacy = await f.watch('alice', null);
  h.beat(report());
  await until(() => legacy.queue.some(m => m.type === 'desktops.changed'), 'list invalidation');
  assert.deepEqual([...new Set(legacy.queue.map(m => m.type))].sort(), ['desktops.changed', 'desktops.ready']);
});
