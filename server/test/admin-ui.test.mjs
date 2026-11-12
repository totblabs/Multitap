import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { parseHTML } from 'linkedom';
import { memoryStorage, loadAdmin, iso, workspaceFixture, registerFixtureDesktops, registeredWorkspaceFixture, clipDenyFixture, deviceKeysFixture, deviceDenyFixture, permissionFixture, ownerFixture, recheck, versionServer } from './admin-ui-harness.mjs';

// Helpers of this file (the shared harness stays as it is).
const press = (f, node) => { assert.ok(node, 'Missing control'); node.dispatchEvent(new f.window.Event('click')); return node; };
const typeInto = (f, node, value) => { node.value = value; node.dispatchEvent(new f.window.Event('input')); };
const listIds = f => [...f.content.querySelectorAll('.vm-list-item')].map(node => node.getAttribute('data-focus'));
// The row of the board a desktop stands in (its name is the link with data-focus=vm-pick-<id>).
const rowOf = (f, id) => f.content.querySelector('[data-focus=vm-pick-' + id + ']').closest('tr');
// 데스크톱's 조회 조건 box: the state is select[data-focus=desktops-state] with the options
// all|bad|live|ok|off|update|manual, beside the search text; the list changes when 조회 is pressed.
const stateSelect = f => f.content.querySelector('select[data-focus=desktops-state]');
const stateOptions = f => [...stateSelect(f).querySelectorAll('option')];
const stateTexts = f => stateOptions(f).map(node => node.textContent);
const shownState = f => stateOptions(f).filter(node => node.hasAttribute('selected')).map(node => node.getAttribute('value'));
const query = f => press(f, f.find(f.content.querySelector('.query-box'), 'button', '조회'));
const pickState = (f, key) => { f.change(stateSelect(f), key); return query(f); };
// The window (창) of the desktop or the account that is open, and its tabs.
const win = f => f.content.querySelector('#detail .win-frame');
const tabNames = f => [...f.content.querySelectorAll('#detail [role=tab]')].map(node => node.textContent);
const openTab = f => f.content.querySelector('#detail [role=tab][aria-selected=true]').textContent;
const panelOf = (f, node) => node.closest('[role=tabpanel]');
// Opening 데스크톱 synchronises the Proxmox inventory; every other write is the page's own.
const ownWrites = f => f.writes.filter(write => !write.path.endsWith('/proxmox/sync'));
// The desktops the global search offers (accounts carry no "VM"/"로컬 PC" prefix in these fixtures).
const searchOffers = (f, query) => { typeInto(f, document.querySelector('#global-search'), query); return [...document.querySelectorAll('#global-search-results .search-option strong')].map(node => node.textContent); };

test('registered Desktop list keeps seven enrolled entries, problems first, including offline and powered off without all-VM access',async t=>{
  const {user,inventory,monitor}=registeredWorkspaceFixture();const before=JSON.stringify({desktops:inventory.desktops,assignments:inventory.assignments});
  const f=await loadAdmin(t,'registered-workspace-default',user,inventory,{hash:'#/desktops',monitor});
  const ids=()=>listIds(f);
  assert.equal(ids().length,7);assert.ok(ids().includes('vm-pick-pve-105'),'powered-off registered VM retained');assert.ok(ids().includes('vm-pick-pve-106'),'offline registered VM retained');assert.ok(ids().includes('vm-pick-pc-fixture'),'approved offline PC retained');
  // Problems first (접속 불가, 접속 중, 접속 가능, 꺼짐); VMs by number, then local PCs.
  assert.deepEqual(ids(),['vm-pick-pve-106','vm-pick-pve-107','vm-pick-pve-108','vm-pick-pve-109','vm-pick-pve-110','vm-pick-pve-105','vm-pick-pc-fixture']);
  assert.deepEqual([...f.content.querySelectorAll('.vm-list-item')].map(n=>n.getAttribute('data-status')),['bad','ok','ok','ok','ok','off','off']);
  // Why stands once, beside the name; the small line under the name always says what the desktop is and whose (on a phone too, where the reason must not show twice in its place).
  assert.equal(f.content.querySelector('[data-focus=vm-pick-pve-106]').closest('td').querySelector('.vm-list-why').textContent,'응답 없음','the row of a problem says why');
  assert.match(f.content.querySelector('[data-focus=vm-pick-pve-106]').getAttribute('aria-label'),/접속 불가 · 응답 없음/);
  assert.equal(f.content.querySelector('[data-focus=vm-pick-pve-106] small').textContent,'VM 106 (fixture) · 사용자 없음');
  assert.match(f.content.querySelector('[data-focus=vm-pick-pc-fixture]').textContent,/로컬 PC/);
  assert.equal(f.content.querySelector('[data-desktop-scope=all]'),null);
  assert.equal(f.content.querySelector('.list-count>span').textContent,'총 7대 · 접속 불가 1 · 접속 중 0 · 접속 가능 4 · 꺼짐 2');assert.equal(f.content.querySelector('.list-count>small').textContent,'정렬: 문제 먼저');
  assert.deepEqual(stateTexts(f),['전체 7','접속 불가 1','접속 중 0','접속 가능 4','꺼짐 2'],'counts of the registered desktops only; no update choices without an update');
  assert.equal(win(f),null,'the page shows only the list: no desktop is open until its row is pressed');assert.equal(f.content.querySelector('[aria-current=true]'),null);
  assert.equal(f.content.querySelector('tbody tr td.col-num').textContent,'1');assert.equal(f.content.querySelector('tbody tr .vm-list-item').getAttribute('data-focus'),'vm-pick-pve-106','the first row is the first registered desktop, never the lowest infrastructure VM');
  assert.equal(f.find(f.content,'button','전체 VM 보기'),undefined);
  assert.ok(!ids().includes('vm-pick-pve-100'),'connected flag alone cannot expose an unregistered VM');
  assert.equal(JSON.stringify({desktops:inventory.desktops,assignments:inventory.assignments}),before,'view scope never mutates inventory or assignments');
  assert.deepEqual(ownWrites(f),[]);
});

test('global search offers registered desktops only, whatever a runtime flag says',async t=>{
  const {user,inventory,monitor}=registeredWorkspaceFixture();
  const f=await loadAdmin(t,'registered-global-search',user,inventory,{hash:'#/desktops',monitor});
  assert.equal(document.querySelector('#global-search').getAttribute('placeholder'),'데스크톱·사용자 검색');
  assert.deepEqual(searchOffers(f,'Windows 11'),['VM 110 · Windows 110'],'VM 111-119 exist in Proxmox and are not registered');
  assert.deepEqual(searchOffers(f,'Infrastructure'),[],'VM 100 reports a connected Host and is not registered');
  assert.deepEqual(searchOffers(f,'Approved'),['로컬 PC · Approved PC']);
  assert.deepEqual(ownWrites(f),[]);
});

// The search fails closed like the list: an unknown or failed Host list offers no desktop.
test('global search does not fall back to every Proxmox VM while the Host list could not be read',async t=>{
  const {user,inventory,monitor}=registeredWorkspaceFixture();
  const f=await loadAdmin(t,'registered-global-search-unknown',user,inventory,{hash:'#/desktops',monitor,respond:url=>url.endsWith('/hosts')?{ok:false,status:503,json:async()=>({error:{code:'temporary_failure'}})}:null});
  assert.match(f.content.textContent,/Host 등록 정보를 확인하지 못했어요/);assert.equal(listIds(f).length,0);
  assert.deepEqual(searchOffers(f,'Windows 11'),[],'an unknown registration must not list unregistered VMs');
  assert.deepEqual(searchOffers(f,'Infrastructure'),[]);
});

test('explicit unregistered Desktop deep link does not expose its detail or expand membership',async t=>{
  const {user,inventory,monitor}=registeredWorkspaceFixture();const f=await loadAdmin(t,'registered-workspace-deeplink',user,inventory,{hash:'#/desktops/pve-100',monitor});
  assert.equal(f.content.querySelector('[data-desktop-scope=all]'),null);
  assert.equal(win(f),null);assert.equal(f.content.querySelector('.dt-head'),null);assert.equal(f.content.querySelector('#desktop-people'),null);
  assert.equal(listIds(f).length,0);
  assert.match(f.content.textContent,/데스크톱을 찾지 못했어요/);assert.match(f.content.textContent,/등록된 목록에 없는 데스크톱이에요/);
  assert.deepEqual(ownWrites(f),[]);
});

test('unknown or failed Host registration resource never falls back to all desktops or false empty onboarding',async t=>{
  const {user,inventory,monitor}=registeredWorkspaceFixture();let release;let failed=false;
  const f=await loadAdmin(t,'registered-workspace-loading',user,inventory,{hash:'#/desktops',monitor,respond:url=>url.endsWith('/hosts')?(failed?{ok:false,status:503,json:async()=>({error:{code:'temporary_failure'}})}:new Promise(resolve=>{release=resolve;})):null});
  assert.equal(f.content.querySelectorAll('.vm-list-item').length,0);assert.match(f.content.textContent,/등록된 데스크톱을 확인/);assert.doesNotMatch(f.content.textContent,/등록된 데스크톱이 없어요/);
  release({ok:true,status:200,json:async()=>({hosts:monitor.hosts})});await f.settle();assert.equal(f.content.querySelectorAll('.vm-list-item').length,7);
  failed=true;document.querySelector('#refresh').dispatchEvent(new f.window.Event('click'));await f.settle();
  assert.match(f.content.textContent,/Host 등록 정보를 확인하지 못했어요/);assert.equal(f.content.querySelectorAll('.vm-list-item').length,0);assert.doesNotMatch(f.content.textContent,/등록된 데스크톱이 없어요/);
});

test('zero registered desktops explains onboarding and keeps the pending request on the page without all-VM escape',async t=>{
  const {user,inventory}=registeredWorkspaceFixture();inventory.desktops=inventory.desktops.filter(d=>d.kind==='vm');
  const f=await loadAdmin(t,'registered-workspace-empty',user,inventory,{hash:'#/desktops',monitor:{hosts:[],requests:[{requestId:'fixture',hostname:'new',version:'1.0.0',machineUuid:'fixture',status:'pending',expiresAt:'2099-01-01T00:00:00Z',candidates:[]}]}});
  assert.match(f.content.textContent,/등록된 데스크톱이 없어요/);assert.match(f.content.textContent,/Windows에 Host를 설치하고 연결 요청을 승인하면 표시돼요/);
  // The request waits in 승인 대기 at the top of the same page.
  const waiting=f.content.querySelector('section.requests-panel');
  assert.match(waiting.querySelector('h2').textContent,/^승인 대기/);assert.equal(waiting.querySelector('h2 .count').textContent,'1');
  assert.equal(waiting.querySelectorAll('.request-card').length,1);assert.equal(waiting.querySelector('.request-card strong').textContent,'new');
  assert.match(waiting.querySelector('.request-match').textContent,/Proxmox에서 맞는 VM을 찾지 못했어요/);
  assert.ok(f.find(waiting,'button','직접 입력'));assert.ok(f.find(waiting,'button','거절'));
  assert.equal(f.find(waiting,'button','로컬 PC로 등록'),undefined,'the server did not offer a local PC registration for this request');
  assert.equal(f.content.querySelectorAll('.vm-list-item').length,0);assert.equal(f.find(f.content,'button','전체 VM 보기'),undefined);
  assert.deepEqual(stateTexts(f),['전체 0','접속 불가 0','접속 중 0','접속 가능 0','꺼짐 0'],'thirty Proxmox VMs, none registered, none counted');
  assert.deepEqual(ownWrites(f),[]);
});

test('malformed Host enrollment rows show a retryable error without exposing full inventory',async t=>{
  const {user,inventory}=registeredWorkspaceFixture();
  const f=await loadAdmin(t,'registered-workspace-invalid-authority',user,inventory,{hash:'#/desktops',monitor:{hosts:[{}]}});
  assert.match(f.content.textContent,/Host 등록 정보를 확인하지 못했어요/);assert.equal(f.content.querySelectorAll('.vm-list-item').length,0);
  assert.doesNotMatch(f.content.textContent,/등록된 데스크톱이 없어요/);assert.ok(f.find(f.content,'button','다시 시도'));
});

test('keyboard skip link preserves the selected route and focuses its existing main content', async t => {
  const { user, inventory } = workspaceFixture();
  const f = await loadAdmin(t, 'skip-link-route-regression', user, inventory, { hash: '#/desktops/pve-170' });
  const before = globalThis.location.hash, crumb = f.crumb();
  const event = new f.window.Event('click', { bubbles: true, cancelable: true });
  document.querySelector('.skip-link').dispatchEvent(event);
  assert.equal(event.defaultPrevented, true, 'native anchor navigation must not reach the hash router');
  assert.equal(globalThis.location.hash, before); assert.equal(f.crumb(), crumb);
  assert.equal(f.focused() === f.content, true);
});

test('mobile drawer confines both keyboard boundaries and restores prior inert state on close and navigation', async t => {
  const { user, inventory } = workspaceFixture();
  const f = await loadAdmin(t, 'drawer-keyboard-isolation', user, inventory, { hash: '#/desktops', narrow: true });
  const sidebar = document.querySelector('#sidebar'), main = document.querySelector('.main'), top = document.querySelector('.topbar');
  top.setAttribute('inert', ''); top.inert = true;
  const toggle = document.querySelector('#nav-toggle'); toggle.dispatchEvent(new f.window.Event('click'));
  assert.equal(main.hasAttribute('inert'), true); assert.equal(top.hasAttribute('inert'), true);
  const first = sidebar.querySelector('a[href]'), last = sidebar.querySelector('[data-page=licenses]');
  assert.equal(document.querySelectorAll('#sidebar .nav a,#sidebar .nav button').length, 14, 'the drawer of a phone or a narrow window holds the whole grouped list');
  const key = (node, name, shiftKey = false) => { const event = new f.window.Event('keydown', { bubbles: true, cancelable: true }); Object.assign(event, { key: name, shiftKey }); node.dispatchEvent(event); return event; };
  last.focus(); assert.equal(key(last, 'Tab').defaultPrevented, true); assert.equal(f.focused() === first, true);
  first.focus(); assert.equal(key(first, 'Tab', true).defaultPrevented, true); assert.equal(f.focused() === last, true);
  key(last, 'Escape'); assert.equal(main.hasAttribute('inert'), false); assert.equal(top.hasAttribute('inert'), true); assert.equal(top.inert, true);
  assert.equal(f.focused() === toggle, true); assert.equal(toggle.getAttribute('aria-expanded'), 'false');
  toggle.dispatchEvent(new f.window.Event('click')); f.navigate('users'); await f.settle();
  assert.equal(main.hasAttribute('inert'), false); assert.equal(document.querySelector('#console').classList.contains('nav-open'), false);
  assert.equal(f.focused() === f.content.querySelector('h1'), true);
});

test('phone menu: the top bar\'s menu button opens the whole grouped list, an entry opens its page and closes the menu, and no strip of places is left', async t => {
  const { user, inventory } = workspaceFixture();
  const f = await loadAdmin(t, 'phone-menu', user, inventory, { hash: '#/desktops', narrow: true });
  const shell = document.querySelector('#console'), toggle = document.querySelector('#nav-toggle'), main = document.querySelector('.main');
  const open = () => { toggle.dispatchEvent(new f.window.Event('click')); assert.equal(shell.classList.contains('nav-open'), true); assert.equal(toggle.getAttribute('aria-expanded'), 'true'); assert.equal(main.hasAttribute('inert'), true); };
  const closed = () => { assert.equal(shell.classList.contains('nav-open'), false); assert.equal(toggle.getAttribute('aria-expanded'), 'false'); assert.equal(main.hasAttribute('inert'), false); assert.equal(document.querySelector('#nav-scrim').hidden, true); };
  assert.equal(toggle.closest('.topbar') !== null, true); assert.equal(toggle.getAttribute('aria-controls'), 'sidebar'); assert.equal(toggle.getAttribute('aria-label'), '메뉴');
  closed();
  // The same list as the wide sidebar: three groups, the two group headings, every entry, the current page marked, the badges.
  open();
  assert.deepEqual([...document.querySelectorAll('#sidebar .nav-group')].map(group => group.querySelectorAll('a,button').length), [6, 2, 6]);
  assert.deepEqual([...document.querySelectorAll('#sidebar .nav-heading')].map(node => node.textContent), ['운영', '업데이트', '설정']); assert.equal(document.querySelector('#nav-settings-heading').textContent, '설정');
  assert.equal(document.querySelector('#sidebar [aria-current=page]').dataset.page, 'desktops'); assert.ok(f.focused() === document.querySelector('#sidebar [data-page=desktops]'), 'the menu opens on the current page');
  assert.equal(document.querySelectorAll('#sidebar .nav-badge').length, 2);
  // A settings page straight from the menu.
  document.querySelector('#sidebar [data-page=licenses]').dispatchEvent(new f.window.Event('click')); await f.settle();
  closed(); assert.equal(f.crumb(), '오픈소스 라이선스'); assert.equal(f.content.querySelector('h1').textContent, '오픈소스 라이선스');
  open(); assert.equal(document.querySelector('#sidebar [aria-current=page]').dataset.page, 'licenses');
  // 계정 보안 closes the menu before its dialog opens; the page behind stays.
  document.querySelector('#nav-account-security').dispatchEvent(new f.window.Event('click')); await f.settle();
  closed(); assert.equal(f.crumb(), '오픈소스 라이선스'); assert.ok(f.requests.some(path => path.endsWith('/mfa')));
  // The scrim and the close button leave the page as it was.
  open(); document.querySelector('#nav-scrim').dispatchEvent(new f.window.Event('click')); closed();
  open(); document.querySelector('#nav-close').dispatchEvent(new f.window.Event('click')); closed(); assert.equal(f.crumb(), '오픈소스 라이선스');
  // Layout rules: on phones the sidebar stays the drawer of narrow windows. Nothing lays the places out as a
  // row or strip, hides the menu button or the drawer's heading, and every row is one 36 px line (the frame is product-ui.css).
  const css = Object.fromEntries(await Promise.all(['product-ui.css', 'usage.css', 'workspace.css', 'console.css'].map(async name => [name, await readFile(new URL('../admin/' + name, import.meta.url), 'utf8')])));
  assert.match(css['product-ui.css'], /@media\(max-width:1000px\)\{[^@]*#console>\.sidebar\{position:fixed;[^}]*transform:translateX\(-102%\)[^}]*\}#console\.nav-open>\.sidebar\{transform:none;visibility:visible\}/);
  assert.match(css['product-ui.css'], /@media\(max-width:1000px\)\{[^@]*#console \.topbar \.nav-toggle\{display:inline-flex\}/);
  for (const name of ['product-ui.css', 'usage.css']) {
    assert.doesNotMatch(css[name], /#console \.nav a\{[^}]*flex-direction:column/, name + ' stacks no icon over a label');
    assert.doesNotMatch(css[name], /#console \.sidebar \.nav\{[^}]*(?:flex-direction:row|overflow-x:auto)/, name + ' has no strip of places');
    assert.doesNotMatch(css[name], /\.nav-toggle[^{}]*\{display:none\}/, name + ' never hides the menu button');
    assert.doesNotMatch(css[name], /#console \.sidebar-head[^{}]*\{display:none\}/, name);
  }
  assert.match(css['product-ui.css'], /#console \.nav a,#console \.nav \.nav-action\{[^}]*min-height:36px;/);
  assert.match(css['product-ui.css'], /#console \.nav-group-later,#console \.nav-group-settings\{[^}]*border-top:1px solid var\(--line\)\}/, 'dividers between the groups');
  assert.deepEqual(f.writes.filter(write => !write.path.endsWith('/proxmox/sync')), []);
});

test('사용자: administrators are the pinned first group of the list, with a count, and the role filter still narrows it', async t => {
  const user = { id: 'owner', username: 'owner', role: 'admin', mustChangeCredentials: false };
  const account = (id, username, role, extra = {}) => ({ id, username, role, disabled: 0, notes: '', ...extra });
  const inventory = { users: [account('u1', 'alice', 'user'), account('u2', 'bora', 'admin'), account('u3', 'chul', 'user', { disabled: 1 }), account('owner', 'owner', 'admin', { is_primary_admin: 1 })], desktops: [], assignments: [], audit: [], proxmox: { configured: true } };
  const f = await loadAdmin(t, 'users-admin-group', user, inventory, { hash: '#/users' });
  const rail = () => [...f.content.querySelectorAll('.account-table tbody > tr')].map(node => node.classList.contains('account-group') ? [node.querySelector('span').textContent, node.querySelector('.count').textContent] : node.querySelector('a.account-choice').getAttribute('aria-label'));
  const apply = () => f.click(f.content.querySelector('.query-box'), '조회');
  assert.deepEqual(rail(), [['관리자', '2'], 'bora', 'owner', ['사용자', '2'], 'alice', 'chul'], 'administrators first, then everyone else, each in name order');
  assert.match(f.content.querySelector('.account-choice[aria-label=owner]').getAttribute('aria-description'), /^메인 관리자/);
  const role = f.content.querySelector('select[aria-label="권한 필터"]');
  f.change(role, 'admin'); apply(); await f.settle();
  assert.deepEqual(rail(), [['관리자', '2'], 'bora', 'owner'], 'a group with nobody shows no heading');
  f.change(f.content.querySelector('select[aria-label="권한 필터"]'), 'user'); apply(); await f.settle();
  assert.deepEqual(rail(), [['사용자', '2'], 'alice', 'chul']);
  // There is no 관리자 계정 entry in the sidebar: this list is where they are.
  assert.equal([...document.querySelectorAll('#sidebar .nav a,#sidebar .nav button')].some(node => /관리자/.test(node.textContent)), false);
  assert.deepEqual(f.writes, []);
});

test('shared modal identifies and focuses confirmation mismatch, then focuses request failure without erasing inputs', async t => {
  const { user, inventory } = workspaceFixture();
  const f = await loadAdmin(t, 'modal-error-association', user, inventory, { hash: '#/users', respond: (url, options) => url.endsWith('/users') && options.method === 'POST' ? { ok: false, status: 409, json: async () => ({ error: { code: 'username_exists' } }) } : null });
  f.click(f.content, '사용자 만들기');
  assert.equal(document.querySelector('#modal-title').textContent, '사용자 만들기');
  const form = document.querySelector('#modal-form'), fields = form.elements;
  fields.username.value = '합성테스트'; fields.password.value = 'Synthetic-Only-Password-1!'; fields.passwordConfirmation.value = 'Synthetic-Only-Password-2!';
  await form.onsubmit({ preventDefault() {} });
  assert.equal(f.writes.filter(row => row.path.endsWith('/users')).length, 0);
  assert.equal(f.focused() === fields.passwordConfirmation, true);
  assert.equal(fields.passwordConfirmation.getAttribute('aria-invalid'), 'true');
  assert.ok(fields.passwordConfirmation.getAttribute('aria-describedby').split(/\s+/).includes('modal-error'));
  fields.passwordConfirmation.value = fields.password.value;
  await form.onsubmit({ preventDefault() {} });
  assert.equal(f.writes.filter(row => row.path.endsWith('/users')).length, 1);
  assert.equal(f.focused() === document.querySelector('#modal-error'), true);
  assert.equal(fields.passwordConfirmation.hasAttribute('aria-invalid'), false);
  assert.equal(fields.username.value, '합성테스트'); assert.equal(f.dialog.open, true);
});

test('unload warns only for changed access settings and clears after revert, reload and verified save', async t => {
  const { user, inventory } = workspaceFixture();
  const applied = { protocol: 'server-access-v1', primaryOrigin: 'https://broker.fixture', https: { enabled: true, origin: 'https://broker.fixture', mode: 'proxy', listenHost: '127.0.0.1', listenPort: 18480, certFile: null, keyFile: null }, http: { enabled: false, origin: null, listenHost: '127.0.0.1', listenPort: 18481 }, trustedProxies: ['127.0.0.1'] };
  let snapshot = { protocol: 'server-access-v1', revision: 'a'.repeat(64), applied, pending: null, supported: true, managementOrigin: 'https://broker.fixture' };
  const ok = value => ({ ok: true, status: 200, json: async () => value });
  const f = await loadAdmin(t, 'access-unload-draft-guard', user, inventory, { hash: '#/server-access', respond: (url, options) => {
    if (url.endsWith('/server-access')) return ok(snapshot);
    if (url.endsWith('/server-access/test')) return ok({ revision: snapshot.revision, ok: true, checks: [{ id: 'fixture', ok: true }], testId: 'synthetic-test', expiresAt: Date.now() + 60000 });
    if (url.endsWith('/server-access/save')) { snapshot = { ...snapshot, revision: 'b'.repeat(64), applied: JSON.parse(options.body).config, operationId: 'synthetic-operation', state: 'applied' }; return ok({ operationId: snapshot.operationId, state: 'applied' }); }
  } });
  const warns = () => f.emitWindow('beforeunload').defaultPrevented;
  const input = () => f.content.querySelector('[name=trustedProxies]');
  const edit = value => { input().value = value; input().dispatchEvent(new f.window.Event('input')); };
  assert.equal(warns(), false, 'viewing settings does not warn');
  edit('127.0.0.1\n192.0.2.1'); assert.equal(warns(), true);
  edit('127.0.0.1'); assert.equal(warns(), false, 'returning to applied values is clean');
  edit('127.0.0.1\n192.0.2.1'); f.click(f.content, '현재 설정 다시 불러오기'); await f.settle();
  assert.match(f.dialog.textContent, /바꾼 내용을 버릴까요/); assert.equal(warns(), true, 'the draft stays until the operator confirms');
  document.querySelector('#modal-form').dispatchEvent(new f.window.Event('submit')); await f.settle(); assert.equal(warns(), false, 'discarding the draft is clean');
  edit('127.0.0.1\n192.0.2.1'); f.click(f.content, '연결 테스트'); await f.settle(); f.click(f.content, '변경 저장');
  assert.equal(warns(), true, 'opening confirmation does not save');
  await document.querySelector('#modal-form').onsubmit({ preventDefault() {} }); await f.settle();
  assert.equal(warns(), false, 'the exact applied revision clears the saved draft');
  assert.equal(f.writes.filter(row => row.path.endsWith('/server-access/save')).length, 1);
});

test('unload ignores unchanged and readonly dialogs but protects actual edits until reverted or cancelled', async t => {
  // The open installation guide polls every three seconds. Keep its timer
  // owned by this fixture so it cannot read another test's restored DOM.
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const { user, inventory } = workspaceFixture();
  const f = await loadAdmin(t, 'modal-unload-draft-guard', user, inventory, { hash: '#/users' });
  const warns = () => f.emitWindow('beforeunload').defaultPrevented;
  f.click(f.content, '사용자 만들기'); assert.equal(warns(), false);
  const field = document.querySelector('#modal [name=username]'); field.value = '합성초안'; assert.equal(warns(), true);
  field.value = ''; assert.equal(warns(), false); field.value = '다시편집';
  f.dialog.close(); assert.equal(warns(), false, 'cancelled fields are no longer an unsaved draft');
  f.navigate('desktops'); await f.settle(); f.click(f.content, '데스크톱 추가'); await f.settle();
  assert.equal(f.dialog.open, true); assert.equal(document.querySelector('#modal-title').textContent, '데스크톱 추가');
  assert.equal(warns(), false, 'a readonly installation guide is safe to leave');
  assert.deepEqual(ownWrites(f), []);
});

test('a desktop opens in a window with 쓰는 사람 on 사용자 and a target-bound assignment that starts with nothing shared, its Host details on 자세히', async t => {
  const {user,inventory}=workspaceFixture();
  const member={id:'simple-member',username:'배정할 사용자',role:'user',disabled:0};
  inventory.users.push(member,{id:'disabled-member',username:'중지된 사용자',role:'user',disabled:1});
  inventory.assignments.push({username:member.username,desktop_id:'pve-101',clipboardPolicy:{filesToRemote:true},devicePolicy:{printers:true}});
  const f=await loadAdmin(t,'direct-desktop-assignment',user,inventory,{hash:'#/desktops/pve-170'});
  assert.equal(f.content.querySelector('.section-navigation'),null);
  // The window opens on 기본 정보; the people are on 사용자 and what the Host reports on 자세히.
  assert.deepEqual(tabNames(f),['기본 정보','사용자 0','최근 기록','자세히']);assert.equal(openTab(f),'기본 정보');
  const people=()=>f.content.querySelector('#desktop-people');
  assert.match(people().querySelector('h2').textContent,/^쓰는 사람/);assert.match(people().textContent,/아직 아무도 쓰지 않아요/);
  assert.equal(panelOf(f,people()).getAttribute('data-tab'),'users');
  assert.deepEqual([...f.content.querySelectorAll('details.dt-more')],[],'deleting is 삭제 in the bar, not folded in 자세히');
  assert.equal(f.content.querySelector('.win-foot .win-actions').firstElementChild.textContent,'삭제','삭제 stands first, at the far left of the bar');
  f.click(people(),'사용자 배정');
  assert.equal(document.querySelector('#modal-title').textContent,'사용자 배정');
  assert.match(f.dialog.textContent,/VM 170 · 업무 B/);assert.match(f.dialog.textContent,/새 배정은 ‘기본’\(아무것도 주고받지 않음\)으로 시작해요/);
  const select=f.dialog.querySelector('[name=username]');assert.ok([...select.options].some(option=>option.value===member.username));
  assert.equal([...select.options].some(option=>option.value==='중지된 사용자'),false);
  assert.equal(f.writes.some(write=>write.path.endsWith('/assignments/batch')),false,'opening the picker never assigns');
  f.change(select,member.username);document.querySelector('#modal-form').dispatchEvent(new f.window.Event('submit'));await f.settle();
  const saves=f.writes.filter(write=>write.path.endsWith('/assignments/batch'));assert.equal(saves.length,1,'exactly one request');
  assert.deepEqual(saves[0].data,{username:member.username,expectedAccountId:member.id,desktopIds:['pve-101','pve-170']});
  const prior=inventory.assignments.find(row=>row.username===member.username&&row.desktop_id==='pve-101');assert.equal(prior.clipboardPolicy.filesToRemote,true);assert.equal(prior.devicePolicy.printers,true);
  const added=inventory.assignments.find(row=>row.username===member.username&&row.desktop_id==='pve-170');assert.equal(added.clipboardPolicy,undefined,'no sharing permission is silently granted');
  assert.match(document.querySelector('#notice').textContent,/배정할 사용자 님에게 업무 B 배정을 저장했어요/);
  const row=people().querySelector('li.person-row[data-person="배정할 사용자"]');
  assert.match(row.querySelector('small').textContent,/^공유: 기본/,'the new assignment shows the server default: nothing shared');
  assert.equal(f.writes.some(write=>write.path.endsWith('/permissions')),false);
});

test('Host updates show on the 홈 badge, the 상태 condition and 지금 할 일; a Host that cannot take one remotely is not counted and stays 접속 가능',async t=>{
  const {user,inventory}=workspaceFixture();
  inventory.hostUpdates={latest:{version:'1.0.12',changelog:'검증된 변경'},desktops:{'pve-170':{connected:true,version:'1.0.0',newer:true,supported:false,available:false,latestVersion:'1.0.12'},'pve-101':{connected:true,version:'1.0.10',newer:true,supported:true,available:true,latestVersion:'1.0.12'}}};
  const f=await loadAdmin(t,'global-host-actions',user,inventory,{hash:'#/desktops/pve-170'});
  assert.equal(f.content.querySelector('.host-attention'),null,'the strip that repeated these counts on every page is gone');
  // The sidebar number is the count of 지금 할 일 (one update).
  assert.equal(document.querySelector('[data-nav-badge=hosts]'),null);
  const badge=document.querySelector('[data-nav-badge=home]');
  assert.equal(badge.textContent,'1');assert.equal(badge.getAttribute('aria-label'),'지금 할 일 1가지');assert.equal(badge.hidden,false);
  assert.deepEqual(stateTexts(f),['전체 3','접속 불가 0','접속 중 0','접속 가능 2','꺼짐 1','업데이트 1']);
  pickState(f,'ok');await f.settle();
  assert.deepEqual(listIds(f),['vm-pick-pve-101','vm-pick-pve-170'],'a Host without remote updates remains online');
  pickState(f,'update');await f.settle();
  assert.deepEqual(listIds(f),['vm-pick-pve-101'],'the automatic update never counts the Host that cannot take it');
  assert.equal(rowOf(f,'pve-101').querySelector('.vm-list-update').textContent,'업데이트');
  assert.ok(f.find(f.content.querySelector('.update-bar'),'button','1대 업데이트'));
  f.navigate('overview');await f.settle();const todo=f.content.querySelector('.todo-panel');
  assert.doesNotMatch(todo.textContent,/직접 설치/);assert.match(todo.textContent,/데스크톱 1대를 업데이트할 수 있어요/);assert.match(todo.textContent,/Host 1\.0\.12 · Windows는 다시 시작하지 않아요/);
  f.follow(todo,'업데이트할 데스크톱 보기');await f.settle();
  assert.equal(f.crumb(),'데스크톱');assert.deepEqual(shownState(f),['update']);assert.deepEqual(listIds(f),['vm-pick-pve-101']);
  pickState(f,'ok');await f.settle();f.pickVm('pve-170');await f.settle();
  assert.equal(f.content.querySelector('.dt-head h2').textContent,'업무 B');assert.match(f.content.querySelector('.dt-main .desktop-host-summary .host-cell').textContent,/^1\.0\.0원격 업데이트 미지원$/);
  const update=f.find(f.content.querySelector('.dt-main .win-foot'),'button','Host 업데이트');
  assert.ok(update.disabled,'no remote update can be started for it');
  assert.deepEqual(ownWrites(f),[]);
});

test('the 상태 condition is the one filter at every width: counts equal the rows they show and focus stays', async t => {
  const { user, inventory } = workspaceFixture();
  inventory.hostUpdates = { latest: { version: '1.0.12' }, desktops: {
    'pve-170': { connected: true, version: '1.0.0', newer: true, supported: false, available: false },
    'pve-101': { connected: true, version: '1.0.10', newer: true, supported: true, available: true, latestVersion: '1.0.12' }
  } };
  const f = await loadAdmin(t, 'compact-host-status', user, inventory, { hash: '#/desktops', narrow: true });
  // One control chooses the state, in the 조회 조건 box; no second one could disagree with it.
  assert.equal(f.content.querySelector('.host-status-compact'), null); assert.equal(f.content.querySelector('.chips,.chip'), null); assert.equal(f.content.querySelectorAll('.query-box select').length, 1);
  assert.equal(f.content.querySelector('.query-box').getAttribute('aria-label'), '데스크톱 조회 조건'); assert.equal(stateSelect(f).closest('label').firstChild.textContent, '상태');
  const keys = ['all', 'bad', 'live', 'ok', 'off', 'update'];
  assert.deepEqual(stateOptions(f).map(node => node.getAttribute('value')), keys, 'no choice offers unregistered VMs');
  assert.deepEqual(stateTexts(f), ['전체 3', '접속 불가 0', '접속 중 0', '접속 가능 2', '꺼짐 1', '업데이트 1']);
  assert.deepEqual(shownState(f), ['all']); assert.equal(listIds(f).length, 3);
  // Choosing a state changes nothing until 조회 is pressed.
  f.change(stateSelect(f), 'off'); await f.settle(); assert.equal(listIds(f).length, 3); f.change(stateSelect(f), 'all');
  for (const key of keys.slice(1)) {
    const count = Number(/ (\d+)$/.exec(stateOptions(f).find(node => node.getAttribute('value') === key).textContent)[1]);
    pickState(f, key); await f.settle();
    assert.deepEqual(shownState(f), [key]);
    assert.ok(f.focused() === f.find(f.content.querySelector('.query-box'), 'button', '조회'), `${key}: the redraw of 조회 retains keyboard focus`);
    assert.equal(listIds(f).length, count, `${key}: the number of the choice is the number of rows`);
    assert.deepEqual(stateTexts(f), ['전체 3', '접속 불가 0', '접속 중 0', '접속 가능 2', '꺼짐 1', '업데이트 1'], 'a filter never changes the counts');
  }
  pickState(f, 'bad'); await f.settle();
  assert.match(f.content.querySelector('.vm-list-items').textContent, /맞는 데스크톱이 없어요/); assert.equal(f.content.querySelector('.vm-list-caption>span').textContent, '0 / 3대');
  pickState(f, 'off'); await f.settle();
  assert.match(f.content.querySelector('.vm-list-items').textContent, /Linux 보관/);
  assert.doesNotMatch(f.content.querySelector('.vm-list-items').textContent, /업무 A|업무 B/);
  assert.equal(f.content.querySelector('.vm-list-caption>span').textContent, '1 / 3대');
  f.click(f.content.querySelector('.query-box'), '초기화'); await f.settle();
  assert.deepEqual(shownState(f), ['all'], '초기화 shows everything again'); assert.equal(listIds(f).length, 3);
  assert.deepEqual(ownWrites(f), [], 'view filtering never installs, updates or changes enrollment');
});

test('server access redraw preserves the actual disclosure before a queued native toggle event', async t => {
  const { user, inventory } = workspaceFixture();
  const applied = { protocol: 'server-access-v1', primaryOrigin: 'https://broker.fixture',
    https: { enabled: true, origin: 'https://broker.fixture', mode: 'proxy', listenHost: '127.0.0.1', listenPort: 18480, certFile: null, keyFile: null },
    http: { enabled: false, origin: null, listenHost: '127.0.0.1', listenPort: 18481 }, trustedProxies: ['127.0.0.1'] };
  const f = await loadAdmin(t, 'access-disclosure-native-toggle', user, inventory, { hash: '#/server-access',
    respond: url => url.endsWith('/server-access') ? { ok: true, status: 200, json: async () => ({
      protocol: 'server-access-v1', revision: 'a'.repeat(64), applied, pending: null, supported: true,
      managementOrigin: 'https://cloud.fixture.example' }) } : null });
  const disclosure = () => f.content.querySelector('[data-keep-open="access-https"]');
  assert.ok(disclosure());
  // No toggle event is dispatched: it has not run yet in this browser race.
  disclosure().setAttribute('open', '');
  f.change(f.content.querySelector('[name="https.mode"]'), 'direct');
  assert.equal(disclosure().hasAttribute('open'), true);
  assert.ok(disclosure().querySelector('[name="https.certFile"]'));
  assert.ok(disclosure().querySelector('[name="https.keyFile"]'));
  disclosure().removeAttribute('open');
  f.change(f.content.querySelector('[name="https.mode"]'), 'proxy');
  assert.equal(disclosure().hasAttribute('open'), false, 'closing before its toggle event is also preserved');
  assert.equal(f.writes.length, 0, 'opening details and choosing TLS mode never saves settings');
});

test('the window of a deep-linked desktop stays open while the list behind it is narrowed, and sends power only to that VM, once', async t => {
  const { user, inventory } = workspaceFixture();
  const f = await loadAdmin(t, 'workspace-filter-power', user, inventory, { hash: '#/desktops/pve-102',
    respond: url => url.endsWith('/power') ? { ok: false, status: 409, json: async () => ({ error: { code: 'power_in_progress' } }) } : null });
  assert.equal(f.crumb(), 'VM 102 · Linux 보관');
  // Connected desktops by number, then the one that is off.
  assert.deepEqual(listIds(f), ['vm-pick-pve-101', 'vm-pick-pve-170', 'vm-pick-pve-102']);
  f.change(stateSelect(f), 'ok');
  typeInto(f, f.content.querySelector('[data-focus=desktops-search]'), '업무 A'); query(f); await f.settle();
  assert.deepEqual(listIds(f), ['vm-pick-pve-101']);
  // The window does not depend on the list behind it: no note about the list is needed.
  assert.equal(f.content.querySelector('.vm-filter-context'), null);
  const head = () => f.content.querySelector('.dt-head'), main = () => f.content.querySelector('.dt-main');
  assert.equal(head().querySelector('h2').textContent, 'Linux 보관');
  assert.match(head().textContent, /VM 102/); assert.equal(head().querySelector('.state-dot').textContent, '꺼짐'); assert.equal(head().querySelector('.pill'), null);
  assert.equal(main().querySelector('.dt-alert'), null, 'off is not a problem to act on');
  const button = label => f.find(win(f), 'button', label);
  assert.equal(button('켜기').disabled, false); assert.equal(button('끄기'), undefined); assert.equal(button('다시 시작'), undefined);
  assert.equal([...win(f).querySelectorAll('button')].filter(node => node.textContent === '켜기').length, 1, 'one start control for a desktop that is off');
  assert.equal(button('켜기').closest('.win-foot') !== null, true, 'power stands in the bar at the bottom of the window'); assert.equal(button('켜기').className, 'primary win-main', 'what the state asks for is the main button, on the right before 닫기'); assert.equal(button('켜기').nextElementSibling.textContent, '닫기');
  f.click(win(f), '켜기');
  const power = () => f.writes.filter(w => w.path.endsWith('/power'));
  assert.deepEqual(power(), [{ path: '/api/v1/admin/desktops/pve-102/power', data: { action: 'start' } }], 'starting interrupts nothing: one request for the selected VM, no dialog');
  assert.notEqual(f.dialog.open, true);
  const busy = button('켜는 중…'); assert.equal(busy.disabled, true, 'a pending start cannot be submitted again');
  press(f, busy); assert.equal(power().length, 1);
  await f.settle();
  assert.equal(power().length, 1, 'a refused request is not retried');
  assert.equal(document.querySelector('#notice').className, 'is-error'); assert.match(document.querySelector('#notice').textContent, /이 데스크톱에서 전원 작업이 진행 중이에요/);
  assert.equal(button('켜기').disabled, false, 'the control returns after the refusal');
  f.click(f.content.querySelector('.query-box'), '초기화'); await f.settle();
  assert.deepEqual(shownState(f), ['all']); assert.equal(f.content.querySelector('[data-focus=desktops-search]').value, '');
  assert.equal(f.content.querySelector('.vm-list-item[aria-current=true]').getAttribute('data-focus'), 'vm-pick-pve-102');
  assert.equal(head().querySelector('h2').textContent, 'Linux 보관', 'clearing the conditions keeps the explicit deep-link target');
  assert.deepEqual(ownWrites(f), power());
});

test('the desktop list preserves the search, the state and node grouping across opening windows without repeating inventory synchronisation', async t => {
  const { user, inventory } = workspaceFixture();
  const f = await loadAdmin(t, 'workspace-switch-filters', user, inventory);
  f.navigate('desktops'); await f.settle();
  const syncs = () => f.requests.filter(p => p.endsWith('/proxmox/sync')).length;
  assert.equal(syncs(), 1);
  assert.equal(win(f), null, 'without an address no window is open');
  typeInto(f, f.content.querySelector('[data-focus=desktops-search]'), '업무');
  pickState(f, 'ok');
  f.change(f.content.querySelector('input[data-focus=desktops-by-node]'), true);
  const assertFilters = () => {
    assert.equal(f.content.querySelector('[data-focus=desktops-search]').value, '업무');
    assert.deepEqual(shownState(f), ['ok']);
    assert.equal(f.content.querySelector('input[data-focus=desktops-by-node]').checked, true);
    assert.deepEqual([...f.content.querySelectorAll('section.vm-list-group')].map(node => node.getAttribute('aria-label')), ['Proxmox pve-fixture']);
    assert.match(f.content.querySelector('.vm-list-group-label').textContent, /Proxmox pve-fixture/);
    assert.deepEqual(listIds(f), ['vm-pick-pve-101', 'vm-pick-pve-170']);
    assert.equal(f.content.querySelector('.vm-list-caption>span').textContent, '2 / 3대');
  };
  assertFilters();
  f.pickVm('pve-170'); await f.settle(); assertFilters();
  assert.equal(f.crumb(), 'VM 170 · 업무 B'); assert.deepEqual([...f.content.querySelectorAll('.vm-list-item[aria-current=true]')].map(node => node.getAttribute('data-focus')), ['vm-pick-pve-170']);
  assert.equal(f.content.querySelector('.dt-head h2').textContent, '업무 B');
  assert.ok(f.requests.includes('/api/v1/admin/desktops/pve-170/diagnostics'));
  f.pickVm('pve-101'); await f.settle(); assertFilters();
  assert.equal(f.crumb(), 'VM 101 · 업무 A');
  f.navigate('desktops'); await f.settle(); assertFilters();
  assert.equal(win(f), null, 'the list alone again');
  assert.equal(syncs(), 1, 'list/detail/VM switches reuse the current inventory instead of syncing Proxmox again');
  assert.equal(f.writes.some(w => /\/power$|\/assignments/.test(w.path)), false);
  assert.deepEqual(ownWrites(f), []);
});

test('desktop details are tabs of its window and refresh reads diagnostics only for the open desktop', async t => {
  const { user, inventory } = workspaceFixture();
  const receivedAt = iso(0), reads = new Map(); let count = 1;
  const f = await loadAdmin(t, 'workspace-tabs-refresh', user, inventory, { hash: '#/desktops/pve-170', respond: url => {
    const match = /\/desktops\/(pve-\d+)\/diagnostics$/.exec(url); if (!match) return null;
    reads.set(match[1], (reads.get(match[1]) ?? 0) + 1);
    return { ok: true, status: 200, json: async () => ({ supported: true, connected: true, receivedAt, stale: false,
      report: { version: '0.2.86', display: { observed: true, monitorCount: count, installed: true, deviceReady: true, owned: true, activeDisplay: true, reason: 'ready' }, native: { available: false }, events: [] } }) };
  } });
  // The window has four tabs: the record first, then the people, the records and the Host's own report under 자세히.
  assert.deepEqual(tabNames(f), ['기본 정보', '사용자 0', '최근 기록', '자세히']); assert.equal(openTab(f), '기본 정보');
  assert.deepEqual([...f.content.querySelectorAll('#detail [role=tabpanel]')].map(node => [node.getAttribute('data-tab'), node.hasAttribute('hidden')]), [['info', false], ['users', true], ['records', true], ['more', true]]);
  assert.match(f.content.querySelector('#desktop-people h2').textContent, /^쓰는 사람/);
  const more = () => f.content.querySelector('#detail-panel-more'), moreTab = () => f.content.querySelector('#detail-tab-more');
  assert.equal(moreTab().textContent, '자세히'); assert.equal(moreTab().getAttribute('aria-controls'), 'detail-panel-more'); assert.equal(more().getAttribute('aria-labelledby'), 'detail-tab-more');
  assert.equal(moreTab().localName, 'button', 'a button: the keyboard opens it');
  assert.equal(more().hasAttribute('hidden'), true, 'not shown until its tab is pressed');
  assert.match(more().querySelector('.display-panel').textContent, /1개/);
  assert.ok(more().querySelector('.device-panel')); assert.match(f.content.querySelector('#detail-panel-records').textContent, /전체 기록/);
  press(f, moreTab()); assert.equal(more().hasAttribute('hidden'), false); assert.equal(openTab(f), '자세히');
  assert.ok(f.focused() === moreTab(), 'the focus stays on the tab that was pressed');
  assert.equal(reads.get('pve-170'), 1, 'the tabs use the report already read: no duplicate reads');
  f.pickVm('pve-101'); await f.settle();
  assert.equal(openTab(f), '기본 정보', 'each desktop opens on its first tab');
  press(f, moreTab());
  const otherReads = reads.get('pve-170'), currentReads = reads.get('pve-101'); count = 2;
  document.querySelector('#refresh').dispatchEvent(new f.window.Event('click')); await f.settle();
  assert.equal(reads.get('pve-101'), currentReads + 1); assert.equal(reads.get('pve-170'), otherReads, 'refresh does not query the previously selected VM');
  assert.equal(f.content.querySelector('.dt-head h2').textContent, '업무 A');
  assert.equal(openTab(f), '자세히', 'a refresh keeps the tab the operator opened');
  assert.match(more().querySelector('.display-panel').textContent, /2개/);
  f.pickVm('pve-170'); await f.settle();
  assert.equal(openTab(f), '기본 정보', 'and another desktop opens on its first tab');
});

// The roving-tabindex tabs of the old VM detail live on 기록 now.
test('기록 tabs support keyboard navigation and keep one selected tab bound to the panel', async t => {
  const { user, inventory } = workspaceFixture();
  const f = await loadAdmin(t, 'log-tabs-keyboard', user, inventory, { hash: '#/log' });
  const key = value => { const tab = f.content.querySelector('[role=tab][aria-selected=true]'), event = new f.window.Event('keydown', { cancelable: true }); event.key = value; tab.dispatchEvent(event); return event; };
  const active = (name, id) => {
    const selected = f.content.querySelectorAll('[role=tab][aria-selected=true]'); assert.equal(selected.length, 1);
    assert.equal(selected[0].textContent, name); assert.equal(selected[0].id, id); assert.equal(selected[0].getAttribute('tabindex'), '0');
    assert.equal(f.content.querySelector('[role=tabpanel]').getAttribute('aria-labelledby'), selected[0].id);
    assert.equal(selected[0].getAttribute('aria-controls'), f.content.querySelector('[role=tabpanel]').id);
    assert.ok([...f.content.querySelectorAll('[role=tab][aria-selected=false]')].every(node => node.getAttribute('tabindex') === '-1'));
    assert.ok(f.focused() === selected[0], name + ' has the focus');
    assert.equal(f.crumb(), '기록');
  };
  assert.deepEqual([...f.content.querySelectorAll('.log-tabs [role=tab]')].map(node => node.id), ['log-tab-problems', 'log-tab-quality', 'log-tab-admin']);
  assert.equal(f.content.querySelector('[role=tab][aria-selected=true]').textContent, '문제 찾기');
  assert.equal(key('ArrowRight').defaultPrevented, true); active('화질·속도', 'log-tab-quality');
  key('End'); active('관리 작업', 'log-tab-admin'); key('ArrowRight'); active('문제 찾기', 'log-tab-problems');
  key('ArrowLeft'); active('관리 작업', 'log-tab-admin'); key('Home'); active('문제 찾기', 'log-tab-problems');
  assert.equal(key('a').defaultPrevented, false, 'other keys are left alone'); active('문제 찾기', 'log-tab-problems');
  assert.deepEqual(f.writes, []);
});

test('홈 shows only reported counts, names the causes that need attention and opens their pages', async t => {
  const user = { id: 'fixture-owner', username: 'owner', role: 'admin', mustChangeCredentials: false };
  const inventory = { users: [{ ...user, disabled: 0, notes: '' }, { id: 'member', username: '홍길동', role: 'user', disabled: 0, notes: '' }], assignments: [{ username: '홍길동', desktop_id: 'pve-101' }],
    audit: [{ id: 2, created_at: Date.now() - 60000, event: 'login_failed', actor: 'mallory', subject: '' }, { id: 1, created_at: Date.now() - 120000, event: 'proxmox_synchronized', actor: user.id, subject: '3' }],
    proxmox: { configured: true, endpoint: 'https://pve.fixture.example:8006', state: 'ok', expiresAt: iso(10 * 86400000 - 60000), expirySource: 'configured' },
    desktops: [
      { id: 'pve-101', pve_vmid: 101, name: 'Connected host', host: { connected: true, version: '0.2.61' }, pve_power_status: 'stopped' },
      { id: 'pve-102', pve_vmid: 102, name: 'Offline host', host: { connected: false }, pve_power_status: 'running' },
      { id: 'pve-103', pve_vmid: 103, name: 'Unregistered host', pve_power_status: 'running' },
    ] };
  const monitor = { sessions: [{ sessionId: 'a7b3a1f4-5f5a-4bb1-8f0a-2e0f1b7c9d10', desktopId: 'pve-101', account: '홍길동', mode: 'stream', attached: true, ready: true }],
    hosts: [{ desktopId: 'pve-101', hostname: 'PC-101', version: '0.2.61', connected: true, enrolledAt: iso(-86400000), lastSeenAt: iso(0), diagnostics: null }, { desktopId: 'pve-102', hostname: 'PC-102', version: '0.2.60', connected: false, enrolledAt: iso(-86400000), lastSeenAt: iso(-3600000), diagnostics: null }],
    requests: [{ requestId: 'fixture-request', hostname: 'NEW-PC', version: '0.2.61', machineUuid: 'fixture-machine', status: 'pending', expiresAt: '2099-01-01T00:00:00Z', candidates: [] }] };
  const f = await loadAdmin(t, 'dashboard', user, inventory, { monitor });
  const { content } = f;
  const tiles = () => [...content.querySelectorAll('nav.stat-tiles a.stat-tile')], values = () => tiles().map(node => node.querySelector('strong').textContent);
  const again = async () => { document.querySelector('#refresh').dispatchEvent(new f.window.Event('click')); await f.settle(); };
  assert.equal(f.crumb(), '홈'); assert.equal(content.querySelector('h1').textContent, '홈');
  assert.equal(content.querySelector('.page-description'), null, 'no subtitle that counts what the list below shows');
  assert.deepEqual(tiles().map(node => node.querySelector('span').textContent), ['접속할 수 있는 데스크톱', '지금 접속 중', '접속 불가', '사용자']);
  assert.deepEqual(values(), ['1 / 2', '1', '1', '2'], 'reachable/registered desktops, connections, unreachable desktops and accounts as reported; the unregistered VM is not counted');
  // 지금 할 일: grouped, each row with its own action; what can wait is folded below.
  const todo = [...content.querySelectorAll('.todo-panel .todo-title')].map(node => node.textContent);
  assert.deepEqual(todo, ['접속할 수 없는 데스크톱 1대', '새 데스크톱 1대가 승인을 기다려요', '최근 24시간 실패 1건', 'Proxmox 토큰 만료 10일 전']);
  assert.equal(content.querySelector('.todo-later .todo-title').textContent, 'Proxmox 토큰 만료 10일 전', 'ten days left can wait');
  const unreachable = content.querySelector('[data-todo=unreachable] .todo-row');
  assert.match(unreachable.textContent, /Offline host/); assert.match(unreachable.textContent, /응답 없음/);
  assert.equal(content.querySelector('[data-todo=unreachable] summary small'), null, 'the group needs no line saying users cannot connect');
  assert.ok(f.find(unreachable, 'button', '다시 시작')); assert.match(f.find(unreachable, 'a', '데스크톱 열기').getAttribute('href'), /desktops\/pve-102$/);
  const waiting = content.querySelector('[data-todo=requests] .todo-row');
  assert.match(waiting.textContent, /NEW-PC/); assert.equal(f.find(waiting, 'a', '요청 확인').getAttribute('href'), '#/desktops', 'a request without one matching VM is approved in 승인 대기 of 데스크톱');
  assert.ok(f.find(waiting, 'button', '거절')); assert.equal(content.querySelector('.attention-item'), null);
  assert.match(content.querySelector('.health-line').textContent, /서버 응답 정상 · 0\.8\.1/); assert.match(content.querySelector('.health-line').textContent, /Proxmox 연결됨/);
  assert.equal(f.writes.length, 0, 'showing the list writes nothing');
  assert.equal(content.querySelector('.first-steps'), null, 'every first-run step is done');
  // The boards that repeated the inventory (node board, 전원·Host 분포, 최근 실패, 시스템) are gone.
  for (const gone of ['.op-board', '.op-vm-card', '.op-summary', '.legend', '.checklist']) assert.equal(content.querySelector(gone), null, gone);
  assert.match(content.querySelector('.live-panel').textContent, /홍길동/); assert.match(content.querySelector('.live-panel').textContent, /Connected host/);
  assert.equal(content.querySelector('.live-panel h2 .count').textContent, '1');
  // Everyone in 지금 접속 중 is connected: no 상태 column, no badge; how long and since when in one cell.
  assert.deepEqual([...content.querySelectorAll('.live-panel th')].map(node => node.textContent), ['사용자', '데스크톱', '연결한 지']);
  assert.equal(content.querySelector('.live-panel .pill,.live-panel .state-dot'), null);
  assert.equal(document.querySelector('[data-nav-badge=hosts]'), null);
  assert.equal(document.querySelector('[data-nav-badge=home]').textContent, '3', 'the 홈 entry counts what has to be done now; what can wait is not counted');
  // The failure count opens 기록 on the last day's problems: the failed login, not the routine synchronisation.
  f.click(content.querySelector('.todo-panel'), '기록 보기'); await f.settle();
  assert.equal(f.crumb(), '기록'); assert.equal(content.querySelector('[role=tab][aria-selected=true]').id, 'log-tab-problems');
  assert.equal(content.querySelector('[data-focus=activity-days] option[selected]').getAttribute('value'), '1');
  assert.equal(content.querySelector('button[data-segment=problems]').getAttribute('aria-pressed'), 'true');
  assert.match(content.querySelector('#log-panel').textContent, /로그인 실패/); assert.equal(content.querySelector('#log-panel').textContent.includes('Proxmox VM 동기화'), false, 'routine synchronisation records are not failures');
  assert.equal(f.writes.length, 0);
  // A tile opens its page; opening the desktop list synchronises the inventory.
  f.navigate('overview'); await f.settle();
  press(f, tiles()[0]); await f.settle();
  assert.equal(f.crumb(), '데스크톱');
  assert.deepEqual(listIds(f), ['vm-pick-pve-102', 'vm-pick-pve-101'], 'registered scope retains the offline Host (first, as the problem), not the unregistered VM');
  assert.match(content.querySelector('section.requests-panel').textContent, /NEW-PC/);
  assert.ok(f.requests.some(path => path.endsWith('/proxmox/sync')));
  f.navigate('overview'); await f.settle();
  press(f, tiles()[2]); await f.settle();
  assert.equal(f.crumb(), '데스크톱'); assert.deepEqual(shownState(f), ['bad']); assert.deepEqual(listIds(f), ['vm-pick-pve-102'], 'the 접속 불가 tile opens exactly the desktops it counted');
  f.navigate('overview'); await f.settle();
  inventory.desktops[0].host.connected = false; monitor.hosts[0].connected = false; monitor.sessions = [];
  await again();
  assert.deepEqual(values(), ['0 / 2', '0', '1', '2'], 'a stopped VM whose Host is silent is off, not a problem');
  assert.match(content.querySelector('.live-panel').textContent, /지금 접속 중인 사람이 없어요/);
  // A missing assignment for an ordinary account keeps the first-run steps visible.
  inventory.assignments = [];
  await again();
  assert.equal(content.querySelector('h1').textContent, '홈', 'desktops exist: the steps are a strip, not the whole page');
  const steps = [...content.querySelectorAll('ol.first-steps li')];
  assert.deepEqual(steps.map(node => node.querySelector('span').textContent), ['서버 접속 설정', 'Proxmox 연결', 'Windows에 Host 설치', '데스크톱 승인', '사용자 만들기', '데스크톱 배정']);
  assert.deepEqual(steps.map(node => node.getAttribute('class')), ['is-done', 'is-done', 'is-done', 'is-done', 'is-done', 'is-now is-shown'], 'the current step is the one shown');
  // Each step opens its own explanation; a later one says what it waits for.
  assert.deepEqual(steps.map(node => node.getAttribute('role')), Array(6).fill('button'));
  assert.equal(steps[5].getAttribute('aria-pressed'), 'true');
  assert.equal(steps[5].getAttribute('aria-current'), 'step');
  assert.match(content.textContent, /6단계 · 데스크톱을 배정하세요/); assert.equal(f.find(content, 'a', '사용자 열기').getAttribute('href'), '#/users');
  assert.ok(content.querySelector('.todo-panel'), 'what has to be done stays below the steps');
  // A missing console permission is named and opens the page with the fix; the sound card is no longer Multitap's.
  inventory.proxmox.soundCardPrivilege = 'missing'; inventory.proxmox.consolePrivilege = 'missing';
  await again();
  const sound = [...content.querySelectorAll('.todo-later .todo-item')].find(node => node.querySelector('.todo-title').textContent === '긴급 콘솔 권한 없음');
  assert.ok(sound); assert.match(sound.querySelector('a').getAttribute('href'), /proxmox$/); assert.equal(sound.querySelector('small').textContent, '사용자가 Console을 열지 못해요.');
  assert.doesNotMatch(content.textContent, /사운드카드/);
});

const unknownHome = async (t, suffix) => {
  const user = { id: 'owner', username: 'owner', role: 'admin', mustChangeCredentials: false };
  const inventory = { users: [user], assignments: [], audit: [], proxmox: { configured: true, state: 'ok' }, desktops: [] };
  return loadAdmin(t, suffix, user, inventory, { respond: url => /\/admin\/(sessions|hosts|client-diagnostics)$/.test(url) ? { ok: false, status: 503, json: async () => ({ error: { code: 'admin_unavailable' } }) } : null });
};
test('홈 values that the server did not report stay unknown instead of estimated', async t => {
  const f = await unknownHome(t, 'dashboard-unknown');
  const tiles = [...f.content.querySelectorAll('nav.stat-tiles a.stat-tile')];
  assert.deepEqual(tiles.map(node => node.querySelector('span').textContent), ['접속할 수 있는 데스크톱', '지금 접속 중', '접속 불가', '사용자']);
  assert.deepEqual(tiles.map(node => node.querySelector('strong').textContent), ['—', '—', '—', '1'], 'no 0 and no 0 / 0 for what could not be read');
  assert.match(f.content.querySelector('.live-panel').textContent, /연결 정보를 확인하지 못했어요/);
  assert.equal(f.content.querySelector('.live-panel h2 .count'), null);
  // An unreadable Host list is not an empty installation.
  assert.equal(f.content.querySelector('h1').textContent, '홈'); assert.equal(f.content.querySelector('.first-steps'), null);
  assert.doesNotMatch(f.content.textContent, /시작하기|등록된 데스크톱이 없어요|전체 VM|QEMU|자동으로 불러/);
  // The failed read is the one thing to do, with its retry; nothing is invented beside it.
  assert.equal(f.content.querySelector('.todo-group'), null); assert.deepEqual([...f.content.querySelectorAll('.todo-panel .todo-title')].map(node => node.textContent), ['데스크톱 상태를 확인하지 못했어요']);
  assert.ok(f.find(f.content.querySelector('.todo-panel'), 'button', '다시 시도')); assert.equal(document.querySelector('[data-nav-badge=home]').textContent, '1');
  assert.equal(f.content.querySelector('.page-description'), null);
  assert.equal(f.content.querySelector('a[href="#/hosts"]'), null); assert.ok(f.content.querySelector('a.stat-tile[href="#/desktops"]'));
  assert.deepEqual(f.writes, []);
});

// With the Host list and the connections unreadable the page states no health it did not
// observe: the failed read is named in 지금 할 일.
test('홈 does not say everything is fine while the Host list and the connections could not be read', async t => {
  const f = await unknownHome(t, 'dashboard-unknown-claim');
  assert.equal(f.content.querySelector('nav.stat-tiles a.stat-tile strong').textContent, '—');
  assert.doesNotMatch(f.content.querySelector('.page-description')?.textContent ?? '', /모두 정상이에요/, 'an unread Host list is not a healthy one');
});

test('Compact Proxmox setup uses the current broker origin and password confirmation blocks mismatched submissions', async t => {
  const user = { id: 'fixture-admin', username: 'owner', role: 'admin', isPrimaryAdmin: true, mustChangeCredentials: false };
  const inventory = { users: [{ id: user.id, username: user.username, notes: '', role: 'admin', disabled: 0, is_primary_admin: 1 }], desktops: [], assignments: [], audit: [], proxmox: { configured: false, setupScriptSha256: 'ab'.repeat(32) } };
  const { writes, dialog, click, follow, navigate, settle, content } = await loadAdmin(t, 'dom-flow', user, inventory);
  navigate('desktops');
  assert.match(content.textContent, /데스크톱/);
  assert.equal(content.textContent.includes('Windows PC'), false);
  assert.equal(content.textContent.includes('데스크톱 등록'), false);
  navigate('proxmox');
  // The command checks the download against the broker's SHA-256 before running it.
  assert.ok(document.querySelector('.shell-setup').textContent.includes('curl -q -fsS "$1" -o "$f" && echo "$2  $f" | sha256sum -c --quiet - && shift 2 && python3 "$f" "$@"'));
  assert.ok(document.querySelector('.shell-command code').textContent.endsWith(' ' + 'ab'.repeat(32) + ' --lang ko'));
  assert.match(document.querySelector('.shell-command code').textContent, /https:\/\/cloud\.fixture\.example\/setup\/proxmox\.py/);
  assert.equal(document.querySelector('#server-origin').textContent, 'cloud.fixture.example');
  assert.equal(document.querySelector('.guide-panel'), null);
  assert.equal(document.querySelector('#content img'), null);
  assert.equal(document.querySelector('.pve-disclosure').hasAttribute('open'), false);
  assert.match(document.querySelector('.shell-setup').textContent, /데스크톱 목록에는 Host를 설치하고 승인한 것만 나와요\. Proxmox 연결은 그 데스크톱의 전원 켜기·끄기와 Console에 써요\./);
  // The fold says, in full, what the command makes and allows.
  const scope = document.querySelector('details.pve-scope');
  assert.ok(scope, 'the Proxmox step explains its account and permissions'); assert.equal(scope.hasAttribute('open'), false, 'folded until asked');
  assert.deepEqual([...scope.querySelectorAll('tbody td:first-child code')].map(node => node.textContent), ['VM.Audit', 'VM.PowerMgmt', 'VM.Console', 'VM.GuestAgent.Audit', 'VM.Monitor']);
  assert.match(scope.textContent, /데스크톱 목록에는 Host를 설치하고 승인한 VM만 나와요\./); assert.match(scope.textContent, /주지 않는 권한: VM 만들기·지우기/);
  assert.equal([...document.querySelectorAll('.shell-setup .setup-step strong')].length, 2, 'two numbered steps: run the command, paste its output');
  assert.equal(document.querySelector('.shell-setup .shell-command button').textContent, '명령 복사');
  assert.equal(document.querySelector('.shell-setup').textContent.includes('VM 번호를 입력'), false);
  const connection = { endpoint: 'https://pve.example.com:8006', tokenId: 'multitap-fixture@pve!broker', secret: 'fixture-secret-only', ca: '' };
  document.querySelector('.connection-import').value = 'MULTITAP-CONNECTION-V1:\n' + Buffer.from(JSON.stringify(connection)).toString('base64') + '\nEND-MULTITAP-CONNECTION';
  click(document.querySelector('.shell-setup'), '연결하기');
  await settle();
  assert.deepEqual(writes[0], { path: '/api/v1/admin/proxmox', data: connection });
  assert.equal(writes[1].path, '/api/v1/admin/proxmox/sync');
  assert.match(document.querySelector('.pve-connection-card').textContent, /연결됨/);
  assert.match(document.querySelector('.pve-connection-card').textContent, /만료일 정보 없음/);
  assert.equal(content.textContent.includes(connection.secret), false);
  assert.equal(document.querySelector('.connection-import').value, '');
  assert.equal(document.querySelector('.pve-change').hasAttribute('open'), false);
  writes.length = 0;
  navigate('users'); follow(content, 'owner'); await settle();
  click(content, '비밀번호 재설정');
  assert.equal(dialog.querySelectorAll('input[type=password]').length, 2);
  const form = document.querySelector('#modal-form');
  form.elements.password.value = 'Password-confirmation-test-71!'; form.elements.passwordConfirmation.value = 'mismatch';
  const accountWrites = () => writes.filter(write => write.path.includes('/users/'));
  await form.onsubmit({ preventDefault() {} });
  assert.match(document.querySelector('#modal-error').textContent, /일치하지 않아요/);
  assert.equal(accountWrites().length, 0); assert.equal(dialog.open, true);
  form.elements.passwordConfirmation.value = form.elements.password.value;
  await form.onsubmit({ preventDefault() {} });
  assert.equal(accountWrites().length, 1); assert.equal(accountWrites()[0].path, '/api/v1/admin/users/owner/password'); assert.equal(accountWrites()[0].data.password, accountWrites()[0].data.passwordConfirmation); assert.equal(dialog.open, false);
});

test('Account pages use Korean IDs and notes, protect the persistent main administrator and open a new account for assignment', async t => {
  const user = { id: 'fixture-primary', username: '메인관리자', displayName: 'Obsolete display name', role: 'admin', isPrimaryAdmin: true, mustChangeCredentials: false };
  const member = { id: 'fixture-member', username: '홍길동', display_name: 'Obsolete member name', notes: '디자인팀\n<img src=x onerror=alert(1)>', role: 'user', disabled: 0, is_primary_admin: 0 };
  const inventory = {
    users: [{ id: user.id, username: user.username, display_name: user.displayName, notes: '총괄 담당', role: 'admin', disabled: 0, is_primary_admin: 1 }, member],
    desktops: [{ id: 'fixture-vm', pve_vmid: 101, name: '업무 VM', enabled: 1 }],
    assignments: [{ username: member.username, desktop_id: 'fixture-vm' }], audit: [], proxmox: { configured: false },
  };
  registerFixtureDesktops(inventory);
  const f = await loadAdmin(t, 'account-identity-flow', user, inventory, { respond: (url, options) => {
    if (url.endsWith('/admin/users') && options.method === 'POST') { const input = JSON.parse(options.body); inventory.users.push({ id: 'created', username: input.username, notes: input.notes, role: input.role, disabled: 0, is_primary_admin: 0 }); }
    return null;
  } });
  const { content, writes, dialog, click, follow, navigate, settle } = f;
  assert.equal(document.querySelector('#current-user').textContent, user.username);
  navigate('users');
  assert.equal(content.textContent.includes('Obsolete'), false);
  assert.ok(content.querySelector('.account-list .account-table'));
  let rows = [...content.querySelectorAll('.account-choice')];
  // A row shows the first line of the memo and the number of desktops; the role is a badge beside the name.
  assert.equal(rows[0].querySelector('.account-choice-text>small').textContent, '총괄 담당 · 데스크톱 0대');
  assert.match(rows[0].getAttribute('aria-description'), /^메인 관리자 · 데스크톱 0대 · 사용 중/);
  assert.equal(rows[1].querySelector('img'), null);
  assert.equal(rows[1].querySelector('.account-choice-text>small').textContent, '디자인팀 · 데스크톱 1대');
  assert.deepEqual([...content.querySelectorAll('.account-table td.is-state')].map(node => node.textContent), ['사용 중', '사용 중'], 'nobody is connected or stopped, and there is no draft mark');
  const search = content.querySelector('[data-focus=users-search]');
  search.value = '디자인팀'; search.dispatchEvent(new f.window.Event('input')); click(content.querySelector('.query-box'), '조회');
  assert.equal(content.querySelectorAll('.account-choice').length, 1);
  assert.match(content.querySelector('.account-table tbody').textContent, /홍길동/);
  click(content.querySelector('.query-box'), '초기화'); assert.equal(content.querySelector('[data-focus=users-search]').value, ''); assert.equal(content.querySelectorAll('.account-choice').length, 2);

  // The main administrator: notes and password only.
  navigate('users'); follow(content, user.username); await settle();
  assert.equal(f.crumb(), user.username);
  // The account's actions are the buttons on the left of the window's bar; 닫기 stands alone on the right.
  const headButtons = () => [...content.querySelectorAll('.user-detail .win-actions button')].map(node => node.textContent);
  assert.equal(content.querySelector('.account-head h2').firstChild.textContent, user.username);
  assert.deepEqual(headButtons(), ['비밀번호 재설정', '정보 수정', '공지 보내기'], 'no 계정 중지 or 삭제 for the main administrator');
  assert.equal(content.querySelector('details.more-menu'), null, 'no action hides in a menu');
  assert.deepEqual(tabNames(f), ['기본 정보', '데스크톱 0', '최근 기록']); assert.equal(content.querySelector('.user-detail .win-done').textContent, '닫기');
  // Why it cannot be stopped or deleted is the title of its 권한, not a sentence in the cell.
  const role = [...content.querySelectorAll('.user-detail .facts dt')].find(node => node.textContent === '권한').nextSibling;
  assert.equal(role.textContent, '메인 관리자'); assert.match(role.firstChild.title, /권한 변경, 중지와 삭제를 할 수 없어요/);
  assert.equal(content.querySelector('.account-head small').textContent, '메인 관리자 · 현재 로그인한 계정 · 총괄 담당', 'what the account is stands after its name as plain text');
  assert.equal(content.querySelector('.user-detail .account-head .pill'), null);
  click(content, '정보 수정');
  const form = document.querySelector('#modal-form');
  assert.equal(form.querySelector('[name=role]'), null);
  assert.equal(form.querySelector('[name=displayName]'), null);
  assert.match(form.textContent, /메인 관리자 권한은 변경할 수 없어요/);
  form.elements.notes.value = '주 관리자 메모';
  await form.onsubmit({ preventDefault() {} }); await settle();
  assert.deepEqual(writes.pop(), { path: '/api/v1/admin/users/' + encodeURIComponent(user.username), data: { notes: '주 관리자 메모', expectedAccountId: user.id } });

  // An ordinary account can change role, be stopped or deleted.
  navigate('users'); follow(content, member.username); await settle();
  // Plain buttons, 삭제 first (red text), none with an icon.
  assert.deepEqual(headButtons(), ['삭제', '비밀번호 재설정', '정보 수정', '공지 보내기', '계정 중지']);
  assert.equal(content.querySelector('.user-detail .win-actions button').className, 'danger dt-delete');
  assert.equal(content.querySelector('.user-detail .win-foot svg'), null);
  // The memo is text everywhere it is shown: the whole memo in 계정, its first line in the head.
  assert.equal(content.querySelector('img'), null);
  assert.equal(content.querySelector('.account-notes').textContent, member.notes);
  assert.equal(content.querySelector('.account-head small').textContent, '디자인팀');
  // The state: one dot and its word, in the title bar and in the form table.
  assert.equal(content.querySelector('.account-head .state-dot').textContent, '사용 중'); assert.equal(content.querySelector('.account-head .state-dot').className, 'state-dot is-blue');
  assert.equal([...content.querySelectorAll('.user-detail .facts dt')].find(node => node.textContent === '상태').nextSibling.querySelector('.state-dot').textContent, '사용 중');
  assert.equal(content.querySelectorAll('#assignments li.assigned-row').length, 1);
  click(content, '정보 수정');
  assert.ok(form.querySelector('[name=role]'));
  assert.equal(form.elements.notes.value, member.notes);
  form.elements.notes.value = '개발팀';
  await form.onsubmit({ preventDefault() {} }); await settle();
  assert.deepEqual(writes.pop(), { path: '/api/v1/admin/users/' + encodeURIComponent(member.username), data: { role: 'user', notes: '개발팀', expectedAccountId: member.id } });
  click(content, '계정 중지');
  assert.match(dialog.textContent, /배정은 유지돼요/);
  await form.onsubmit({ preventDefault() {} }); await settle();
  assert.deepEqual(writes.pop(), { path: '/api/v1/admin/users/' + encodeURIComponent(member.username) + '/enabled', data: { enabled: false, expectedAccountId: member.id } });
  click(content, '삭제');
  assert.match(dialog.textContent, /되돌릴 수 없어요/); assert.equal(document.querySelector('#modal-save').className, 'danger');
  assert.match(dialog.textContent, /배정된 데스크톱 1대와 공유 설정도 함께 지워져요/, 'the confirmation says what goes with the account');
  await form.onsubmit({ preventDefault() {} }); await settle();
  assert.deepEqual(writes.pop(), { path: '/api/v1/admin/users/' + encodeURIComponent(member.username), data: { expectedAccountId: member.id } }, 'one DELETE of the account');
  assert.equal(f.crumb(), '사용자', 'a deleted account returns to the account list');
  assert.match(document.querySelector('#notice').textContent, /삭제했어요/);

  // Creating an account opens it, ready for desktop assignment.
  navigate('users');
  click(content, '사용자 만들기');
  assert.equal(form.textContent.includes('초기 비밀번호'), false);
  assert.equal(form.elements.username.getAttribute('minlength'), '2');
  assert.match(form.textContent, /한글/);
  assert.equal(form.querySelector('[name=displayName]'), null);
  assert.equal(form.elements.notes.hasAttribute('required'), false);
  form.elements.username.value = '김철수'; form.elements.notes.value = '';
  form.elements.password.value = form.elements.passwordConfirmation.value = 'Fixture-create-password-10!';
  await form.onsubmit({ preventDefault() {} }); await settle();
  const created = writes.find(write => write.path === '/api/v1/admin/users');
  assert.equal(created.data.username, '김철수'); assert.equal(created.data.notes, ''); assert.equal('displayName' in created.data, false);
  assert.equal(dialog.open, false);
  assert.equal(f.crumb(), '김철수');
  assert.match(document.querySelector('#notice').textContent, /김철수 계정을 만들었어요/);
  // The new account opens with 쓸 수 있는 데스크톱 empty and 데스크톱 배정 one press away.
  const assignments = () => content.querySelector('#assignments');
  assert.match(assignments().querySelector('h2').textContent, /^쓸 수 있는 데스크톱/); assert.match(assignments().textContent, /아직 배정된 데스크톱이 없어요/);
  writes.length = 0;
  click(assignments(), '데스크톱 배정');
  assert.equal(document.querySelector('#modal-title').textContent, '데스크톱 배정');
  const box = dialog.querySelector('input[data-vm-id="fixture-vm"]');
  assert.ok(box, 'the registered desktop is offered'); assert.equal(dialog.querySelectorAll('input[data-vm-id]').length, 1);
  assert.equal(writes.length, 0, 'opening the picker never assigns');
  await form.onsubmit({ preventDefault() {} });
  assert.match(document.querySelector('#modal-error').textContent, /배정할 데스크톱을 고르세요/); assert.equal(writes.length, 0, 'nothing chosen, nothing sent');
  box.checked = true;
  await form.onsubmit({ preventDefault() {} }); await settle();
  assert.deepEqual(writes.filter(write => write.path.endsWith('/assignments/batch')), [{ path: '/api/v1/admin/assignments/batch', data: { username: '김철수', expectedAccountId: 'created', desktopIds: ['fixture-vm'] } }]);
  assert.equal(dialog.open, false);
  assert.ok(assignments().querySelector('li.assigned-row[data-desktop="fixture-vm"]'));
  assert.equal(assignments().querySelector('button[data-preset=default][data-vm="fixture-vm"]').getAttribute('aria-pressed'), 'true', 'a new assignment starts with nothing shared');
  assert.equal(content.textContent.includes('Obsolete'), false);
});

test('another administrator sees the primary administrator\'s password reset turned off, with the reason', async t => {
  const deputy = { id: 'deputy', username: 'deputy', role: 'admin', mustChangeCredentials: false };
  const inventory = { users: [{ id: 'primary', username: 'owner', notes: '', role: 'admin', disabled: 0, is_primary_admin: 1 }, { id: deputy.id, username: deputy.username, notes: '', role: 'admin', disabled: 0, is_primary_admin: 0 }],
    desktops: [], assignments: [], audit: [], proxmox: { configured: true } };
  const f = await loadAdmin(t, 'primary-password', deputy, inventory);
  const { content, follow, navigate, settle, writes } = f;
  navigate('users'); follow(content, 'owner'); await settle();
  const reset = [...content.querySelectorAll('.user-detail .win-actions button')].find(node => node.textContent === '비밀번호 재설정');
  assert.equal(reset.disabled, true); assert.equal(reset.title, '대표 관리자의 비밀번호는 대표 관리자 본인만 바꿀 수 있어요.');
  reset.click(); assert.ok(!document.querySelector('#modal').open, 'no dialog'); assert.equal(writes.some(write => write.path.endsWith('/password')), false);
  // Its own account and an ordinary one keep the action.
  navigate('users'); follow(content, 'deputy'); await settle();
  assert.equal([...content.querySelectorAll('.user-detail .win-actions button')].find(node => node.textContent === '비밀번호 재설정').disabled, false);
});
