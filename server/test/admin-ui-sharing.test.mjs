import { test } from 'node:test';
import assert from 'node:assert/strict';
import { messages } from '../admin/js/labels.js';
import { loadAdmin, iso, workspaceFixture, clipDenyFixture, deviceKeysFixture, deviceDenyFixture, permissionFixture, recheck } from './admin-ui-harness.mjs';

// Sharing has no drafts: a named choice, one switch, "모두에 한 번에", 데스크톱 배정
// and 빼기 are each saved at once as one request. Helpers for the 사용자 page.
const fire = (f, node, type = 'click') => node.dispatchEvent(new f.window.Event(type));
const batches = f => f.writes.filter(write => write.path.endsWith('/assignments/batch'));
const openUser = async (f, name) => { f.navigate('users'); f.follow(f.content, name); await f.settle(); };
const row = (f, id) => f.content.querySelector(`li.assigned-row[data-desktop="${id}"]`);
const details = (f, id) => f.content.querySelector(`[data-permission-vm="${id}"]`);
const preset = (f, id, key) => f.content.querySelector(`button[data-preset="${key}"][data-vm="${id}"]`);
const toggle = (f, id, key) => f.content.querySelector(`input[data-vm="${id}"][data-clipboard-permission="${key}"],input[data-vm="${id}"][data-device-permission="${key}"]`);
const same = (f, key) => f.content.querySelector(`.same-for-all button[data-same-preset="${key}"]`);
const on = node => node.hasAttribute('checked');
const sentence = (f, id) => row(f, id).querySelector('.row-text small').textContent;
const noticeNode = () => document.querySelector('#notice');
const submit = async f => { await document.querySelector('#modal-form').onsubmit({ preventDefault() {} }); await f.settle(); };
const dialogTitle = () => document.querySelector('#modal-title').textContent;
const dialogSave = () => document.querySelector('#modal-save').textContent;
const allClipboard = value => Object.fromEntries(Object.keys(clipDenyFixture).map(key => [key, value]));
// Every device set to `value`.
const allDevices = value => Object.fromEntries(deviceKeysFixture.map(key => [key, value]));
const session = (account, desktopId) => ({ sessionId: `session-${account}-${desktopId}`, account, desktopId, attached: true, ready: true });
const failed = (status, code) => ({ ok: false, status, json: async () => ({ error: { code } }) });

test('sharing has no sound-card switch: any assigned account may add one, and the retired flag is never sent', async t => {
  const { user, member, inventory } = permissionFixture();
  for (const desktop of inventory.desktops) desktop.kind = 'vm';
  inventory.assignments[0].soundCardAdd = true;
  const f = await loadAdmin(t, 'assignment-no-sound-card-switch', user, inventory);
  await openUser(f, member.username);
  fire(f, details(f, 'pve-101'));
  assert.equal(f.content.querySelector('[data-vm-action]'), null);
  assert.equal(/사운드카드/.test(f.content.querySelector('#assignments').textContent), false, 'no switch and no summary line');
  assert.equal(row(f, 'pve-101').querySelectorAll('.sharing-fine [data-device-permission]').length, 5);
  f.change(toggle(f, 'pve-101', 'filesToLocal'), true); await f.settle();
  assert.equal(batches(f).length, 1, 'one switch is one request, sent at once');
  const sent = batches(f)[0].data.assignments.find(entry => entry.desktopId === 'pve-101');
  assert.deepEqual(sent.clipboardPolicy, { ...clipDenyFixture, textToRemote: true, filesToLocal: true });
  assert.equal(Object.hasOwn(sent, 'soundCardAdd'), false);
  assert.deepEqual(Object.keys(sent.devicePolicy), deviceKeysFixture, 'every key is sent');
  assert.equal(inventory.assignments.find(entry => entry.desktop_id === 'pve-101').soundCardAdd, true, 'the stored value is left alone');
});

test('"모두에 한 번에" over mixed VM and local PC assignments leaves every stored sound-card value alone', async t => {
  const { user, member, inventory } = permissionFixture();
  for (const desktop of inventory.desktops) desktop.kind = 'vm';
  inventory.assignments[0].soundCardAdd = true;
  inventory.desktops.push({ id: 'pc-audio-bulk', name: '로컬 시험 PC', kind: 'pc', localPcApproved: true });
  inventory.fixtureHosts.push({ desktopId: 'pc-audio-bulk', hostId: '00000000-0000-4000-8000-000000000004' });
  inventory.assignments.push({ username: member.username, desktop_id: 'pc-audio-bulk', clipboardPolicy: clipDenyFixture, devicePolicy: deviceDenyFixture });
  const f = await loadAdmin(t, 'assignment-mixed-vm-sound-card', user, inventory);
  await openUser(f, member.username);
  assert.equal(f.content.querySelector('.same-for-all').textContent.startsWith('모두에 한 번에'), true);
  assert.deepEqual([...f.content.querySelectorAll('.same-for-all button')].map(node => node.textContent), ['기본', '글·그림 복사', '모두 허용']);
  assert.equal(f.content.querySelector('[data-vm-action]'), null, 'mixed targets have no VM-only action');
  fire(f, same(f, 'all')); await f.settle();
  assert.notEqual(f.dialog.open, true, 'turning sharing on asks nothing');
  assert.equal(batches(f).length, 1, 'one atomic request for every desktop of this account');
  assert.deepEqual(batches(f)[0].data, { username: member.username, expectedAccountId: member.id,
    assignments: ['pve-101', 'pve-102', 'pc-audio-bulk'].map(desktopId => ({ desktopId, clipboardPolicy: allClipboard(true), devicePolicy: allDevices(true) })) });
  assert.equal(batches(f)[0].data.assignments.some(entry => Object.hasOwn(entry, 'soundCardAdd')), false, 'a sharing change omits every unchanged action permission');
  assert.equal(inventory.assignments.find(entry => entry.desktop_id === 'pve-101').soundCardAdd, true);
  assert.equal(inventory.assignments.find(entry => entry.desktop_id === 'pve-102').soundCardAdd, false);
  assert.equal(noticeNode().textContent, `저장했어요.\n${member.username}: 데스크톱 3대 · 모두 허용`);
});

test('one click allows every item for a desktop or returns it to 기본', async t => {
  const { user, member, inventory } = permissionFixture();
  for (const desktop of inventory.desktops) desktop.kind = 'vm';
  const f = await loadAdmin(t, 'permission-presets', user, inventory);
  await openUser(f, member.username);
  fire(f, details(f, 'pve-102'));
  const switches = () => [...row(f, 'pve-102').querySelectorAll('.sharing-fine input[role=switch]')];
  assert.deepEqual(['default', 'clipboard', 'all'].map(key => preset(f, 'pve-102', key).textContent), ['기본', '글·그림 복사', '모두 허용']);
  assert.deepEqual(['default', 'clipboard', 'all'].map(key => preset(f, 'pve-102', key).getAttribute('aria-pressed')), ['true', 'false', 'false']);
  assert.equal(row(f, 'pve-102').querySelector('[data-preset="custom"]'), null);
  fire(f, preset(f, 'pve-102', 'default')); await f.settle();
  assert.deepEqual(f.writes, [], 'the choice already in effect sends nothing');
  assert.equal(row(f, 'pve-102').querySelector('.sharing-help').hasAttribute('open'), false, 'the long guidance stays behind 자세히');
  fire(f, preset(f, 'pve-102', 'all')); await f.settle();
  const all = value => ({ desktopId: 'pve-102', clipboardPolicy: allClipboard(value), devicePolicy: allDevices(value) });
  assert.equal(batches(f).length, 1, 'a named choice is saved at once, as one request');
  assert.deepEqual(batches(f)[0].data, { username: member.username, expectedAccountId: member.id, assignments: [{ desktopId: 'pve-101' }, all(true)] });
  assert.equal(switches().length, 9); assert.ok(switches().every(on));
  assert.equal(preset(f, 'pve-102', 'all').getAttribute('aria-pressed'), 'true');
  assert.equal(sentence(f, 'pve-102'), 'VM 102 · 글·그림 복사 양쪽, 파일 양쪽, 공유 폴더, 프린터, USB 장치, 스마트 카드, 직렬(COM) 포트');
  assert.equal(noticeNode().textContent, `저장했어요.\n${member.username} · 업무 102: 모두 허용`); assert.equal(noticeNode().className, 'is-success');
  assert.equal(row(f, 'pve-102').querySelector('.saved-mark').textContent, '저장했어요');
  assert.equal(row(f, 'pve-101').querySelector('.saved-mark'), null, 'only the changed row is marked');
  // Nobody is connected, so returning to 기본 asks nothing either.
  fire(f, preset(f, 'pve-102', 'default')); await f.settle();
  assert.notEqual(f.dialog.open, true);
  assert.equal(batches(f).length, 2);
  assert.deepEqual(batches(f)[1].data.assignments, [{ desktopId: 'pve-101' }, all(false)]);
  assert.equal(switches().some(on), false);
  assert.equal(sentence(f, 'pve-102'), 'VM 102 · 아무것도 주고받지 않아요');
});

test('the desktop page gives several users one value in one atomic request', async t => {
  const { user, member, inventory } = permissionFixture();
  for (const desktop of inventory.desktops) desktop.kind = 'vm';
  const second = { id: 'fixture-second', username: '둘째', notes: '', role: 'user', disabled: 0 }, stopped = { id: 'fixture-stopped', username: '중지됨', notes: '', role: 'user', disabled: 1 };
  inventory.users.push(second, stopped); inventory.proxmox = { configured: true, state: 'ok' };
  inventory.assignments.push({ username: second.username, desktop_id: 'pve-101', clipboardPolicy: clipDenyFixture, devicePolicy: deviceDenyFixture }, { username: stopped.username, desktop_id: 'pve-101', clipboardPolicy: clipDenyFixture, devicePolicy: deviceDenyFixture });
  const monitor = { sessions: [] };
  const f = await loadAdmin(t, 'desktop-permissions', user, inventory, { hash: '#/desktops/pve-101', monitor, respond: (url, options) => {
    if (!url.endsWith('/desktops/pve-101/permissions') || options.method !== 'PUT') return null;
    for (const entry of JSON.parse(options.body).permissions) Object.assign(inventory.assignments.find(a => a.username === entry.username && a.desktop_id === 'pve-101'), entry);
    return { ok: true, status: 200, json: async () => ({ ok: true, count: 2, policiesChanged: 2 }) };
  } });
  const panel = () => f.content.querySelector('#desktop-people');
  const person = name => panel().querySelector(`li.person-row[data-person="${name}"]`);
  const sent = () => f.writes.filter(write => write.path.endsWith('/desktops/pve-101/permissions'));
  assert.deepEqual([...panel().querySelectorAll('li.person-row')].map(node => node.getAttribute('data-person')), [second.username, member.username, stopped.username]);
  const stoppedButton = f.find(person(stopped.username), 'button', '공유 바꾸기');
  assert.equal(stoppedButton.disabled, true, 'a stopped account is not changed'); assert.equal(stoppedButton.title, '사용 중지된 계정은 바꿀 수 없어요.');
  assert.equal(person(member.username).querySelector('.row-text small').textContent, '공유: 직접 설정 · 글·그림 복사는 데스크톱으로만');
  assert.equal(panel().querySelector('.same-for-all').textContent.startsWith('모두에게 한 번에'), true);
  // What 기본 means is said where the sharing of a desktop is changed (there is no 기본 공유 page).
  assert.match(panel().querySelector('p.note').textContent, /‘기본’은 아무것도 주고받지 않아요./);
  fire(f, panel().querySelector('.same-for-all [data-same-preset="all"]')); await f.settle();
  assert.notEqual(f.dialog.open, true, 'turning sharing on asks nothing');
  assert.equal(sent().length, 1);
  assert.deepEqual(sent()[0].data, { permissions: [
    { username: second.username, expectedAccountId: second.id, clipboardPolicy: allClipboard(true), devicePolicy: allDevices(true) },
    { username: member.username, expectedAccountId: member.id, clipboardPolicy: allClipboard(true), devicePolicy: allDevices(true) },
  ] }, 'one request for the active accounts only, the sound-card grant omitted');
  assert.equal(f.writes.some(write => write.path.endsWith('/assignments/batch')), false);
  assert.equal(noticeNode().textContent, '저장했어요.\n업무 101: 사용자 2명 · 모두 허용');
  assert.equal(person(member.username).querySelector('.row-text small').textContent, '공유: 모두 허용');
  // Turning it off again while one of them is connected asks once, naming who is connected.
  monitor.sessions = [session(member.username, 'pve-101')]; await recheck(f);
  fire(f, panel().querySelector('.same-for-all [data-same-preset="default"]')); await f.settle();
  assert.equal(f.dialog.open, true); assert.equal(dialogTitle(), '공유를 끌까요?'); assert.equal(dialogSave(), '끄고 연결 끊기');
  assert.equal(f.dialog.querySelector('p.confirm-who').textContent, `${member.username} 님이 지금 업무 101에 접속 중이에요`);
  assert.equal(sent().length, 1, 'nothing is sent before the answer');
  f.click(f.dialog, '취소'); await f.settle();
  assert.equal(f.dialog.open, false); assert.equal(sent().length, 1, 'cancel sends nothing');
  assert.equal(person(member.username).querySelector('.row-text small').textContent, '공유: 모두 허용');
  fire(f, panel().querySelector('.same-for-all [data-same-preset="default"]')); await f.settle();
  await submit(f);
  assert.equal(sent().length, 2, 'the confirmed change is one request');
  assert.deepEqual(sent()[1].data, { permissions: [
    { username: second.username, expectedAccountId: second.id, clipboardPolicy: allClipboard(false), devicePolicy: allDevices(false) },
    { username: member.username, expectedAccountId: member.id, clipboardPolicy: allClipboard(false), devicePolicy: allDevices(false) },
  ] });
  assert.equal(inventory.assignments.find(a => a.username === stopped.username).clipboardPolicy, clipDenyFixture, 'the stopped account was never written');
});

test("one user's sharing on the desktop page is saved at once under that account's ID", async t => {
  const { user, member, inventory } = permissionFixture();
  const second = { id: 'fixture-second', username: '둘째', notes: '', role: 'user', disabled: 0 };
  inventory.users.push(second);
  inventory.assignments.push({ username: second.username, desktop_id: 'pve-101', clipboardPolicy: clipDenyFixture, devicePolicy: deviceDenyFixture });
  const monitor = { sessions: [session(member.username, 'pve-101')] };
  const f = await loadAdmin(t, 'desktop-person-sharing', user, inventory, { hash: '#/desktops/pve-101', monitor, respond: (url, options) => {
    if (!url.endsWith('/desktops/pve-101/permissions') || options.method !== 'PUT') return null;
    for (const entry of JSON.parse(options.body).permissions) Object.assign(inventory.assignments.find(a => a.username === entry.username && a.desktop_id === 'pve-101'), entry);
    return { ok: true, status: 200, json: async () => ({ ok: true, count: 1, policiesChanged: 1 }) };
  } });
  const person = name => f.content.querySelector(`#desktop-people li.person-row[data-person="${name}"]`);
  const sent = () => f.writes.filter(write => write.path.endsWith('/desktops/pve-101/permissions'));
  assert.equal(f.content.querySelector('#desktop-people .sharing-control'), null, 'no editor until a row is opened');
  f.click(person(second.username), '공유 바꾸기');
  assert.equal(f.content.querySelectorAll('#desktop-people .sharing-control').length, 1);
  assert.equal(f.find(person(second.username), 'button', '닫기').getAttribute('aria-expanded'), 'true');
  const control = name => person(name).querySelector('.sharing-control');
  assert.deepEqual([...control(second.username).querySelectorAll('input[role=switch]')].map(node => node.getAttribute('data-account')), Array(9).fill(second.username));
  f.change(control(second.username).querySelector('[data-device-permission="printers"]'), true); await f.settle();
  assert.notEqual(f.dialog.open, true);
  assert.deepEqual(sent().map(write => write.data), [{ permissions: [{ username: second.username, expectedAccountId: second.id, clipboardPolicy: clipDenyFixture, devicePolicy: { ...deviceDenyFixture, printers: true } }] }]);
  assert.equal(noticeNode().textContent, `저장했어요.\n${second.username} · 업무 101: 직접 설정`);
  assert.equal(control(second.username).querySelector('.saved-mark').textContent, '저장했어요');
  // The connected account: turning off its clipboard asks first; the not connected one above did not.
  f.click(person(member.username), '공유 바꾸기');
  assert.equal(f.content.querySelectorAll('#desktop-people .sharing-control').length, 1, 'one editor at a time');
  f.change(control(member.username).querySelector('[data-clipboard-permission="textToRemote"]'), false); await f.settle();
  assert.equal(f.dialog.open, true); assert.equal(dialogTitle(), '복사·붙여넣기를 끌까요?');
  assert.equal(f.dialog.querySelector('p.confirm-who').textContent, `${member.username} 님이 지금 업무 101에 접속 중이에요`);
  assert.equal(on(control(member.username).querySelector('[data-clipboard-permission="textToRemote"]')), true, 'the switch shows the saved value until the answer');
  f.click(f.dialog, '취소'); await f.settle();
  assert.equal(sent().length, 1, 'cancel sends nothing');
  f.change(control(member.username).querySelector('[data-clipboard-permission="textToRemote"]'), false); await f.settle();
  await submit(f);
  assert.equal(sent().length, 2);
  assert.deepEqual(sent()[1].data, { permissions: [{ username: member.username, expectedAccountId: member.id, clipboardPolicy: clipDenyFixture, devicePolicy: deviceDenyFixture }] });
  assert.equal(f.writes.some(write => write.path.endsWith('/assignments/batch')), false);
});

test('a desktop shows what its Host can provide for each device feature, from the automatic Host report', async t => {
  const { user, inventory } = workspaceFixture();
  const devices = { schema: 1, drives: { winfsp: false, package: true, webClient: 'usable' }, usb: { supported: true, package: true, controller: 'stopped' }, printers: { reason: 'print_spooler_unavailable', spooler: 'disabled' }, audio: { output: 'unknown' },
    displayAdapters: { read: true, current: 1, legacy: 2, duplicate: 0, unproven: 0, foreign: 1, currentStarted: true, blocker: '', cleanup: 'pending_restart' }, usbController: 'foreign' };
  let reported = null;
  const f = await loadAdmin(t, 'device-panel', user, inventory, { hash: '#/desktops/pve-170', respond: url => /\/desktops\/pve-170\/diagnostics$/.test(url) ? { ok: true, status: 200, json: async () => ({ supported: true, connected: true, receivedAt: iso(0), stale: false,
    report: { version: '0.2.140', display: { observed: true, monitorCount: 1, installed: true, deviceReady: true, owned: true, activeDisplay: true, reason: '' }, native: { available: false }, events: [], ...(reported ? { devices: reported } : {}) } }) } : null });
  await f.settle();
  assert.match(f.content.querySelector('.device-panel').textContent, /장치 상태 보고가 아직 없어요/, 'a Host without the report says so instead of guessing');
  reported = devices;
  await recheck(f);
  const device = name => f.content.querySelector(`.device-panel [data-device="${name}"]`);
  assert.deepEqual(['가상 화면', '공유 폴더', 'USB', '프린터', '사운드카드'].map(name => device(name).querySelector('.state-dot').textContent), ['확인 필요', '준비됨', '다른 프로그램이 사용', '확인 필요', '알 수 없음']);
  assert.match(device('가상 화면').textContent, /사용 중 1 · 남은 장치 3 \(Multitap 것 아님 1\)/);
  assert.equal(device('가상 화면').querySelector('details').hasAttribute('open'), false, 'the counts sit behind 자세히');
  assert.match(device('가상 화면').querySelector('details').textContent, /이전 버전 2 · 중복 0 · 확인 안 됨 0 · 다른 제품 1[\s\S]*Windows 재시작 후 정리돼요/);
  assert.match(device('공유 폴더').textContent, /WebDAV/); assert.match(device('프린터').textContent, /사용 안 함/);
  // The observation sits in the same window as the grants (쓰는 사람 on 사용자), on the 자세히 tab.
  const more = f.content.querySelector('.device-panel').closest('[role=tabpanel]');
  assert.equal(f.content.querySelector('#' + more.getAttribute('aria-labelledby')).textContent, '자세히');
  assert.equal(more.hasAttribute('hidden'), true, 'shown when its tab is pressed');
  assert.equal(f.content.querySelector('#desktop-people').closest('[role=tabpanel]').getAttribute('data-tab'), 'users');
});

test('데스크톱 배정 offers only registered desktops and approved local PCs; a new one starts with nothing shared and each change is one request', async t => {
  const { user, member, inventory } = permissionFixture();
  const pc = { id: 'pc-approved', name: '업무 로컬 PC', kind: 'pc', localPcApproved: true, os: 'Windows' };
  inventory.desktops.push(pc, { id: 'pc-unapproved', name: '미승인 수동 PC', kind: 'pc' }, { id: 'pc-unregistered', name: '등록 안 된 PC', kind: 'pc', localPcApproved: true },
    { id: 'pve-104', pve_vmid: 104, name: '등록 안 된 VM', os: '미확인', pve_node: 'pve', pve_available: 1, pve_power_status: 'running' });
  // A Host record alone does not approve a PC; approval alone does not register one.
  inventory.fixtureHosts.push({ desktopId: pc.id, hostId: '00000000-0000-4000-8000-000000000004' }, { desktopId: 'pc-unapproved', hostId: '00000000-0000-4000-8000-000000000005' });
  inventory.desktops[0].enabled = 0; // Historical VM-enabled flag is not assignment authorization.
  const f = await loadAdmin(t, 'unified-approved-pc-assignment', user, inventory);
  await openUser(f, member.username);
  assert.deepEqual([...f.content.querySelectorAll('li.assigned-row')].map(node => node.getAttribute('data-desktop')), ['pve-101', 'pve-102']);
  f.click(f.content, '데스크톱 배정');
  assert.equal(f.dialog.open, true); assert.equal(dialogTitle(), '데스크톱 배정'); assert.equal(dialogSave(), '배정');
  assert.deepEqual([...f.dialog.querySelectorAll('input[data-vm-id]')].map(node => node.getAttribute('data-vm-id')), ['pve-103', 'pc-approved'], 'registered and not yet assigned; never an unapproved or unregistered one');
  const checkbox = f.dialog.querySelector('[data-vm-id="pc-approved"]');
  assert.match(checkbox.getAttribute('aria-label'), /로컬 PC.*업무 로컬 PC/);
  await submit(f);
  assert.equal(f.dialog.open, true); assert.equal(document.querySelector('#modal-error').textContent, '배정할 데스크톱을 고르세요.');
  assert.deepEqual(f.writes, [], 'nothing is sent without a choice');
  checkbox.checked = true;
  await submit(f);
  assert.equal(f.dialog.open, false);
  assert.deepEqual(f.writes, [{ path: '/api/v1/admin/assignments/batch', data: { username: member.username, expectedAccountId: member.id, desktopIds: ['pve-101', 'pve-102', 'pc-approved'] } }], 'the assignment carries no sharing value');
  assert.equal(noticeNode().textContent, `${member.username} 님에게 데스크톱 1대 배정을 저장했어요.`);
  assert.deepEqual([...f.content.querySelectorAll('li.assigned-row')].map(node => node.getAttribute('data-desktop')), ['pve-101', 'pve-102', 'pc-approved']);
  assert.equal(sentence(f, 'pc-approved'), '로컬 PC · 아무것도 주고받지 않아요');
  assert.equal(preset(f, 'pc-approved', 'default').getAttribute('aria-pressed'), 'true');
  fire(f, details(f, 'pc-approved'));
  assert.equal(row(f, 'pc-approved').querySelectorAll('.sharing-fine input[role=switch]').length, 9);
  assert.equal([...row(f, 'pc-approved').querySelectorAll('.sharing-fine input')].some(on), false, 'new PC sharing starts denied');
  f.change(toggle(f, 'pc-approved', 'filesToLocal'), true); await f.settle();
  assert.equal(batches(f).length, 2);
  assert.deepEqual(batches(f)[1].data, { username: member.username, expectedAccountId: member.id, assignments: [{ desktopId: 'pve-101' }, { desktopId: 'pve-102' },
    { desktopId: pc.id, clipboardPolicy: { ...clipDenyFixture, filesToLocal: true }, devicePolicy: deviceDenyFixture }] }, 'untouched saved VM values are preserved by omission');
  fire(f, details(f, 'pve-101'));
  const savedVmClipboard = { ...clipDenyFixture, textToRemote: true };
  assert.deepEqual(Object.fromEntries([...row(f, 'pve-101').querySelectorAll('[data-clipboard-permission]')].map(input => [input.getAttribute('data-clipboard-permission'), on(input)])), savedVmClipboard, 'the PC change does not leak to the VM');
  assert.deepEqual(inventory.assignments.find(entry => entry.desktop_id === 'pve-101').clipboardPolicy, savedVmClipboard, 'the request preserved the saved VM grant and all denials');
  assert.deepEqual(inventory.assignments.find(entry => entry.desktop_id === pc.id).clipboardPolicy, { ...clipDenyFixture, filesToLocal: true });
});

test('unapproved PC deep links stay excluded and a local PC has no power targets', async t => {
  const { user, inventory } = workspaceFixture();
  inventory.desktops.push({ id: 'pc-approved', name: '승인된 물리 PC', kind: 'pc', localPcApproved: true }, { id: 'pc-unapproved', name: '미승인 물리 PC', kind: 'pc' });
  inventory.fixtureHosts.push({ desktopId: 'pc-approved', hostId: '00000000-0000-4000-8000-000000000004', version: '1.0.12' });
  inventory.desktops[0].enabled = 0;
  const f = await loadAdmin(t, 'workspace-pc-deep-link', user, inventory, { hash: '#/desktops/pc-unapproved' });
  const powerButtons = () => [...f.content.querySelectorAll('button')].filter(button => ['켜기', '끄기', '다시 시작'].includes(button.textContent.trim()));
  assert.match(f.content.textContent, /데스크톱을 찾지 못했어요/);
  assert.equal(f.content.querySelector('.dt-head'), null);
  assert.deepEqual(powerButtons(), []);
  f.navigate('desktops'); await f.settle();
  assert.equal(f.content.querySelector('.vm-list-item[data-focus="vm-pick-pc-unapproved"]'), null, 'legacy kind=pc is never approved implicitly');
  assert.ok(f.content.querySelector('.vm-list-item[data-focus="vm-pick-pc-approved"]'), 'approved PCs with registered Hosts remain visible');
  assert.ok(f.content.querySelector('.vm-list-item[data-focus="vm-pick-' + inventory.desktops[0].id + '"]'), 'enabled0 does not hide a real VM');
  f.pickVm('pve-101'); await f.settle();
  assert.ok(powerButtons().length > 0, 'a running VM has its power buttons');
  f.pickVm('pc-approved'); await f.settle();
  assert.equal(f.content.querySelector('.dt-head h2').textContent, '승인된 물리 PC');
  assert.deepEqual(powerButtons(), [], 'a local PC has no Proxmox power controls');
  assert.equal(f.writes.some(write => /\/desktops\/pc-(?:approved|unapproved)\/power$/.test(write.path)), false);
});

test('grouping by Proxmox node keeps local PCs outside every node and lists registered Hosts only', async t => {
  const { user, inventory } = workspaceFixture();
  inventory.desktops.push({ id: 'pc-approved', name: '노드에 없는 물리 PC', kind: 'pc', localPcApproved: true }, { id: 'pc-registered', name: '등록된 물리 PC', kind: 'pc', localPcApproved: true });
  inventory.fixtureHosts.push({ desktopId: 'pc-registered', hostId: '00000000-0000-4000-8000-000000000004', version: '1.0.12' });
  const f = await loadAdmin(t, 'node-board-pc-boundary', user, inventory);
  f.navigate('desktops'); await f.settle();
  f.change(f.content.querySelector('input[data-focus="desktops-by-node"]'), true); await f.settle();
  const groups = Object.fromEntries([...f.content.querySelectorAll('section.vm-list-group')].map(group => [group.getAttribute('aria-label'), [...group.querySelectorAll('a.vm-list-item')].map(node => node.getAttribute('data-focus'))]));
  assert.deepEqual(Object.keys(groups), ['Proxmox pve-fixture', '로컬 PC']);
  assert.deepEqual(groups['Proxmox pve-fixture'].toSorted(), inventory.desktops.filter(desktop => desktop.kind !== 'pc').map(desktop => 'vm-pick-' + desktop.id).toSorted(), 'a node holds its VMs and no PC');
  assert.deepEqual(groups['로컬 PC'], ['vm-pick-pc-registered'], 'an approved PC without a registered Host is not listed');
  assert.equal(f.content.textContent.includes('노드에 없는 물리 PC'), false);
  assert.deepEqual(f.writes.filter(write => /\/power$|\/assignments/.test(write.path)), []);
});

test('the account page keeps the user × desktop target explicit, opens one editor at a time and grants a new desktop nothing', async t => {
  const { user, member, inventory } = permissionFixture();
  const f = await loadAdmin(t, 'account-workspace-target', user, inventory);
  await openUser(f, member.username);
  assert.equal(f.content.querySelector('.sharing-fine'), null, 'the switches stay behind 세부');
  assert.equal(row(f, 'pve-101').querySelector('.preset-seg').getAttribute('aria-label'), `${member.username} · 업무 101 공유`);
  fire(f, details(f, 'pve-101')); fire(f, details(f, 'pve-102'));
  assert.equal(f.content.querySelectorAll('.sharing-fine').length, 1, 'only the chosen desktop has an editor');
  assert.equal(details(f, 'pve-102').textContent, '닫기'); assert.equal(details(f, 'pve-101').textContent, '세부');
  const editor = () => row(f, 'pve-102').querySelector('.sharing-fine');
  assert.deepEqual([...editor().querySelectorAll('input[role=switch]')].map(node => [node.getAttribute('data-account'), node.getAttribute('data-vm')]), Array(9).fill([member.username, 'pve-102']));
  assert.equal(editor().querySelectorAll('[data-clipboard-permission]').length, 4);
  assert.equal(editor().querySelectorAll('[data-device-permission]').length, 5);
  assert.equal(editor().querySelector('.sharing-help').hasAttribute('open'), false, 'long explanations start folded');
  assert.equal(editor().querySelector('.sharing-rare').hasAttribute('open'), false);
  f.follow(f.content.querySelector('.account-list'), user.username); await f.settle();
  assert.equal(f.content.querySelector('.sharing-fine'), null, 'a different account does not inherit the editor');
  assert.equal(f.content.querySelector('li.assigned-row'), null);
  f.follow(f.content.querySelector('.account-list'), member.username); await f.settle();
  assert.deepEqual(f.writes, [], 'looking sends nothing');
  f.click(f.content, '데스크톱 배정');
  assert.deepEqual([...f.dialog.querySelectorAll('input[data-vm-id]')].map(node => node.getAttribute('data-vm-id')), ['pve-103']);
  f.click(f.dialog, '취소'); await f.settle();
  assert.deepEqual(f.writes, [], 'a cancelled assignment sends nothing');
  f.click(f.content, '데스크톱 배정');
  f.dialog.querySelector('input[data-vm-id="pve-103"]').checked = true;
  await submit(f);
  assert.deepEqual(f.writes, [{ path: '/api/v1/admin/assignments/batch', data: { username: member.username, expectedAccountId: member.id, desktopIds: ['pve-101', 'pve-102', 'pve-103'] } }]);
  assert.equal(preset(f, 'pve-103', 'default').getAttribute('aria-pressed'), 'true');
  fire(f, details(f, 'pve-103'));
  assert.equal([...row(f, 'pve-103').querySelectorAll('.sharing-fine input')].some(on), false, 'a new desktop is granted nothing');
  // 빼기 asks once; cancel sends nothing, and the confirmed request names what remains.
  fire(f, f.content.querySelector('[data-remove-vm="pve-101"]'));
  assert.equal(f.dialog.open, true); assert.equal(dialogTitle(), '배정 빼기'); assert.equal(dialogSave(), '빼기');
  assert.match(f.dialog.textContent, /VM 101 · 업무 101/); assert.match(f.dialog.textContent, new RegExp(member.username));
  f.click(f.dialog, '취소'); await f.settle();
  assert.equal(f.writes.length, 1, 'a cancelled 빼기 sends nothing'); assert.ok(row(f, 'pve-101'));
  fire(f, f.content.querySelector('[data-remove-vm="pve-101"]'));
  await submit(f);
  assert.equal(f.writes.length, 2);
  assert.deepEqual(f.writes[1], { path: '/api/v1/admin/assignments/batch', data: { username: member.username, expectedAccountId: member.id, desktopIds: ['pve-102', 'pve-103'] } });
  assert.equal(row(f, 'pve-101'), null);
  assert.equal(noticeNode().textContent, `${member.username} 님의 업무 101 배정을 뺐어요.`);
});

test('while a save is in flight every sharing control is locked and an earlier enabled control sends nothing', async t => {
  const { user, member, inventory } = permissionFixture();
  let release;
  const pending = new Promise(resolve => { release = resolve; });
  t.after(() => release(null));
  const f = await loadAdmin(t, 'assignment-pending-revert', user, inventory, { respond: (url, options) => url.endsWith('/assignments/batch') && options.method === 'PUT' ? pending : null });
  await openUser(f, member.username);
  fire(f, details(f, 'pve-102'));
  const controls = () => [...f.content.querySelectorAll('#assignments button[data-preset],#assignments button[data-same-preset],#assignments button[data-remove-vm]'), f.find(f.content, 'button', '데스크톱 배정')];
  const earlier = { controls: controls(), switches: [...f.content.querySelectorAll('#assignments input[role=switch]')] };
  assert.equal(earlier.controls.length, 12); assert.equal(earlier.switches.length, 9);
  assert.equal(earlier.controls.some(node => node.disabled), false);
  f.change(toggle(f, 'pve-102', 'filesToRemote'), true); await f.settle();
  assert.equal(batches(f).length, 1);
  assert.equal(controls().every(node => node.disabled), true);
  for (const node of f.content.querySelectorAll('#assignments button[data-preset],#assignments button[data-same-preset]')) assert.equal(node.title, '저장하는 중이에요.');
  const switches = () => [...f.content.querySelectorAll('#assignments input[role=switch]')];
  assert.equal(switches().length, 9); assert.equal(switches().every(node => node.hasAttribute('disabled')), true);
  // Synthetic events also exercise the handler guards, beyond native disabled behaviour.
  for (const node of [...controls(), ...earlier.controls]) fire(f, node);
  for (const node of [...switches(), ...earlier.switches]) { node.checked = true; fire(f, node, 'change'); }
  await f.settle();
  assert.notEqual(f.dialog.open, true, 'no assignment or removal dialog opens meanwhile');
  f.navigate('users'); f.follow(f.content, member.username); await f.settle();
  assert.equal(controls().every(node => node.disabled), true, 'still locked after leaving and returning');
  for (const node of controls()) fire(f, node);
  await f.settle();
  assert.equal(batches(f).length, 1, 'exactly one request while it is pending');
  assert.equal(f.writes.length, 1);
  assert.equal(batches(f)[0].data.assignments.find(a => a.desktopId === 'pve-102').clipboardPolicy.filesToRemote, true);
  release(null); await f.settle();
  assert.equal(on(toggle(f, 'pve-102', 'filesToRemote')), true, 'the acknowledged request becomes the saved value');
  assert.equal(controls().some(node => node.disabled), false);
  assert.equal(switches().some(node => node.hasAttribute('disabled')), false);
  assert.equal(f.writes.length, 1);
  assert.match(noticeNode().textContent, /^저장했어요\./);
});

test('a failed save shows the saved value again, says so, and sends no second request', async t => {
  const { user, member, inventory } = permissionFixture();
  const before = structuredClone(inventory.assignments);
  const monitor = { sessions: [session(member.username, 'pve-101')] };
  const f = await loadAdmin(t, 'sharing-failed-save', user, inventory, { monitor, respond: (url, options) => url.endsWith('/assignments/batch') && options.method === 'PUT' ? failed(500, 'internal_error') : null });
  await openUser(f, member.username);
  fire(f, details(f, 'pve-102'));
  f.change(toggle(f, 'pve-102', 'filesToRemote'), true); await f.settle();
  assert.equal(batches(f).length, 1, 'no automatic retry');
  assert.equal(noticeNode().className, 'is-error'); assert.equal(noticeNode().textContent, '요청을 처리하지 못했어요. 입력과 서버 상태를 확인하세요.');
  assert.equal(on(toggle(f, 'pve-102', 'filesToRemote')), false, 'the switch shows what is saved');
  assert.equal(f.content.querySelector('.saved-mark'), null);
  assert.equal(preset(f, 'pve-102', 'default').getAttribute('aria-pressed'), 'true');
  assert.equal(preset(f, 'pve-102', 'all').disabled, false, 'the controls are usable again');
  fire(f, preset(f, 'pve-102', 'all')); await f.settle();
  assert.equal(batches(f).length, 2, 'a new click is a new single request');
  assert.equal(preset(f, 'pve-102', 'all').getAttribute('aria-pressed'), 'false');
  assert.equal(sentence(f, 'pve-102'), 'VM 102 · 아무것도 주고받지 않아요');
  // A refused confirmation keeps its dialog and the saved value.
  fire(f, details(f, 'pve-101'));
  f.change(toggle(f, 'pve-101', 'textToRemote'), false); await f.settle();
  assert.equal(f.dialog.open, true);
  await submit(f);
  assert.equal(batches(f).length, 3);
  assert.equal(f.dialog.open, true); assert.equal(document.querySelector('#modal-error').textContent, '요청을 처리하지 못했어요. 입력과 서버 상태를 확인하세요.');
  f.click(f.dialog, '취소'); await f.settle();
  assert.equal(batches(f).length, 3);
  assert.equal(on(toggle(f, 'pve-101', 'textToRemote')), true);
  assert.equal(toggle(f, 'pve-101', 'textToRemote').hasAttribute('disabled'), false);
  assert.deepEqual(inventory.assignments, before);
});

test('turning sharing off for a connected account asks first; turning on, or off for someone not connected, does not', async t => {
  const { user, member, inventory } = permissionFixture();
  inventory.assignments[0].devicePolicy = { ...deviceDenyFixture, printers: true, drives: true };
  inventory.assignments[1].clipboardPolicy = { ...clipDenyFixture, textToLocal: true };
  const monitor = { sessions: [session(member.username, 'pve-101')] };
  const f = await loadAdmin(t, 'sharing-confirm-connected', user, inventory, { monitor });
  await openUser(f, member.username);
  assert.match(row(f, 'pve-101').textContent, /접속 중/); assert.doesNotMatch(row(f, 'pve-102').textContent, /접속 중/);
  fire(f, details(f, 'pve-101'));
  // On, for the connected account: at once.
  f.change(toggle(f, 'pve-101', 'filesToRemote'), true); await f.settle();
  assert.notEqual(f.dialog.open, true); assert.equal(batches(f).length, 1);
  // A clipboard item off: the connection ends, so it asks and names who is connected.
  f.change(toggle(f, 'pve-101', 'textToRemote'), false); await f.settle();
  assert.equal(f.dialog.open, true); assert.equal(dialogTitle(), '복사·붙여넣기를 끌까요?'); assert.equal(dialogSave(), '끄고 연결 끊기');
  assert.equal(f.dialog.querySelector('p.confirm-who').textContent, `${member.username} 님이 지금 업무 101에 접속 중이에요`);
  assert.match(f.dialog.textContent, /끄면 지금 연결이 끊겨요/);
  assert.equal(on(toggle(f, 'pve-101', 'textToRemote')), true, 'the switch goes back until the answer');
  assert.equal(batches(f).length, 1, 'nothing is sent before the answer');
  f.click(f.dialog, '취소'); await f.settle();
  assert.equal(f.dialog.open, false); assert.equal(batches(f).length, 1, 'cancel sends nothing');
  assert.equal(on(toggle(f, 'pve-101', 'textToRemote')), true);
  assert.equal(inventory.assignments[0].clipboardPolicy.textToRemote, true);
  f.change(toggle(f, 'pve-101', 'textToRemote'), false); await f.settle();
  await submit(f);
  assert.equal(f.dialog.open, false); assert.equal(batches(f).length, 2, 'the confirmed change is one request');
  assert.deepEqual(batches(f)[1].data, { username: member.username, expectedAccountId: member.id, assignments: [
    { desktopId: 'pve-101', clipboardPolicy: { ...clipDenyFixture, filesToRemote: true }, devicePolicy: { ...deviceDenyFixture, printers: true, drives: true } }, { desktopId: 'pve-102' }] });
  assert.equal(on(toggle(f, 'pve-101', 'textToRemote')), false);
  // A device off: the connection stays, and the question names the device.
  f.change(toggle(f, 'pve-101', 'printers'), false); await f.settle();
  assert.equal(f.dialog.open, true); assert.equal(dialogTitle(), '프린터 공유를 끌까요?'); assert.equal(dialogSave(), '끄기');
  assert.match(f.dialog.textContent, /연결은 그대로고, 끈 장치만 바로 빠져요/);
  f.click(f.dialog, '취소'); await f.settle();
  assert.equal(batches(f).length, 2);
  // A named choice that turns both kinds off.
  fire(f, preset(f, 'pve-101', 'default')); await f.settle();
  assert.equal(f.dialog.open, true); assert.equal(dialogTitle(), '공유를 끌까요?'); assert.equal(dialogSave(), '끄고 연결 끊기');
  f.click(f.dialog, '취소'); await f.settle();
  assert.equal(batches(f).length, 2);
  // Off for the desktop this account is not connected to: at once.
  fire(f, details(f, 'pve-102'));
  f.change(toggle(f, 'pve-102', 'textToLocal'), false); await f.settle();
  assert.notEqual(f.dialog.open, true); assert.equal(batches(f).length, 3);
  assert.deepEqual(batches(f)[2].data.assignments, [{ desktopId: 'pve-101' }, { desktopId: 'pve-102', clipboardPolicy: clipDenyFixture, devicePolicy: deviceDenyFixture }]);
  // "모두에 한 번에" asks only for the desktop that is connected.
  fire(f, same(f, 'default')); await f.settle();
  assert.equal(f.dialog.open, true); assert.equal(dialogTitle(), '공유를 끌까요?');
  assert.equal(f.dialog.querySelector('p.confirm-who').textContent, `${member.username} 님이 지금 업무 101에 접속 중이에요`);
  f.click(f.dialog, '취소'); await f.settle();
  assert.equal(batches(f).length, 3);
});

test('when the session list is unknown, turning sharing off asks first and cancel sends nothing', async t => {
  const { user, member, inventory } = permissionFixture();
  const f = await loadAdmin(t, 'sharing-confirm-unknown', user, inventory, { respond: url => url.endsWith('/admin/sessions') ? failed(500, 'internal_error') : null });
  await openUser(f, member.username);
  fire(f, details(f, 'pve-101'));
  f.change(toggle(f, 'pve-101', 'textToRemote'), false); await f.settle();
  assert.equal(f.dialog.open, true); assert.equal(dialogTitle(), '복사·붙여넣기를 끌까요?');
  assert.equal(f.dialog.querySelector('p.confirm-who').textContent, '접속 중인 사람을 확인하지 못했어요');
  f.click(f.dialog, '취소'); await f.settle();
  assert.deepEqual(f.writes, [], 'cancel sends nothing');
  assert.equal(on(toggle(f, 'pve-101', 'textToRemote')), true);
  f.change(toggle(f, 'pve-101', 'textToLocal'), true); await f.settle();
  assert.notEqual(f.dialog.open, true, 'turning on never asks');
  assert.equal(batches(f).length, 1);
  f.change(toggle(f, 'pve-101', 'textToRemote'), false); await f.settle();
  await submit(f);
  assert.equal(batches(f).length, 2);
  assert.deepEqual(batches(f)[1].data.assignments, [{ desktopId: 'pve-101', clipboardPolicy: { ...clipDenyFixture, textToLocal: true }, devicePolicy: deviceDenyFixture }, { desktopId: 'pve-102' }]);
});

// The account changes between the render and the click: the control that was
// on screen must not write to whatever now answers to that name.
const identityChanges = {
  replaced: { apply: (inventory, member) => { inventory.users[1] = { ...member, id: 'fixture-member-next' }; }, message: messages.account_identity_changed },
  renamed: { apply: (inventory, member) => { inventory.users[1] = { ...member, username: '바뀐이름' }; }, message: messages.account_identity_changed },
  stopped: { apply: (inventory, member) => { inventory.users[1] = { ...member, disabled: 1 }; }, message: messages.user_or_desktop_unavailable },
};
for (const [label, { apply, message }] of Object.entries(identityChanges)) test(`controls shown for an account that was then ${label} get no write and an error`, async t => {
  const { user, member, inventory } = permissionFixture();
  inventory.assignments[1].clipboardPolicy = { ...clipDenyFixture, textToLocal: true };
  const f = await loadAdmin(t, 'sharing-identity-' + label, user, inventory);
  await openUser(f, member.username);
  fire(f, details(f, 'pve-102'));
  const shown = { preset: preset(f, 'pve-102', 'all'), off: preset(f, 'pve-102', 'default'), on: toggle(f, 'pve-102', 'filesToRemote'), same: same(f, 'all'), assign: f.find(f.content, 'button', '데스크톱 배정'), remove: f.content.querySelector('[data-remove-vm="pve-102"]') };
  apply(inventory, member); await recheck(f);
  const refused = async (act, text, expected = message) => {
    document.querySelector('.notice-close')?.dispatchEvent(new f.window.Event('click'));
    act(); await f.settle();
    assert.deepEqual(f.writes, [], text); assert.notEqual(f.dialog.open, true, text);
    assert.equal(noticeNode().className, 'is-error', text); assert.equal(noticeNode().textContent, expected, text);
  };
  await refused(() => fire(f, shown.preset), 'a named choice');
  await refused(() => fire(f, shown.off), 'turning off');
  await refused(() => { shown.on.checked = true; fire(f, shown.on, 'change'); }, 'one switch');
  await refused(() => fire(f, shown.same), '모두에 한 번에');
  await refused(() => fire(f, shown.assign), '데스크톱 배정');
  if (label !== 'stopped') await refused(() => fire(f, shown.remove), '빼기');
  if (label === 'replaced') {
    // The page now shows the new account, and its own controls write under the new ID.
    fire(f, preset(f, 'pve-102', 'all')); await f.settle();
    assert.equal(batches(f).length, 1); assert.equal(batches(f)[0].data.expectedAccountId, 'fixture-member-next');
  } else if (label === 'renamed') {
    assert.match(f.content.textContent, /이 계정이 없어요/);
  } else {
    for (const node of f.content.querySelectorAll('#assignments button[data-preset],#assignments button[data-same-preset]')) { assert.equal(node.disabled, true); assert.equal(node.title, '사용 중지된 계정은 바꿀 수 없어요.'); }
    assert.equal([...f.content.querySelectorAll('#assignments input[role=switch]')].every(node => node.hasAttribute('disabled')), true);
  }
});

// EXPECTED TO FAIL until the source is fixed (suspected bug, reported; the test
// is not bent to pass). The server refuses every assignment change of a
// stopped account (src/store.mjs replaceVmAssignments: user.disabled ->
// user_or_desktop_unavailable), and every other control of a stopped account
// is locked, but 빼기 stays enabled on both pages (admin/js/users.js
// desktopsPanel, admin/js/desktops.js peoplePanel; permissions.js
// removeAssignment does not look at `disabled`), so its dialog can only fail.
test('a stopped account offers no 빼기, like its other controls: the server refuses any assignment change of a stopped account', async t => {
  const { user, member, inventory } = permissionFixture();
  inventory.users[1] = { ...member, disabled: 1 };
  const f = await loadAdmin(t, 'sharing-stopped-remove', user, inventory);
  await openUser(f, member.username);
  assert.equal(f.find(f.content, 'button', '데스크톱 배정').disabled, true);
  assert.equal(preset(f, 'pve-101', 'all').disabled, true);
  assert.deepEqual([...f.content.querySelectorAll('#assignments [data-remove-vm]')].map(node => [node.getAttribute('data-remove-vm'), node.disabled]), [['pve-101', true], ['pve-102', true]], 'the account page');
  f.navigate('desktops'); f.pickVm('pve-101'); await f.settle();
  const person = f.content.querySelector(`#desktop-people li.person-row[data-person="${member.username}"]`);
  assert.equal(f.find(person, 'button', '공유 바꾸기').disabled, true);
  assert.equal(f.find(person, 'button', '빼기').disabled, true, "the desktop's 쓰는 사람");
  assert.deepEqual(f.writes, []);
});

test('the account list has no sharing controls; the window of each account edits its desktops', async t => {
  const { user, member, inventory } = permissionFixture();
  const { writes, follow, navigate, content, settle, crumb } = await loadAdmin(t, 'account-permission-entry', user, inventory);
  navigate('users');
  assert.equal(content.querySelector('[data-preset],[data-same-preset],[data-clipboard-permission],[data-device-permission],[data-permission-vm],[data-remove-vm]'), null);
  assert.equal([...content.querySelectorAll('button')].some(node => ['클립보드', '권한 편집', '세부', '빼기', '데스크톱 배정'].includes(node.textContent)), false);
  assert.equal(content.textContent.includes('전송 차단'), false);
  // One short sentence stands beside the title; the numbers stand above the table.
  assert.equal(content.querySelector('.page-header .page-description'), null, 'no subtitle that narrates the list');
  assert.match(content.querySelector('.account-list .list-count').textContent, /^총 \d+명 · 관리자 \d+/);
  // What a new assignment starts with is said where assignments are made: the window's 데스크톱 tab.
  assert.equal(content.querySelector('.account-list .list-note'), null);
  assert.equal(content.querySelector('#detail'), null, 'no window until a row is pressed');
  follow(content, member.username); await settle();
  assert.equal(crumb(), member.username);
  assert.match(content.querySelector('#assignments .panel-head').textContent, /^쓸 수 있는 데스크톱2/);
  assert.match(content.querySelector('.account-head').textContent, /보존할 비고/);
  assert.deepEqual(writes, []);
});

test('each assigned desktop edits its own clipboard and device sharing in place, and every change is one request sent at once', async t => {
  const { user, member, inventory } = permissionFixture();
  const { window, writes, dialog, click, follow, navigate, settle, content, change } = await loadAdmin(t, 'assignment-permissions', user, inventory);
  const f = { window, content };
  navigate('users'); follow(content, member.username); await settle();
  assert.deepEqual([...content.querySelectorAll('#assignments li.assigned-row')].map(node => node.getAttribute('data-desktop')), ['pve-101', 'pve-102']);
  assert.equal(details(f, 'pve-103'), null, 'an unassigned desktop has no sharing to edit');
  assert.equal(content.querySelector('#assignments [data-vm="pve-103"]'), null);
  // The saved value as one named choice and one sentence per row.
  assert.equal(row(f, 'pve-101').querySelector('span[data-preset="custom"]').textContent, '직접 설정');
  assert.deepEqual(['default', 'clipboard', 'all'].map(key => preset(f, 'pve-101', key).getAttribute('aria-pressed')), ['false', 'false', 'false']);
  assert.equal(sentence(f, 'pve-101'), 'VM 101 · 글·그림 복사는 데스크톱으로만');
  assert.equal(preset(f, 'pve-102', 'default').getAttribute('aria-pressed'), 'true'); assert.equal(row(f, 'pve-102').querySelector('[data-preset="custom"]'), null);
  assert.equal(sentence(f, 'pve-102'), 'VM 102 · 아무것도 주고받지 않아요');
  assert.equal([...content.querySelectorAll('button')].some(node => ['변경 적용', '되돌리기'].includes(node.textContent)), false, 'there is nothing to apply later');
  // The switches open in place: both directions and every device item, default deny.
  assert.equal(details(f, 'pve-102').getAttribute('aria-expanded'), 'false');
  assert.equal(details(f, 'pve-102').getAttribute('aria-controls'), `sharing-${member.id}:pve-102`);
  fire(f, details(f, 'pve-101'));
  assert.equal(on(toggle(f, 'pve-101', 'textToRemote')), true, 'the row opens the actual saved grant');
  assert.equal([...row(f, 'pve-101').querySelectorAll('[data-device-permission]')].some(on), false, 'saved device denials are shown as saved');
  fire(f, details(f, 'pve-102'));
  const panel = () => row(f, 'pve-102').querySelector('.sharing-fine');
  assert.ok(panel(), 'the editor opens inside its own row');
  assert.equal(panel().id, `sharing-${member.id}:pve-102`);
  assert.equal(details(f, 'pve-102').getAttribute('aria-expanded'), 'true');
  assert.notEqual(dialog.open, true, 'no dialog for per-desktop sharing');
  assert.equal(panel().querySelectorAll('[data-clipboard-permission]').length, 4);
  assert.deepEqual([...panel().querySelectorAll('[data-device-permission]')].map(input => input.getAttribute('data-device-permission')), deviceKeysFixture);
  assert.equal([...panel().querySelectorAll('input[type=checkbox]')].some(on), false);
  assert.match(panel().textContent, /PC → 데스크톱/); assert.match(panel().textContent, /데스크톱 → PC/);
  for (const label of ['글·그림 PC → 데스크톱', '글·그림 데스크톱 → PC', '파일 PC → 데스크톱', '파일 데스크톱 → PC']) assert.ok([...panel().querySelectorAll('[data-clipboard-permission]')].some(input => input.getAttribute('aria-label') === label + ' 허용'), label);
  for (const label of ['공유 폴더', '프린터', 'USB 장치', '스마트 카드', '직렬(COM) 포트']) assert.ok([...panel().querySelectorAll('[data-device-permission]')].some(input => input.getAttribute('aria-label') === label + ' 허용'), label);
  assert.match(panel().textContent, /복사·붙여넣기를 켜면 연결 중에도 바로 적용돼요\(앱 1\.1\.13부터\)\. 그 밖에 켠 것은 다음 접속부터예요\./); assert.match(panel().textContent, /복사·붙여넣기를 끄면 그 데스크톱의 진행 중인 연결과 전송이 끝나요\./);
  assert.match(content.querySelector('#assignments .note').textContent, /바꾸면 바로 저장돼요\./);
  assert.equal(panel().querySelector('input[type=password]'), null);
  assert.deepEqual(writes, [], 'opening and reading sends nothing');
  // One switch is one request, at once; unchanged desktops omit their value so the saved one is kept.
  const sent = () => writes.filter(write => write.path.endsWith('/assignments/batch'));
  const expectBatch = (index, assignments) => { assert.equal(writes.length, index + 1, 'exactly one request per change'); assert.deepEqual(writes[index], { path: '/api/v1/admin/assignments/batch', data: { username: member.username, expectedAccountId: member.id, assignments } }); };
  change(toggle(f, 'pve-102', 'filesToLocal'), true); await settle();
  expectBatch(0, [{ desktopId: 'pve-101' }, { desktopId: 'pve-102', clipboardPolicy: { ...clipDenyFixture, filesToLocal: true }, devicePolicy: deviceDenyFixture }]);
  assert.equal(on(toggle(f, 'pve-102', 'filesToLocal')), true, 'the re-rendered switch shows the saved value');
  assert.equal(noticeNode().textContent, `저장했어요.\n${member.username} · 업무 102: 직접 설정`);
  assert.equal(row(f, 'pve-102').querySelector('.saved-mark').textContent, '저장했어요');
  assert.equal(sentence(f, 'pve-102'), 'VM 102 · 파일은 PC로만');
  change(toggle(f, 'pve-102', 'drives'), true); await settle();
  expectBatch(1, [{ desktopId: 'pve-101' }, { desktopId: 'pve-102', clipboardPolicy: { ...clipDenyFixture, filesToLocal: true }, devicePolicy: { ...deviceDenyFixture, drives: true } }]);
  change(toggle(f, 'pve-102', 'usbDevices'), true); await settle();
  expectBatch(2, [{ desktopId: 'pve-101' }, { desktopId: 'pve-102', clipboardPolicy: { ...clipDenyFixture, filesToLocal: true }, devicePolicy: { ...deviceDenyFixture, drives: true, usbDevices: true } }]);
  assert.equal(on(toggle(f, 'pve-102', 'drives')), true); assert.equal(on(toggle(f, 'pve-102', 'usbDevices')), true);
  assert.equal(sentence(f, 'pve-102'), 'VM 102 · 파일은 PC로만, 공유 폴더, USB 장치');
  assert.equal(row(f, 'pve-102').querySelector('span[data-preset="custom"]').textContent, '직접 설정');
  assert.equal(writes.some(write => write.path.includes('/users/')), false, 'no account-wide permission write');
  fire(f, details(f, 'pve-101'));
  assert.equal(on(toggle(f, 'pve-101', 'textToRemote')), true, 'the omitted saved grant is kept');
  assert.deepEqual(inventory.assignments.find(a => a.desktop_id === 'pve-101').clipboardPolicy, { ...clipDenyFixture, textToRemote: true });
  // "모두에 한 번에": one named choice for every assigned desktop in one atomic request.
  const text = { clipboardPolicy: { ...clipDenyFixture, textToRemote: true, textToLocal: true }, devicePolicy: deviceDenyFixture };
  fire(f, same(f, 'clipboard')); await settle();
  assert.notEqual(dialog.open, true, 'nobody is connected, so nothing is asked');
  expectBatch(3, [{ desktopId: 'pve-101', ...text }, { desktopId: 'pve-102', ...text }]);
  assert.equal(noticeNode().textContent, `저장했어요.\n${member.username}: 데스크톱 2대 · 글·그림 복사`);
  for (const id of ['pve-101', 'pve-102']) { assert.equal(preset(f, id, 'clipboard').getAttribute('aria-pressed'), 'true'); assert.equal(sentence(f, id), `VM ${id.slice(4)} · 글·그림 복사 양쪽`); assert.ok(row(f, id).querySelector('.saved-mark')); }
  // Assigning one more desktop sends no sharing value: it starts denied.
  click(content, '데스크톱 배정');
  assert.deepEqual([...dialog.querySelectorAll('input[data-vm-id]')].map(node => node.getAttribute('data-vm-id')), ['pve-103']);
  dialog.querySelector('input[data-vm-id="pve-103"]').checked = true;
  await document.querySelector('#modal-form').onsubmit({ preventDefault() {} }); await settle();
  assert.equal(writes.length, 5);
  assert.deepEqual(writes[4], { path: '/api/v1/admin/assignments/batch', data: { username: member.username, expectedAccountId: member.id, desktopIds: ['pve-101', 'pve-102', 'pve-103'] } });
  assert.equal(preset(f, 'pve-103', 'default').getAttribute('aria-pressed'), 'true');
  fire(f, details(f, 'pve-103'));
  assert.equal([...row(f, 'pve-103').querySelectorAll('.sharing-fine input[type=checkbox]')].some(on), false, 'a newly assigned desktop starts denied');
  change(toggle(f, 'pve-103', 'textToLocal'), true); await settle();
  expectBatch(5, [{ desktopId: 'pve-101' }, { desktopId: 'pve-102' }, { desktopId: 'pve-103', clipboardPolicy: { ...clipDenyFixture, textToLocal: true }, devicePolicy: deviceDenyFixture }]);
  // The bulk choice names every desktop and sends a value only where it changes something.
  fire(f, same(f, 'clipboard')); await settle();
  expectBatch(6, [{ desktopId: 'pve-101' }, { desktopId: 'pve-102' }, { desktopId: 'pve-103', ...text }]);
  fire(f, same(f, 'clipboard')); await settle();
  assert.equal(writes.length, 7, 'a choice that changes nothing sends nothing');
  fire(f, same(f, 'all')); await settle();
  expectBatch(7, ['pve-101', 'pve-102', 'pve-103'].map(desktopId => ({ desktopId, clipboardPolicy: allClipboard(true), devicePolicy: allDevices(true) })));
  assert.equal(sent().length, writes.length, 'every write was the one assignment request');
});
