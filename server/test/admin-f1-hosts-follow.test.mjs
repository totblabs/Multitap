// 홈, 데스크톱 and 업데이트, each with the answers the real server gives:
// a desktop's state follows the Host list the console keeps reading (and the inventory is read again when the
// two differ), the bulk Host update counts only the desktops it offered, a failed remote update is over once
// the Host runs its target version, and the 업데이트 page neither jumps nor shows the restart twice.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { loadAdmin, iso, registerFixtureDesktops } from './admin-ui-harness.mjs';

const user = { id: 'owner', username: 'owner', role: 'admin', mustChangeCredentials: false };
const inventoryReads = f => f.requests.filter(url => url === '/api/v1/admin/inventory').length;

test('a desktop turns 접속 가능 by itself when its Host answers, and the inventory is read again once per difference, never under an open form', async t => {
  t.mock.timers.enable({ apis: ['setInterval', 'Date'], now: Date.now() });
  const vm = connected => ({ id: 'pve-101', kind: 'vm', name: '디자인 1', pve_vmid: 101, pve_node: 'pve1', pve_available: 1, pve_power_status: 'running', host: connected ? { connected: true, version: '1.0.43', lastSeenAt: iso(0) } : { connected: false }, connectionModes: { stream: connected } });
  const row = connected => ({ desktopId: 'pve-101', hostId: '00000000-0000-4000-8000-000000000001', hostname: 'WIN11-DESIGN', version: '1.0.43', connected, enrolledAt: iso(-60000), lastSeenAt: connected ? iso(0) : null, diagnostics: null });
  // Right after an approval: the desktop is registered, its Host has not connected yet (it asks every 3 s).
  const inventory = { users: [user], assignments: [], audit: [], proxmox: { configured: true, state: 'ok' }, desktops: [vm(false)], hostUpdates: { latest: null, desktops: {} } };
  const monitor = { hosts: [row(false)] };
  const f = await loadAdmin(t, 'f1-follow-hosts', user, inventory, { monitor });
  const unreachable = () => f.content.querySelector('[data-todo=unreachable]');
  // The polling interval reads the Host list again once its last answer is older than 9 s.
  const poll = async () => { t.mock.timers.tick(10000); await f.settle(); };
  assert.ok(unreachable(), 'not connected yet: nobody can reach it'); assert.equal(inventoryReads(f), 1);
  // The Host connects. The Host list says so; this fixture's inventory still says what it said.
  monitor.hosts = [row(true)]; await poll();
  assert.equal(unreachable(), null, 'the polled Host list decides, not the inventory read at the approval');
  assert.equal(inventoryReads(f), 2, 'the difference makes the console read the inventory again');
  await poll(); assert.equal(inventoryReads(f), 2, 'the same difference is not read for again');
  f.navigate('desktops'); await f.settle();
  assert.ok(f.content.querySelector('[data-status=ok]'), 'the list says 접속 가능 too'); assert.equal(f.content.querySelector('[data-status=bad]'), null);
  // It goes away again: 접속 불가 without a manual 새로고침.
  f.navigate('overview'); await f.settle();
  monitor.hosts = [row(false)]; await poll();
  assert.ok(unreachable()); const afterGone = inventoryReads(f);
  // Under an open form nothing is read again; the state still follows the list. Closing the form lets it be read.
  f.dialog.showModal(); monitor.hosts = [row(true)]; await poll();
  assert.equal(unreachable(), null); assert.equal(inventoryReads(f), afterGone, 'no inventory read under an open form');
  f.dialog.close(); await poll(); assert.equal(inventoryReads(f), afterGone + 1);
});

const hostView = extra => ({ protocol: 'host-update-v1', minimumVersion: '0.2.59', connected: true, supported: true, version: '0.2.59', latestVersion: '0.2.60', newer: true, available: true, sessionActive: false, operation: null, ...extra });
const hostVm = (vmid, name, version = '0.2.59') => ({ id: 'pve-' + vmid, name, kind: 'vm', pve_vmid: vmid, pve_available: 1, pve_node: 'fixture', pve_power_status: 'running', host: { connected: true, version } });
const latest = { version: '0.2.60', filename: 'Multitap-Host-0.2.60-windows-x64.exe', size: 17000000, sha256: 'a'.repeat(64), changelog: '안정성 개선' };
const failed = (targetVersion, code = 'driver_install_required') => ({ id: 'a7b3a1f4-5f5a-4bb1-8f0a-2e0f1b7c9d10', status: 'failed', code, fromVersion: '0.2.59', targetVersion, progress: null, rebootRequired: false, disconnectSessions: false, submittedAt: '2026-11-12T00:00:00.000Z', checkedAt: '2026-11-12T00:00:00.000Z' });

test('the bulk Host update leaves out of its count the desktops it never offered: the server answers a row for every registered desktop', async t => {
  const fleet = () => { const inventory = { users: [user], assignments: [], audit: [], proxmox: { configured: true },
    desktops: [hostVm(149, 'TESTER'), hostVm(150, 'SECOND'), hostVm(151, 'CURRENT', '0.2.60'), hostVm(152, 'ALSO-CURRENT', '0.2.60')],
    hostUpdates: { latest, desktops: { 'pve-149': hostView(), 'pve-150': hostView(), 'pve-151': hostView({ version: '0.2.60', newer: false, available: false }), 'pve-152': hostView({ version: '0.2.60', newer: false, available: false }) } } };
    registerFixtureDesktops(inventory); return inventory; };
  const submit = async f => { await document.querySelector('#modal-form').onsubmit({ preventDefault() {} }); await f.settle(); };
  // Both offered desktops start; the two that already run the latest Host are in the answer, not in the words.
  const all = await loadAdmin(t, 'f1-bulk-all', user, fleet());
  all.navigate('updates'); await all.settle(); all.click(all.content, '2대 모두 업데이트'); await all.settle(); await submit(all);
  assert.deepEqual(all.writes.filter(write => write.path.endsWith('/host-updates/all')).map(write => write.data), [{ version: '0.2.60' }]);
  assert.equal(document.querySelector('#notice .notice-text').textContent, 'Host 0.2.60 업데이트를 2대에 요청했어요.');
  assert.equal(all.content.querySelector('.host-batch-summary').textContent.includes('대상 아님'), false);
  assert.equal(all.content.querySelectorAll('.host-batch tbody tr').length, 2);
  // One of the offered desktops cannot start after all: that one is counted.
  const inventory = fleet(), one = await loadAdmin(t, 'f1-bulk-one-left', user, inventory);
  one.navigate('updates'); await one.settle(); one.click(one.content, '2대 모두 업데이트'); await one.settle();
  inventory.hostUpdates.desktops['pve-150'] = hostView({ available: false, operation: { ...failed('0.2.60'), status: 'downloading', progress: 10 } });
  await submit(one);
  assert.equal(document.querySelector('#notice .notice-text').textContent, 'Host 0.2.60 업데이트를 1대에 요청했어요. 1대는 대상이 아니거나 이미 진행 중이에요.');
  assert.ok(one.content.querySelector('.host-batch-summary').textContent.endsWith(' · 대상 아님 1대'));
});

test('a remote update that failed is over once the Host runs the target version; one waiting desktop is not "all"', async t => {
  const inventory = { users: [user], assignments: [], audit: [], proxmox: { configured: true },
    // FIXED was updated by hand after the failure, as its text asked; STILL-OLD was not.
    desktops: [hostVm(149, 'FIXED', '0.2.60'), hostVm(150, 'STILL-OLD')],
    hostUpdates: { latest, desktops: { 'pve-149': hostView({ version: '0.2.60', newer: false, available: false, operation: failed('0.2.60') }), 'pve-150': hostView({ operation: failed('0.2.60') }) } } };
  registerFixtureDesktops(inventory);
  const f = await loadAdmin(t, 'f1-failed-then-fixed', user, inventory);
  f.navigate('desktops'); await f.settle();
  f.pickVm('pve-149'); await f.settle();
  assert.equal(f.content.querySelector('[data-problem="host-update"]'), null, 'no failure line for a Host that runs the target version');
  assert.equal(f.content.querySelector('#detail .host-cell').textContent.includes('업데이트 실패'), false);
  f.navigate('desktops'); await f.settle(); f.pickVm('pve-150'); await f.settle();
  assert.ok(f.content.querySelector('[data-problem="host-update"]'), 'a Host still on the old version keeps its failure and 다시 시도');
  f.navigate('updates'); await f.settle();
  const rows = [...f.content.querySelectorAll('.settings-updates .row-line')].map(node => node.textContent);
  assert.equal(rows.filter(text => text.includes('지난 업데이트 실패')).length, 1); assert.ok(rows.some(text => text.includes('STILL-OLD · 지난 업데이트 실패')));
  assert.ok(f.find(f.content, 'button', '1대 업데이트'), 'one desktop: 1대 업데이트'); assert.equal(f.find(f.content, 'button', '1대 모두 업데이트'), undefined);
});

test('지금 업데이트(연결 끊김) says the update was requested, though the server answers "waiting" while the Host ends the session', async t => {
  const fleet = () => { const inventory = { users: [user], assignments: [], audit: [], proxmox: { configured: true }, desktops: [hostVm(150, 'BUSY')], hostUpdates: { latest, desktops: { 'pve-150': hostView({ sessionActive: true }) } } }; registerFixtureDesktops(inventory); return inventory; };
  // host-update.mjs request(): the session's stop is still pending, so the operation is 'waiting' either way.
  const respond = url => url.endsWith('/desktops/pve-150/host-update') ? { ok: true, status: 202, json: async () => ({ hostUpdate: hostView({ available: false, operation: { ...failed('0.2.60', ''), status: 'waiting' } }) }) } : null;
  const open = async suffix => { const f = await loadAdmin(t, suffix, user, fleet(), { respond }); f.navigate('desktops'); await f.settle(); f.pickVm('pve-150'); await f.settle(); f.click(f.content.querySelector('#detail'), 'Host 업데이트'); await f.settle(); return f; };
  const now = await open('f1-toast-now');
  now.click(now.dialog, '지금 업데이트(연결 끊김)'); await now.settle();
  assert.deepEqual(now.writes.filter(write => write.path.endsWith('/host-update')).map(write => write.data), [{ version: '0.2.60', disconnectSessions: true }]);
  assert.equal(document.querySelector('#notice .notice-text').textContent, 'BUSY: Host 0.2.60 업데이트를 요청했어요. 진행 상태가 표시돼요.');
  const later = await open('f1-toast-later');
  await document.querySelector('#modal-form').onsubmit({ preventDefault() {} }); await later.settle();
  assert.deepEqual(later.writes.filter(write => write.path.endsWith('/host-update')).map(write => write.data), [{ version: '0.2.60', disconnectSessions: false }]);
  assert.equal(document.querySelector('#notice .notice-text').textContent, 'BUSY: 연결이 끝나면 Host 0.2.60 업데이트를 시작해요.');
});

const serverInfo = (update, version = '0.8.1', kind = 'systemd') => ({ ok: true, status: 200, json: async () => ({ service: 'multitap-broker', version, deployment: { kind, configured: true }, update, checkedAt: new Date().toISOString() }) });
const emptyInventory = () => ({ users: [user], desktops: [], assignments: [], audit: [], proxmox: { configured: true } });

test('업데이트: a failed check is 확인 실패 on every install, and a running check does not ask for the button it disabled', async t => {
  const page = async (suffix, update, kind) => { const f = await loadAdmin(t, suffix, user, emptyInventory(), { deploymentKind: kind, respond: async url => url.endsWith('/server-info') ? serverInfo(update, '0.8.1', kind) : null });
    t.mock.timers.enable({ apis: ['setTimeout'] }); f.navigate('updates'); await f.settle(); t.mock.timers.reset(); return f.content.querySelector('.server-release').textContent; };
  // systemd: the updater could not read the feed (no release, no operation).
  const check = await page('f1-check-failed', { state: 'failed', feedConfigured: true, applySupported: true, errorCode: 'update_download_failed' }, 'systemd');
  assert.ok(check.includes('확인 실패')); assert.equal(check.includes('업데이트 실패'), false); assert.ok(check.includes('새 버전을 확인하지 못했어요'));
  // An update that did not finish stays 업데이트 실패.
  const apply = await page('f1-apply-failed', { state: 'failed', feedConfigured: true, applySupported: true, errorCode: 'update_disk_space', releaseId: 'a'.repeat(64), latest: { version: '0.8.2', releasedAt: '2026-11-11T03:00:00.000Z', changelog: 'x' } }, 'systemd');
  assert.ok(apply.includes('업데이트 실패'));
  // Docker, the first check still running.
  const checking = await page('f1-checking', { state: 'checking', feedConfigured: true, applySupported: false, channel: 'docker-release-feed', currentVersion: '0.8.1' }, 'docker');
  assert.ok(checking.includes('업데이트 확인 중')); assert.equal(checking.includes('업데이트 확인을 눌러'), false);
});

test('업데이트: while the server restarts the page keeps its shape and tells the restart once, as progress', async t => {
  const releaseId = 'a'.repeat(64), update = { state: 'available', feedConfigured: true, applySupported: true, releaseId, currentVersion: '0.8.1', latest: { version: '0.8.2', releasedAt: '2026-11-11T03:00:00.000Z', changelog: '변경' } };
  let down = false, reads = 0;
  const f = await loadAdmin(t, 'f1-restart', user, emptyInventory(), { deploymentKind: 'systemd', respond: async url => {
    if (url.endsWith('/server-info')) { reads++; if (down) throw new TypeError('Failed to fetch'); return serverInfo(update); }
    // As the updater answers: accepted, and for a moment its state is still 'available'.
    if (url.endsWith('/server-update/apply')) return { ok: true, status: 202, json: async () => ({ accepted: true, operationId: 'op' }) };
    return null;
  } });
  f.navigate('updates'); await f.settle();
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const foot = () => f.content.querySelector('.server-current .panel-foot'), progress = () => f.content.querySelector('.server-release .progress-block p')?.textContent ?? null;
  f.click(f.content, '업데이트'); await f.settle(); await document.querySelector('#modal-form').onsubmit({ preventDefault() {} }); await f.settle();
  // The first answer after the apply still says 'available': the page keeps following the update.
  assert.ok(progress(), 'still in progress though the updater has not changed its state yet'); assert.equal(foot(), null);
  const before = reads; t.mock.timers.tick(1500); await f.settle(); assert.ok(reads > before, 'it keeps asking');
  update.state = 'restarting'; t.mock.timers.tick(1500); await f.settle();
  assert.equal(progress(), '서버 다시 시작 중'); assert.equal(foot(), null);
  // The server is away: one sentence, in the progress, and no line added under 현재 서버.
  down = true; t.mock.timers.tick(1500); await f.settle();
  const sentence = '서버가 다시 시작되는 동안 연결을 확인하고 있어요.';
  assert.equal(progress(), sentence); assert.equal(foot(), null); assert.equal(f.content.textContent.split(sentence).length - 1, 1);
  t.mock.timers.tick(1500); await f.settle(); assert.equal(progress(), sentence); assert.equal(foot(), null);
  // Back with the new state: the progress follows it again.
  down = false; update.state = 'health_check'; t.mock.timers.tick(1500); await f.settle();
  assert.equal(progress(), '상태 확인 중'); assert.equal(foot(), null);
});
