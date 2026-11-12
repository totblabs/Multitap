import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { mkdtemp, mkdir, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { ServerResources, CpuWindow, machineCpu, cgroupCpu, dataSize, cgroupDir, parseProcStat, parseMeminfo, parseUptime, parseNetDev, parseSelfCgroup, parseCpuStat, parseCpuMax, parseMemoryMax, parseInactiveFile, parseIoStat } from '../src/admin-resources.mjs';

// 홈 자원, the server part (src/admin-resources.mjs): the machine from /proc and
// the Multitap container or service from its cgroup v2 files. Every file here
// is fixture text; nothing reads this PC's /proc or any real server.
const gib = 1024 ** 3, mib = 1024 ** 2;
const procStat = (user, system, idle, iowait) => `cpu  ${user} 0 ${system} ${idle} ${iowait} 0 0 0 0 0\n` + [0, 1, 2, 3].map(n => `cpu${n} 1 0 1 1 1 0 0 0 0 0`).join('\n') + '\nintr 12345 0 0\nctxt 999\nbtime 1700000000\n';
const meminfo = 'MemTotal:        8388608 kB\nMemFree:          524288 kB\nMemAvailable:    6291456 kB\nBuffers:          102400 kB\n';
const netDev = `Inter-|   Receive                                                |  Transmit
 face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed
    lo:  999999      10    0    0    0     0          0         0   999999      10    0    0    0     0       0          0
  eth0: 5000000000  100    0    0    0     0          0         0 1000000000    50    0    0    0     0       0          0
  eth1:    1000       1    0    0    0     0          0         0     2000       1    0    0    0     0       0          0
`;
const ioStat = '8:0 rbytes=1048576 wbytes=2097152 rios=10 wios=20 dbytes=0 dios=0\n253:0 rbytes=1048576 wbytes=0 rios=1 wios=0 dbytes=0 dios=0\n';

// A fake file system: the path's text, a missing file throws ENOENT. Counts reads per path.
function files(layout) {
  const reads = new Map();
  const read = async path => { reads.set(path, (reads.get(path) ?? 0) + 1); if (!Object.hasOwn(layout, path)) throw Object.assign(new Error('ENOENT'), { code: 'ENOENT' }); const value = layout[path]; return typeof value === 'function' ? value() : value; };
  return { layout, reads, read };
}
const dockerLayout = () => ({
  '/proc/self/cgroup': '0::/\n', '/proc/stat': procStat(10000, 5000, 80000, 5000), '/proc/meminfo': meminfo, '/proc/uptime': '1036800.55 3000000.00\n', '/proc/net/dev': netDev,
  '/sys/fs/cgroup/cpu.stat': 'usage_usec 50000000\nuser_usec 40000000\nsystem_usec 10000000\nnr_periods 0\n', '/sys/fs/cgroup/cpu.max': '200000 100000\n',
  '/sys/fs/cgroup/memory.current': '536870912\n', '/sys/fs/cgroup/memory.stat': 'anon 300000000\nfile 200000000\ninactive_file 134217728\nactive_file 1000\n', '/sys/fs/cgroup/memory.max': '1073741824\n',
  '/sys/fs/cgroup/io.stat': ioStat });
const unit = '/sys/fs/cgroup/system.slice/multitap-broker.service';
const systemdLayout = () => ({
  '/proc/self/cgroup': '0::/system.slice/multitap-broker.service\n', '/proc/stat': procStat(10000, 5000, 80000, 5000), '/proc/meminfo': meminfo, '/proc/uptime': '3600.2 7000.1\n', '/proc/net/dev': netDev,
  [unit + '/cpu.stat']: 'usage_usec 1000000\n', [unit + '/cpu.max']: 'max 100000\n', [unit + '/memory.current']: '209715200\n', [unit + '/memory.stat']: 'inactive_file 0\n', [unit + '/memory.max']: 'max\n' });
function resources(fs, extra = {}) {
  let now = 5000000;
  const server = new ServerResources({ dataDir: '/data', now: () => now, cpuUsage: () => ({ user: 0, system: 0 }), rss: () => 150 * mib, uptime: () => 7260.4, cores: () => 4,
    statfs: async () => ({ bsize: 4096, blocks: 1000, bavail: 250, bfree: 300 }), readFile: fs.read, platform: 'linux', totalMemory: () => 8 * gib,
    hostname: () => 'vdi-broker', release: () => '6.8.12-4-pve', machineCores: () => 4, sizeOf: async () => 1536 * mib, ...extra });
  return { server, advance: ms => { now += ms; } };
}

test('/proc parsers: stat, meminfo, uptime, net/dev; garbage and missing fields are null', () => {
  assert.deepEqual(parseProcStat(procStat(10000, 5000, 80000, 5000)), { total: 100000, idle: 85000, cpus: 4 });
  assert.deepEqual(parseProcStat('cpu  1 2 3 4\n'), { total: 10, idle: 4, cpus: null }, 'an old kernel without iowait');
  for (const bad of ['', 'garbage', 'cpu  a b c d\n', 'cpu0 1 2 3 4\n', 'cpu  0 0 0 0\n', null]) assert.equal(parseProcStat(bad), null, String(bad));
  assert.deepEqual(parseMeminfo(meminfo), { totalBytes: 8 * gib, usedBytes: 2 * gib });
  assert.deepEqual(parseMeminfo('MemTotal: 1024 kB\n'), { totalBytes: 1024 * 1024, usedBytes: null }, 'no MemAvailable: used unknown');
  assert.deepEqual(parseMeminfo('MemTotal: 1024 kB\nMemAvailable: 4096 kB\n'), { totalBytes: 1024 * 1024, usedBytes: null }, 'more available than total is not believed');
  for (const bad of ['', 'MemTotal: lots\n', 'MemTotal: 0 kB\n', undefined]) assert.equal(parseMeminfo(bad), null);
  assert.equal(parseUptime('1036800.55 3000000.00\n'), 1036800);
  assert.equal(parseUptime('42 1'), 42);
  for (const bad of ['', 'x y', '-5 1', '99999999999999 1']) assert.equal(parseUptime(bad), null, bad);
  assert.deepEqual(parseNetDev(netDev), { receivedBytes: 5000001000, sentBytes: 1000002000 }, 'lo is left out');
  assert.equal(parseNetDev(netDev.split('\n').slice(0, 3).join('\n')), null, 'only lo: unknown');
  assert.equal(parseNetDev('eth0: 1 2 3\n'), null, 'too few fields');
  assert.equal(parseNetDev('garbage'), null);
});

test('cgroup parsers: v2 with and without limits, v1 layouts, unsafe paths and garbage', () => {
  assert.equal(parseSelfCgroup('0::/\n'), '/');
  assert.equal(parseSelfCgroup('0::/system.slice/multitap-broker.service\n'), '/system.slice/multitap-broker.service');
  assert.equal(parseSelfCgroup('12:memory:/docker/abc\n11:cpu,cpuacct:/docker/abc\n0::/docker/abc\n'), '/docker/abc', 'hybrid: the v2 line');
  assert.equal(parseSelfCgroup('12:memory:/docker/abc\n11:cpu,cpuacct:/docker/abc\n'), null, 'v1 only: no v2 folder');
  for (const bad of ['0::/../../etc\n', '0::/a/./b\n', '0::relative\n', '', 'garbage']) assert.equal(parseSelfCgroup(bad), null, bad);
  assert.equal(cgroupDir('/'), '/sys/fs/cgroup');
  assert.equal(cgroupDir('/system.slice/multitap-broker.service'), unit);
  for (const bad of [null, '', 'system.slice', 42]) assert.equal(cgroupDir(bad), null, String(bad));
  assert.equal(parseCpuStat('usage_usec 50000000\nuser_usec 1\n'), 50000000);
  for (const bad of ['', 'usage_usec x\n', 'user_usec 5\n']) assert.equal(parseCpuStat(bad), null, bad);
  assert.deepEqual(parseCpuMax('max 100000\n'), { limited: false, cores: null });
  assert.deepEqual(parseCpuMax('200000 100000\n'), { limited: true, cores: 2 });
  assert.deepEqual(parseCpuMax('150000 100000'), { limited: true, cores: 1.5 });
  for (const bad of ['', 'max', '0 100000', '100000 0', 'lots 100000', '1 2 3']) assert.equal(parseCpuMax(bad), null, bad);
  assert.deepEqual(parseMemoryMax('max\n'), { limited: false, bytes: null });
  assert.deepEqual(parseMemoryMax('1073741824\n', 8 * gib), { limited: true, bytes: gib });
  assert.deepEqual(parseMemoryMax('9223372036854771712', 8 * gib), { limited: false, bytes: null }, 'at or above the machine: no limit');
  for (const bad of ['', '0', 'lots', '-1']) assert.equal(parseMemoryMax(bad, 8 * gib), null, bad);
  assert.equal(parseInactiveFile('anon 1\ninactive_file 4096\n'), 4096);
  assert.equal(parseInactiveFile('anon 1\n'), null);
  assert.deepEqual(parseIoStat(ioStat), { readBytes: 2 * mib, writtenBytes: 2 * mib }, 'summed over the devices');
  assert.deepEqual(parseIoStat(''), { readBytes: 0, writtenBytes: 0 }, 'no block I/O yet');
  for (const bad of ['garbage', '8:0 rios=1\n', '8:0 rbytes=1\n']) assert.equal(parseIoStat(bad), null, bad);
});

test('CPU windows: first null, closer than 1 s keeps the value, over 2 min starts again, counters going back are null', () => {
  const window = new CpuWindow();
  let reads = 0; const read = value => () => { reads++; return value; };
  assert.equal(window.next(1000, read(parseProcStat(procStat(10000, 5000, 80000, 5000))), machineCpu), null, 'the first reading has no window');
  assert.equal(window.next(2500, read(parseProcStat(procStat(13000, 6000, 84000, 7000))), machineCpu), 40, '4000 of 10000 jiffies busy');
  assert.equal(window.next(2900, read(null), machineCpu), 40, 'a reading closer than 1 s is not taken');
  assert.equal(reads, 2, 'and the file is not read');
  assert.equal(window.next(200000, read(parseProcStat(procStat(20000, 9000, 90000, 9000))), machineCpu), null, 'after a long gap');
  assert.equal(window.next(201500, read(parseProcStat(procStat(1, 1, 1, 1))), machineCpu), null, 'a counter that went back (a reboot)');
  assert.equal(window.next(203000, read(null), machineCpu), null, 'an unreadable file');
  const cgroup = new CpuWindow(), twoCores = cgroupCpu(2);
  assert.equal(cgroup.next(0, () => 50000000, twoCores), null);
  assert.equal(cgroup.next(2000, () => 51000000, twoCores), 25, '1 s of CPU in 2 s over a 2-core limit');
  assert.equal(cgroup.next(4000, () => 59000000, twoCores), 100, 'capped at 100');
  assert.equal(cgroup.next(6000, () => 1, twoCores), null, 'going back');
  assert.equal(cgroupCpu(8)(0, 8000000, 1000), 100);
  assert.equal(cgroupCpu(0)(0, 1, 1000), null, 'no cores known');
});

test('data size: the files under the folder, null past the file count or the time limit or when unreadable', async t => {
  const dir = await mkdtemp(join(tmpdir(), 'multitap-size-'));
  t.after(() => rm(dir, { recursive: true, force: true }));
  await mkdir(join(dir, 'downloads', 'old'), { recursive: true });
  await writeFile(join(dir, 'multitap.db'), Buffer.alloc(3000));
  await writeFile(join(dir, 'downloads', 'a.exe'), Buffer.alloc(1200));
  await writeFile(join(dir, 'downloads', 'old', 'b.exe'), Buffer.alloc(800));
  assert.equal(await dataSize(dir), 5000);
  assert.equal(await dataSize(dir, { maxFiles: 5 }), 5000, 'five entries: three files and two folders');
  assert.equal(await dataSize(dir, { maxFiles: 4 }), null, 'the file count limit');
  let clock = 0;
  assert.equal(await dataSize(dir, { now: () => (clock += 400), maxMs: 1000 }), null, 'the time limit');
  assert.equal(await dataSize(join(dir, 'missing')), null);
  assert.equal(await dataSize(null), null);
  const denied = await dataSize(dir, { open: async path => { if (path !== dir) throw Object.assign(new Error('EACCES'), { code: 'EACCES' }); const { opendir } = await import('node:fs/promises'); return opendir(path); } });
  assert.equal(denied, null, 'a folder it cannot read: the size is not known');
  const vanished = await dataSize(dir, { stat: async path => { if (path.endsWith('a.exe')) throw Object.assign(new Error('ENOENT'), { code: 'ENOENT' }); const { lstat } = await import('node:fs/promises'); return lstat(path); } });
  assert.equal(vanished, 3800, 'a file removed meanwhile is gone, not unknown');
});

test('Docker: the machine from /proc, the container from its cgroup (limits set); its name and network are the container\'s', async () => {
  const fs = files(dockerLayout());
  const { server, advance } = resources(fs, { deploymentKind: 'docker' });
  const first = await server.sample();
  assert.equal(first.deploymentKind, 'docker');
  assert.deepEqual(first.machine, { name: null, kernel: '6.8.12-4-pve', uptimeSeconds: 1036800, cpuPercent: null, cpuCount: 4, memoryUsedBytes: 2 * gib, memoryTotalBytes: 8 * gib,
    disk: { freeBytes: 250 * 4096, totalBytes: 1000 * 4096, usedBytes: 700 * 4096 }, network: null });
  assert.deepEqual(first.service, { kind: 'container', state: 'running', measuredBy: 'cgroup', name: 'vdi-broker', cpuPercent: null, cpuBasis: 'quota', cpuLimitCores: 2,
    memoryUsedBytes: 384 * mib, memoryLimitBytes: gib, memoryUnlimited: false, network: { receivedBytes: 5000001000, sentBytes: 1000002000 }, blockIo: { readBytes: 2 * mib, writtenBytes: 2 * mib }, dataBytes: 1536 * mib, uptimeSeconds: 7260 });
  // Two seconds on: the machine 40 % busy, the container 1 s of CPU over its 2-core limit.
  fs.layout['/proc/stat'] = procStat(13000, 6000, 84000, 7000);
  fs.layout['/sys/fs/cgroup/cpu.stat'] = 'usage_usec 51000000\n';
  advance(2000);
  const second = await server.sample();
  assert.equal(second.machine.cpuPercent, 40);
  assert.equal(second.service.cpuPercent, 25);
  // The 1.0 fields beside them are unchanged.
  assert.deepEqual([second.cpuCount, second.memoryRssBytes, second.memoryLimitBytes, second.uptimeSeconds], [4, 150 * mib, gib, 7260], 'the old limit still reads /sys/fs/cgroup/memory.max');
});

test('Docker without limits: all host cores and "no limit"', async () => {
  const layout = dockerLayout();
  layout['/sys/fs/cgroup/cpu.max'] = 'max 100000\n'; layout['/sys/fs/cgroup/memory.max'] = 'max\n';
  const fs = files(layout), { server, advance } = resources(fs, { deploymentKind: 'docker' });
  await server.sample();
  fs.layout['/sys/fs/cgroup/cpu.stat'] = 'usage_usec 52000000\n';
  advance(2000);
  const { service } = await server.sample();
  assert.deepEqual([service.cpuBasis, service.cpuLimitCores, service.cpuPercent, service.memoryLimitBytes, service.memoryUnlimited], ['cores', null, 25, null, true], '2 s of CPU in 2 s over 4 cores');
});

// Docker inside a Proxmox CT. The container's /proc is the
// node's (32 cores, 186 GB, 11 days up) while the CT has 2 cores: the console
// would show a false memory warning and measure the container against 32 cores.
test('Docker in a CT: /proc lists more cores than the server may use, so the machine CPU, memory and uptime are unknown and the container is measured against its own cores', async () => {
  const layout = dockerLayout();
  layout['/sys/fs/cgroup/cpu.max'] = 'max 100000\n';
  const fs = files(layout), { server, advance } = resources(fs, { deploymentKind: 'docker', cores: () => 2 });
  const first = await server.sample();
  assert.deepEqual([first.machine.uptimeSeconds, first.machine.cpuPercent, first.machine.cpuCount, first.machine.memoryUsedBytes, first.machine.memoryTotalBytes], [null, null, 2, null, null]);
  assert.equal(first.machine.kernel, '6.8.12-4-pve', 'the kernel and the disk are still read');
  assert.ok(first.machine.disk);
  fs.layout['/proc/stat'] = procStat(13000, 6000, 84000, 7000);
  fs.layout['/sys/fs/cgroup/cpu.stat'] = 'usage_usec 52000000\n';
  advance(2000);
  const second = await server.sample();
  assert.equal(second.machine.cpuPercent, null);
  assert.deepEqual([second.service.cpuBasis, second.service.cpuPercent], ['cores', 50], '2 s of CPU in 2 s over 2 cores');
});

test('systemd: "Multitap 서비스" from the unit\'s cgroup; the machine has the name and the network', async () => {
  const fs = files(systemdLayout());
  const { server } = resources(fs, { deploymentKind: 'systemd' });
  const value = await server.sample();
  assert.equal(fs.reads.get(unit + '/cpu.stat'), 1, 'the unit\'s folder from /proc/self/cgroup');
  assert.deepEqual(value.machine, { name: 'vdi-broker', kernel: '6.8.12-4-pve', uptimeSeconds: 3600, cpuPercent: null, cpuCount: 4, memoryUsedBytes: 2 * gib, memoryTotalBytes: 8 * gib,
    disk: { freeBytes: 250 * 4096, totalBytes: 1000 * 4096, usedBytes: 700 * 4096 }, network: { receivedBytes: 5000001000, sentBytes: 1000002000 } });
  assert.deepEqual(value.service, { kind: 'service', state: 'running', measuredBy: 'cgroup', name: null, cpuPercent: null, cpuBasis: 'cores', cpuLimitCores: null,
    memoryUsedBytes: 200 * mib, memoryLimitBytes: null, memoryUnlimited: true, network: null, blockIo: null, dataBytes: 1536 * mib, uptimeSeconds: 7260 }, 'no io.stat (IOAccounting off): block I/O unknown');
});

test('no cgroup v2 (v1 only, or unreadable): the service is the server process; missing /proc files are null one by one', async () => {
  const fs = files({ '/proc/self/cgroup': '4:memory:/system.slice/multitap-broker.service\n', '/proc/meminfo': 'MemTotal: lots\n', '/proc/uptime': '120.5 3\n' });
  const { server, advance } = resources(fs, { deploymentKind: 'systemd', cpuUsage: (() => { let used = 0; return () => ({ user: (used += 2000000), system: 0 }); })() });
  const first = await server.sample();
  assert.deepEqual(first.machine, { name: 'vdi-broker', kernel: '6.8.12-4-pve', uptimeSeconds: 120, cpuPercent: null, cpuCount: 4, memoryUsedBytes: null, memoryTotalBytes: null,
    disk: { freeBytes: 250 * 4096, totalBytes: 1000 * 4096, usedBytes: 700 * 4096 }, network: null });
  assert.deepEqual([first.service.measuredBy, first.service.cpuBasis, first.service.memoryUsedBytes, first.service.memoryUnlimited, first.service.blockIo], ['process', 'process', 150 * mib, null, null]);
  advance(2000);
  assert.equal((await server.sample()).service.cpuPercent, 25, 'the process CPU: 2 s in 2 s on 4 cores');
});

test('another platform: nothing is read, the machine\'s numbers are null, the service is the process', async () => {
  const fs = files(dockerLayout());
  const { server } = resources(fs, { platform: 'win32', deploymentKind: 'unknown' });
  const value = await server.sample();
  assert.equal(fs.reads.size, 0, 'no /proc or cgroup read');
  assert.deepEqual(value.machine, { name: null, kernel: null, uptimeSeconds: null, cpuPercent: null, cpuCount: null, memoryUsedBytes: null, memoryTotalBytes: null, disk: { freeBytes: 250 * 4096, totalBytes: 1000 * 4096, usedBytes: 700 * 4096 }, network: null });
  assert.deepEqual([value.service.kind, value.service.measuredBy, value.service.memoryUsedBytes, value.service.dataBytes], ['service', 'process', 150 * mib, 1536 * mib]);
});

test('bounded and cached: counters once a second, limits and names once a minute, the data size once a minute, a slow file is null', async () => {
  let walks = 0;
  const fs = files(dockerLayout());
  const { server, advance } = resources(fs, { deploymentKind: 'docker', sizeOf: async () => { walks++; return null; } });
  await Promise.all([server.sample(), server.sample()]);
  assert.deepEqual([fs.reads.get('/proc/stat'), fs.reads.get('/sys/fs/cgroup/cpu.max'), walks], [1, 1, 1], 'two reads at once share one');
  advance(500); await server.sample();
  assert.equal(fs.reads.get('/proc/stat'), 1, 'within a second');
  advance(600); await server.sample();
  assert.deepEqual([fs.reads.get('/proc/stat'), fs.reads.get('/sys/fs/cgroup/cpu.max'), walks], [2, 1, 1]);
  advance(60000); const later = await server.sample();
  assert.deepEqual([fs.reads.get('/sys/fs/cgroup/cpu.max'), walks], [2, 2]);
  assert.equal(later.service.dataBytes, null, 'a walk that hit its limit says unknown');
  // A file that never answers is null after the read time limit; the rest of the answer is there.
  const stuck = files({ ...dockerLayout(), '/proc/meminfo': () => new Promise(() => {}) });
  const slow = resources(stuck, { deploymentKind: 'docker', readTimeoutMs: 30 }).server;
  const started = Date.now(), value = await slow.sample();
  assert.ok(Date.now() - started < 1000);
  assert.deepEqual([value.machine.memoryTotalBytes, value.machine.uptimeSeconds, value.service.memoryLimitBytes], [null, 1036800, gib], 'without meminfo the limit still stands');
  // A broken reader never throws the whole answer.
  const broken = new ServerResources({ platform: 'linux', deploymentKind: 'docker', readFile: () => { throw new Error('boom'); }, hostname: () => { throw new Error('boom'); }, release: () => 'x'.repeat(300), sizeOf: async () => { throw new Error('boom'); } });
  const answer = await broken.sample();
  assert.equal(answer.machine.kernel, null, 'an implausible kernel name is not shown');
  assert.equal(answer.service.name, null);
  assert.equal(answer.service.dataBytes, null);
});

test('GET /resources: the server part on Docker and on systemd (shape, no Docker socket, admin only)', async t => {
  for (const kind of ['docker', 'systemd']) {
    const store = new Store(':memory:'), vault = new Vault(randomBytes(32)), token = newToken();
    store.addUser({ name: 'admin', passwordHash: 'fixture', role: 'admin' });
    store.createSession(store.findUser('admin'), tokenHash(token), Date.now(), 3600000, 'admin');
    store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve.invalid', tokenId: 'fixture@pve!r', secret: 'resources-secret' }), 'fixture'); store.db.prepare("INSERT INTO settings(key,value) VALUES('proxmox_initialized','1') ON CONFLICT(key) DO UPDATE SET value=excluded.value").run();
    const fs = files(kind === 'docker' ? dockerLayout() : systemdLayout());
    const server = await buildServer({ store, vault, proxmox: { inventory: async () => [] }, publicOrigin: 'https://resources.invalid', deploymentKind: kind, dataDir: null,
      resourceOptions: { readFile: fs.read, platform: 'linux', hostname: () => 'vdi-broker', release: () => '6.8.12-4-pve', machineCores: () => 4 }, logger: { error: value => assert.fail(value) } });
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
    const base = `http://127.0.0.1:${server.address().port}/api/v1/admin/resources`;
    assert.equal((await fetch(base)).status, 401, 'administrators only');
    const body = await (await fetch(base, { headers: { Cookie: '__Secure-multitap-admin=' + token } })).json();
    assert.equal(body.protocol, 'admin-resources-v1', 'the same protocol: the new fields are beside the old ones');
    assert.deepEqual(Object.keys(body.server).sort(), ['cpuCount', 'cpuPercent', 'deploymentKind', 'disk', 'machine', 'memoryLimitBytes', 'memoryRssBytes', 'service', 'uptimeSeconds']);
    assert.deepEqual(Object.keys(body.server.machine).sort(), ['cpuCount', 'cpuPercent', 'disk', 'kernel', 'memoryTotalBytes', 'memoryUsedBytes', 'name', 'network', 'uptimeSeconds']);
    assert.deepEqual(Object.keys(body.server.service).sort(), ['blockIo', 'cpuBasis', 'cpuLimitCores', 'cpuPercent', 'dataBytes', 'kind', 'measuredBy', 'memoryLimitBytes', 'memoryUnlimited', 'memoryUsedBytes', 'name', 'network', 'state', 'uptimeSeconds']);
    assert.equal(body.server.deploymentKind, kind);
    assert.equal(body.server.service.kind, kind === 'docker' ? 'container' : 'service');
    assert.equal(body.server.machine.name, kind === 'docker' ? null : 'vdi-broker');
    assert.equal(body.server.service.name, kind === 'docker' ? 'vdi-broker' : null);
    assert.equal(body.server.machine.disk, null, 'no data directory here');
    assert.equal(body.server.service.dataBytes, null);
    for (const path of fs.reads.keys()) assert.match(path, /^\/(proc\/(self\/cgroup|stat|meminfo|uptime|net\/dev)|sys\/fs\/cgroup\/)/, 'only /proc and /sys/fs/cgroup: ' + path);
    assert.equal([...fs.reads.keys()].some(path => /docker\.sock/.test(path)), false);
    assert.equal(JSON.stringify(body).includes('resources-secret'), false);
    if (kind === 'docker') {
      const css = await fetch(`http://127.0.0.1:${server.address().port}/admin/resources.css`);
      assert.equal(css.status, 200, 'the stylesheet is served');
      assert.match(await css.text(), /\.res-facts\{/);
      const { readFile } = await import('node:fs/promises');
      assert.match(await readFile(new URL('../admin/index.html', import.meta.url), 'utf8'), /<link rel="stylesheet" href="\/admin\/resources\.css">/);
    }
  }
});

test('the module never talks to Docker: no socket path, no HTTP client, no child process', async () => {
  const { readFile } = await import('node:fs/promises');
  const source = await readFile(new URL('../src/admin-resources.mjs', import.meta.url), 'utf8');
  const code = source.split('\n').filter(line => !/^\s*\/\//.test(line)).join('\n');
  assert.doesNotMatch(code, /docker\.sock|\/var\/run|node:(http|https|net|child_process)|\bspawn\(|execFile/);
});
