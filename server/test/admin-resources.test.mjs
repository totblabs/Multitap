import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { AdminResources, ServerResources, adminResourcesProtocol } from '../src/admin-resources.mjs';

// 홈 자원: GET /api/v1/admin/resources (src/admin-resources.mjs). Administrator
// only, read-only, unknown numbers are null (never 0), no secrets.
const origin = 'https://resources.invalid';
const gib = 1024 ** 3;
const uuid = vmid => `12345678-1234-4234-8234-${String(vmid).padStart(12, '0')}`;
const measured = (vmid, node, extra = {}) => ({ vmid, node, name: 'VM' + vmid, status: 'running', cpuUsage: 0.25, vcpus: 4, memoryUsedBytes: 6 * gib, memoryBytes: 8 * gib, uptimeSeconds: 3600, ...extra });

async function fixture(t, { commissioned = true, dataDir = null } = {}) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32)), tokens = {};
  for (const [name, role, purpose] of [['admin', 'admin', 'admin'], ['alice', 'user', 'client']]) {
    store.addUser({ name, passwordHash: 'fixture', role });
    tokens[name] = newToken(); store.createSession(store.findUser(name), tokenHash(tokens[name]), Date.now(), 3600000, purpose);
  }
  const listed = [{ vmid: 710, node: 'pve-a' }, { vmid: 711, node: 'pve-a' }, { vmid: 712, node: 'pve-a' }, { vmid: 720, node: 'pve-b' }];
  store.syncProxmox(listed.map(({ vmid, node }) => ({ vmid, node, name: 'VM' + vmid, status: 'running' })), 'fixture');
  const hostTokens = {};
  for (const { vmid } of listed) {
    const code = newToken(); hostTokens[vmid] = newToken();
    store.issueEnrollment('pve-' + vmid, uuid(vmid), tokenHash(code), 'fixture');
    store.consumeEnrollment(tokenHash(code), uuid(vmid), { hostname: 'fixture-' + vmid, version: '1.0.43', tokenHash: tokenHash(hostTokens[vmid]) });
  }
  store.assign('alice', 'pve-710', 'fixture');
  if (commissioned) store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve-secret-endpoint.invalid', tokenId: 'fixture@pve!resources', secret: 'resources-secret-fixture' }), 'fixture');
  else store.db.prepare("DELETE FROM settings WHERE key='proxmox_initialized'").run();
  // 710 measured; 711 running but Proxmox gave no numbers; 712 stopped; 720 on another node.
  let now = Date.now(), reads = 0, failing = false;
  let answers = { 710: measured(710, 'pve-a'), 711: measured(711, 'pve-a', { cpuUsage: null, vcpus: null, memoryUsedBytes: null, memoryBytes: null, uptimeSeconds: null }), 712: measured(712, 'pve-a', { status: 'stopped', cpuUsage: 0, memoryUsedBytes: 0, uptimeSeconds: 0 }), 720: measured(720, 'pve-b', { cpuUsage: 0.95, vcpus: 8, memoryUsedBytes: Math.round(15.5 * gib), memoryBytes: 16 * gib, uptimeSeconds: 2 * 86400 }) };
  const proxmox = { inventory: async (config, { targets }) => { reads++; if (failing) throw new Error('unreachable'); return targets.map(target => answers[target.vmid]); } };
  const server = await buildServer({ store, vault, proxmox, publicOrigin: origin, dataDir, powerOptions: { now: () => now }, relayOptions: { authorizationMs: 20, heartbeatMs: 1000 }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  const base = `http://127.0.0.1:${server.address().port}`;
  const admin = async (method, path, { cookie = tokens.admin, write = method !== 'GET' } = {}) => {
    const response = await fetch(base + '/api/v1/admin' + path, { method, headers: { ...(cookie ? { Cookie: '__Secure-multitap-admin=' + cookie } : {}), ...(write ? { Origin: origin, 'X-CSRF-Token': vault.csrf(cookie), 'Content-Type': 'application/json' } : {}) }, body: write ? '{}' : undefined });
    return { status: response.status, text: await response.text(), get body() { return JSON.parse(this.text); } };
  };
  return { store, base, tokens, hostTokens, admin, advance: ms => { now += ms; }, fail: value => { failing = value; }, get reads() { return reads; } };
}

test('resources: administrator only, read-only, waits for Proxmox commissioning', async t => {
  const f = await fixture(t);
  assert.equal((await f.admin('GET', '/resources', { cookie: null })).status, 401);
  assert.equal((await fetch(f.base + '/api/v1/admin/resources', { headers: { Authorization: 'Bearer ' + f.tokens.alice } })).status, 401, 'a client bearer token is not an administrator session');
  assert.equal((await f.admin('GET', '/resources', { cookie: f.tokens.alice })).status, 401, 'a client session is not an administrator cookie');
  assert.equal((await fetch(f.base + '/api/v1/resources', { headers: { Authorization: 'Bearer ' + f.tokens.alice } })).status, 404, 'there is no client route');
  assert.equal((await f.admin('POST', '/resources', { write: false })).status, 403, 'a write still needs the same-origin check');
  assert.equal((await f.admin('POST', '/resources')).status, 405);
  assert.equal((await f.admin('GET', '/resources/x')).status, 404);
  const ok = await f.admin('GET', '/resources');
  assert.equal(ok.status, 200);
  for (const secret of [f.tokens.admin, f.tokens.alice, ...Object.values(f.hostTokens), uuid(710), 'pve-secret-endpoint', 'fixture@pve!resources', 'resources-secret-fixture']) assert.equal(ok.text.includes(secret), false, 'no token, machine identity or Proxmox setting: ' + secret);
  const fresh = await fixture(t, { commissioned: false });
  assert.equal((await fresh.admin('GET', '/resources')).body.error.code, 'proxmox_setup_required');
});

test('resources: running desktops with their numbers, unknown as null, node sums, server numbers', async t => {
  const dataDir = await mkdtemp(join(tmpdir(), 'multitap-resources-'));
  t.after(() => rm(dataDir, { recursive: true, force: true }));
  const f = await fixture(t, { dataDir });
  const first = (await f.admin('GET', '/resources')).body;
  assert.equal(first.protocol, adminResourcesProtocol);
  assert.deepEqual(first.proxmox, { configured: true, reachable: true });
  assert.equal(f.reads, 1, 'the read let the shared Proxmox read run once');
  const byId = Object.fromEntries(first.desktops.map(row => [row.id, row]));
  assert.deepEqual(Object.keys(byId).sort(), ['pve-710', 'pve-711', 'pve-720'], 'only running desktops');
  assert.deepEqual({ ...byId['pve-710'], uptimeSeconds: null }, { id: 'pve-710', kind: 'vm', node: 'pve-a', cpuPercent: 25, vcpus: 4, memoryUsedBytes: 6 * gib, memoryTotalBytes: 8 * gib, uptimeSeconds: null, measuredBy: 'proxmox' });
  assert.ok(byId['pve-710'].uptimeSeconds >= 3600 && byId['pve-710'].uptimeSeconds < 3700);
  assert.deepEqual(byId['pve-711'], { id: 'pve-711', kind: 'vm', node: 'pve-a', cpuPercent: null, vcpus: null, memoryUsedBytes: null, memoryTotalBytes: null, uptimeSeconds: null, measuredBy: 'proxmox' }, 'unknown stays null, never 0');
  assert.deepEqual(first.nodes, [
    { node: 'pve-a', running: 2, vcpusUsed: 1, vcpusAssigned: 4, memoryUsedBytes: 6 * gib, memoryTotalBytes: 8 * gib },
    { node: 'pve-b', running: 1, vcpusUsed: 7.6, vcpusAssigned: 8, memoryUsedBytes: Math.round(15.5 * gib), memoryTotalBytes: 16 * gib }]);
  for (const node of first.nodes) assert.deepEqual(Object.keys(node).sort(), ['memoryTotalBytes', 'memoryUsedBytes', 'node', 'running', 'vcpusAssigned', 'vcpusUsed'], 'no node capacity');
  const server = first.server;
  assert.equal(server.cpuPercent, null, 'the first read has no window to measure CPU over');
  assert.ok(Number.isSafeInteger(server.cpuCount) && server.cpuCount >= 1);
  assert.ok(Number.isSafeInteger(server.memoryRssBytes) && server.memoryRssBytes > 0);
  assert.ok(server.memoryLimitBytes === null || Number.isSafeInteger(server.memoryLimitBytes));
  assert.ok(server.disk && server.disk.totalBytes > 0 && server.disk.freeBytes >= 0 && server.disk.freeBytes <= server.disk.totalBytes, 'the data directory\'s file system');
  assert.ok(Number.isSafeInteger(server.uptimeSeconds) && server.uptimeSeconds >= 0);

  // Within the 10 s window the samples are reused, not read again.
  await f.admin('GET', '/resources');
  assert.equal(f.reads, 1);
  await new Promise(resolve => setTimeout(resolve, 1100));
  const second = (await f.admin('GET', '/resources')).body;
  assert.ok(typeof second.server.cpuPercent === 'number' && second.server.cpuPercent >= 0 && second.server.cpuPercent <= 100, 'CPU over the time since the last read');

  // Proxmox stops answering: the running desktops stay listed, their numbers become unknown.
  f.fail(true); f.advance(11000);
  const failed = (await f.admin('GET', '/resources')).body;
  assert.equal(f.reads, 2);
  assert.deepEqual(failed.proxmox, { configured: true, reachable: false });
  assert.deepEqual(failed.desktops.map(row => row.id).sort(), ['pve-710', 'pve-711', 'pve-720']);
  for (const row of failed.desktops) for (const key of ['cpuPercent', 'vcpus', 'memoryUsedBytes', 'memoryTotalBytes', 'uptimeSeconds']) assert.equal(row[key], null, `${row.id} ${key}`);
  assert.deepEqual(failed.nodes.map(node => [node.node, node.vcpusUsed, node.memoryUsedBytes]), [['pve-a', null, null], ['pve-b', null, null]]);
});

test('resources: without a data directory the disk is unknown', async t => {
  const f = await fixture(t);
  assert.equal((await f.admin('GET', '/resources')).body.server.disk, null);
});

// The pieces without a server: the Host's own numbers, a PC, and the server process.
function fakeStore(rows, { configured = true } = {}) {
  return { setting: key => key === 'proxmox' && configured ? 'revision' : null, registeredHostIds: () => new Set(rows.map(row => row.id)), db: { prepare: () => ({ all: () => rows }) } };
}
test('resources: Host-reported CPU and memory win as clients see them; a PC shows its Host report only', async () => {
  const now = Date.parse('2026-11-11T00:00:00Z');
  const rows = [{ id: 'pc-a', kind: 'pc', pve_node: null, pve_vmid: null, pve_power_status: null, pve_power_error: null },
    { id: 'pc-b', kind: 'pc', pve_node: null, pve_vmid: null, pve_power_status: null, pve_power_error: null },
    { id: 'pve-1', kind: 'vm', pve_node: 'n', pve_vmid: 1, pve_power_status: 'running', pve_power_error: null },
    { id: 'pve-2', kind: 'vm', pve_node: 'n', pve_vmid: 2, pve_power_status: 'running', pve_power_error: null }];
  const sample = { source: 'proxmox', checkedAt: new Date(now - 4000).toISOString(), cpuUsagePercent: 50, vcpus: 4, memoryUsedBytes: 8 * gib, memoryTotalBytes: 8 * gib, uptimeSeconds: 100 };
  const power = { snapshotError: null, latestSample: vmid => vmid === 1 ? sample : null, refresh: async () => {} };
  const reports = { 'pve-1': { cpuPercent: 12.25, cpuCount: 4, memoryUsedBytes: 3 * gib, memoryTotalBytes: 8 * gib, at: now }, 'pc-a': { cpuPercent: 5, cpuCount: 12, memoryUsedBytes: 4 * gib, memoryTotalBytes: 16 * gib, uptimeSeconds: 600, at: now } };
  const resources = new AdminResources({ store: fakeStore(rows), power, relay: { hostResources: id => reports[id] ?? null }, now: () => now, server: { sample: async () => ({}) } });
  const value = await resources.read();
  assert.deepEqual(value.desktops, [
    { id: 'pc-a', kind: 'pc', node: null, cpuPercent: 5, vcpus: 12, memoryUsedBytes: 4 * gib, memoryTotalBytes: 16 * gib, uptimeSeconds: 600, measuredBy: 'host' },
    { id: 'pve-1', kind: 'vm', node: 'n', cpuPercent: 12.3, vcpus: 4, memoryUsedBytes: 3 * gib, memoryTotalBytes: 8 * gib, uptimeSeconds: 104, measuredBy: 'host' },
    { id: 'pve-2', kind: 'vm', node: 'n', cpuPercent: null, vcpus: null, memoryUsedBytes: null, memoryTotalBytes: null, uptimeSeconds: null, measuredBy: null }],
  'a PC without a fresh report is not listed; Proxmox\'s uptime moves on with the time since its sample');
  assert.deepEqual(value.nodes, [{ node: 'n', running: 2, vcpusUsed: 0.5, vcpusAssigned: 4, memoryUsedBytes: 3 * gib, memoryTotalBytes: 8 * gib }], 'a PC is not on a Proxmox node');
  // No Proxmox connection: nothing is read and it says so.
  let refreshed = false;
  const bare = new AdminResources({ store: fakeStore([], { configured: false }), power: { refresh: async () => { refreshed = true; } }, relay: null, server: { sample: async () => ({}) } });
  const empty = await bare.read();
  assert.deepEqual([empty.proxmox, empty.desktops, empty.nodes, refreshed], [{ configured: false, reachable: null }, [], [], false]);
});

test('resources: a slow Proxmox is waited for at most waitMs', async () => {
  let release;
  const power = { snapshotError: null, latestSample: () => null, refresh: () => new Promise(resolve => { release = resolve; }) };
  const resources = new AdminResources({ store: fakeStore([]), power, relay: null, waitMs: 30, server: { sample: async () => ({}) } });
  const started = Date.now();
  await resources.read();
  assert.ok(Date.now() - started < 1000);
  release();
});

test('server numbers: CPU share over the window, cgroup limit, disk, and null when unknown', async () => {
  let now = 1000000, used = 0;
  const files = { '/sys/fs/cgroup/memory.max': '2147483648\n' };
  const server = new ServerResources({ dataDir: '/data', now: () => now, cpuUsage: () => ({ user: used, system: 0 }), rss: () => 150 * 1024 * 1024, uptime: () => 90061.7, cores: () => 4,
    statfs: async dir => { assert.equal(dir, '/data'); return { bsize: 4096, blocks: 1000, bavail: 250 }; }, readFile: async file => { if (!Object.hasOwn(files, file)) throw new Error('ENOENT'); return files[file]; }, platform: 'linux', totalMemory: () => 16 * gib });
  const first = await server.sample();
  // The 1.0 fields keep their meaning (the machine and the service are new fields beside them: admin-resources-system.test.mjs).
  const old = ({ cpuPercent, cpuCount, memoryRssBytes, memoryLimitBytes, disk, uptimeSeconds }) => ({ cpuPercent, cpuCount, memoryRssBytes, memoryLimitBytes, disk, uptimeSeconds });
  assert.deepEqual(old(first), { cpuPercent: null, cpuCount: 4, memoryRssBytes: 150 * 1024 * 1024, memoryLimitBytes: 2 * gib, disk: { freeBytes: 250 * 4096, totalBytes: 1000 * 4096, usedBytes: 750 * 4096 }, uptimeSeconds: 90061 });
  now += 2000; used += 2000000; // 2 s of CPU time in 2 s on 4 cores
  assert.equal((await server.sample()).cpuPercent, 25);
  now += 300; used += 999999999;
  assert.equal((await server.sample()).cpuPercent, 25, 'reads closer than one second keep the last value');
  now += 200000;
  assert.equal((await server.sample()).cpuPercent, null, 'after a long gap the next window starts');

  // "max", v1's huge number, an unreadable file and another platform: no limit.
  for (const [layout, platform, expected] of [[{ '/sys/fs/cgroup/memory.max': 'max\n' }, 'linux', null], [{ '/sys/fs/cgroup/memory/memory.limit_in_bytes': '9223372036854771712' }, 'linux', null],
    [{ '/sys/fs/cgroup/memory/memory.limit_in_bytes': String(gib) }, 'linux', gib], [{}, 'linux', null], [{ '/sys/fs/cgroup/memory.max': String(gib) }, 'win32', null]]) {
    const other = new ServerResources({ dataDir: null, readFile: async file => { if (!Object.hasOwn(layout, file)) throw new Error('ENOENT'); return layout[file]; }, platform, totalMemory: () => 16 * gib });
    const sample = await other.sample();
    assert.equal(sample.memoryLimitBytes, expected, JSON.stringify(layout) + platform);
    assert.equal(sample.disk, null);
  }
  const broken = new ServerResources({ dataDir: '/gone', statfs: async () => { throw new Error('ENOENT'); }, platform: 'win32' });
  assert.equal((await broken.sample()).disk, null);
});
