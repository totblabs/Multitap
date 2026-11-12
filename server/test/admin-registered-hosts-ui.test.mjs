import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseHTML } from 'linkedom';
import { readFile } from 'node:fs/promises';
import { el } from '../admin/js/dom.js';
import { detail, list, workspaceNeeds } from '../admin/js/desktops.js';

// Real modules and DOM, with memory-only inventory/API collaborators.
// No Host registration, network, VM or file operation is performed.
// There is no separate Host page or table: registered Hosts are the rows of
// 데스크톱, and a desktop is deleted (its registration removed) with 삭제 at the far left of its window's
// bar. Assignments have no draft: 데스크톱 배정 saves at once.
const hostId = '2d408afb-954a-44bc-adde-799e75193ed3';
const usersUrl = new URL('../admin/js/users.js', import.meta.url);
const usersSource = (await readFile(usersUrl, 'utf8')).replace(/from '(\.[^']+)'/g, (_, path) => `from '${new URL(path, usersUrl).href}'`);
const users = await import('data:text/javascript;base64,' + Buffer.from(usersSource + '\nexport { assignDesktops, desktopsPanel };').toString('base64'));
function fixture(t) {
  const old = { document: globalThis.document, Node: globalThis.Node };
  const window = parseHTML('<!doctype html><html><body></body></html>');
  globalThis.document = window.document; globalThis.Node = window.Node;
  t.after(() => Object.assign(globalThis, old));
  const registered = { id: 'pve-105', kind: 'vm', name: 'REGISTERED', pve_vmid: 105, pve_available: 1, pve_os_family: 'windows', pve_power_status: 'stopped', host: { connected: false } };
  const hidden = { ...registered, id: 'pve-100', name: 'UNREGISTERED_PRIVATE', pve_vmid: 100, host: { connected: true } };
  const values = { hosts: { hosts: [{ desktopId: registered.id, hostId, hostname: 'WORK-PC', version: '1.0.12' }] }, sessions: { sessions: [] }, hostRequests: { requests: [] } };
  const entries = { hosts: { value: values.hosts } }, writes = [], dialogs = [], notices = [], loads = [], navigations = [];
  const ctx = {
    state: { user: { id: 'admin-a', username: 'admin-a' }, csrf: 'csrf-a', pveLoaded: true, pveBusy: false, pveError: '', assignmentSaving: false, route: { page: 'user' },
      // A remembered view from the old console (scope/os/power/host/tab/kind) and a filter that no longer exists.
      ui: { desktops: { scope: 'all', query: '', os: 'all', power: 'all', host: 'none', tab: 'summary', kind: 'all', filter: 'none', byNode: false, selected: hidden.id } },
      data: { desktops: [hidden, registered], users: [], assignments: [], audit: [], proxmox: { configured: true }, hostUpdates: { desktops: {} } } },
    value: key => values[key], resource: key => entries[key] ?? { value: values[key] },
    desktopById: id => ctx.state.data.desktops.find(d => d.id === id),
    userByName: name => ctx.state.data.users.find(u => u.username === name) ?? null,
    pageHeader: ({ title, description, actions = [] }) => el('header', {}, el('h1', {}, title), el('p', {}, description), ...actions),
    searchInput: () => el('input'), link: (text, route, id, attrs = {}) => el('a', { ...attrs, href: '#' + route + '/' + (id ?? '') }, text),
    hashFor: (route, id) => '#' + route + '/' + (id ?? ''),
    api: async (path, method, body) => { writes.push({ path, method, body }); return {}; },
    modal: (title, fields, action, options) => { dialogs.push({ title, fields, action, options }); return dialogs.length; },
    confirmAction: (title, description, action, save, danger, extra = {}) => ctx.modal(title, [], action, { description, save, danger, ...extra }),
    pageAction: () => { const actor = ctx.state.user, csrf = ctx.state.csrf; const current = () => ctx.state.user === actor && ctx.state.csrf === csrf; return { current, check() { if (!current()) throw Object.assign(new Error('session changed'), { code: 'admin_session_changed' }); } }; },
    notice: (...args) => notices.push(args), load: async (...args) => loads.push(args), refresh: async () => {}, render() {}, navigate: (...args) => navigations.push(args), syncProxmox() {}, scheduleHostUpdatePoll() {},
  };
  // The registered desktop's own window: where its registration is removed.
  const render = () => el('main', {}, detail(ctx, { page: 'desktop', id: registered.id }));
  const find = (root, text) => [...root.querySelectorAll('button')].find(node => node.textContent === text);
  const removal = root => root.querySelector('.win-foot button.dt-delete') ?? undefined;
  const openDelete = () => {
    const page = render(), control = removal(page);
    assert.ok(control, 'the desktop offers 삭제'); assert.equal(control.textContent, '삭제'); assert.match(control.getAttribute('class'), /\bdanger\b/);
    assert.equal(control.closest('.win-actions').firstElementChild, control, '삭제 stands first, at the far left of the bar');
    assert.equal(page.querySelector('details.dt-more'), null, 'nothing about deleting is folded in 자세히 any more');
    control.click(); return dialogs.at(-1);
  };
  return { ctx, values, entries, writes, dialogs, notices, loads, navigations, registered, hidden, render, find, removal, openDelete };
}
const listed = root => [...root.querySelectorAll('a.vm-list-item')].map(node => node.getAttribute('data-focus'));

test('desktop list excludes an unregistered runtime-connected VM and offers no unregistered status', t => {
  const f = fixture(t), root = el('main', {}, list(f.ctx));
  assert.deepEqual(listed(root), ['vm-pick-pve-105']);
  assert.match(root.textContent, /REGISTERED/); assert.doesNotMatch(root.textContent, /UNREGISTERED_PRIVATE|미등록/);
  const choices = [...root.querySelectorAll('select[data-focus=desktops-state] option')];
  assert.deepEqual(choices.map(node => node.getAttribute('value')), ['all', 'bad', 'live', 'ok', 'off']);
  assert.deepEqual(choices.map(node => node.textContent), ['전체 1', '접속 불가 0', '접속 중 0', '접속 가능 0', '꺼짐 1']);
  assert.equal(root.querySelector('option[value=none],[data-chip=none]'), null, 'no choice shows unregistered desktops'); assert.equal(root.querySelectorAll('select').length, 1, 'the state is the one select; the old OS, power and Host selects are gone');
  assert.equal(root.querySelector('.list-count>span').textContent, '총 1대 · 접속 불가 0 · 접속 중 0 · 접속 가능 0 · 꺼짐 1'); assert.equal(root.querySelector('.list-count>small').textContent, '정렬: 문제 먼저');
});
test('remembered all scope and explicit unregistered deep link expose no detail or diagnostic reads', t => {
  const f = fixture(t), root = el('main', {}, detail(f.ctx, { id: f.hidden.id }));
  assert.equal(root.querySelector('[data-desktop-scope=all]'), null);
  assert.equal(root.querySelector('.dt-main'), null);
  assert.match(root.textContent, /데스크톱을 찾지 못했어요/); assert.doesNotMatch(root.textContent, /UNREGISTERED_PRIVATE/);
  assert.equal(root.querySelector('button.dt-delete'), null);
  assert.ok(!workspaceNeeds(f.ctx, { id: f.hidden.id }).some(key => key.includes(f.hidden.id)));
  // Without a deep link nothing is open: the list reads no desktop, and the remembered selection of an unregistered one is not followed.
  assert.deepEqual(workspaceNeeds(f.ctx, {}).filter(key => key.includes(':')), []);
  assert.deepEqual(workspaceNeeds(f.ctx, { id: f.registered.id }).filter(key => key.includes(':')), ['diagnostics:pve-105', 'clientDiagnostics:pve-105'], 'the window of a registered desktop reads that desktop');
});
test('a stale remembered filter normalizes to all registered entries, and a stale remembered selection opens nothing', t => {
  const f = fixture(t), root = el('main', {}, list(f.ctx));
  assert.equal(f.ctx.state.ui.desktops.filter, 'all');
  assert.equal(root.querySelector('select[data-focus=desktops-state] option[value=all]').hasAttribute('selected'), true);
  assert.deepEqual(listed(root), ['vm-pick-pve-105']);
  assert.equal(root.querySelector('.dt-main'), null, 'the list opens no window on its own'); assert.equal(root.querySelector('[aria-current]'), null);
  assert.equal(el('main', {}, detail(f.ctx, { id: f.registered.id })).querySelector('.dt-main .dt-head h2').textContent, 'REGISTERED');
  assert.doesNotMatch(root.textContent, /UNREGISTERED_PRIVATE|미등록 Windows VM/);
});
test('Host load error suppresses cached rows and mutation controls', t => {
  const f = fixture(t); f.entries.hosts.error = 'offline fixture';
  for (const root of [el('main', {}, list(f.ctx)), f.render()]) {
    assert.match(root.textContent, /Host 등록 정보를 확인하지 못했어요/);
    assert.doesNotMatch(root.textContent, /REGISTERED|UNREGISTERED_PRIVATE/);
    assert.deepEqual(listed(root), []); assert.equal(root.querySelector('.vm-workspace'), null);
    assert.equal(f.removal(root), undefined);
    assert.deepEqual([...root.querySelectorAll('main > :not(header) button')].map(node => node.textContent), ['다시 시도'], 'only the retry of the registration read is offered');
  }
  assert.deepEqual(workspaceNeeds(f.ctx, { id: f.registered.id }).filter(key => key.includes(':')), [], 'no per-desktop read while registration is unknown');
});
test('registration removal requires confirmation and sends exact captured identity only once', async t => {
  const f = fixture(t), form = f.openDelete();
  assert.equal(f.writes.length, 0);
  assert.equal(form.title, '데스크톱 삭제'); assert.equal(form.options.save, '삭제');
  // The dialog says what goes (assignments and permissions) and what stays, and the name is typed to confirm.
  assert.equal(form.options.description, 'REGISTERED: Multitap 목록에서 지워요.\n배정된 사용자는 없어요.\nVM과 파일, 사용자 계정은 그대로예요.\nHost 프로그램은 Windows에 그대로 남아요. 다시 쓰려면 Host에서 연결을 요청하고 승인하면 돼요.');
  assert.deepEqual(form.options.confirmText, { expected: 'REGISTERED', label: '확인을 위해 데스크톱 이름을 입력하세요: REGISTERED' });
  assert.doesNotMatch(form.options.description, /배정은 유지|배정.*유지돼요/); assert.equal(form.options.danger, true);
  await form.action({ expectedHostId: 'injected', confirm: false });
  assert.deepEqual(f.writes, [{ path: '/desktops/pve-105/host', method: 'DELETE', body: { expectedHostId: hostId, confirm: true } }]);
  assert.ok(f.loads.some(([key]) => key === 'hosts'));
  assert.deepEqual(f.notices.at(-1), ['REGISTERED: 삭제했어요.', false]);
  assert.deepEqual(f.navigations, [], 'another page stays where it is');
});
test('removing the desktop that is open returns to the desktop list', async t => {
  const f = fixture(t); f.ctx.state.route = { page: 'desktop', id: f.registered.id };
  await f.openDelete().action({});
  assert.deepEqual(f.navigations, [['desktops']]); assert.equal(f.writes.length, 1);
});
test('registration removal names how many assigned users and permissions go with the Host', async t => {
  const f = fixture(t);
  f.ctx.state.data.assignments.push({ username: 'alice', desktop_id: f.registered.id }, { username: 'bob', desktop_id: f.registered.id }, { username: 'alice', desktop_id: f.hidden.id });
  f.ctx.api = async (path, method, body) => { f.writes.push({ path, method, body }); return { ok: true, desktopId: f.registered.id, hostId, removedAssignments: 2 }; };
  const form = f.openDelete();
  assert.match(form.options.description, /^REGISTERED: Multitap 목록에서 지워요\.\n배정된 사용자 2명과 권한도 함께 지워져요\.\nVM과 파일, 사용자 계정은 그대로예요\.\n/);
  await form.action({});
  assert.deepEqual(f.notices.at(-1), ['REGISTERED: 삭제했어요.\n배정 2건도 함께 지웠어요.', false]);
});
test('removing a local PC says the PC and its files stay', t => {
  const f = fixture(t); Object.assign(f.registered, { kind: 'pc', localPcApproved: true, pve_vmid: null });
  const form = f.openDelete();
  assert.match(form.options.description, /\n로컬 PC와 파일, 사용자 계정은 그대로예요\.\nHost 프로그램은 Windows에 그대로 남아요\./);
});
test('registration replaced or mutated after dialog open cannot retarget deletion', async t => {
  const f = fixture(t), form = f.openDelete();
  f.values.hosts.hosts[0].hostId = 'b4cba48a-ebc4-445f-af99-79c6b866d185';
  await assert.rejects(form.action({}), /등록.*바뀌|다시.*확인/); assert.equal(f.writes.length, 0);
});
test('administrator session changed after dialog open prevents deletion', async t => {
  const f = fixture(t), form = f.openDelete(); f.ctx.state.csrf = 'csrf-b';
  await assert.rejects(form.action({})); assert.equal(f.writes.length, 0);
});
test('missing registration identity cannot open actionable removal', t => {
  const f = fixture(t); delete f.values.hosts.hosts[0].hostId;
  const control = f.removal(f.render());
  assert.ok(!control || control.disabled); assert.equal(f.dialogs.length, 0);
  assert.equal(control?.title, 'Host 등록 정보를 새로고침한 뒤 다시 선택하세요.', 'the disabled control says why');
  control.click();
  assert.equal(f.dialogs.length, 0, 'even a forced press opens no confirmation'); assert.deepEqual(f.writes, []);
});
test('in-flight removal does not duplicate request and backend busy remains an error', async t => {
  const f = fixture(t); let reject; const pending = new Promise((_, r) => { reject = r; });
  f.ctx.api = async (path, method, body) => { f.writes.push({ path, method, body }); return pending; };
  const form = f.openDelete(), first = form.action({});
  await form.action({}); assert.equal(f.writes.length, 1);
  reject(Object.assign(new Error('disconnect first'), { code: 'host_in_use' }));
  await assert.rejects(first, /disconnect first/); assert.equal(f.writes.length, 1); assert.equal(f.notices.length, 0);
});

test('late registration removal response cannot announce success in a newer administrator session', async t => {
  const f = fixture(t); let resolve;
  f.ctx.api = (path, method, body) => { f.writes.push({ path, method, body }); return new Promise(r => { resolve = r; }); };
  const form = f.openDelete(), pending = form.action({});
  f.ctx.state.csrf = 'csrf-new'; resolve({ ok: true });
  await assert.rejects(pending, /session changed/);
  assert.equal(f.writes.length, 1); assert.deepEqual(f.notices, []); assert.deepEqual(f.loads, []); assert.deepEqual(f.navigations, []);
});

// 사용자 > 쓸 수 있는 데스크톱 and its 데스크톱 배정 dialog.
const account = f => { const user = { id: 'user-fixture', username: 'fixture-user', disabled: false }; f.ctx.state.data.users.push(user); return user; };
const openAssign = (f, user) => { users.assignDesktops(f.ctx, user); const form = f.dialogs.at(-1); assert.equal(form?.title, '데스크톱 배정'); return { form, box: id => form.fields.map(field => field.node?.querySelector(`input[data-vm-id="${id}"]`)).find(Boolean) ?? null }; };
test('assignment registry failure offers no assignment or sharing control and keeps what is saved', t => {
  const f = fixture(t), user = account(f), saved = { username: user.username, desktop_id: f.registered.id, clipboardPolicy: { textToRemote: true } };
  f.ctx.state.data.assignments.push(saved); f.entries.hosts.error = 'fixture registry unavailable';
  const root = users.desktopsPanel(f.ctx, user);
  assert.match(root.textContent, /Host 등록 정보를 확인하지 못했어요/); assert.equal(f.find(root, '데스크톱 배정'), undefined);
  assert.equal(root.querySelector('li.assigned-row,button[data-preset],button[data-remove-vm],button[data-permission-vm],button[data-same-preset],input'), null);
  assert.doesNotMatch(root.textContent, /REGISTERED/);
  users.assignDesktops(f.ctx, user);
  assert.deepEqual(f.dialogs, [], 'no dialog opens while registration is unknown');
  assert.deepEqual(f.notices, [['Host 등록 정보를 새로고침한 뒤 다시 선택하세요.', true]]);
  assert.deepEqual(f.writes, []); assert.deepEqual(f.ctx.state.data.assignments, [saved]); assert.equal(saved.clipboardPolicy.textToRemote, true);
});
for (const change of ['withdrawn', 'registry-error']) test('a registration ' + change + ' after the assignment dialog opened is rejected before any replacement request', async t => {
  const f = fixture(t), user = account(f), { form, box } = openAssign(f, user);
  box(f.registered.id).checked = true;
  if (change === 'withdrawn') f.values.hosts.hosts.splice(0); else f.entries.hosts.error = 'fixture registry unavailable';
  await assert.rejects(form.action({}), /등록된 Host만/);
  assert.deepEqual(f.writes, []); assert.equal(box(f.registered.id).checked, true, 'the dialog keeps the choice; nothing is saved or discarded behind it');
  assert.equal(f.ctx.state.assignmentSaving, false);
});
test('assignment UI and request contain registered targets only; hidden saved policies remain untouched locally', async t => {
  const f = fixture(t), user = account(f);
  const saved = { username: user.username, desktop_id: f.hidden.id, clipboardPolicy: { filesToLocal: true } };
  f.ctx.state.data.assignments.push(saved);
  const panel = users.desktopsPanel(f.ctx, user);
  assert.equal(panel.querySelector('li.assigned-row'), null, 'an assignment to an unregistered desktop is not a row');
  assert.doesNotMatch(panel.textContent, /UNREGISTERED_PRIVATE/);
  assert.equal(panel.querySelector('[data-vm="pve-100"],[data-remove-vm="pve-100"],[data-permission-vm="pve-100"]'), null);
  const { form, box } = openAssign(f, user);
  assert.ok(box('pve-105')); assert.equal(box('pve-100'), null);
  assert.equal(form.options.save, '배정');
  await assert.rejects(form.action({}), /배정할 데스크톱을 고르세요/); assert.deepEqual(f.writes, [], 'nothing ticked sends nothing');
  box('pve-105').checked = true;
  await form.action({});
  assert.deepEqual(f.writes, [{ path: '/assignments/batch', method: 'PUT', body: { username: user.username, expectedAccountId: user.id, desktopIds: [f.registered.id] } }]);
  assert.equal(f.ctx.state.data.assignments[0], saved); assert.equal(saved.clipboardPolicy.filesToLocal, true);
  // Backend preservation is a separate Store transaction acceptance gate.
});
