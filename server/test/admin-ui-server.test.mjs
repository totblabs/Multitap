import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { parseHTML } from 'linkedom';
import { memoryStorage, loadAdmin, iso, workspaceFixture, registerFixtureDesktops, registeredWorkspaceFixture, clipDenyFixture, deviceKeysFixture, deviceDenyFixture, permissionFixture, ownerFixture, recheck, versionServer } from './admin-ui-harness.mjs';

test('server update page uses actual server/config information without promising an unconfigured update channel', async t => {
  for (const kind of ['unknown', 'docker', 'systemd']) await t.test(kind, async t => {
    const user = { id: 'fixture-owner', username: 'owner', role: 'admin', mustChangeCredentials: false };
    const inventory = { users: [user], desktops: [], assignments: [], audit: [], proxmox: { configured: true } };
    const { navigate, settle, click, writes, requests, content, crumb } = await loadAdmin(t, 'server-info-' + kind, user, inventory, { deploymentKind: kind });
    navigate('updates'); await settle();
    assert.equal(document.querySelector('#broker-version').textContent, '0.8.1');
    assert.equal(document.querySelector('#server-version-header').textContent, '0.8.1');
    navigate('overview'); await settle();
    click(document.querySelector('.top-actions'), '서버 0.8.1'); await settle();
    assert.equal(crumb(), '업데이트');
    assert.match(content.textContent, /0.8.1/); assert.match(content.textContent, /연결되지 않았어요/); assert.match(content.textContent, /새 버전과 변경 내역을 조회/);
    assert.equal([...content.querySelectorAll('button')].some(b => b.textContent === '업데이트'), false);
    if (kind === 'unknown') assert.match([...content.querySelectorAll('.server-current [title]')].map(node => node.title).join('\n'), /배포 방식이 명시 설정되지 않아/, 'why the kind is unknown is the title of its value, not a sentence in the cell');
    else assert.match(content.textContent, kind === 'docker' ? /sudo multitap update.*Compose를 직접 운영한다면/s : /초기 관리자 재설정은 사용하지 않아요/);
    assert.equal(content.querySelector('.server-current .pill,.server-current .panel-head .state-dot'), null, 'no 서버 badge on the current server');
    click(content, '업데이트 확인'); await settle();
    assert.ok(requests.filter(path => path.endsWith('/server-info')).length >= 2); assert.deepEqual(writes, []);
    assert.equal(requests.some(path => path.includes('docker.sock') || path.includes('/execute')), false);
  });
});

test('server access unavailable identifies the local management service and can recover on reload', async t => {
  const { user, inventory } = workspaceFixture();
  let unavailable = true;
  const applied = { protocol: 'server-access-v1', primaryOrigin: 'https://cloud.fixture.example',
    https: { enabled: true, origin: 'https://cloud.fixture.example', mode: 'proxy', listenHost: '127.0.0.1', listenPort: 18480, certFile: null, keyFile: null },
    http: { enabled: false, origin: null, listenHost: '127.0.0.1', listenPort: 18481 }, trustedProxies: ['127.0.0.1'] };
  const f = await loadAdmin(t, 'access-unavailable-recovery', user, inventory, { hash: '#/server-access', respond: url => {
    if (url.endsWith('/server-access')) return unavailable
      ? { ok: false, status: 503, json: async () => ({ error: { code: 'server_access_unavailable' } }) }
      : { ok: true, status: 200, json: async () => ({ protocol: 'server-access-v1', revision: 'a'.repeat(64), applied, pending: null, supported: true, managementOrigin: applied.primaryOrigin }) };
  } });
  assert.match(f.content.textContent, /서버 접속 설정 서비스에 연결하지 못했어요/);
  assert.doesNotMatch(f.content.textContent, /입력과 서버 상태/);
  unavailable = false; f.click(f.content, '다시 불러오기'); await f.settle();
  assert.ok(f.content.querySelector('[name="https.origin"]'));
  assert.doesNotMatch(f.content.textContent, /서버 접속 설정 서비스에 연결하지 못했어요/);
  assert.deepEqual(f.writes, []);
});

test('server update errors distinguish unavailable service, metadata check and actual apply failures', async t => {
  for (const [name, update, expected] of [
    ['unavailable', { state: 'unavailable', errorCode: 'server_update_unavailable' }, /업데이트 서비스에 연결하지 못했어요/],
    ['check', { state: 'failed', errorCode: 'update_signature_invalid' }, /새 버전을 확인하지 못했어요/],
    ['apply-before-operation', { state: 'failed', errorCode: 'update_disk_space', releaseId: 'a'.repeat(64), latest: { version: '0.8.2', changelog: '변경' } }, /업데이트를 완료하지 못했어요/],
    ['apply', { state: 'failed', errorCode: 'update_failed', operationId: 'fixture-operation' }, /업데이트를 완료하지 못했어요/],
    ['rollback', { state: 'rolled_back', errorCode: 'update_health_failed', operationId: 'fixture-operation' }, /이전 버전과 데이터를 복구/],
  ]) await t.test(name, async t => {
    const { user, inventory } = workspaceFixture();
    const f = await loadAdmin(t, 'update-error-' + name, user, inventory, { deploymentKind: 'systemd', respond: url => {
      if (url.endsWith('/server-info')) return { ok: true, status: 200, json: async () => ({ service: 'multitap-broker', version: '0.8.1', deployment: { kind: 'systemd', configured: true }, update: { feedConfigured: true, applySupported: false, ...update }, checkedAt: '2026-11-04T01:00:00.000Z' }) };
    } });
    f.navigate('updates'); await f.settle();
    assert.match(f.content.querySelector('.server-release [role="alert"]').textContent, expected);
    assert.deepEqual(f.writes, []);
  });
});

test('configured server update requires changelog review and confirmation before one exact apply request', async t => {
  const user = { id: 'fixture-owner', username: 'owner', role: 'admin', mustChangeCredentials: false }, inventory = { users: [user], desktops: [], assignments: [], audit: [], proxmox: { configured: true } }, releaseId = 'a'.repeat(64);
  const update = { state: 'available', feedConfigured: true, applySupported: true, releaseId, latest: { version: '0.8.2', releasedAt: '2026-10-24T10:00:00.000Z', changelog: '한글 업데이트\n<script>문자 그대로 표시</script>' } };
  let version = '0.8.1', reloads = 0;
  const { navigate, settle, click, writes, dialog, content } = await loadAdmin(t, 'server-update-configured', user, inventory, { deploymentKind: 'systemd', respond: async url => {
    if (url.endsWith('/server-info')) return { ok: true, status: 200, json: async () => ({ service: 'multitap-broker', version, deployment: { kind: 'systemd', configured: true }, update, checkedAt: '2026-10-24T10:00:00.000Z' }) };
    if (url.endsWith('/server-update/apply')) { version = '0.8.2'; update.state = 'succeeded'; return { ok: true, status: 202, json: async () => ({ accepted: true }) }; }
    if (url.endsWith('/server-update/check')) return { ok: true, status: 202, json: async () => ({ accepted: true }) };
  } });
  globalThis.location.reload = () => { reloads++; };
  // The dashboard already names an available server update.
  assert.match(content.textContent, /서버 0\.8\.2 업데이트가 있어요/);
  navigate('updates'); await settle(); assert.match(content.textContent, /한글 업데이트/); assert.equal(content.querySelector('script'), null);
  click(content, '업데이트'); await settle(); assert.equal(dialog.open, true); assert.match(dialog.textContent, /진행 중인 원격 연결은 종료/); assert.deepEqual(writes, []);
  click(dialog, '취소'); await settle(); assert.deepEqual(writes, []);
  t.mock.timers.enable({ apis: ['setTimeout'] });
  click(content, '업데이트'); await settle(); await document.querySelector('#modal-form').onsubmit({ preventDefault() {} }); await settle();
  assert.deepEqual(writes, [{ path: '/api/v1/admin/server-update/apply', data: { releaseId, fromVersion: '0.8.1', confirm: true } }]); assert.equal(dialog.open, false);
  // The new server version reloads the console after a short notice.
  assert.match(document.querySelector('#notice').textContent, /새 버전으로 다시 불러와요/); assert.equal(reloads, 0);
  t.mock.timers.tick(1500); assert.equal(reloads, 1);
});

test('Docker Compose server update shows the verified newer version and its exact commands, with no apply button', async t => {
  const user = { id: 'fixture-owner', username: 'owner', role: 'admin', mustChangeCredentials: false }, inventory = { users: [user], desktops: [], assignments: [], audit: [], proxmox: { configured: true } };
  const commands = ["sed -i '/^MULTITAP_VERSION=/d' .env && echo 'MULTITAP_VERSION=1.0.0' >> .env", 'docker compose pull && docker compose up -d'];
  const update = { state: 'available', feedConfigured: true, applySupported: false, channel: 'docker-release-feed', currentVersion: '0.8.1', releaseId: 'b'.repeat(64),
    latest: { version: '1.0.0', releasedAt: '2026-11-03T00:00:00.000Z', changelog: '공개 1.0.0' },
    manual: { kind: 'docker-compose', version: '1.0.0', commands, sourceCommands: ['git pull', 'docker compose -f compose.yaml -f compose.build.yaml up -d --build'] } };
  const { navigate, settle, click, writes, content } = await loadAdmin(t, 'server-update-docker-manual', user, inventory, { deploymentKind: 'docker', respond: async url => {
    if (url.endsWith('/server-info')) return { ok: true, status: 200, json: async () => ({ service: 'multitap-broker', version: '0.8.1', deployment: { kind: 'docker', configured: true }, update, checkedAt: '2026-11-03T01:00:00.000Z' }) };
    if (url.endsWith('/server-update/check')) { update.state = 'current'; delete update.manual; return { ok: true, status: 202, json: async () => ({ accepted: true }) }; }
  } });
  assert.match(content.textContent, /서버 1\.0\.0 업데이트가 있어요/, 'the dashboard badge also names a Docker update');
  navigate('updates'); await settle();
  assert.match(content.textContent, /새 버전 1\.0\.0/); assert.match(content.textContent, /공개 1\.0\.0/);
  // A server made with the install command: one command; editing Compose by hand and building from source are folded below.
  assert.deepEqual([...content.querySelectorAll('.docker-update > .shell-command code')].map(node => node.textContent), ['sudo multitap update']);
  const folds = [...content.querySelectorAll('.docker-update details')];
  assert.deepEqual(folds.map(node => node.querySelector('summary').textContent), ['Compose를 직접 운영한다면', '소스에서 빌드했다면']);
  assert.deepEqual([...folds[0].querySelectorAll('.shell-command code')].map(node => node.textContent), commands);
  assert.equal([...folds[1].querySelectorAll('.shell-command code')].length, 2);
  assert.equal([...content.querySelectorAll('button')].some(b => b.textContent === '업데이트'), false, 'Docker installs are never applied from the console');
  assert.match(content.textContent, /적용은 서버에서 직접 해요/);
  click(content, '업데이트 확인'); await settle();
  assert.deepEqual(writes.map(w => w.path), ['/api/v1/admin/server-update/check']);
  assert.equal(content.querySelector('.docker-update'), null, 'commands disappear once the server is current');
  assert.match(content.textContent, /최신 버전/);
});

test('header update indicator: the sidebar badge source, shown only for a verified newer version, opens 업데이트, icon only when narrow', async t => {
  const { user, inventory } = ownerFixture();
  let update = { state: 'available', feedConfigured: true, applySupported: false, channel: 'docker-release-feed', currentVersion: '1.0.2', releaseId: 'c'.repeat(64), latest: { version: '1.0.3', releasedAt: '2026-11-03T00:00:00.000Z', changelog: '1.0.3' } };
  let reachable = true, at = Date.parse('2026-11-03T01:00:00.000Z');
  const f = await loadAdmin(t, 'header-update-indicator', user, inventory, { deploymentKind: 'docker', respond: async url => {
    if (!url.endsWith('/server-info')) return null;
    if (!reachable) return { ok: false, status: 503, json: async () => ({ error: { code: 'server_update_unavailable' } }) };
    return { ok: true, status: 200, json: async () => ({ service: 'multitap-broker', version: '1.0.2', deployment: { kind: 'docker', configured: true }, update, checkedAt: new Date(at += 1000).toISOString() }) };
  } });
  const indicator = document.querySelector('#server-update-indicator'), nav = document.querySelector('[data-nav-badge=updates]');
  assert.equal(document.querySelector('[data-nav-badge=server]'), null, '설정 carries no badge any more');
  assert.equal(nav.closest('a').getAttribute('data-page'), 'updates');
  assert.ok(indicator.closest('.top-actions')); assert.equal(indicator.tagName, 'BUTTON'); assert.equal(indicator.getAttribute('type'), 'button');
  assert.equal(indicator.hidden, false, 'a Docker install badges its verified release too'); assert.equal(nav.hidden, false, 'the same check badges the sidebar'); assert.equal(nav.textContent, '1'); assert.equal(nav.getAttribute('aria-label'), '업데이트할 수 있어요: 서버 1.0.3');
  assert.equal(indicator.querySelector('.update-indicator-label').textContent, '업데이트 1.0.3');
  assert.equal(indicator.getAttribute('aria-label'), '서버 1.0.3 업데이트 가능'); assert.equal(indicator.getAttribute('title'), '서버 1.0.3 업데이트 가능');
  assert.ok(indicator.querySelector('[data-icon] svg'));
  indicator.dispatchEvent(new f.window.Event('click')); await f.settle();
  assert.equal(f.crumb(), '업데이트');
  // A narrow top bar keeps the button and its icon; only the label hides there.
  const css = await readFile(new URL('../admin/server-update-badge.css', import.meta.url), 'utf8');
  assert.match(css, /@media\(max-width:1250px\)\{#console \.update-indicator\{[^}]*\}\.update-indicator-label\{display:none\}\}/);
  assert.equal(css.match(/\.update-indicator-label\{display:none\}/g).length, 1, 'wide top bars show the label');
  // Nothing when the server is current, its check failed or server information is unavailable.
  for (const [why, change] of [['current', () => { update = { ...update, state: 'current' }; }], ['check failed', () => { update = { ...update, state: 'failed', errorCode: 'update_download_failed' }; }], ['server information failed', () => { reachable = false; }]]) {
    update = { ...update, state: 'available' }; delete update.errorCode; reachable = true;
    await recheck(f); assert.equal(indicator.hidden, false, why);
    change(); await recheck(f);
    assert.equal(indicator.hidden, true, why); assert.equal(nav.hidden, true, why); assert.equal(nav.textContent, '', why); assert.equal(nav.getAttribute('aria-label'), null, why); assert.equal(indicator.getAttribute('aria-label'), null, why);
  }
});

test('a changed server version reloads the console once after a short notice, and a flapping version cannot loop', async t => {
  const { user, inventory } = ownerFixture(), server = versionServer('1.0.2'), storage = memoryStorage();
  let reloads = 0;
  const f = await loadAdmin(t, 'version-reload', user, inventory, { respond: server.respond, storage });
  globalThis.location.reload = () => { reloads++; };
  t.mock.timers.enable({ apis: ['setTimeout'] });
  await recheck(f); t.mock.timers.tick(5000); assert.equal(reloads, 0, 'the same version never reloads');
  server.version = '1.0.3'; await recheck(f);
  assert.equal(document.querySelector('#notice .notice-text').textContent, '새 버전으로 다시 불러와요.'); assert.equal(reloads, 0);
  await recheck(f); t.mock.timers.tick(1500); assert.equal(reloads, 1, 'one reload, even when the change is seen twice');
  assert.equal(storage.items.get('multitap-admin-reloaded:1.0.3'), '1');
  await recheck(f); t.mock.timers.tick(5000); assert.equal(reloads, 1);
  // The reloaded page lives in the same tab: the version flapping back cannot reload it again.
  await t.test('same tab, version flaps', async sub => {
    server.version = '1.0.2';
    const g = await loadAdmin(sub, 'version-reload-flap', user, inventory, { respond: server.respond, storage });
    globalThis.location.reload = () => { reloads++; };
    server.version = '1.0.3'; await recheck(g); t.mock.timers.tick(5000);
    assert.equal(reloads, 1); assert.equal(document.querySelector('#notice').hidden, true);
  });
});

test('after the server was unreachable, the next poll rechecks its version and reloads for the new one', async t => {
  const { user, inventory } = ownerFixture(), server = versionServer('1.0.2');
  let reloads = 0, restarting = false;
  t.mock.timers.enable({ apis: ['setTimeout', 'setInterval', 'Date'], now: Date.parse('2026-11-03T01:00:00.000Z') });
  const f = await loadAdmin(t, 'version-reload-reconnect', user, inventory, { respond: async url => { if (restarting && url.endsWith('/sessions')) throw new TypeError('fetch failed'); return server.respond(url); } });
  globalThis.location.reload = () => { reloads++; };
  // The update restarts the server: a polled request fails, then it answers with the new version.
  restarting = true; server.version = '1.0.3'; t.mock.timers.tick(10000); await f.settle();
  assert.equal(document.querySelector('#notice').hidden, true, 'the 10-minute version check is not due yet');
  restarting = false; t.mock.timers.tick(5000); await f.settle();
  assert.equal(document.querySelector('#notice .notice-text').textContent, '새 버전으로 다시 불러와요.');
  t.mock.timers.tick(1500); assert.equal(reloads, 1);
});

test('unsaved work is asked about instead of reloading, and reloads once the work is done', async t => {
  const { user, inventory } = ownerFixture(), server = versionServer('1.0.2');
  let reloads = 0;
  const f = await loadAdmin(t, 'version-reload-unsaved', user, inventory, { respond: server.respond });
  globalThis.location.reload = () => { reloads++; };
  t.mock.timers.enable({ apis: ['setTimeout'] });
  f.navigate('users'); await f.settle(); f.click(f.content, '사용자 만들기'); await f.settle(); assert.equal(f.dialog.open, true);
  assert.ok(f.dialog.querySelector('#modal-fields input[name=username]'), 'the open dialog has fields, which is the unsaved work');
  server.version = '1.0.3'; await recheck(f);
  const notice = document.querySelector('#notice'), asked = '서버가 새 버전으로 바뀌었어요.\n저장하지 않은 변경이 있어요.\n변경을 마치면 다시 불러와요.';
  assert.equal(notice.querySelector('.notice-text').textContent, asked); assert.ok(f.find(notice, 'button', '지금 다시 불러오기'));
  t.mock.timers.tick(10000); await recheck(f); t.mock.timers.tick(10000);
  assert.equal(reloads, 0, 'no reload over a dialog with fields'); assert.equal(notice.hidden, false, 'the question stays until it is answered or closed');
  // Closing the dialog finishes the work: the next check reloads after the notice.
  document.querySelector('#modal-cancel').onclick(); await recheck(f);
  assert.equal(notice.querySelector('.notice-text').textContent, '새 버전으로 다시 불러와요.');
  t.mock.timers.tick(1500); assert.equal(reloads, 1);
});

test('without sessionStorage a new server version only asks, and its button reloads', async t => {
  const { user, inventory } = ownerFixture(), server = versionServer('1.0.2');
  let reloads = 0;
  const f = await loadAdmin(t, 'version-reload-no-storage', user, inventory, { respond: server.respond, storage: null });
  globalThis.location.reload = () => { reloads++; };
  t.mock.timers.enable({ apis: ['setTimeout'] });
  server.version = '1.0.3'; await recheck(f); t.mock.timers.tick(5000);
  const notice = document.querySelector('#notice');
  assert.equal(reloads, 0); assert.equal(notice.querySelector('.notice-text').textContent, '서버가 새 버전으로 바뀌었어요.\n다시 불러오세요.');
  f.click(notice, '지금 다시 불러오기'); assert.equal(reloads, 1);
});

test('The list puts problems first with a state condition, the OS comes from Proxmox configuration, and Host approval confirms the matching VM and the code, without assignment', async t => {
  const user = { id: 'owner', username: '관리자', role: 'admin', mustChangeCredentials: false };
  const desktops = [
    { id: 'pve-170', pve_vmid: 170, name: 'Windows 업무', pve_os_family: 'windows', pve_os_type: 'win11', pve_os_source: 'proxmox-config', host: { connected: true }, pve_available: 1, pve_power_status: 'running' },
    { id: 'pve-121', pve_vmid: 121, name: 'Linux 서버', pve_os_family: 'linux', pve_os_type: 'l26', pve_os_source: 'proxmox-config', pve_available: 1, pve_power_status: 'running' },
    { id: 'pve-1000', pve_vmid: 1000, name: '새 VM', os: 'Windows 표시 정보', pve_os_family: 'unknown', pve_os_source: 'unavailable', pve_available: 1 },
  ];
  const inventory = { users: [{ ...user, disabled: 0, notes: '' }], desktops, assignments: [{ username: user.username, desktop_id: 'pve-1000' }], audit: [], proxmox: { configured: true } };
  registerFixtureDesktops(inventory);
  let csrf;
  const requestsFixture = [{ requestId: 'fixture-request', hostname: 'TESTER', version: '0.2.12', machineUuid: 'fixture-machine', status: 'pending', expiresAt: '2099-01-01T00:00:00Z', verificationCode: 'TEST-CODE', codeDisplay: 'shown', candidates: [{ desktopId: 'pve-170', vmid: 170, name: 'Windows 업무', hasExistingHost: true }] }];
  const f = await loadAdmin(t, 'os-enrollment-flow', user, inventory, { respond: async (url, options) => {
    if (url.endsWith('/hosts/requests')) { csrf = options.headers['X-CSRF-Token']; return { ok: true, status: 200, json: async () => ({ requests: requestsFixture, pendingCount: 1 }) }; }
  } });
  const { writes, click, follow, navigate, dialog, settle, content, change } = f;
  navigate('desktops'); await settle();
  const items = () => [...content.querySelectorAll('.vm-list-item')];
  const state = () => content.querySelector('select[data-focus=desktops-state]'), choice = key => [...state().querySelectorAll('option')].find(node => node.getAttribute('value') === key);
  // Problems first: the two VMs whose Host does not answer (by VM number), then the reachable one.
  assert.deepEqual(items().map(row => row.getAttribute('data-focus')), ['vm-pick-pve-121', 'vm-pick-pve-1000', 'vm-pick-pve-170']);
  assert.deepEqual(items().map(row => row.getAttribute('data-status')), ['bad', 'bad', 'ok']);
  // A row of the board starts with its number and its state; the name and the VM number follow.
  assert.match(content.querySelector('.vm-list-item[data-focus="vm-pick-pve-170"]').closest('tr').textContent, /^3접속 가능Windows 업무.*VM 170/);
  assert.deepEqual([...content.querySelectorAll('.vm-table th')].map(node => node.textContent), ['', '번호', '상태', '이름', '종류', '노드', '사용자', 'Host', 'Host 응답', '관리']);
  assert.equal(content.querySelector('.list-count>span').textContent, '총 3대 · 접속 불가 2 · 접속 중 0 · 접속 가능 1 · 꺼짐 0'); assert.equal(content.querySelector('.list-count>small').textContent, '정렬: 문제 먼저');
  // One state condition replaces the old OS, power and Host selects; update choices appear only when there is something to update.
  assert.deepEqual(['all', 'bad', 'live', 'ok', 'off'].map(key => choice(key).textContent), ['전체 3', '접속 불가 2', '접속 중 0', '접속 가능 1', '꺼짐 0']);
  assert.equal(choice('update'), undefined); assert.equal(choice('manual'), undefined);
  assert.equal(content.querySelector('.vm-list-filters'), null); assert.equal(content.querySelector('[aria-label="VM OS 필터"]'), null);
  f.change(state(), 'ok'); f.click(content.querySelector('.query-box'), '조회');
  assert.equal(choice('ok').hasAttribute('selected'), true); assert.equal(choice('all').hasAttribute('selected'), false);
  assert.deepEqual(items().map(row => row.getAttribute('data-focus')), ['vm-pick-pve-170']);
  assert.equal(content.querySelector('.vm-list-caption').textContent, '조회 1 / 3대');
  f.click(content.querySelector('.query-box'), '초기화');
  assert.equal(choice('all').hasAttribute('selected'), true); assert.equal(items().length, 3);
  // The OS is what Proxmox configuration says, marked as such; a display text alone never becomes an OS.
  f.pickVm('pve-170'); await settle();
  assert.equal(content.querySelector('.dt-head small').textContent, 'VM 170 · 노드 미확인 · Windows');
  const osNote = [...content.querySelectorAll('.dt-main [title]')].find(node => node.title.includes('Proxmox 운영체제 설정'));
  assert.equal(osNote.textContent, 'Windows'); assert.match(osNote.title, /win11/); assert.match(osNote.title, /실제 설치 버전과 다를 수 있어요/);
  // The pending request waits on the same page; approval confirms the VM first.
  assert.equal(csrf, 'fixture-csrf');
  assert.equal(content.querySelector('.requests-panel h2').textContent, '승인 대기1');
  const card = content.querySelector('.requests-panel .request-card');
  assert.match(card.textContent, /TESTER/); assert.match(card.textContent, /VM 170 · Windows 업무/);
  assert.equal(content.textContent.includes('일회용 연결 코드'), false);
  click(card, '확인하고 승인');
  assert.equal(dialog.open, true); assert.match(dialog.textContent, /기존 Host 교체/);
  assert.equal(f.focused(), dialog.querySelector('[name=acknowledgeCode]'), 'the dialog starts on its first field, the code tick');
  assert.equal(writes.some(write => write.path.endsWith('/approve')), false);
  click(dialog, '취소');
  assert.equal(writes.some(write => write.path.endsWith('/approve')), false, 'closing the confirmation approves nothing');
  // A registered desktop needs no installation action of its own. The guide
  // is the page's 데스크톱 추가, whichever desktop is selected.
  f.pickVm('pve-1000'); await settle();
  assert.equal(f.crumb(), 'VM 1000 · 새 VM');
  assert.equal(content.querySelector('.dt-head small').textContent, 'VM 1000 · 노드 미확인', 'the display text "Windows 표시 정보" is not an operating system');
  const detailButtons = () => [...content.querySelectorAll('.dt-main button')].map(node => node.textContent);
  assert.equal(detailButtons().some(text => ['Host 설치 안내', '데스크톱 추가'].includes(text)), false);
  f.pickVm('pve-121'); await settle();
  assert.equal(content.querySelector('.dt-head small').textContent, 'VM 121 · 노드 미확인 · Linux');
  assert.equal(detailButtons().some(text => ['Host 설치 안내', '데스크톱 추가'].includes(text)), false);
  click(content, '데스크톱 추가'); await settle();
  assert.equal(dialog.open, true); assert.equal(document.querySelector('#modal-title').textContent, '데스크톱 추가'); assert.match(dialog.textContent, /연결 요청/);
  assert.equal(writes.some(write => write.path.endsWith('/enrollment')), false);
  dialog.close(); await settle();
  // Assigning offers the registered desktops the account does not have yet, and opening it writes nothing.
  navigate('users'); follow(content, user.username); await settle();
  assert.deepEqual([...content.querySelectorAll('#assignments li.assigned-row')].map(row => row.getAttribute('data-desktop')), ['pve-1000']);
  click(content, '데스크톱 배정');
  assert.equal(dialog.open, true); assert.equal(document.querySelector('#modal-title').textContent, '데스크톱 배정');
  assert.deepEqual([...dialog.querySelectorAll('input[data-vm-id]')].map(node => node.getAttribute('data-vm-id')), ['pve-121', 'pve-170']);
  document.querySelector('#modal-cancel').onclick(); await settle();
  assert.equal(writes.some(write => write.path.includes('/assignments')), false);
  assert.deepEqual(writes.filter(write => !write.path.endsWith('/proxmox/sync')), [], 'only the read-only Proxmox synchronisation was requested');
});

test('VM pages synchronize automatically, report failed refresh honestly, and renew only the connected token', async t => {
  const user = { id: 'fixture-owner', username: '관리자', role: 'admin', mustChangeCredentials: false };
  const tokenId = 'multitap-fixture@pve!broker';
  const inventory = {
    users: [{ id: user.id, username: user.username, role: 'admin', disabled: 0, is_primary_admin: 1, notes: '' }],
    desktops: [], assignments: [], audit: [],
    proxmox: { configured: true, endpoint: 'https://pve.fixture.example:8006', tokenId, hasSecret: true, ca: '', state: 'unchecked', expiresAt: '2099-01-01T00:00:00.000Z', expirySource: 'configured', setupScriptSha256: 'ab'.repeat(32) },
  };
  let releaseSync, failSync = false;
  const registeredHosts = [];
  const syncWait = new Promise(resolve => { releaseSync = resolve; });
  const { writes, requests, dialog, click, navigate, pickVm, settle, content, find } = await loadAdmin(t, 'automatic-vm-flow', user, inventory, { origin: 'https://second.fixture.example', monitor: { hosts: registeredHosts }, respond: async url => {
    if (!url.endsWith('/proxmox/sync')) return;
    if (failSync) return { ok: false, status: 502, json: async () => ({ error: { code: 'proxmox_unreachable' } }) };
    await syncWait;
  } });
  navigate('desktops');
  assert.match(content.textContent, /등록된 VM 상태를 확인하고 있어요/);
  assert.equal(content.querySelector('tbody'), null); assert.equal(content.querySelectorAll('.vm-list-item').length, 0);
  navigate('proxmox');
  assert.equal([...content.querySelectorAll('button')].find(node => node.textContent === '토큰 갱신').disabled, true);
  assert.equal(content.querySelector('.pve-change').hasAttribute('inert'), true);
  navigate('desktops');
  inventory.desktops = [
    { id: 'pve-101', pve_vmid: 101, name: '자동 VM 하나', os: '미확인', enabled: 1, pve_available: 1, pve_node: 'pve-one', pve_power_status: 'running' },
    { id: 'pve-102', pve_vmid: 102, name: '자동 VM 둘', os: '미확인', enabled: 1, pve_available: 1, pve_node: 'pve-two', pve_power_status: 'stopped' },
  ];
  registerFixtureDesktops(inventory);
  registeredHosts.push(...inventory.fixtureHosts);
  releaseSync(); await settle();
  assert.equal(content.querySelectorAll('.vm-list-item').length, 2);
  assert.match(content.textContent, /자동 VM 하나/);
  assert.match(content.textContent, /자동 VM 둘/);
  assert.equal(content.textContent.includes('VM 가져오기'), false);
  // Hosts are managed on this page. Deleting: the list deletes only through 선택 삭제 and its
  // confirmation, which stays off until a row is chosen; the window of a desktop has 삭제 in its bar.
  const selected = content.querySelector('button[data-focus=desktops-delete-selected]');
  assert.equal(selected.textContent, '선택 삭제'); assert.equal(selected.disabled, true, 'nothing chosen, nothing to delete');
  assert.equal([...content.querySelectorAll('button')].filter(node => node.textContent === '삭제').length, 0, 'no row deletes by itself');
  pickVm('pve-101'); await settle();
  const removal = [...content.querySelectorAll('.win-foot button.dt-delete')];
  assert.deepEqual(removal.map(node => node.textContent), ['삭제']);
  assert.equal(content.querySelector('details.dt-more'), null);
  assert.equal(writes.some(write => write.path.endsWith('/host')), false);
  assert.ok(requests.some(path => path.endsWith('/proxmox/sync')));

  navigate('proxmox'); await settle();
  assert.match(content.querySelector('.pve-connection-card').textContent, /일 남음/);
  assert.match(content.querySelector('.pve-connection-card').textContent, /토큰 만료일은 연결할 때 Proxmox Shell에서 받은 값이에요\./);
  assert.equal(content.querySelector('.pve-change').hasAttribute('open'), false);
  inventory.proxmox.expiresAt = null; inventory.proxmox.expirySource = 'unknown';
  navigate('proxmox'); await settle();
  assert.match(content.querySelector('.pve-connection-card').textContent, /만료일 정보 없음/);
  Object.assign(inventory.proxmox, { expirySource: 'proxmox', accountExpiryKnown: true, tokenExpiresAt: null, accountExpiresAt: null });
  navigate('proxmox'); await settle();
  assert.match(content.querySelector('.pve-connection-card').textContent, /만료일 없음/);
  assert.equal(content.querySelector('.pve-connection-card .inline-alert.is-warn'), null);
  inventory.proxmox.agentPrivilegeMissing = true;
  navigate('proxmox'); await settle();
  const agentWarning = content.querySelector('.pve-connection-card .inline-alert.is-warn');
  assert.match(agentWarning.textContent, /게스트 에이전트 권한이 없어 '연결 준비 중' 단계를 건너뛰어요/);
  assert.match(agentWarning.querySelector('code').textContent, /--renew 'multitap-fixture@pve!broker'/);
  inventory.proxmox.agentPrivilegeMissing = false;
  inventory.proxmox.expirySource = 'unknown';
  click(content, '토큰 갱신');
  const form = document.querySelector('#modal-form');
  assert.match(form.querySelector('code').textContent, /second\.fixture\.example\/setup\/proxmox\.py/);
  assert.match(form.querySelector('code').textContent, /--renew 'multitap-fixture@pve!broker'/);
  const block = value => 'MULTITAP-RENEWAL-V1:\n' + Buffer.from(JSON.stringify(value)).toString('base64') + '\nEND-MULTITAP-RENEWAL';
  const before = writes.length;
  form.elements.renewal.value = block({ tokenId: 'wrong@pve!broker', expiresAt: '2099-01-01T00:00:00Z' });
  await form.onsubmit({ preventDefault() {} });
  assert.equal(writes.length, before); assert.equal(dialog.open, true);
  assert.match(form.querySelector('#modal-error').textContent, /현재 연결 계정/);
  form.elements.renewal.value = block({ tokenId, expiresAt: '2099-01-01T00:00:00Z' });
  await form.onsubmit({ preventDefault() {} });
  const saved = writes.slice(before).find(write => write.path === '/api/v1/admin/proxmox');
  assert.deepEqual(saved.data, { endpoint: inventory.proxmox.endpoint, tokenId, ca: '', expiresAt: '2099-01-01T00:00:00.000Z' });
  assert.equal('secret' in saved.data, false);
  assert.equal(dialog.open, false);
  // 갱신 확인 with nothing pasted and the same expiry as before confirms nothing: the dialog stays and says so.
  navigate('proxmox'); await settle();
  click(content, '토큰 갱신');
  const unchanged = writes.length;
  await document.querySelector('#modal-form').onsubmit({ preventDefault() {} });
  assert.equal(dialog.open, true);
  assert.equal(document.querySelector('#modal-error').textContent, '만료일이 그대로예요.\nProxmox Shell에서 위 명령을 실행한 뒤 다시 확인하세요.');
  assert.equal(writes.slice(unchanged).some(write => write.path === '/api/v1/admin/proxmox'), false);
  document.querySelector('#modal-cancel').onclick();

  inventory.desktops = inventory.desktops.slice(0, 1);
  navigate('desktops'); await settle();
  assert.equal(content.querySelectorAll('.vm-list-item').length, 1);
  failSync = true;
  click(content, '새로고침'); await settle();
  const failure = [...content.querySelectorAll('.empty.is-error')].find(node => node.textContent.includes('VM 목록을 확인하지 못했어요'));
  assert.equal(failure.getAttribute('role'), 'alert', 'the failed check is announced above the list');
  assert.ok(find(failure, 'button', '다시 시도'));
  assert.equal(content.querySelectorAll('.vm-table tbody tr').length, 1);
  assert.equal(content.querySelectorAll('.vm-list-item').length, 1, 'known registered row survives temporary Proxmox failure');
  assert.equal(writes.some(write => write.path.endsWith('/power')), false, 'a failed check requests no power action');
});

// Replaces the draft editor test (per-account drafts, 표시된 VM 선택, 변경 적용):
// an assignment is now saved at once, so the same properties are checked on
// the 데스크톱 배정 dialog.
test('Assigning desktops saves at once in one batch for one account, keeps no hidden draft, starts new ones denied and protects Korean IME input', async t => {
  const user = { id: 'fixture-owner', username: '관리자', role: 'admin', mustChangeCredentials: false };
  const users = [{ ...user, disabled: 0, notes: '' }, { id: 'fixture-member', username: '홍길동', role: 'user', notes: '개발팀', disabled: 0, unavailableAssignmentCount: 2 }];
  const desktops = [1000, 101, 999, 100].map(vmid => ({ id: 'pve-' + vmid, pve_vmid: vmid, name: '업무 ' + vmid, os: '미확인', pve_node: 'pve', pve_available: 1, pve_power_status: vmid === 101 ? 'running' : 'stopped' }));
  const inventory = { users, desktops, assignments: [{ username: '홍길동', desktop_id: 'pve-101' }], audit: [], proxmox: { configured: false } };
  registerFixtureDesktops(inventory);
  const { window, writes, click, follow, find, change, navigate, settle, content, dialog, focused } = await loadAdmin(t, 'batch-assignment-flow', user, inventory);
  const open = async name => { navigate('users'); follow(content, name); await settle(); };
  const rows = () => [...content.querySelectorAll('#assignments li.assigned-row')].map(row => row.getAttribute('data-desktop'));
  const boxes = () => [...dialog.querySelectorAll('input[data-vm-id]')];
  const batches = () => writes.filter(write => write.path.endsWith('/assignments/batch'));
  const submit = () => document.querySelector('#modal-form').onsubmit({ preventDefault() {} });
  await open('홍길동');
  assert.deepEqual(rows(), ['pve-101']);
  assert.match(content.querySelector('.assignment-retained').textContent, /지금 목록에 없는 데스크톱 2대의 배정과 공유는 그대로 있어요/);
  for (const name of ['변경 적용', '되돌리기', '표시된 VM 선택']) assert.equal(find(content, 'button', name), undefined, name + ' belonged to the draft editor');
  assert.equal(content.querySelector('.assignment-review'), null);

  // The dialog offers what the account does not have yet, by VM number.
  click(content, '데스크톱 배정');
  assert.equal(dialog.open, true); assert.equal(document.querySelector('#modal-save').textContent, '배정');
  assert.deepEqual(boxes().map(node => node.getAttribute('data-vm-id')), ['pve-100', 'pve-999', 'pve-1000']);
  assert.match(dialog.textContent, /새 배정은 ‘기본’\(아무것도 주고받지 않음\)으로 시작해요/);
  // Choosing and closing sends nothing and leaves nothing behind (no draft).
  change(boxes()[0], true);
  document.querySelector('#modal-cancel').onclick(); await settle();
  assert.deepEqual(batches(), []);
  navigate('users');
  assert.doesNotMatch(content.querySelector('.account-table tbody').textContent, /초안/);
  await open('관리자'); await open('홍길동');
  assert.deepEqual(rows(), ['pve-101']);
  click(content, '데스크톱 배정');
  assert.equal(boxes().some(node => node.checked === true), false, 'the closed dialog kept no choice');
  // Saving without a choice asks for one and requests nothing.
  await submit();
  assert.equal(document.querySelector('#modal-error').textContent, '배정할 데스크톱을 고르세요.'); assert.equal(dialog.open, true);
  assert.deepEqual(batches(), []);
  document.querySelector('#modal-cancel').onclick(); await settle();

  // Korean IME: typing never re-renders, the Enter that ends a composition does not search, and the next Enter filters once.
  navigate('users');
  const userSearch = content.querySelector('[data-focus=users-search]');
  const enter = (node, isComposing) => { const event = new window.Event('keydown', { cancelable: true }); Object.defineProperty(event, 'key', { value: 'Enter' }); Object.defineProperty(event, 'isComposing', { value: isComposing }); node.dispatchEvent(event); return event; };
  userSearch.value = '홍';
  const composing = new window.Event('input'); Object.defineProperty(composing, 'isComposing', { value: true });
  userSearch.dispatchEvent(composing);
  assert.equal(content.querySelector('[data-focus=users-search]'), userSearch);
  assert.equal(content.querySelectorAll('.account-choice').length, 2);
  assert.equal(enter(userSearch, true).defaultPrevented, false);
  userSearch.dispatchEvent(new window.Event('compositionend')); userSearch.dispatchEvent(new window.Event('input'));
  assert.equal(content.querySelector('[data-focus=users-search]'), userSearch, 'nothing is searched until 조회 or Enter');
  assert.equal(content.querySelectorAll('.account-choice').length, 2);
  assert.equal(enter(userSearch, false).defaultPrevented, true);
  // The list is searched; the 조회 조건 box itself is not made again (a refresh must not disturb it either).
  assert.equal(content.querySelector('[data-focus=users-search]'), userSearch);
  assert.equal(content.querySelectorAll('.account-choice').length, 1);
  assert.ok(focused() === content.querySelector('[data-focus=users-search]'), 'Enter leaves the focus in the search field');
  // A re-rendered search field keeps the caret.
  const typed = content.querySelector('[data-focus=users-search]');
  typed.value = '홍길'; typed.selectionStart = 1; typed.selectionEnd = 2;
  const inputPrototype = Object.getPrototypeOf(typed), oldRange = inputPrototype.setSelectionRange;
  let restoredRange;
  inputPrototype.setSelectionRange = (start, end) => { restoredRange = [start, end]; };
  t.after(() => { inputPrototype.setSelectionRange = oldRange; });
  typed.dispatchEvent(new window.Event('input')); enter(typed, false);
  assert.deepEqual(restoredRange, [1, 2], 'the re-rendered search keeps the caret');
  assert.equal(content.querySelectorAll('.account-choice').length, 1);

  // One request names this account, its identity and every desktop it keeps.
  await open('홍길동');
  click(content, '데스크톱 배정');
  change(boxes()[0], true); change(boxes()[2], true);
  assert.deepEqual(batches(), [], 'ticking a box is not a request');
  await submit(); await settle();
  assert.deepEqual(batches(), [{ path: '/api/v1/admin/assignments/batch', data: { username: '홍길동', expectedAccountId: users[1].id, desktopIds: ['pve-101', 'pve-100', 'pve-1000'] } }]);
  assert.equal(writes.some(write => write.path.endsWith('/assignments')), false);
  assert.equal(dialog.open, false);
  assert.equal(document.querySelector('#notice .notice-text').textContent, '홍길동 님에게 데스크톱 2대 배정을 저장했어요.');
  assert.deepEqual(rows(), ['pve-100', 'pve-101', 'pve-1000']);
  // A new assignment starts with nothing shared.
  for (const id of ['pve-100', 'pve-1000']) {
    assert.equal(content.querySelector('button[data-preset=default][data-vm="' + id + '"]').getAttribute('aria-pressed'), 'true');
    assert.match(content.querySelector('li.assigned-row[data-desktop="' + id + '"] .row-text small').textContent, /아무것도 주고받지 않아요/);
  }
  assert.deepEqual(inventory.assignments.filter(row => row.username !== '홍길동'), []);
  assert.equal(batches().length, 1, 'nothing is sent again after the save');
});
