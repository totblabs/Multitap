import { test, mock } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { loadAdmin, registerFixtureDesktops } from './admin-ui-harness.mjs';
import { connectedText } from '../admin/js/usage.js';

// The console reads the clock (how long a live login has lasted, which day is
// today in the browser). The fixture's today is 2026-11-10, so the clock is
// held there: with the real one the tests fail once that day has passed.
mock.timers.enable({ apis: ['Date'], now: Date.parse('2026-11-10T13:00:00+09:00') });

// 모니터링 (admin/js/usage.js) and the sidebar's 업데이트 count, in the real
// console DOM against fixture answers shaped like server/src/usage.mjs.
// DOM behaviour only: layout is checked in the renders. Times are shown in the
// browser's zone, which the tests pin.
process.env.TZ = 'Asia/Seoul';
const HOUR = 3600000, MIN = 60000, today = '2026-11-10';
const at = (day, time) => new Date(`${day}T${time}:00+09:00`).toISOString();
const zone = { protocol: 'usage-v1', timeZone: 'Asia/Seoul', utcOffsetMinutes: 540 };
function world() {
  const user = { id: 'owner', username: 'owner', role: 'admin', mustChangeCredentials: false };
  const inventory = { users: [{ ...user, disabled: 0, notes: '' }, { id: 'u1', username: '김민지', role: 'user', disabled: 0, notes: '' }, { id: 'u2', username: 'lee.junho', role: 'user', disabled: 0, notes: '' }], assignments: [], audit: [], proxmox: { configured: true, state: 'ok' },
    desktops: [{ id: 'pve-101', kind: 'vm', pve_vmid: 101, name: '디자인 1', pve_node: 'n', pve_available: 1, pve_power_status: 'running', host: { connected: true } }, { id: 'pve-102', kind: 'vm', pve_vmid: 102, name: '개발 1', pve_node: 'n', pve_available: 1, pve_power_status: 'running', host: { connected: true } }] };
  registerFixtureDesktops(inventory);
  const live = { ...zone, checkedAt: at(today, '13:00'), today: { date: today, logins: 3, connections: 4 }, totals: { logins: 1204, connections: 2310 }, recordingSince: at('2026-11-10', '09:00'), retentionDays: 730, people: 2,
    logins: [{ account: '김민지', since: at(today, '08:55'), platform: 'windows', version: '1.0.41', address: '203.0.113.7', connections: 1 }, { account: 'lee.junho', since: at(today, '09:10'), platform: 'android', version: null, address: null, connections: 0 }],
    connections: [{ sessionId: '11111111-2222-4333-8444-555555555555', account: '김민지', desktopId: 'pve-101', desktopName: '디자인 1', mode: 'stream', since: at(today, '09:00'), ready: true, platform: 'windows', version: '1.0.41', address: '203.0.113.7', activeMs: null }] };
  const days = (from, to) => { const list = []; for (let d = new Date(from + 'T00:00:00Z'); d <= new Date(to + 'T00:00:00Z'); d = new Date(d.getTime() + 86400000)) list.push(d.toISOString().slice(0, 10)); return list; };
  const summary = (from, to) => {
    const list = days(from, to), fill = values => list.map((_, index) => values[index - (list.length - values.length)] ?? 0);
    return { ...zone, from, to, days: list, truncated: false, recordingSince: live.recordingSince, retentionDays: 730,
      totals: { logins: 9, connections: 7, connectedMs: 20 * HOUR + 30 * MIN, firstAt: at(from, '09:00'), lastAt: at(to, '13:00'), daily: fill([0]), dailyConnections: fill([0]), activeMs: null },
      users: [
        { account: '김민지', logins: 5, connections: 4, connectedMs: 12 * HOUR + 30 * MIN, firstAt: at(from, '09:00'), lastAt: at(to, '13:00'), daily: fill([8 * HOUR, 30 * MIN, 4 * HOUR]), dailyConnections: fill([2, 1, 1]), activeMs: null },
        { account: 'lee.junho', logins: 3, connections: 3, connectedMs: 8 * HOUR, firstAt: at(from, '10:00'), lastAt: at(to, '11:00'), daily: fill([5 * HOUR, 0, 3 * HOUR]), dailyConnections: fill([2, 0, 1]), activeMs: null },
        { account: 'owner', logins: 1, connections: 0, connectedMs: 0, firstAt: at(to, '08:00'), lastAt: at(to, '08:00'), daily: fill([0]), dailyConnections: fill([0]), activeMs: null }],
      desktops: [
        { desktopId: 'pve-101', desktopName: '옛 이름', connections: 5, connectedMs: 15 * HOUR, firstAt: at(from, '09:00'), lastAt: at(to, '13:00'), daily: fill([9 * HOUR, 30 * MIN, 5 * HOUR + 30 * MIN]), dailyConnections: fill([3, 1, 1]), activeMs: null,
          users: [{ account: '김민지', connections: 4, connectedMs: 12 * HOUR + 30 * MIN }, { account: 'lee.junho', connections: 1, connectedMs: 2 * HOUR + 30 * MIN }] },
        { desktopId: 'pve-900', desktopName: '지워진 데스크톱', connections: 2, connectedMs: 5 * HOUR + 30 * MIN, firstAt: at(from, '10:00'), lastAt: at(to, '11:00'), daily: fill([4 * HOUR, 0, 90 * MIN]), dailyConnections: fill([1, 0, 1]), activeMs: null, users: [{ account: 'lee.junho', connections: 2, connectedMs: 5 * HOUR + 30 * MIN }] }] };
  };
  const event = (kind, account, day, start, end, extra = {}) => ({ kind, account, startedAt: at(day, start), endedAt: end ? at(day, end) : null, live: false, durationMs: end ? Date.parse(at(day, end)) - Date.parse(at(day, start)) : null, estimated: false, endReason: kind === 'login' ? 'logout' : 'client_stopped',
    ...(kind === 'connection' ? { desktopId: 'pve-101', desktopName: '디자인 1', mode: 'stream', activeMs: null } : {}), platform: 'windows', version: '1.0.41', address: '203.0.113.7', source: 'server', ...extra });
  const dayEvents = day => [
    event('connection', '김민지', day, '11:10', null, { live: true, durationMs: 110 * MIN, endReason: null }),
    event('connection', 'lee.junho', day, '10:00', '10:45', { estimated: true, endReason: 'client_timeout', address: '2001:db8:4a:1::/64', platform: 'android', version: '1.0.28', desktopId: 'pve-900', desktopName: '지워진 데스크톱' }),
    event('login', 'lee.junho', day, '09:58', '11:00', { endReason: 'session_idle_timeout', address: '2001:db8:4a:1::/64', platform: 'android', version: '1.0.28' }),
    event('connection', '김민지', day, '09:00', '10:30'),
    event('login', '김민지', day, '08:55', null, { live: true, durationMs: 245 * MIN, endReason: null }),
    event('login', 'owner', day, '07:00', null, { source: 'audit', platform: null, version: null, address: null, endReason: null })];
  const paths = [];
  const usage = path => {
    paths.push(path);
    if (path === '/usage/live') return live;
    const [, kind, from, to, ...rest] = path.split('/').slice(1);
    if (kind === 'summary') return summary(from, to);
    const subject = rest[0] === 'user' || rest[0] === 'desktop' ? decodeURIComponent(rest[1]) : null, page = rest.includes('page') ? Number(rest[rest.indexOf('page') + 1]) : 1;
    let events = dayEvents(to).filter(e => rest[0] !== 'user' || e.account === subject).filter(e => rest[0] !== 'desktop' || e.desktopId === subject);
    const accounts = ['김민지', 'lee.junho', 'owner'].map(account => ({ account, logins: events.filter(e => e.account === account && e.kind === 'login').length, connections: events.filter(e => e.account === account && e.kind === 'connection').length })).filter(entry => entry.logins + entry.connections);
    const counts = { logins: events.filter(e => e.kind === 'login').length, connections: events.filter(e => e.kind === 'connection').length };
    // The day before today has a second page.
    const more = to === '2026-11-09' && rest[0] === undefined && page === 1;
    if (page === 2) events = [event('login', 'lee.junho', to, '06:00', '06:30')];
    return { ...zone, from, to, page, pageSize: 200, more, counts, accounts, events };
  };
  return { user, inventory, monitor: { usage }, paths, live };
}
const texts = (scope, selector) => [...scope.querySelectorAll(selector)].map(node => node.textContent);
const press = (f, selector) => { const node = f.content.querySelector(selector); assert.ok(node, 'Missing control: ' + selector); node.dispatchEvent(new f.window.Event('click')); };

test('모니터링 is its own place: one summary row, four views, and one honest line about what is measured and kept', async t => {
  const w = world(), f = await loadAdmin(t, 'usage-frame', w.user, w.inventory, { monitor: w.monitor, hash: '#/monitoring' });
  assert.equal(f.crumb(), '모니터링'); assert.equal(f.content.querySelector('h1').textContent, '모니터링');
  assert.equal(document.querySelector('#sidebar [data-page=monitoring]').getAttribute('aria-current'), 'page');
  // One summary row (not boxed tiles above a second strip): now, today and the totals.
  assert.equal(f.content.querySelector('.stat-tiles,.usage-tiles'), null);
  assert.equal(f.content.querySelectorAll('.usage-strip').length, 1);
  assert.deepEqual([...f.content.querySelectorAll('.usage-strip > div')].map(node => [node.querySelector('dt').textContent, node.querySelector('dd').textContent]),
    [['지금 로그인', '2명'], ['지금 접속 중', '1개'], ['오늘 로그인', '3회'], ['오늘 연결', '4회'], ['누적 로그인', '1,204회'], ['누적 연결', '2,310회'], ['사용자', '3명'], ['데스크톱', '2대']]);
  assert.equal(f.content.querySelector('.page-description'), null, 'no subtitle that narrates the page');
  assert.deepEqual(texts(f.content, '.log-tabs [role=tab]'), ['지금', '사용자별', '데스크톱별', '로그인·연결 내역']);
  const note = f.content.querySelector('.usage-note').textContent;
  for (const line of [/연결 시간은 데스크톱에 연결해 있던 시간이에요\. 자리를 비운 시간도 들어 있어요\./, /로그인·연결 기록은 2년 동안 보관해요\./, /주소는 접속한 곳의 IP 주소예요\./, /하루는 서버 시간대\(Asia\/Seoul\) 기준이에요\./, /이전은 로그인 시각과 클라이언트가 보고한 연결만 있어요\./]) assert.match(note, line);
  // The measure is connected time, never work time, in the page and in its source.
  for (const file of ['js/usage.js', 'js/charts.js', 'i18n/ko.js']) assert.doesNotMatch(await readFile(new URL('../admin/' + file, import.meta.url), 'utf8'), /근무|업무 시간|일한 시간/, file);
  for (const tab of ['users', 'desktops', 'history']) { f.navigate('monitoring/' + tab); await f.settle(); assert.doesNotMatch(f.content.textContent, /근무|일한 시간/, tab); assert.equal(f.crumb(), '모니터링'); }
  // A tab opens its list; the tabs move with the arrow keys.
  press(f, '#usage-tab-history'); await f.settle();
  assert.equal(f.content.querySelector('#usage-tab-history').getAttribute('aria-selected'), 'true');
  const key = value => { const tab = f.content.querySelector('.log-tabs [role=tab][aria-selected=true]'), event = new f.window.Event('keydown', { cancelable: true }); event.key = value; tab.dispatchEvent(event); return event; };
  key('ArrowLeft'); await f.settle(); assert.equal(f.content.querySelector('[role=tab][aria-selected=true]').id, 'usage-tab-desktops'); assert.ok(f.focused() === f.content.querySelector('#usage-tab-desktops'));
  key('Home'); await f.settle(); assert.equal(f.content.querySelector('[role=tab][aria-selected=true]').id, 'usage-tab-now'); assert.equal(f.content.querySelector('#usage-panel').getAttribute('aria-labelledby'), 'usage-tab-now');
  assert.deepEqual(f.writes, [], 'the page only reads');
  assert.equal(connectedText(59000), '1분 미만'); assert.equal(connectedText(0), '0분'); assert.equal(connectedText(3 * HOUR), '3시간'); assert.equal(connectedText(3 * HOUR + 59 * MIN + 59000), '3시간 59분'); assert.equal(connectedText(NaN), '—');
});

test('사용자별: people ranked by connected time with the exact hours as text, a range, a table view, and one person\'s days and connections', async t => {
  const w = world(), f = await loadAdmin(t, 'usage-users', w.user, w.inventory, { monitor: w.monitor, hash: '#/monitoring/users' });
  const { content } = f;
  assert.ok(w.paths.includes('/usage/summary/2026-11-04/2026-11-10'), 'seven days ending on the server\'s today');
  assert.equal(content.querySelector('.usage-ranking h2').textContent, '사람별 연결 시간3');
  assert.equal(content.querySelector('.usage-ranking .panel-title p').textContent, '모두 20시간 30분 · 연결 7회 · 로그인 9회');
  assert.match(content.querySelector('.usage-range small').textContent, /^2026-11-04 \(수\) ~ 2026-11-10 \(화\)$/);
  const rows = () => [...content.querySelectorAll('.usage-ranking .rank-bars > li > button.rank-row')];
  assert.deepEqual(rows().map(row => [row.querySelector('.rank-name').textContent, row.querySelector('.rank-value strong').textContent, row.querySelector('.rank-value small').textContent, row.getAttribute('aria-pressed')]),
    [['김민지', '12시간 30분', '연결 4회 · 로그인 5회', 'false'], ['lee.junho', '8시간', '연결 3회 · 로그인 3회', 'false'], ['owner', '0분', '연결 0회 · 로그인 1회', 'false']]);
  const widths = rows().map(row => Number(row.querySelector('rect.rank-fill').getAttribute('width')));
  assert.equal(widths[0], 1000); assert.equal(widths[1], 640); assert.equal(widths[2], 0, 'bars are proportional to the longest; nothing is drawn for zero');
  assert.equal(content.querySelector('.usage-detail'), null); assert.match(content.querySelector('.usage-ranking').textContent, /이름을 누르면 날짜별 연결 시간과 연결 목록이 보여요\./);
  // One person: numbers, one bar per day with a keyboard readout, and their sign-ins and connections.
  rows()[0].dispatchEvent(new f.window.Event('click')); await f.settle();
  // The chosen one opens in a window over the list: the focus moves to its title, and the window says whose it is and of which days.
  assert.equal(rows()[0].getAttribute('aria-pressed'), 'true'); assert.equal(f.focused().id, 'detail-title', 'the focus moves into the window');
  assert.equal(content.querySelector('#detail').hasAttribute('open'), true);
  assert.equal(content.querySelector('.usage-detail .win-head').textContent.replace('닫기', ''), '사람별 연결 시간김민지2026-11-04 (수) ~ 2026-11-10 (화)');
  assert.ok(w.paths.includes('/usage/events/2026-11-04/2026-11-10/user/' + encodeURIComponent('김민지')));
  const detail = content.querySelector('.usage-detail');
  assert.equal(detail.querySelector('h2').textContent, '김민지');
  assert.deepEqual([...detail.querySelectorAll('.usage-facts > div')].map(node => [node.querySelector('dt').textContent, node.querySelector('dd').textContent]).slice(0, 4), [['연결 시간', '12시간 30분'], ['연결', '4회'], ['로그인', '5회'], ['연결한 날', '3일 / 7일']]);
  const chart = detail.querySelector('figure.day-chart'), frame = chart.querySelector('.chart-frame');
  assert.equal(chart.querySelectorAll('rect.day-bar').length, 3, 'a day without a connection has no bar');
  assert.equal(frame.getAttribute('tabindex'), '0'); assert.equal(frame.getAttribute('role'), 'img');
  assert.match(frame.getAttribute('aria-label'), /^날짜별 연결 시간: 7일 가운데 가장 긴 날은 11월 8일 \(일\) 8시간\./);
  assert.equal(chart.querySelector('.chart-readout').textContent, '8시간 가장 긴 날 · 11월 8일 (일) · 연결 2회');
  assert.deepEqual(texts(chart, '.chart-y span'), ['8시간', '4시간', '0분']); assert.deepEqual(texts(chart, '.chart-x span'), ['11월 4일 (수)', '11월 10일 (화)']);
  const arrow = value => { const event = new f.window.Event('keydown', { cancelable: true }); event.key = value; frame.dispatchEvent(event); return event; };
  assert.equal(arrow('ArrowRight').defaultPrevented, true); assert.equal(chart.querySelector('.chart-readout').textContent, '30분 11월 9일 (월) · 연결 1회');
  arrow('End'); assert.equal(chart.querySelector('.chart-readout').textContent, '4시간 11월 10일 (화) · 연결 1회'); arrow('Home'); assert.equal(chart.querySelector('.chart-readout').textContent, '0분 11월 4일 (수) · 연결 0회');
  assert.deepEqual(texts(detail, 'table.usage-events th'), ['시각', '종류', '데스크톱', '클라이언트 · 주소', '연결 시간', '끝']);
  assert.deepEqual([...detail.querySelectorAll('table.usage-events tbody tr')].map(row => [...row.querySelectorAll('td')].map(cell => cell.textContent)), [
    ['11월 10일 (화)11:10 ~ 지금', '연결', '디자인 1', 'Windows · 1.0.41203.0.113.7', '1시간 50분', '진행 중'],
    ['11월 10일 (화)09:00 ~ 10:30', '연결', '디자인 1', 'Windows · 1.0.41203.0.113.7', '1시간 30분', '사용자가 연결을 끊음'],
    ['11월 10일 (화)08:55 ~ 지금', '로그인', '—', 'Windows · 1.0.41203.0.113.7', '4시간 5분', '진행 중']]);
  assert.equal(detail.querySelector('a.button').getAttribute('href'), '#/users/' + encodeURIComponent('김민지'));
  // The same numbers as a table.
  f.click(content.querySelector('.usage-ranking'), '표'); await f.settle();
  assert.equal(content.querySelector('.usage-ranking .rank-bars'), null);
  assert.deepEqual(texts(content, '.usage-ranking table.usage-summary th'), ['사용자', '연결 시간', '연결', '로그인', '처음', '마지막']);
  assert.deepEqual([...content.querySelectorAll('.usage-ranking table.usage-summary tbody tr')].map(row => [...row.querySelectorAll('td')].slice(0, 4).map(cell => cell.textContent)), [['김민지', '12시간 30분', '4회', '5회'], ['lee.junho', '8시간', '3회', '3회'], ['owner', '0분', '0회', '1회']]);
  assert.deepEqual([...content.querySelectorAll('.usage-detail table')][0].querySelectorAll('tbody tr').length, 7, 'the days as rows');
  assert.equal(content.querySelector('.usage-detail figure.day-chart'), null);
  // The window has the same choice of its own (the list is under it), above what it changes.
  assert.deepEqual([...content.querySelectorAll('.usage-detail .win-body [aria-label="보기"] button')].map(node => [node.textContent, node.getAttribute('aria-pressed')]), [['그래프', 'false'], ['표', 'true']]);
  f.click(content.querySelector('.usage-detail .win-body'), '그래프'); await f.settle();
  assert.ok(content.querySelector('.usage-detail figure.day-chart')); assert.ok(content.querySelector('.usage-ranking .rank-bars'), 'one choice for the list and the window');
  assert.equal(f.focused().getAttribute('data-focus'), 'usage-view-chart');
  // Another range reads its own summary and the chosen person's list for it.
  f.click(content.querySelector('.usage-range'), '30일'); await f.settle();
  assert.ok(w.paths.includes('/usage/summary/2026-10-12/2026-11-10')); assert.ok(w.paths.includes('/usage/events/2026-10-12/2026-11-10/user/' + encodeURIComponent('김민지')));
  f.click(content.querySelector('.usage-range'), '오늘'); await f.settle();
  assert.ok(w.paths.includes('/usage/summary/2026-11-10/2026-11-10'));
  assert.equal(content.querySelector('.usage-detail figure.day-chart'), null, 'one day is a number, not a chart'); assert.equal(content.querySelector('.usage-range small').textContent, '2026-11-10 (화)');
  f.click(content.querySelector('.usage-range'), '직접 고르기'); await f.settle();
  const [from, to] = content.querySelectorAll('.usage-range input[type=date]');
  assert.deepEqual([from.getAttribute('aria-label'), from.getAttribute('max'), from.getAttribute('min'), to.getAttribute('aria-label')], ['시작', today, '2024-11-10', '끝']);
  f.change(from, '2026-11-01'); await f.settle();
  assert.ok(w.paths.includes('/usage/summary/2026-11-01/2026-11-10'));
  f.click(content.querySelector('.usage-detail .win-foot'), '닫기'); await f.settle(); assert.equal(content.querySelector('.usage-detail'), null);
  assert.equal(f.focused().getAttribute('data-focus'), 'usage-users-김민지', 'closing returns the focus to the row that opened it');
  assert.deepEqual(f.writes, []);
});

test('데스크톱별: each desktop\'s connected time and who used it; one no longer in the inventory keeps its recorded name', async t => {
  const w = world(), f = await loadAdmin(t, 'usage-desktops', w.user, w.inventory, { monitor: w.monitor, hash: '#/monitoring/desktops' });
  const { content } = f, rows = () => [...content.querySelectorAll('.usage-ranking .rank-bars > li > button.rank-row')];
  assert.equal(content.querySelector('.usage-ranking h2').textContent, '데스크톱별 연결 시간2');
  assert.deepEqual(rows().map(row => [row.querySelector('.rank-name').textContent, row.querySelector('.rank-value strong').textContent, row.querySelector('.rank-value small').textContent]),
    [['디자인 1', '15시간', '연결 5회 · 2명'], ['지워진 데스크톱', '5시간 30분', '연결 2회 · 1명']], 'a desktop in the inventory shows its current name');
  rows()[0].dispatchEvent(new f.window.Event('click')); await f.settle();
  assert.ok(w.paths.includes('/usage/events/2026-11-04/2026-11-10/desktop/pve-101'));
  const detail = content.querySelector('.usage-detail');
  assert.equal(detail.querySelector('h2').textContent, '디자인 1'); assert.equal(detail.querySelector('a.button').getAttribute('href'), '#/desktops/pve-101');
  assert.deepEqual(texts(detail, 'h3'), ['누가 썼나', '연결 목록']);
  assert.deepEqual([...detail.querySelectorAll('.usage-shares .rank-row')].map(row => [row.tagName, row.querySelector('.rank-name').textContent, row.querySelector('.rank-value').textContent]), [['DIV', '김민지', '12시간 30분연결 4회'], ['DIV', 'lee.junho', '2시간 30분연결 1회']]);
  assert.deepEqual(texts(detail, 'table.usage-events th'), ['시각', '종류', '사용자', '클라이언트 · 주소', '연결 시간', '끝']);
  assert.deepEqual([...detail.querySelectorAll('table.usage-events tbody tr')].map(row => row.querySelectorAll('td')[1].textContent), ['연결', '연결'], 'a desktop lists connections only');
  rows()[1].dispatchEvent(new f.window.Event('click')); await f.settle();
  assert.equal(content.querySelector('.usage-detail h2').textContent, '지워진 데스크톱'); assert.equal(content.querySelector('.usage-detail a.button'), null, 'nothing to open for a desktop that is gone');
});

test('기록: one day with per-person counts, a search over name, address and desktop, a date stepper and more pages', async t => {
  const w = world(), f = await loadAdmin(t, 'usage-history', w.user, w.inventory, { monitor: w.monitor, hash: '#/monitoring/history' });
  const { content } = f;
  assert.ok(w.paths.includes('/usage/events/2026-11-10/2026-11-10'));
  assert.equal(content.querySelector('.usage-day').textContent, '오늘 (2026-11-10 화)');
  assert.equal(content.querySelector('[data-focus=usage-next]').disabled, true, 'there is no tomorrow'); assert.equal(f.find(content, 'button', '오늘'), undefined);
  // The person is a choice of the conditions box, each with the day's count; the list has the whole width.
  assert.equal(content.querySelector('.usage-people'), null);
  assert.deepEqual([...content.querySelectorAll('[data-focus=usage-who] option')].map(node => [node.textContent, node.hasAttribute('selected')]), [['전체 6건', true], ['김민지 3건', false], ['lee.junho 2건', false], ['owner 1건', false]]);
  assert.equal(content.querySelector('[data-focus=usage-who]').closest('.query-box').getAttribute('aria-label'), '로그인·연결 내역 조회 조건');
  assert.equal(content.querySelector('.usage-history-list .result-bar span').textContent, '6건 · 로그인 3회 · 연결 3회');
  assert.deepEqual(texts(content, 'table.usage-events th'), ['시각', '종류', '사용자', '데스크톱', '클라이언트 · 주소', '연결 시간', '끝']);
  const rows = () => [...content.querySelectorAll('table.usage-events tbody tr')].map(row => [...row.querySelectorAll('td')].map(cell => cell.textContent));
  assert.deepEqual(rows(), [
    ['11:10 ~ 지금', '연결', '김민지', '디자인 1', 'Windows · 1.0.41203.0.113.7', '1시간 50분', '진행 중'],
    ['10:00 ~ 10:45', '연결', 'lee.junho', '지워진 데스크톱', 'Android · 1.0.282001:db8:4a:1::/64', '45분추정', '클라이언트 응답 없음'],
    ['09:58 ~ 11:00', '로그인', 'lee.junho', '—', 'Android · 1.0.282001:db8:4a:1::/64', '1시간 2분', '입력이 없어 자동 로그아웃'],
    ['09:00 ~ 10:30', '연결', '김민지', '디자인 1', 'Windows · 1.0.41203.0.113.7', '1시간 30분', '사용자가 연결을 끊음'],
    ['08:55 ~ 지금', '로그인', '김민지', '—', 'Windows · 1.0.41203.0.113.7', '4시간 5분', '진행 중'],
    ['07:00 ~ ?', '로그인', 'owner', '—', '이전 기록', '—', '끝난 시각 기록 없음']]);
  assert.equal(content.querySelector('table.usage-events .usage-estimated').getAttribute('title'), '끝난 시각을 서버가 추정했어요.');
  assert.equal(content.querySelector('table.usage-events .pill'), null, 'the kind, an estimate and a live connection are plain words');
  assert.equal(content.querySelectorAll('table.usage-events tbody tr')[1].querySelector('td[data-label="데스크톱"] a'), null, 'a desktop that is gone is not a link');
  assert.equal(content.querySelectorAll('table.usage-events tbody tr')[0].querySelector('td[data-label="데스크톱"] a').getAttribute('href'), '#/desktops/pve-101');
  // Search: name, address, desktop.
  const search = content.querySelector('.usage-history-bar input[type=search]');
  assert.equal(search.getAttribute('placeholder'), '이름·주소·데스크톱 검색');
  const type = async value => { const node = content.querySelector('.usage-history-bar input[type=search]'); node.value = value; node.dispatchEvent(new f.window.Event('input')); await f.settle(); };
  await type('2001:db8'); assert.deepEqual(rows().map(row => row[2]), ['lee.junho', 'lee.junho']); assert.equal(content.querySelector('.usage-history-list .result-bar strong').textContent, '2건');
  await type('디자인'); assert.deepEqual(rows().map(row => row[3]), ['디자인 1', '디자인 1']);
  await type('민지'); assert.equal(rows().length, 3);
  await type('없는 말'); assert.equal(content.querySelector('.usage-history-list .empty strong').textContent, '조건에 맞는 기록이 없어요');
  await type('');
  // One person: the server filters, so the counts and the pages are theirs.
  f.change(content.querySelector('[data-focus=usage-who]'), 'lee.junho'); await f.settle();
  assert.ok(w.paths.includes('/usage/events/2026-11-10/2026-11-10/user/lee.junho'));
  assert.deepEqual(rows().map(row => row[2]), ['lee.junho', 'lee.junho']);
  assert.deepEqual([...content.querySelectorAll('[data-focus=usage-who] option')].map(node => [node.textContent, node.hasAttribute('selected')]), [['전체 6건', false], ['김민지 3건', false], ['lee.junho 2건', true], ['owner 1건', false]], 'the choices keep the whole day');
  assert.equal(f.focused().getAttribute('data-focus'), 'usage-who', 'the choice keeps the focus');
  // The day before: its own read, a way back to today, and a second page on request.
  press(f, '[data-focus=usage-prev]'); await f.settle();
  assert.ok(w.paths.includes('/usage/events/2026-11-09/2026-11-09')); assert.equal(content.querySelector('.usage-day').textContent, '2026-11-09 (월)');
  assert.equal(content.querySelector('[data-focus=usage-who] option[selected]').getAttribute('value'), 'all', 'a new day starts with everyone');
  assert.equal(content.querySelector('[data-focus=usage-next]').disabled, false); assert.ok(f.find(content, 'button', '오늘'));
  assert.equal(rows().length, 6); assert.equal(f.find(content, 'button', '더 보기').getAttribute('data-focus'), 'usage-more');
  f.click(content, '더 보기'); await f.settle();
  assert.ok(w.paths.includes('/usage/events/2026-11-09/2026-11-09/page/2')); assert.equal(rows().length, 7); assert.equal(rows().at(-1)[0], '06:00 ~ 06:30'); assert.equal(f.find(content, 'button', '더 보기'), undefined);
  f.change(content.querySelector('.usage-day-input'), '2026-10-01'); await f.settle();
  assert.ok(w.paths.includes('/usage/events/2026-10-01/2026-10-01')); assert.equal(content.querySelector('.usage-day').textContent, '2026-10-01 (목)');
  f.click(content, '오늘'); await f.settle(); assert.equal(content.querySelector('.usage-day').textContent, '오늘 (2026-11-10 화)');
  assert.deepEqual(f.writes, []);
});

test('an install without history says so in every view instead of drawing empty charts', async t => {
  const w = world(), f = await loadAdmin(t, 'usage-empty', w.user, w.inventory, { hash: '#/monitoring/users' });
  assert.equal(f.content.querySelector('.usage-ranking .empty strong').textContent, '아직 기록이 없어요');
  assert.equal(f.content.querySelector('.usage-ranking .rank-bars'), null); assert.equal(f.content.querySelector('.usage-ranking svg.chart-plot'), null);
  f.navigate('monitoring/desktops'); await f.settle(); assert.equal(f.content.querySelector('.usage-ranking .empty strong').textContent, '아직 기록이 없어요');
  f.navigate('monitoring/history'); await f.settle();
  assert.equal(f.content.querySelector('.usage-history-list .empty strong').textContent, '아직 기록이 없어요'); assert.deepEqual(texts(f.content, '[data-focus=usage-who] option'), ['전체 0건']);
  f.navigate('monitoring'); await f.settle();
  assert.deepEqual(texts(f.content, '#usage-panel .empty strong'), ['지금 접속 중인 사람이 없어요', '지금 로그인한 사람이 없어요']);
  assert.deepEqual(texts(f.content, '.usage-strip dd').slice(0, 4), ['0명', '0개', '0회', '0회']);
  // A read that fails is named with a way to try again, never shown as "no history".
  const failing = await loadAdmin(t, 'usage-failed', w.user, w.inventory, { hash: '#/monitoring/users', respond: async url => url.includes('/usage/summary/') ? { ok: false, status: 500, json: async () => ({ error: { code: 'internal_error' } }) } : null });
  assert.equal(failing.content.querySelector('.usage-ranking .empty.is-error strong').textContent, '연결 시간을 불러오지 못했어요'); assert.ok(failing.find(failing.content, 'button', '다시 시도'));
});

test('the 업데이트 entry counts what can be updated now: a newer server and the desktops with a Host update', async t => {
  const w = world();
  const view = extra => ({ connected: true, version: '1.0.10', newer: true, supported: true, available: true, latestVersion: '1.0.12', ...extra });
  w.inventory.hostUpdates = { latest: { version: '1.0.12', changelog: '' }, desktops: { 'pve-101': view(), 'pve-102': view() } };
  let update = { state: 'available', feedConfigured: true, applySupported: true, currentVersion: '1.0.39', releaseId: 'c'.repeat(64), latest: { version: '1.0.40', releasedAt: '2026-11-10T00:00:00.000Z', changelog: '모니터링' } };
  let stamp = Date.parse('2026-11-10T01:00:00.000Z');
  const f = await loadAdmin(t, 'updates-badge', w.user, w.inventory, { monitor: w.monitor, respond: async url => url.endsWith('/server-info')
    ? { ok: true, status: 200, json: async () => ({ service: 'multitap-broker', version: '1.0.39', deployment: { kind: 'systemd', configured: true }, update, checkedAt: new Date(stamp += 1000).toISOString() }) } : null });
  const badge = () => document.querySelector('[data-nav-badge=updates]');
  const refresh = async () => { document.querySelector('#refresh').dispatchEvent(new f.window.Event('click')); await f.settle(); };
  assert.equal(badge().closest('a').getAttribute('href'), '#/updates');
  assert.deepEqual([badge().hidden, badge().textContent, badge().getAttribute('aria-label'), badge().getAttribute('title')], [false, '3', '업데이트할 수 있어요: 서버 1.0.40, 데스크톱 2대', '업데이트할 수 있어요: 서버 1.0.40, 데스크톱 2대']);
  // The page itself: the server first, then the Hosts, with the existing actions.
  f.navigate('updates'); await f.settle();
  assert.deepEqual(texts(f.content, '.panel > .panel-head h2'), ['현재 서버', '새 버전 1.0.40', 'Host 업데이트']);
  assert.ok(f.find(f.content, 'button', '업데이트 확인')); assert.ok(f.find(f.content.querySelector('.server-release'), 'button', '업데이트'));
  assert.match(f.content.querySelector('.server-release').textContent, /백업 → 적용 → 재시작 → 상태 확인 순서로 진행해요\./);
  assert.ok(f.find(f.content.querySelector('.settings-updates'), 'button', '2대 모두 업데이트')); assert.ok(f.find(f.content.querySelector('.settings-updates'), 'a', '한 대씩 고르기'));
  // Only desktops: the server is current.
  update = { ...update, state: 'current' }; await refresh();
  assert.deepEqual([badge().hidden, badge().textContent, badge().getAttribute('aria-label')], [false, '2', '업데이트할 수 있어요: 데스크톱 2대']);
  // Only the server.
  update = { ...update, state: 'available' }; w.inventory.hostUpdates.desktops = { 'pve-101': view({ newer: false, available: false, version: '1.0.12' }), 'pve-102': view({ newer: false, available: false, version: '1.0.12' }) }; await refresh();
  assert.deepEqual([badge().hidden, badge().textContent, badge().getAttribute('aria-label')], [false, '1', '업데이트할 수 있어요: 서버 1.0.40']);
  assert.match(f.content.querySelector('.settings-updates').textContent, /모든 데스크톱이 최신 Host예요 · 1\.0\.12/);
  // Nothing to update: no badge at all.
  update = { ...update, state: 'current' }; await refresh();
  assert.deepEqual([badge().hidden, badge().textContent, badge().getAttribute('aria-label'), badge().getAttribute('title')], [true, '', null, null]);
  assert.equal(document.querySelector('[data-nav-badge=home]').closest('a').getAttribute('data-page'), 'overview', 'the 홈 count is its own');
  assert.deepEqual(f.writes.filter(write => !write.path.endsWith('/proxmox/sync')), [], 'counting requests nothing');
});
