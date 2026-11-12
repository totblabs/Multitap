import { test } from 'node:test';
import assert from 'node:assert/strict';
import { validHostResources, freshHostResources, mergeResources, pcResources, hostResourcesFreshMs, hostResourcesProtocol } from '../src/host-resources.mjs';

const good = { cpuPercent: 12.5, cpuCount: 4, memoryUsedBytes: 1000, memoryTotalBytes: 4000 };
const sample = { source: 'proxmox', checkedAt: 'x', cpuUsagePercent: 1, vcpus: 2, memoryUsedBytes: 4000, memoryTotalBytes: 4000, uptimeSeconds: 77 };

test('validHostResources accepts a sane report and rejects everything else', () => {
  assert.deepEqual(validHostResources({ ...good, extra: 'dropped' }), good);
  for (const bad of [null, [], 'x', {}, { ...good, cpuPercent: 101 }, { ...good, cpuPercent: -1 }, { ...good, cpuPercent: NaN }, { ...good, cpuPercent: '5' },
    { ...good, cpuCount: 0 }, { ...good, cpuCount: 1.5 }, { ...good, memoryTotalBytes: 0 }, { ...good, memoryUsedBytes: 4001 }, { ...good, memoryUsedBytes: -1 }]) assert.equal(validHostResources(bad), null, JSON.stringify(bad));
});

test('freshHostResources: open Host, within the freshness window only', () => {
  const host = { ws: { readyState: 1 }, resources: { ...good, at: 1000 } };
  assert.ok(freshHostResources(host, 1000 + hostResourcesFreshMs));
  assert.equal(freshHostResources(host, 1000 + hostResourcesFreshMs + 1), null, 'stale');
  assert.equal(freshHostResources({ ...host, ws: { readyState: 3 } }, 2000), null, 'closed Host');
  assert.equal(freshHostResources({ ws: { readyState: 1 } }, 2000), null, 'old Host never reported');
  assert.equal(freshHostResources(undefined, 2000), null);
});

test('mergeResources: Host CPU/memory win, uptime and source stay; no report falls back', () => {
  const merged = mergeResources(sample, { ...good, at: 1 });
  assert.deepEqual(merged, { ...sample, measuredBy: 'host', cpuUsagePercent: 12.5, vcpus: 4, memoryUsedBytes: 1000, memoryTotalBytes: 4000 });
  assert.deepEqual(mergeResources(sample, null), sample);
  assert.equal(mergeResources(null, good), null);
  assert.equal(mergeResources(sample, { ...good, uptimeSeconds: 5, at: 1 }).uptimeSeconds, 77, 'a VM keeps Proxmox\'s uptime');
});

test('host-resources-v2: an optional whole uptime; a bad one drops only the uptime', () => {
  assert.equal(hostResourcesProtocol, 'host-resources-v2');
  assert.deepEqual(validHostResources({ ...good, uptimeSeconds: 0 }), { ...good, uptimeSeconds: 0 });
  assert.deepEqual(validHostResources({ ...good, uptimeSeconds: 100 * 366 * 86400 }), { ...good, uptimeSeconds: 100 * 366 * 86400 });
  for (const uptimeSeconds of [-1, 1.5, '60', null, NaN, Infinity, 100 * 366 * 86400 + 1]) assert.deepEqual(validHostResources({ ...good, uptimeSeconds }), good, String(uptimeSeconds));
});

test('pcResources: the fresh Host report alone, in the VM fields, as source host', () => {
  assert.equal(pcResources(null), null);
  assert.deepEqual(pcResources({ ...good, uptimeSeconds: 90, at: Date.UTC(2026, 10, 3) }), { source: 'host', measuredBy: 'host', checkedAt: '2026-11-03T00:00:00.000Z', stale: false,
    cpuUsagePercent: 12.5, vcpus: 4, memoryUsedBytes: 1000, memoryTotalBytes: 4000, uptimeSeconds: 90 });
  assert.equal(pcResources({ ...good, at: 0 }).uptimeSeconds, null, 'a Host before v2: no uptime');
});
