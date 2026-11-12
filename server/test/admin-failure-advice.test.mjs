import test from 'node:test';
import assert from 'node:assert/strict';
import { parseHTML } from 'linkedom';
import { el } from '../admin/js/dom.js';
import { clientEntry, entryFacts, failureAdviceFor, hostEntries, logBoard, logWindow } from '../admin/js/timeline.js';
import { failureAdvice, clientCodes } from '../admin/js/labels.js';

// DOM text only: a failed connection as a row of the 기록 board (원인 in a few words, 조치 필요 when the operator has
// something to do) and its detail (the fix, the code and the raw values).
function context(t, desktops = []) {
  const previous = { document: globalThis.document, Node: globalThis.Node };
  const window = parseHTML('<!doctype html><html><body></body></html>');
  Object.assign(globalThis, { document: window.document, Node: window.Node });
  t.after(() => Object.assign(globalThis, previous));
  return { state: { ui: {} }, render() {}, desktopById: id => desktops.find(d => d.id === id) ?? null, userByName: () => null,
    link: (text, page, id, attrs = {}) => el('a', { ...attrs, 'data-page': page, 'data-id': id }, text) };
}
const rowOf = (ctx, entry, options = {}) => [logBoard(ctx, [entry], options)].flat()[0].querySelector('tbody tr[data-entry]');
const cell = (row, name) => row.querySelector('td.col-' + name);
const factsOf = node => Object.fromEntries([...node.querySelectorAll('.facts>div')].map(div => [div.querySelector('dt').textContent, div.querySelector('dd').textContent]));
const failed = (code, values = { ok: false, elapsedMs: 1200 }) => clientEntry({ time: '2026-11-09T08:02:11Z', event: 'streamStart', mode: 'stream', ...(code ? { code } : {}), values, account: 'fixture-user', version: '1.0.41', runId: 'f'.repeat(32), sequence: 1 });

test('A failure the operator can fix: the row says the cause in a few words and 조치 필요; the fix and the code are in its detail', t => {
  const ctx = context(t), entry = failed('host_offline'), row = rowOf(ctx, entry);
  assert.equal(row.getAttribute('data-advice'), 'admin');
  assert.equal(cell(row, 'who').textContent, 'fixture-user');
  assert.equal(cell(row, 'event').textContent, '스트리밍 시작');
  assert.equal(cell(row, 'result').textContent, '실패'); assert.ok(cell(row, 'result').querySelector('.is-bad'), 'a failure is red text');
  assert.equal(cell(row, 'cause').firstChild.textContent, '데스크톱이 서버에 응답하지 않았어요.');
  assert.equal(cell(row, 'cause').querySelector('small.log-todo').textContent, '조치 필요');
  assert.doesNotMatch(row.textContent, /무슨 일|해결|관리자가 할 일|host_offline/, 'no labels, no fix sentence and no raw code in the row');
  assert.equal(row.querySelector('.pill,.icon'), null, 'no badge and no icon');
  const facts = factsOf(entryFacts(ctx, entry));
  assert.equal(facts['원인'], '데스크톱이 서버에 응답하지 않았어요.');
  assert.equal(facts['조치'], '데스크톱이 켜져 있고 Windows에 로그인돼 있는지 확인하거나 다시 시작하세요.');
  assert.equal(facts['코드'], clientCodes.host_offline + ' host_offline', 'the code with its name');
  assert.match(facts['내용'], /소요\(ms\) 1,200/);
});

test('A row opens its detail: a window on 기록 (데스크톱 열기 only for a desktop in the list), or a line under the row in a window', t => {
  const desktop = { id: 'fixture-desktop', kind: 'vm', pve_vmid: 104, name: 'FIXTURE-DESKTOP' };
  const ctx = context(t, [desktop]), entry = { ...failed('host_offline'), desktopId: desktop.id };
  let opened = null;
  const row = rowOf(ctx, entry, { open: value => { opened = value; } });
  assert.equal(cell(row, 'desktop').textContent, 'FIXTURE-DESKTOP');
  row.querySelector('td.col-manage button').click();
  assert.equal(opened, entry);
  const frame = logWindow(ctx, entry, () => {});
  assert.equal(frame.querySelector('#detail-title').textContent, '스트리밍 시작');
  const open = frame.querySelector('.win-actions a');
  assert.equal(open.textContent, '데스크톱 열기');
  assert.deepEqual([open.getAttribute('data-page'), open.getAttribute('data-id')], ['desktop', desktop.id]);
  assert.equal(logWindow(ctx, { ...entry, desktopId: 'gone-desktop' }, () => {}).querySelector('.win-actions a'), null, 'a desktop that is not in the list cannot be opened');
  // Inside a window: the detail opens under the row.
  const inline = () => [logBoard(ctx, [entry], { inline: 'fixture' })].flat()[0];
  assert.equal(inline().querySelector('tr.board-detail'), null);
  inline().querySelector('td.col-manage button').click();
  assert.equal(ctx.state.ui.logRow, 'fixture|' + entry.key);
  assert.match(inline().querySelector('tr.board-detail').textContent, /데스크톱이 켜져 있고/);
});

test('A cause on the user side asks nothing of the operator and offers no fix', t => {
  const ctx = context(t);
  for (const code of ['network_error', 'other_user', 'session_expired', 'operation_cancelled']) {
    const entry = failed(code), row = rowOf(ctx, entry);
    assert.equal(row.getAttribute('data-advice'), 'user', code);
    assert.equal(cell(row, 'cause').querySelector('.log-todo'), null, code);
    assert.equal(factsOf(entryFacts(ctx, entry))['조치'], undefined, code);
  }
  const login = { key: 'audit:1', time: Date.now(), source: 'audit', event: 'login_failed', title: '로그인 실패', detail: ['입력한 아이디 fixture-user'], failure: true, actor: 'fixture-user' };
  const row = rowOf(ctx, login);
  assert.equal(cell(row, 'who').textContent, 'fixture-user');
  assert.equal(cell(row, 'cause').textContent, '아이디나 비밀번호가 맞지 않았어요.');
});

test('A code without advice is named by its label or 원인 미확인, keeps the raw code in the detail and invents no fix', t => {
  const ctx = context(t);
  const labelled = failed('initial_frame_timeout'), row = rowOf(ctx, labelled);
  assert.equal(row.getAttribute('data-advice'), 'unknown');
  assert.equal(cell(row, 'cause').textContent, clientCodes.initial_frame_timeout);
  assert.equal(factsOf(entryFacts(ctx, labelled))['코드'], 'initial_frame_timeout');
  assert.equal(factsOf(entryFacts(ctx, labelled))['조치'], undefined);
  for (const code of ['unknown_failure', 'not_in_any_table', null]) {
    const node = rowOf(ctx, failed(code));
    assert.equal(node.getAttribute('data-advice'), 'unknown', String(code));
    assert.equal(cell(node, 'cause').textContent, '원인 미확인', String(code));
    assert.doesNotMatch(node.textContent, /undefined|null|문서화|설명되지/);
  }
});

test('Advice covers only known client codes, and success or Host records keep their own wording', t => {
  for (const [code, advice] of Object.entries(failureAdvice)) {
    assert.ok(code === 'login_failed' || code in clientCodes, code + ' is a client code');
    assert.ok(['admin', 'user', 'wait'].includes(advice.who), code); assert.ok(advice.what.length > 5);
    assert.equal(advice.who === 'admin', typeof advice.fix === 'string', code + ': only an operator task has a fix');
  }
  assert.equal(failureAdviceFor(failed('host_offline', { ok: true })), null, 'a code beside ok:true is information');
  const [locked] = hostEntries([{ desktopId: 'fixture-pc', diagnostics: { version: '1.0.43', events: [{ time: '2026-11-09T08:02:09Z', event: 'hostAction', values: { action: 200, stage: 244, error: 3, ok: true } }] } }]);
  assert.equal(failureAdviceFor(locked), null);
  const row = rowOf(context(t), locked, { cause: false });
  assert.equal(row.getAttribute('data-advice'), null); assert.equal(cell(row, 'result').textContent, '정상');
  assert.match(row.textContent, /보안 확인 창/, 'the Host label names the security prompt');
});
