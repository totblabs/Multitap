import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { parseHTML } from 'linkedom';
import { memoryStorage, loadAdmin, iso, workspaceFixture, registerFixtureDesktops, registeredWorkspaceFixture, clipDenyFixture, deviceKeysFixture, deviceDenyFixture, permissionFixture, ownerFixture, recheck, versionServer } from './admin-ui-harness.mjs';

test('First setup requires Proxmox completion before navigation, and a later outage does not repeat onboarding', async t => {
  const user = { id: 'fixture-owner', username: '관리자', role: 'admin', mustChangeCredentials: false };
  const inventory = { users: [{ ...user, notes: '', disabled: 0 }], desktops: [], assignments: [], audit: [], proxmox: { configured: false }, onboarding: { stage: 'proxmox', credentialsRequired: false, proxmoxRequired: true, complete: false } };
  let failSync = false;
  const { writes, click, navigate, settle, content, crumb } = await loadAdmin(t, 'onboarding-flow', user, inventory, { hash: '#/users', respond: async url => failSync && url.endsWith('/proxmox/sync') ? { ok: false, status: 502, json: async () => ({ error: { code: 'proxmox_unreachable' } }) } : undefined });
  assert.equal(crumb(), 'Proxmox 연결', 'a bookmarked page waits for commissioning');
  assert.equal(content.hidden, false, 'workspace page metadata must never be treated as a hidden sidebar entry');
  assert.deepEqual([...document.querySelectorAll('#sidebar [data-page]')].map(node => node.dataset.page), ['overview', 'desktops', 'users', 'log', 'monitoring', 'notices', 'updates', 'downloads', 'server-access', 'proxmox', 'backup', 'server-info', 'licenses']);
  for (const page of ['overview', 'desktops', 'users', 'log', 'monitoring', 'notices', 'updates', 'downloads', 'server-info', 'licenses']) assert.equal(document.querySelector('#sidebar [data-page=' + page + ']').hidden, true, page);
  // 백업·복원 is offered too: a new server may start from a backup instead of a new Proxmox connection.
  for (const page of ['proxmox', 'server-access', 'backup']) assert.equal(document.querySelector('#sidebar [data-page=' + page + ']').hidden, false, page);
  assert.equal(document.querySelector('#nav-account-security').hidden, true, 'account security waits for the setup too');
  assert.deepEqual([...document.querySelectorAll('#sidebar .nav-group')].map(group => group.hidden), [true, true, false], 'a group with nothing to offer shows no heading or divider');
  assert.equal(document.querySelector('.global-search').hidden, true, 'nothing to search before Proxmox is connected');
  assert.equal(document.querySelector('.section-navigation a[href="#/proxmox"]').getAttribute('aria-current'), 'page');
  assert.deepEqual([...document.querySelectorAll('.section-navigation a')].map(node => node.getAttribute('href')), ['#/server-access', '#/proxmox', '#/backup'], 'commissioning exposes only Proxmox setup, server connection settings and putting a backup back, in the sidebar order');
  assert.deepEqual([...document.querySelectorAll('.section-navigation a')].map(node => node.getAttribute('href')), [...document.querySelectorAll('#sidebar [data-page]:not([hidden])')].map(node => node.getAttribute('href')), 'the bar and the sidebar list them in one order');
  navigate('users');
  assert.equal(crumb(), 'Proxmox 연결');
  assert.match(content.textContent, /관리자 계정 변경 완료/);
  // The step list names the first steps of the bar and of 홈 시작하기 in their order: the account, the address, Proxmox (this page).
  assert.deepEqual([...content.querySelectorAll('.onboarding-steps li')].map(node => node.textContent), ['1. 관리자 계정 변경 완료', '2. 서버 접속 설정', '3. Proxmox 연결']);
  assert.equal(content.querySelector('.onboarding-steps li[aria-current=step]').textContent, '3. Proxmox 연결');
  assert.equal(content.querySelector('.onboarding-steps li:nth-child(2) a').getAttribute('href'), '#/server-access');
  assert.equal(writes.length, 0);
  const connection = { endpoint: 'https://pve.fixture.example', tokenId: 'multitap-fixture@pve!broker', secret: 'fixture-secret-only', ca: '' };
  document.querySelector('.connection-import').value = 'MULTITAP-CONNECTION-V1:\n' + Buffer.from(JSON.stringify(connection)).toString('base64') + '\nEND-MULTITAP-CONNECTION';
  click(content, '연결하기'); await settle();
  assert.equal(crumb(), '홈');
  assert.equal(document.querySelector('.section-navigation'), null, 'the two-page bar belongs to the first setup only');
  for (const node of document.querySelectorAll('#sidebar [data-page],#sidebar .nav-group,#nav-account-security')) assert.equal(node.hidden, false);
  assert.equal(document.querySelector('.global-search').hidden, false);
  failSync = true;
  navigate('desktops'); await settle();
  assert.match(content.textContent, /VM 목록을 확인하지 못했어요/);
  assert.equal(document.querySelector('[data-page=users]').hidden, false);
  navigate('users'); assert.equal(crumb(), '사용자');
});

test('A first Proxmox connection that did not connect is still the first connection, and Back moves between the setup pages', async t => {
  const user = { id: 'fixture-owner', username: '관리자', role: 'admin', mustChangeCredentials: false };
  const inventory = { users: [{ ...user, notes: '', disabled: 0 }], desktops: [], assignments: [], audit: [], proxmox: { configured: false }, onboarding: { stage: 'proxmox', credentialsRequired: false, proxmoxRequired: true, complete: false } };
  // The address is saved, the check of it fails (as a wrong address or token does): the setup stays required.
  const f = await loadAdmin(t, 'onboarding-failed-first', user, inventory, { respond: async url => url.endsWith('/proxmox/sync') ? { ok: false, status: 502, json: async () => ({ error: { code: 'proxmox_unreachable' } }) }
    // The server already has an address (a Docker install made by the install command): the address step is ticked.
    : url.endsWith('/server-access') ? { ok: true, status: 200, json: async () => ({ protocol: 'server-access-v1', revision: 'a'.repeat(64), pending: null, supported: false, deploymentKind: 'docker', managementOrigin: 'http://192.0.2.10:18480',
      applied: { protocol: 'server-access-v1', primaryOrigin: 'http://192.0.2.10:18480', trustedProxies: [], https: { enabled: false, origin: null, mode: 'proxy', listenHost: '0.0.0.0', listenPort: 18443, certFile: null, keyFile: null }, http: { enabled: true, origin: 'http://192.0.2.10:18480', listenHost: '0.0.0.0', listenPort: 18480 } } }) } : undefined });
  const { content, click, settle, writes, crumb } = f;
  assert.equal(content.querySelector('.onboarding-steps li:nth-child(2)').getAttribute('class'), 'complete');
  const block = secret => 'MULTITAP-CONNECTION-V1:\n' + Buffer.from(JSON.stringify({ endpoint: 'https://pve.fixture.example', tokenId: 'multitap-fixture@pve!broker', secret, ca: '' })).toString('base64') + '\nEND-MULTITAP-CONNECTION';
  document.querySelector('.connection-import').value = block('fixture-secret-wrong');
  click(content, '연결하기'); await settle();
  assert.equal(crumb(), 'Proxmox 연결'); assert.match(content.querySelector('.pve-connection-card').textContent, /연결 오류/);
  // The two guided steps stay open (they were folded into 연결 설정 변경), with no token to renew yet.
  assert.equal(content.querySelector('.pve-change'), null); assert.ok(content.querySelector('#shell-setup .connection-import'));
  assert.deepEqual([...content.querySelectorAll('.page-actions button')].map(node => node.textContent), ['새로고침']);
  // Pasting the right block replaces what never worked without the "Proxmox 연결 바꾸기" confirmation.
  const before = writes.length;
  document.querySelector('.connection-import').value = block('fixture-secret-right');
  click(content, '연결하기'); await settle();
  assert.notEqual(f.dialog.open, true);
  assert.equal(writes.slice(before).filter(write => write.path === '/api/v1/admin/proxmox').length, 1);
  // Back, Forward and a typed address move between the three setup pages; any other address shows Proxmox 연결.
  for (const [hash, title] of [['#/server-access', '서버 접속 설정'], ['#/backup', '백업·복원'], ['#/users', 'Proxmox 연결'], ['', 'Proxmox 연결']]) {
    globalThis.location = new URL('https://cloud.fixture.example/admin/' + hash); f.emitWindow('hashchange'); await settle();
    assert.equal(crumb(), title, hash);
  }
});

test('During first setup, 백업·복원 and 서버 접속 설정 stay open while the page refreshes itself', async t => {
  const user = { id: 'fixture-owner', username: '관리자', role: 'admin', mustChangeCredentials: false };
  const inventory = { users: [{ ...user, notes: '', disabled: 0 }], desktops: [], assignments: [], audit: [], proxmox: { configured: false }, onboarding: { stage: 'proxmox', credentialsRequired: false, proxmoxRequired: true, complete: false } };
  const asked = [];
  t.mock.timers.enable({ apis: ['setTimeout', 'setInterval', 'Date'], now: Date.parse('2026-11-11T07:00:00.000Z') });
  // Every read that needs Proxmox is refused the way the server refuses it before Proxmox is connected.
  const { navigate, settle, crumb } = await loadAdmin(t, 'onboarding-poll', user, inventory, { hash: '#/backup', respond: async url => {
    asked.push(url);
    if (/\/(hosts|host-requests|hostRequests|desktops|sessions)(\?|$)/.test(url)) return { ok: false, status: 409, json: async () => ({ error: { code: 'proxmox_setup_required' } }) };
    return undefined;
  } });
  for (const page of ['backup', 'server-access']) {
    navigate(page); await settle();
    const title = page === 'backup' ? '백업·복원' : '서버 접속 설정';
    assert.equal(crumb(), title);
    for (let n = 0; n < 3; n++) { t.mock.timers.tick(5000); await settle(); }
    assert.equal(crumb(), title, page + ' is not pulled back to the Proxmox step by the refresh');
  }
  assert.equal(asked.some(url => /\/(hosts|host-requests)(\?|$)/.test(url)), false, 'no Proxmox-bound badge read during first setup');
});
test('VM detail shows automatic Host diagnostics, native failure and client records without granting actions to old hosts', async t => {
  const user = { id: 'owner', username: 'owner', role: 'admin', mustChangeCredentials: false };
  const inventory = { users: [{ ...user, disabled: 0, notes: '' }], assignments: [], audit: [], proxmox: { configured: true }, desktops: [{ id: 'pve-149', name: 'TESTER', kind: 'vm', pve_vmid: 149, pve_available: 1, pve_node: 'fixture', pve_power_status: 'running', host: { connected: true }, connectionModes: { stream: false } }] };
  const receivedAt = new Date().toISOString(); let diagnosticReads = 0;
  const clientEvents = [{ event: 'displayConfigure', time: receivedAt, receivedAt, account: 'alice', version: '0.4.38+52', desktopId: 'pve-149', operationId: '614c03e4-5595-4d77-a075-f6a61cfc4e8b', code: 'display_configuration_failed', mode: 'stream', values: { ok: false, elapsedMs: 7354 } }];
  const f = await loadAdmin(t, 'automatic-host-diagnostics', user, inventory, { monitor: { clientEvents, hosts: [{desktopId:'pve-149',connected:true}] }, respond: url => {
    if (!url.endsWith('/desktops/pve-149/diagnostics')) return null;
    diagnosticReads++;
    return { ok: true, status: 200, json: async () => ({ supported: false, connected: true, receivedAt, stale: false, report: { version: '0.2.24', display: { observed: true, observationAgeMs: 4000, monitorCount: 0, installed: true, deviceReady: true, owned: true, activeDisplay: false, rebootRequired: false, recoveryRequired: true, reason: 'native_topology_recovery_required' }, interactiveSession: true, consoleSession: true, native: { available: true, phase: 'recovery', failureReason: 'controller_start_failed', leaseActive: false, activationVerified: false, activation: { reason: 'native_topology_validate_failed', systemError: 1610, attempted: false, windowsVerified: false }, activationCurrent: false }, events: [] } }) };
  } });
  f.navigate('desktops'); await f.settle(); f.pickVm('pve-149'); await f.settle();
  assert.equal(f.crumb(), 'VM 149 · TESTER');
  // The window's tabs: the Host's details are under 자세히, always in the page and shown when that tab is pressed.
  assert.deepEqual([...f.content.querySelectorAll('#detail [role=tab]')].map(node => node.textContent), ['기본 정보', '사용자 0', '최근 기록', '자세히']);
  const more = f.content.querySelector('#detail-panel-more');
  assert.equal(f.content.querySelector('#detail-tab-more').getAttribute('aria-selected'), 'false'); assert.equal(more.hasAttribute('hidden'), true);
  assert.ok(more.querySelector('.display-panel'));
  const display = f.content.querySelector('.display-panel').textContent;
  for (const text of ['가상 모니터 생성 실패', '0개', '(자동)', '화면 복구 필요', '이전 활성화 시도', 'Windows가 요청한 화면 배치를 거절함', 'Windows 오류 1610']) assert.ok(display.includes(text), text);
  // 자세히 is the same form table as 기본 정보: one fact per cell, a label and its value, no slash labels or dot chains.
  const cells = [...f.content.querySelectorAll('.display-panel > .facts.form-table > div')].map(row => [row.querySelector('dt').textContent, row.querySelector('dd').textContent]);
  assert.deepEqual(cells.slice(0, 2).map(([label]) => label), ['마지막 수집', 'Host 연결']);
  for (const [label, value] of [['가상 모니터 드라이버', '설치됨'], ['드라이버 장치', '시작됨'], ['가상 모니터 장치', '있음'], ['가상 모니터 화면', '꺼짐'], ['Windows 다시 시작', '필요 없음'], ['화면 복구', '복구 필요'], ['Windows 로그인', '로그인됨'], ['콘솔 세션', '맞음'], ['Host 버전', '0.2.24'], ['마지막 화면 확인', '4초 전']]) assert.ok(cells.some(([l, v]) => l === label && v === value), label);
  assert.equal(cells.some(([label, value]) => /\s\/\s|·/.test(label + value)), false, 'no slash labels or dot chains');
  assert.equal(f.content.querySelector('.display-panel .diagnostic-grid, .display-panel .panel-status'), null);
  const collect = [...f.content.querySelectorAll('button')].find(node => node.textContent === '지금 상태 수집');
  assert.ok(collect.disabled, 'a Host without the diagnostics protocol cannot be asked'); assert.equal(diagnosticReads, 1);
  assert.equal(f.writes.filter(value => value.path.endsWith('/diagnostics')).length, 0);
  // 기본 정보 is the desktop's record as a form table; 최근 기록 holds the problems and every record.
  assert.match(f.content.querySelector('#detail-panel-info .desktop-host-summary').textContent, /스트리밍불가/);
  // 전체 기록 is a board; a row's 상세 opens its whole record under it (the version, the code, the operation).
  const recordsPanel = () => [...f.content.querySelectorAll('#detail-panel-records .panel')].find(node => node.querySelector('h3')?.textContent === '전체 기록');
  assert.deepEqual([...recordsPanel().querySelectorAll('.log-board thead th')].map(node => node.textContent), ['시간', '사용자', '작업', '결과', '내용', '관리'], 'no 데스크톱 column inside the desktop');
  assert.match(recordsPanel().querySelector('.log-board tbody tr[data-entry]').textContent, /가상 화면 구성 · 스트리밍실패데스크톱이 가상 화면을 준비하지 못했어요\./);
  assert.equal(recordsPanel().querySelector('tr.board-detail'), null);
  f.click(recordsPanel(), '상세'); await f.settle();
  const records = recordsPanel().querySelector('tr.board-detail').textContent;
  for (const text of ['클라이언트 0.4.38+52', 'display_configuration_failed', '가상 화면 구성 실패', '작업 614c03e4']) assert.ok(records.includes(text), text);
  // 최근 문제 says who could not connect, what happened and why in a row; what to do is in the row's detail.
  const problem = f.content.querySelector('.dt-problems .log-board tbody tr[data-entry]');
  assert.equal(problem.getAttribute('data-advice'), 'admin');
  assert.deepEqual([...problem.querySelectorAll('td')].slice(1, 4).map(node => node.textContent), ['alice', '가상 화면 구성 · 스트리밍', '실패']);
  assert.equal(problem.querySelector('td.col-cause').textContent, '데스크톱이 가상 화면을 준비하지 못했어요.조치 필요');
  assert.equal(problem.querySelector('.tl-fix,.tl-cause,.pill'), null, 'no labelled sentences and no badges in the row');
  f.click(f.content.querySelector('.dt-problems'), '상세'); await f.settle();
  assert.match(f.content.querySelector('.dt-problems tr.board-detail').textContent, /조치데스크톱을 다시 시작하세요\. 반복되면 자세히의 화면 상태를 확인하세요\./);
  // 기록에서 보기 opens 기록 for this desktop.
  f.click(f.content.querySelector('.dt-problems'), '기록에서 보기'); await f.settle();
  assert.equal(f.crumb(), '기록'); assert.equal(f.content.querySelector('#log-tab-problems').getAttribute('aria-selected'), 'true');
  assert.ok(f.requests.some(url => url.endsWith('/admin/client-diagnostics')));
  assert.equal(f.content.querySelector('[data-focus=activity-vm]').querySelector('option[selected]').getAttribute('value'), 'pve-149');
  assert.match(f.content.querySelector('#log-panel').textContent, /alice/);
  assert.equal(f.writes.filter(value => value.path.endsWith('/diagnostics')).length, 0);
});

// Host 관리 merged into 데스크톱: the versions, the update button and the bulk
// update of the old Host table are on the selected desktop, behind the 업데이트
// chip, and in 설정 › 업데이트.
test('Desktops show Host versions, explain unsupported Hosts and confirm a remote Host update with its changelog; a bulk update is followed per desktop', async t => {
  const user = { id: 'owner', username: 'owner', role: 'admin', mustChangeCredentials: false };
  const operation = extra => ({ id: 'a7b3a1f4-5f5a-4bb1-8f0a-2e0f1b7c9d10', status: 'dispatched', code: '', fromVersion: '0.2.59', targetVersion: '0.2.60', progress: null, rebootRequired: false, disconnectSessions: false, submittedAt: '2026-10-25T00:00:00.000Z', checkedAt: '2026-10-25T00:00:00.000Z', ...extra });
  const view = extra => ({ protocol: 'host-update-v1', minimumVersion: '0.2.59', connected: true, supported: true, version: '0.2.59', latestVersion: '0.2.60', newer: true, available: true, sessionActive: false, operation: null, ...extra });
  const vm = (vmid, name, version) => ({ id: 'pve-' + vmid, name, kind: 'vm', pve_vmid: vmid, pve_available: 1, pve_node: 'fixture', pve_power_status: 'running', host: { connected: true, version } });
  const inventory = { users: [user], assignments: [], audit: [], proxmox: { configured: true },
    desktops: [vm(149, 'TESTER', '0.2.59'), vm(150, 'BUSY', '0.2.59'), vm(151, 'LEGACY', '0.2.58'), vm(152, 'UPDATING', '0.2.59')],
    hostUpdates: { latest: { version: '0.2.60', filename: 'Multitap-Host-0.2.60-windows-x64.exe', size: 17000000, sha256: 'a'.repeat(64), changelog: '원격 업데이트 지원\n안정성 개선' },
      desktops: { 'pve-149': view(), 'pve-150': view({ sessionActive: true }), 'pve-151': view({ supported: false, version: '0.2.58', available: false }), 'pve-152': view({ available: false, operation: operation({ status: 'downloading', progress: 42 }) }) } } };
  registerFixtureDesktops(inventory);
  const f = await loadAdmin(t, 'host-update-ui', user, inventory, { respond: url => {
    const match = /\/desktops\/(pve-\d+)\/host-update$/.exec(url);
    if (match) return { ok: true, status: 202, json: async () => ({ hostUpdate: view({ available: false, operation: operation() }) }) };
    if (url.endsWith('/host-updates/all')) return { ok: true, status: 202, json: async () => ({ version: '0.2.60', results: [{ desktopId: 'pve-149', started: true, status: 'dispatched' }, { desktopId: 'pve-150', started: true, status: 'waiting' }] }) };
    return null;
  } });
  f.navigate('desktops'); await f.settle();
  const content = f.content;
  assert.equal(content.querySelector('.hosts-table'), null); assert.equal(content.querySelector('.host-section-tabs'), null);
  const buttonIn = (scope, text) => [...scope.querySelectorAll('button')].find(node => node.textContent === text);
  const main = () => content.querySelector('.dt-main'), hostCell = () => content.querySelector('.desktop-host-summary .host-cell');
  const listed = () => [...content.querySelectorAll('.vm-list-item')].map(node => node.getAttribute('data-focus').replace('vm-pick-', ''));
  // The state condition of the 조회 조건 box and its choices.
  const state = () => content.querySelector('select[data-focus=desktops-state]'), choice = key => [...state().querySelectorAll('option')].find(node => node.getAttribute('value') === key);
  const open = async id => { f.pickVm(id); await f.settle(); };
  const updateWrites = id => f.writes.filter(value => value.path.endsWith('/desktops/' + id + '/host-update'));
  // The list marks what can be updated; the 업데이트 choice counts the same desktops.
  assert.deepEqual(listed(), ['pve-149', 'pve-150', 'pve-151', 'pve-152']);
  assert.deepEqual([...content.querySelectorAll('.vm-list-item')].filter(node => node.closest('tr').querySelector('.vm-list-update')).map(node => node.getAttribute('data-focus')), ['vm-pick-pve-149', 'vm-pick-pve-150']);
  assert.equal(choice('update').textContent, '업데이트 2'); assert.equal(choice('manual'), undefined);
  await open('pve-149');
  // The Host cell is the version and, in plain amber words, that an update waits (the version it would get is the
  // title); the update itself is a plain button of the window's bar, and nothing stands above the form table.
  assert.equal(hostCell().textContent, '0.2.59업데이트 있음'); assert.equal(hostCell().querySelector('.host-note.is-warn').title, '0.2.60 업데이트 가능');
  assert.equal(main().querySelector('.dt-alert'), null); assert.ok(buttonIn(main().querySelector('.win-foot'), 'Host 업데이트'));
  // A Host that cannot be updated remotely says so and is never offered an enabled update.
  await open('pve-151');
  const unsupported = [...hostCell().querySelectorAll('.host-note')].find(node => node.textContent === '원격 업데이트 미지원');
  assert.match(unsupported.title, /업데이트 지원 상태/); assert.doesNotMatch(unsupported.title, /0\.2\.59 이상/);
  assert.ok(hostCell().textContent.startsWith('0.2.58')); assert.equal(content.textContent.includes('0.2.59 이상'), false);
  const legacy = buttonIn(content, 'Host 업데이트');
  assert.ok(!legacy || (legacy.disabled && /업데이트 지원 상태/.test(legacy.title)), 'an unsupported Host has no usable update button');
  await open('pve-152');
  assert.ok(main().textContent.includes('다운로드 중 42%')); assert.equal(buttonIn(content, 'Host 업데이트'), undefined);
  assert.equal(main().querySelector('progress.host-update-progress').getAttribute('value'), '42');
  await open('pve-149');
  f.click(main(), 'Host 업데이트'); await f.settle();
  assert.equal(f.dialog.open, true); assert.ok(f.dialog.textContent.includes('0.2.59 → 0.2.60')); assert.ok(f.dialog.textContent.includes('원격 업데이트 지원'));
  assert.ok(f.dialog.textContent.includes('자동으로 다시 시작하지 않아요'));
  assert.equal(document.querySelector('#modal-save').textContent, '업데이트'); assert.equal(buttonIn(f.dialog, '지금 업데이트(연결 끊김)'), undefined);
  assert.equal(f.writes.some(value => value.path.endsWith('/host-update')), false);
  await document.querySelector('#modal-form').onsubmit({ preventDefault() {} }); await f.settle();
  assert.deepEqual(updateWrites('pve-149').map(value => value.data), [{ version: '0.2.60', disconnectSessions: false }]);
  // A desktop in use offers waiting for the session, or a second explicit choice.
  await open('pve-150');
  f.click(main(), 'Host 업데이트'); await f.settle();
  assert.equal(document.querySelector('#modal-save').textContent, '연결이 끝나면 업데이트');
  assert.deepEqual(updateWrites('pve-150'), [], 'opening the choice requests nothing');
  f.click(f.dialog, '지금 업데이트(연결 끊김)'); await f.settle();
  assert.deepEqual(updateWrites('pve-150').map(value => value.data), [{ version: '0.2.60', disconnectSessions: true }]);
  assert.equal(f.dialog.open, false);
  // The fixture inventory object is shared with the page state; restore the eligible views.
  Object.assign(inventory.hostUpdates.desktops, { 'pve-149': view(), 'pve-150': view({ sessionActive: true }) }); f.navigate('desktops'); await f.settle();
  // The bulk update sits behind the 업데이트 state.
  assert.equal(content.querySelector('.update-bar'), null);
  f.change(state(), 'update'); f.click(content.querySelector('.query-box'), '조회');
  assert.equal(choice('update').hasAttribute('selected'), true); assert.deepEqual(listed(), ['pve-149', 'pve-150']);
  assert.equal(content.querySelector('.update-bar span').textContent, 'Host 0.2.60 · Windows는 다시 시작하지 않아요.');
  f.click(content.querySelector('.update-bar'), '2대 모두 업데이트'); await f.settle();
  assert.equal(document.querySelector('#modal-title').textContent, '데스크톱 2대를 업데이트할까요?');
  // The session list names nobody here, so the count comes from the desktops in use, without a name.
  assert.equal(f.dialog.querySelector('p.confirm-who').textContent, '접속 중인 1대의 연결을 먼저 끊어요');
  assert.ok(f.dialog.textContent.includes('Host를 0.2.60 버전으로 올려요. Windows는 다시 시작하지 않아요.')); assert.ok(f.dialog.textContent.includes('원격 업데이트 지원'));
  assert.equal(document.querySelector('#modal-save').textContent, '연결을 끊고 2대 업데이트');
  assert.ok(buttonIn(f.dialog, '접속 안 한 1대만'));
  assert.equal(f.writes.some(value => value.path.endsWith('/host-updates/all')), false, 'the consequence is shown before any bulk request');
  await document.querySelector('#modal-form').onsubmit({ preventDefault() {} }); await f.settle();
  assert.deepEqual(f.writes.filter(value => value.path.endsWith('/host-updates/all')).map(value => value.data), [{ version: '0.2.60' }]);
  assert.equal(updateWrites('pve-149').length + updateWrites('pve-150').length, 2, 'the bulk request adds no single-desktop request');
  // Each started Host gets a row with the client's stage words and a short
  // reason; installing while the Host is away reads 다시 연결.
  const batchRow = name => [...content.querySelectorAll('.host-batch tbody tr')].find(node => node.textContent.includes(name));
  const stage = name => batchRow(name).querySelector('[data-stage]').getAttribute('data-stage');
  Object.assign(inventory.hostUpdates.desktops, { 'pve-149': view({ available: false, connected: false, operation: operation({ status: 'installing', progress: 100 }) }),
    'pve-150': view({ available: false, sessionActive: true, operation: operation({ status: 'waiting', code: 'session_active' }) }) });
  f.navigate('desktops'); await f.settle();
  assert.equal(content.querySelectorAll('.host-batch tbody tr').length, 2);
  assert.equal(stage('TESTER'), '다시 연결'); assert.ok(batchRow('TESTER').textContent.includes('Host 서비스가 다시 시작돼 연결을 기다려요.'));
  assert.equal(stage('BUSY'), '대기'); assert.ok(batchRow('BUSY').textContent.includes('사용 중인 연결이 끝나면 자동으로 시작해요.'));
  assert.equal(content.querySelector('.host-batch-summary').textContent, '진행 중 2대 · 완료 0대 · 실패 0대');
  Object.assign(inventory.hostUpdates.desktops, { 'pve-149': view({ version: '0.2.60', newer: false, available: false, operation: operation({ status: 'completed', progress: 100, rebootRequired: true }) }),
    'pve-150': view({ available: true, operation: operation({ status: 'failed', code: 'hash_mismatch' }) }) });
  f.navigate('desktops'); await f.settle();
  assert.equal(stage('TESTER'), '완료'); assert.ok(batchRow('TESTER').textContent.includes('Windows를 다시 시작해야 해요.'));
  assert.equal(stage('BUSY'), '실패'); assert.ok(batchRow('BUSY').textContent.includes('파일의 SHA-256이 배포 정보와 달라 중단했어요.'));
  assert.equal(content.querySelector('.host-batch-summary').textContent, '모두 끝났어요 · 완료 1대 · 실패 1대');
  // 업데이트 shows the same result, what is left to update and the failed desktop with its retry; 설정 keeps no copy.
  f.navigate('updates'); await f.settle();
  assert.equal(f.crumb(), '업데이트'); assert.equal(document.querySelector('[data-nav-badge=updates]').textContent, '1', 'one desktop can still be updated');
  assert.equal(content.querySelector('.host-batch-summary').textContent, '모두 끝났어요 · 완료 1대 · 실패 1대');
  const updates = content.querySelector('.settings-updates'), updateRow = text => [...updates.querySelectorAll('.row-line')].find(node => node.querySelector('strong').textContent === text);
  assert.ok(updateRow('데스크톱 1대 → Host 0.2.60')); assert.ok(buttonIn(updateRow('데스크톱 1대 → Host 0.2.60'), '1대 업데이트'));
  assert.equal(updateRow('BUSY · 지난 업데이트 실패').querySelector('small').textContent, '파일의 SHA-256이 배포 정보와 달라 중단했어요.');
  assert.ok(buttonIn(updateRow('BUSY · 지난 업데이트 실패'), '다시 시도'));
  f.click(content.querySelector('.host-batch'), '닫기'); await f.settle();
  assert.equal(content.querySelector('.host-batch'), null);
  f.navigate('desktops'); await f.settle();
  assert.equal(content.querySelector('.host-batch'), null, 'closed everywhere');
});

test('모니터링 › 지금 lists live connections, and 기록 › 화질·속도 summarises streaming performance windows per session with charts, findings and a table view', async t => {
  const user = { id: 'owner', username: 'owner', role: 'admin', mustChangeCredentials: false };
  const inventory = { users: [{ ...user, disabled: 0, notes: '' }, { id: 'u2', username: '홍길동', role: 'user', disabled: 0, notes: '' }], assignments: [], audit: [], proxmox: { configured: true },
    desktops: [{ id: 'pve-149', pve_vmid: 149, name: 'TESTER', pve_power_status: 'running', host: { connected: true } }, { id: 'pve-101', pve_vmid: 101, name: 'DESIGN', pve_power_status: 'running', host: { connected: true } }] };
  const live = '11111111-2222-4333-8444-555555555555', old = '66666666-7777-4888-9999-aaaaaaaaaaaa';
  const windows = (count, values) => Array.from({ length: count }, (_, i) => ({ time: iso(-((count - i) * 30000)), receivedAt: iso(0), values: { samples: 30, windowMs: 30000, width: 1920, height: 1080, srcWidth: 1920, srcHeight: 1080, ...values(i) } }));
  const monitor = {
    sessions: [{ sessionId: live, desktopId: 'pve-149', account: '홍길동', mode: 'stream', attached: true, ready: true }],
    streamStats: [
      { sessionId: live, desktopId: 'pve-149', account: '홍길동', version: '0.4.52', windows: windows(3, i => ({ textureFps10: 480 + i * 10, decodeFps10: 480 + i * 10, decodeFpsMin10: 400, captureFps10: 600, encodeFps10: 590, captureUs: 3000, convertUs: 2000, encodeUs: 18000, decodeUs: 4000, hostWorkUs: 23000, rttMs: 20, inputFrameP50Us: 52000, inputFrameP95Us: 120000, receiveKbps: 20000, targetKbps: 24000, auto: true })) },
      { sessionId: old, desktopId: 'pve-101', account: '홍길동', version: '0.4.51', windows: windows(2, () => ({ textureFps10: 600, decodeFps10: 600, captureFps10: 600, captureUs: 1500, convertUs: 1000, encodeUs: 4000, decodeUs: 2500, rttMs: 12 })) },
    ],
    clientEvents: [{ event: 'streamStart', time: iso(-120000), receivedAt: iso(0), account: '홍길동', version: '0.4.52', desktopId: 'pve-149', sessionId: live, mode: 'stream', values: {} }],
    usage: path => path === '/usage/live' ? { protocol: 'usage-v1', timeZone: 'Asia/Seoul', today: { date: '2026-11-10', logins: 1, connections: 1 }, totals: { logins: 7, connections: 5 }, recordingSince: null, retentionDays: 730, people: 1,
      logins: [{ account: '홍길동', since: iso(-600000), platform: 'windows', version: '0.4.52', address: '203.0.113.9', connections: 1 }],
      connections: [{ sessionId: live, account: '홍길동', desktopId: 'pve-149', desktopName: 'TESTER', mode: 'stream', since: iso(-120000), ready: true, platform: 'windows', version: '0.4.52', address: '203.0.113.9', activeMs: null }] } : null,
  };
  const f = await loadAdmin(t, 'sessions-performance', user, inventory, { monitor });
  // 접속 현황 is in two places now: 모니터링 › 지금 (the live table) and 기록 › 화질·속도.
  f.navigate('monitoring'); await f.settle();
  const content = f.content;
  assert.equal(f.crumb(), '모니터링');
  assert.deepEqual([...content.querySelectorAll('.log-tabs [role=tab]')].map(node => [node.id, node.textContent, node.getAttribute('aria-selected')]),
    [['usage-tab-now', '지금', 'true'], ['usage-tab-users', '사용자별', 'false'], ['usage-tab-desktops', '데스크톱별', 'false'], ['usage-tab-history', '로그인·연결 내역', 'false']]);
  assert.equal(content.querySelector('#usage-panel').getAttribute('aria-labelledby'), 'usage-tab-now');
  assert.deepEqual([...content.querySelectorAll('#usage-panel table')].map(table => [...table.querySelectorAll('th')].map(node => node.textContent)),
    [['사용자', '데스크톱', '클라이언트', '주소', '연결한 지', '화질'], ['사용자', '클라이언트', '주소', '로그인한 지', '연결']], 'every row of 지금 접속 중 is connected, so it has no 상태 column');
  assert.equal(content.querySelector('#usage-panel .pill'), null, 'counts and states are plain text');
  assert.match(content.querySelectorAll('#usage-panel table')[1].querySelector('tbody tr').textContent, /연결 1개$/);
  assert.match(content.querySelectorAll('#usage-panel table')[0].querySelector('tbody tr').textContent, /2분 \(\d\d:\d\d부터\)/, 'how long and since when, on one line');
  // One summary row above the tabs instead of tiles and a second strip.
  assert.equal(content.querySelector('.stat-tiles'), null);
  assert.deepEqual([...content.querySelectorAll('.usage-strip dt')].map(node => node.textContent), ['지금 로그인', '지금 접속 중', '오늘 로그인', '오늘 연결', '누적 로그인', '누적 연결', '사용자', '데스크톱']);
  const liveRows = content.querySelectorAll('#usage-panel table')[0].querySelectorAll('tbody tr');
  assert.equal(liveRows.length, 1); assert.match(liveRows[0].textContent, /홍길동/); assert.match(liveRows[0].textContent, /TESTER/); assert.match(liveRows[0].textContent, /Windows · 0\.4\.52/); assert.match(liveRows[0].textContent, /203\.0\.113\.9/);
  assert.match(liveRows[0].textContent, /50\.0 fps · 20 ms/, 'the latest reported window');
  assert.equal(content.querySelectorAll('.perf-session').length, 0, 'the statistics have their own tab in 기록');
  f.navigate('log'); await f.settle();
  assert.deepEqual([...content.querySelectorAll('.log-tabs [role=tab]')].map(node => [node.id, node.textContent]), [['log-tab-problems', '문제 찾기'], ['log-tab-quality', '화질·속도'], ['log-tab-admin', '관리 작업']], '기록 keeps no second live table');
  f.navigate('log/quality'); await f.settle();
  assert.equal(content.querySelector('#log-tab-quality').getAttribute('aria-selected'), 'true'); assert.equal(content.querySelector('#log-panel tbody tr td[data-label="사용자"]'), null);
  const cards = [...content.querySelectorAll('.perf-session')];
  assert.equal(cards.length, 2);
  // One connection is a row with who, where and its numbers; its charts, findings and table open in a window with 상세.
  assert.match(cards[0].querySelector('.perf-title').textContent, /홍길동 · TESTER \(VM 149\)/); assert.match(cards[0].querySelector('.perf-title').textContent, /접속 중/);
  assert.doesNotMatch(cards[1].querySelector('.perf-title').textContent, /접속 중/, 'only the session that is connected now is marked');
  assert.match(cards[0].textContent, /49\.0 fps/, 'sample-weighted mean of the displayed frames');
  assert.match(cards[0].textContent, /1920×1080/);
  assert.equal(content.querySelector('.perf-window'), null, 'no window before 상세');
  f.click(cards[0], '상세');
  const shown = content.querySelector('#detail .perf-window');
  assert.equal(content.querySelector('#detail').hasAttribute('open'), true);
  assert.match(shown.querySelector('.win-head').textContent, /화질·속도홍길동 · TESTER \(VM 149\)/); assert.match(shown.querySelector('.win-head').textContent, /접속 중/);
  assert.equal(shown.querySelectorAll('svg.chart-plot').length, 4);
  assert.match(shown.querySelector('.findings').textContent, /가장 오래 걸린 단계는 인코딩 18 ms/);
  assert.match(shown.querySelector('.findings').textContent, /입력→화면 지연 p95가 120 ms/);
  assert.deepEqual([...shown.querySelectorAll('.perf-stages .legend li')].map(node => node.textContent), ['캡처3.0 ms', '변환2.0 ms', '인코딩18 ms', '디코딩4.0 ms']);
  assert.equal(shown.querySelectorAll('.table-view tbody tr').length, 3, 'every window is reachable without the chart');
  assert.match(shown.querySelector('.chart-frame').getAttribute('aria-label'), /최소 .* 최대 .* 마지막/);
  f.click(shown.querySelector('.win-foot'), '닫기');
  assert.equal(content.querySelector('.perf-window'), null); assert.equal(content.querySelector('#detail[open]'), null);
  assert.equal(f.focused(), content.querySelector('.perf-session [data-focus^=perf-open-]'), 'focus returns to the 상세 that opened it');
  assert.match(content.textContent, /서버가 측정한 값이 아니에요/);
  // The conditions act when 조회 is pressed.
  const lookUp = () => f.click(content.querySelector('.query-box'), '조회');
  f.change(content.querySelector('[data-focus=perf-vm]'), 'pve-101');
  assert.equal(content.querySelectorAll('.perf-session').length, 2, 'not before 조회');
  lookUp();
  assert.equal(content.querySelectorAll('.perf-session').length, 1);
  assert.match(content.querySelector('.perf-session .perf-title').textContent, /DESIGN \(VM 101\)/);
  f.change(content.querySelector('[data-focus=perf-vm]'), 'all'); f.change(content.querySelector('[data-focus=perf-account]'), '홍길동'); lookUp();
  assert.equal(content.querySelectorAll('.perf-session').length, 2);
  assert.deepEqual(f.writes, [], 'reading connections and statistics writes nothing');
});

test('기록 › 문제 찾기 shows failures first and, under 전체, audit, client and Host records with source, desktop, account and search filters; 관리 작업 is the audit trail', async t => {
  const user = { id: 'owner', username: 'owner', role: 'admin', mustChangeCredentials: false };
  const inventory = { users: [{ ...user, disabled: 0, notes: '' }, { id: 'alice-id', username: 'alice', role: 'user', disabled: 0, notes: '' }], assignments: [], proxmox: { configured: true },
    desktops: [{ id: 'pve-149', pve_vmid: 149, name: 'TESTER', pve_power_status: 'running' }, { id: 'pve-150', pve_vmid: 150, name: 'SECOND', pve_power_status: 'running' }],
    audit: [
      { id: 4, created_at: Date.now() - 1000, event: 'power_failed', actor: 'owner', subject: 'pve-150:shutdown' },
      { id: 3, created_at: Date.now() - 2000, event: 'proxmox_synchronized', actor: 'owner', subject: '2' },
      { id: 2, created_at: Date.now() - 3000, event: 'clipboard_policy_changed', actor: 'owner', subject: 'alice-id:pve-149' },
      { id: 1, created_at: Date.now() - 4000, event: 'host_update_failed', actor: 'owner', subject: 'pve-149:0.2.59>0.2.60:hash_mismatch' },
    ] };
  const monitor = {
    clientEvents: [
      { event: 'displayConfigure', time: iso(-5000), receivedAt: iso(0), account: 'alice', version: '0.4.52', desktopId: 'pve-149', code: 'display_configuration_failed', mode: 'stream', values: { ok: false } },
      { event: 'streamStats', time: iso(-6000), receivedAt: iso(0), account: 'alice', version: '0.4.52', desktopId: 'pve-149', sessionId: '11111111-2222-4333-8444-555555555555', mode: 'stream', values: { textureFps10: 600, rttMs: 12 } },
      { event: 'appStarted', time: iso(-7000), receivedAt: iso(0), account: 'alice', version: '0.4.52', values: {} },
    ],
    hosts: [{ desktopId: 'pve-150', hostname: 'H', version: '0.2.61', connected: true, enrolledAt: iso(-1e6), lastSeenAt: iso(0), diagnostics: { receivedAt: iso(0), stale: false, version: '0.2.61', collectedAt: iso(0), display: {}, native: { available: false }, events: [{ time: iso(-8000), event: 'driverInstall', values: { ok: false, error: 1610 } }] } }],
  };
  const f = await loadAdmin(t, 'activity-filters', user, inventory, { monitor });
  // 활동 기록 is 기록 › 문제 찾기 (failures only until 전체 is pressed) and 기록 › 관리 작업 (the audit trail).
  f.navigate('log'); await f.settle();
  const content = f.content, rows = () => [...content.querySelectorAll('.log-board tbody tr[data-entry]')], titles = () => rows().map(row => row.querySelector('td.col-event').textContent);
  const segment = key => content.querySelector('button[data-segment=' + key + ']'), press = key => segment(key).dispatchEvent(new f.window.Event('click'));
  // The conditions (who, which desktop, how far back, the source, the two switches, the search text) act when 조회 is pressed; 문제만 / 전체 acts at once.
  const lookUp = () => f.click(content.querySelector('.query-box'), '조회');
  const source = key => { f.change(content.querySelector('[data-focus=activity-source]'), key); lookUp(); };
  const failed = '가상 화면 구성 · 스트리밍';
  assert.equal(content.querySelector('#log-tab-problems').getAttribute('aria-selected'), 'true');
  assert.equal(segment('problems').getAttribute('aria-pressed'), 'true'); assert.equal(segment('everything').getAttribute('aria-pressed'), 'false');
  assert.equal(content.querySelector('[data-focus=activity-days]').querySelector('option[selected]').getAttribute('value'), '7');
  assert.deepEqual(titles(), ['VM 전원 요청 실패', 'Host 업데이트 실패', failed, '드라이버 설치'], 'only what failed, from every source');
  assert.equal(content.querySelector('.result-bar strong').textContent, '4건');
  // The source filters and the statistics and bookkeeping switches belong to 전체.
  assert.equal(content.querySelector('[data-focus=activity-source]'), null);
  assert.equal(content.querySelector('[data-focus=activity-stats]'), null); assert.equal(content.querySelector('[data-focus=activity-routine]'), null);
  // A board: 시간 / 사용자 / 데스크톱 / 작업 / 결과 / 원인. A failed connection reads who, which desktop, what and why;
  // the fix, the code and 데스크톱 열기 are in the window its row opens.
  assert.deepEqual([...content.querySelectorAll('.log-board thead th')].map(node => node.textContent), ['시간', '사용자', '데스크톱', '작업', '결과', '원인', '관리']);
  assert.equal(content.querySelector('#log-panel .pill,#log-panel .tl-item,#log-panel .tl-cause'), null, 'no cards, badges or labelled sentences');
  const problem = rows()[2];
  assert.equal(problem.getAttribute('data-advice'), 'admin');
  assert.deepEqual([...problem.querySelectorAll('td')].slice(1, 6).map(node => node.textContent), ['alice', 'TESTER', failed, '실패', '데스크톱이 가상 화면을 준비하지 못했어요.조치 필요']);
  assert.equal(problem.querySelector('td.col-desktop a').getAttribute('href'), '#/desktops/pve-149');
  assert.equal(content.querySelector('.log-window'), null, 'no window before 상세');
  f.click(problem, '상세');
  const shown = content.querySelector('#detail .log-window');
  assert.equal(shown.querySelector('#detail-title').textContent, failed);
  assert.match(shown.querySelector('.facts').textContent, /조치데스크톱을 다시 시작하세요\./); assert.match(shown.querySelector('.facts').textContent, /코드가상 화면 구성 실패 display_configuration_failed/);
  const open = [...shown.querySelectorAll('.win-actions a')].find(node => node.textContent === '데스크톱 열기');
  assert.equal(open.getAttribute('href'), '#/desktops/pve-149');
  f.click(shown.querySelector('.win-foot'), '닫기');
  assert.equal(content.querySelector('.log-window'), null);
  press('everything');
  assert.deepEqual(titles(), ['VM 전원 요청 실패', '클립보드 권한 변경', 'Host 업데이트 실패', failed, '프로그램 시작', '드라이버 설치'], 'routine synchronisation and performance windows are hidden by default');
  assert.match(content.querySelector('.result-bar').textContent, /6건 · 실패 4건/);
  assert.equal(content.querySelector('.log-board thead th.col-cause').textContent, '내용', 'under 전체 the last column says what each record says');
  const audit = rows();
  assert.match(audit[0].textContent, /정상 종료/); assert.match(audit[0].textContent, /SECOND/);
  assert.equal(audit[1].querySelector('td.col-who').textContent, 'alice', 'the account an administrator acted on'); assert.equal(audit[1].querySelector('td.col-who span').title, '수행한 사람 owner'); assert.match(audit[1].textContent, /TESTER/);
  assert.match(audit[5].textContent, /오류 번호 1610/, 'codes keep their digits');
  f.click(audit[2], '상세');
  assert.match(content.querySelector('#detail .log-window .facts').textContent, /0\.2\.59 → 0\.2\.60/); assert.match(content.querySelector('#detail .log-window .facts').textContent, /hash_mismatch/);
  f.click(content.querySelector('#detail .win-foot'), '닫기');
  assert.deepEqual([...content.querySelectorAll('[data-focus=activity-source] option')].map(node => node.textContent), ['모든 출처', '관리', '클라이언트', 'Host']);
  source('host'); assert.deepEqual(titles(), ['드라이버 설치']);
  source('audit'); assert.deepEqual(titles(), ['VM 전원 요청 실패', '클립보드 권한 변경', 'Host 업데이트 실패']);
  source('all');
  f.change(content.querySelector('[data-focus=activity-vm]'), 'pve-149');
  assert.equal(titles().length, 6, 'a changed condition waits for 조회');
  lookUp();
  assert.deepEqual(titles(), ['클립보드 권한 변경', 'Host 업데이트 실패', failed]);
  f.change(content.querySelector('[data-focus=activity-stats]'), true); lookUp();
  assert.ok(titles().includes('스트리밍 성능'));
  f.change(content.querySelector('[data-focus=activity-vm]'), 'all');
  f.change(content.querySelector('[data-focus=activity-account]'), 'alice'); lookUp();
  assert.deepEqual(titles(), ['클립보드 권한 변경', failed, '스트리밍 성능', '프로그램 시작']);
  press('problems');
  assert.deepEqual(titles(), [failed]);
  f.change(content.querySelector('[data-focus=activity-account]'), 'all'); lookUp(); press('everything');
  f.change(content.querySelector('[data-focus=activity-routine]'), true); lookUp();
  assert.ok(titles().includes('Proxmox VM 동기화'));
  const search = content.querySelector('[data-focus=activity-search]');
  search.value = 'display_configuration'; search.dispatchEvent(new f.window.Event('input')); lookUp();
  assert.deepEqual(titles(), [failed]);
  // 초기화 puts every condition back.
  f.click(content.querySelector('.query-box'), '초기화');
  assert.equal(content.querySelector('[data-focus=activity-search]').value, ''); assert.equal(titles().length, 6);
  // 관리 작업: audit records only, with its own search and bookkeeping switch.
  f.navigate('log/admin'); await f.settle();
  assert.equal(content.querySelector('#log-tab-admin').getAttribute('aria-selected'), 'true');
  f.change(content.querySelector('[data-focus=admin-routine]'), false); lookUp();
  assert.deepEqual(titles(), ['VM 전원 요청 실패', '클립보드 권한 변경', 'Host 업데이트 실패']);
  assert.equal(content.querySelector('.result-bar strong').textContent, '3건');
  f.change(content.querySelector('[data-focus=admin-routine]'), true); lookUp();
  assert.deepEqual(titles(), ['VM 전원 요청 실패', 'Proxmox VM 동기화', '클립보드 권한 변경', 'Host 업데이트 실패']);
  const adminSearch = content.querySelector('[data-focus=admin-search]');
  adminSearch.value = 'hash_mismatch'; adminSearch.dispatchEvent(new f.window.Event('input')); lookUp();
  assert.deepEqual(titles(), ['Host 업데이트 실패']);
  assert.deepEqual(f.writes, [], 'looking up records writes nothing');
});

// app.js rewrites the address with history.replaceState. Node has no history,
// so this records the calls and keeps location.hash in step, as a browser does.
function recordHistory(t) {
  const had = Object.hasOwn(globalThis, 'history'), previous = globalThis.history, calls = [];
  const write = kind => (_state, _title, url) => { calls.push([kind, url]); globalThis.location.hash = url; };
  globalThis.history = { pushState: write('push'), replaceState: write('replace') };
  t.after(() => { if (had) globalThis.history = previous; else delete globalThis.history; });
  return calls;
}
const movedAddresses = [
  // old address, the place it opens, its new address, the sidebar entry, the 기록 tab
  ['#/hosts', '데스크톱', '#/desktops', 'desktops', null],
  ['#/sessions', '모니터링', '#/monitoring', 'monitoring', null],
  ['#/log/live', '모니터링', '#/monitoring', 'monitoring', null],
  ['#/server', '업데이트', '#/updates', 'updates', null],
  ['#/settings/update', '업데이트', '#/updates', 'updates', null],
  ['#/settings/downloads', '다운로드', '#/downloads', 'downloads', null],
  ['#/activity', '기록', '#/log', 'log', 'problems'],
  ['#/audit', '기록', '#/log/admin', 'log', 'admin'],
  ['#/assignments', '사용자', '#/users', 'users', null],
];

test('Addresses route to pages and details, older bookmarks open their new places with the address rewritten, and global search opens registered desktops and accounts', async t => {
  const user = { id: 'owner', username: 'owner', role: 'admin', mustChangeCredentials: false };
  const inventory = { users: [{ ...user, disabled: 0, notes: '' }, { id: 'u2', username: '홍길동', role: 'user', disabled: 0, notes: '디자인팀' }], assignments: [], audit: [], proxmox: { configured: false },
    desktops: [{ id: 'pve-149', pve_vmid: 149, name: 'TESTER', pve_node: 'pve-a', pve_power_status: 'running' }, { id: 'pve-150', pve_vmid: 150, name: 'SECOND', pve_power_status: 'stopped' }] };
  registerFixtureDesktops(inventory);
  // Known to Proxmox or reported by a Host, but not registered: never a destination.
  inventory.desktops.push({ id: 'pve-151', pve_vmid: 151, name: 'UNREGISTERED', pve_node: 'pve-a', pve_power_status: 'running' }, { id: 'pc-1', kind: 'pc', name: 'PC 1 미승인' });
  const history = recordHistory(t);
  const selected = () => [...document.querySelectorAll('#sidebar [aria-current=page]')].map(node => node.dataset.page);
  const detail = await loadAdmin(t, 'route-detail', user, inventory, { hash: '#/desktops/pve-149' });
  assert.equal(detail.crumb(), 'VM 149 · TESTER');
  assert.deepEqual(selected(), ['desktops'], 'a detail page highlights its place');
  assert.equal(document.title, 'VM 149 · TESTER · Multitap 관리자 페이지');
  assert.equal(detail.content.querySelector('.dt-head h2').textContent, 'TESTER');
  assert.equal(globalThis.location.hash, '#/desktops/pve-149'); assert.deepEqual(history, [], 'a current address is left alone');
  const unregistered = await loadAdmin(t, 'route-unregistered', user, inventory, { hash: '#/desktops/pve-151' });
  assert.match(unregistered.content.textContent, /데스크톱을 찾지 못했어요/); assert.equal(unregistered.content.querySelector('.dt-head'), null);
  const account = await loadAdmin(t, 'route-user', user, inventory, { hash: '#/users/' + encodeURIComponent('홍길동') });
  assert.equal(account.crumb(), '홍길동'); assert.deepEqual(selected(), ['users']);
  const tab = await loadAdmin(t, 'route-log-tab', user, inventory, { hash: '#/log/quality' });
  assert.equal(tab.crumb(), '기록'); assert.equal(tab.content.querySelector('#log-tab-quality').getAttribute('aria-selected'), 'true');
  assert.equal(globalThis.location.hash, '#/log/quality'); assert.deepEqual(history, []);
  // Bookmarks of the pages that merged open the place that took them over and show its address.
  for (const [old, place, address, entry, logTab] of movedAddresses) {
    const moved = await loadAdmin(t, 'route-moved-' + old.slice(2).replace('/', '-'), user, inventory, { hash: old });
    assert.equal(moved.crumb(), place, old); assert.deepEqual(selected(), [entry], old);
    assert.equal(globalThis.location.hash, address, old + ' is rewritten'); assert.deepEqual(history.at(-1), ['replace', address], old + ' adds no history entry');
    if (logTab) assert.equal(moved.content.querySelector('#log-tab-' + logTab).getAttribute('aria-selected'), 'true', old);
    assert.equal(moved.content.querySelector('.hosts-table'), null);
    assert.deepEqual(moved.writes.filter(write => !write.path.endsWith('/proxmox/sync')), [], old + ' changes nothing');
  }
  const unknown = await loadAdmin(t, 'route-unknown', user, inventory, { hash: '#/no-such-page' });
  assert.equal(unknown.crumb(), '홈'); assert.equal(globalThis.location.hash, '#/');
  const f = await loadAdmin(t, 'global-search', user, inventory);
  // The same when the address is edited or reached with back/forward in an open console.
  for (const [old, place, address, entry, logTab] of movedAddresses) {
    globalThis.location.hash = old; f.emitWindow('hashchange'); await f.settle();
    assert.equal(f.crumb(), place, old); assert.deepEqual(selected(), [entry], old);
    assert.equal(globalThis.location.hash, address, old + ' is rewritten'); assert.deepEqual(history.at(-1), ['replace', address]);
    if (logTab) assert.equal(f.content.querySelector('#log-tab-' + logTab).getAttribute('aria-selected'), 'true', old);
  }
  const box = document.querySelector('#global-search'), list = document.querySelector('#global-search-results');
  assert.equal(box.getAttribute('placeholder'), '데스크톱·사용자 검색');
  const key = name => { const event = new f.window.Event('keydown'); Object.defineProperty(event, 'key', { value: name }); box.dispatchEvent(event); };
  box.value = '디자인'; box.dispatchEvent(new f.window.Event('input'));
  assert.equal(list.hidden, false); assert.equal(box.getAttribute('aria-expanded'), 'true');
  assert.deepEqual([...list.querySelectorAll('[role=option] strong')].map(node => node.textContent), ['홍길동']);
  key('Enter'); await f.settle();
  assert.equal(f.crumb(), '홍길동'); assert.equal(list.hidden, true); assert.equal(box.value, '');
  assert.equal(globalThis.location.hash, '#/users/' + encodeURIComponent('홍길동'));
  box.value = '1'; box.dispatchEvent(new f.window.Event('input'));
  assert.deepEqual([...list.querySelectorAll('[role=option] strong')].map(node => node.textContent), ['VM 149 · TESTER', 'VM 150 · SECOND'], 'registered desktops only');
  key('ArrowDown'); assert.equal(box.getAttribute('aria-activedescendant'), 'search-option-1');
  key('Enter'); await f.settle();
  assert.equal(f.crumb(), 'VM 150 · SECOND'); assert.equal(globalThis.location.hash, '#/desktops/pve-150');
  box.value = 'UNREGISTERED'; box.dispatchEvent(new f.window.Event('input'));
  assert.match(list.textContent, /일치하는 데스크톱이나 사용자가 없어요/);
  box.value = 'zzz'; box.dispatchEvent(new f.window.Event('input'));
  assert.match(list.textContent, /일치하는 데스크톱이나 사용자가 없어요/);
  key('Escape'); assert.equal(list.hidden, true);
});

test('Design pass: labelled table cells for the narrow card layout, icons, info dialogs and dismissible notices', async t => {
  const user = { id: 'owner', username: 'owner', role: 'admin', mustChangeCredentials: false };
  const inventory = { users: [{ ...user, notes: '', disabled: 0, is_primary_admin: 1 }], assignments: [], audit: [], proxmox: { configured: true },
    desktops: [{ id: 'pve-149', name: 'TESTER', kind: 'vm', pve_vmid: 149, pve_available: 1, pve_node: 'fixture', pve_power_status: 'running', host: { connected: false } }] };
  const catalog = [{ product: 'client', version: '0.4.52', filename: 'Multitap-0.4.52-windows-x64.exe', url: '/downloads/Multitap-0.4.52-windows-x64.exe', format: 'installer' }];
  registerFixtureDesktops(inventory);
  const sessions = [{ sessionId: '11111111-2222-4333-8444-555555555555', desktopId: 'pve-149', account: 'owner', mode: 'stream', attached: true, ready: true }];
  const usage = path => path === '/usage/live' ? { protocol: 'usage-v1', timeZone: 'Asia/Seoul', today: { date: '2026-11-10', logins: 1, connections: 1 }, totals: { logins: 1, connections: 1 }, recordingSince: null, retentionDays: 730, people: 1, logins: [],
    connections: [{ sessionId: sessions[0].sessionId, account: 'owner', desktopId: 'pve-149', desktopName: 'TESTER', mode: 'stream', since: iso(-60000), ready: true, platform: 'windows', version: '0.4.52', address: '203.0.113.9', activeMs: null }] } : null;
  const f = await loadAdmin(t, 'design-pass', user, inventory, { monitor: { releases: catalog, sessions, usage }, narrow: true });
  // Static icon placeholders are filled; their SVGs add no text to labels.
  assert.equal(document.querySelectorAll('[data-icon]:empty').length, 0);
  assert.ok(document.querySelector('[data-page=desktops] svg.icon'));
  assert.equal(document.querySelector('[data-page=desktops]').textContent, '데스크톱');
  assert.equal(document.querySelector('.skip-link').getAttribute('href'), '#content');
  // The assignment table is gone (assignments are rows of a list); the tables
  // that remain keep labelled cells, here the live connections of 모니터링.
  const content = f.content;
  f.navigate('users'); await f.settle(); f.follow(content, 'owner'); await f.settle();
  assert.equal(content.querySelector('#assignments table'), null); assert.equal(content.querySelector('.assignment-picker'), null);
  f.navigate('monitoring'); await f.settle();
  const table = content.querySelector('#usage-panel table');
  assert.equal(table.classList.contains('data-table'), true);
  const headers = [...table.querySelectorAll('th')].map(node => node.textContent);
  const cells = [...table.querySelectorAll('tbody tr:first-child td')];
  assert.equal(cells.length, 6);
  assert.deepEqual(cells.map(node => node.getAttribute('data-label')), headers, 'each cell names its column for the card layout');
  assert.ok(cells.every(node => node.getAttribute('role') === 'cell'));
  assert.equal(cells[0].querySelector('a').getAttribute('href'), '#/users/owner', 'the account link is in its labelled cell');
  // The client download is offered in the sign-in footer; inside the console 다운로드 is a place of the sidebar.
  const downloads = [...document.querySelectorAll('[data-client-download]')];
  assert.equal(downloads.length, 1); assert.ok(downloads[0].closest('#login')); assert.equal(document.querySelector('#sidebar .client-download'), null);
  assert.equal(document.querySelector('#sidebar [data-page=downloads]').getAttribute('href'), '#/downloads');
  assert.ok(downloads.every(link => !link.hidden && link.getAttribute('href') === catalog[0].url && link.textContent === 'Windows 클라이언트 0.4.52 다운로드'));
  // A filter that matches nothing says so.
  f.navigate('desktops'); await f.settle();
  const filter = content.querySelector('[data-focus=desktops-search]');
  filter.value = 'no-such-vm'; filter.dispatchEvent(new f.window.Event('input')); f.click(content.querySelector('.query-box'), '조회');
  assert.equal(content.querySelector('.vm-list .empty strong').textContent, '맞는 데스크톱이 없어요');
  assert.equal(content.querySelector('.vm-list-caption span').textContent, '0 / 1대');
  // 초기화 in the empty list does what 초기화 of the conditions does: it clears the search.
  f.click(content.querySelector('.vm-list .empty'), '초기화');
  assert.equal(content.querySelector('[data-focus=desktops-search]').value, ''); assert.equal(content.querySelectorAll('.vm-list-item').length, 1);
  // An information-only dialog offers one closing action, and a later form restores 취소.
  f.pickVm('pve-149'); await f.settle();
  assert.equal(f.find(content.querySelector('.dt-main'), 'button', 'Host 설치 안내'), undefined);
  f.click(content, '데스크톱 추가'); await f.settle();
  assert.equal(f.dialog.open, true); assert.equal(f.dialog.getAttribute('class'), 'is-wide');
  assert.equal(document.querySelector('#modal-title').textContent, '데스크톱 추가');
  assert.equal(document.querySelector('#modal-cancel').hidden, true);
  assert.equal(document.querySelector('#modal-save').textContent, '닫기');
  f.dialog.close();
  f.click(content, '표시 정보 수정');
  assert.equal(document.querySelector('#modal-cancel').hidden, false); assert.equal(f.dialog.getAttribute('class'), '');
  f.dialog.close();
  // Notices carry a tone and can be dismissed.
  const notice = document.querySelector('#notice');
  f.navigate('users'); f.follow(content, 'owner'); await f.settle();
  f.click(content, '비밀번호 재설정');
  const form = document.querySelector('#modal-form');
  form.elements.password.value = form.elements.passwordConfirmation.value = 'Design-pass-password-10!';
  await form.onsubmit({ preventDefault() {} }); await f.settle();
  assert.equal(notice.hidden, false); assert.equal(notice.getAttribute('class'), 'is-success');
  assert.match(notice.textContent, /owner 계정의 비밀번호를 재설정했어요/);
  notice.querySelector('.notice-close').dispatchEvent(new f.window.Event('click'));
  assert.equal(notice.hidden, true);
  // The narrow-screen drawer opens and closes from the top bar.
  document.querySelector('#nav-toggle').dispatchEvent(new f.window.Event('click'));
  assert.equal(document.querySelector('#console').classList.contains('nav-open'), true); assert.equal(document.querySelector('#nav-toggle').getAttribute('aria-expanded'), 'true');
  assert.equal(document.querySelector('#nav-scrim').hidden, false);
  document.querySelector('#nav-close').dispatchEvent(new f.window.Event('click'));
  assert.equal(document.querySelector('#console').classList.contains('nav-open'), false);
  // 서버 정보: this installation's origin, on its own page of the sidebar's 설정 group (it was folded under 고급).
  f.monitor.releases.push({ product: 'host', version: '9.9.9', filename: 'x.exe', url: 'javascript:alert(1)', format: 'installer' });
  f.navigate('server-info'); await f.settle();
  assert.equal(f.crumb(), '서버 정보'); assert.equal(content.querySelector('h1').textContent, '서버 정보');
  // A plain label/value table: no icon per row, the protections in a few words, how they work under 자세히 (the one fold).
  assert.deepEqual([...content.querySelectorAll('details')].map(node => [node.closest('.panel').classList.contains('server-protections'), node.querySelector('summary').textContent]), [[true, '자세히']]);
  assert.equal(content.querySelector('.server-protections .icon,.protection-icon,.pill'), null);
  assert.deepEqual([...content.querySelectorAll('.server-protections .facts dt')].map(node => node.textContent), ['관리자 로그인', '요청 위조 방지', '콘텐츠 보안 정책', '비밀 정보', '진단 수집 범위']);
  assert.doesNotMatch(content.querySelector('.server-protections .facts').textContent, /SameSite|CSRF/, 'the jargon is under 자세히');
  assert.match(content.querySelector('.server-protections details').textContent, /SameSite=Strict/); assert.match(content.querySelector('.server-protections details').textContent, /CSRF/);
  assert.match(content.querySelector('.server-facts').textContent, /https:\/\/cloud\.fixture\.example/);
  assert.deepEqual([...content.querySelectorAll('a[download]')], [], 'the install files are on 다운로드 only');
  // Administrators are the first group of 사용자 (they were a row and a panel of 설정).
  f.navigate('users'); await f.settle();
  assert.deepEqual([...content.querySelectorAll('.account-table tbody > tr')].map(node => node.classList.contains('account-group') ? node.textContent : node.querySelector('a.account-choice').getAttribute('aria-label')), ['관리자1', 'owner']);
  // 다운로드 names the published versions; the host entry with a script URL is not one.
  f.navigate('downloads'); await f.settle();
  assert.deepEqual([...content.querySelectorAll('a[download]')].map(link => link.getAttribute('href')), [catalog[0].url], 'only this server\'s download paths become links');
  assert.match(content.querySelector('.dl-card[data-product=host]').textContent, /이 서버에 게시된 설치 파일이 없어요/);
  assert.match(content.querySelector('.dl-card[data-product=client] .dl-meta').textContent, /버전 0\.4\.52/);
  assert.equal([...content.querySelectorAll('a')].some(link => /^javascript:/i.test(link.getAttribute('href') ?? '')), false);
});

test('다운로드: the public download cards, each with what it is for and this server\'s short links, and the Host guide starts at /get/host', async t => {
  const user = { id: 'owner', username: 'owner', role: 'admin', mustChangeCredentials: false };
  const inventory = { users: [{ ...user, notes: '', disabled: 0, is_primary_admin: 1 }], assignments: [], audit: [], proxmox: { configured: true },
    desktops: [{ id: 'pve-149', name: 'TESTER', kind: 'vm', pve_vmid: 149, pve_available: 1, pve_node: 'fixture', pve_power_status: 'running', host: { connected: false } }] };
  const sha = 'c'.repeat(64), origin = 'https://cloud.fixture.example';
  const releases = [
    { product: 'client', version: '0.4.78', filename: 'Multitap-0.4.78-windows-x64.exe', url: '/downloads/Multitap-0.4.78-windows-x64.exe', format: 'installer', size: 52734567, sha256: sha, releasedAt: '2026-10-26T03:00:00.000Z' },
    { product: 'client', version: '0.4.77', filename: 'Multitap-0.4.77-windows-x64.exe', url: '/downloads/Multitap-0.4.77-windows-x64.exe', format: 'installer', size: 52000000, sha256: sha },
    { product: 'host', version: '0.2.67', filename: 'Multitap-Host-0.2.67-windows-x64.exe', url: '/downloads/Multitap-Host-0.2.67-windows-x64.exe', format: 'installer', size: 18765432, sha256: sha },
    { product: 'host', version: '9.9.9', filename: 'x.exe', url: 'javascript:alert(1)', format: 'installer' },
    ...['arm64-v8a', 'armeabi-v7a'].map(abi => ({ product: 'android-client', version: '0.1.0-preview.5', abi, format: 'apk', filename: `Multitap-0.1.0-preview.5-android-${abi}.apk`, url: `/downloads/Multitap-0.1.0-preview.5-android-${abi}.apk`, size: 24923887, sha256: sha })),
  ];
  const f = await loadAdmin(t, 'downloads-page', user, inventory, { monitor: { releases } });
  const { content } = f;
  // 다운로드 opens from the sidebar and there is no tab bar.
  f.navigate('downloads'); await f.settle();
  assert.equal(document.querySelector('.section-navigation'), null);
  assert.equal(f.crumb(), '다운로드'); assert.equal(content.querySelector('h1').textContent, '다운로드');
  assert.equal(document.querySelector('#sidebar [data-page=downloads]').getAttribute('aria-current'), 'page');
  assert.equal(content.querySelector('a.back-link'), null);
  assert.deepEqual([...document.querySelectorAll('#crumbs li')].map(node => node.textContent), ['다운로드']);
  // Each card is the product's name and one plain line: no eyebrow, no icon tile, no highlighted first card.
  assert.deepEqual([...content.querySelectorAll('.dl-card')].map(node => [node.querySelector('.dl-title').textContent, node.querySelector('.dl-text').textContent]),
    [['Windows 클라이언트', '데스크톱에 접속할 PC에 설치해요.'], ['Multitap Host', '데스크톱으로 쓸 Windows(VM이나 PC)마다 설치해요.'], ['Android 앱', '휴대폰과 태블릿에서 데스크톱에 접속해요.']]);
  assert.equal(content.querySelector('.dl-where,.dl-icon,.dl-card.is-primary,a.dl-button.primary'), null);
  assert.deepEqual([...content.querySelectorAll('.dl-card .dl-short-label')].map(node => node.textContent), ['짧은 주소', '짧은 주소', '짧은 주소']);
  assert.deepEqual([...content.querySelectorAll('.dl-card')].map(node => node.getAttribute('data-product')), ['client', 'host', 'android']);
  assert.deepEqual([...content.querySelectorAll('a.dl-button')].map(link => link.getAttribute('href')), ['/downloads/Multitap-0.4.78-windows-x64.exe', '/downloads/Multitap-Host-0.2.67-windows-x64.exe', '/downloads/Multitap-0.1.0-preview.5-android-arm64-v8a.apk']);
  assert.equal([...content.querySelectorAll('a')].some(link => /^javascript:/i.test(link.getAttribute('href') ?? '')), false, 'only this server\'s download paths become links');
  assert.deepEqual([...content.querySelectorAll('.dl-card .dl-short-box code')].map(node => node.textContent), [`${origin}/get/client`, `${origin}/get/host`, `${origin}/get/android`]);
  assert.deepEqual([...content.querySelectorAll('.dl-address code')].map(node => node.textContent), [origin, `${origin}/download`]);
  // The viewer's own calendar day in the page's language (docs/i18n.md), not the UTC day of the ISO text.
  assert.ok(content.querySelector('.dl-card[data-product=client] .dl-meta').textContent.startsWith('버전 0.4.78' + new Intl.DateTimeFormat('ko-KR', { dateStyle: 'medium' }).format(new Date('2026-10-26T03:00:00.000Z'))));
  assert.equal(content.querySelector('.dl-card[data-product=client] .dl-meta time').getAttribute('datetime'), '2026-10-26T03:00:00.000Z');
  assert.ok(content.querySelector('.dl-card[data-product=host] .dl-verify').textContent.includes(sha));
  assert.equal(content.querySelector('a[href="/download"]').getAttribute('target'), '_blank');
  // The Host installation guide starts with the short link typed inside the
  // Windows that becomes a desktop. With no registered desktop the list offers it by name.
  f.navigate('desktops'); await f.settle();
  assert.equal(content.querySelectorAll('.vm-list-item').length, 0, 'the fixture VM has no registered Host');
  f.click(content.querySelector('.vm-list'), 'Host 설치 안내'); await f.settle();
  assert.equal(f.dialog.open, true); assert.equal(document.querySelector('#modal-title').textContent, '데스크톱 추가');
  assert.deepEqual([...f.dialog.querySelectorAll('.shell-command code')].map(node => node.textContent), [`${origin}/get/host`, origin]);
  assert.match(f.dialog.textContent, /1\. 데스크톱으로 쓸 Windows에서 Host 설치 파일 받기/);
  assert.match(f.dialog.textContent, /Multitap-Host-0\.2\.67-windows-x64\.exe/);
  assert.equal(f.dialog.querySelector('a.download-link').getAttribute('href'), '/downloads/Multitap-Host-0.2.67-windows-x64.exe');
  f.dialog.close();
});

test('Design pass: completing first setup shows its confirmation in the success tone, and a failed login restores the error tone', async t => {
  const user = { id: 'fresh', username: 'admin', role: 'admin', mustChangeCredentials: true };
  const inventory = { users: [], desktops: [], assignments: [], audit: [], proxmox: { configured: false } };
  const f = await loadAdmin(t, 'design-setup', user, inventory, { respond: url => url.endsWith('/login') ? { ok: false, status: 401, json: async () => ({ error: { code: 'invalid_credentials' } }) } : null });
  await f.settle();
  assert.equal(document.querySelector('#setup').hidden, false);
  const setup = document.querySelector('#setup-form');
  setup.elements.username.value = '관리자'; setup.elements.password.value = setup.elements.confirmation.value = 'Design-setup-password-10!';
  setup.dispatchEvent(new f.window.Event('submit')); await f.settle();
  const message = document.querySelector('#login-error');
  assert.equal(document.querySelector('#login').hidden, false);
  assert.equal(message.getAttribute('class'), 'form-message is-success'); assert.match(message.textContent, /관리자 계정을 변경했어요/);
  const login = document.querySelector('#login-form');
  login.elements.password.value = 'wrong-password';
  login.dispatchEvent(new f.window.Event('submit')); await f.settle();
  assert.equal(message.getAttribute('class'), 'error'); assert.match(message.textContent, /아이디 또는 비밀번호/);
});

test('First setup asks for the one-time setup code from the server log and sends it with the new account', async t => {
  const user = { id: 'fresh', username: 'admin', role: 'admin', mustChangeCredentials: true };
  const inventory = { users: [], desktops: [], assignments: [], audit: [], proxmox: { configured: false } };
  let reject = true;
  const f = await loadAdmin(t, 'setup-code', user, inventory, { respond: url => url.endsWith('/setup') && reject ? { ok: false, status: 403, json: async () => ({ error: { code: 'setup_code_invalid' } }) } : null });
  const setup = document.querySelector('#setup-form');
  assert.equal(document.querySelector('#setup').hidden, false);
  const field = setup.querySelector('input[name=setupCode]');
  assert.ok(field, 'the setup screen has a setup code field'); assert.equal(field.getAttribute('autocomplete'), 'one-time-code');
  assert.match(setup.textContent, /설치가 끝날 때 보여 준 설정 코드를 넣고, 새 아이디와 비밀번호를 정해요\./);
  assert.match(setup.textContent, /docker logs 또는 systemd 저널/);
  setup.elements.setupCode.value = '7KQ4-2F9M-XC3P'; setup.elements.username.value = '관리자'; setup.elements.password.value = setup.elements.confirmation.value = 'Design-setup-password-10!';
  setup.dispatchEvent(new f.window.Event('submit')); await f.settle();
  assert.match(document.querySelector('#setup-error').textContent, /설정 코드가 맞지 않아요\..*multitap code/);
  assert.equal(document.querySelector('#setup').hidden, false);
  // Only the code was wrong: the two passwords that were typed are still there.
  assert.equal(setup.elements.password.value, 'Design-setup-password-10!'); assert.equal(setup.elements.confirmation.value, 'Design-setup-password-10!');
  reject = false; setup.elements.setupCode.value = '7KQ4-2F9M-XC3P'; setup.elements.password.value = setup.elements.confirmation.value = 'Design-setup-password-10!';
  setup.dispatchEvent(new f.window.Event('submit')); await f.settle();
  const sent = f.writes.filter(write => write.path.endsWith('/setup'));
  assert.equal(sent.length, 2); assert.deepEqual(sent[1].data, { username: '관리자', password: 'Design-setup-password-10!', passwordConfirmation: 'Design-setup-password-10!', setupCode: '7KQ4-2F9M-XC3P' });
  assert.equal(document.querySelector('#login').hidden, false);
});

test('Host requests show the verification code, say plainly when a Host cannot show one, and guard several claims on one VM', async t => {
  const user = { id: 'owner', username: '관리자', role: 'admin', mustChangeCredentials: false };
  const desktops = [{ id: 'pve-170', pve_vmid: 170, name: 'Windows 업무', pve_os_family: 'windows', pve_available: 1, pve_power_status: 'running' }, { id: 'pve-171', pve_vmid: 171, name: 'Windows 둘', pve_os_family: 'windows', pve_available: 1, pve_power_status: 'running' }];
  const inventory = { users: [{ ...user, disabled: 0, notes: '' }], desktops, assignments: [], audit: [], proxmox: { configured: true } };
  const vm170 = [{ desktopId: 'pve-170', vmid: 170, name: 'Windows 업무', hasExistingHost: false }], vm171 = [{ desktopId: 'pve-171', vmid: 171, name: 'Windows 둘', hasExistingHost: false }];
  const base = { status: 'pending', expiresAt: '2099-01-01T00:00:00Z' };
  const requests = [
    { ...base, requestId: 'real', hostname: 'REAL-PC', version: '0.2.68', machineUuid: 'uuid-170', candidates: vm170, verificationCode: '49J2-HQP1', codeDisplay: 'shown', duplicateRequestIds: ['squat'] },
    { ...base, requestId: 'squat', hostname: 'REAL-PC', version: '0.2.67', machineUuid: 'uuid-170', candidates: vm170, verificationCode: 'Z0ZZ-1111', codeDisplay: 'unavailable', duplicateRequestIds: ['real'] },
    { ...base, requestId: 'legacy', hostname: 'OLD-PC', version: '0.2.66', machineUuid: 'uuid-171', candidates: vm171, verificationCode: 'ABCD-EFGH', codeDisplay: 'unavailable', duplicateRequestIds: [] },
    // No VM in the table or in Proxmox: it may be a PC outside Proxmox.
    { ...base, requestId: 'outside', hostname: 'DESK-PC', version: '1.0.46', machineUuid: 'uuid-pc', candidates: [], vmLookup: { state: 'not_found' }, verificationCode: 'PC12-CODE', codeDisplay: 'shown', duplicateRequestIds: [], localPcRegistration: { allowed: true } },
  ];
  const f = await loadAdmin(t, 'request-codes', user, inventory, { respond: async url => url.endsWith('/hosts/requests') ? { ok: true, status: 200, json: async () => ({ requests, pendingCount: 4 }) } : null });
  const { content, click, dialog, writes, navigate, settle, change } = f;
  // The requests wait at the top of 데스크톱 (the Host 관리 page is gone), and 홈 counts them as one thing to do.
  assert.equal(document.querySelector('[data-nav-badge=home]').textContent, '1'); assert.equal(document.querySelector('[data-nav-badge=hosts]'), null);
  // Nothing is registered or assigned yet, so 홈 is the first-run page on its third step.
  assert.equal(content.querySelector('h1').textContent, '시작하기');
  assert.deepEqual([...content.querySelectorAll('ol.first-steps li')].map(node => node.querySelector('span').textContent), ['서버 접속 설정', 'Proxmox 연결', 'Windows에 Host 설치', '데스크톱 승인', '사용자 만들기', '데스크톱 배정']);
  assert.equal(content.querySelector('ol.first-steps li[aria-current=step] span').textContent, '데스크톱 승인');
  assert.match(content.textContent, /4단계 · 설치한 데스크톱을 승인하세요요청 4건 도착/);
  navigate('desktops'); await settle();
  assert.equal(content.querySelector('.requests-panel h2').textContent, '승인 대기4');
  const [real, squat, legacy, outside] = [...content.querySelectorAll('.requests-panel .request-card')];
  assert.equal(real.querySelector('.request-code-value').textContent, '49J2-HQP1');
  assert.match(real.textContent, /Host 설정 화면에 같은 코드가 표시되는지 확인한 뒤 승인하세요/);
  assert.match(real.querySelector('.request-duplicate').textContent, /연결 요청이 2건 있어요/);
  assert.ok(real.className.includes('is-contested'));
  // A released Host shows no code: said plainly, and it cannot be approved, with or without a rival claim.
  assert.match(squat.textContent, /확인 코드를 표시하지 않아 승인할 수 없어요/);
  const blocked = [...squat.querySelectorAll('button')].find(node => node.textContent === '확인하고 승인');
  assert.equal(blocked.disabled, true); assert.match(blocked.title, /코드가 같아요/);
  assert.equal(legacy.querySelector('.request-duplicate'), null);
  assert.equal([...legacy.querySelectorAll('button')].find(node => node.textContent === '확인하고 승인').disabled, true);
  // The contested request with a code asks for the comparison explicitly and sends it.
  click(real, '확인하고 승인');
  assert.equal(dialog.open, true); assert.match(dialog.textContent, /49J2-HQP1/); assert.match(dialog.textContent, /같은 VM에 대한 다른 요청이 1건 있어요/);
  // One plain checkbox next to the code; until it is ticked nothing is sent.
  const codeBox = dialog.querySelector('[name=acknowledgeCode]');
  assert.equal(codeBox.type, 'checkbox'); assert.equal(codeBox.closest('label').textContent, '코드가 같아요');
  change(dialog.querySelector('[name=acknowledgeDuplicates]'), 'yes');
  const sent = writes.length;
  document.querySelector('#modal-form').dispatchEvent(new f.window.Event('submit')); await settle();
  assert.equal(dialog.open, true); assert.match(document.querySelector('#modal-error').textContent, /코드가 같아요/); assert.equal(writes.length, sent);
  change(codeBox, true);
  document.querySelector('#modal-form').dispatchEvent(new f.window.Event('submit')); await settle();
  const approval = writes.find(write => write.path.endsWith('/real/approve'));
  assert.deepEqual(approval.data, { desktopId: 'pve-170', acknowledgeCode: true, replaceExisting: false, acknowledgeDuplicates: true });
  // A request the server could not place says why in one line, offers typing
  // the VM behind ‘직접 입력’ and registering a local PC on the same card.
  assert.ok(outside.className.includes('is-unmatched'));
  assert.equal(outside.querySelector('.request-match span').textContent, 'Proxmox에서 맞는 VM을 찾지 못했어요.');
  assert.deepEqual([...outside.querySelectorAll('button')].map(node => node.textContent), ['직접 입력', '로컬 PC로 등록', '거절']);
  assert.equal(outside.querySelector('.request-pc-hint').textContent, 'Proxmox 밖의 PC라면 로컬 PC로 등록하세요.');
  const before = writes.length;
  click(outside, '로컬 PC로 등록');
  assert.equal(document.querySelector('#modal-title').textContent, '로컬 PC로 등록'); assert.match(dialog.textContent, /PC12-CODE/);
  // Neither confirmation is answered yet: nothing is sent.
  document.querySelector('#modal-form').dispatchEvent(new f.window.Event('submit')); await settle();
  assert.equal(dialog.open, true); assert.notEqual(document.querySelector('#modal-error').textContent, '');
  assert.equal(writes.length, before);
  change(dialog.querySelector('[name=acknowledgeCode]'), 'yes'); change(dialog.querySelector('[name=acknowledgeUnlinked]'), 'yes');
  document.querySelector('#modal-form').dispatchEvent(new f.window.Event('submit')); await settle();
  assert.deepEqual(writes.slice(before).filter(write => !write.path.endsWith('/proxmox/sync')), [{ path: '/api/v1/admin/hosts/requests/outside/approve-pc', data: { name: 'DESK-PC', acknowledgeCode: true, acknowledgeUnlinked: true } }]);
});

test('Polish: power offers only valid actions and asks before it interrupts, unavailable controls say why, nothing waits to be applied and dialogs start on a safe control', async t => {
  const user = { id: 'owner', username: 'owner', role: 'admin', isPrimaryAdmin: true, mustChangeCredentials: false };
  const member = { id: 'member', username: '사용자', role: 'user', disabled: 0, notes: '' };
  const stopped = { id: 'stopped', username: '중지 사용자', role: 'user', disabled: 1, notes: '' };
  const inventory = { users: [{ id: user.id, username: user.username, role: 'admin', disabled: 0, is_primary_admin: 1, notes: '' }, member, stopped], audit: [], proxmox: { configured: true },
    desktops: [{ id: 'pve-101', pve_vmid: 101, name: '실행 VM', pve_node: 'pve', pve_available: 1, pve_power_status: 'running' }, { id: 'pve-102', pve_vmid: 102, name: '꺼진 VM', pve_node: 'pve', pve_available: 1, pve_power_status: 'stopped' }],
    assignments: [{ username: member.username, desktop_id: 'pve-101' }, { username: stopped.username, desktop_id: 'pve-102' }] };
  // Every power request is refused here so no task polling outlives the test.
  registerFixtureDesktops(inventory);
  const f = await loadAdmin(t, 'polish-controls', user, inventory, { respond: url => url.endsWith('/power') ? { ok: false, status: 409, json: async () => ({ error: { code: 'power_in_progress' } }) } : null });
  const { content, dialog, navigate, settle, follow } = f;
  const buttonIn = (scope, text) => [...scope.querySelectorAll('button')].find(node => node.textContent === text);
  const pane = () => content.querySelector('.dt-main');
  const powerWrites = () => f.writes.filter(write => write.path.endsWith('/power'));
  const cancel = async () => { document.querySelector('#modal-cancel').onclick(); await settle(); };
  navigate('desktops'); await settle();
  // A stopped VM can only be switched on: its button is the request (no confirmation, no picker).
  f.pickVm('pve-102'); await settle();
  // Off is a state, not a problem: no line above the form table, the state and 전원 say it, 켜기 is the main button.
  assert.equal(content.querySelector('.dt-alert'), null);
  assert.equal(pane().querySelector('.desktop-host-summary .state-dot').textContent, '꺼짐'); assert.match(pane().querySelector('.desktop-host-summary').textContent, /전원꺼짐/);
  assert.equal(pane().querySelector('.win-foot .win-main').textContent, '켜기');
  for (const name of ['끄기', '다시 시작', '전원', '시작', '정상 종료', '재시작']) assert.equal(buttonIn(pane(), name), undefined, name);
  assert.equal([...pane().querySelectorAll('button')].filter(node => node.textContent === '켜기').length, 1);
  f.click(pane(), '켜기');
  assert.notEqual(dialog.open, true);
  assert.deepEqual(powerWrites(), [{ path: '/api/v1/admin/desktops/pve-102/power', data: { action: 'start' } }]);
  await settle();
  assert.equal(document.querySelector('#notice').getAttribute('class'), 'is-error');
  assert.equal(powerWrites().length, 1, 'a refused start is not sent again');
  // A running VM can be switched off or restarted, each behind its own question that names who is connected.
  f.pickVm('pve-101'); await settle();
  for (const name of ['켜기', '강제 종료', '리셋', '전원']) assert.equal(buttonIn(pane(), name), undefined, name);
  assert.ok(buttonIn(pane(), '끄기')); assert.ok(buttonIn(pane(), '다시 시작'));
  f.click(pane(), '끄기');
  assert.equal(document.querySelector('#modal-title').textContent, '실행 VM 끄기');
  assert.equal(dialog.querySelector('p.confirm-who').textContent, '지금 접속 중인 사람은 없어요');
  // Nobody is connected: the question does not speak of a connection that would be cut.
  assert.match(dialog.textContent, /저장하지 않은 작업은 사라질 수 있어요/); assert.doesNotMatch(dialog.textContent, /연결이 끊기고/); assert.match(dialog.textContent, /Windows를 정상 종료해요/);
  assert.equal(document.querySelector('#modal-save').textContent, '끄기'); assert.equal(document.querySelector('#modal-save').getAttribute('class'), 'danger');
  assert.equal(dialog.querySelector('#modal-fields select'), null, 'one action per question, no picker');
  assert.equal(f.focused() === document.querySelector('#modal-cancel'), true);
  assert.equal(powerWrites().length, 1, 'opening the confirmation does not issue a power request');
  await cancel();
  assert.equal(powerWrites().length, 1, 'closing the question requests nothing');
  f.click(content.querySelector('.dt-main .win-foot'), '다시 시작');
  assert.equal(document.querySelector('#modal-title').textContent, '실행 VM 다시 시작'); assert.equal(document.querySelector('#modal-save').textContent, '다시 시작');
  assert.match(dialog.textContent, /Windows를 다시 시작해요/);
  assert.equal(f.focused() === document.querySelector('#modal-cancel'), true);
  await document.querySelector('#modal-form').onsubmit({ preventDefault() {} }); await settle();
  assert.deepEqual(powerWrites().slice(1), [{ path: '/api/v1/admin/desktops/pve-101/power', data: { action: 'reboot' } }], 'the answer is exactly one request');
  // The refusal stays in the question; it is not repeated on its own.
  assert.equal(dialog.open, true); assert.notEqual(document.querySelector('#modal-error').textContent, '');
  await cancel();
  assert.equal(powerWrites().length, 2);
  // Assignments: nothing waits to be applied, and an account that cannot be changed says why on each control.
  navigate('users'); follow(content, member.username); await settle();
  for (const name of ['변경 적용', '되돌리기']) assert.equal(buttonIn(content, name), undefined, name);
  assert.equal(content.querySelector('.assignment-review'), null); assert.doesNotMatch(content.textContent, /추가 예정|해제 예정|적용 전/);
  assert.match(content.querySelector('#assignments .note').textContent, /바꾸면 바로 저장돼요/);
  const controls = () => [...content.querySelectorAll('#assignments button[data-preset]'), buttonIn(content, '데스크톱 배정')];
  assert.equal(controls().length, 4); assert.ok(controls().every(node => !node.disabled && !node.hasAttribute('title')));
  follow(content, stopped.username); await settle();
  assert.equal(controls().length, 4);
  for (const node of controls()) { assert.equal(node.disabled, true, node.textContent); assert.equal(node.title, '사용 중지된 계정은 바꿀 수 없어요.'); }
  assert.match(content.querySelector('#assignments .note').textContent, /사용 중지된 계정이에요\. 다시 켜면 배정과 공유를 바꿀 수 있어요/);
  // Deleting names what goes with the account and starts on 취소.
  follow(content, member.username); await settle();
  f.click(content, '삭제');
  assert.match(dialog.textContent, /배정된 데스크톱 1대와 공유 설정도 함께 지워져요/); assert.match(dialog.textContent, /되돌릴 수 없어요/);
  assert.equal(f.focused() === document.querySelector('#modal-cancel'), true);
  dialog.close();
  // Account creation names its action and starts in the login ID field.
  navigate('users'); f.click(content, '사용자 만들기');
  assert.equal(document.querySelector('#modal-save').textContent, '사용자 만들기');
  assert.equal(f.focused(), dialog.querySelector('[name=username]'));
  dialog.close();
  assert.deepEqual(f.writes.filter(write => !write.path.endsWith('/proxmox/sync') && !write.path.endsWith('/power')), []);
});

test('Polish: console copy says Host (not 호스트/에이전트) and uses one sentence ending style', async () => {
  const { readFile, readdir } = await import('node:fs/promises');
  const files = ['app.js', 'proxmox-shell-setup.js', 'i18n/ko.js', ...(await readdir(new URL('../admin/js/', import.meta.url))).map(name => 'js/' + name)];
  for (const file of files) {
    const source = await readFile(new URL('../admin/' + file, import.meta.url), 'utf8');
    // "호스트 이름"/"https://호스트:포트" name a network host and stay.
    assert.equal(/호스트(?! 이름|:포트)|(?<!게스트 )에이전트/.test(source), false, file + ' uses the product term Host');
    // The console speaks 해요체 like the Windows client: no 합니다-style sentence endings.
    assert.equal(/니다[.)'"`]/.test(source), false, file + ' keeps the 해요 style of the console');
  }
});

test('every page is a sidebar entry in three groups; the 설정 group lists its pages and there is no 설정 overview page', async t => {
  const { user, inventory } = workspaceFixture();
  // The Proxmox and access pages render real connection data, so the fixture carries it.
  inventory.proxmox = { configured: true, state: 'ok', endpoint: 'https://pve.fixture.example:8006', tokenId: 'multitap-fixture@pve!broker', hasSecret: true, ca: '' };
  inventory.desktopPreviews = { enabled: true };
  const applied = { protocol: 'server-access-v1', primaryOrigin: 'https://cloud.fixture.example',
    https: { enabled: true, origin: 'https://cloud.fixture.example', mode: 'proxy', listenHost: '127.0.0.1', listenPort: 18480, certFile: null, keyFile: null },
    http: { enabled: false, origin: null, listenHost: '127.0.0.1', listenPort: 18481 }, trustedProxies: ['127.0.0.1'] };
  recordHistory(t);
  const f = await loadAdmin(t, 'approved-five-places', user, inventory, { respond: url => url.endsWith('/server-access')
    ? { ok: true, status: 200, json: async () => ({ protocol: 'server-access-v1', revision: 'a'.repeat(64), applied, pending: null, supported: true, managementOrigin: applied.primaryOrigin }) } : null });
  const current = () => [...document.querySelectorAll('#sidebar [aria-current=page]')].map(node => node.dataset.page);
  // The entry's own label, without its icon or its badge.
  const label = node => node.querySelector('span:not([data-icon]):not(.nav-badge)').textContent;
  const places = [['overview', '홈', '#/'], ['desktops', '데스크톱', '#/desktops'], ['users', '사용자', '#/users'], ['log', '기록', '#/log'], ['monitoring', '모니터링', '#/monitoring'], ['notices', '공지', '#/notices'], ['updates', '업데이트', '#/updates'], ['downloads', '다운로드', '#/downloads']];
  const settingsPages = [['server-access', '서버 접속 설정', '#/server-access'], ['proxmox', 'Proxmox 연결', '#/proxmox'], ['backup', '백업·복원', '#/backup'], ['server-info', '서버 정보', '#/server-info'], ['licenses', '오픈소스 라이선스', '#/licenses']];
  // The navigation table: three groups with dividers, the third under the visible heading 설정; the keyboard order is the visual order.
  const groups = [...document.querySelectorAll('#sidebar .nav .nav-group')];
  assert.deepEqual(groups.map(group => [group.getAttribute('aria-label') ?? document.getElementById(group.getAttribute('aria-labelledby')).textContent, [...group.querySelectorAll('a,button')].map(node => node.dataset.page ?? node.id)]),
    [['매일 보는 곳', ['overview', 'desktops', 'users', 'log', 'monitoring', 'notices']], ['업데이트와 다운로드', ['updates', 'downloads']], ['설정', ['server-access', 'proxmox', 'nav-account-security', 'backup', 'server-info', 'licenses']]]);
  assert.equal(groups[2].querySelector('h2.nav-heading').textContent, '설정'); assert.equal(groups[2].querySelector('.nav-heading a'), null, 'the heading is not a page');
  assert.equal([...document.querySelectorAll('#sidebar .nav a,#sidebar .nav button')].some(node => node.hasAttribute('tabindex')), false);
  assert.equal(document.querySelector('#sidebar .client-download'), null, 'the download card at the bottom is gone');
  assert.deepEqual([...document.querySelectorAll('#sidebar .nav a')].map(node => [node.dataset.page, label(node), node.getAttribute('href')]), [...places, ...settingsPages]);
  for (const gone of ['hosts', 'sessions', 'activity', 'settings', 'server', 'sharing', 'previews']) assert.equal(document.querySelector('#sidebar [data-page=' + gone + ']'), null, gone);
  assert.equal(document.querySelector('[data-nav-badge=hosts]'), null);
  assert.deepEqual([...document.querySelectorAll('#sidebar .nav-badge')].map(node => node.getAttribute('data-nav-badge')), ['home', 'updates'], 'the badges are unchanged');
  assert.equal(document.querySelectorAll('#sidebar [data-icon]:empty').length, 0, 'every entry has its icon');
  // One name per page: the sidebar entry, the breadcrumb and the page heading read the same, and only that entry is marked.
  for (const [page, name, address] of places) {
    f.navigate(page); await f.settle();
    assert.equal(f.crumb(), name); assert.equal(f.content.querySelector('h1').textContent, name); assert.equal(document.title, name + ' · Multitap 관리자 페이지');
    assert.deepEqual(current(), [page]); assert.equal(globalThis.location.hash, address);
    assert.deepEqual([...document.querySelectorAll('#crumbs li')].map(node => node.textContent), [name], 'a place has no parent');
    assert.equal(f.content.querySelector('.section-navigation'), null, 'no tab bar outside the first setup');
  }
  // Each settings page opens straight from its entry: no overview page, no row to click first, no way "back to 설정".
  for (const [page, name, address] of settingsPages) {
    f.navigate(page); await f.settle();
    assert.equal(f.crumb(), name, page); assert.equal(f.content.querySelector('h1').textContent, name, page); assert.equal(document.title, name + ' · Multitap 관리자 페이지');
    assert.deepEqual([...document.querySelectorAll('#crumbs li')].map(node => node.textContent), ['설정', name]);
    assert.equal(document.querySelector('#crumbs li a'), null, '설정 in the breadcrumb is the group name, not a link');
    assert.deepEqual(current(), [page], page + ' marks its own entry'); assert.equal(document.querySelector('#sidebar [data-page=' + page + ']').classList.contains('active'), true);
    assert.equal(globalThis.location.hash, address);
    assert.equal(f.content.querySelector('.section-navigation'), null); assert.equal(f.content.querySelector('a.back-link'), null, page + ' has no back link');
    assert.equal(f.content.querySelector('details.settings-advanced'), null);
  }
  // What each page holds (it was a row, a panel or a part of 고급 on the 설정 page).
  const headings = async page => { f.navigate(page); await f.settle(); return [...f.content.querySelectorAll('.panel > .panel-head h2')].map(node => node.textContent); };
  // There is no 기본 공유 or 미리보기 화면 page: the console has no preview switch although the inventory still carries the state.
  assert.doesNotMatch(document.body.textContent, /기본 공유|미리보기 화면/);
  assert.deepEqual(await headings('server-info'), ['이 서버', '관리자 페이지 보호']);
  assert.match(f.content.querySelector('.server-facts').textContent, /클라이언트 서버 주소[\s\S]*서버 버전/); assert.equal(f.content.querySelectorAll('.protection-list li').length, 5); assert.ok(f.content.querySelector('.health-line'));
  assert.deepEqual(await headings('licenses'), ['구성요소6']); assert.equal(f.content.querySelectorAll('.server-licenses a[target=_blank]').length, 6);
  // 계정 보안 opens its dialog from the sidebar; it is not a page, so the current page stays.
  const security = document.querySelector('#nav-account-security');
  assert.equal(security.tagName, 'BUTTON'); assert.equal(label(security), '계정 보안'); assert.equal(security.getAttribute('aria-haspopup'), 'dialog');
  security.dispatchEvent(new f.window.Event('click')); await f.settle();
  assert.ok(f.requests.some(path => path.endsWith('/mfa')), 'the account security dialog read its state'); assert.equal(f.crumb(), '오픈소스 라이선스');
  // Old addresses of 설정 and of what stood under it.
  for (const [old, name, address] of [['#/settings', '서버 정보', '#/server-info'], ['#/settings/proxmox', 'Proxmox 연결', '#/proxmox'], ['#/settings/server-access', '서버 접속 설정', '#/server-access'], ['#/settings/unknown', '서버 정보', '#/server-info'], ['#/advanced', '서버 정보', '#/server-info'],
    // Two more old addresses: 기본 공유 opens 사용자 (where sharing is chosen), 미리보기 화면 opens 서버 정보.
    ['#/sharing', '사용자', '#/users'], ['#/settings/sharing', '사용자', '#/users'], ['#/previews', '서버 정보', '#/server-info'], ['#/settings/previews', '서버 정보', '#/server-info']]) {
    globalThis.location.hash = old; f.emitWindow('hashchange'); await f.settle();
    assert.equal(f.crumb(), name, old); assert.equal(globalThis.location.hash, address, old + ' is rewritten');
  }
  // The server version in the top bar opens 업데이트.
  f.click(document.querySelector('.top-actions'), '서버 0.8.1'); await f.settle();
  assert.equal(f.crumb(), '업데이트'); assert.deepEqual(current(), ['updates']);
  // The merged Host 관리 address opens 데스크톱; nothing of the old page is left.
  globalThis.location.hash = '#/hosts'; f.emitWindow('hashchange'); await f.settle();
  assert.equal(f.crumb(), '데스크톱'); assert.deepEqual(current(), ['desktops']); assert.equal(globalThis.location.hash, '#/desktops');
  assert.equal(f.content.querySelector('.section-navigation'), null);
  assert.equal(f.content.querySelector('.host-section-tabs'), null); assert.equal(f.content.querySelector('.hosts-table'), null);
  assert.deepEqual(f.writes.filter(write => !write.path.endsWith('/proxmox/sync')), [], 'moving between pages does not mutate enrollment, updates or VM state');
});

test('desktop workspace includes only explicitly approved PCs and never offers them Proxmox power', async t => {
  const { user, inventory } = workspaceFixture();
  inventory.desktops.push({ id: 'pc-owned', kind: 'pc', localPcApproved: true, name: '업무 PC', host: { connected: true, hostname: 'OFFICE' }, wake: { adapters: [{ mac: 'D8:BB:C1:12:34:56', wakeEnabled: false, magicPacket: true }], reportedAt: '2026-11-02T00:00:00.000Z' } },
    { id: 'pc-silent', kind: 'pc', localPcApproved: true, name: '꺼진 PC', host: { connected: false } }, { id: 'pc-manual', kind: 'pc', name: '미승인 PC' });
  inventory.fixtureHosts.push({ desktopId: 'pc-owned', hostId: '00000000-0000-4000-8000-000000000004', version: '1.0.12' }, { desktopId: 'pc-silent', hostId: '00000000-0000-4000-8000-000000000005', version: '1.0.12' });
  // The signed-in administrator is assigned to both PCs: even so, a PC gets no browser Console link.
  inventory.assignments = [{ username: user.username, desktop_id: 'pc-owned' }, { username: user.username, desktop_id: 'pc-silent' }];
  const f = await loadAdmin(t, 'approved-pc-workspace', user, inventory);
  f.navigate('desktops'); await f.settle();
  const items = () => [...f.content.querySelectorAll('.vm-list-item')];
  const pane = () => f.content.querySelector('.dt-main');
  // Connected first (VMs by number, then PCs), then what is off: the stopped VM and the PC that does not answer.
  assert.deepEqual(items().map(node => [node.getAttribute('data-focus').replace('vm-pick-', ''), node.getAttribute('data-status')]),
    [['pve-101', 'ok'], ['pve-170', 'ok'], ['pc-owned', 'ok'], ['pve-102', 'off'], ['pc-silent', 'off']]);
  assert.equal(f.content.textContent.includes('미승인 PC'), false);
  // A PC that does not answer is not a fault the operator can fix from here: 응답 없음, not 접속 불가.
  assert.equal(items()[4].closest('tr').querySelector('.state-dot').textContent, '응답 없음'); assert.equal(items()[3].closest('tr').querySelector('.state-dot').textContent, '꺼짐');
  assert.equal(f.content.querySelector('.vm-table .pill'), null, 'states are a dot and a word in the list');
  assert.equal(f.content.querySelector('select[data-focus=desktops-state] option[value=bad]').textContent, '접속 불가 0'); assert.equal(f.content.querySelector('select[data-focus=desktops-state] option[value=off]').textContent, '꺼짐 2');
  // The old kind filter is gone; grouping by Proxmox node sets the local PCs apart.
  assert.equal(f.content.querySelector('.desktop-kind-filter'), null);
  f.change(f.content.querySelector('input[data-focus=desktops-by-node]'), true);
  const groups = [...f.content.querySelectorAll('section.vm-list-group')];
  assert.deepEqual(groups.map(node => [node.getAttribute('aria-label'), node.querySelector('.vm-list-group-label span').textContent, node.querySelectorAll('.vm-list-item').length]),
    [['Proxmox pve-fixture', 'Proxmox pve-fixture', 3], ['로컬 PC', '로컬 PC (Proxmox 밖)', 2]]);
  f.change(f.content.querySelector('input[data-focus=desktops-by-node]'), false);
  assert.equal(f.content.querySelectorAll('section.vm-list-group').length, 0); assert.equal(items().length, 5);
  f.pickVm('pc-owned'); await f.settle();
  assert.equal(f.crumb(), '로컬 PC · 업무 PC');
  assert.equal(f.content.querySelector('.dt-head h2').textContent, '업무 PC'); assert.equal(f.content.querySelector('.dt-head small').textContent, '로컬 PC');
  assert.match(pane().textContent, /긴급 콘솔지원 안 함/);
  // PC power belongs to the assigned accounts' clients (pc-power-v1); the
  // administrator sees the Host's wake report (in a few words; each card's settings are the title), never a Proxmox power button.
  const wake = pane().querySelector('.wake-fact');
  assert.equal(wake.textContent, 'Windows 설정 꺼짐 · D8:BB:C1:12:34:56');
  assert.match(wake.title, /D8:BB:C1:12:34:56 · Windows 깨우기 꺼짐 · 매직 패킷 켜짐/); assert.match(wake.title, /브로드캐스트 범위/);
  assert.match(pane().textContent, /전원클라이언트에서 켜고 끔/);
  const noPower = () => {
    for (const name of ['켜기', '끄기', '다시 시작', '시작', '정상 종료', '재시작', '전원', '긴급 콘솔']) assert.equal(f.find(pane(), 'button', name), undefined, name);
    assert.equal([...pane().querySelectorAll('a')].some(node => (node.getAttribute('href') ?? '').startsWith('/console/')), false, 'no browser Console for a PC');
  };
  noPower();
  f.pickVm('pc-silent'); await f.settle();
  assert.equal(f.content.querySelector('.dt-alert'), null); assert.equal(f.content.querySelector('.dt-head .state-dot').textContent, '응답 없음');
  noPower();
  f.pickVm('pc-owned'); await f.settle();
  f.click(pane(), '이름 변경');
  assert.equal(document.querySelector('#modal-title').textContent, '로컬 PC 이름 변경');
  f.dialog.querySelector('[name=name]').value = '새 PC 이름';
  document.querySelector('#modal-form').dispatchEvent(new f.window.Event('submit')); await f.settle();
  assert.deepEqual(f.writes, [{ path: '/api/v1/admin/proxmox/sync', data: {} }, { path: '/api/v1/admin/desktops/pc-owned', data: { name: '새 PC 이름' } }, { path: '/api/v1/admin/proxmox/sync', data: {} }], 'rename is the only PC mutation; list entry and post-save refresh keep the existing sync');
  assert.equal(f.requests.some(url => /\/desktops\/pc-(owned|silent)\/power$/.test(url)), false);
});

// Replaces "bulk permission drafts and copy…": there is no draft, no target
// checkboxes and no copy tool. 모두에 한 번에 gives every desktop of the account
// one named choice, saved at once in one atomic request.
test('one sharing choice for all of a user\'s desktops is saved at once in one atomic request limited to that user\'s assigned desktops', async t => {
  const { user, inventory } = workspaceFixture();
  const member = { id: 'bulk-member', username: '권한 사용자', role: 'user', disabled: 0, notes: '' };
  const other = { id: 'other-member', username: '다른 사용자', role: 'user', disabled: 0, notes: '' };
  inventory.users.push(member, other);
  inventory.assignments = [{ username: member.username, desktop_id: 'pve-101' }, { username: member.username, desktop_id: 'pve-170' }, { username: other.username, desktop_id: 'pve-102', clipboardPolicy: { filesToRemote: true } }];
  const f = await loadAdmin(t, 'bulk-assigned-permissions', user, inventory);
  f.navigate('users'); f.follow(f.content, member.username); await f.settle();
  const batches = () => f.writes.filter(write => write.path.endsWith('/assignments/batch'));
  const pressed = id => [...f.content.querySelectorAll('[data-preset][data-vm="' + id + '"]')].filter(node => node.getAttribute('aria-pressed') === 'true').map(node => node.getAttribute('data-preset'));
  assert.deepEqual([...f.content.querySelectorAll('#assignments li.assigned-row')].map(node => node.getAttribute('data-desktop')), ['pve-101', 'pve-170']);
  assert.equal(f.content.querySelector('[data-bulk-target]'), null); assert.equal(f.content.querySelector('.permission-bulk'), null);
  assert.deepEqual(['pve-101', 'pve-170'].map(pressed), [['default'], ['default']]);
  const same = f.content.querySelector('#assignments .same-for-all');
  assert.equal(same.querySelector('span').textContent, '모두에 한 번에');
  assert.deepEqual([...same.querySelectorAll('button[data-same-preset]')].map(node => [node.getAttribute('data-same-preset'), node.textContent]), [['default', '기본'], ['clipboard', '글·그림 복사'], ['all', '모두 허용']]);
  assert.deepEqual(batches(), []);
  same.querySelector('button[data-same-preset=all]').dispatchEvent(new f.window.Event('click')); await f.settle();
  assert.notEqual(f.dialog.open, true, 'turning sharing on is not asked about');
  assert.equal(batches().length, 1, 'both desktops in one request');
  const saved = batches()[0].data;
  assert.deepEqual(Object.keys(saved).sort(), ['assignments', 'expectedAccountId', 'username']);
  assert.equal(saved.username, member.username); assert.equal(saved.expectedAccountId, member.id);
  assert.deepEqual(saved.assignments.map(row => row.desktopId), ['pve-101', 'pve-170']);
  for (const row of saved.assignments) {
    assert.deepEqual(Object.keys(row).sort(), ['clipboardPolicy', 'desktopId', 'devicePolicy'], 'the retired sound-card flag is never sent');
    assert.deepEqual(row.clipboardPolicy, { textToRemote: true, textToLocal: true, filesToRemote: true, filesToLocal: true });
    assert.deepEqual(row.devicePolicy, Object.fromEntries(deviceKeysFixture.map(key => [key, true])), 'every device is on');
  }
  assert.equal(document.querySelector('#notice').getAttribute('class'), 'is-success');
  assert.equal(document.querySelector('#notice .notice-text').textContent, '저장했어요.\n권한 사용자: 데스크톱 2대 · 모두 허용');
  assert.deepEqual(['pve-101', 'pve-170'].map(pressed), [['all'], ['all']]);
  assert.equal(f.content.querySelectorAll('#assignments .saved-mark').length, 2);
  // Narrowing one desktop names every assigned desktop and carries a value only for the changed one.
  // Nobody is connected, so turning things off needs no question.
  f.content.querySelector('button[data-preset=clipboard][data-vm="pve-101"]').dispatchEvent(new f.window.Event('click')); await f.settle();
  assert.notEqual(f.dialog.open, true);
  assert.equal(batches().length, 2);
  assert.deepEqual(batches()[1].data, { username: member.username, expectedAccountId: member.id, assignments: [
    { desktopId: 'pve-101', clipboardPolicy: { textToRemote: true, textToLocal: true, filesToRemote: false, filesToLocal: false }, devicePolicy: deviceDenyFixture }, { desktopId: 'pve-170' }] });
  assert.equal(document.querySelector('#notice .notice-text').textContent, '저장했어요.\n권한 사용자 · 업무 A: 글·그림 복사');
  assert.deepEqual(['pve-101', 'pve-170'].map(pressed), [['clipboard'], ['all']]);
  // Pressing the choice that is already saved requests nothing.
  f.content.querySelector('button[data-preset=clipboard][data-vm="pve-101"]').dispatchEvent(new f.window.Event('click')); await f.settle();
  assert.equal(batches().length, 2);
  assert.deepEqual(inventory.assignments.find(row => row.username === other.username), { username: other.username, desktop_id: 'pve-102', clipboardPolicy: { filesToRemote: true } });
});

// Replaces "failed bulk apply retains the exact draft…": with no draft, a
// refused save must leave the page showing what is saved and must not repeat itself.
test('a refused sharing save shows the saved state again, says so, and does not issue a second request', async t => {
  const { user, inventory } = workspaceFixture();
  const member = { id: 'bulk-failure-member', username: '실패 사용자', role: 'user', disabled: 0, notes: '' };
  inventory.users.push(member); inventory.assignments = [{ username: member.username, desktop_id: 'pve-101' }];
  const f = await loadAdmin(t, 'bulk-failure-draft', user, inventory, { respond: (url, options) => url.endsWith('/assignments/batch') && options.method === 'PUT' ? { ok: false, status: 409, json: async () => ({ error: { code: 'assignment_update_conflict' } }) } : null });
  f.navigate('users'); f.follow(f.content, member.username); await f.settle();
  const batches = () => f.writes.filter(write => write.path.endsWith('/assignments/batch'));
  const box = () => f.content.querySelector('input[data-clipboard-permission=filesToLocal][data-vm="pve-101"]');
  assert.equal(box(), null, 'the switches are folded until 세부 is pressed');
  f.content.querySelector('[data-permission-vm="pve-101"]').dispatchEvent(new f.window.Event('click'));
  assert.equal(box().hasAttribute('checked'), false); assert.equal(box().getAttribute('data-account'), member.username);
  f.change(box(), true); await f.settle();
  assert.deepEqual(batches(), [{ path: '/api/v1/admin/assignments/batch', data: { username: member.username, expectedAccountId: member.id,
    assignments: [{ desktopId: 'pve-101', clipboardPolicy: { ...clipDenyFixture, filesToLocal: true }, devicePolicy: deviceDenyFixture }] } }]);
  const notice = document.querySelector('#notice');
  assert.equal(notice.hidden, false); assert.equal(notice.getAttribute('class'), 'is-error'); assert.doesNotMatch(notice.textContent, /저장했어요/);
  // The page shows what is saved, not what was tried.
  assert.notEqual(box().checked, true); assert.equal(box().hasAttribute('checked'), false);
  assert.equal(f.content.querySelector('.saved-mark'), null);
  assert.equal(f.content.querySelector('button[data-preset=default][data-vm="pve-101"]').getAttribute('aria-pressed'), 'true');
  assert.match(f.content.querySelector('li.assigned-row[data-desktop="pve-101"] .row-text small').textContent, /아무것도 주고받지 않아요/);
  assert.equal(inventory.assignments[0].clipboardPolicy, undefined, 'failed response never commits fixture policy');
  // Nothing retries on its own, also not on the next refresh; the control is free for a deliberate retry.
  await recheck(f);
  assert.equal(batches().length, 1);
  assert.equal(box().disabled, false, 'failure leaves a deliberate manual retry');
  f.change(box(), true); await f.settle();
  assert.equal(batches().length, 2, 'a deliberate retry is one more request');
  assert.deepEqual(batches()[1].data, batches()[0].data);
});

// Replaces "compact assignment candidates keep eighteen targets editable with
// isolated permission drafts…": the candidate table became the 데스크톱 배정
// dialog and each assignment's switches save at once.
test('with eighteen desktops, one sharing editor opens at a time, a change touches one assignment of one account, and the assignment dialog filters without losing a choice; new assignments start denied', async t => {
  const { user, member, inventory } = permissionFixture();
  const other = { id: 'density-other', username: '다른 사용자', role: 'user', disabled: 0, notes: '' };
  inventory.users.push(other);
  inventory.desktops.push(...Array.from({ length: 15 }, (_, i) => {
    const vmid = 104 + i;
    return { ...inventory.desktops[0], id: 'pve-' + vmid, pve_vmid: vmid, name: vmid === 104 ? '아주 긴 이름의 Windows 업무 VM · 개발팀 검증용 작업 공간' : '업무 ' + vmid, pve_power_status: vmid % 2 ? 'running' : 'stopped' };
  }));
  const fullClipboard = Object.fromEntries(Object.keys(clipDenyFixture).map(key => [key, true]));
  const fullDevices = Object.fromEntries(deviceKeysFixture.map(key => [key, true]));
  inventory.assignments.push(
    { username: member.username, desktop_id: 'pve-104', clipboardPolicy: fullClipboard, devicePolicy: fullDevices },
    { username: member.username, desktop_id: 'pve-105', clipboardPolicy: { ...clipDenyFixture, filesToRemote: true }, devicePolicy: deviceDenyFixture },
    { username: other.username, desktop_id: 'pve-103', clipboardPolicy: { ...clipDenyFixture, textToLocal: true }, devicePolicy: deviceDenyFixture },
  );
  const stored = structuredClone(inventory.assignments);
  registerFixtureDesktops(inventory);
  const f = await loadAdmin(t, 'compact-eighteen-assignments', user, inventory);
  f.navigate('users'); f.follow(f.content, member.username); await f.settle();
  const click = node => node.dispatchEvent(new f.window.Event('click'));
  const rows = () => [...f.content.querySelectorAll('#assignments li.assigned-row')].map(node => node.getAttribute('data-desktop'));
  const row = id => f.content.querySelector('li.assigned-row[data-desktop="' + id + '"]');
  const detail = id => row(id).querySelector('[data-permission-vm]');
  const editors = () => [...f.content.querySelectorAll('.sharing-fine')];
  const switches = () => [...editors()[0].querySelectorAll('input[type=checkbox]')];
  const shown = id => [...row(id).querySelectorAll('[data-preset]')].filter(node => node.getAttribute('aria-pressed') === 'true' || node.tagName === 'SPAN').map(node => node.getAttribute('data-preset'));
  const batches = () => f.writes.filter(write => write.path.endsWith('/assignments/batch'));
  const policies = name => inventory.assignments.filter(entry => entry.username === name).map(entry => [entry.desktop_id, entry.clipboardPolicy, entry.devicePolicy]);
  const storedPolicies = name => stored.filter(entry => entry.username === name).map(entry => [entry.desktop_id, entry.clipboardPolicy, entry.devicePolicy]);
  // Only this account's assignments are rows, each with its sharing as one named choice.
  assert.deepEqual(rows(), ['pve-101', 'pve-102', 'pve-104', 'pve-105']);
  assert.equal(f.content.querySelector('.assignment-picker'), null);
  assert.deepEqual(['pve-101', 'pve-102', 'pve-104', 'pve-105'].map(shown), [['custom'], ['default'], ['all'], ['custom']]);
  assert.equal(row('pve-101').querySelector('span[data-preset=custom]').textContent, '직접 설정');
  assert.equal(row('pve-101').querySelector('.row-text small').textContent, 'VM 101 · 글·그림 복사는 데스크톱으로만');
  assert.equal(row('pve-102').querySelector('.row-text small').textContent, 'VM 102 · 아무것도 주고받지 않아요');
  // The list does not duplicate full editors: none until 세부, then exactly one.
  assert.equal(editors().length, 0);
  click(detail('pve-104'));
  assert.equal(editors().length, 1);
  assert.equal(detail('pve-104').getAttribute('aria-expanded'), 'true'); assert.equal(detail('pve-104').textContent, '닫기');
  assert.equal(detail('pve-104').getAttribute('aria-controls'), editors()[0].id);
  assert.equal(switches().length, 4 + deviceKeysFixture.length);
  assert.equal(switches().every(input => input.checked && input.getAttribute('data-vm') === 'pve-104' && input.getAttribute('data-account') === member.username), true, 'the full saved policy is behind 세부');
  click(detail('pve-105'));
  assert.equal(editors().length, 1); assert.equal(detail('pve-105').getAttribute('aria-controls'), editors()[0].id);
  assert.equal(detail('pve-104').getAttribute('aria-expanded'), 'false');
  assert.deepEqual(switches().filter(input => input.checked).map(input => input.getAttribute('data-clipboard-permission')), ['filesToRemote']);
  assert.deepEqual(f.writes, [], 'opening and closing editors writes nothing');
  // One switch: one request that names every desktop of this account and carries a value only for the changed one.
  click(detail('pve-104'));
  f.change(editors()[0].querySelector('[data-clipboard-permission=filesToLocal]'), false); await f.settle();
  assert.notEqual(f.dialog.open, true, 'nobody is connected, so turning it off is not asked about');
  assert.deepEqual(batches(), [{ path: '/api/v1/admin/assignments/batch', data: { username: member.username, expectedAccountId: member.id, assignments: [
    { desktopId: 'pve-101' }, { desktopId: 'pve-102' },
    { desktopId: 'pve-104', clipboardPolicy: { ...fullClipboard, filesToLocal: false }, devicePolicy: fullDevices }, { desktopId: 'pve-105' }] } }]);
  assert.deepEqual(shown('pve-104'), ['custom']);
  assert.equal(row('pve-104').querySelector('.row-text small').textContent, 'VM 104 · 글·그림 복사 양쪽, 파일은 데스크톱으로만, 공유 폴더, 프린터, USB 장치, 스마트 카드, 직렬(COM) 포트');
  assert.ok(row('pve-104').querySelector('.saved-mark')); assert.equal(f.content.querySelectorAll('.saved-mark').length, 1);
  assert.equal(editors().length, 1, 'the editor stays open on the row that was changed');
  assert.equal(editors()[0].querySelector('[data-clipboard-permission=filesToLocal]').hasAttribute('checked'), false);
  // The other assignments of this account and the other account keep every saved policy field.
  assert.deepEqual(policies(member.username).filter(([id]) => id !== 'pve-104'), storedPolicies(member.username).filter(([id]) => id !== 'pve-104'));
  assert.deepEqual(inventory.assignments.filter(entry => entry.username === other.username), stored.filter(entry => entry.username === other.username));

  // 데스크톱 배정: the fourteen registered desktops this account does not have, as native labelled checkboxes.
  f.click(f.content, '데스크톱 배정');
  const boxes = () => [...f.dialog.querySelectorAll('input[data-vm-id]')], box = id => boxes().find(node => node.getAttribute('data-vm-id') === id);
  const visible = () => boxes().filter(node => !node.closest('label').hidden).map(node => node.getAttribute('data-vm-id'));
  const candidates = ['pve-103', ...Array.from({ length: 13 }, (_, i) => 'pve-' + (106 + i))];
  assert.deepEqual(boxes().map(node => node.getAttribute('data-vm-id')), candidates);
  for (const input of boxes()) {
    assert.equal(input.getAttribute('type'), 'checkbox');
    assert.ok(input.closest('label'), 'the assignment control is a native labelled checkbox');
    assert.match(input.getAttribute('aria-label'), /^VM [0-9]+ · .+ 배정$/);
  }
  // With more than six candidates the dialog has a search; a ticked desktop stays in view and in the request.
  const search = f.dialog.querySelector('input[type=search]');
  const type = text => { search.value = text; search.dispatchEvent(new f.window.Event('input')); };
  type('110'); assert.deepEqual(visible(), ['pve-110']);
  f.change(box('pve-110'), true);
  type('107'); assert.deepEqual(visible(), ['pve-107', 'pve-110'], 'filtering does not drop a choice already made');
  f.change(box('pve-107'), true);
  type(''); assert.deepEqual(visible(), candidates);
  assert.equal(batches().length, 1, 'choosing is not a request');
  await document.querySelector('#modal-form').onsubmit({ preventDefault() {} }); await f.settle();
  assert.equal(batches().length, 2);
  assert.deepEqual(batches()[1].data, { username: member.username, expectedAccountId: member.id, desktopIds: ['pve-101', 'pve-102', 'pve-104', 'pve-105', 'pve-107', 'pve-110'] });
  assert.deepEqual(rows(), ['pve-101', 'pve-102', 'pve-104', 'pve-105', 'pve-107', 'pve-110']);
  // A new assignment starts with every sharing item denied; the earlier ones keep their values.
  for (const id of ['pve-107', 'pve-110']) assert.deepEqual(shown(id), ['default']);
  click(detail('pve-107'));
  assert.equal(switches().length, 4 + deviceKeysFixture.length); assert.equal(switches().some(input => input.checked), false, 'a new target starts with every sharing permission denied');
  assert.deepEqual(['pve-101', 'pve-102', 'pve-104', 'pve-105'].map(shown), [['custom'], ['default'], ['custom'], ['custom']]);
  assert.deepEqual(policies(member.username).filter(([id]) => ['pve-101', 'pve-102', 'pve-105'].includes(id)), storedPolicies(member.username).filter(([id]) => id !== 'pve-104'));
  assert.deepEqual(inventory.assignments.filter(entry => entry.username === other.username), stored.filter(entry => entry.username === other.username));
  assert.equal(batches().length, 2);
});

test('Expiry banner: the Proxmox token warns on every page from 30 days, red from 7; the TLS certificate from 14, red from 3', async t => {
  const { user, inventory } = workspaceFixture();
  inventory.proxmox = { configured: true, state: 'ok', endpoint: 'https://pve.fixture.example:8006', tokenId: 'multitap@pve!broker', hasSecret: true, ca: '', expiresAt: iso(10 * 86400000 - 60000), expirySource: 'configured' };
  inventory.audit = [{ id: 7, created_at: Date.now() - 60000, event: 'tls_certificate_expiring', actor: 'server', subject: '3:2026-11-05:0123456789abcdef' }];
  let tls = { state: 'critical', source: 'origin', expiresAt: iso(2 * 86400000 + 60000), daysLeft: 3, checkedAt: iso(0) };
  const respond = url => url === '/api/v1/admin/server-info' ? { ok: true, status: 200, json: async () => ({ service: 'multitap-broker', version: '0.8.1', deployment: { kind: 'unknown', configured: false }, update: { state: 'not_configured', feedConfigured: false, applySupported: false }, tlsCertificate: tls, checkedAt: iso(0) }) } : null;
  const f = await loadAdmin(t, 'expiry-banner', user, inventory, { respond });
  const banner = document.querySelector('#expiry-banner');
  const items = () => [...banner.querySelectorAll('.expiry-item')].map(node => [node.className.replace('expiry-item tone-', ''), node.querySelector('strong').textContent]);
  assert.equal(banner.hidden, false);
  assert.deepEqual(items(), [['warn', 'Proxmox 토큰 만료 10일 전'], ['bad', 'TLS 인증서 만료 3일 전']]);
  // 홈 lists the token among the things that can wait (the banner already says it), and the
  // server's own record of the certificate warning is in 기록 › 관리 작업.
  assert.match(f.content.querySelector('.todo-later').textContent, /급하지 않은 것 1건/); assert.match(f.content.querySelector('.todo-later').textContent, /Proxmox 토큰 만료 10일 전/);
  f.navigate('log/admin'); await f.settle();
  assert.equal(items().length, 2);
  assert.match(f.content.querySelector('#log-panel').textContent, /TLS 인증서 만료 임박/); assert.match(f.content.querySelector('#log-panel').textContent, /3일 전 알림/);
  // Every page, and the token's item opens the Proxmox page.
  for (const page of ['desktops', 'users', 'log', 'server-info', 'licenses']) { f.navigate(page); await f.settle(); assert.equal(items().length, 2, page); }
  f.navigate('users'); await f.settle();
  f.follow(banner, banner.querySelector('a.expiry-item').textContent); await f.settle();
  assert.equal(f.crumb(), 'Proxmox 연결');
  // 30 days and 7 days for the token (pve-health-v1); a renewed token and certificate clear it.
  inventory.proxmox.expiresAt = iso(20 * 86400000);
  document.querySelector('#refresh').dispatchEvent(new f.window.Event('click')); await f.settle();
  assert.deepEqual(items(), [['warn', 'Proxmox 토큰 만료 20일 전'], ['bad', 'TLS 인증서 만료 3일 전']]);
  inventory.proxmox.expiresAt = iso(5 * 86400000);
  document.querySelector('#refresh').dispatchEvent(new f.window.Event('click')); await f.settle();
  assert.deepEqual(items(), [['bad', 'Proxmox 토큰 만료 5일 전'], ['bad', 'TLS 인증서 만료 3일 전']]);
  inventory.proxmox.expiresAt = iso(31 * 86400000); tls = { state: 'ok', source: 'origin', expiresAt: iso(200 * 86400000), daysLeft: 200, checkedAt: iso(0) };
  document.querySelector('#refresh').dispatchEvent(new f.window.Event('click')); await f.settle();
  assert.equal(banner.hidden, true); assert.deepEqual(items(), []);
  inventory.proxmox.expiresAt = iso(-60000);
  document.querySelector('#refresh').dispatchEvent(new f.window.Event('click')); await f.settle();
  assert.deepEqual(items(), [['bad', 'Proxmox 토큰이 만료됐어요']]);
});

test('power and bulk update confirmations name who is connected, a failed Host update offers a retry, and ‘접속 안 한 N대만’ ends no connection', async t => {
  const { user, inventory } = workspaceFixture();
  const view = extra => ({ connected: true, version: '1.0.10', newer: true, supported: true, available: true, latestVersion: '1.0.12', ...extra });
  inventory.hostUpdates = { latest: { version: '1.0.12', changelog: '검증된 변경' }, desktops: { 'pve-170': view({ sessionActive: true }), 'pve-101': view({ operation: { status: 'failed', targetVersion: '1.0.12', code: 'install_failed', rebootRequired: false } }) } };
  const monitor = { sessions: [{ sessionId: 'a7b3a1f4-5f5a-4bb1-8f0a-2e0f1b7c9d10', desktopId: 'pve-170', account: '홍길동', mode: 'stream', attached: true, ready: true }] };
  const respond = url => /\/desktops\/pve-\d+\/host-update$/.test(url) ? { ok: true, status: 202, json: async () => ({ hostUpdate: view({ available: false, operation: { status: 'dispatched', targetVersion: '1.0.12', code: '', rebootRequired: false } }) }) } : null;
  const f = await loadAdmin(t, 'stage3-confirmations', user, inventory, { hash: '#/desktops/pve-170', monitor, respond });
  const cancel = async () => { document.querySelector('#modal-cancel').onclick(); await f.settle(); };
  const who = () => f.dialog.querySelector('p.confirm-who').textContent;
  const main = () => f.content.querySelector('.dt-main');
  const changes = () => f.writes.filter(write => /host-update|\/power/.test(write.path));
  // The desktop in use: its list row and its form table name the account (no line above the table: nothing is
  // wrong), and both power questions name the account and say the connection is cut. The window itself carries
  // no such sentence: it belongs to the question.
  assert.equal(f.content.querySelector('.vm-list-item[data-focus="vm-pick-pve-170"]').getAttribute('data-status'), 'live');
  assert.equal(main().querySelector('.dt-alert'), null);
  const fact = label => [...main().querySelectorAll('.desktop-host-summary dt')].find(node => node.textContent === label)?.nextSibling;
  assert.equal(fact('지금 접속 중').textContent, '홍길동'); assert.equal(fact('전원').textContent, '켜짐');
  assert.doesNotMatch(main().querySelector('#detail-panel-info').textContent, /끊겨요|연결이 끊기고/);
  f.click(main(), '끄기'); await f.settle();
  assert.equal(document.querySelector('#modal-title').textContent, '업무 B 끄기'); assert.equal(who(), '홍길동 님이 지금 접속 중이에요');
  assert.match(f.dialog.textContent, /연결이 끊기고, 저장하지 않은 작업은 사라질 수 있어요/);
  await cancel();
  f.click(main(), '다시 시작'); await f.settle();
  assert.equal(document.querySelector('#modal-title').textContent, '업무 B 다시 시작'); assert.equal(who(), '홍길동 님이 지금 접속 중이에요');
  assert.match(f.dialog.textContent, /연결이 끊기고, 저장하지 않은 작업은 사라질 수 있어요/);
  await cancel(); f.pickVm('pve-101'); await f.settle();
  f.click(main(), '끄기'); await f.settle();
  assert.equal(who(), '지금 접속 중인 사람은 없어요'); assert.doesNotMatch(f.dialog.textContent, /홍길동/);
  await cancel();
  // A run that failed is the one line above the form table, with its reason and 다시 시도 in it; the bar has no
  // second update button. It is offered again by name in 설정 › 업데이트 too.
  const failed = main().querySelector('.dt-alert[data-problem=host-update]');
  assert.equal(failed.querySelector('strong').textContent, '업데이트 실패 · Host 1.0.12'); assert.equal(failed.querySelector('span').textContent, '설치를 완료하지 못했어요.');
  assert.ok(f.find(failed, 'button', '다시 시도'), 'a run that failed is offered again by name'); assert.equal(f.find(main(), 'button', 'Host 업데이트'), undefined);
  assert.equal(main().querySelectorAll('.dt-alert').length, 1);
  assert.equal(f.find(main(), 'button', '다시 시도').getAttribute('aria-label'), 'Host 업데이트 다시 시도');
  f.pickVm('pve-170'); await f.settle();
  assert.ok(f.find(main(), 'button', 'Host 업데이트')); assert.equal(f.find(main(), 'button', '다시 시도'), undefined);
  f.navigate('updates'); await f.settle();
  assert.equal(document.querySelector('[data-nav-badge=updates]').textContent, '2', 'two desktops can be updated');
  const updates = f.content.querySelector('.settings-updates'), row = text => [...updates.querySelectorAll('.row-line')].find(node => node.querySelector('strong').textContent === text);
  assert.ok(f.find(row('업무 A · 지난 업데이트 실패'), 'button', '다시 시도')); assert.equal(row('업무 B · 지난 업데이트 실패'), undefined);
  assert.equal(row('데스크톱 2대 → Host 1.0.12').querySelector('small').textContent, 'Windows는 다시 시작하지 않아요. 접속 중인 1대는 연결을 먼저 끊어요.');
  // 한 대씩 고르기 opens 데스크톱 on the 업데이트 state; the bulk question is the same from both pages.
  f.follow(row('데스크톱 2대 → Host 1.0.12'), '한 대씩 고르기'); await f.settle();
  assert.equal(f.crumb(), '데스크톱'); assert.equal(f.content.querySelector('select[data-focus=desktops-state] option[value=update]').hasAttribute('selected'), true);
  assert.deepEqual([...f.content.querySelectorAll('.vm-list-item')].map(node => node.getAttribute('data-focus')), ['vm-pick-pve-170', 'vm-pick-pve-101'], 'the desktop in use first, then the available one');
  f.click(f.content.querySelector('.update-bar'), '2대 모두 업데이트'); await f.settle();
  assert.equal(document.querySelector('#modal-title').textContent, '데스크톱 2대를 업데이트할까요?');
  assert.equal(who(), '접속 중인 1명의 연결을 먼저 끊어요');
  assert.ok([...f.dialog.querySelectorAll('.modal-description')].some(node => node.textContent === '홍길동 (업무 B)'), 'who is connected to which desktop');
  assert.match(f.dialog.textContent, /Windows는 다시 시작하지 않아요\./);
  assert.equal(document.querySelector('#modal-save').textContent, '연결을 끊고 2대 업데이트');
  assert.deepEqual(changes(), [], 'opening a confirmation requests nothing');
  await cancel();
  assert.deepEqual(changes(), [], 'closing it requests nothing');
  // ‘접속 안 한 1대만’: the single-desktop request for the idle desktop only, never the bulk request.
  f.click(f.content.querySelector('.update-bar'), '2대 모두 업데이트'); await f.settle();
  f.click(f.dialog, '접속 안 한 1대만'); await f.settle();
  assert.deepEqual(changes(), [{ path: '/api/v1/admin/desktops/pve-101/host-update', data: { version: '1.0.12', disconnectSessions: false } }]);
  assert.equal(f.dialog.open, false);
  assert.equal(document.querySelector('#notice .notice-text').textContent, 'Host 1.0.12 업데이트를 1대에 요청했어요.');
  const batch = [...f.content.querySelectorAll('.host-batch tbody tr')];
  assert.equal(batch.length, 1); assert.match(batch[0].textContent, /업무 A/); assert.equal(batch[0].querySelector('[data-stage]').getAttribute('data-stage'), '대기');
  assert.equal(f.writes.some(write => write.path.endsWith('/host-updates/all') || write.path.endsWith('/desktops/pve-170/host-update')), false, 'the desktop in use is left alone');
});

// Expected to fail until the source is corrected (reported, not bent): in
// server/admin/js/hosts.js hostUpdateAll, ‘접속 안 한 N대만’ swallows a refused
// request and counts that desktop as "대상이 아니거나 이미 진행 중", in the success tone.
test('‘접속 안 한 N대만’ says so when the update request was refused, instead of counting the desktop as no target', async t => {
  const { user, inventory } = workspaceFixture();
  const view = extra => ({ connected: true, version: '1.0.10', newer: true, supported: true, available: true, latestVersion: '1.0.12', ...extra });
  inventory.hostUpdates = { latest: { version: '1.0.12', changelog: '검증된 변경' }, desktops: { 'pve-170': view({ sessionActive: true }), 'pve-101': view() } };
  const monitor = { sessions: [{ sessionId: 'a7b3a1f4-5f5a-4bb1-8f0a-2e0f1b7c9d10', desktopId: 'pve-170', account: '홍길동', mode: 'stream', attached: true, ready: true }] };
  const respond = url => /\/desktops\/pve-\d+\/host-update$/.test(url) ? { ok: false, status: 502, json: async () => ({ error: { code: 'host_disconnected' } }) } : null;
  const f = await loadAdmin(t, 'stage7-idle-only-refused', user, inventory, { hash: '#/desktops', monitor, respond });
  f.change(f.content.querySelector('select[data-focus=desktops-state]'), 'update'); f.click(f.content.querySelector('.query-box'), '조회');
  f.click(f.content.querySelector('.update-bar'), '2대 모두 업데이트'); await f.settle();
  f.click(f.dialog, '접속 안 한 1대만'); await f.settle();
  // The request itself is right: one, for the idle desktop, and not repeated.
  assert.deepEqual(f.writes.filter(write => /host-update/.test(write.path)), [{ path: '/api/v1/admin/desktops/pve-101/host-update', data: { version: '1.0.12', disconnectSessions: false } }]);
  // It was refused: nothing was started, and the console must not say otherwise.
  const said = (document.querySelector('#notice').textContent ?? '') + (f.dialog.open ? document.querySelector('#modal-error').textContent : '') + (f.content.querySelector('.host-batch')?.textContent ?? '');
  assert.doesNotMatch(said, /대상이 아니거나 이미 진행 중|대상 아님/, 'a refused request is not a desktop that was no target');
  assert.ok(f.dialog.open ? document.querySelector('#modal-error').textContent !== '' : document.querySelector('#notice').getAttribute('class') === 'is-error', 'the refusal is shown as an error');
});

test('saving over a working Proxmox connection asks once before it writes', async t => {
  const { user, inventory } = workspaceFixture();
  inventory.proxmox = { configured: true, state: 'ok', endpoint: 'https://pve.fixture.example:8006', tokenId: 'multitap-fixture@pve!broker' };
  const f = await loadAdmin(t, 'stage3-proxmox-overwrite', user, inventory, { hash: '#/proxmox' });
  const form = f.content.querySelector('.pve-raw-form');
  form.dispatchEvent(new f.window.Event('submit')); await f.settle();
  assert.match(f.dialog.textContent, /Proxmox 연결 바꾸기/); assert.match(f.dialog.textContent, /지금 저장된 Proxmox 연결 설정을 새 값으로 바꿔요/);
  assert.equal(f.writes.some(write => write.path.endsWith('/proxmox')), false, 'nothing is saved before the answer');
  document.querySelector('#modal-form').dispatchEvent(new f.window.Event('submit')); await f.settle();
  assert.equal(f.writes.filter(write => write.path.endsWith('/proxmox')).length, 1);
});

// Product vocabulary (docs/i18n.md): the remote machine is 데스크톱 everywhere; "PC" is the
// computer in front of the user and a registered physical machine is 로컬 PC.
// Never "이 PC"/"내 PC". 배정, not 할당. Plain status words.
function vocabularyFixture() {
  const { user, inventory } = workspaceFixture();
  const member = { id: 'vocab-member', username: '홍길동', role: 'user', disabled: 0, notes: '' };
  inventory.users.push(member);
  // 업무 C: registered, running, its Host does not answer. 회의실: an approved local PC that does not answer.
  inventory.desktops.push({ id: 'pve-103', pve_vmid: 103, name: '업무 C', pve_node: 'pve-fixture', pve_available: 1, pve_power_status: 'running', pve_os_family: 'windows', pve_os_source: 'proxmox-config', pve_os_type: 'win11', host: { connected: false } },
    { id: 'pc-desk', kind: 'pc', localPcApproved: true, name: '회의실', host: { connected: false } });
  inventory.fixtureHosts.push({ desktopId: 'pve-103', connected: false, version: '0.2.86', enrolledAt: iso(-86400000), lastSeenAt: iso(-3600000) }, { desktopId: 'pc-desk', connected: false, version: '0.2.86', enrolledAt: iso(-86400000) });
  inventory.assignments = [{ username: member.username, desktop_id: 'pve-170' }, { username: member.username, desktop_id: 'pc-desk' }];
  const monitor = { sessions: [{ sessionId: '11111111-2222-4333-8444-555555555555', desktopId: 'pve-170', account: member.username, mode: 'stream', attached: true, ready: true }],
    clientEvents: [{ event: 'streamStart', time: iso(-60000), receivedAt: iso(0), account: member.username, version: '1.0.46', desktopId: 'pve-103', code: 'host_offline', mode: 'stream', values: { ok: false } }] };
  return { user, member, inventory, monitor };
}
const forbiddenWords = /할당|온라인|오프라인|Proxmox 연동|이 PC|내 PC|이 컴퓨터|내 컴퓨터/;

// A list says a state with one small dot and its word, and nothing on the pages is a
// coloured badge. The one exception is the Proxmox setup steps (proxmox-shell-setup.js), which read well as they are.
test('plain lists: no coloured badge on any page, a state is a dot and its word, and no subtitle narrates the page', async t => {
  const { user, inventory, monitor } = vocabularyFixture();
  const f = await loadAdmin(t, 'plain-lists', user, inventory, { monitor });
  for (const page of ['overview', 'desktops', 'users', 'log', 'log/quality', 'log/admin', 'monitoring', 'monitoring/users', 'monitoring/desktops', 'monitoring/history', 'notices', 'updates', 'downloads', 'server-access', 'server-info', 'backup', 'licenses']) {
    f.navigate(page); await f.settle();
    assert.equal(f.content.querySelector('.pill'), null, page + ': no badge');
    assert.doesNotMatch(f.content.querySelector('.page-description')?.textContent ?? '', /줄을 누르면|상세 창이 떠요|할 일이 \d+가지|접속하지 못해요/, page + ': no subtitle that narrates the page');
  }
  // States in the lists: the dot and its word; only a problem colours the word (pages.css).
  f.navigate('desktops'); await f.settle();
  const states = [...f.content.querySelectorAll('.vm-table td.col-state .state-dot')].map(node => [node.textContent, node.className]);
  assert.deepEqual(states, [['접속 불가', 'state-dot is-red'], ['접속 중', 'state-dot is-green'], ['접속 가능', 'state-dot is-blue'], ['꺼짐', 'state-dot'], ['응답 없음', 'state-dot']]);
  f.navigate('users'); await f.settle();
  assert.ok([...f.content.querySelectorAll('.account-table td .state-dot')].length >= 2);
  assert.equal(f.content.querySelector('.account-table .account-role')?.tagName, 'SMALL', 'the role is plain text beside the name');
  // 지금 접속 중 on 홈 and on 모니터링: everyone listed is connected, so neither has a 상태 column.
  f.navigate('overview'); await f.settle();
  assert.deepEqual([...f.content.querySelectorAll('.live-panel th')].map(node => node.textContent), ['사용자', '데스크톱', '연결한 지']);
  assert.equal(f.content.querySelector('.todo-group[data-todo=unreachable] summary small'), null);
  // 기록 › 문제 찾기 is a board, not cards: no red bar, no icon column, no labelled sentences.
  f.navigate('log'); await f.settle();
  assert.ok(f.content.querySelector('table.board.log-board'));
  assert.equal(f.content.querySelector('.tl-item,.tl-source,.tl-cause,.tl-fix,.tl-tag'), null);
  assert.equal(f.content.querySelector('.log-board .icon'), null);
  assert.doesNotMatch(f.content.querySelector('#log-panel').textContent, /무슨 일|해결:|관리자가 할 일|사용자 쪽|문서화되지/);
});

test('one vocabulary: 데스크톱 for the remote machine in page chrome and failure advice, 배정 for assignments, plain status words, never 이 PC or 내 PC', async t => {
  // The sources never spell the forbidden words (메모리 할당, a memory allocation, is not an assignment).
  const { readdir } = await import('node:fs/promises');
  for (const file of ['index.html', 'app.js', 'i18n/ko.js', ...(await readdir(new URL('../admin/js/', import.meta.url))).map(name => 'js/' + name)]) {
    const source = await readFile(new URL('../admin/' + file, import.meta.url), 'utf8');
    assert.doesNotMatch(source, /이 PC|내 PC|이 컴퓨터|내 컴퓨터|(?<!메모리 )할당|Proxmox 연동/, file);
  }
  const { user, member, inventory, monitor } = vocabularyFixture();
  const f = await loadAdmin(t, 'stage7-vocabulary', user, inventory, { hash: '#/hosts', monitor });
  const seen = {};
  // Headings, tabs, the names of the conditions and their fixed choices, tiles, table headers and buttons: the remote machine is
  // never "VM" or "PC" there. A choice that is one desktop (its own label, as in the lists) is data, not chrome.
  const chrome = () => [...[...f.content.querySelectorAll('h1,h2,h3,h4,[role=tab],.query-name,.stat-tile,summary,th,button')].map(node => node.textContent),
    ...[...f.content.querySelectorAll('.query-box label')].map(node => node.firstChild?.nodeType === 3 ? node.firstChild.textContent : ''),
    ...[...f.content.querySelectorAll('.query-box select')].filter(node => !/-vm$/.test(node.getAttribute('data-focus') ?? '')).flatMap(node => [...node.querySelectorAll('option')].map(option => option.textContent))];
  const look = name => {
    seen[name] = f.content.textContent;
    assert.doesNotMatch(seen[name], forbiddenWords, name);
    for (const text of chrome()) assert.doesNotMatch(text, /VM|PC/, name + ' chrome: ' + text);
  };
  assert.equal(f.crumb(), '데스크톱', 'the old Host 관리 address opens 데스크톱');
  assert.doesNotMatch(document.body.textContent, forbiddenWords, 'the static frame and the sign-in screens');
  assert.equal(document.querySelector('#global-search').getAttribute('placeholder'), '데스크톱·사용자 검색');
  assert.deepEqual([...document.querySelectorAll('#sidebar .nav a span:not([data-icon]):not(.nav-badge)')].map(node => node.textContent), ['홈', '데스크톱', '사용자', '기록', '모니터링', '공지', '업데이트', '다운로드', '서버 접속 설정', 'Proxmox 연결', '백업·복원', '서버 정보', '오픈소스 라이선스']);
  for (const page of ['overview', 'desktops', 'users', 'log', 'log/quality', 'log/admin', 'monitoring', 'monitoring/users', 'monitoring/desktops', 'monitoring/history', 'updates', 'downloads', 'backup', 'server-info', 'licenses']) { f.navigate(page); await f.settle(); look(page); }
  // 홈: the numbers and the things to do speak of desktops.
  f.navigate('overview'); await f.settle();
  assert.deepEqual([...f.content.querySelectorAll('a.stat-tile')].map(node => [node.querySelector('span').textContent, node.querySelector('strong').textContent]),
    [['접속할 수 있는 데스크톱', '2 / 5'], ['지금 접속 중', '1'], ['접속 불가', '1'], ['사용자', '2']]);
  assert.deepEqual([...f.content.querySelectorAll('.todo-panel .todo-title')].map(node => node.textContent), ['접속할 수 없는 데스크톱 1대', '최근 24시간 실패 1건']);
  assert.doesNotMatch(seen.overview, /접속 가능한 PC|Host 온라인/);
  // 데스크톱: one plain word per status. A local PC that does not answer is 응답 없음, a VM whose Host does not answer 접속 불가.
  f.navigate('desktops'); await f.settle();
  assert.equal(f.content.querySelector('h1').textContent, '데스크톱');
  assert.deepEqual([...f.content.querySelectorAll('.vm-list-item')].map(node => [node.querySelector('strong').textContent, node.getAttribute('data-status'), node.closest('tr').querySelector('.state-dot').textContent]),
    [['업무 C', 'bad', '접속 불가'], ['업무 B', 'live', '접속 중'], ['업무 A', 'ok', '접속 가능'], ['Linux 보관', 'off', '꺼짐'], ['회의실', 'off', '응답 없음']]);
  // The small line is what the desktop is and whose; the reason stands once beside the name and is read aloud with the row.
  assert.equal(f.content.querySelector('.vm-list-item[data-status=bad] small').textContent, 'VM 103 (pve-fixture) · 사용자 없음');
  assert.equal(f.content.querySelector('.vm-list-item[data-status=bad]').closest('td').querySelector('.vm-list-why').textContent, '응답 없음');
  assert.match(f.content.querySelector('.vm-list-item[data-status=bad]').getAttribute('aria-label'), /접속 불가 · 응답 없음 \(마지막 응답 /);
  assert.equal(f.content.querySelector('.vm-list-item[data-focus="vm-pick-pc-desk"] small').textContent, '로컬 PC · 홍길동');
  assert.deepEqual([...f.content.querySelectorAll('select[data-focus=desktops-state] option')].map(node => node.textContent), ['전체 5', '접속 불가 1', '접속 중 1', '접속 가능 1', '꺼짐 2']);
  for (const id of ['pve-103', 'pve-170', 'pve-101', 'pve-102']) {
    f.pickVm(id); await f.settle(); look(id);
    // Every desktop window is a plain form: no pill among its values (상태 is one dot and its word), no sentence
    // of warning in a cell, and a bar of plain buttons without icons with 삭제 first.
    const win = f.content.querySelector('.dt-main'), summary = win.querySelector('.desktop-host-summary');
    assert.equal(summary.querySelector('.pill'), null, id + ': no pill in the form table');
    assert.equal(summary.querySelectorAll('.state-dot').length, 1, id + ': one state dot');
    assert.equal(summary.querySelector('dd button'), null, id + ': no button inside a cell');
    assert.doesNotMatch(summary.textContent, /끊겨요|분 전 확인|Proxmox .* 확인/, id + ': no explanation in a cell');
    assert.equal(win.querySelector('.dt-head .pill'), null, id + ': the title bar has a dot, not a pill');
    assert.equal(win.querySelector('.win-foot svg'), null, id + ': no icon in the bar');
    assert.equal(win.querySelector('.win-foot .win-actions button').textContent, '삭제', id + ': 삭제 stands first');
    assert.ok(f.find(win.querySelector('.win-foot'), 'button', '표시 정보 수정'), id + ': 표시 정보 수정 is a button of the bar');
  }
  // Nothing is wrong (접속 가능, 꺼짐): nothing stands above the form table.
  for (const id of ['pve-101', 'pve-102']) { f.pickVm(id); await f.settle(); assert.equal(f.content.querySelector('.dt-main .dt-alert'), null, id); }
  f.pickVm('pve-101'); await f.settle();
  assert.equal(f.content.querySelector('.dt-head .state-dot').className, 'state-dot is-blue'); assert.equal(f.content.querySelector('.dt-head .state-dot').textContent, '접속 가능');
  f.pickVm('pve-103'); await f.settle();
  // 접속 불가: one plain line above the form table says what is wrong, and the state says it with a red dot and a few red words.
  const alerts = f.content.querySelectorAll('.dt-main .dt-alert');
  assert.equal(alerts.length, 1); assert.equal(alerts[0].getAttribute('role'), 'alert');
  assert.equal(alerts[0].querySelector('strong').textContent, '지금 접속할 수 없어요'); assert.match(alerts[0].querySelector('span').textContent, /^데스크톱이 응답하지 않아요/);
  assert.equal(alerts[0].querySelector('.icon,svg'), null, 'no icon in the line');
  const state = f.content.querySelector('.dt-main .desktop-host-summary .state-fact');
  assert.equal(state.querySelector('.state-dot').className, 'state-dot is-red'); assert.equal(state.querySelector('.state-dot').textContent, '접속 불가'); assert.equal(state.querySelector('.state-why').textContent, '응답 없음');
  assert.equal(f.content.querySelector('.dt-main .desktop-host-summary .pill'), null);
  assert.equal(f.content.querySelector('#desktop-people h2').textContent, '쓰는 사람0'); assert.ok(f.find(f.content, 'button', '사용자 배정'));
  // Failure advice: the desktop did not answer, in the record (원인) and in what to do (조치, the row's detail).
  const problem = f.content.querySelector('.dt-problems .log-board tr[data-entry]');
  assert.equal(problem.querySelector('td.col-cause').firstChild.textContent, '데스크톱이 서버에 응답하지 않았어요.');
  f.click(f.content.querySelector('.dt-problems'), '상세'); await f.settle();
  assert.match(f.content.querySelector('.dt-problems tr.board-detail').textContent, /조치데스크톱이 켜져 있고 Windows에 로그인돼 있는지 확인하거나 다시 시작하세요\./);
  f.navigate('log'); await f.settle();
  const row = f.content.querySelector('.log-board tr[data-entry]');
  assert.deepEqual([...row.querySelectorAll('td')].slice(1, 4).map(node => node.textContent), ['홍길동', '업무 C', '스트리밍 시작']);
  f.click(row, '상세');
  assert.ok(f.find(f.content.querySelector('#detail .win-foot'), 'a', '데스크톱 열기'), 'the record opens the desktop it is about');
  for (const node of f.content.querySelectorAll('#detail .facts dd')) assert.doesNotMatch(node.textContent, /\bVM\b|\bPC\b/);
  f.click(f.content.querySelector('#detail .win-foot'), '닫기');
  assert.equal(f.content.querySelector('[data-focus=activity-vm]').getAttribute('aria-label'), '데스크톱 필터');
  for (const node of f.content.querySelectorAll('.log-board td.col-cause')) assert.doesNotMatch(node.textContent, /VM|PC/);
  // 사용자: 배정 and 공유, with the user's own computer as PC only in the direction of a transfer.
  f.navigate('users'); f.follow(f.content, member.username); await f.settle(); look('user');
  assert.match(seen.user, /배정/); assert.match(seen.user, /새로 배정한 데스크톱은 ‘기본’으로 시작해요\./); assert.equal(f.content.querySelector('#assignments h2').textContent, '쓸 수 있는 데스크톱2');
  assert.ok(f.find(f.content, 'button', '데스크톱 배정'));
  f.content.querySelector('[data-permission-vm="pve-170"]').dispatchEvent(new f.window.Event('click')); look('user sharing');
  assert.match(f.content.querySelector('.sharing-fine').textContent, /PC ↔ 데스크톱/); assert.match(f.content.querySelector('.sharing-fine').textContent, /PC → 데스크톱/);
  // The questions before an interruption use the same words.
  f.navigate('desktops'); await f.settle(); f.pickVm('pve-170'); await f.settle();
  f.click(f.content.querySelector('.dt-main'), '끄기');
  assert.equal(document.querySelector('#modal-title').textContent, '업무 B 끄기'); assert.doesNotMatch(f.dialog.textContent, forbiddenWords); assert.doesNotMatch(f.dialog.textContent, /VM|PC/);
  document.querySelector('#modal-cancel').onclick();
  assert.deepEqual(f.writes.filter(write => !write.path.endsWith('/proxmox/sync')), []);
});

// Expected to fail until the source is corrected (reported, not bent): the 장치
// 기능 panel of a local PC is described as "이 PC이 지금 제공할 수 있는지예요."
// (server/admin/js/hosts.js devicePanel, `이 ${noun}이`).
test('a local PC is 로컬 PC in its own detail, never 이 PC', async t => {
  const { user, inventory, monitor } = vocabularyFixture();
  const f = await loadAdmin(t, 'stage7-vocabulary-local-pc', user, inventory, { hash: '#/desktops/pc-desk', monitor });
  assert.equal(f.crumb(), '로컬 PC · 회의실');
  assert.equal(f.content.querySelector('.dt-head h2').textContent, '회의실'); assert.equal(f.content.querySelector('.dt-head small').textContent, '로컬 PC');
  assert.equal(f.content.querySelector('.dt-head .state-dot').textContent, '응답 없음'); assert.equal(f.content.querySelector('.dt-alert'), null);
  const description = f.content.querySelector('.device-panel .panel-title p').textContent;
  assert.doesNotMatch(description, /이 PC|내 PC/, 'the 장치 기능 description');
  assert.doesNotMatch(f.content.querySelector('.dt-main').textContent, forbiddenWords);
});
