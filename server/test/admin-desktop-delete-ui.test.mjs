// 삭제: a desktop is deleted with 삭제 in its window's bar or, several at once, with the
// list's checkboxes and 선택 삭제. One confirmation, typed: the desktop's name for one, 삭제 for several.
// Each desktop then goes through the existing DELETE /desktops/:id/host with its own expectedHostId, one by
// one; a busy one (409 host_registration_busy) is skipped, any other error stops the run.
// Real console (admin-ui-harness), memory-only API answers; nothing is deleted anywhere.
import test from 'node:test';
import assert from 'node:assert/strict';
import { loadAdmin, registeredWorkspaceFixture, iso } from './admin-ui-harness.mjs';

const hostIdOf = n => `00000000-0000-4000-8000-${String(n).padStart(12, '0')}`;
function deleteFixture({ answer = () => null, sessions = [] } = {}) {
  const { user, inventory, monitor } = registeredWorkspaceFixture();
  // Six registered VMs (pve-105 … pve-110) and the approved PC, each with its registration id.
  monitor.hosts = monitor.hosts.map((host, index) => ({ ...host, hostId: hostIdOf(index + 1) }));
  monitor.sessions = sessions;
  inventory.assignments = [{ username: user.username, desktop_id: 'pve-105' }, { username: 'second', desktop_id: 'pve-105' }, { username: user.username, desktop_id: 'pve-106' }];
  const deletes = [];
  const respond = async (url, options) => {
    const match = /^\/api\/v1\/admin\/desktops\/([^/]+)\/host$/.exec(url);
    if (!match || options.method !== 'DELETE') return null;
    const id = decodeURIComponent(match[1]), body = JSON.parse(options.body);
    deletes.push({ id, body });
    const own = answer(id);
    if (own) return { ok: false, status: own.status, json: async () => ({ error: { code: own.code } }) };
    const removed = inventory.assignments.filter(row => row.desktop_id === id).length;
    monitor.hosts = monitor.hosts.filter(host => host.desktopId !== id);
    inventory.assignments = inventory.assignments.filter(row => row.desktop_id !== id);
    return { ok: true, status: 200, json: async () => ({ ok: true, desktopId: id, hostId: body.expectedHostId, removedAssignments: removed }) };
  };
  return { user, inventory, monitor, deletes, respond };
}
const open = async (t, name, options = {}) => {
  const fixture = deleteFixture(options);
  const f = await loadAdmin(t, name, fixture.user, fixture.inventory, { monitor: fixture.monitor, respond: fixture.respond, hash: '#/desktops' });
  await f.settle();
  return { ...fixture, f };
};
const pick = (f, id) => f.content.querySelector(`input.row-check[data-focus="pick:${id}"]`);
const tick = async (f, node, on = true) => { node.checked = on; node.dispatchEvent(new f.window.Event('change')); await f.settle(); };
// The rows in the order the list shows them (problems first), and each desktop's name and registration id.
const inList = (f, ids) => [...f.content.querySelectorAll('input.row-check[data-focus^="pick:"]')].map(node => node.getAttribute('data-focus').slice(5)).filter(id => ids.includes(id));
const nameOf = id => id === 'pc-fixture' ? 'Approved PC' : 'Windows ' + id.slice(4);
const hostOf = id => hostIdOf(['pve-105', 'pve-106', 'pve-107', 'pve-108', 'pve-109', 'pve-110', 'pc-fixture'].indexOf(id) + 1);
const selectedButton = f => f.content.querySelector('button[data-focus=desktops-delete-selected]');
const typeConfirm = async (f, text) => {
  const field = document.querySelector('#modal input[data-confirm-text]');
  Object.defineProperty(field, 'value', { configurable: true, writable: true, value: text });
  field.dispatchEvent(new f.window.Event('input')); await f.settle();
  return field;
};
const save = () => document.querySelector('#modal-save');
const submit = async f => { document.querySelector('#modal-form').dispatchEvent(new f.window.Event('submit', { cancelable: true })); await f.settle(); };

test('a row checkbox chooses the row without opening its window; 선택 삭제 counts the choice', async t => {
  const { f } = await open(t, 'delete-pick');
  assert.equal(selectedButton(f).disabled, true); assert.equal(selectedButton(f).getAttribute('title'), '지울 데스크톱을 먼저 고르세요.');
  const box = pick(f, 'pve-105');
  assert.ok(box, 'every row has a checkbox'); assert.equal(box.getAttribute('aria-label'), 'Windows 105 선택');
  box.dispatchEvent(new f.window.Event('click')); await f.settle();
  assert.equal(document.querySelector('.win-frame'), null, 'pressing the checkbox opens no window');
  assert.equal(globalThis.location.hash, '#/desktops', 'and the address stays on the list');
  await tick(f, pick(f, 'pve-105'));
  assert.equal(pick(f, 'pve-105').checked, true); assert.match(pick(f, 'pve-105').closest('tr').getAttribute('class'), /is-chosen/);
  assert.equal(selectedButton(f).textContent, '선택 삭제 1'); assert.equal(selectedButton(f).disabled, false);
  assert.equal(document.querySelector('.win-frame'), null, 'choosing opened nothing either');
  // 선택 삭제 stands left of 새로고침 in the list's actions.
  const actions = [...f.content.querySelectorAll('.list-actions > button')].map(node => node.textContent);
  assert.ok(actions.indexOf('선택 삭제 1') >= 0 && actions.indexOf('선택 삭제 1') < actions.indexOf('새로고침'), JSON.stringify(actions));
});

test('the heading checkbox chooses only the rows the filter shows, and a search drops hidden choices', async t => {
  const { f } = await open(t, 'delete-all');
  const search = f.content.querySelector('[data-focus=desktops-search]');
  Object.defineProperty(search, 'value', { configurable: true, writable: true, value: 'Windows 10' });
  search.dispatchEvent(new f.window.Event('input')); f.click(f.content, '조회'); await f.settle();
  const shown = [...f.content.querySelectorAll('input.row-check[data-focus^="pick:"]')].map(node => node.getAttribute('data-focus').slice(5));
  assert.ok(shown.length > 1 && shown.length < 31, 'the search narrows the list');
  await tick(f, f.content.querySelector('input[data-focus="pick-all:all"]'));
  const chosen = [...f.content.querySelectorAll('input.row-check[data-focus^="pick:"]')].filter(node => node.checked).map(node => node.getAttribute('data-focus').slice(5));
  assert.deepEqual(chosen, shown, 'all shown rows, nothing else');
  assert.equal(selectedButton(f).textContent, '선택 삭제 ' + shown.length);
  f.click(f.content, '초기화'); await f.settle();
  assert.ok(f.content.querySelectorAll('input.row-check[data-focus^="pick:"]').length > shown.length);
  assert.equal(selectedButton(f).textContent, '선택 삭제 ' + shown.length, 'a wider list keeps what was chosen');
  Object.defineProperty(f.content.querySelector('[data-focus=desktops-search]'), 'value', { configurable: true, writable: true, value: 'Infrastructure' });
  f.content.querySelector('[data-focus=desktops-search]').dispatchEvent(new f.window.Event('input')); f.click(f.content, '조회'); await f.settle();
  assert.equal(selectedButton(f).textContent, '선택 삭제', 'rows the list no longer shows are not chosen');
});

test('several chosen: one typed confirmation, then one request per desktop in order, each with its own expectedHostId', async t => {
  const { f, deletes } = await open(t, 'delete-many');
  for (const id of ['pve-105', 'pve-106', 'pve-107']) await tick(f, pick(f, id));
  const order = inList(f, ['pve-105', 'pve-106', 'pve-107']), names = order.map(nameOf).join(', ');
  f.click(f.content, '선택 삭제 3'); await f.settle();
  assert.equal(document.querySelector('#modal-title').textContent, '데스크톱 삭제');
  const description = document.querySelector('#modal .modal-description').textContent;
  assert.equal(description, '데스크톱 3대를 Multitap 목록에서 지워요: ' + names + '\n배정된 사용자 3명과 권한도 함께 지워져요.\nVM과 파일, 사용자 계정은 그대로예요.\nHost 프로그램은 Windows에 그대로 남아요. 다시 쓰려면 Host에서 연결을 요청하고 승인하면 돼요.');
  assert.match(document.querySelector('#modal label.confirm-text').textContent, /^확인을 위해 삭제라고 입력하세요\./);
  assert.equal(save().textContent, '삭제'); assert.equal(save().className, 'danger');
  assert.equal(save().disabled, true, 'off until the word is typed');
  await typeConfirm(f, '삭 제'); assert.equal(save().disabled, true, 'a wrong text keeps it off');
  await submit(f); assert.deepEqual(deletes, [], 'Enter with a mismatch does nothing'); assert.equal(document.querySelector('#modal').open, true);
  await typeConfirm(f, '  삭제 '); assert.equal(save().disabled, false, 'spaces at both ends do not count');
  await submit(f);
  assert.deepEqual(deletes, order.map(id => ({ id, body: { expectedHostId: hostOf(id), confirm: true } })), 'in the order of the list, one at a time');
  assert.equal(document.querySelector('#modal').open, false);
  assert.equal(document.querySelector('#notice .notice-text').textContent, '삭제했어요: ' + names + '\n배정 3건도 함께 지웠어요.');
  assert.equal(selectedButton(f).textContent, '선택 삭제', 'the choice is cleared'); assert.equal(pick(f, 'pve-105'), null, 'the deleted rows are gone');
});

test('more than five names are shortened, and a connected desktop is named and left out', async t => {
  const sessions = [{ account: 'viewer', desktopId: 'pve-107', startedAt: iso(-60000), ready: true }];
  const { f, deletes } = await open(t, 'delete-busy', { sessions });
  const all = ['pve-105', 'pve-106', 'pve-107', 'pve-108', 'pve-109', 'pve-110', 'pc-fixture'];
  for (const id of all) await tick(f, pick(f, id));
  const targets = inList(f, all).filter(id => id !== 'pve-107');
  f.click(f.content, '선택 삭제 7'); await f.settle();
  const description = document.querySelector('#modal .modal-description').textContent;
  assert.ok(description.startsWith('데스크톱 6대를 Multitap 목록에서 지워요: ' + targets.slice(0, 5).map(nameOf).join(', ') + ' 외 1대\n'), description);
  assert.match(description, /\nVM·로컬 PC와 파일, 사용자 계정은 그대로예요\.\n/, 'VMs and a local PC together');
  assert.match(description, /\nWindows 107: 접속 중인 데스크톱은 지울 수 없어요\. 연결을 끊은 뒤 지우세요\.$/);
  await typeConfirm(f, '삭제'); await submit(f);
  assert.equal(deletes.some(row => row.id === 'pve-107'), false, 'the connected desktop gets no request');
});

test('a busy answer skips that desktop and goes on; any other error stops and says what was and was not deleted', async t => {
  const answer = id => id === 'pve-106' ? { status: 409, code: 'host_registration_busy' } : id === 'pve-108' ? { status: 503, code: 'host_registration_unavailable' } : null;
  const { f, deletes } = await open(t, 'delete-partial', { answer });
  const chosen = ['pve-105', 'pve-106', 'pve-107', 'pve-108', 'pve-109', 'pve-110'];
  for (const id of chosen) await tick(f, pick(f, id));
  // pve-108 fails last but one, whatever order the list shows: the busy one and a deleted one come first.
  const order = inList(f, chosen), stop = order.indexOf('pve-108');
  f.click(f.content, '선택 삭제 6'); await f.settle();
  await typeConfirm(f, '삭제'); await submit(f);
  assert.deepEqual(deletes.map(row => row.id), order.slice(0, stop + 1), 'no automatic retry, and nothing after the stop');
  const done = order.slice(0, stop).filter(id => id !== 'pve-106'), rest = order.slice(stop + 1);
  const text = document.querySelector('#notice .notice-text').textContent;
  if (done.length) assert.ok(text.includes('삭제했어요: ' + done.map(nameOf).join(', ')), text);
  if (order.indexOf('pve-106') < stop) assert.ok(text.includes('Windows 106: 사용 중이라 지우지 않았어요.'), text);
  assert.ok(text.includes('Windows 108: 지우지 못했어요.'), text);
  if (rest.length) assert.ok(text.includes('남은 데스크톱은 지우지 않았어요: ' + rest.map(nameOf).join(', ')), text);
  assert.equal(selectedButton(f).textContent, '선택 삭제');
});

test('one desktop from its window: the name is typed, and nothing is sent before it matches', async t => {
  const { f, deletes } = await open(t, 'delete-one');
  f.pickVm('pve-105'); await f.settle();
  const button = document.querySelector('.win-foot .win-actions').firstElementChild;
  assert.equal(button.textContent, '삭제'); assert.match(button.getAttribute('class'), /danger/);
  button.dispatchEvent(new f.window.Event('click')); await f.settle();
  assert.equal(document.querySelector('#modal-title').textContent, '데스크톱 삭제');
  assert.match(document.querySelector('#modal .modal-description').textContent, /^Windows 105: Multitap 목록에서 지워요\.\n배정된 사용자 2명과 권한도 함께 지워져요\./);
  assert.match(document.querySelector('#modal label.confirm-text').textContent, /^확인을 위해 데스크톱 이름을 입력하세요: Windows 105/);
  assert.equal(save().disabled, true);
  for (const wrong of ['삭제', 'windows 105', 'Windows 10']) { await typeConfirm(f, wrong); assert.equal(save().disabled, true, wrong); await submit(f); }
  assert.deepEqual(deletes, [], 'the API is not called before a match');
  await typeConfirm(f, 'Windows 105'); assert.equal(save().disabled, false);
  await submit(f);
  assert.deepEqual(deletes, [{ id: 'pve-105', body: { expectedHostId: hostOf('pve-105'), confirm: true } }]);
  assert.equal(document.querySelector('#notice').textContent.includes('Windows 105: 삭제했어요.'), true);
  assert.equal(document.querySelector('.win-frame'), null, 'the window of the deleted desktop closed');
});
