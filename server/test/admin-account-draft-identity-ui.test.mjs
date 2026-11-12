import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { parseHTML } from 'linkedom';
import { exactPermissions, presetValue, savedPermissions, saveUserSharing, saveDesktopSharing, removeAssignment } from '../admin/js/permissions.js';
import { messages } from '../admin/js/labels.js';

// The console has no drafts any more: every assignment and sharing change is
// saved at once. What stays is the identity protection, tested here on the
// shipped modules with test exports added only in memory. The DOM is inert;
// API/modal/navigation collaborators record requests without a server.
async function moduleCopy(path, exports) {
  const url = new URL(path, import.meta.url);
  const source = (await readFile(url, 'utf8')).replace(/from\s+(['"])(\.[^'"]+)\1/g,
    (_, quote, relative) => `from ${quote}${new URL(relative, url).href}${quote}`);
  return import('data:text/javascript;base64,' + Buffer.from(source +
    '\nexport {' + exports + '};').toString('base64'));
}
const users = await moduleCopy('../admin/js/users.js', 'assignDesktops,desktopsPanel');
const desktops = await moduleCopy('../admin/js/desktops.js', 'peoplePanel');
const turn = () => new Promise(resolve => setImmediate(resolve));
const granted = { clipboardPolicy: { textToRemote: true, filesToLocal: true }, devicePolicy: { drives: true } };
const fixture = t => {
  const window = parseHTML('<html><body></body></html>');
  const old = { document: globalThis.document, Node: globalThis.Node };
  globalThis.document = window.document; globalThis.Node = window.Node;
  t.after(() => { for (const [key, value] of Object.entries(old)) {
    if (value === undefined) delete globalThis[key]; else globalThis[key] = value;
  } });
  const a = { id: 'account_A', username: 'alice', role: 'user', disabled: false };
  const c = { id: 'account_C', username: 'carol', role: 'user', disabled: false };
  const desktop = { id: 'fixture-vm', name: 'Fixture VM', kind: 'vm', pve_vmid: 900, pve_available: 1, enabled: true };
  const spare = { id: 'fixture-vm-2', name: 'Fixture VM 2', kind: 'vm', pve_vmid: 901, pve_available: 1, enabled: true };
  const requests = [], notices = [], modals = [];
  // `sessions` is the current session list; null is a list the console could not read.
  const live = { sessions: [], hosts: [{ desktopId: desktop.id }, { desktopId: spare.id }] };
  const ctx = {
    state: { user: { id: 'fixture-admin' }, csrf: 'fixture-csrf', route: { page: 'user' }, ui: {},
      data: { users: [a, c], desktops: [desktop, spare], assignments: [], proxmox: { configured: false } },
      assignmentSaving: false, pveBusy: false },
    userByName: name => ctx.state.data.users.find(user => user.username === name),
    userById: id => ctx.state.data.users.find(user => user.id === id),
    desktopById: id => ctx.state.data.desktops.find(row => row.id === id),
    value: key => key === 'hosts' ? { hosts: live.hosts } : key === 'sessions' ? live.sessions && { sessions: live.sessions } : null,
    resource: () => null,
    pageAction: () => ({ check() {}, current: () => true }),
    modal(title, fields, run, options) { modals.push({ title, fields, run, options }); },
    render() {}, navigate() {}, async refresh() {},
    link(text) { const node = document.createElement('a'); node.textContent = text; return node; },
    notice: (...args) => notices.push(args),
    async api(path, method, data) {
      requests.push({ path, method, data });
      return { ok: true, added: 1, removed: 0, policiesChanged: 2 };
    },
  };
  const replace = () => {
    const b = { ...a, id: 'account_B' };
    ctx.state.data.users = [b, c]; return b;
  };
  // Both accounts have the desktop; `value` is what is saved for each.
  const assigned = (value = {}) => { ctx.state.data.assignments = [a, c].map(user => ({ username: user.username, desktop_id: desktop.id, ...structuredClone(value) })); };
  const click = (root, label) => {
    const node = [...root.querySelectorAll('button')].find(button => button.textContent === label);
    assert.ok(node, label); assert.equal(node.disabled, false, label);
    node.dispatchEvent(new window.Event('click'));
  };
  const flip = (root, selector, checked) => {
    const node = root.querySelector(selector);
    assert.ok(node, selector); assert.equal(node.hasAttribute('disabled'), false, selector);
    node.checked = checked; node.dispatchEvent(new window.Event('change'));
  };
  // The account's page panel with one desktop's switches open, as it was rendered.
  const userPanel = (user, open = desktop) => { ctx.state.ui.sharingOpen = `${user.id}:${open.id}`; return users.desktopsPanel(ctx, user); };
  const peoplePanel = (open = null) => { ctx.state.ui.sharingOpen = open ? `${open.id}:${desktop.id}` : null; return desktops.peoplePanel(ctx, desktop); };
  const identityNotice = code => [messages[code], true];
  return { window, ctx, a, c, desktop, spare, requests, notices, modals, live, replace, assigned, click, flip, userPanel, peoplePanel, identityNotice };
};
const allOn = exactPermissions({ clipboardPolicy: { textToRemote: true, textToLocal: true, filesToRemote: true, filesToLocal: true },
  devicePolicy: { drives: true, printers: true, usbDevices: true, smartCards: true, serialPorts: true } });

test('a stable copy of the account saves under its immutable ID, one request naming every assigned desktop', async t => {
  const f = fixture(t); f.assigned();
  f.ctx.state.data.assignments.push({ username: 'alice', desktop_id: f.spare.id });
  f.click(f.userPanel({ ...f.a }), '모두 허용'); await turn();
  assert.deepEqual(f.modals, [], 'turning sharing on asks nothing');
  assert.deepEqual(f.requests, [{ path: '/assignments/batch', method: 'PUT', data: { username: 'alice', expectedAccountId: 'account_A',
    assignments: [{ desktopId: f.desktop.id, clipboardPolicy: allOn.clipboardPolicy, devicePolicy: allOn.devicePolicy }, { desktopId: f.spare.id }] } }]);
  assert.equal(f.requests[0].data.assignments.some(row => Object.hasOwn(row, 'soundCardAdd')), false);
  assert.match(f.notices.at(-1)[0], /^저장했어요\./); assert.notEqual(f.notices.at(-1)[1], true);
  assert.equal(f.ctx.state.assignmentSaving, false);
});
test('saved sharing is read for a stable account copy but never for a same-name replacement', t => {
  const f = fixture(t); f.assigned(granted);
  const saved = savedPermissions(f.ctx, { ...f.a }, f.desktop.id);
  assert.equal(saved.clipboardPolicy.filesToLocal, true); assert.equal(saved.devicePolicy.drives, true);
  f.replace();
  // The rows are still stored under the username; the retired account must not read them.
  assert.deepEqual(savedPermissions(f.ctx, f.a, f.desktop.id), exactPermissions(undefined));
});
test('a page rendered for a retired account shows none of the replacement account sharing and is locked', t => {
  const f = fixture(t); f.assigned(granted); f.replace();
  const panel = f.userPanel(f.a);
  assert.equal([...panel.querySelectorAll('input[role=switch]')].length, 9);
  assert.equal([...panel.querySelectorAll('input[role=switch]')].some(input => input.hasAttribute('checked')), false, 'no value of the same-name account');
  for (const input of panel.querySelectorAll('input[role=switch]')) assert.equal(input.hasAttribute('disabled'), true);
  for (const node of panel.querySelectorAll('button[data-preset],button[data-same-preset],button[data-remove-vm]')) {
    assert.equal(node.disabled, true); assert.equal(node.title, messages.account_identity_changed);
  }
  const assign = [...panel.querySelectorAll('button')].find(node => node.textContent === '데스크톱 배정');
  assert.equal(assign.disabled, true); assert.equal(assign.title, messages.account_identity_changed);
});
test('데스크톱 배정 carries the owner ID and no sharing value, so the new assignment starts with the server default', async t => {
  const f = fixture(t); f.ctx.state.data.assignments = [{ username: 'alice', desktop_id: f.spare.id, ...structuredClone(granted) }];
  users.assignDesktops(f.ctx, f.a);
  assert.equal(f.modals.length, 1); assert.equal(f.modals[0].title, '데스크톱 배정'); assert.equal(f.modals[0].options.save, '배정');
  const list = f.modals[0].fields.find(field => field.node).node;
  assert.deepEqual([...list.querySelectorAll('input[data-vm-id]')].map(node => node.getAttribute('data-vm-id')), [f.desktop.id], 'only what the account does not have yet');
  await assert.rejects(f.modals[0].run({}), /배정할 데스크톱을 고르세요/);
  assert.deepEqual(f.requests, [], 'nothing is sent without a choice');
  list.querySelector('input[data-vm-id]').checked = true;
  await f.modals[0].run({});
  assert.deepEqual(f.requests, [{ path: '/assignments/batch', method: 'PUT',
    data: { username: 'alice', expectedAccountId: 'account_A', desktopIds: [f.spare.id, f.desktop.id] } }]);
  assert.equal(f.ctx.state.assignmentSaving, false);
});
// The account changes after its page was rendered: the old controls must not
// write to whatever now answers to that username.
const changes = {
  'replaced by a same-name account': f => { f.replace(); return 'account_identity_changed'; },
  'renamed': f => { f.ctx.state.data.users = [{ ...f.a, username: 'alice-renamed' }, f.c]; return 'account_identity_changed'; },
  'removed': f => { f.ctx.state.data.users = [f.c]; return 'account_identity_changed'; },
  'stopped': f => { f.ctx.state.data.users = [{ ...f.a, disabled: true }, f.c]; return 'user_or_desktop_unavailable'; },
};
for (const [label, change] of Object.entries(changes)) test(`controls rendered for an account that was then ${label} send nothing`, async t => {
  const f = fixture(t); f.assigned(granted);
  f.ctx.state.data.assignments.push({ username: 'alice', desktop_id: f.spare.id });
  f.live.sessions = [{ account: 'alice', desktopId: f.desktop.id }];
  const panel = f.userPanel(f.a), people = f.peoplePanel(f.a);
  const code = change(f), expected = f.identityNotice(code);
  const refused = async (act, text) => { f.notices.length = 0; act(); await turn(); assert.deepEqual(f.notices, [expected], text); assert.deepEqual(f.requests, [], text); assert.deepEqual(f.modals, [], text); };
  await refused(() => f.click(panel.querySelector(`[data-desktop="${f.desktop.id}"]`), '모두 허용'), 'a named choice');
  await refused(() => f.click(panel.querySelector(`[data-desktop="${f.desktop.id}"]`), '기본'), 'turning off, before any confirmation');
  await refused(() => f.flip(panel, '[data-clipboard-permission="filesToRemote"]', true), 'one switch');
  await refused(() => f.click(panel.querySelector('.same-for-all'), '모두 허용'), '모두에 한 번에');
  await refused(() => f.click(panel, '데스크톱 배정'), '데스크톱 배정');
  await refused(() => f.click(people.querySelector('[data-person="alice"] .sharing-control'), '모두 허용'), "the desktop's row");
  await refused(() => f.flip(people, '[data-person="alice"] [data-device-permission="printers"]', true), "the desktop's switch");
  await refused(() => f.click(people.querySelector('.same-for-all'), '모두 허용'), '모두에게 한 번에');
  if (code === 'account_identity_changed') {
    await refused(() => f.click(panel.querySelector(`[data-desktop="${f.desktop.id}"]`), '빼기'), '빼기');
    await refused(() => f.click(people.querySelector('[data-person="alice"]'), '빼기'), "the desktop's 빼기");
  }
  assert.equal(f.ctx.state.assignmentSaving, false);
});
test('the turn-off confirmation refuses an account replaced while it is open', async t => {
  const f = fixture(t); f.assigned(granted); f.live.sessions = [{ account: 'alice', desktopId: f.desktop.id }];
  f.click(f.userPanel(f.a), '기본'); await turn();
  assert.equal(f.modals.length, 1); assert.equal(f.modals[0].title, '공유를 끌까요?'); assert.equal(f.modals[0].options.save, '끄고 연결 끊기');
  assert.deepEqual(f.requests, [], 'nothing is sent before the answer');
  f.replace();
  await assert.rejects(f.modals[0].run(), /새로고침/);
  assert.deepEqual(f.requests, []); assert.equal(f.ctx.state.assignmentSaving, false);
});
test('the confirmation captures primitive identity rather than a mutable account object', async t => {
  const f = fixture(t); f.assigned(granted); f.live.sessions = [{ account: 'alice', desktopId: f.desktop.id }];
  f.click(f.userPanel(f.a), '기본'); await turn();
  assert.equal(f.modals.length, 1);
  f.a.id = 'account_B';
  await assert.rejects(f.modals[0].run(), /새로고침/);
  assert.deepEqual(f.requests, []);
});
test('a confirmed turn-off of a stable account is one request under the ID shown', async t => {
  const f = fixture(t); f.assigned(granted); f.live.sessions = [{ account: 'alice', desktopId: f.desktop.id }];
  f.click(f.userPanel(f.a), '기본'); await turn();
  await f.modals[0].run();
  const off = exactPermissions(undefined);
  assert.deepEqual(f.requests, [{ path: '/assignments/batch', method: 'PUT', data: { username: 'alice', expectedAccountId: 'account_A',
    assignments: [{ desktopId: f.desktop.id, clipboardPolicy: off.clipboardPolicy, devicePolicy: off.devicePolicy }] } }]);
});
test('데스크톱 배정 and 배정 빼기 dialogs refuse an account replaced while they are open', async t => {
  for (const mutate of [f => f.replace(), f => { f.a.id = 'account_B'; }]) {
    const f = fixture(t); f.assigned();
    users.assignDesktops(f.ctx, f.a); removeAssignment(f.ctx, f.a, f.desktop);
    assert.deepEqual(f.modals.map(modal => modal.title), ['데스크톱 배정', '배정 빼기']);
    f.modals[0].fields.find(field => field.node).node.querySelector('input[data-vm-id]').checked = true;
    mutate(f);
    await assert.rejects(f.modals[0].run({}), /새로고침/);
    await assert.rejects(f.modals[1].run({}), /새로고침/);
    assert.deepEqual(f.requests, []); assert.equal(f.ctx.state.assignmentSaving, false);
  }
});
test('배정 빼기 of a stable account names the remaining desktops under the ID shown', async t => {
  const f = fixture(t); f.assigned(); f.ctx.state.data.assignments.push({ username: 'alice', desktop_id: f.spare.id });
  removeAssignment(f.ctx, f.a, f.desktop);
  assert.equal(f.modals[0].options.save, '빼기'); assert.deepEqual(f.requests, [], 'nothing is sent before the answer');
  await f.modals[0].run({});
  assert.deepEqual(f.requests, [{ path: '/assignments/batch', method: 'PUT', data: { username: 'alice', expectedAccountId: 'account_A', desktopIds: [f.spare.id] } }]);
});
test('desktop 쓰는 사람 rows each carry their own captured immutable account ID in one request', async t => {
  const f = fixture(t); f.assigned();
  f.click(f.peoplePanel().querySelector('.same-for-all'), '모두 허용'); await turn();
  assert.equal(f.requests.length, 1);
  assert.equal(f.requests[0].path, '/desktops/' + f.desktop.id + '/permissions'); assert.equal(f.requests[0].method, 'PUT');
  const rows = f.requests[0].data.permissions;
  assert.deepEqual(rows.map(row => [row.username, row.expectedAccountId]), [['alice', 'account_A'], ['carol', 'account_C']]);
  for (const row of rows) { assert.deepEqual(row.clipboardPolicy, allOn.clipboardPolicy); assert.deepEqual(row.devicePolicy, allOn.devicePolicy); assert.equal(Object.hasOwn(row, 'soundCardAdd'), false); }
});
test('desktop bulk change refuses the entire request if one captured owner changed', async t => {
  const f = fixture(t); f.assigned();
  const panel = f.peoplePanel();
  f.replace(); f.click(panel.querySelector('.same-for-all'), '모두 허용'); await turn();
  assert.equal(f.requests.length, 0);
  assert.deepEqual(f.notices, [f.identityNotice('account_identity_changed')]);
  assert.deepEqual(savedPermissions(f.ctx, f.c, f.desktop.id), exactPermissions(undefined), 'the unchanged account is not written either');
});
test('an old desktop panel row writes only the account it was rendered for', async t => {
  const f = fixture(t); f.assigned();
  const carol = f.peoplePanel(f.c), alice = f.peoplePanel(f.a);
  f.replace();
  f.click(alice.querySelector('[data-person="alice"] .sharing-control'), '글·그림 복사'); await turn();
  assert.deepEqual(f.requests, [], 'the retired account row sends nothing');
  f.click(carol.querySelector('[data-person="carol"] .sharing-control'), '글·그림 복사'); await turn();
  assert.equal(f.requests.length, 1);
  assert.deepEqual(f.requests[0].data.permissions.map(row => [row.username, row.expectedAccountId]), [['carol', 'account_C']]);
});
test('desktop assignment dialog retains the identity in its original choices', async t => {
  const f = fixture(t); desktops.assignDesktopUser(f.ctx, f.desktop);
  assert.equal(f.modals.length, 1); f.replace();
  await assert.rejects(f.modals[0].run({ username: 'alice' }), /새로고침/);
  assert.equal(f.requests.length, 0);
});
test('stable desktop assignment carries the selected original account ID and no sharing value', async t => {
  const f = fixture(t); desktops.assignDesktopUser(f.ctx, f.desktop);
  await f.modals[0].run({ username: 'alice' });
  assert.deepEqual(f.requests, [{ path: '/assignments/batch', method: 'PUT', data: { username: 'alice', expectedAccountId: 'account_A', desktopIds: [f.desktop.id] } }]);
});
test('사용자 배정 opens only for a registered desktop or an approved, registered local PC, and rechecks at submit', async t => {
  const f = fixture(t);
  const pc = { id: 'fixture-pc', name: 'Fixture PC', kind: 'pc', localPcApproved: true }, unapproved = { id: 'fixture-pc-unapproved', name: 'Unapproved PC', kind: 'pc' };
  f.ctx.state.data.desktops.push(pc, unapproved);
  f.live.hosts.push({ desktopId: unapproved.id });
  desktops.assignDesktopUser(f.ctx, unapproved);
  assert.deepEqual(f.modals, [], 'a Host record does not approve a PC');
  desktops.assignDesktopUser(f.ctx, pc);
  assert.deepEqual(f.modals, [], 'approval alone does not register a PC'); assert.deepEqual(f.notices, [[messages.host_not_registered, true]]);
  f.live.hosts.push({ desktopId: pc.id });
  desktops.assignDesktopUser(f.ctx, pc); desktops.assignDesktopUser(f.ctx, pc);
  assert.equal(f.modals.length, 2); assert.equal(f.modals[0].title, '사용자 배정');
  await f.modals[0].run({ username: 'carol' });
  assert.deepEqual(f.requests, [{ path: '/assignments/batch', method: 'PUT', data: { username: 'carol', expectedAccountId: 'account_C', desktopIds: [pc.id] } }]);
  // The Host is removed while the second dialog is open.
  f.live.hosts.pop();
  await assert.rejects(f.modals[1].run({ username: 'alice' }), error => error.message === messages.host_not_registered);
  assert.equal(f.requests.length, 1);
});
test('missing and ambiguous identities never fall back to a username mutation', async t => {
  const f = fixture(t); f.assigned(granted);
  for (const conflict of ['missing', 'duplicate-name', 'duplicate-id']) {
    const target = { ...f.a };
    if (conflict === 'duplicate-name') f.ctx.state.data.users = [target, { ...target, id: 'account_B' }];
    else if (conflict === 'duplicate-id') f.ctx.state.data.users = [target, { ...target, username: 'different-name' }];
    else { delete target.id; f.ctx.state.data.users = [target]; }
    const value = presetValue('all', exactPermissions(undefined));
    for (const [name, act] of Object.entries({
      sharing: () => saveUserSharing(f.ctx, target, [{ desktop: f.desktop, value }]),
      desktopSharing: () => saveDesktopSharing(f.ctx, f.desktop, [{ owner: target, value }]),
      assign: () => users.assignDesktops(f.ctx, target),
      remove: () => removeAssignment(f.ctx, target, f.desktop),
    })) {
      f.notices.length = 0; act(); await turn();
      assert.equal(f.requests.length, 0, conflict + ' ' + name);
      assert.equal(f.modals.length, 0, conflict + ' ' + name);
      assert.equal(f.notices.length, 1, conflict + ' ' + name); assert.equal(f.notices[0][1], true);
      assert.equal(f.notices[0][0], messages[conflict === 'missing' ? 'account_identity_required' : 'account_identity_changed'], conflict + ' ' + name);
    }
  }
});
