// Host-measured CPU/memory (host-resources-v1). The Host runs inside the VM and
// reports its own numbers in the optional `resources` object of
// `host.heartbeat`; released brokers ignore unknown heartbeat fields and
// released Hosts simply never send it. Self-reported display data only: it
// never grants authorization or readiness, and is bounded and validated.
//
// Why: Proxmox shows a Windows guest without the VirtIO balloon service at
// about 100 % memory always (host-side view); the in-guest figure is real.
// Heartbeats come about every 10 s (earlier on larger changes), so a report
// counts as fresh for 25 s: one missed heartbeat does not flip the source.
// A physical PC's tile has only these numbers (pcResources below).
export const hostResourcesFreshMs = 25000;
// Named in host.capabilities.ack: the Host then adds `uptimeSeconds` (how long
// Windows has run since it last started or woke) to the report. Only a
// physical PC's tile uses it; a VM keeps Proxmox's uptime.
export const hostResourcesProtocol = 'host-resources-v2';
const maxUptimeSeconds = 100 * 366 * 86400;

const whole = (value, min, max) => Number.isSafeInteger(value) && value >= min && value <= max;

// Returns the sanitized report or null (then the heartbeat's other fields still apply).
export function validHostResources(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return null;
  const { cpuPercent, cpuCount, memoryUsedBytes, memoryTotalBytes, uptimeSeconds } = value;
  if (typeof cpuPercent !== 'number' || !Number.isFinite(cpuPercent) || cpuPercent < 0 || cpuPercent > 100) return null;
  if (!whole(cpuCount, 1, 4096) || !whole(memoryTotalBytes, 1, Number.MAX_SAFE_INTEGER) || !whole(memoryUsedBytes, 0, memoryTotalBytes)) return null;
  // A bad uptime drops only the uptime.
  return { cpuPercent, cpuCount, memoryUsedBytes, memoryTotalBytes, ...(whole(uptimeSeconds, 0, maxUptimeSeconds) ? { uptimeSeconds } : {}) };
}

// The latest fresh Host report of an open Host connection, else null.
export function freshHostResources(host, now, maxAgeMs = hostResourcesFreshMs) {
  const report = host?.resources;
  if (!report || host.ws?.readyState !== 1 || now - report.at > maxAgeMs || now < report.at - 1000) return null;
  return report;
}

// Prefers fresh Host numbers for CPU and memory (and the CPU count); uptime and
// freshness stay Proxmox's (Proxmox's uptime is the VM's, Windows' boot time
// can differ through Fast Startup/hibernation). `source` stays 'proxmox' for
// released clients that only accept that value; `measuredBy: 'host'` is added\n// only for Host numbers (absent = Proxmox), so Proxmox-only payloads are unchanged.
export function mergeResources(sample, report) {
  if (!sample) return sample;
  if (!report) return sample;
  return { ...sample, measuredBy: 'host', cpuUsagePercent: report.cpuPercent, vcpus: report.cpuCount, memoryUsedBytes: report.memoryUsedBytes, memoryTotalBytes: report.memoryTotalBytes };
}

// A physical PC has no Proxmox sample: its tile shows the fresh Host report
// alone, in the VM fields, with `source: 'host'` (a released client accepts
// only 'proxmox' and so shows nothing new). Null without a fresh report.
export function pcResources(report) {
  if (!report) return null;
  return { source: 'host', measuredBy: 'host', checkedAt: new Date(report.at).toISOString(), stale: false,
    cpuUsagePercent: report.cpuPercent, vcpus: report.cpuCount, memoryUsedBytes: report.memoryUsedBytes, memoryTotalBytes: report.memoryTotalBytes,
    uptimeSeconds: report.uptimeSeconds ?? null };
}
