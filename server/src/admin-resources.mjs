// 홈 자원 (admin-resources-v1): what the server and the running desktops use,
// for the administrator console only (admin-http.mjs GET /api/v1/admin/resources).
// Read-only and bounded:
// - the desktops' numbers are the samples the broker already keeps
//   (power.mjs refresh, the same registered-VM read the clients' live channel
//   uses, merged with the Host's own report as clients see it); a read here
//   only lets that one read run if its last run is older than `refreshMs`,
//   and never waits for it longer than `waitMs`;
// - the server's numbers: its own process (CPU time, RSS, uptime), the data
//   directory's file system (one statfs) and, on Linux, the container memory
//   limit (one small cgroup file, cached). These keep their 1.0 meaning.
// - also `server.machine` (the machine, CT or VM the server
//   runs on: /proc/stat, /proc/meminfo, /proc/uptime, /proc/net/dev) and
//   `server.service` (the Multitap container on Docker, else the Multitap
//   service: its cgroup v2 files under /sys/fs/cgroup, the data folder's size).
//   Only small /proc and /sys/fs/cgroup files are read, each with a time limit;
//   the quick ones at most once a second, the slow ones once a minute.
//   Never the Docker socket: a container that can reach it can start
//   containers with the host's disks and so controls the host. What Docker
//   itself would say (docker stats) comes from the same cgroup files.
// Node capacity is not read: the Proxmox token has VM.Audit only.
// Unknown is null, never 0; one file that fails makes its own numbers null,
// never the whole answer.
import { availableParallelism, totalmem, hostname as osHostname, release as osRelease, cpus as osCpus } from 'node:os';
import { readFile, statfs, opendir, lstat } from 'node:fs/promises';
import { join, posix } from 'node:path';
import { mergeResources, pcResources } from './host-resources.mjs';

export const adminResourcesProtocol = 'admin-resources-v1';
const maxUptimeSeconds = 100 * 366 * 86400;
const count = (value, max = Number.MAX_SAFE_INTEGER) => Number.isSafeInteger(value) && value >= 0 && value <= max ? value : null;
const percent = value => typeof value === 'number' && Number.isFinite(value) && value >= 0 && value <= 100 ? Math.round(value * 10) / 10 : null;
const cgroupRoot = '/sys/fs/cgroup';
const maxText = 256 * 1024;
const digits = /^\d{1,20}$/;
const whole = text => digits.test(text) ? count(Number(text)) : null;

// ---- Parsers. Each takes the file's text and answers null for anything it
// does not recognise (a missing field, garbage, a number out of range).

// /proc/stat: the "cpu" line's jiffies (user nice system idle iowait irq
// softirq steal; guest and guest_nice are already inside user and nice) and
// the number of "cpuN" lines.
export function parseProcStat(text) {
  const lines = String(text ?? '').split('\n');
  const first = lines.find(line => /^cpu\s/.test(line));
  if (!first) return null;
  const fields = first.trim().split(/\s+/).slice(1, 9);
  if (fields.length < 4 || !fields.every(field => digits.test(field))) return null;
  const values = fields.map(Number);
  const total = values.reduce((sum, value) => sum + value, 0), idle = values[3] + (values[4] ?? 0);
  if (!Number.isSafeInteger(total) || total <= 0) return null;
  const cpus = lines.filter(line => /^cpu\d+\s/.test(line)).length;
  return { total, idle, cpus: cpus || null };
}
// /proc/meminfo: MemTotal and MemAvailable in kB.
export function parseMeminfo(text) {
  const field = name => { const match = new RegExp(`^${name}:\\s+(\\d{1,17})\\s*kB\\s*$`, 'm').exec(String(text ?? '')); return match ? count(Number(match[1]) * 1024) : null; };
  const totalBytes = field('MemTotal'), availableBytes = field('MemAvailable');
  if (totalBytes === null || totalBytes === 0) return null;
  return { totalBytes, usedBytes: availableBytes !== null && availableBytes <= totalBytes ? totalBytes - availableBytes : null };
}
// /proc/uptime: "seconds-up seconds-idle".
export function parseUptime(text) {
  const match = /^(\d{1,12})(?:\.\d+)?\s/.exec(String(text ?? '') + ' ');
  return match ? count(Number(match[1]), maxUptimeSeconds) : null;
}
// /proc/net/dev: received and sent bytes summed over every interface but lo.
export function parseNetDev(text) {
  let receivedBytes = 0, sentBytes = 0, seen = 0;
  for (const line of String(text ?? '').split('\n')) {
    const match = /^\s*([^\s:]+):\s*(.*)$/.exec(line);
    if (!match || match[1] === 'lo') continue;
    const fields = match[2].trim().split(/\s+/);
    if (fields.length < 9 || !digits.test(fields[0]) || !digits.test(fields[8])) return null;
    receivedBytes += Number(fields[0]); sentBytes += Number(fields[8]); seen++;
  }
  return seen && Number.isSafeInteger(receivedBytes) && Number.isSafeInteger(sentBytes) ? { receivedBytes, sentBytes } : null;
}
// /proc/self/cgroup: the cgroup v2 line "0::/path". A v1-only layout has none.
export function parseSelfCgroup(text) {
  const match = /^0::(\/[^\n\0]{0,1024})$/m.exec(String(text ?? ''));
  if (!match) return null;
  const path = match[1].trim();
  return path.split('/').some(part => part === '..' || part === '.') ? null : path;
}
// The cgroup's folder under /sys/fs/cgroup, never outside it. Inside a
// container with its own cgroup namespace the path is "/" (the root of what
// it sees is its own cgroup).
export function cgroupDir(path) {
  if (typeof path !== 'string' || !path.startsWith('/')) return null;
  const dir = posix.join(cgroupRoot, path).replace(/\/+$/, '');
  return dir === cgroupRoot || dir.startsWith(cgroupRoot + '/') ? dir : null;
}
// cpu.stat: usage_usec.
export function parseCpuStat(text) {
  const match = /^usage_usec (\d{1,20})$/m.exec(String(text ?? ''));
  return match ? count(Number(match[1])) : null;
}
// cpu.max: "max 100000" (no quota) or "200000 100000" (two cores' worth).
export function parseCpuMax(text) {
  const match = /^(max|\d{1,15}) (\d{1,15})$/.exec(String(text ?? '').trim());
  if (!match) return null;
  const period = Number(match[2]);
  if (match[1] === 'max') return { limited: false, cores: null };
  const quota = Number(match[1]);
  return quota > 0 && period > 0 ? { limited: true, cores: Math.round(quota / period * 100) / 100 } : null;
}
// memory.max: "max" or bytes. A value at or above the machine's memory is no
// limit either (cgroup v1 writes a huge number for none).
export function parseMemoryMax(text, machineBytes = null) {
  const value = String(text ?? '').trim();
  if (value === 'max') return { limited: false, bytes: null };
  if (!digits.test(value) || Number(value) === 0) return null;
  const bytes = Number(value);
  return !Number.isSafeInteger(bytes) || (machineBytes && bytes >= machineBytes) ? { limited: false, bytes: null } : { limited: true, bytes };
}
// memory.stat: inactive_file, which docker stats leaves out of "used".
export function parseInactiveFile(text) {
  const match = /^inactive_file (\d{1,20})$/m.exec(String(text ?? ''));
  return match ? count(Number(match[1])) : null;
}
// io.stat: "8:0 rbytes=… wbytes=… rios=…" per device, summed.
export function parseIoStat(text) {
  const lines = String(text ?? '').split('\n').filter(line => line.trim());
  let readBytes = 0, writtenBytes = 0;
  for (const line of lines) {
    if (!/^\d+:\d+\s/.test(line)) return null;
    const read = /\brbytes=(\d{1,20})\b/.exec(line), written = /\bwbytes=(\d{1,20})\b/.exec(line);
    if (!read || !written) return null;
    readBytes += Number(read[1]); writtenBytes += Number(written[1]);
  }
  // An empty io.stat is a cgroup that has not read or written a block device yet.
  return Number.isSafeInteger(readBytes) && Number.isSafeInteger(writtenBytes) ? { readBytes, writtenBytes } : null;
}

// ---- CPU over a window. The first reading, and one more than two minutes
// after the previous, starts a window and says null; a reading closer than
// one second to the previous keeps the last value.
export class CpuWindow {
  constructor({ minMs = 1000, maxMs = 120000 } = {}) { Object.assign(this, { minMs, maxMs }); this.base = null; this.value = null; }
  // `read()` gives the counter now (or null); `compute(before, after, elapsedMs)` the percent.
  next(at, read, compute) {
    const base = this.base;
    if (base && at - base.at >= 0 && at - base.at < this.minMs) return this.value;
    const reading = read();
    if (reading === null || reading === undefined) { this.base = null; this.value = null; return null; }
    this.base = { at, reading };
    if (!base || at - base.at > this.maxMs || at <= base.at) { this.value = null; return null; }
    const value = compute(base.reading, reading, at - base.at);
    this.value = value === null ? null : percent(Math.min(100, Math.max(0, value)));
    return this.value;
  }
}
// The whole machine: the share of jiffies that were not idle.
export const machineCpu = (before, after) => {
  const total = after.total - before.total, idle = after.idle - before.idle;
  return total > 0 && idle >= 0 && idle <= total ? (total - idle) / total * 100 : null;
};
// A cgroup: CPU time over the time passed times the cores it may use.
export const cgroupCpu = cores => (before, after, elapsedMs) => {
  const used = after - before;
  return used >= 0 && cores > 0 ? used / (elapsedMs * 1000 * cores) * 100 : null;
};

// ---- The data folder's size: a walk that stops (null) at `maxFiles`
// entries or after `maxMs`, does not follow links and counts file sizes.
export async function dataSize(dir, { maxFiles = 20000, maxMs = 1000, now = Date.now, open = opendir, stat = lstat } = {}) {
  if (!dir) return null;
  const started = now(), stack = [dir];
  let total = 0, seen = 0;
  while (stack.length) {
    const current = stack.pop();
    let handle;
    try { handle = await open(current); } catch (error) { if (current !== dir && error?.code === 'ENOENT') continue; return null; }
    try {
      for await (const entry of handle) {
        if (++seen > maxFiles || now() - started > maxMs) return null;
        const path = join(current, entry.name);
        if (entry.isDirectory()) stack.push(path);
        else if (entry.isFile()) {
          try { total += Number((await stat(path)).size) || 0; } catch (error) { if (error?.code !== 'ENOENT') return null; /* A file removed meanwhile (a journal) is gone. */ }
        }
      }
    } catch (error) { if (error?.code !== 'ENOENT') return null; }
  }
  return count(total);
}

// One read with a time limit; any failure or a slow file is null.
async function bounded(read, path, ms) {
  let timer;
  try {
    const value = await Promise.race([Promise.resolve().then(() => read(path, 'utf8')), new Promise(resolve => { timer = setTimeout(() => resolve(null), ms); timer.unref?.(); })]);
    return value === null || value === undefined ? null : String(value).slice(0, maxText);
  } catch { return null; } finally { clearTimeout(timer); }
}

// The server: its process as before, and on Linux the machine and the
// Multitap container or service.
export class ServerResources {
  constructor({ dataDir = null, deploymentKind = 'unknown', now = Date.now, cpuUsage = () => process.cpuUsage(), rss = () => process.memoryUsage.rss(), uptime = () => process.uptime(), cores = availableParallelism,
    statfs: fsStat = statfs, readFile: read = readFile, platform = process.platform, totalMemory = totalmem, limitTtlMs = 60000,
    hostname = osHostname, release = osRelease, machineCores = () => osCpus().length, quickTtlMs = 1000, slowTtlMs = 60000, readTimeoutMs = 1000,
    sizeOf = dir => dataSize(dir), dataSizeTtlMs = 60000 } = {}) {
    Object.assign(this, { dataDir, deploymentKind, now, cpuUsage, rss, uptime, cores, fsStat, read, platform, totalMemory, limitTtlMs, hostname, release, machineCores, quickTtlMs, slowTtlMs, readTimeoutMs, sizeOf, dataSizeTtlMs });
    this.base = null; this.cpuPercent = null; this.limit = { at: -Infinity, value: null };
    this.machineWindow = new CpuWindow(); this.cgroupWindow = new CpuWindow();
    this.slow = { at: -Infinity, value: null, pending: null }; this.quick = { at: -Infinity, value: null, pending: null }; this.size = { at: -Infinity, value: null, pending: null };
  }
  cpu() {
    const at = this.now(), usage = this.cpuUsage(), base = this.base;
    if (base && at - base.at < 1000) return this.cpuPercent;
    this.base = { at, usage };
    if (!base || at - base.at > 120000) { this.cpuPercent = null; return null; }
    const used = (usage.user - base.usage.user) + (usage.system - base.usage.system), cores = Math.max(1, this.cores());
    this.cpuPercent = Number.isFinite(used) && used >= 0 ? percent(Math.min(100, used / 1000 / (at - base.at) / cores * 100)) : null;
    return this.cpuPercent;
  }
  // cgroup v2 memory.max, else v1 memory.limit_in_bytes. "max" or a value at
  // or above the machine's memory is no limit (null).
  async memoryLimit() {
    if (this.platform !== 'linux') return null;
    if (this.now() - this.limit.at < this.limitTtlMs) return this.limit.value;
    let value = null;
    for (const file of ['/sys/fs/cgroup/memory.max', '/sys/fs/cgroup/memory/memory.limit_in_bytes']) {
      try {
        const text = String(await this.read(file, 'utf8')).trim();
        if (text === 'max') break;
        if (/^\d{1,20}$/.test(text)) { const bytes = Number(text); value = Number.isSafeInteger(bytes) && bytes > 0 && bytes < this.totalMemory() ? bytes : null; break; }
      } catch { /* Not this cgroup layout, or not readable: no limit is known. */ }
    }
    this.limit = { at: this.now(), value };
    return value;
  }
  async disk() {
    if (!this.dataDir) return null;
    try {
      const stat = await this.fsStat(this.dataDir), size = Number(stat.bsize);
      const totalBytes = Number(stat.blocks) * size, freeBytes = Number(stat.bavail) * size, spareBytes = Number(stat.bfree) * size;
      if (!(Number.isSafeInteger(totalBytes) && totalBytes > 0 && Number.isSafeInteger(freeBytes) && freeBytes >= 0 && freeBytes <= totalBytes)) return null;
      // Used is what df says: all blocks but the free ones (root's reserve counts as neither used nor free to others).
      const used = Number.isSafeInteger(spareBytes) && spareBytes >= freeBytes && spareBytes <= totalBytes ? totalBytes - spareBytes : totalBytes - freeBytes;
      return { freeBytes, totalBytes, usedBytes: used };
    } catch { return null; }
  }
  file(path) { return bounded(this.read, path, this.readTimeoutMs); }
  // A cached value with one read in flight at a time.
  async cached(slot, ttl, load) {
    if (this.now() - slot.at < ttl) return slot.value;
    if (!slot.pending) slot.pending = (async () => { try { const value = await load(); Object.assign(slot, { at: this.now(), value }); return value; } catch { Object.assign(slot, { at: this.now(), value: null }); return null; } finally { slot.pending = null; } })();
    return slot.pending;
  }
  // Once a minute: names, the cgroup's folder and its limits.
  slowFacts() {
    return this.cached(this.slow, this.slowTtlMs, async () => {
      const safe = fn => { try { const value = fn(); return typeof value === 'string' && value && value.length <= 255 ? value : null; } catch { return null; } };
      const [self, meminfo] = await Promise.all([this.file('/proc/self/cgroup'), this.file('/proc/meminfo')]);
      const dir = cgroupDir(parseSelfCgroup(self)), machineBytes = parseMeminfo(meminfo)?.totalBytes ?? null;
      const [cpuMax, memoryMax] = dir ? await Promise.all([this.file(dir + '/cpu.max'), this.file(dir + '/memory.max')]) : [null, null];
      return { name: safe(this.hostname), kernel: safe(this.release), dir, cpuMax: parseCpuMax(cpuMax), memoryMax: parseMemoryMax(memoryMax, machineBytes) };
    });
  }
  // At most once a second: the counters.
  quickFacts(dir) {
    return this.cached(this.quick, this.quickTtlMs, async () => {
      const files = ['/proc/stat', '/proc/meminfo', '/proc/uptime', '/proc/net/dev', ...(dir ? ['cpu.stat', 'memory.current', 'memory.stat', 'io.stat'].map(name => dir + '/' + name) : [])];
      const [stat, meminfo, uptime, netDev, cpuStat, memoryCurrent, memoryStat, ioStat] = await Promise.all(files.map(file => this.file(file)));
      return { at: this.now(), stat: parseProcStat(stat), memory: parseMeminfo(meminfo), uptimeSeconds: parseUptime(uptime), network: parseNetDev(netDev),
        cgroupUsage: parseCpuStat(cpuStat), memoryCurrent: typeof memoryCurrent === 'string' ? whole(memoryCurrent.trim()) : null, inactiveFile: parseInactiveFile(memoryStat), io: typeof ioStat === 'string' ? parseIoStat(ioStat) : null };
    });
  }
  dataBytes() { return this.dataDir ? this.cached(this.size, this.dataSizeTtlMs, () => this.sizeOf(this.dataDir)) : Promise.resolve(null); }

  // The machine and the Multitap container/service. On another platform
  // than Linux nothing is read and every number is null.
  async system(own) {
    const container = this.deploymentKind === 'docker', linux = this.platform === 'linux';
    const [slow, data] = await Promise.all([linux ? this.slowFacts() : null, this.dataBytes().catch(() => null)]);
    const quick = linux ? await this.quickFacts(slow?.dir ?? null) : null;
    const machineCpus = count(quick?.stat?.cpus ?? (() => { try { return this.machineCores(); } catch { return null; } })(), 4096) || null;
    const ownCores = (() => { try { return count(this.cores(), 4096) || null; } catch { return null; } })();
    const foreign = container && ownCores !== null && machineCpus !== null && ownCores < machineCpus;
    const cpus = foreign ? ownCores : machineCpus;
    const machinePercent = quick && !foreign ? this.machineWindow.next(quick.at, () => quick.stat, machineCpu) : null;
    // Inside Docker the network counters, and the host name, are the container's own.
    const machine = { name: container ? null : slow?.name ?? null, kernel: slow?.kernel ?? null, uptimeSeconds: foreign ? null : quick?.uptimeSeconds ?? null, cpuPercent: machinePercent, cpuCount: linux ? cpus : null,
      memoryUsedBytes: foreign ? null : quick?.memory?.usedBytes ?? null, memoryTotalBytes: foreign ? null : quick?.memory?.totalBytes ?? null, disk: own.disk, network: container ? null : quick?.network ?? null };
    // The cgroup when its counters are readable, else the server process.
    const cgroup = quick?.cgroupUsage !== null && quick?.cgroupUsage !== undefined;
    const quotaCores = slow?.cpuMax?.limited ? slow.cpuMax.cores : null, basisCores = quotaCores ?? cpus;
    const cpuPercent = cgroup ? (basisCores ? this.cgroupWindow.next(quick.at, () => quick.cgroupUsage, cgroupCpu(basisCores)) : null) : own.cpuPercent;
    const current = quick?.memoryCurrent ?? null, inactive = quick?.inactiveFile ?? null;
    const memoryUsedBytes = current !== null ? (inactive !== null && inactive <= current ? current - inactive : current) : own.memoryRssBytes;
    const memoryMax = current !== null ? slow?.memoryMax ?? null : null;
    const service = { kind: container ? 'container' : 'service', state: 'running', measuredBy: cgroup ? 'cgroup' : 'process', name: container ? slow?.name ?? null : null,
      cpuPercent, cpuBasis: cgroup ? (quotaCores ? 'quota' : 'cores') : 'process', cpuLimitCores: quotaCores,
      memoryUsedBytes, memoryLimitBytes: memoryMax?.limited ? memoryMax.bytes : null, memoryUnlimited: memoryMax ? !memoryMax.limited : null,
      network: container ? quick?.network ?? null : null, blockIo: cgroup ? quick?.io ?? null : null, dataBytes: count(data), uptimeSeconds: own.uptimeSeconds };
    return { machine, service };
  }
  async sample() {
    const cpuPercent = this.cpu();
    let rss = null; try { rss = count(this.rss()); } catch { rss = null; }
    const [memoryLimitBytes, disk] = await Promise.all([this.memoryLimit().catch(() => null), this.disk()]);
    const up = Math.floor(this.uptime());
    const own = { cpuPercent, cpuCount: count(this.cores(), 4096), memoryRssBytes: rss, memoryLimitBytes, disk, uptimeSeconds: count(up, maxUptimeSeconds) };
    let system;
    try { system = await this.system(own); } catch { system = { machine: null, service: null }; }
    return { ...own, deploymentKind: this.deploymentKind, ...system };
  }
}

// One running desktop's numbers: Proxmox's sample, with the Host's own CPU and
// memory when it reports them (as the clients show it); a PC has only its Host.
function desktopRow(row, { power, relay, now, maxAgeMs }) {
  const report = relay?.hostResources?.(row.id) ?? null;
  if (row.kind === 'pc') {
    const value = pcResources(report);
    return value ? { id: row.id, kind: 'pc', node: null, ...numbers(value), measuredBy: 'host' } : null;
  }
  if (row.pve_power_status !== 'running') return null;
  const sample = row.pve_power_error ? null : power?.latestSample?.(row.pve_vmid, maxAgeMs) ?? null;
  const merged = sample ? mergeResources(sample, report) : report ? { measuredBy: 'host', cpuUsagePercent: report.cpuPercent, vcpus: report.cpuCount, memoryUsedBytes: report.memoryUsedBytes, memoryTotalBytes: report.memoryTotalBytes, uptimeSeconds: null } : null;
  const values = numbers(merged ?? {});
  // Proxmox's uptime moves on with the time since its sample was read.
  const at = sample ? Date.parse(sample.checkedAt) : NaN;
  if (values.uptimeSeconds !== null && Number.isFinite(at)) values.uptimeSeconds = count(values.uptimeSeconds + Math.max(0, Math.floor((now - at) / 1000)), maxUptimeSeconds);
  return { id: row.id, kind: 'vm', node: row.pve_node ?? null, ...values, measuredBy: merged ? merged.measuredBy ?? 'proxmox' : null };
}
function numbers(value) {
  const memoryTotalBytes = count(value.memoryTotalBytes) || null, used = count(value.memoryUsedBytes);
  return { cpuPercent: percent(value.cpuUsagePercent), vcpus: count(value.vcpus, 4096) || null, memoryUsedBytes: used !== null && (memoryTotalBytes === null || used <= memoryTotalBytes) ? used : null, memoryTotalBytes, uptimeSeconds: count(value.uptimeSeconds, maxUptimeSeconds) };
}
// Per Proxmox node, the running desktops' sums. A sum covers the desktops
// whose two numbers are both known (used beside assigned), else null.
function nodeSums(desktops) {
  const nodes = new Map();
  for (const desktop of desktops) {
    if (desktop.kind !== 'vm' || !desktop.node) continue;
    const sum = nodes.get(desktop.node) ?? { node: desktop.node, running: 0, cpu: 0, cpuUsed: 0, vcpus: 0, memory: 0, memoryUsed: 0, memoryTotal: 0 };
    sum.running++;
    if (desktop.cpuPercent !== null && desktop.vcpus !== null) { sum.cpu++; sum.cpuUsed += desktop.cpuPercent / 100 * desktop.vcpus; sum.vcpus += desktop.vcpus; }
    if (desktop.memoryUsedBytes !== null && desktop.memoryTotalBytes !== null) { sum.memory++; sum.memoryUsed += desktop.memoryUsedBytes; sum.memoryTotal += desktop.memoryTotalBytes; }
    nodes.set(desktop.node, sum);
  }
  return [...nodes.values()].sort((a, b) => a.node.localeCompare(b.node, 'en', { numeric: true })).map(sum => ({ node: sum.node, running: sum.running,
    vcpusUsed: sum.cpu ? Math.round(sum.cpuUsed * 10) / 10 : null, vcpusAssigned: sum.cpu ? sum.vcpus : null,
    memoryUsedBytes: sum.memory ? sum.memoryUsed : null, memoryTotalBytes: sum.memory ? sum.memoryTotal : null }));
}

export class AdminResources {
  constructor({ store, power, relay, server = new ServerResources(), now = Date.now, refreshMs = 10000, waitMs = 3000, maxAgeMs = 60000 }) {
    Object.assign(this, { store, power, relay, server, now, refreshMs, waitMs, maxAgeMs });
  }
  // Let the shared Proxmox read run when its last run is old enough; answer
  // with what is known after at most waitMs (a slow Proxmox keeps reading
  // in the background and the next read sees its result).
  async refresh() {
    if (!this.store.setting('proxmox') || typeof this.power?.refresh !== 'function') return;
    let timer;
    const wait = new Promise(resolve => { timer = setTimeout(resolve, this.waitMs); timer.unref?.(); });
    try { await Promise.race([this.power.refresh({ maxAgeMs: this.refreshMs }).catch(() => {}), wait]); } finally { clearTimeout(timer); }
  }
  async read() {
    await this.refresh();
    const server = await this.server.sample();
    const now = this.now(), registered = this.store.registeredHostIds();
    const rows = this.store.db.prepare("SELECT id,kind,pve_node,pve_vmid,pve_power_status,pve_power_error FROM desktops WHERE kind IN ('vm','pc') ORDER BY id").all().filter(row => registered.has(row.id) && (row.kind === 'pc' ? row.pve_vmid === null : Number.isSafeInteger(row.pve_vmid)));
    const desktops = rows.map(row => desktopRow(row, { power: this.power, relay: this.relay, now, maxAgeMs: this.maxAgeMs })).filter(Boolean);
    const configured = !!this.store.setting('proxmox'), error = configured ? this.power?.snapshotError ?? null : null;
    return { protocol: adminResourcesProtocol, checkedAt: new Date(now).toISOString(),
      server, proxmox: { configured, reachable: configured ? !error : null },
      desktops, nodes: nodeSums(desktops) };
  }
}
