import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseHTML } from 'linkedom';
import { el } from '../admin/js/dom.js';
import { requestCards } from '../admin/js/hosts.js';
import { list, detail, powerForm } from '../admin/js/desktops.js';

// Actual admin DOM/actions with memory-only API answers. No server/network,
// enrollment, Windows or display operation runs in this fixture.
// There is no Host 관리 page and no kind filter any more: a local PC is a row
// of 데스크톱 beside the VMs, and a request the server could not place on a
// VM offers ‘로컬 PC로 등록’ on the same card as ‘직접 입력’.
function fixture(t, { requests = [], desktops = [], assignments = [], configured = false } = {}) {
  const previous = { document: globalThis.document, Node: globalThis.Node };
  const window = parseHTML('<!doctype html><html><body></body></html>');
  globalThis.document = window.document; globalThis.Node = window.Node;
  t.after(() => Object.assign(globalThis, previous));
  const writes = [], dialogs = [];
  const values = { hostRequests: { requests }, hosts: { hosts: desktops.map(d => ({ desktopId: d.id, hostname: d.name, version: '0.2.87' })) }, sessions: { sessions: [] } };
  const ctx = {
    state: { user: { id: 'fixture-admin', username: 'fixture-admin' }, csrf: 'fixture-csrf', pveBusy: false, pveError: '', pveLoaded: true, assignmentSaving: false, route: { page: 'desktops', id: null }, ui: {},
      data: { desktops, users: [], assignments, audit: [], proxmox: { configured }, hostUpdates: { desktops: {} } } },
    value: key => values[key] ?? null, resource: key => values[key] ? { value: values[key] } : null,
    desktopById: id => desktops.find(d => d.id === id) ?? null, userByName: () => null,
    api: async (path, method, body) => { writes.push({ path, method, body }); return {}; },
    modal: (title, fields, action, options) => { dialogs.push({ title, fields, action, options }); },
    notice() {}, load: async () => {}, render() {}, navigate() {}, syncProxmox() {}, scheduleHostUpdatePoll() {},
    pageAction: () => { const actor = ctx.state.user, csrf = ctx.state.csrf; return { current: () => ctx.state.user === actor && ctx.state.csrf === csrf, check() { if (ctx.state.user !== actor || ctx.state.csrf !== csrf) throw new Error('로그인 상태가 바뀌었어요.'); } }; },
    pageHeader: ({ title, actions = [] }) => el('header', {}, el('h1', {}, title), actions),
    searchInput: () => el('input'),
    link: (text, route, id, attrs = {}) => el('a', { ...attrs, href: '#' + route + '/' + id }, text),
  };
  // The cards as the 데스크톱 page shows them (승인 대기): no kind is passed.
  const cards = () => requestCards(ctx);
  const find = (root, label) => [...root.querySelectorAll('button')].find(b => b.textContent.trim() === label);
  const render = (id = null) => el('main', {}, id ? detail(ctx, { page: 'desktop', id }) : list(ctx));
  return { ctx, requests, writes, dialogs, cards, find, render };
}
const request = (extra = {}) => ({
  requestId: 'fixture-request', hostname: 'OFFICE-PC', machineUuid: 'fixture-machine',
  version: '0.2.87', status: 'pending', expiresAt: '2099-01-01T00:00:00.000Z',
  verificationCode: 'ABCD-EFGH', codeDisplay: 'shown', candidates: [],
  duplicateRequestIds: [], localPcRegistration: { allowed: true }, ...extra,
});
const pc = { id: 'pc-fixture', name: '업무 PC', kind: 'pc', localPcApproved: true };
const powerLabels = ['다시 시작', '끄기', '켜기', '켜는 중…'];

test('an unplaced request offers local PC registration on its own card and requires explicit code comparison', async t => {
  const f = fixture(t, { requests: [request()] });
  const ui = f.render();
  const waiting = ui.querySelector('section.requests-panel');
  assert.equal(waiting.querySelector('h2').textContent, '승인 대기1');
  assert.equal(ui.querySelector('.host-kind-filter,.host-section-tabs'), null, 'no Host kind has to be chosen first');
  const card = waiting.querySelector('article.request-card');
  assert.equal(waiting.querySelectorAll('article.request-card').length, 1);
  assert.deepEqual([...card.querySelectorAll('button')].map(b => b.textContent.trim()), ['직접 입력', '로컬 PC로 등록', '거절'], 'one card: type the VM, or register it as a local PC');
  assert.equal(card.querySelector('.request-match').textContent, 'Proxmox에서 맞는 VM을 찾지 못했어요.직접 입력');
  assert.equal(card.querySelector('.request-pc-hint').textContent, 'Proxmox 밖의 PC라면 로컬 PC로 등록하세요.');
  assert.equal(f.find(card, '로컬 PC로 등록').disabled, false);
  assert.equal(f.writes.length, 0);
  f.find(card, '로컬 PC로 등록').click();
  const form = f.dialogs[0];
  assert.equal(form.title, '로컬 PC로 등록'); assert.equal(form.options.save, 'PC 등록');
  assert.equal(form.fields.find(field => field.name === 'acknowledgeCode').value, 'no');
  assert.equal(form.fields.find(field => field.name === 'acknowledgeUnlinked').value, 'no');
  await assert.rejects(form.action({ name: '업무 PC', acknowledgeCode: 'no' }), /확인/);
  await assert.rejects(form.action({ name: '업무 PC', acknowledgeCode: 'yes', acknowledgeUnlinked: 'no' }), /Proxmox 연결 없이 Host로 등록하는 데 동의해야 해요\./);
  assert.equal(f.writes.length, 0);
  await form.action({ name: ' 업무 PC ', acknowledgeCode: 'yes', acknowledgeUnlinked: 'yes' });
  assert.deepEqual(f.writes, [{ path: '/hosts/requests/fixture-request/approve-pc', method: 'POST', body: { name: '업무 PC', acknowledgeCode: true, acknowledgeUnlinked: true } }]);
});

test('VM-matched requests stay outside PC registration even if metadata wrongly permits it', t => {
  const f = fixture(t, { requests: [request({ candidates: [{ desktopId: 'pve-170', vmid: 170, name: 'VM' }] })] });
  assert.equal(f.find(f.cards(), '로컬 PC로 등록'), undefined);
  assert.ok(f.find(f.cards(), '확인하고 승인'));
  // The same for a new VM the server found in Proxmox.
  f.requests.splice(0, 1, request({ vmLookup: { state: 'matched', vm: { node: 'fixture-node', vmid: 171, name: 'FOUND-VM' } } }));
  assert.equal(f.find(f.cards(), '로컬 PC로 등록'), undefined);
  assert.equal(f.cards().querySelector('.request-pc-hint'), null);
  assert.equal(f.writes.length, 0); assert.equal(f.dialogs.length, 0);
});

test('unknown, ambiguous and Host-without-code requests cannot open PC approval', t => {
  const f = fixture(t, { requests: [] });
  const twoVms = [{ desktopId: 'pve-170', vmid: 170, name: 'VM-A' }, { desktopId: 'pve-171', vmid: 171, name: 'VM-B' }];
  for (const extra of [{ localPcRegistration: { allowed: false, reason: 'host_request_vm_ambiguous' } }, { localPcRegistration: { allowed: false } }, { candidates: twoVms }, { codeDisplay: 'unavailable' }, { verificationCode: '' }, { expiresAt: '2000-01-01T00:00:00.000Z' }]) {
    f.requests.splice(0, f.requests.length, request(extra));
    const approve = f.find(f.cards(), '로컬 PC로 등록');
    assert.equal(approve.disabled, true, JSON.stringify(extra));
    assert.ok(approve.title, 'the disabled control says why');
    approve.click();
    assert.equal(f.dialogs.length, 0);
    assert.equal(f.writes.length, 0);
  }
  // A request that carries no local PC registration answer at all is not offered as one.
  f.requests.splice(0, f.requests.length, request({ localPcRegistration: undefined }));
  assert.equal(f.find(f.cards(), '로컬 PC로 등록'), undefined);
});

test('duplicate PC requires manual existing-target selection and explicit Host replacement', async t => {
  const f = fixture(t, { desktops: [pc], requests: [request({ localPcRegistration: { allowed: true, existingDesktopId: pc.id, existingName: pc.name } })] });
  f.find(f.cards(), '로컬 PC로 등록').click();
  const form = f.dialogs[0];
  assert.equal(form.options.save, '기존 PC에 등록');
  assert.equal(form.fields.find(field => field.name === 'desktopId').value, '');
  assert.equal(form.fields.find(field => field.name === 'replaceExisting').value, 'no');
  await assert.rejects(form.action({ name: pc.name, acknowledgeCode: 'yes', acknowledgeUnlinked: 'yes', desktopId: '', replaceExisting: 'yes' }), /교체/);
  await assert.rejects(form.action({ name: pc.name, acknowledgeCode: 'yes', acknowledgeUnlinked: 'yes', desktopId: pc.id, replaceExisting: 'no' }), /교체/);
  assert.equal(f.writes.length, 0);
  await form.action({ name: pc.name, acknowledgeCode: 'yes', acknowledgeUnlinked: 'yes', desktopId: pc.id, replaceExisting: 'yes' });
  assert.deepEqual(f.writes[0].body, { name: pc.name, acknowledgeCode: true, acknowledgeUnlinked: true, desktopId: pc.id, replaceExisting: true });
});

test('late request reclassification and changed administrator session prevent PC approval', async t => {
  const f = fixture(t, { requests: [request()] });
  f.find(f.cards(), '로컬 PC로 등록').click();
  f.requests[0].localPcRegistration = { allowed: false };
  await assert.rejects(f.dialogs[0].action({ name: 'PC', acknowledgeCode: 'yes' }), /등록/);
  f.requests[0].localPcRegistration = { allowed: true };
  // The server placed it on a Proxmox VM after the dialog opened: it is no longer a local PC.
  f.requests[0].candidates = [{ desktopId: 'pve-170', vmid: 170, name: 'VM' }];
  await assert.rejects(f.dialogs[0].action({ name: 'PC', acknowledgeCode: 'yes', acknowledgeUnlinked: 'yes' }));
  f.requests[0].candidates = [];
  f.ctx.state.csrf = 'different-fixture-session';
  await assert.rejects(f.dialogs[0].action({ name: 'PC', acknowledgeCode: 'yes' }), /로그인/);
  assert.equal(f.writes.length, 0);
});

test('only explicitly approved PCs are listed, beside the VMs', t => {
  const f = fixture(t, { configured: true, desktops: [pc, { id: 'legacy', name: 'LEGACY-MANUAL', kind: 'pc' }, { id: 'pve-170', name: 'WINDOWS-VM', kind: 'vm', pve_vmid: 170 }] });
  const ui = f.render();
  assert.deepEqual([...ui.querySelectorAll('a.vm-list-item')].map(node => [node.getAttribute('data-focus'), node.getAttribute('data-status')]),
    [['vm-pick-pve-170', 'bad'], ['vm-pick-pc-fixture', 'off']], 'VMs then local PCs; an unapproved PC is never a row');
  assert.doesNotMatch(ui.textContent, /LEGACY-MANUAL|VM null|VM undefined/);
  const row = ui.querySelector('a.vm-list-item[data-focus="vm-pick-pc-fixture"]');
  assert.equal(row.querySelector('.vm-list-name strong').textContent, '업무 PC');
  assert.equal(row.querySelector('.vm-list-name small').textContent, '로컬 PC · 사용자 없음');
  assert.equal(row.closest('tr').querySelector('.state-dot').textContent, '응답 없음', 'Proxmox does not report a PC: silent is not 접속 불가');
  assert.equal(row.closest('tr').querySelector('.pill'), null, 'the state is a dot and its word, never a badge');
  assert.deepEqual([...row.closest('tr').querySelectorAll('td')].slice(4, 6).map(node => node.textContent), ['로컬 PC', '—'], 'its kind, and no Proxmox node');
  assert.equal(ui.querySelector('select[data-focus=desktops-state] option[value=bad]').textContent, '접속 불가 1');
  assert.equal(ui.querySelector('select[data-focus=desktops-state] option[value=off]').textContent, '꺼짐 1');
  // An unapproved PC is not reachable by its address either.
  const direct = f.render('legacy');
  assert.match(direct.textContent, /데스크톱을 찾지 못했어요/); assert.equal(direct.querySelector('.dt-main'), null);
  assert.doesNotMatch(direct.textContent, /LEGACY-MANUAL/);
  // Grouped by node, local PCs have their own group outside Proxmox.
  f.ctx.state.ui.desktops.byNode = true;
  assert.deepEqual([...f.render().querySelectorAll('section.vm-list-group')].map(node => [node.querySelector('h3 span').textContent, [...node.querySelectorAll('a.vm-list-item strong')].map(name => name.textContent)]),
    [['Proxmox 노드 미확인', ['WINDOWS-VM']], ['로컬 PC (Proxmox 밖)', ['업무 PC']]]);
  assert.equal(f.writes.length, 0);
});

test('a local PC is renamed in place and never gets Proxmox power, Console or virtual-display controls', async t => {
  const vm = { id: 'pve-170', name: 'WINDOWS-VM', kind: 'vm', pve_vmid: 170 };
  const f = fixture(t, { configured: true, desktops: [pc, vm], assignments: [{ desktop_id: pc.id, username: 'fixture-admin' }, { desktop_id: vm.id, username: 'fixture-admin' }] });
  const pane = f.render(pc.id).querySelector('.dt-main');
  assert.equal(pane.querySelector('.dt-head h2').textContent, '업무 PC');
  assert.equal(pane.querySelector('.dt-head small').textContent, '로컬 PC');
  // A PC that does not answer is most likely off: its state says so, and nothing stands above the form table.
  assert.equal(pane.querySelector('.dt-alert'), null);
  assert.equal(pane.querySelector('.desktop-host-summary .state-dot').textContent, '응답 없음');
  assert.deepEqual([...pane.querySelectorAll('button')].map(b => b.textContent.trim()).filter(label => powerLabels.includes(label)), [], 'no power button anywhere on a local PC');
  const power = [...pane.querySelectorAll('.desktop-host-summary dt')].find(node => node.textContent === '전원').nextSibling.firstChild;
  assert.equal(power.textContent, '클라이언트에서 켜고 끔'); assert.equal(power.title, '배정된 사용자가 클라이언트의 ⏻에서 끄고 켜요.');
  assert.deepEqual([...pane.querySelectorAll('.win-foot button')].map(b => b.textContent), ['삭제', '이름 변경', '닫기'], 'the bar of a PC nobody is connected to holds 삭제 (far left), 이름 변경 and 닫기 only');
  assert.equal(pane.querySelector('a[href^="/console/"]'), null, 'no Proxmox console for a PC, even when it is assigned to this administrator');
  assert.equal(pane.querySelector('.display-panel'), null);
  assert.match(pane.textContent, /긴급 콘솔지원 안 함/);
  assert.equal([...pane.querySelectorAll('dd span')].find(node => node.textContent === '지원 안 함').title, '로컬 PC에서는 지원하지 않아요.', 'the reason is the title, not a sentence in the cell');
  assert.equal(f.find(pane, '표시 정보 수정'), undefined);
  // The exported action refuses a PC whatever calls it.
  for (const action of ['start', 'shutdown', 'reboot']) powerForm(f.ctx, pc, action);
  assert.equal(f.dialogs.length, 0); assert.equal(f.writes.length, 0);
  // The VM beside it does have them, so the absence above is the PC's.
  const vmPane = f.render(vm.id).querySelector('.dt-main');
  assert.deepEqual([...new Set([...vmPane.querySelectorAll('button')].map(b => b.textContent.trim()).filter(label => powerLabels.includes(label)))].sort(), ['끄기', '다시 시작', '켜기'].sort());
  assert.equal(vmPane.querySelector('.win-foot a[href="/console/pve-170"]').textContent, '화면 보기 (Console)');
  assert.ok(vmPane.querySelector('.display-panel'));
  assert.equal(f.find(vmPane, '이름 변경'), undefined);
  f.find(pane, '이름 변경').click();
  assert.equal(f.dialogs[0].title, '로컬 PC 이름 변경');
  await assert.rejects(f.dialogs[0].action({ name: '   ' })); assert.equal(f.writes.length, 0);
  await f.dialogs[0].action({ name: '새 PC 이름' });
  assert.deepEqual(f.writes[0], { path: '/desktops/pc-fixture', method: 'PUT', body: { name: '새 PC 이름' } });
});

test('existing VM approval keeps the original VM endpoint and identity choice', async t => {
  const f = fixture(t, { requests: [request({ candidates: [{ desktopId: 'pve-170', vmid: 170, name: '업무 VM' }], localPcRegistration: { allowed: false } })] });
  f.find(f.cards(), '확인하고 승인').click();
  assert.equal(f.dialogs[0].title, 'Host 연결 승인');
  await assert.rejects(f.dialogs[0].action({ desktopId: 'pve-170' }), /코드가 같아요/); assert.deepEqual(f.writes, [], 'not ticked: nothing is sent');
  await f.dialogs[0].action({ desktopId: 'pve-170', acknowledgeCode: 'yes' });
  assert.equal(f.writes[0].path, '/hosts/requests/fixture-request/approve');
  assert.equal(f.writes[0].body.desktopId, 'pve-170'); assert.equal(f.writes[0].body.acknowledgeCode, true);
});

test('changed displayed code blocks approval and a pending request cannot submit twice', async t => {
  const f = fixture(t, { requests: [request()] });
  f.find(f.cards(), '로컬 PC로 등록').click();
  f.requests[0].verificationCode = 'ZZZZ-9999';
  await assert.rejects(f.dialogs[0].action({ name: 'PC', acknowledgeCode: 'yes' }), /바뀌었/);
  assert.equal(f.writes.length, 0);
  f.requests[0].verificationCode = 'ABCD-EFGH';
  let finish;
  f.ctx.api = (path, method, body) => { f.writes.push({ path, method, body }); return new Promise(resolve => { finish = resolve; }); };
  const first = f.dialogs[0].action({ name: 'PC', acknowledgeCode: 'yes', acknowledgeUnlinked: 'yes' });
  try {
    await f.dialogs[0].action({ name: 'PC', acknowledgeCode: 'yes', acknowledgeUnlinked: 'yes' });
    assert.equal(f.writes.length, 1);
  } finally { finish({}); await first; }
  // Committed: a later press of the same dialog sends nothing more.
  await f.dialogs[0].action({ name: 'PC', acknowledgeCode: 'yes', acknowledgeUnlinked: 'yes' });
  assert.equal(f.writes.length, 1);
});
