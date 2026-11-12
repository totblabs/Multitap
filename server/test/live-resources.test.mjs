import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { mkdtemp, rm, writeFile, rename } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { CatalogWatch } from '../src/catalog-watch.mjs';
import { newToken, tokenHash } from '../src/security.mjs';

const origin = 'https://live-fixture.example';
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const gib = 1024 ** 3;
const vm = (vmid, cpu = 0.1, uptime = 100, status = 'running') => ({ vmid, node: 'n', name: `VM${vmid}`, status, cpuUsage: cpu, vcpus: 4, memoryUsedBytes: 2 * gib, memoryBytes: 8 * gib, uptimeSeconds: uptime });

async function connect(url, token, live) {
  const headers = { Authorization: 'Bearer ' + token, Origin: origin, ...(live ? { 'X-ToTB-Live': live } : {}) };
  const ws = new WebSocket(url, { headers, perMessageDeflate: false });
  ws.queue = [];
  ws.on('message', bytes => ws.queue.push(JSON.parse(bytes.toString())));
  await new Promise((resolve, reject) => { ws.once('open', resolve); ws.once('error', reject); });
  return ws;
}
async function until(predicate, label) {
  for (let i = 0; i < 150; i++) { if (predicate()) return; await delay(20); }
  assert.fail('timeout: ' + label);
}

async function fixture(t, { downloadDir, catalogWatchOptions } = {}) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32)), tokens = {}, sockets = [];
  for (const name of ['alice', 'bobby']) {
    store.addUser({ name, passwordHash: 'isolated-live-password-fixture', role: 'user' });
    tokens[name] = newToken(); store.createSession(store.findUser(name), tokenHash(tokens[name]), Date.now(), 3600000, 'client');
  }
  store.syncProxmox([{ vmid: 710, node: 'n', name: 'VM710', status: 'running' }, { vmid: 711, node: 'n', name: 'VM711', status: 'running' }], 'fixture');
  for (const vmid of [710,711]) {
    const code=newToken(), uuid=`12345678-1234-4234-8234-${String(vmid).padStart(12,'0')}`;
    store.issueEnrollment(`pve-${vmid}`,uuid,tokenHash(code),'fixture');
    store.consumeEnrollment(tokenHash(code),uuid,{hostname:'fixture-host',version:'1.0.14',tokenHash:tokenHash(newToken())});
  }
  store.assign('alice', 'pve-710', 'fixture'); store.assign('bobby', 'pve-711', 'fixture');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://isolated-pve.example', tokenId: 'fixture@pve!live', secret: 'isolated-live-token-fixture' }), 'fixture');
  let now = Date.now(), inventory = [vm(710), vm(711)], reads = 0;
  const proxmox = { inventory: async () => { reads++; return inventory; } };
  const server = await buildServer({ store, vault, proxmox, publicOrigin: origin, downloadDir, powerOptions: { now: () => now }, vmStateOptions: { syncMs: 45000, resourceMs: 5000 }, relayOptions: { heartbeatMs: 1000, authorizationMs: 20 },
    desktopEventOptions: { tickMs: 20, authorizationMs: 20, heartbeatMs: 1000 }, catalogWatchOptions, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { for (const ws of sockets) ws.terminate(); await new Promise(resolve => server.close(resolve)); store.close(); });
  const url = `ws://127.0.0.1:${server.address().port}/api/v1/events`;
  return { store, server, tokens, url, advance: ms => { now += ms; }, setInventory: value => { inventory = value; }, get reads() { return reads; },
    async open(name, live) { const ws = await connect(url, tokens[name], live); sockets.push(ws); return ws; } };
}
const resourcesOf = ws => ws.queue.filter(m => m.type === 'resources');
const only = (ws, type) => ws.queue.filter(m => m.type === type);

test('resources: sampled every resourceMs while a client is connected, only changed values pushed, per-user scope', async t => {
  const f = await fixture(t);
  const alice = await f.open('alice', 'resources,catalog'), bobby = await f.open('bobby', 'resources');
  await until(() => only(alice, 'desktops.ready').length === 1 && only(bobby, 'desktops.ready').length === 1, 'ready');
  // The first push after ready carries the current values of the user's own VMs.
  await until(() => resourcesOf(alice).length === 1 && resourcesOf(bobby).length === 1, 'initial resources');
  assert.deepEqual(resourcesOf(alice)[0].desktops.map(d => d.id), ['pve-710']);
  assert.deepEqual(resourcesOf(bobby)[0].desktops.map(d => d.id), ['pve-711']);
  const first = resourcesOf(alice)[0];
  assert.equal(first.desktops[0].resources.cpuUsagePercent, 10);
  assert.equal(first.desktops[0].resources.source, 'proxmox');
  assert.equal(first.revision, 1);
  // Shorter than the sample period: no new Proxmox read.
  const baseline = f.reads;
  f.setInventory([vm(710, 0.5, 103), vm(711, 0.1, 100)]); f.advance(3000); await delay(150);
  assert.equal(f.reads, baseline, 'no read before resourceMs');
  f.advance(2500);
  await until(() => resourcesOf(alice).length === 2, 'changed push');
  assert.equal(f.reads, baseline + 1, 'one registered-target observation batch serves every user');
  assert.equal(resourcesOf(alice)[1].desktops[0].resources.cpuUsagePercent, 50);
  assert.equal(resourcesOf(alice)[1].revision, 2);
  // Bobby's VM did not change except... its uptime is identical -> nothing for bobby.
  await delay(100);
  assert.equal(resourcesOf(bobby).length, 1, 'unchanged values are not pushed');
  // Pushed values replace list re-reads: a new-style watcher gets no desktops.changed.
  assert.equal(only(alice, 'desktops.changed').length, 0);
  // A VM that left the user's scope is cleared.
  f.store.revoke('alice', 'pve-710', 'fixture');
  await until(() => resourcesOf(alice).some(m => m.desktops.some(d => d.id === 'pve-710' && d.resources === null)), 'cleared');
});

test('resources: legacy watchers keep list invalidations and never receive new message types', async t => {
  const f = await fixture(t);
  const legacy = await f.open('alice');
  await until(() => only(legacy, 'desktops.ready').length === 1, 'ready');
  f.setInventory([vm(710, 0.7, 120), vm(711)]); f.advance(5100);
  await until(() => only(legacy, 'desktops.changed').length === 1, 'legacy changed');
  assert.deepEqual([...new Set(legacy.queue.map(m => m.type))].sort(), ['desktops.changed', 'desktops.ready']);
});

test('resources: sampling stops when no client is connected and resumes with one', async t => {
  const f = await fixture(t);
  const alice = await f.open('alice', 'resources');
  await until(() => resourcesOf(alice).length === 1, 'initial');
  alice.close(); await until(() => f.server.listening && alice.readyState === WebSocket.CLOSED, 'closed'); await delay(60);
  const reads = f.reads; f.advance(60000); await delay(150);
  assert.equal(f.reads, reads, 'nobody connected, nothing sampled');
  const again = await f.open('alice', 'resources');
  await until(() => only(again, 'desktops.ready').length === 1, 'ready again');
  f.advance(6000); f.setInventory([vm(710, 0.3, 200), vm(711)]);
  await until(() => resourcesOf(again).some(m => m.desktops[0].resources.cpuUsagePercent === 30), 'resumed');
});

test('catalog: replacing releases.json (atomic rename) pushes once to clients that asked; touching it does not', async t => {
  const dir = await mkdtemp(join(tmpdir(), 'multitap-catalog-live-')); t.after(() => rm(dir, { recursive: true, force: true }));
  await writeFile(join(dir, 'releases.json'), '[]');
  const f = await fixture(t, { downloadDir: dir, catalogWatchOptions: { intervalMs: 30 } });
  const modern = await f.open('alice', 'resources,catalog'), legacy = await f.open('bobby');
  await until(() => only(modern, 'desktops.ready').length === 1 && only(legacy, 'desktops.ready').length === 1, 'ready');
  await delay(150);
  assert.equal(only(modern, 'catalog').length, 0, 'baseline is not a change');
  await writeFile(join(dir, 'releases.tmp'), '[{"changed":true}]'); await rename(join(dir, 'releases.tmp'), join(dir, 'releases.json'));
  await until(() => only(modern, 'catalog').length === 1, 'catalog push');
  assert.equal(only(modern, 'catalog')[0].revision, 1);
  await writeFile(join(dir, 'releases.json'), '[{"changed":true}]'); // same content, new mtime
  await delay(200);
  assert.equal(only(modern, 'catalog').length, 1, 'same content is not a change');
  await writeFile(join(dir, 'releases.json'), '[{"changed":2}]');
  await until(() => only(modern, 'catalog').length === 2, 'second change');
  assert.equal(only(legacy, 'catalog').length, 0, 'legacy watcher gets no new type');
});

test('catalog watch: baseline, content change, deletion', async t => {
  const dir = await mkdtemp(join(tmpdir(), 'multitap-catalog-watch-')); t.after(() => rm(dir, { recursive: true, force: true }));
  let changes = 0;
  const watch = new CatalogWatch({ directory: dir, onChange: () => { changes++; }, intervalMs: 3600000 }); t.after(() => watch.close());
  await watch.check(); assert.equal(changes, 0, 'missing file at start is the baseline');
  await writeFile(join(dir, 'releases.json'), '[1]'); assert.equal(await watch.check(), true);
  assert.equal(await watch.check(), false);
  await writeFile(join(dir, 'releases.json'), '[1]'); assert.equal(await watch.check(), false);
  await rm(join(dir, 'releases.json')); assert.equal(await watch.check(), true);
  assert.equal(changes, 2);
});

test('host: a different version reported over the Host channel pushes a list change at once', async t => {
  const f = await fixture(t);
  // The fixture's own machine enrolls again; another UUID would be another machine and drop the assignment.
  const hostToken = newToken(), code = newToken(), uuid = '12345678-1234-4234-8234-000000000710';
  f.store.issueEnrollment('pve-710', uuid, tokenHash(code), 'fixture');
  f.store.consumeEnrollment(tokenHash(code), uuid, { hostname: 'Live VM', version: '0.2.10', tokenHash: tokenHash(hostToken) });
  const host = new WebSocket(f.url.replace('/events', '/hosts/channel'), { headers: { Authorization: 'Host ' + hostToken, 'X-Multitap-Host-Protocol': 'capabilities-v1' }, perMessageDeflate: false });
  t.after(() => host.terminate());
  await new Promise((resolve, reject) => { host.once('open', resolve); host.once('error', reject); });
  const alice = await f.open('alice', 'resources,catalog');
  await until(() => only(alice, 'desktops.ready').length === 1, 'ready');
  await delay(100);
  const before = only(alice, 'desktops.changed').length;
  host.send(JSON.stringify({ type: 'host.heartbeat', version: '0.2.11' }));
  await until(() => only(alice, 'desktops.changed').length > before, 'version push');
});

test('host resources: a fresh Host report replaces CPU/memory (source stays proxmox, measuredBy host); bad reports and old Hosts keep Proxmox values', async t => {
  const f = await fixture(t);
  const hostToken = newToken(), code = newToken(), uuid = '12345678-1234-4234-8234-000000000710';
  f.store.issueEnrollment('pve-710', uuid, tokenHash(code), 'fixture');
  f.store.consumeEnrollment(tokenHash(code), uuid, { hostname: 'Live VM', version: '0.2.10', tokenHash: tokenHash(hostToken) });
  const host = new WebSocket(f.url.replace('/events', '/hosts/channel'), { headers: { Authorization: 'Host ' + hostToken, 'X-Multitap-Host-Protocol': 'capabilities-v1' }, perMessageDeflate: false });
  t.after(() => host.terminate());
  await new Promise((resolve, reject) => { host.once('open', resolve); host.once('error', reject); });
  const alice = await f.open('alice', 'resources');
  await until(() => resourcesOf(alice).length === 1, 'initial');
  const initial = resourcesOf(alice)[0].desktops[0].resources;
  assert.equal(initial.measuredBy, undefined); assert.equal(initial.memoryUsedBytes, 2 * gib);
  // Invalid (used > total) is ignored; an old-style heartbeat changes nothing.
  host.send(JSON.stringify({ type: 'host.heartbeat', version: '0.2.10', resources: { cpuPercent: 5, cpuCount: 2, memoryUsedBytes: 9 * gib, memoryTotalBytes: 8 * gib } }));
  host.send(JSON.stringify({ type: 'host.heartbeat', version: '0.2.10' }));
  await delay(200);
  assert.equal(resourcesOf(alice).length, 1);
  host.send(JSON.stringify({ type: 'host.heartbeat', version: '0.2.10', resources: { cpuPercent: 37.5, cpuCount: 6, memoryUsedBytes: 3 * gib, memoryTotalBytes: 6 * gib } }));
  await until(() => resourcesOf(alice).length === 2, 'host push');
  const pushed = resourcesOf(alice)[1].desktops[0].resources;
  assert.deepEqual([pushed.source, pushed.measuredBy, pushed.cpuUsagePercent, pushed.vcpus, pushed.memoryUsedBytes, pushed.memoryTotalBytes], ['proxmox', 'host', 37.5, 6, 3 * gib, 6 * gib]);
  assert.equal(pushed.uptimeSeconds, 100, 'uptime stays Proxmox\'s');
  // The Host goes away: back to Proxmox's numbers.
  host.close();
  await until(() => resourcesOf(alice).length === 3, 'fallback push');
  assert.equal(resourcesOf(alice)[2].desktops[0].resources.measuredBy, undefined);
});
