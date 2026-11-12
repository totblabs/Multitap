// Small behaviours of the console, each on the page code itself.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { loadAdmin, permissionFixture, workspaceFixture, registerFixtureDesktops } from './admin-ui-harness.mjs';

const session = (n, desktopId, account) => ({ sessionId: `${String(n).repeat(8)}-1b2c-4d3e-8f00-1234567890ab`, desktopId, account, mode: 'stream', attached: true, ready: true });

test('홈 지금 접속 중 says when it lists fewer connections than it counts, and every 지금 할 일 title starts on the same line', async t => {
  const { user, inventory } = workspaceFixture();
  // Two things that can wait: the token's expiry (no dot) and the missing Console permission (amber dot).
  inventory.proxmox = { configured: true, state: 'ok', expiresAt: new Date(Date.now() + 20 * 86400000).toISOString(), consolePrivilege: 'missing' };
  const monitor = { sessions: Array.from({ length: 7 }, (_, i) => session(i + 1, 'pve-170', 'user-' + i)) };
  const f = await loadAdmin(t, 'f3-home', user, inventory, { monitor });
  const live = f.content.querySelector('.live-panel');
  assert.equal(live.querySelector('.count').textContent, '7'); assert.equal(live.querySelectorAll('tbody tr').length, 6);
  assert.equal(live.querySelector('.live-more').textContent, '7건 중 6건이에요. 나머지는 ‘모두 보기’에 있어요.');
  // A row without a red or amber dot keeps the dot's place (it stood 15 px to the left of its neighbours).
  const heads = [...f.content.querySelectorAll('.todo-panel .todo-item')];
  assert.equal(heads.length, 2); for (const head of heads) assert.equal(head.querySelectorAll('.todo-dot').length, 1, head.textContent);
  assert.deepEqual(heads.map(head => head.querySelector('.todo-dot').className).sort(), ['todo-dot is-none', 'todo-dot is-warn']);
  monitor.sessions = monitor.sessions.slice(0, 6); document.querySelector('#refresh').dispatchEvent(new f.window.Event('click')); await f.settle();
  assert.equal(f.content.querySelector('.live-panel .live-more'), null);
});

test('계정 중지, 계정 삭제 and 비밀번호 재설정 say that a person who is connected now loses the connection', async t => {
  const { user, member, inventory } = permissionFixture();
  const monitor = { sessions: [session(1, 'pve-101', member.username)] };
  const f = await loadAdmin(t, 'f3-users', user, inventory, { monitor, hash: '#/users/' + encodeURIComponent(member.username) });
  const bar = () => f.content.querySelector('#detail .win-foot');
  for (const [label, title] of [['계정 중지', '계정 중지'], ['삭제', '계정 삭제'], ['비밀번호 재설정', '비밀번호 재설정']]) {
    f.click(bar(), label);
    assert.equal(document.querySelector('#modal-title').textContent, title);
    assert.equal(f.dialog.querySelector('.confirm-who').textContent, '사용자 님이 지금 접속 중이에요. 연결이 끊겨요.', label);
    f.click(f.dialog, '취소');
  }
  // Nobody connected: nothing is claimed.
  monitor.sessions = []; document.querySelector('#refresh').dispatchEvent(new f.window.Event('click')); await f.settle();
  f.click(bar(), '계정 중지'); assert.equal(f.dialog.querySelector('.confirm-who'), null);
  assert.match(f.dialog.querySelector('.modal-description').textContent, /사용자 계정을 중지하고 기존 로그인을 해제해요/);
  assert.equal(document.querySelector('#modal-save').textContent, '계정 중지'); assert.equal(document.querySelector('#modal-save').className, 'danger');
  f.click(f.dialog, '취소');
});

test('answers that differ only in the moment they were made do not redraw the page; a real change does', async t => {
  t.mock.timers.enable({ apis: ['setInterval', 'Date'], now: Date.parse('2026-11-12T03:00:00Z') });
  const { user, member, inventory } = permissionFixture();
  const monitor = { sessions: [] };
  const f = await loadAdmin(t, 'f3-poll', user, inventory, { monitor, hash: '#/users' });
  const poll = async () => { t.mock.timers.tick(5000); await f.settle(); t.mock.timers.tick(5000); await f.settle(); };
  const row = () => f.content.querySelector('.account-table tbody tr.is-pick');
  const before = row(), reads = () => f.requests.filter(url => url === '/api/v1/admin/sessions').length, start = reads();
  await poll();
  assert.ok(reads() > start, 'the poll read the connections again (a new checkedAt each time)');
  assert.equal(row(), before, 'the same nodes: a selection in them is not cleared');
  monitor.sessions = [session(1, 'pve-101', member.username)];
  await poll();
  assert.notEqual(row(), before); assert.match(f.content.querySelector('.account-list .list-count').textContent, /접속 중 1/);
  // With nothing changed the page is still drawn again within about a minute, so its relative times move on.
  const later = row(); t.mock.timers.tick(65000); await f.settle(); t.mock.timers.tick(10000); await f.settle();
  assert.notEqual(row(), later);
});
