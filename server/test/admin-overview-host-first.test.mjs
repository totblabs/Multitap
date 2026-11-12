import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseHTML } from 'linkedom';
import { el } from '../admin/js/dom.js';
import { render, todoCount } from '../admin/js/overview.js';

// Only registered synthetic inventory is supplied, as in the production API.
// Real 홈 DOM is constructed; no login, server, network or write runs.
// 홈 has no node board any more: a registered desktop is counted in the four
// stat tiles and named only when it needs something (지금 할 일).
function fixture(t, { desktops = [], configured = false, busy = false, pveError = '', hostsError = '' } = {}) {
  const previous = { document: globalThis.document, Node: globalThis.Node, location: globalThis.location };
  const window = parseHTML('<!doctype html><html><body></body></html>');
  Object.assign(globalThis, { document: window.document, Node: window.Node, location: new URL('https://overview.fixture.invalid/admin/') });
  t.after(() => Object.assign(globalThis, previous));
  const values = {
    hosts: { hosts: desktops.map(d => ({ desktopId: d.id, hostname: d.name, version: '1.0.12', connected: !!d.host?.connected, ...(d.hostDiagnostics ? { diagnostics: d.hostDiagnostics } : {}) })) },
    sessions: { sessions: [] }, hostRequests: { requests: [] }, clientDiagnostics: { events: [] },
  };
  const calls = [], dialogs = [];
  const ctx = {
    state: {
      user: { id: 'fixture-admin', username: 'fixture-admin' }, csrf: 'fixture-csrf',
      pveBusy: busy, pveError, ui: {}, onboarding: { complete: true },
      data: { users: [], desktops, assignments: [], audit: [], proxmox: { configured, state: pveError ? 'error' : 'ok' }, hostUpdates: { desktops: {} } },
    },
    value: key => hostsError && key === 'hosts' ? null : values[key],
    resource: key => hostsError && key === 'hosts' ? { value: null, error: hostsError } : { value: values[key] },
    desktopById: id => desktops.find(d => d.id === id),
    pageHeader: ({ title, description, actions }) => el('header', {}, el('h1', {}, title), el('p', {}, description), actions),
    hashFor: (page, id) => '#/' + page + (id ? '/' + id : ''),
    link: (text, page, id, attrs = {}) => el('a', { ...attrs, href: '#/' + page + (id ? '/' + id : '') }, text),
    render() {}, navigate: page => calls.push(['navigate', page]),
    refresh: async () => calls.push(['refresh']), load: async name => calls.push(['load', name]), loadServerInfo: async () => calls.push(['server-info']),
    api: async (path, method) => { calls.push(['api', method, path]); return {}; },
    pageAction: () => ({ current: () => true, check() {} }),
    modal: (title, fields, action, options = {}) => { dialogs.push({ title, save: options.save }); return dialogs.length; },
    modalCurrent: () => false, productRelease: async () => null, notice() {},
  };
  const root = el('main', {}, render(ctx));
  const tile = label => [...root.querySelectorAll('nav.stat-tiles a.stat-tile')].find(node => node.querySelector('span').textContent === label)?.querySelector('strong').textContent;
  const button = (scope, label) => [...scope.querySelectorAll('button')].find(node => node.textContent.trim() === label);
  const step = label => [...root.querySelectorAll('ol.first-steps > li')].find(node => node.querySelector('span').textContent === label);
  return { root, calls, dialogs, ctx, tile, button, step };
}

const pc = { id: 'registered-pc', kind: 'pc', name: 'REGISTERED-OFFICE-PC', localPcApproved: true, host: { connected: true, version: '1.0.12' } };
const vm = { id: 'registered-vm', kind: 'vm', name: 'REGISTERED-VM', pve_vmid: 247, pve_node: 'fixture-node', pve_power_status: 'stopped', host: { connected: false } };
const powerWords = /다시 시작|켜기|끄기|꺼짐|실행 중/;

test('registered local PC is reachable without Proxmox and has no VM power fiction', t => {
  const f = fixture(t, { desktops: [pc] });
  assert.equal(f.root.querySelector('h1').textContent, '홈');
  assert.equal(f.tile('접속할 수 있는 데스크톱'), '1 / 1', 'the registered local PC is counted as reachable');
  assert.equal(f.tile('접속 불가'), '0');
  assert.equal(f.root.querySelector('.todo-panel .todo-calm strong').textContent, '지금 할 일이 없어요');
  assert.equal(f.root.querySelector('[data-todo]'), null);
  assert.equal(todoCount(f.ctx), 0);
  assert.doesNotMatch(f.root.textContent, /Proxmox를 연결하세요|Proxmox 연결 오류|전체 VM|QEMU/);
  assert.doesNotMatch(f.root.textContent, powerWords);
  // A registered desktop already answers the first three steps: Proxmox is not asked for.
  assert.ok(f.step('Proxmox 연결').getAttribute('class').includes('is-done'));
  assert.ok(f.step('데스크톱 승인').getAttribute('class').includes('is-done'));
  assert.equal(f.step('사용자 만들기').getAttribute('aria-current'), 'step');
  assert.equal(f.root.querySelector('a[href="#/proxmox"]'), null);
  assert.deepEqual(f.calls, []);
});

test('a local PC that does not answer is 응답 없음, never unreachable-with-restart; a display problem names it without a power button', t => {
  const silent = fixture(t, { desktops: [{ ...pc, host: { connected: false } }] });
  assert.equal(silent.tile('접속할 수 있는 데스크톱'), '0 / 1');
  assert.equal(silent.tile('접속 불가'), '0', 'Proxmox does not report a PC; a silent one is most likely off');
  assert.equal(silent.root.querySelector('[data-todo=unreachable]'), null);
  assert.doesNotMatch(silent.root.textContent, powerWords);

  const broken = fixture(t, { desktops: [{ ...pc, hostDiagnostics: { display: { rebootRequired: true } } }] });
  assert.equal(broken.tile('접속 불가'), '1');
  const group = broken.root.querySelector('[data-todo=unreachable]');
  assert.equal(group.querySelector('.todo-title').textContent, '접속할 수 없는 데스크톱 1대');
  const row = group.querySelector('.todo-row');
  assert.equal(row.querySelector('strong').textContent, pc.name);
  assert.match(row.textContent, /Windows를 다시 시작해야 해요/);
  assert.equal(row.querySelectorAll('button').length, 0, 'a local PC has no Proxmox power control');
  assert.equal(row.querySelector('a[href="#/desktop/registered-pc"]').textContent, '데스크톱 열기');
  assert.deepEqual([...silent.calls, ...broken.calls], []);
});

for (const state of [{ busy: true }, { pveError: 'fixture Proxmox unavailable' }]) {
  test('local PC stays counted beside a VM during ' + (state.busy ? 'Proxmox refresh' : 'Proxmox failure'), t => {
    const f = fixture(t, { desktops: [vm, pc], configured: true, ...state });
    assert.equal(f.tile('접속할 수 있는 데스크톱'), '1 / 2', 'the PC is reachable whatever Proxmox says; the stopped VM stays registered');
    assert.equal(f.tile('접속 불가'), '0');
    // A Proxmox failure is its own item; it does not turn the PC into a problem.
    const titles = [...f.root.querySelectorAll('.todo-panel .todo-title')].map(node => node.textContent);
    assert.deepEqual(titles, state.pveError ? ['Proxmox 연결 오류'] : []);
    assert.equal(f.root.querySelector('[data-todo=unreachable]'), null);
    assert.deepEqual(f.calls, []);
  });
}

test('configured registered VM: stopped is only counted; running without a Host answer is named with the exact detail link and a restart that asks first', t => {
  const stopped = fixture(t, { desktops: [vm], configured: true });
  assert.equal(stopped.tile('접속할 수 있는 데스크톱'), '0 / 1', 'a registered VM is counted even when it is off');
  assert.equal(stopped.tile('접속 불가'), '0');
  assert.equal(stopped.root.querySelector('[data-todo=unreachable]'), null);
  assert.deepEqual(stopped.calls, []);

  const f = fixture(t, { desktops: [{ ...vm, pve_power_status: 'running' }], configured: true });
  assert.equal(f.tile('접속할 수 있는 데스크톱'), '0 / 1');
  assert.equal(f.tile('접속 불가'), '1');
  assert.equal(f.root.querySelector('header p')?.textContent ?? '', '', 'no subtitle that counts what 지금 할 일 lists');
  assert.equal(todoCount(f.ctx), 1);
  const row = f.root.querySelector('[data-todo=unreachable] .todo-row');
  assert.equal(row.querySelector('strong').textContent, vm.name);
  assert.match(row.textContent, /응답 없음/);
  assert.equal(row.querySelector('a[href="#/desktop/registered-vm"]').textContent, '데스크톱 열기');
  assert.equal(row.querySelectorAll('a').length, 1);
  f.button(row, '다시 시작').click();
  assert.deepEqual(f.dialogs, [{ title: 'REGISTERED-VM 다시 시작', save: '다시 시작' }]);
  assert.deepEqual(f.calls, [], 'opening the confirmation sends nothing');
});

test('an unknown Host list leaves the counts unknown instead of counting every desktop', t => {
  const f = fixture(t, { desktops: [vm, pc], configured: true, hostsError: 'fixture hosts unavailable' });
  assert.equal(f.tile('접속할 수 있는 데스크톱'), '—');
  assert.equal(f.tile('접속 불가'), '—');
  assert.equal(f.root.querySelector('[data-todo=unreachable]'), null);
  assert.equal(f.root.querySelector('ol.first-steps'), null, 'no step is claimed done or missing while registration is unknown');
  assert.doesNotMatch(f.root.textContent, /REGISTERED-VM|REGISTERED-OFFICE-PC/);
  assert.deepEqual(f.calls, []);
});

for (const configured of [false, true]) {
  test('empty home starts with Host enrollment without automatic VM-discovery claims, configured=' + configured, async t => {
    const f = fixture(t, { configured });
    assert.equal(f.root.querySelector('h1').textContent, '시작하기');
    assert.deepEqual([...f.root.querySelectorAll('ol.first-steps > li > span:not(.visually-hidden)')].map(node => node.textContent),
      ['서버 접속 설정', 'Proxmox 연결', 'Windows에 Host 설치', '데스크톱 승인', '사용자 만들기', '데스크톱 배정']);
    assert.equal(f.root.querySelector('ol.first-steps > li[aria-current=step] > span').textContent, configured ? 'Windows에 Host 설치' : 'Proxmox 연결');
    assert.equal(f.root.querySelector('nav.stat-tiles'), null, 'nothing is counted before the first desktop');
    assert.doesNotMatch(f.root.textContent, /전체 VM|QEMU|자동으로 불러/);
    assert.equal(f.root.querySelector('a[href^="#/desktop/"]'), null);
    // Proxmox comes first: the server opens
    // nothing else until it is connected, so the step offers no way to skip
    // it; the Host installation guide is offered once it is connected.
    assert.equal(!!f.root.querySelector('a[href="#/proxmox"]'), !configured);
    assert.doesNotMatch(f.root.textContent, /건너뛰/);
    const guide = f.button(f.root, 'Host 설치 안내');
    assert.equal(!!guide, configured);
    if (configured) {
      guide.click();
      for (let turn = 0; turn < 5; turn++) await new Promise(resolve => setImmediate(resolve));
      assert.deepEqual(f.dialogs, [{ title: '데스크톱 추가', save: '닫기' }]);
      assert.deepEqual(f.calls, [], 'the installation guide is read-only');
    }
  });
}

// A strip drawn before the server's address is read would start on a later step and jump back.
test('시작하기 waits for the first read of the server access settings, then starts on the step that is really next', t => {
  const f = fixture(t, { configured: true });
  const strip = () => el('main', {}, render(f.ctx)).querySelector('ol.first-steps');
  const now = () => strip().querySelector('li[aria-current=step] > span').textContent;
  const resource = f.ctx.resource, value = f.ctx.value;
  // The read is out: nothing is claimed yet.
  f.ctx.resource = key => key === 'serverAccess' ? { value: null, error: '', loading: true, at: 0 } : resource(key);
  f.ctx.value = key => key === 'serverAccess' ? null : value(key);
  assert.equal(strip(), null);
  // A fresh native install has no address yet: the address is the next step, Proxmox (connected) is ticked.
  const fresh = { protocol: 'server-access-v1', revision: 'bootstrap', applied: null, pending: null };
  f.ctx.resource = key => key === 'serverAccess' ? { value: fresh, error: '', loading: false, at: 1 } : resource(key);
  f.ctx.value = key => key === 'serverAccess' ? fresh : value(key);
  assert.equal(now(), '서버 접속 설정');
  // A read that failed claims nothing missing (as before).
  f.ctx.resource = key => key === 'serverAccess' ? { value: null, error: '서버에 연결하지 못했어요.', loading: true, at: 1 } : resource(key);
  f.ctx.value = key => key === 'serverAccess' ? null : value(key);
  assert.equal(now(), 'Windows에 Host 설치');
});
