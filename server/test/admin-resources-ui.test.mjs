import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseHTML } from 'linkedom';
import { el } from '../admin/js/dom.js';
import { render } from '../admin/js/overview.js';
import { resourcePanel, upFor, sortRows, amount, pair, level } from '../admin/js/resources.js';

// 홈 자원 (admin/js/resources.js) on real DOM: no server, network or write.
const gib = 1024 ** 3;
const vm = (n, node = 'pve1') => ({ id: 'pve-' + n, kind: 'vm', name: '데스크톱 ' + n, pve_vmid: n, pve_node: node, pve_power_status: 'running', host: { connected: true } });
const row = (n, extra = {}) => ({ id: 'pve-' + n, kind: 'vm', node: 'pve1', cpuPercent: 10, vcpus: 4, memoryUsedBytes: 4 * gib, memoryTotalBytes: 8 * gib, uptimeSeconds: 3600, measuredBy: 'proxmox', ...extra });
const mib = 1024 ** 2, disk = { freeBytes: Math.round(41.3 * gib), totalBytes: 64 * gib, usedBytes: Math.round(22.7 * gib) };
// A Docker install (compose.yaml): the machine is the Docker host, the container has its own name and network.
const server = { cpuPercent: 2.4, cpuCount: 8, memoryRssBytes: 187 * mib, memoryLimitBytes: 2 * gib, disk, uptimeSeconds: 7260, deploymentKind: 'docker',
  machine: { name: null, kernel: '6.8.12-4-pve', uptimeSeconds: 12 * 86400 + 5 * 3600, cpuPercent: 14.2, cpuCount: 8, memoryUsedBytes: 6 * gib, memoryTotalBytes: 8 * gib, disk, network: null },
  service: { kind: 'container', state: 'running', measuredBy: 'cgroup', name: '3f9c2a1b7d4e', cpuPercent: 3.2, cpuBasis: 'quota', cpuLimitCores: 2, memoryUsedBytes: 384 * mib, memoryLimitBytes: gib, memoryUnlimited: false,
    network: { receivedBytes: 5e9, sentBytes: 1e9 }, blockIo: { readBytes: 2 * mib, writtenBytes: 512 * 1024 }, dataBytes: Math.round(1.5 * gib), uptimeSeconds: 7260 } };
const answer = (desktops, extra = {}) => ({ protocol: 'admin-resources-v1', checkedAt: new Date().toISOString(), server, proxmox: { configured: true, reachable: true }, desktops, nodes: [], ...extra });

// A table of the server part: row key -> [label, value text, note].
export function facts(root, selector) {
  return Object.fromEntries([...root.querySelectorAll(selector + ' tbody tr')].map(tr => [tr.getAttribute('data-fact'), [tr.querySelector('th').textContent, (tr.querySelector('.res-text') ?? tr.querySelector('.res-unknown')).textContent, tr.querySelector('.res-hint')?.textContent ?? '']]));
}
function fixture(t, { desktops = [], resources = undefined, error = '', configured = true } = {}) {
  const previous = { document: globalThis.document, Node: globalThis.Node, location: globalThis.location };
  const window = parseHTML('<!doctype html><html><body></body></html>');
  Object.assign(globalThis, { document: window.document, Node: window.Node, location: new URL('https://resources.fixture.invalid/admin/') });
  t.after(() => Object.assign(globalThis, previous));
  const values = { hosts: { hosts: desktops.map(d => ({ desktopId: d.id, hostname: d.name, version: '1.0.43', connected: true })) }, sessions: { sessions: [] }, hostRequests: { requests: [] }, clientDiagnostics: { events: [] }, resources };
  const calls = [];
  const ctx = {
    state: { user: { id: 'a', username: 'a' }, csrf: 'c', pveBusy: false, pveError: '', ui: {}, onboarding: { complete: true },
      data: { users: [{ username: 'a' }, { username: 'b' }], desktops, assignments: [{ username: 'b', desktop_id: desktops[0]?.id }], audit: [], proxmox: { configured, state: 'ok' }, hostUpdates: { desktops: {} } } },
    value: key => values[key] ?? null,
    resource: key => key === 'resources' && error ? { value: null, error } : values[key] ? { value: values[key] } : null,
    desktopById: id => desktops.find(d => d.id === id),
    pageHeader: ({ title }) => el('header', {}, el('h1', {}, title)),
    hashFor: (page, id) => '#/' + page + (id ? '/' + id : ''),
    link: (text, page, id, attrs = {}) => el('a', { ...attrs, href: '#/' + page + (id ? '/' + id : ''), onclick: event => { event.preventDefault?.(); calls.push(['navigate', page, id]); } }, text),
    navigate: (page, id) => calls.push(['navigate', page, id]), render() {}, refresh: async () => {}, load: async () => {}, loadServerInfo: async () => {},
  };
  const panel = el('main', {}, resourcePanel(ctx)).querySelector('.resource-panel');
  const rows = () => [...panel.querySelectorAll('.res-table tbody tr')];
  return { ctx, panel, rows, calls, window };
}

test('자원 shows the server line, the running desktops sorted by memory then CPU, at most ten, with 모두 보기', t => {
  const desktops = Array.from({ length: 12 }, (_, i) => vm(101 + i));
  const resources = answer(desktops.map((d, i) => row(d.pve_vmid, { memoryUsedBytes: (i % 6) * gib, cpuPercent: i })), { nodes: [{ node: 'pve1', running: 12, vcpusUsed: 3.2, vcpusAssigned: 48, memoryUsedBytes: 30 * gib, memoryTotalBytes: 96 * gib }] });
  const f = fixture(t, { desktops, resources });
  assert.equal(f.panel.querySelector('.panel-head h2').textContent, '자원');
  assert.deepEqual([...f.panel.querySelectorAll('.res-facts thead th')].map(th => th.textContent), ['서버', 'Multitap 컨테이너']);
  const rows = f.rows();
  assert.equal(rows.length, 10, 'at most ten rows');
  // memory 5 GB: i = 5 and 11 (CPU 11 first), then 4 GB: i = 10, 4 ...
  assert.deepEqual(rows.slice(0, 4).map(tr => tr.querySelector('td.is-name').textContent), ['데스크톱 112', '데스크톱 106', '데스크톱 111', '데스크톱 105']);
  assert.deepEqual([...rows[0].querySelectorAll('td')].map(td => td.textContent), ['데스크톱 112', '11%', '5.0 / 8.0 GB', '1시간 0분']);
  assert.equal(f.panel.querySelector('.res-more').textContent, '켜진 데스크톱 12대 중 메모리를 많이 쓰는 10대예요.');
  const all = [...f.panel.querySelectorAll('.panel-head a')].find(a => a.textContent === '모두 보기');
  assert.equal(all.getAttribute('href'), '#/desktops');
  // 노드별 합계: used beside assigned, no capacity.
  assert.equal(f.panel.querySelector('.panel-head h3').textContent, '노드별 합계');
  assert.deepEqual([...f.panel.querySelectorAll('.res-nodes tbody td')].map(td => td.textContent), ['pve1', '12대', '7% (48 vCPU)', '30.0 / 96.0 GB'], 'node CPU as a share of the vCPUs set, like a desktop');
  assert.doesNotMatch(f.panel.textContent, /용량|capacity|남은 메모리/);
});

test('one running desktop: no 노드별 합계, which would repeat its row', t => {
  const desktops = [vm(7)];
  const f = fixture(t, { desktops, resources: answer([row(7)], { nodes: [{ node: 'pve1', running: 1, vcpusUsed: 0.4, vcpusAssigned: 4, memoryUsedBytes: 4 * gib, memoryTotalBytes: 8 * gib }] }) });
  assert.equal(f.rows().length, 1);
  assert.equal(f.panel.querySelector('.res-nodes'), null);
  assert.doesNotMatch(f.panel.textContent, /노드별 합계/);
});
test('90 % memory or CPU is in the warning colour; below is not', t => {
  const desktops = [vm(1), vm(2), vm(3)];
  const f = fixture(t, { desktops, resources: answer([row(1, { memoryUsedBytes: Math.round(7.2 * gib) }), row(2, { cpuPercent: 90 }), row(3, { cpuPercent: 89.4, memoryUsedBytes: Math.round(7.1 * gib) })]) });
  const byName = Object.fromEntries(f.rows().map(tr => [tr.querySelector('td.is-name').textContent, tr]));
  assert.ok(byName['데스크톱 1'].querySelector('td.col-memory .res-value').getAttribute('class').includes('is-warn'), '7.2 of 8 GB is 90 %');
  assert.ok(byName['데스크톱 1'].querySelector('.res-fill').getAttribute('class').includes('is-warn'));
  assert.equal(byName['데스크톱 1'].querySelector('td.col-cpu .is-warn'), null);
  assert.equal(byName['데스크톱 2'].querySelector('td.col-cpu .is-warn').textContent, '90%');
  assert.equal(byName['데스크톱 3'].querySelectorAll('.is-warn').length, 0, '89 % CPU and 88.75 % memory are calm');
});

test('a row opens that desktop\'s window; the name is the link', t => {
  const desktops = [vm(7)];
  const f = fixture(t, { desktops, resources: answer([row(7)]) });
  const [tr] = f.rows();
  tr.querySelector('td.col-up').dispatchEvent(new f.window.Event('click', { bubbles: true }));
  assert.deepEqual(f.calls, [['navigate', 'desktop', 'pve-7']]);
  assert.equal(tr.querySelector('td.is-name a').getAttribute('href'), '#/desktop/pve-7');
});

test('unknown numbers are —, never 0; a desktop not in the console\'s list is left out', t => {
  const f = fixture(t, { desktops: [vm(1), vm(2)], resources: answer([row(1, { cpuPercent: null, memoryUsedBytes: null, memoryTotalBytes: null, uptimeSeconds: null }), row(2), row(99)], { server: { ...server, cpuPercent: null, memoryLimitBytes: null, disk: null, uptimeSeconds: null } }) });
  assert.equal(facts(f.panel, '.res-machine').cpu[1], '14%', 'the old process fields are not what the server part shows');
  const rows = f.rows();
  assert.equal(rows.length, 2);
  assert.deepEqual([...rows[1].querySelectorAll('td')].map(td => td.textContent), ['데스크톱 1', '—', '—', '—'], 'unknown sorts last');
  assert.equal(rows[1].querySelector('.res-bar'), null, 'no bar without numbers');
});

test('empty states: no Proxmox connection, no running desktop, loading and a failed read', t => {
  const none = fixture(t, { configured: false, resources: answer([], { proxmox: { configured: false, reachable: null } }) });
  assert.equal(none.panel.querySelector('.res-empty').textContent, 'Proxmox에 연결하면 데스크톱 자원이 보여요.');
  assert.equal(none.panel.querySelector('.res-table'), null);
  assert.ok(none.panel.querySelector('.res-sections .res-machine') && none.panel.querySelector('.res-sections .res-service'), 'the server part stays');
  assert.equal(none.panel.querySelector('.panel-head a'), null, 'nothing to see in full');
  const off = fixture(t, { desktops: [vm(1)], resources: answer([]) });
  assert.equal(off.panel.querySelector('.res-empty').textContent, '켜진 데스크톱이 없어요.');
  assert.equal(off.panel.querySelector('.res-nodes'), null);
  const loading = fixture(t, { desktops: [vm(1)] });
  assert.ok(loading.panel.querySelector('.is-loading'));
  const failed = fixture(t, { desktops: [vm(1)], error: '서버에 연결하지 못했어요.' });
  assert.equal(failed.panel.querySelector('.empty strong').textContent, '자원 정보를 확인하지 못했어요');
});

test('홈 places 자원 after 지금 할 일 and before 지금 접속 중', t => {
  const desktops = [vm(1)];
  const f = fixture(t, { desktops, resources: answer([row(1)]) });
  const root = el('main', {}, render(f.ctx));
  const titles = [...root.querySelectorAll('section.panel > .panel-head h2')].map(h => h.firstChild.textContent);
  assert.deepEqual(titles, ['지금 할 일', '자원', '지금 접속 중']);
});

test('formatting: how long, and the order', () => {
  assert.deepEqual([upFor(30), upFor(60), upFor(3 * 3600 + 5 * 60), upFor(86400 * 3 + 4 * 3600 + 59), upFor(null), upFor(-1)], ['1분 미만', '1분', '3시간 5분', '3일 4시간', '—', '—']);
  const name = r => r.id;
  assert.deepEqual(sortRows([{ id: 'b', memoryUsedBytes: 1, cpuPercent: 1 }, { id: 'a', memoryUsedBytes: 1, cpuPercent: 1 }, { id: 'c', memoryUsedBytes: null, cpuPercent: 99 }, { id: 'd', memoryUsedBytes: 2, cpuPercent: 0 }, { id: 'e', memoryUsedBytes: 1, cpuPercent: 5 }], name).map(name), ['d', 'e', 'a', 'b', 'c']);
});

test('서버 and Multitap 컨테이너 (Docker): 항목 | 값, numbers with units, the name and network under the container', t => {
  const f = fixture(t, { desktops: [vm(1)], resources: answer([row(1)]) });
  assert.deepEqual(facts(f.panel, '.res-machine'), {
    kernel: ['커널', '6.8.12-4-pve', ''], uptime: ['켜진 시간', '12일 5시간', ''], cpu: ['CPU', '14%', '8코어'],
    memory: ['메모리', '6.0 / 8.0 GB', '75%'], disk: ['디스크', '22.7 / 64.0 GB', '35%'] }, 'no name or network row: inside Docker they are the container\'s');
  assert.deepEqual(facts(f.panel, '.res-service'), {
    name: ['이름', '3f9c2a1b7d4e', ''], state: ['상태', '실행 중', ''], cpu: ['CPU', '3.2%', 'CPU 한도 2코어 기준'], memory: ['메모리', '384 MB / 1.0 GB', '38%'],
    network: ['네트워크', '수신 4.7 GB · 송신 954 MB', ''], io: ['블록 I/O', '읽기 2 MB · 쓰기 512 KB', ''], data: ['데이터 크기', '1.5 GB', ''], uptime: ['켜진 시간', '2시간 1분', ''] });
  assert.equal(f.panel.querySelector('.res-service tr[data-fact=memory]').getAttribute('title'), '페이지 캐시를 뺀 값이에요 (docker stats와 같은 방식).');
  assert.equal(f.panel.querySelector('.res-service tr[data-fact=network]').getAttribute('title'), '컨테이너가 시작된 뒤 누적');
  // One thin bar per share (CPU, memory, disk), none for the rest; no icons, no badges.
  assert.deepEqual([...f.panel.querySelectorAll('.res-machine tr')].filter(tr => tr.querySelector('.res-gauge')).map(tr => tr.getAttribute('data-fact')), ['cpu', 'memory', 'disk']);
  assert.deepEqual([...f.panel.querySelectorAll('.res-service tr')].filter(tr => tr.querySelector('.res-gauge')).map(tr => tr.getAttribute('data-fact')), ['cpu', 'memory']);
  assert.equal(f.panel.querySelectorAll('.res-sections .icon, .res-sections .pill, .res-sections .badge').length, 0);
  assert.ok(f.rows().length === 1, 'the desktops part is still there');
});

test('Multitap 서비스 (systemd): the machine has the name and the network; no memory limit says 제한 없음, without a bar', t => {
  const systemd = { ...server, deploymentKind: 'systemd', machine: { ...server.machine, name: 'vdi-broker', network: { receivedBytes: 3 * 1024 ** 4, sentBytes: 900 * 1024 } },
    service: { ...server.service, kind: 'service', name: null, network: null, cpuBasis: 'cores', cpuLimitCores: null, memoryUsedBytes: 200 * mib, memoryLimitBytes: null, memoryUnlimited: true, blockIo: null } };
  const f = fixture(t, { desktops: [vm(1)], resources: answer([row(1)], { server: systemd }) });
  assert.deepEqual([...f.panel.querySelectorAll('.res-facts thead th')].map(th => th.textContent), ['서버', 'Multitap 서비스']);
  const machine = facts(f.panel, '.res-machine'), service = facts(f.panel, '.res-service');
  assert.deepEqual(Object.keys(machine), ['name', 'kernel', 'uptime', 'cpu', 'memory', 'disk', 'network']);
  assert.deepEqual([machine.name, machine.network], [['이름', 'vdi-broker', ''], ['네트워크', '수신 3.0 TB · 송신 900 KB', '']]);
  assert.deepEqual(Object.keys(service), ['state', 'cpu', 'memory', 'io', 'data', 'uptime']);
  assert.deepEqual([service.cpu, service.memory, service.io], [['CPU', '3.2%', '전체 코어 8개 기준'], ['메모리', '200 MB', '제한 없음'], ['블록 I/O', '확인할 수 없어요', '']]);
  assert.equal(f.panel.querySelector('.res-service tr[data-fact=memory] .res-gauge'), null);
  assert.equal(f.panel.querySelector('.res-service tr[data-fact=memory]').getAttribute('title'), 'Multitap 서비스와 그 하위 프로세스가 쓰는 양이에요.');
  // Without its cgroup the service is the server process, and says so.
  const plain = fixture(t, { desktops: [vm(1)], resources: answer([row(1)], { server: { ...systemd, service: { ...systemd.service, measuredBy: 'process', cpuBasis: 'process', memoryUnlimited: null, memoryUsedBytes: 187 * mib } } }) });
  assert.deepEqual(facts(plain.panel, '.res-service').cpu, ['CPU', '3.2%', '서버 프로세스 기준']);
  assert.deepEqual(facts(plain.panel, '.res-service').memory, ['메모리', '187 MB', '']);
});

test('unknown is "확인할 수 없어요" in words; another platform (all null) and an older answer without the new fields', t => {
  const empty = { name: null, kernel: null, uptimeSeconds: null, cpuPercent: null, cpuCount: null, memoryUsedBytes: null, memoryTotalBytes: null, disk: null, network: null };
  const service = { kind: 'service', state: 'running', measuredBy: 'process', name: null, cpuPercent: null, cpuBasis: 'process', cpuLimitCores: null, memoryUsedBytes: null, memoryLimitBytes: null, memoryUnlimited: null, network: null, blockIo: null, dataBytes: null, uptimeSeconds: null };
  const f = fixture(t, { desktops: [vm(1)], resources: answer([row(1)], { server: { ...server, disk: null, deploymentKind: 'unknown', machine: empty, service } }) });
  for (const [key, [, text]] of Object.entries(facts(f.panel, '.res-machine'))) assert.equal(text, '확인할 수 없어요', 'machine ' + key);
  for (const [key, [, text]] of Object.entries(facts(f.panel, '.res-service'))) assert.equal(text, key === 'state' ? '실행 중' : '확인할 수 없어요', 'service ' + key);
  assert.equal(f.panel.querySelectorAll('.res-sections .res-gauge').length, 0, 'no bar without a number');
  assert.doesNotMatch(f.panel.querySelector('.res-sections').textContent, /—|\b0%/, 'never a dash or 0');
  // An answer from before the machine and service fields: the old disk still shows, the rest is unknown.
  const old = fixture(t, { desktops: [vm(1)], resources: answer([row(1)], { server: { cpuPercent: 1, cpuCount: 4, memoryRssBytes: mib, memoryLimitBytes: null, disk: { freeBytes: 48 * gib, totalBytes: 64 * gib }, uptimeSeconds: 60 } }) });
  assert.deepEqual(facts(old.panel, '.res-machine').disk, ['디스크', '16.0 / 64.0 GB', '25%']);
  assert.equal(facts(old.panel, '.res-machine').cpu[1], '확인할 수 없어요');
});

test('a share is neutral up to 80 %, amber above 80 %, red above 95 %', t => {
  assert.deepEqual([level(null), level(12), level(80), level(80.1), level(95), level(95.5), level(100)], ['', '', '', 'is-high', 'is-high', 'is-full', 'is-full']);
  const hot = { ...server, machine: { ...server.machine, cpuPercent: 85, memoryUsedBytes: Math.round(7.7 * gib), disk: { usedBytes: Math.round(40 * gib), freeBytes: 24 * gib, totalBytes: 64 * gib } } };
  const f = fixture(t, { desktops: [vm(1)], resources: answer([row(1)], { server: hot }) });
  const tone = key => [f.panel.querySelector(`.res-machine tr[data-fact=${key}] .res-text`).getAttribute('class'), f.panel.querySelector(`.res-machine tr[data-fact=${key}] .res-level`).getAttribute('class')];
  assert.deepEqual(tone('cpu'), ['res-text is-high', 'res-level is-high']);
  assert.deepEqual(tone('memory'), ['res-text is-full', 'res-level is-full'], '96 %');
  assert.deepEqual(tone('disk'), ['res-text', 'res-level is-calm'], '62.5 %');
  assert.equal(f.panel.querySelector('.res-machine tr[data-fact=disk] .res-level').getAttribute('width'), '62.5');
});

test('amounts and pairs', () => {
  assert.deepEqual([amount(0), amount(512 * 1024), amount(212 * mib), amount(3.14 * gib), amount(1.25 * 1024 ** 4), amount(null), amount(-1)], ['0 KB', '512 KB', '212 MB', '3.1 GB', '1.3 TB', null, null]);
  assert.deepEqual([pair(6 * gib, 8 * gib), pair(384 * mib, gib), pair(null, gib), pair(gib, null)], ['6.0 / 8.0 GB', '384 MB / 1.0 GB', null, null]);
});