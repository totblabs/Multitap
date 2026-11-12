// 공지 in the administrator console (admin/js/notices.js): the page's states,
// writing and sending, the history, the two 공지 보내기 entries, and 먼저 알리기
// in the confirmation of each operation that ends connections.
// DOM behaviour against scripted answers (admin-ui-harness.mjs): not a browser
// layout test (the renders are) and not the real broker (notices.test.mjs is).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseHTML } from 'linkedom';
import { loadAdmin, ownerFixture, registerFixtureDesktops } from './admin-ui-harness.mjs';
import { el } from '../admin/js/dom.js';
import { announceFirst, announcements, announceWaits, announceMinimumMs, everyoneAudience } from '../admin/js/notices.js';
import { render as renderServerAccess } from '../admin/js/server-access.js';

process.env.TZ = 'Asia/Seoul';
const answer = (value, status = 200) => ({ ok: status < 400, status, json: async () => value });
const limits = { title: 60, body: 500, bodyLines: 12, generalHours: 24, maintenanceDays: 30, sendsPerMinute: 10, retentionDays: 180 };
const login = (account, platform = 'windows', connections = 0) => ({ account, platform, version: platform === 'windows' ? '1.0.44' : '1.0.31', since: '2026-11-10T00:10:00.000Z', connections });
const connection = (account, desktopId, desktopName) => ({ account, desktopId, desktopName, mode: 'stream', since: '2026-11-10T00:20:00.000Z' });
const audienceOf = (logins = [], connections = []) => ({ people: new Set(logins.map(row => row.account)).size, apps: logins.length, logins, connections });
const busy = () => audienceOf([login('김민지', 'windows', 1), login('lee.junho', 'android')], [connection('김민지', 'pve-101', '디자인 1')]);
const sentNotice = (change = {}) => ({ id: 'a'.repeat(32), kind: 'general', title: '오늘 오후 6시 서버 점검 안내', body: '저장하고 연결을 끊어 주세요.', target: { kind: 'all', label: '' }, source: 'manual',
  sentAt: '2026-11-10T03:00:00.000Z', expiresAt: '2099-01-01T00:00:00.000Z', scheduledAt: null, withdrawnAt: null, by: 'owner', addressed: 2, apps: 3, people: 2, seen: 1, active: true, ...change });
const listOf = (audience = audienceOf(), notices = []) => ({ protocol: 'notice-v1', limits, audience, notices });

function world() {
  const user = { id: 'owner', username: 'owner', role: 'admin', mustChangeCredentials: false };
  const inventory = { users: [{ ...user, disabled: 0, notes: '' }, { id: 'u1', username: '김민지', role: 'user', disabled: 0, notes: '' }, { id: 'u2', username: 'lee.junho', role: 'user', disabled: 0, notes: '' }, { id: 'u3', username: '퇴사자', role: 'user', disabled: 1, notes: '' }], assignments: [{ username: '김민지', desktop_id: 'pve-101' }], audit: [], proxmox: { configured: true, state: 'ok' },
    desktops: [{ id: 'pve-101', kind: 'vm', pve_vmid: 101, name: '디자인 1', pve_node: 'n', pve_available: 1, pve_power_status: 'running', host: { connected: true, version: '1.0.52' }, connectionModes: { stream: true } },
      { id: 'pve-102', kind: 'vm', pve_vmid: 102, name: '개발 1', pve_node: 'n', pve_available: 1, pve_power_status: 'running', host: { connected: true, version: '1.0.52' }, connectionModes: { stream: true } }] };
  registerFixtureDesktops(inventory);
  return { user, inventory };
}
// The console with a scripted /notices: `state.list` is what GET answers.
async function open(t, name, { list = listOf(), hash = '#/notices', monitor = {}, respond = null, inventory = null, deploymentKind } = {}) {
  const w = world(), state = { list, sendError: null, sends: [], withdrawn: [] };
  const f = await loadAdmin(t, 'notices-' + name, w.user, inventory ?? w.inventory, { hash, monitor, deploymentKind, respond: async (url, options = {}) => {
    const given = respond ? await respond(url, options, state) : null;
    if (given) return given;
    const path = url.replace('/api/v1/admin', ''), method = options.method ?? 'GET';
    if (path === '/notices' && method === 'GET') return state.list instanceof Promise ? state.list : state.list?.error ? answer({ error: { code: state.list.error } }, 500) : answer(state.list);
    if (path === '/notices' && method === 'POST') {
      const input = JSON.parse(options.body); state.sends.push(input);
      if (state.sendError) return answer({ error: { code: state.sendError } }, state.sendError === 'too_many_notices' ? 429 : 409);
      const notice = sentNotice({ id: String(state.sends.length).padStart(32, 'b'), kind: input.operation ? 'maintenance' : input.kind, title: input.title, body: input.body, source: input.operation ? 'operation' : 'manual', addressed: 2, apps: 0, people: 0, seen: 0 });
      state.list = listOf(state.list.audience, [notice, ...state.list.notices]);
      return answer({ notice }, 201);
    }
    const withdraw = /^\/notices\/([a-f0-9]{32})\/withdraw$/.exec(path);
    if (withdraw) { state.withdrawn.push(withdraw[1]); state.list = listOf(state.list.audience, state.list.notices.map(notice => notice.id === withdraw[1] ? { ...notice, active: false, withdrawnAt: '2026-11-10T04:00:00.000Z' } : notice)); return answer({ notice: {} }); }
    return null;
  } });
  await f.settle();
  const type = (node, value) => { node.value = value; node.dispatchEvent(new f.window.Event('input')); };
  const choose = (node, value) => { for (const option of node.options) { if (option.value === value) option.setAttribute('selected', ''); else option.removeAttribute('selected'); } node.dispatchEvent(new f.window.Event('change')); };
  const submitModal = async () => { await document.querySelector('#modal-form').onsubmit({ preventDefault() {} }); await f.settle(); };
  // 공지 쓰기 is a window of the page: reading the form opens it with the page's ‘공지 쓰기’ button when it is not open.
  const compose = () => { if (!f.content.querySelector('.notice-compose')) f.content.querySelector('[data-focus=notice-write]')?.dispatchEvent(new f.window.Event('click')); return f.content.querySelector('.notice-compose'); };
  const field = key => { if (/^notice-(?!write)/.test(key)) compose(); return f.content.querySelector(`[data-focus="${key}"]`); };
  const toast = () => document.querySelector('#notice .notice-text')?.textContent ?? '';
  const writesTo = part => f.writes.filter(write => write.path.includes(part));
  return { ...f, state, type, choose, submitModal, compose, field, toast, writesTo };
}

test('공지 is a sidebar entry of the daily group; an empty server says who can receive and that nothing was sent', async t => {
  const f = await open(t, 'empty');
  const group = document.querySelector('#sidebar .nav-group');
  assert.deepEqual([...group.querySelectorAll('[data-page]')].map(node => node.dataset.page), ['overview', 'desktops', 'users', 'log', 'monitoring', 'notices']);
  assert.equal(document.querySelector('#sidebar [data-page=notices]').getAttribute('aria-current'), 'page');
  assert.equal(f.crumb(), '공지'); assert.equal(document.title, '공지 · Multitap 관리자 페이지');
  assert.equal(f.content.querySelector('h1').textContent, '공지');
  assert.equal(f.content.querySelector('.page-description'), null, 'the page name says what it is');
  assert.deepEqual([...f.content.querySelectorAll('.panel h2')].map(node => node.firstChild.textContent), ['보낸 공지', '지금 받을 수 있는 사람']);
  // Writing is a window of the page, opened with ‘공지 쓰기’ in the page header.
  assert.equal(f.content.querySelector('.notice-compose'), null, 'the form is not on the page before it is asked for');
  assert.equal(f.content.querySelector('.page-header [data-focus=notice-write]').textContent, '공지 쓰기');
  assert.equal(f.compose().querySelector('.win-head h2').textContent, '공지 쓰기'); assert.equal(f.content.querySelector('#detail').hasAttribute('open'), true);
  assert.match(f.content.querySelector('.notice-audience').textContent, /지금 로그인한 사람이 없어요보내 두면 보이는 동안 로그인하는 사람이 받아요\./);
  assert.match(f.content.querySelector('.notice-history').textContent, /아직 보낸 공지가 없어요/);
  assert.equal(f.content.querySelector('.notice-recipients').textContent, '지금 로그인한 사람은 없어요.\n24시간 안에 로그인하는 사람도 받아요.');
  assert.equal(f.content.querySelector('.notice-when').hidden, true, '일반 has no time');
  assert.match(f.compose().textContent, /글자만 보내요\. 링크는 눌러지지 않고 글자로 보여요\./);
  assert.deepEqual(f.writes, []);
});

test('while the answer is on its way the page says so, and a failed read offers a retry', async t => {
  let release; const pending = new Promise(resolve => { release = resolve; });
  const f = await open(t, 'loading', { list: pending });
  assert.match(f.content.querySelector('.notice-audience').textContent, /지금 로그인한 사람을 확인하고 있어요/);
  assert.match(f.content.querySelector('.notice-history').textContent, /보낸 공지를 불러오고 있어요/);
  const sendButton = [...f.compose().querySelectorAll('button')].find(node => node.textContent === '보내기');
  assert.equal(sendButton.disabled, true); assert.equal(sendButton.title, '받을 수 있는 사람을 확인하는 중이에요.');
  release(answer(listOf(busy(), [sentNotice()]))); await f.settle();
  assert.equal(f.content.querySelectorAll('.notice-person').length, 2);
  assert.equal([...f.compose().querySelectorAll('button')].find(node => node.textContent === '보내기').disabled, false);

  const failed = await open(t, 'failed', { list: { error: 'internal_error' } });
  assert.equal(failed.content.querySelectorAll('.notice-audience [role=alert],.notice-history [role=alert]').length, 2);
  assert.match(failed.content.querySelector('.notice-audience').textContent, /불러오지 못했어요/);
  failed.state.list = listOf(busy());
  failed.click(failed.content.querySelector('.notice-audience'), '다시 시도'); await failed.settle();
  assert.equal(failed.content.querySelectorAll('.notice-person').length, 2);
});

test('the list beside the form names each sign-in with its app and desktop; the history shows who, to whom, how many apps and people', async t => {
  const ended = sentNotice({ id: 'c'.repeat(32), title: '<b>어제 공지</b>', body: 'https://example.invalid 은 글자예요', target: { kind: 'user', label: '김민지' }, active: false, expiresAt: '2026-11-09T03:00:00.000Z', apps: 1, people: 1, seen: 0 });
  const planned = sentNotice({ id: 'd'.repeat(32), kind: 'maintenance', title: '디자인 1 데스크톱을 다시 시작해요', body: '1분 뒤 연결이 끊겨요.', target: { kind: 'desktop', label: '디자인 1' }, source: 'operation', scheduledAt: '2099-01-01T00:00:00.000Z' });
  const f = await open(t, 'filled', { list: listOf(busy(), [sentNotice(), planned, ended]) });
  const people = [...f.content.querySelectorAll('.notice-person')].map(node => [node.querySelector('strong').textContent, node.querySelector('small').textContent, node.querySelector('.notice-where').textContent]);
  assert.equal(f.content.querySelector('.notice-audience .pill,.notice-history .pill'), null, 'where each one is, the kind and the state are plain words');
  assert.deepEqual(people, [['김민지', 'Windows 1.0.44 · 09:10부터', '디자인 1'], ['lee.junho', 'Android 1.0.31 · 09:10부터', '목록 화면']]);
  assert.equal(f.content.querySelector('.notice-audience .count').textContent, '2');
  assert.equal(f.compose().querySelector('.notice-recipients').textContent, '지금 로그인한 2명이 바로 받아요.\n24시간 안에 로그인하는 사람도 받아요.');
  const rows = [...f.content.querySelectorAll('.notice-table tbody tr')].map(row => [...row.querySelectorAll('td')].map(cell => cell.textContent));
  assert.deepEqual(rows.map(row => [row[1], row[2], row[4], row[5]]), [['owner', '로그인한 모든 사람', '3개', '1명받은 2명 중'], ['owner', '디자인 1먼저 알리기', '3개', '1명받은 2명 중'], ['owner', '김민지', '1개', '0명받은 1명 중']]);
  assert.match(rows[0][0], /^2026\. 11\. 10\. 12:00$/); assert.match(rows[0][3], /^일반오늘 오후 6시 서버 점검 안내저장하고 연결을 끊어 주세요\.$/);
  assert.match(rows[1][3], /^점검 예고2099\. 1\. 1\. 09:00 예정/); assert.match(rows[0][6], /^보이는 중.*까지그만 보이기$/); assert.match(rows[2][6], /^끝남2026\. 11\. 9\. 12:00$/);
  // A notice is text: markup and addresses in it stay characters.
  assert.equal(f.content.querySelector('.notice-table b'), null); assert.equal(f.content.querySelector('.notice-table a'), null);
  assert.ok(rows[2][3].includes('<b>어제 공지</b>') && rows[2][3].includes('https://example.invalid'));
  assert.deepEqual([...f.content.querySelectorAll('.notice-table th')].map(node => node.textContent), ['보낸 때', '보낸 사람', '받는 사람', '공지', '받은 앱', '본 사람', '상태', '관리']);
  assert.deepEqual(rows.map(row => row[7]), ['상세', '상세', '상세'], 'each sent notice opens in its window');
  assert.equal(f.content.querySelector('.notice-history .panel-title p').textContent, '기록은 180일 동안 남아요.', 'the retention only; what a number counts is the title of its cell');
  assert.equal(f.content.querySelector('.notice-table .notice-number').title, '이 공지를 받은 앱(로그인) 수예요.');
  assert.deepEqual([...f.content.querySelectorAll('.notice-table .notice-state .state-dot')].map(node => [node.textContent, node.className]), [['보이는 중', 'state-dot is-green'], ['보이는 중', 'state-dot is-green'], ['끝남', 'state-dot']]);
});

test('writing: the title is required, the counters follow, and a send is confirmed once with exactly what was written', async t => {
  const f = await open(t, 'send', { list: listOf(busy()) });
  f.click(f.compose(), '보내기');
  assert.equal(f.compose().querySelector('.notice-error').textContent, '제목을 적어 주세요.'); assert.equal(f.dialog.open, undefined);
  assert.equal(f.focused(), f.field('notice-title'), 'the field that needs attention is focused');
  f.type(f.field('notice-title'), '  오늘 오후 6시   서버 점검 안내 ');
  assert.equal(f.compose().querySelector('.notice-error').textContent, '', 'typing clears the message');
  f.type(f.field('notice-body'), '저장하고 연결을 끊어 주세요.\nhttps://example.invalid/guide');
  assert.deepEqual([...f.compose().querySelectorAll('.notice-count')].map(node => node.textContent), ['22/60', '46/500'], 'what is typed is counted, in characters');
  f.click(f.compose(), '보내기'); await f.settle();
  assert.equal(f.dialog.open, true); assert.equal(document.querySelector('#modal-title').textContent, '공지를 보낼까요?');
  assert.equal(f.dialog.querySelector('.confirm-who').textContent, '받는 사람: 로그인한 모든 사람');
  assert.equal(f.dialog.querySelector('.notice-preview strong').textContent, '오늘 오후 6시 서버 점검 안내');
  assert.equal(f.dialog.querySelector('.notice-preview p').textContent, '저장하고 연결을 끊어 주세요.\nhttps://example.invalid/guide');
  assert.equal(f.dialog.querySelector('.notice-preview a'), null, 'an address is shown as text');
  assert.match(f.dialog.textContent, /지금 로그인한 2명이 바로 받아요\.\n24시간 안에 로그인하는 사람도 받아요\.\n보낸 뒤에는 고칠 수 없어요\. 그만 보이게 할 수는 있어요\./);
  assert.equal(document.querySelector('#modal-save').textContent, '보내기'); assert.deepEqual(f.state.sends, [], 'nothing is sent before the confirmation');
  f.click(f.dialog, '취소'); assert.deepEqual(f.state.sends, []);
  assert.equal(f.field('notice-title').value, '  오늘 오후 6시   서버 점검 안내 ', 'cancelling keeps what was written');
  f.click(f.compose(), '보내기'); await f.settle(); await f.submitModal();
  assert.deepEqual(f.state.sends, [{ kind: 'general', title: '오늘 오후 6시 서버 점검 안내', body: '저장하고 연결을 끊어 주세요.\nhttps://example.invalid/guide', target: { kind: 'all' } }]);
  assert.equal(f.dialog.open, false); assert.equal(f.toast(), '공지를 보냈어요.');
  assert.equal(f.field('notice-title').value, ''); assert.equal(f.field('notice-body').value, '', 'the form is empty for the next one');
  assert.equal(f.content.querySelectorAll('.notice-table tbody tr').length, 1, 'the history shows it at once');
  // A refusal is said in the dialog and keeps the text.
  f.type(f.field('notice-title'), '둘째 공지'); f.state.sendError = 'too_many_notices';
  f.click(f.compose(), '보내기'); await f.settle(); await f.submitModal();
  assert.equal(f.dialog.open, true); assert.equal(document.querySelector('#modal-error').textContent, '공지를 너무 자주 보냈어요.\n1분 뒤에 다시 보내 주세요.');
  f.click(f.dialog, '취소'); assert.equal(f.field('notice-title').value, '둘째 공지');
});

test('one person or one desktop: the choice names who gets it now; a desktop nobody uses cannot be chosen', async t => {
  const f = await open(t, 'targets', { list: listOf(busy()) });
  const segment = (label, key) => [...f.compose().querySelectorAll(`[aria-label="${label}"] [data-segment]`)].find(node => node.getAttribute('data-segment') === key);
  assert.deepEqual([...f.compose().querySelectorAll('[aria-label="받는 사람"] .segment')].map(node => node.textContent), ['로그인한 모든 사람', '한 사람', '한 데스크톱']);
  segment('받는 사람', 'user').dispatchEvent(new f.window.Event('click'));
  assert.equal(segment('받는 사람', 'user').getAttribute('aria-pressed'), 'true');
  const people = f.field('notice-user');
  assert.deepEqual([...people.options].map(option => option.textContent), ['사람 고르기', '김민지 · 로그인 중', 'lee.junho · 로그인 중', 'owner'], 'a stopped account is not offered');
  assert.equal(f.content.querySelector('.notice-recipients').textContent, '받을 사람을 골라 주세요.');
  f.type(f.field('notice-title'), '개인 공지'); f.click(f.compose(), '보내기');
  assert.equal(f.compose().querySelector('.notice-error').textContent, '받을 사람을 골라 주세요.'); assert.equal(f.dialog.open, undefined);
  f.choose(people, 'owner');
  assert.equal(f.content.querySelector('.notice-recipients').textContent, 'owner 님은 지금 로그인하지 않았어요.\n다음에 로그인하면 받아요.');
  f.choose(f.field('notice-user'), '김민지');
  assert.equal(f.content.querySelector('.notice-recipients').textContent, '김민지 님이 바로 받아요.');
  f.click(f.compose(), '보내기'); await f.settle(); await f.submitModal();
  assert.deepEqual(f.state.sends.at(-1), { kind: 'general', title: '개인 공지', body: '', target: { kind: 'user', username: '김민지' } });
  // 한 데스크톱: only the desktops somebody is connected to.
  segment('받는 사람', 'desktop').dispatchEvent(new f.window.Event('click'));
  assert.deepEqual([...f.field('notice-desktop').options].map(option => option.textContent), ['데스크톱 고르기', '디자인 1 · 1명 접속 중']);
  f.choose(f.field('notice-desktop'), 'pve-101');
  assert.equal(f.content.querySelector('.notice-recipients').textContent, '바로 받는 사람: 디자인 1에 접속 중인 김민지 님');
  f.type(f.field('notice-title'), '데스크톱 공지'); f.click(f.compose(), '보내기'); await f.settle(); await f.submitModal();
  assert.deepEqual(f.state.sends.at(-1).target, { kind: 'desktop', desktopIds: ['pve-101'] });
  // 이 사람에게 in the list addresses the form to that person.
  f.click(f.content.querySelectorAll('.notice-person')[1], '이 사람에게');
  assert.equal(segment('받는 사람', 'user').getAttribute('aria-pressed'), 'true'); assert.equal(f.content.querySelector('.notice-recipients').textContent, 'lee.junho 님이 바로 받아요.');

  const quiet = await open(t, 'targets-quiet', { list: listOf(audienceOf([login('lee.junho', 'android')])) });
  [...quiet.compose().querySelectorAll('[aria-label="받는 사람"] [data-segment]')].find(node => node.getAttribute('data-segment') === 'desktop').dispatchEvent(new quiet.window.Event('click'));
  assert.equal(quiet.field('notice-desktop'), null);
  assert.equal(quiet.content.querySelector('.notice-recipients').textContent, '지금 접속 중인 데스크톱이 없어요.\n접속 중인 사람이 있을 때 보낼 수 있어요.');
});

test('점검 예고 asks for its time, refuses a past one and sends it as the stated time', async t => {
  const f = await open(t, 'maintenance', { list: listOf(busy()) });
  [...f.compose().querySelectorAll('[aria-label="종류"] [data-segment]')].find(node => node.getAttribute('data-segment') === 'maintenance').dispatchEvent(new f.window.Event('click'));
  assert.equal(f.content.querySelector('.notice-when').hidden, false);
  assert.match(f.field('notice-when').value, /^\d{4}-\d\d-\d\dT\d\d:00$/, 'the next full hour is proposed');
  assert.equal(f.content.querySelector('.notice-recipients').textContent, '지금 로그인한 2명이 바로 받아요.\n점검 시각까지 로그인하는 사람도 받아요.');
  f.type(f.field('notice-title'), '서버 점검');
  f.type(f.field('notice-when'), '2020-01-01T10:00'); f.click(f.compose(), '보내기');
  assert.equal(f.compose().querySelector('.notice-error').textContent, '점검 시각을 지금보다 뒤로 정해 주세요.');
  f.type(f.field('notice-when'), '2199-01-01T10:00'); f.click(f.compose(), '보내기');
  assert.equal(f.compose().querySelector('.notice-error').textContent, '점검 시각은 30일 안으로 정해 주세요.');
  const at = new Date(Date.now() + 3 * 3600000); at.setSeconds(0, 0);
  const pad = value => String(value).padStart(2, '0');
  f.type(f.field('notice-when'), `${at.getFullYear()}-${pad(at.getMonth() + 1)}-${pad(at.getDate())}T${pad(at.getHours())}:${pad(at.getMinutes())}`);
  f.click(f.compose(), '보내기'); await f.settle();
  assert.match(f.dialog.querySelector('.notice-preview-head').textContent, /^점검 예고(오늘|내일) \d\d:\d\d 예정$/);
  await f.submitModal();
  assert.deepEqual(f.state.sends, [{ kind: 'maintenance', title: '서버 점검', body: '', target: { kind: 'all' }, scheduledAt: at.toISOString() }]);
});

test('the form being written survives a redraw of the page: the same fields, the same text', async t => {
  const f = await open(t, 'stable', { list: listOf(busy()) });
  const title = f.field('notice-title'), body = f.field('notice-body');
  f.type(title, '쓰는 중인 제목'); f.type(body, '쓰는 중인 내용');
  // Somebody signs in: the list changes and the page is drawn again.
  f.state.list = listOf(audienceOf([...busy().logins, login('새사람')], busy().connections));
  document.querySelector('#refresh').dispatchEvent(new f.window.Event('click')); await f.settle();
  assert.equal(f.content.querySelectorAll('.notice-person').length, 3, 'the list was redrawn');
  assert.equal(f.field('notice-title'), title, 'the very same input node'); assert.equal(f.field('notice-body'), body);
  assert.equal(title.value, '쓰는 중인 제목'); assert.equal(body.value, '쓰는 중인 내용');
  assert.equal(f.content.querySelector('.notice-recipients').textContent, '지금 로그인한 3명이 바로 받아요.\n24시간 안에 로그인하는 사람도 받아요.');
});

test('그만 보이기 asks once, withdraws that notice and the history says so', async t => {
  const f = await open(t, 'withdraw', { list: listOf(busy(), [sentNotice()]) });
  f.click(f.content.querySelector('.notice-table'), '그만 보이기'); await f.settle();
  assert.equal(document.querySelector('#modal-title').textContent, '이 공지를 그만 보일까요?');
  assert.equal(document.querySelector('.modal-description').textContent, '받은 사람의 화면과 목록에서 바로 사라져요.\n보낸 기록은 남아요.');
  assert.equal(document.querySelector('#modal-save').className, 'danger'); assert.deepEqual(f.state.withdrawn, []);
  await f.submitModal();
  assert.deepEqual(f.state.withdrawn, ['a'.repeat(32)]); assert.equal(f.toast(), '공지를 그만 보이게 했어요.');
  assert.match(f.content.querySelector('.notice-table tbody tr').textContent, /그만 보임/);
  assert.deepEqual([...f.content.querySelectorAll('.notice-table button')].map(node => node.textContent), ['상세'], 'nothing is left to withdraw; the row still opens its window');
});

test('공지 보내기 in the window of a user and of a desktop in use opens the page already addressed', async t => {
  const f = await open(t, 'entries', { list: listOf(busy()), hash: '#/users/' + encodeURIComponent('김민지'), monitor: { sessions: [{ sessionId: 's1', desktopId: 'pve-101', account: '김민지', mode: 'stream', attached: true, ready: true }] } });
  f.click(f.content.querySelector('.user-detail .win-foot'), '공지 보내기'); await f.settle();
  assert.equal(f.crumb(), '공지');
  assert.equal(f.content.querySelector('.notice-recipients').textContent, '김민지 님이 바로 받아요.');
  assert.equal([...f.field('notice-user').options].find(option => option.hasAttribute('selected')).value, '김민지');
  // A stopped account has no such entry.
  f.navigate('users'); f.follow(f.content, '퇴사자'); await f.settle();
  assert.equal(f.content.querySelector('.user-detail .account-head h2').firstChild.textContent, '퇴사자'); assert.equal([...f.content.querySelectorAll('.user-detail button')].some(node => node.textContent === '공지 보내기'), false);
  // The desktop somebody is connected to offers it beside its power actions; an idle one does not.
  f.navigate('desktops'); await f.settle(); f.pickVm('pve-102'); await f.settle();
  assert.equal([...f.content.querySelectorAll('.dt-main button')].some(node => node.textContent === '공지 보내기'), false);
  f.pickVm('pve-101'); await f.settle();
  f.click(f.content.querySelector('.dt-main'), '공지 보내기'); await f.settle();
  assert.equal(f.crumb(), '공지');
  assert.equal(f.content.querySelector('.notice-recipients').textContent, '바로 받는 사람: 디자인 1에 접속 중인 김민지 님');
});

// ---- 먼저 알리기 -------------------------------------------------------------
// The step alone, with a clock the test moves.
function step(t, { people = 2, refuse = null } = {}) {
  const window = parseHTML('<!doctype html><html><body><div id="fields"></div></body></html>');
  const previous = Object.fromEntries(['document', 'Node'].map(key => [key, Object.getOwnPropertyDescriptor(globalThis, key)]));
  Object.defineProperty(globalThis, 'document', { configurable: true, value: window.document });
  Object.defineProperty(globalThis, 'Node', { configurable: true, value: window.Node });
  t.after(() => { for (const [key, descriptor] of Object.entries(previous)) { if (descriptor) Object.defineProperty(globalThis, key, descriptor); else delete globalThis[key]; } });
  t.mock.timers.enable({ apis: ['setInterval', 'Date'], now: Date.parse('2026-11-10T09:00:00Z') });
  const calls = [];
  const ctx = { api: async (path, method, data) => {
    calls.push({ path, method, data });
    if (path === '/notices') { if (refuse) throw Object.assign(new Error(refuse), { code: refuse }); return { notice: { id: 'e'.repeat(32), addressed: people } }; }
    return { notice: {} };
  } };
  const made = announceFirst(ctx, { audience: people === null ? null : { target: { kind: 'all' }, people }, text: announcements.serverUpdate });
  if (made) window.document.querySelector('#fields').append(made.node);
  const tick = async ms => { t.mock.timers.tick(ms); for (let i = 0; i < 5; i++) await Promise.resolve(); };
  const flush = async () => { for (let i = 0; i < 10; i++) await Promise.resolve(); };
  const toggle = on => { const box = made.node.querySelector('[data-announce=toggle]'); box.checked = on; box.dispatchEvent(new window.Event('change')); };
  const wait = seconds => made.node.querySelector(`[data-segment="${seconds}"]`).dispatchEvent(new window.Event('click'));
  return { window, made, calls, tick, flush, toggle, wait, node: made?.node };
}

test('먼저 알리기 is not offered when nobody is connected; unticked, the operation runs untouched', async t => {
  assert.equal(step(t, { people: null }).made, null);
  assert.equal(announceFirst({}, { audience: { target: { kind: 'all' }, people: 0 }, text: announcements.serverUpdate }), null);
  assert.equal(everyoneAudience({ value: () => ({ audience: audienceOf() }) }), null);
  assert.deepEqual(everyoneAudience({ value: () => ({ audience: busy() }) }), { target: { kind: 'all' }, people: 2 });
  assert.equal(everyoneAudience({ value: () => null }), null, 'an unknown audience offers nothing');
  assert.deepEqual(announceWaits, [[0, '지금 바로'], [60, '1분'], [300, '5분']]);
});

test('먼저 알리기: ticked, it sends the prepared notice, counts down and then runs the operation once', async t => {
  const f = step(t), ran = [];
  assert.equal(f.node.querySelector('.announce-toggle').textContent, '먼저 알리기접속 중인 사람에게 공지를 보내고 기다린 뒤 진행해요.');
  assert.equal(f.node.querySelector('.announce-options').hidden, true); assert.equal(f.node.querySelector('.announce-countdown').hidden, true);
  // Unticked: the action itself, with its input, and no request.
  await f.made.wrap(async input => ran.push(input))({ password: 'x' });
  assert.deepEqual([ran, f.calls], [[{ password: 'x' }], []]);
  f.toggle(true);
  assert.equal(f.node.querySelector('.announce-options').hidden, false);
  assert.deepEqual([...f.node.querySelectorAll('.announce-waits .segment')].map(node => [node.textContent, node.getAttribute('aria-pressed')]), [['지금 바로', 'false'], ['1분', 'true'], ['5분', 'false']]);
  assert.equal(f.node.querySelector('.announce-preview strong').textContent, '서버를 업데이트해요');
  assert.equal(f.node.querySelector('.announce-preview p').textContent, '1분 뒤 연결이 잠시 끊겨요.\n하던 작업을 저장해 주세요.\n끝나면 다시 연결할 수 있어요.');
  assert.match(f.node.querySelector('.announce-options .field-hint').textContent, /접속 중인 2명에게 보내요\. 기다리는 동안 취소할 수 있어요\./);
  f.wait(300);
  assert.match(f.node.querySelector('.announce-preview p').textContent, /^5분 뒤 연결이 잠시 끊겨요\./);
  f.wait(60);
  const pending = f.made.wrap(async input => ran.push(input))({ n: 2 });
  await f.flush();
  assert.deepEqual(f.calls, [{ path: '/notices', method: 'POST', data: { title: '서버를 업데이트해요', body: '1분 뒤 연결이 잠시 끊겨요.\n하던 작업을 저장해 주세요.\n끝나면 다시 연결할 수 있어요.', target: { kind: 'all' }, operation: { waitSeconds: 60 } } }]);
  const countdown = f.node.querySelector('.announce-countdown');
  assert.equal(countdown.hidden, false); assert.equal(countdown.getAttribute('role'), 'status');
  assert.equal(countdown.querySelector('strong').textContent, '2명에게 알렸어요.');
  assert.equal(countdown.querySelector('.announce-remaining').textContent, '1:00 뒤에 서버를 업데이트해요.');
  assert.equal(f.node.querySelector('.announce-options').hidden, true); assert.equal(f.node.querySelector('[data-announce=toggle]').disabled, true);
  await f.tick(30500);
  assert.equal(countdown.querySelector('.announce-remaining').textContent, '0:30 뒤에 서버를 업데이트해요.');
  assert.equal(ran.length, 1, 'still waiting');
  await f.tick(29000); assert.equal(ran.length, 1);
  await f.tick(1000); await pending;
  assert.deepEqual(ran, [{ password: 'x' }, { n: 2 }], 'the operation ran once, with its input, after the wait');
  assert.equal(countdown.hidden, true);
  assert.equal(f.calls.length, 1, 'no withdrawal when it ran');
});

test('먼저 알리기: 지금 바로 still gives the apps a moment; 취소 ends it before the operation and withdraws the notice', async t => {
  const f = step(t), ran = [];
  f.toggle(true); f.wait(0);
  assert.match(f.node.querySelector('.announce-preview p').textContent, /^곧 연결이 잠시 끊겨요\./);
  const now = f.made.wrap(async () => ran.push('now'))({});
  await f.flush();
  assert.deepEqual(f.calls[0].data.operation, { waitSeconds: 0 });
  assert.equal(f.node.querySelector('.announce-remaining').textContent, '0:03 뒤에 서버를 업데이트해요.');
  await f.tick(announceMinimumMs - 500); assert.deepEqual(ran, []);
  await f.tick(500); await now; assert.deepEqual(ran, ['now']);
  // 취소 while it waits.
  f.wait(300);
  const cancelled = f.made.wrap(async () => ran.push('never'))({});
  await f.flush(); await f.tick(120000);
  assert.equal(f.node.querySelector('.announce-remaining').textContent, '3:00 뒤에 서버를 업데이트해요.');
  f.node.querySelector('[data-announce=cancel]').dispatchEvent(new f.window.Event('click'));
  // 취소 is said in the step itself (plain, not the dialog's red error line); the action still ends without running.
  await assert.rejects(cancelled, error => error.code === 'announce_cancelled' && error.message === '');
  assert.equal(f.node.querySelector('.announce-note').hidden, false); assert.equal(f.node.querySelector('.announce-note').textContent, '취소했어요.\n아무것도 바뀌지 않았고, 보낸 공지는 그만 보이게 했어요.');
  await f.flush();
  assert.deepEqual(ran, ['now'], 'the operation did not run');
  assert.deepEqual(f.calls.at(-1), { path: '/notices/' + 'e'.repeat(32) + '/withdraw', method: 'POST', data: {} });
  assert.equal(f.node.querySelector('.announce-countdown').hidden, true); assert.equal(f.node.querySelector('.announce-options').hidden, false, 'the choice is back');
  await f.tick(600000); assert.deepEqual(ran, ['now'], 'and it never runs later');
});

test('먼저 알리기: nobody left means nothing to wait for; another refusal stops before the operation; a closed dialog runs nothing', async t => {
  const gone = step(t, { refuse: 'notice_no_recipients' }), ran = [];
  gone.toggle(true);
  await gone.made.wrap(async () => ran.push('gone'))({});
  assert.deepEqual(ran, ['gone'], 'everyone left: the operation simply runs');
  assert.equal(gone.node.querySelector('.announce-countdown').hidden, true);
  t.mock.timers.reset();

  const limited = step(t, { refuse: 'too_many_notices' });
  limited.toggle(true);
  await assert.rejects(limited.made.wrap(async () => ran.push('limited'))({}), error => error.code === 'too_many_notices');
  assert.deepEqual(ran, ['gone']);
  t.mock.timers.reset();

  const closed = step(t);
  closed.toggle(true);
  const pending = closed.made.wrap(async () => ran.push('closed'))({});
  await closed.flush();
  closed.node.remove(); // the dialog was closed or replaced (a sign-out)
  await closed.tick(1000);
  await assert.rejects(pending, error => error.code === 'announce_cancelled');
  await closed.tick(120000);
  assert.deepEqual(ran, ['gone']);
});

test('the prepared texts are short, plain and name the wait', () => {
  const texts = [announcements.serverUpdate, announcements.serverAccess, announcements.restore, announcements.hostUpdate, announcements.reboot('디자인 1'), announcements.shutdown('디자인 1')];
  for (const text of texts) {
    assert.ok([...text.title].length <= 60, text.title); assert.match(text.title, /요$/);
    for (const when of ['곧', '1분 뒤', '5분 뒤']) {
      const body = text.body(when);
      assert.ok(body.startsWith(when + ' 연결이'), body); assert.ok(body.split('\n').length <= 3 && [...body].length <= 500, body);
      assert.doesNotMatch(body + text.title, /이 PC|내 PC|VM\b/, 'the reader is told about a 데스크톱 and a 연결');
    }
  }
  assert.equal(announcements.reboot('가'.repeat(80)).title.length <= 60, true, 'a long name is cut');
  assert.equal(announcements.reboot('디자인 1').title, '디자인 1 데스크톱을 다시 시작해요'); assert.equal(announcements.shutdown('디자인 1').title, '디자인 1 데스크톱을 꺼요');
});

// ---- 먼저 알리기 in each confirmation ------------------------------------------
const announceIn = scope => scope.querySelector('.announce-first');
async function tickTo(t, f, ms) { t.mock.timers.tick(ms); await f.settle(); }

test('server update: the option appears only while somebody is signed in, and the update waits for the countdown', async t => {
  const releaseId = 'a'.repeat(64);
  const serverInfo = () => answer({ service: 'multitap-broker', version: '0.8.1', deployment: { kind: 'systemd', configured: true }, update: { state: 'available', feedConfigured: true, applySupported: true, releaseId, latest: { version: '0.8.2', releasedAt: '2026-10-24T10:00:00.000Z', changelog: '변경' } }, checkedAt: '2026-10-24T10:00:00.000Z' });
  const respond = url => url.endsWith('/server-info') ? serverInfo() : url.endsWith('/server-update/apply') ? answer({ accepted: true }, 202) : null;
  const quiet = await open(t, 'update-quiet', { hash: '#/updates', deploymentKind: 'systemd', respond });
  quiet.click(quiet.content, '업데이트'); await quiet.settle();
  assert.equal(quiet.dialog.open, true); assert.equal(announceIn(quiet.dialog), null, 'nobody is signed in: nothing to announce');
  assert.match(quiet.dialog.textContent, /진행 중인 원격 연결은 종료돼요/);
  await quiet.submitModal();
  assert.deepEqual(quiet.writes.map(write => write.path), ['/api/v1/admin/server-update/apply']);

  const f = await open(t, 'update-busy', { hash: '#/updates', deploymentKind: 'systemd', respond, list: listOf(busy()) });
  f.click(f.content, '업데이트'); await f.settle();
  const step = announceIn(f.dialog);
  assert.ok(step, 'somebody is signed in'); assert.notEqual(step.querySelector('[data-announce=toggle]').checked, true, 'off unless chosen');
  // Unticked: exactly the request it always sent.
  await f.submitModal();
  assert.deepEqual(f.writes.map(write => [write.path, write.data]), [['/api/v1/admin/server-update/apply', { releaseId, fromVersion: '0.8.1', confirm: true }]]);
  // Ticked, 1분: the notice first, the update after the countdown.
  f.writes.length = 0;
  f.click(f.content, '업데이트'); await f.settle();
  const box = announceIn(f.dialog).querySelector('[data-announce=toggle]'); box.checked = true; box.dispatchEvent(new f.window.Event('change'));
  assert.equal(announceIn(f.dialog).querySelector('.announce-preview strong').textContent, '서버를 업데이트해요');
  t.mock.timers.enable({ apis: ['setInterval', 'Date'] });
  const submitted = document.querySelector('#modal-form').onsubmit({ preventDefault() {} }); await f.settle();
  assert.deepEqual(f.writes.map(write => [write.path, write.data]), [['/api/v1/admin/notices', { title: '서버를 업데이트해요', body: '1분 뒤 연결이 잠시 끊겨요.\n하던 작업을 저장해 주세요.\n끝나면 다시 연결할 수 있어요.', target: { kind: 'all' }, operation: { waitSeconds: 60 } }]]);
  assert.equal(f.dialog.open, true); assert.equal(announceIn(f.dialog).querySelector('.announce-countdown').hidden, false);
  assert.equal(announceIn(f.dialog).querySelector('.announce-countdown strong').textContent, '2명에게 알렸어요.');
  assert.equal(document.querySelector('#modal-save').disabled, true, 'the dialog cannot be submitted twice while it waits');
  assert.equal(f.dialog.classList.contains('is-announcing'), true, 'its own buttons step aside (notices.css)');
  assert.equal(announceIn(f.dialog).querySelector('[data-announce=cancel]').disabled, false, 'its own 취소 stays usable');
  await tickTo(t, f, 59000); assert.equal(f.writesTo('/server-update/apply').length, 0);
  await tickTo(t, f, 1500); await submitted; await f.settle();
  assert.deepEqual(f.writes.map(write => write.path), ['/api/v1/admin/notices', '/api/v1/admin/server-update/apply']);
  assert.equal(f.dialog.open, false); assert.equal(f.dialog.classList.contains('is-announcing'), false);
});

test('server update: 취소 during the countdown sends no update, withdraws the notice and leaves the dialog open', async t => {
  const releaseId = 'a'.repeat(64);
  const respond = url => url.endsWith('/server-info') ? answer({ service: 'multitap-broker', version: '0.8.1', deployment: { kind: 'systemd', configured: true }, update: { state: 'available', feedConfigured: true, applySupported: true, releaseId, latest: { version: '0.8.2', releasedAt: '2026-10-24T10:00:00.000Z', changelog: '변경' } }, checkedAt: '2026-10-24T10:00:00.000Z' }) : null;
  const f = await open(t, 'update-cancel', { hash: '#/updates', deploymentKind: 'systemd', respond, list: listOf(busy()) });
  f.click(f.content, '업데이트'); await f.settle();
  const box = announceIn(f.dialog).querySelector('[data-announce=toggle]'); box.checked = true; box.dispatchEvent(new f.window.Event('change'));
  announceIn(f.dialog).querySelector('[data-segment="300"]').dispatchEvent(new f.window.Event('click'));
  t.mock.timers.enable({ apis: ['setInterval', 'Date'] });
  const submitted = document.querySelector('#modal-form').onsubmit({ preventDefault() {} }); await f.settle();
  assert.deepEqual(f.state.sends.at(-1).operation, { waitSeconds: 300 });
  await tickTo(t, f, 100000);
  assert.equal(announceIn(f.dialog).querySelector('.announce-remaining').textContent, '3:20 뒤에 서버를 업데이트해요.');
  f.click(announceIn(f.dialog), '취소'); await submitted; await f.settle();
  assert.equal(f.dialog.open, true); assert.equal(document.querySelector('#modal-error').textContent, '', 'a cancel is not an error');
  assert.equal(announceIn(f.dialog).querySelector('.announce-note').hidden, false); assert.equal(announceIn(f.dialog).querySelector('.announce-note').textContent, '취소했어요.\n아무것도 바뀌지 않았고, 보낸 공지는 그만 보이게 했어요.');
  assert.equal(document.querySelector('#modal-save').disabled, false, 'the dialog is usable again'); assert.equal(f.dialog.classList.contains('is-announcing'), false);
  await tickTo(t, f, 400000);
  assert.deepEqual(f.writes.map(write => write.path), ['/api/v1/admin/notices', '/api/v1/admin/notices/' + f.state.sends.length.toString().padStart(32, 'b') + '/withdraw']);
  assert.equal(f.writesTo('/server-update/apply').length, 0);
});

test('desktop restart and shutdown: offered only for a desktop somebody is connected to, addressed to that desktop', async t => {
  const sessions = [{ sessionId: 's1', desktopId: 'pve-101', account: '김민지', mode: 'stream', attached: true, ready: true }];
  const respond = url => url.endsWith('/power') ? answer({ task: 'UPID:fixture', node: 'n' }, 202) : url.endsWith('/proxmox/task') ? answer({ status: 'stopped', exitStatus: 'OK' }) : null;
  const f = await open(t, 'power', { hash: '#/desktops/pve-102', monitor: { sessions }, respond, list: listOf(busy()) });
  f.click(f.content.querySelector('.dt-main'), '다시 시작'); await f.settle();
  assert.equal(f.dialog.open, true); assert.equal(announceIn(f.dialog), null, 'nobody is connected to 개발 1');
  f.click(f.dialog, '취소');
  f.pickVm('pve-101'); await f.settle();
  for (const [label, title, then] of [['다시 시작', '디자인 1 데스크톱을 다시 시작해요', '데스크톱을 다시 시작해요'], ['끄기', '디자인 1 데스크톱을 꺼요', '데스크톱을 꺼요']]) {
    f.click(f.content.querySelector('.dt-main'), label); await f.settle();
    assert.equal(f.dialog.querySelector('.confirm-who').textContent, '김민지 님이 지금 접속 중이에요');
    const step = announceIn(f.dialog); assert.ok(step, label);
    assert.match(step.querySelector('.field-hint').textContent, /접속 중인 1명에게 보내요/);
    const box = step.querySelector('[data-announce=toggle]'); box.checked = true; box.dispatchEvent(new f.window.Event('change'));
    step.querySelector('[data-segment="0"]').dispatchEvent(new f.window.Event('click'));
    assert.equal(step.querySelector('.announce-preview strong').textContent, title);
    f.writes.length = 0;
    t.mock.timers.enable({ apis: ['setInterval', 'Date'] });
    const submitted = document.querySelector('#modal-form').onsubmit({ preventDefault() {} }); await f.settle();
    assert.deepEqual(f.writes.map(write => write.path), ['/api/v1/admin/notices']);
    assert.deepEqual([f.writes[0].data.target, f.writes[0].data.operation, f.writes[0].data.title], [{ kind: 'desktop', desktopIds: ['pve-101'] }, { waitSeconds: 0 }, title]);
    assert.equal(step.querySelector('.announce-remaining').textContent, `0:03 뒤에 ${then}.`);
    await tickTo(t, f, 3100);
    t.mock.timers.reset();
    await submitted.catch(() => {}); await f.settle();
    assert.deepEqual(f.writes.slice(0, 2).map(write => [write.path, write.data?.action]), [['/api/v1/admin/notices', undefined], ['/api/v1/admin/desktops/pve-101/power', label === '다시 시작' ? 'reboot' : 'shutdown']]);
    // The confirmation closes once Proxmox took the request; its task is followed after the dialog
    // (1.5 s to the first look), and until it ends the desktop takes no second power request.
    assert.equal(f.dialog.open, false, 'the dialog does not wait for the Proxmox task');
    assert.equal(f.writes.some(write => write.path.endsWith('/proxmox/task')), false, 'the task is looked at after the dialog closed');
    await new Promise(resolve => setTimeout(resolve, 1700)); await f.settle();
    assert.equal(f.writes.some(write => write.path.endsWith('/proxmox/task')), true);
    if (f.dialog.open) { f.click(f.dialog, '취소'); await f.settle(); }
  }
});

test('Host update: the bulk update and 지금 업데이트 offer it for the desktops in use; waiting for the connection does not', async t => {
  const view = extra => ({ protocol: 'host-update-v1', minimumVersion: '0.2.59', connected: true, supported: true, version: '0.2.59', latestVersion: '0.2.60', newer: true, available: true, sessionActive: false, operation: null, ...extra });
  const w = world();
  w.inventory.hostUpdates = { latest: { version: '0.2.60', filename: 'Multitap-Host-0.2.60-windows-x64.exe', size: 17000000, sha256: 'a'.repeat(64), changelog: '안정성 개선' }, desktops: { 'pve-101': view({ sessionActive: true }), 'pve-102': view() } };
  const sessions = [{ sessionId: 's1', desktopId: 'pve-101', account: '김민지', mode: 'stream', attached: true, ready: true }];
  const respond = url => /\/desktops\/pve-\d+\/host-update$/.test(url) ? answer({ hostUpdate: view({ available: false, operation: { id: 'a7b3a1f4-5f5a-4bb1-8f0a-2e0f1b7c9d10', status: 'dispatched', code: '', fromVersion: '0.2.59', targetVersion: '0.2.60', progress: null } }) }, 202)
    : url.endsWith('/host-updates/all') ? answer({ version: '0.2.60', results: [{ desktopId: 'pve-101', started: true, status: 'dispatched' }, { desktopId: 'pve-102', started: true, status: 'dispatched' }] }, 202) : null;
  const f = await open(t, 'host-update', { hash: '#/desktops', monitor: { sessions }, respond, inventory: w.inventory, list: listOf(busy()) });
  // The bulk update.
  f.change(f.content.querySelector('select[data-focus=desktops-state]'), 'update'); f.click(f.content.querySelector('.query-box'), '조회'); await f.settle();
  f.click(f.content.querySelector('.update-bar'), '2대 모두 업데이트'); await f.settle();
  assert.equal(document.querySelector('#modal-save').textContent, '연결을 끊고 2대 업데이트');
  const step = announceIn(f.dialog); assert.ok(step);
  assert.match(step.querySelector('.field-hint').textContent, /접속 중인 1명에게 보내요/);
  assert.equal(step.querySelector('.announce-preview strong').textContent, '데스크톱 Host를 업데이트해요');
  const box = step.querySelector('[data-announce=toggle]'); box.checked = true; box.dispatchEvent(new f.window.Event('change'));
  step.querySelector('[data-segment="0"]').dispatchEvent(new f.window.Event('click'));
  t.mock.timers.enable({ apis: ['setInterval', 'Date'] });
  // Opening 데스크톱 synchronises Proxmox; only what the dialog sends is looked at.
  const sent = () => f.writes.filter(write => !write.path.includes('/proxmox/'));
  const submitted = document.querySelector('#modal-form').onsubmit({ preventDefault() {} }); await f.settle();
  assert.deepEqual(sent().map(write => [write.path, write.data.target ?? null]), [['/api/v1/admin/notices', { kind: 'desktop', desktopIds: ['pve-101'] }]], 'only the desktop in use is addressed');
  await tickTo(t, f, 3100); await submitted; await f.settle();
  assert.deepEqual(sent().map(write => write.path), ['/api/v1/admin/notices', '/api/v1/admin/host-updates/all']);
  t.mock.timers.reset();
  // ‘접속 안 한 N대만’ ends no connection: it never announces.
  Object.assign(w.inventory.hostUpdates.desktops, { 'pve-101': view({ sessionActive: true }), 'pve-102': view() }); f.navigate('desktops'); await f.settle();
  f.writes.length = 0;
  // One desktop in use: 지금 업데이트 announces, 연결이 끝나면 업데이트 does not.
  f.pickVm('pve-101'); await f.settle();
  f.click(f.content.querySelector('.dt-main'), 'Host 업데이트'); await f.settle();
  const single = announceIn(f.dialog); assert.ok(single);
  const tick = single.querySelector('[data-announce=toggle]'); tick.checked = true; tick.dispatchEvent(new f.window.Event('change'));
  await f.submitModal();
  assert.deepEqual(sent().map(write => [write.path, write.data]), [['/api/v1/admin/desktops/pve-101/host-update', { version: '0.2.60', disconnectSessions: false }]], 'waiting for the connection sends no notice, ticked or not');
  Object.assign(w.inventory.hostUpdates.desktops, { 'pve-101': view({ sessionActive: true }) }); f.navigate('desktops'); await f.settle(); f.pickVm('pve-101'); await f.settle();
  f.writes.length = 0;
  f.click(f.content.querySelector('.dt-main'), 'Host 업데이트'); await f.settle();
  const again = announceIn(f.dialog).querySelector('[data-announce=toggle]'); again.checked = true; again.dispatchEvent(new f.window.Event('change'));
  announceIn(f.dialog).querySelector('[data-segment="0"]').dispatchEvent(new f.window.Event('click'));
  t.mock.timers.enable({ apis: ['setInterval', 'Date'] });
  f.click(f.dialog, '지금 업데이트(연결 끊김)'); await f.settle();
  assert.deepEqual(sent().map(write => write.path), ['/api/v1/admin/notices']);
  await tickTo(t, f, 3100); await f.settle();
  assert.deepEqual(sent().map(write => [write.path, write.data.disconnectSessions ?? null]), [['/api/v1/admin/notices', null], ['/api/v1/admin/desktops/pve-101/host-update', true]]);
  // A desktop nobody uses: its dialog has no such step.
  t.mock.timers.reset();
  f.pickVm('pve-102'); await f.settle();
  if ([...f.content.querySelectorAll('.dt-main button')].some(node => node.textContent === 'Host 업데이트')) { f.click(f.content.querySelector('.dt-main'), 'Host 업데이트'); await f.settle(); assert.equal(announceIn(f.dialog), null); }
});

test('서버 접속 설정 저장: the confirmation offers it while somebody is signed in and saves after the countdown', async t => {
  const window = parseHTML('<!doctype html><html><body></body></html>');
  const previous = Object.fromEntries(['document', 'Node'].map(key => [key, Object.getOwnPropertyDescriptor(globalThis, key)]));
  Object.defineProperty(globalThis, 'document', { configurable: true, value: window.document });
  Object.defineProperty(globalThis, 'Node', { configurable: true, value: window.Node });
  const rev = 'a'.repeat(64);
  const config = { protocol: 'server-access-v1', primaryOrigin: 'https://broker.fixture', https: { enabled: true, origin: 'https://broker.fixture', mode: 'proxy', listenHost: '127.0.0.1', listenPort: 18400, certFile: null, keyFile: null }, http: { enabled: false, origin: null, listenHost: '0.0.0.0', listenPort: 18480 }, trustedProxies: ['127.0.0.1'] };
  const snapshot = { protocol: 'server-access-v1', revision: rev, applied: config, pending: null, managementOrigin: 'https://console.fixture' };
  let audience = audienceOf(), clock = 1000000;
  const writes = [], dialogs = [], state = { user: { id: 'fixture-admin' }, ui: {} };
  const ctx = { state, now: () => clock, setupRequired: () => false, link: (text, page) => el('a', { href: '#/' + page }, text),
    resource: () => ({ value: snapshot, error: '', loading: false }), value: name => name === 'notices' ? listOf(audience) : snapshot,
    pageHeader: ({ title, description }) => el('header', {}, el('h1', {}, title), el('p', {}, description)),
    render: () => window.document.body.replaceChildren(...renderServerAccess(ctx)),
    api: async (path, method, body) => {
      writes.push({ path, body: JSON.parse(JSON.stringify(body)) });
      if (path === '/notices') return { notice: { id: 'e'.repeat(32), addressed: 2 } };
      return path.endsWith('/test') ? { testId: 'fixture-test', revision: rev, ok: true, checks: [{ id: 'configuration', ok: true }], expiresAt: clock + 600000 } : { operationId: 'fixture-operation', state: 'waiting_idle' };
    },
    modal: (title, fields, action, options) => dialogs.push({ title, fields, action, options }), load: async () => {} };
  ctx.render();
  t.after(() => { clearTimeout(state.ui.serverAccess?.expiryTimer); for (const [key, descriptor] of Object.entries(previous)) { if (descriptor) Object.defineProperty(globalThis, key, descriptor); else delete globalThis[key]; } });
  const find = selector => window.document.querySelector(selector);
  // linkedom's select.value is read-only: a select is changed through its options.
  const change = (name, value) => {
    const input = find('[name="' + name + '"]');
    if (input.type === 'checkbox') input.checked = value;
    else if (input.tagName === 'SELECT') for (const option of input.options) { if (option.value === value) option.setAttribute('selected', ''); else option.removeAttribute('selected'); }
    else input.value = value;
    input.dispatchEvent(new window.Event(input.tagName === 'SELECT' || input.type === 'checkbox' ? 'change' : 'input'));
  };
  const flush = async () => { for (let i = 0; i < 4; i++) await new Promise(resolve => setImmediate(resolve)); };
  change('http.enabled', true); change('http.origin', 'http://broker.fixture:18490'); change('http.listenPort', '18491'); change('primaryOrigin', 'http://broker.fixture:18490');
  find('[data-access-test]').click(); await flush();
  // Nobody signed in: the confirmation is as it always was.
  find('[data-access-save]').click();
  assert.equal(dialogs.length, 1); assert.deepEqual(dialogs[0].fields, [], 'no step when nobody is signed in');
  // Somebody signs in: the next confirmation carries the step.
  audience = busy();
  find('[data-access-save]').click();
  assert.equal(dialogs[1].fields.length, 1);
  const node = dialogs[1].fields[0].node; window.document.body.append(node);
  assert.equal(node.querySelector('.announce-preview strong').textContent, '서버 접속 설정을 바꿔요');
  assert.match(node.querySelector('.announce-preview p').textContent, /^1분 뒤 연결이 끊겨요\.\n하던 작업을 저장해 주세요\.\n잠시 뒤 다시 연결해 주세요\.$/);
  const box = node.querySelector('[data-announce=toggle]'); box.checked = true; box.dispatchEvent(new window.Event('change'));
  writes.length = 0;
  t.mock.timers.enable({ apis: ['setInterval', 'Date'] });
  const saving = dialogs[1].action(); for (let i = 0; i < 10; i++) await Promise.resolve();
  assert.deepEqual(writes.map(write => write.path), ['/notices']); assert.deepEqual(writes[0].body.operation, { waitSeconds: 60 });
  assert.equal(node.querySelector('.announce-remaining').textContent, '1:00 뒤에 설정을 저장해요.');
  t.mock.timers.tick(60500); await saving;
  assert.deepEqual(writes.map(write => write.path), ['/notices', '/server-access/save']);
  assert.equal(writes[1].body.confirm, true);
});

test('백업으로 복원: the confirmation offers it while somebody is signed in, never on the first setup screen', async t => {
  const preview = { createdAt: '2026-11-09T03:10:00.000Z', serverVersion: '1.0.43', origin: 'https://cloud.fixture.example', counts: { accounts: 12, admins: 2, desktops: 9, hosts: 8, assignments: 21, adminNames: ['chief'] }, proxmox: { configured: true, readable: true, endpoint: 'https://pve.fixture.example:8006' }, unreadableSecrets: 0, schemaVersion: 19, migrated: false, ignoredObjects: 0, access: { available: true, differs: false, primaryOrigin: 'https://cloud.fixture.example' }, expiresAt: '2026-11-10T09:00:00.000Z' };
  const backup = { restore: { state: 'idle' } };
  const status = () => ({ protocol: 'backup-v1', export: { available: true, passphraseMinimum: 12, maxDatabaseBytes: 268435456, last: null }, restore: { supported: true, chunkBytes: 1000, maxFileBytes: 300000000, ...backup.restore }, upload: null, last: null, reauthentication: { setupCode: false, mfa: false }, connections: { people: 2, sessions: 1, consoles: 0 } });
  const respond = (url, request = {}) => {
    const path = url.replace('/api/v1/admin', ''), method = request.method ?? 'GET', body = request.body ? JSON.parse(request.body) : null;
    if (!path.startsWith('/backup')) return null;
    if (path === '/backup' && method === 'GET') return answer(status());
    if (path === '/backup/restore/upload') return answer({ uploadId: 'u'.repeat(32), chunkBytes: 1000, size: body.size }, 201);
    if (path === '/backup/restore/upload/' + 'u'.repeat(32) && method === 'PUT') return answer({ received: body.offset + Buffer.from(body.data, 'base64').length, complete: false });
    if (path === '/backup/restore/inspect') return answer({ uploadId: body.uploadId, preview });
    if (path === '/backup/restore/apply') { backup.restore = { state: 'waiting_idle', operationId: '0a0a0a0a-1234-4234-8234-123456789abc', busy: true }; return answer({ accepted: true, operationId: backup.restore.operationId, state: 'waiting_idle' }, 202); }
    return answer({ ok: true });
  };
  const checked = async f => {
    const restore = f.content.querySelector('.backup-restore'), input = restore.querySelector('input[type=file]');
    Object.defineProperty(input, 'files', { configurable: true, value: [new File([Buffer.alloc(1500, 3)], '백업.mtbk', { lastModified: 1794300000000 })] }); input.dispatchEvent(new f.window.Event('change'));
    f.content.querySelector('.backup-restore input[name=passphrase]').value = '맞는 백업 암호 열두 글자';
    f.content.querySelector('.backup-restore form.backup-choose').dispatchEvent(new f.window.Event('submit', { cancelable: true }));
    for (let i = 0; i < 8; i++) await f.settle();
    f.click(f.content.querySelector('.backup-restore .backup-preview'), '이 백업으로 복원'); await f.settle();
  };
  const quiet = await open(t, 'restore-quiet', { hash: '#/backup', respond });
  await checked(quiet);
  assert.equal(document.querySelector('#modal-title').textContent, '백업으로 복원'); assert.equal(announceIn(quiet.dialog), null);
  quiet.click(quiet.dialog, '취소');

  const f = await open(t, 'restore-busy', { hash: '#/backup', respond, list: listOf(busy()) });
  await checked(f);
  const step = announceIn(f.dialog); assert.ok(step);
  assert.equal(step.querySelector('.announce-preview strong').textContent, '서버를 백업으로 복원해요');
  assert.match(step.querySelector('.announce-preview p').textContent, /^1분 뒤 연결이 끊기고 서버가 다시 시작돼요\.\n하던 작업을 저장해 주세요\.$/);
  const box = step.querySelector('[data-announce=toggle]'); box.checked = true; box.dispatchEvent(new f.window.Event('change'));
  step.querySelector('[data-segment="0"]').dispatchEvent(new f.window.Event('click'));
  [...document.querySelectorAll('#modal-fields [name]')].find(node => node.name === 'password').value = 'account-password';
  f.writes.length = 0;
  t.mock.timers.enable({ apis: ['setInterval', 'Date'] });
  const submitted = document.querySelector('#modal-form').onsubmit({ preventDefault() {} }); await f.settle();
  assert.deepEqual(f.writes.map(write => write.path), ['/api/v1/admin/notices']);
  assert.equal(JSON.stringify(f.writes[0].data).includes('account-password'), false, 'the password is not part of the notice');
  assert.equal(step.querySelector('.announce-remaining').textContent, '0:03 뒤에 복원을 시작해요.');
  await tickTo(t, f, 3100); await submitted; await f.settle();
  t.mock.timers.reset();
  const apply = f.writes.find(write => write.path.endsWith('/backup/restore/apply'));
  assert.ok(apply, 'the restore was requested after the countdown');
  assert.deepEqual([apply.data.confirm, apply.data.password], [true, 'account-password']);
  assert.deepEqual(f.writes.slice(0, 2).map(write => write.path), ['/api/v1/admin/notices', '/api/v1/admin/backup/restore/apply']);
});

// The form is kept for the whole sign-in, so the 점검 시각 proposed when it was first opened
// would still be there hours later, in the past, where 보내기 refuses it.
test('공지 쓰기 opened again with a 점검 시각 that has passed proposes the next full hour again', async t => {
  const f = await open(t, 'stale-when', { list: listOf(busy()) });
  [...f.compose().querySelectorAll('[aria-label="종류"] [data-segment]')].find(node => node.getAttribute('data-segment') === 'maintenance').dispatchEvent(new f.window.Event('click'));
  f.type(f.field('notice-when'), '2020-01-01T10:00');
  assert.equal(f.field('notice-when').value, '2020-01-01T10:00', 'what is typed stays while the window is open');
  f.click(f.compose(), '닫기'); assert.equal(f.content.querySelector('.notice-compose'), null);
  assert.match(f.field('notice-when').value, /^\d{4}-\d\d-\d\dT\d\d:00$/); assert.ok(Date.parse(f.field('notice-when').value) > Date.now());
  // A time still ahead is kept.
  f.type(f.field('notice-when'), '2199-01-01T10:00'); f.click(f.compose(), '닫기');
  assert.equal(f.field('notice-when').value, '2199-01-01T10:00');
});
