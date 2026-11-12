// 홈 자원's server part in English (docs/i18n.md). This file runs in its own
// process: the stored choice 'en' is set before any console module is
// imported, as a browser that chose English has it.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseHTML } from 'linkedom';

const stored = new Map([['multitap-lang', 'en']]);
globalThis.localStorage = { getItem: key => stored.get(key) ?? null, setItem: (key, value) => { stored.set(key, String(value)); }, removeItem: key => { stored.delete(key); } };
const { el } = await import('../admin/js/dom.js');
const { resourcePanel } = await import('../admin/js/resources.js');
const { lang } = await import('../admin/js/i18n.js');

const gib = 1024 ** 3, mib = 1024 ** 2, HANGUL = /[ᄀ-ᇿ㄰-㆏가-힯]/;
const disk = { freeBytes: Math.round(41.3 * gib), totalBytes: 64 * gib, usedBytes: Math.round(22.7 * gib) };
const docker = { cpuPercent: 2.4, cpuCount: 8, memoryRssBytes: 187 * mib, memoryLimitBytes: gib, disk, uptimeSeconds: 7260, deploymentKind: 'docker',
  machine: { name: null, kernel: '6.8.12-4-pve', uptimeSeconds: 12 * 86400 + 5 * 3600, cpuPercent: 14.2, cpuCount: 8, memoryUsedBytes: 6 * gib, memoryTotalBytes: 8 * gib, disk, network: null },
  service: { kind: 'container', state: 'running', measuredBy: 'cgroup', name: '3f9c2a1b7d4e', cpuPercent: 3.2, cpuBasis: 'quota', cpuLimitCores: 2, memoryUsedBytes: 384 * mib, memoryLimitBytes: gib, memoryUnlimited: false,
    network: { receivedBytes: 5e9, sentBytes: 1e9 }, blockIo: { readBytes: 2 * mib, writtenBytes: 512 * 1024 }, dataBytes: Math.round(1.5 * gib), uptimeSeconds: 7260 } };

function render(t, server) {
  const previous = { document: globalThis.document, Node: globalThis.Node };
  const window = parseHTML('<!doctype html><html lang="en"><body></body></html>');
  Object.assign(globalThis, { document: window.document, Node: window.Node });
  t.after(() => Object.assign(globalThis, previous));
  const desktop = { id: 'pve-1', kind: 'vm', name: 'Design 1', pve_vmid: 1, pve_node: 'pve1', pve_power_status: 'running' };
  const value = { protocol: 'admin-resources-v1', checkedAt: new Date().toISOString(), server, proxmox: { configured: true, reachable: true },
    desktops: [{ id: 'pve-1', kind: 'vm', node: 'pve1', cpuPercent: 10, vcpus: 4, memoryUsedBytes: 4 * gib, memoryTotalBytes: 8 * gib, uptimeSeconds: 3600, measuredBy: 'proxmox' }], nodes: [] };
  const ctx = { value: key => key === 'resources' ? value : null, resource: key => key === 'resources' ? { value } : null, desktopById: id => id === 'pve-1' ? desktop : null,
    link: (text, page, id, attrs = {}) => el('a', { ...attrs, href: '#/' + page + (id ? '/' + id : '') }, text), navigate() {} };
  return el('main', {}, resourcePanel(ctx)).querySelector('.resource-panel');
}
const facts = (root, selector) => Object.fromEntries([...root.querySelectorAll(selector + ' tbody tr')].map(tr => [tr.getAttribute('data-fact'), [tr.querySelector('th').textContent, (tr.querySelector('.res-text') ?? tr.querySelector('.res-unknown')).textContent, tr.querySelector('.res-hint')?.textContent ?? '']]));
const readable = root => [...root.querySelectorAll('*')].flatMap(node => [...node.childNodes].filter(child => child.nodeType === 3).map(child => child.nodeValue.trim()).concat(['title', 'aria-label'].map(name => node.getAttribute(name) ?? ''))).filter(Boolean);

test('Server and Multitap container in English: the words, the units, no Korean left', t => {
  assert.equal(lang, 'en');
  const panel = render(t, docker);
  assert.deepEqual([...panel.querySelectorAll('.res-facts thead th')].map(th => th.textContent), ['Server', 'Multitap container']);
  assert.deepEqual(facts(panel, '.res-machine'), {
    kernel: ['Kernel', '6.8.12-4-pve', ''], uptime: ['Uptime', '12d 5h', ''], cpu: ['CPU', '14%', '8 cores'], memory: ['Memory', '6.0 / 8.0 GB', '75%'], disk: ['Disk', '22.7 / 64.0 GB', '35%'] });
  assert.deepEqual(facts(panel, '.res-service'), {
    name: ['Name', '3f9c2a1b7d4e', ''], state: ['State', 'Running', ''], cpu: ['CPU', '3.2%', 'Of the 2-core CPU limit'], memory: ['Memory', '384 MB / 1.0 GB', '38%'],
    network: ['Network', 'Received 4.7 GB · Sent 954 MB', ''], io: ['Block I/O', 'Read 2 MB · Written 512 KB', ''], data: ['Data size', '1.5 GB', ''], uptime: ['Uptime', '2h 1m', ''] });
  const leftovers = readable(panel).filter(text => HANGUL.test(text));
  assert.deepEqual(leftovers, []);
  assert.deepEqual(readable(panel).filter(text => /\{[A-Za-z]\w*\}/.test(text)), [], 'placeholders filled');
});

test('Multitap service in English, one core, no limit, and Not available for what could not be read', t => {
  const panel = render(t, { ...docker, deploymentKind: 'systemd', machine: { ...docker.machine, name: 'vdi-broker', cpuCount: 1, network: null, kernel: null, memoryUsedBytes: null },
    service: { ...docker.service, kind: 'service', name: null, network: null, cpuBasis: 'cores', cpuLimitCores: null, memoryLimitBytes: null, memoryUnlimited: true, memoryUsedBytes: 200 * mib, blockIo: null, dataBytes: null } });
  assert.deepEqual([...panel.querySelectorAll('.res-facts thead th')].map(th => th.textContent), ['Server', 'Multitap service']);
  const machine = facts(panel, '.res-machine'), service = facts(panel, '.res-service');
  assert.deepEqual([machine.name, machine.kernel, machine.cpu, machine.memory, machine.network], [['Name', 'vdi-broker', ''], ['Kernel', 'Not available', ''], ['CPU', '14%', '1 core'], ['Memory', 'Not available', ''], ['Network', 'Not available', '']]);
  assert.deepEqual([service.cpu, service.memory, service.io, service.data], [['CPU', '3.2%', 'Of the 1 core'], ['Memory', '200 MB', 'No limit'], ['Block I/O', 'Not available', ''], ['Data size', 'Not available', '']]);
  assert.deepEqual(readable(panel).filter(text => HANGUL.test(text)), []);
});
