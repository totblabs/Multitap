import test from 'node:test';
import assert from 'node:assert/strict';
import { parseHTML } from 'linkedom';
import { el } from '../admin/js/dom.js';
import { render, usageNeeds } from '../admin/js/usage.js';
import { logTabs } from '../admin/js/monitor.js';

// Actual production DOM with synthetic read-only observations. No server,
// login, API request or browser-layout claim is involved.
// The connections table is the 지금 view of 모니터링 (route { page: 'monitoring', id: null }).
function fixture(t, { live, logins = [], stats = [] }) {
  const previous = { document: globalThis.document, Node: globalThis.Node };
  const window = parseHTML('<!doctype html><html><body></body></html>');
  Object.assign(globalThis, { document: window.document, Node: window.Node });
  t.after(() => Object.assign(globalThis, previous));
  const usageLive = { protocol: 'usage-v1', timeZone: 'Asia/Seoul', today: { date: '2026-11-06', logins: 3, connections: 2 }, totals: { logins: 40, connections: 31 }, recordingSince: null, retentionDays: 730, people: new Set(logins.map(row => row.account)).size, logins, connections: live };
  const values = { usageLive, streamStats: { sessions: stats }, hosts: { hosts: [{ desktopId: 'fixture-vm' }] } };
  const desktops = [{ id: 'fixture-vm', pve_vmid: 247, name: 'FIXTURE-PC' }], users = [{ username: 'fixture-user' }];
  const calls = [];
  const ctx = {
    state: { ui: {}, data: { users, desktops } },
    value: key => values[key], resource: key => ({ value: values[key] }),
    desktopById: id => desktops.find(d => d.id === id), userByName: name => users.find(u => u.username === name),
    hashFor: (page, id) => '#/' + page + (id ? '/' + id : ''),
    link: (text, page, id, attrs = {}) => el('a', { ...attrs, href: '#/' + page + '/' + id }, text),
    pageHeader: ({ title, description, actions }) => el('header', {}, el('h1', {}, title), el('p', {}, description), actions),
    render: () => calls.push('render'), load: async name => calls.push(['load', name]),
    navigate: (page, id) => calls.push(['navigate', page, id]), refresh: async () => calls.push('refresh'),
  };
  const route = { page: 'monitoring', id: null };
  assert.deepEqual(usageNeeds(ctx, route), ['usageLive', 'streamStats'], 'The view reads exactly what its columns show');
  assert.equal(logTabs.includes('live'), false, '기록 no longer has a second copy of the live table');
  const root = el('main', {}, render(ctx, route));
  assert.equal(root.querySelector('h1').textContent, '모니터링');
  const tab = root.querySelector('.log-tabs button#usage-tab-now[role=tab]');
  assert.equal(tab.getAttribute('aria-selected'), 'true'); assert.equal(tab.textContent, '지금');
  assert.deepEqual([...root.querySelectorAll('.log-tabs [role=tab][aria-selected=true]')].length, 1);
  assert.equal(root.querySelector('#usage-panel').getAttribute('aria-labelledby'), 'usage-tab-now');
  assert.deepEqual([...root.querySelectorAll('.usage-strip > div')].slice(0, 4).map(node => [node.querySelector('dt').textContent, node.querySelector('dd').textContent]),
    [['지금 로그인', usageLive.people + '명'], ['지금 접속 중', live.length + '개'], ['오늘 로그인', '3회'], ['오늘 연결', '2회']], 'The summary row counts the same connections the table lists');
  const panels = [...root.querySelectorAll('#usage-panel section.panel')];
  assert.deepEqual(panels.map(panel => panel.querySelector('h2').textContent), ['지금 접속 중' + live.length, '지금 로그인' + logins.length]);
  const table = panels[0].querySelector('table');
  if (!live.length) { assert.deepEqual(calls, []); return { root, rows: [], panels }; }
  assert.ok(table, 'The actual current-connections table must render');
  const headers = [...table.querySelectorAll('thead th')].map(cell => cell.textContent);
  assert.deepEqual(headers, ['사용자', '데스크톱', '클라이언트', '주소', '연결한 지', '화질'], 'every row is connected: no 상태 column');
  assert.equal(table.querySelector('.pill,.state-dot'), null);
  const rows = [...table.querySelectorAll('tbody tr')].map(row => [...row.querySelectorAll('td')]);
  assert.equal(rows.length, live.length, 'One row per relayed connection');
  for (const cells of rows) {
    assert.equal(cells.length, headers.length, 'Every live row must align with all six column headings');
    assert.deepEqual(cells.map(cell => cell.getAttribute('data-label')), headers, 'Narrow-screen labels must match the same actual columns');
  }
  assert.deepEqual(calls, [], 'Rendering observations never initiates a load or mutation');
  return { root, rows, panels };
}
const connection = extra => ({ sessionId: 'stream-fixture', account: 'fixture-user', desktopId: 'fixture-vm', desktopName: 'FIXTURE-PC', mode: 'stream', since: new Date(Date.now() - 42 * 60000).toISOString(), ready: true, platform: 'windows', version: '1.0.41+7', address: '203.0.113.7', activeMs: null, ...extra });

test('a stream row shows who, which desktop, the client and address the server recorded, since when, and the latest reported window', t => {
  const early = '2026-11-06T00:00:10.000Z', later = '2026-11-06T00:00:20.000Z';
  const f = fixture(t, {
    live: [connection()],
    logins: [{ account: 'fixture-user', since: new Date(Date.now() - 3 * 3600000).toISOString(), platform: 'windows', version: '1.0.41', address: '203.0.113.7', connections: 1 }],
    stats: [{ sessionId: 'stream-fixture', desktopId: 'fixture-vm', account: 'fixture-user', windows: [
      { time: early, values: { decodeFps10: 100, rttMs: 99 } },
      { time: later, values: { textureFps10: 599, decodeFps10: 600, rttMs: 12 } },
    ] }, { sessionId: 'different-session', windows: [{ time: later, values: { textureFps10: 10, rttMs: 900 } }] }],
  });
  const cells = f.rows[0];
  assert.equal(cells[0].querySelector('a').getAttribute('href'), '#/user/fixture-user');
  assert.equal(cells[1].querySelector('a').getAttribute('href'), '#/desktop/fixture-vm');
  assert.equal(cells[1].textContent, 'FIXTURE-PC');
  assert.equal(cells[2].textContent, 'Windows · 1.0.41', 'the build number of a version is not shown');
  assert.equal(cells[3].textContent, '203.0.113.7');
  assert.match(cells[4].textContent, /^42분 \(.+부터\)$/, 'how long, and since when, on one line'); assert.ok(Number.isFinite(Date.parse(cells[4].querySelector('time').getAttribute('datetime'))));
  // 화질 is the last reported window, and nothing else.
  assert.equal(cells[5].textContent, '59.9 fps · 12 ms');
  assert.equal(cells[5].querySelector('a').getAttribute('href'), '#/log/quality', 'the numbers open 기록 › 화질·속도');
  assert.doesNotMatch(cells.map(cell => cell.textContent).join(' '), /TCP|UDP|근무/);
  const login = [...f.panels[1].querySelectorAll('tbody tr td')].map(cell => cell.textContent);
  assert.deepEqual([login[0], login[1], login[2], login[4]], ['fixture-user', 'Windows · 1.0.41', '203.0.113.7', '연결 1개']); assert.equal(login.length, 5);
  assert.doesNotMatch(f.root.textContent, /입력/, 'nothing about a person\'s input is shown');
  assert.match(login[3], /^3시간 0분 \(.+부터\)$/);
  assert.match(f.root.querySelector('.usage-note').textContent, /자리를 비운 시간도 들어 있어요/);
  assert.match(f.root.querySelector('.usage-note').textContent, /로그인·연결 기록은 2년 동안 보관해요/);
  assert.match(f.root.querySelector('.usage-note').textContent, /서버 시간대\(Asia\/Seoul\)/);
});

test('a connection still preparing and absent telemetry keep their own cells and invent no numbers', t => {
  const f = fixture(t, { live: [
    connection({ sessionId: 'preparing', ready: false }),
    connection({ sessionId: 'no-report' }),
    connection({ sessionId: 'empty-window' }),
  ], stats: [{ sessionId: 'empty-window', windows: [{ time: '2026-11-06T00:00:00Z', values: {} }] }] });
  assert.deepEqual(f.rows.map(cells => cells[5].textContent), ['준비 중', '—', '— · —'], 'one still connecting says so where its numbers would be');
  assert.equal(f.rows[0][5].querySelector('a'), null, 'a connection that is not ready has no numbers to open');
});

test('what the server does not know stays unknown: no account, no desktop in the inventory, no client, address or start', t => {
  const f = fixture(t, {
    live: [connection({ sessionId: 'unknown-values', account: null, desktopId: 'missing-vm', desktopName: 'missing-vm', since: null, platform: null, version: null, address: null, ready: false }),
      connection({ sessionId: 'future', account: 'gone-user', desktopId: 'missing-vm', desktopName: '옛 데스크톱', platform: '<future-platform>', version: null, address: null })],
  });
  const [none, gone] = f.rows;
  assert.equal(none[0].textContent, '알 수 없는 계정'); assert.equal(none[1].textContent, 'missing-vm');
  assert.equal(none[1].querySelector('a'), null, 'A desktop the inventory does not have is not a link');
  assert.equal(none[2].textContent, '알 수 없음'); assert.equal(none[3].textContent, '기록 없음'); assert.equal(none[4].textContent, '기록 없음');
  assert.equal(gone[0].textContent, 'gone-user'); assert.equal(gone[0].querySelector('a'), null, 'An account the inventory does not have is not a link');
  assert.equal(gone[1].textContent, '옛 데스크톱'); assert.equal(gone[2].textContent, '알 수 없음');
  assert.doesNotMatch(f.root.textContent, /future|TCP|UDP/);
  assert.equal(f.root.querySelector('future-platform'), null);
  const empty = fixture(t, { live: [] });
  assert.deepEqual([...empty.root.querySelectorAll('#usage-panel .empty strong')].map(node => node.textContent), ['지금 접속 중인 사람이 없어요', '지금 로그인한 사람이 없어요']);
});
