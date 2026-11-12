import test from 'node:test';
import assert from 'node:assert/strict';
import {parseHTML} from 'linkedom';
import {hostEntries, logBoard, entryFacts, streamGpuObservation} from '../admin/js/timeline.js';

// A record as a row of the 기록 board and as its detail (the form table its row opens).
const boardRow = (ctx, entry) => [logBoard(ctx, [entry], {cause: false})].flat()[0].querySelector('tbody tr[data-entry]');
const factsOf = (ctx, entry) => Object.fromEntries([...entryFacts(ctx, entry).querySelectorAll('.facts>div')].map(div => [div.querySelector('dt').textContent, div.querySelector('dd').textContent]));

const values = (stage, error, extra = {}) => ({action: 200, stage, error, ok: false, ...extra});
const row = (value, event = 'hostAction') => hostEntries([{desktopId: 'fixture', diagnostics: {version: '1.0.12', events: [
  {time: '2026-11-04T15:00:00Z', event, values: value},
]}}])[0];

test('observed priority3413 is an applied class, not Windows error3413', () => {
  const entry = row(values(242, 3413));
  assert.equal(entry.failure, false);
  assert.match(entry.title, /GPU 우선순위 관측/);
  assert.match(entry.detail.join(' '), /요청 실시간 · 적용 실시간/);
  assert.match(entry.detail.join(' '), /HAGS 꺼짐/);
  assert.match(entry.detail.join(' '), /GPU 스레드 상향 확인/);
  assert.match(entry.detail.join(' '), /당시 관측이며 현재 GPU 사용이나 영상 선명도를 뜻하지 않아요/);
  assert.doesNotMatch(entry.detail.join(' '), /Windows 오류|오류 번호|현재 GPU 사용 중/);
  assert.match(entry.rawDiagnostic, /error=3413/);
});

test('confirmed high, realtime fallback, and already sufficient classes remain observations', () => {
  // Unknown HAGS/high; HAGS on/high; HAGS off/realtime refused but high applied;
  // realtime applied without thread elevation; previous sufficient realtime/high.
  for (const packed of [68, 836, 325, 1365, 4597, 4340]) {
    const entry = row(values(242, packed));
    assert.equal(entry.failure, false, String(packed));
    assert.doesNotMatch(entry.title, /확인 필요/);
  }
  assert.match(row(values(242, 325)).detail[0], /요청 실시간 · 적용 높음/);
  assert.match(row(values(242, 4597)).detail[0], /기존 값 유지\(요청 이상\)/);
});

test('unconfirmed priority application stays a failure even when a GPU thread was raised', () => {
  for (const packed of [501, 244, 501 | 0x800]) {
    const entry = row(values(242, packed));
    assert.equal(entry.failure, true);
    assert.match(entry.detail[0], /적용 확인 안 됨/);
  }
});

test('priority uint32 and reserved-bit violations never become healthy observations', () => {
  for (const packed of [-1, 3413.5, '3413', null, NaN, Infinity, 2 ** 32, 0x80000d55, 3413 | 0x2000]) {
    const entry = row(values(242, packed));
    assert.equal(entry.failure, true, String(packed));
    assert.match(entry.title, /확인 필요/);
  }
});

test('contradictory wanted, applied, HAGS and already bits stay unconfirmed', () => {
  // Bad wanted/applied classes; HAGS on without known; wanted4 with HAGS off;
  // wanted5 without observed HAGS off; applied5 for wanted4; already+new apply.
  for (const packed of [0x33, 0x64, 0x244, 0x144, 0x55, 0x54, 3413 | 0x1000]) {
    const entry = row(values(242, packed));
    assert.equal(entry.failure, true, String(packed));
    assert.match(entry.title, /확인 필요/);
  }
});

test('only the released four-field typed shape is interpreted', () => {
  const missing = values(242, 3413); delete missing.error;
  for (const input of [missing, values(242, 3413, {extra: 1}), values(242, 3413, {ok: 0}), values(243, 1, {ok: 'false'})]) {
    assert.match(streamGpuObservation(input).title, /확인 필요/);
  }
});

test('only exact texture active record1 becomes a healthy texture observation', () => {
  const active = row(values(243, 1));
  assert.equal(active.failure, false);
  assert.match(active.detail.join(' '), /당시 캡처와 인코더의 GPU 텍스처 경로 활성/);
  // Broken GPU paths (5-8, with their failed step) and records that are not a released reason stay failures.
  for (const packed of [0, 5, 6, 7, 8, 5 | (3 << 8), 257, 65537, 10, '1', -1]) {
    assert.equal(row(values(243, packed)).failure, true, String(packed));
  }
});

test('243 reasons for a CPU path are named observations, not failures', () => {
  const cases = {2: /CPU\(WARP·GDI\)/, 3: /크기 조정/, 4: /다른 GPU/, 9: /텍스처 입력을 지원하지 않음/};
  for (const [reason, text] of Object.entries(cases)) {
    for (const ok of [true, false]) {  // Older Hosts recorded every 243 ok:false.
      const entry = row(values(243, Number(reason), {ok}));
      assert.equal(entry.failure, false, reason);
      assert.doesNotMatch(entry.title, /확인 필요/);
      assert.match(entry.detail.join(' '), text);
    }
  }
  const broken = row(values(243, 6 | (9 << 8) | (0x57 << 16)));
  assert.equal(broken.failure, true);
  assert.match(broken.detail.join(' '), /GPU 변환 중 실패.*실패 단계 9/);
  assert.match(row(values(243, 2 | (1 << 8))).title, /확인 필요/, 'a CPU-path reason never carries a failed step');
});

test('244 secure desktop notice records read as what happened', () => {
  const shown = row(values(244, 1, {ok: true}));
  assert.equal(shown.failure, false); assert.match(shown.title, /보안 확인 창/); assert.match(shown.detail.join(" "), /데스크톱에 UAC 같은 보안 확인 창이 떠서 안내 화면으로 바꿈\(연결 유지\)/);
  const back = row(values(244, 2 | (121 << 8), {ok: true}));
  assert.match(back.detail.join(' '), /원래 화면으로 돌아옴.*보안 확인 창 121초/);
  for (const [what, text] of [[3, /잠겨/], [4, /10분/]]) {
    const ended = row(values(244, what));
    assert.equal(ended.failure, false, 'a decided ending is no product failure'); assert.match(ended.detail.join(' '), text);
  }
  assert.match(row(values(244, 3 | (5 << 8))).title, /확인 필요/);
});

test('282 render re-arm and 291 capability records are labelled', () => {
  const requested = row({action: 282, stage: 1, ok: true});
  assert.match(requested.title, /GPU 재준비/); assert.equal(requested.failure, false);
  assert.match(row({action: 282, stage: 2, ok: true}).detail.join(' '), /다음 연결부터 GPU/);
  assert.equal(row({action: 282, stage: 2, ok: false}).failure, true);
  const waiting = row({action: 282, stage: 3, ok: false, error: 4});
  assert.equal(waiting.failure, false); assert.match(waiting.detail.join(' '), /대기: 스트리밍/);
  assert.equal(row({action: 282, stage: 3, ok: false, error: 1}).failure, false);
  assert.equal(row({action: 282, stage: 3, ok: false, error: 7}).failure, true);
  const capability = row({action: 291, ok: true, active: false, changed: true});
  assert.match(capability.title, /서버 기능 확인/); assert.equal(capability.failure, false);
  assert.match(capability.detail.join(' '), /로그인 화면 없음\(정상\)/);
});

test('capability, other preparation, USB and display failures are not suppressed', () => {
  for (const input of [
    {action: 255, stage: 1, error: 37, cause: 6968, ok: false},
    values(299, 123),
    {action: 250, stage: 24, error: 32, ok: false},
    {action: 214, stage: 8, error: 87, ok: false},
  ]) {
    assert.equal(streamGpuObservation(input), null);
    assert.equal(row(input).failure, true);
  }
  assert.equal(row(values(243, 1), 'streamStart').failure, true, 'different events cannot borrow the hostAction interpretation');
});

test('original records are immutable and unrecognized text never enters diagnostic descriptions', () => {
  for (const input of [values(242, 3413), values(243, 1), values(242, 3413, {secret: 'DO_NOT_EXPORT'})]) {
    Object.freeze(input);
    const before = JSON.stringify(input), entry = row(input);
    assert.equal(JSON.stringify(input), before);
    assert.doesNotMatch(JSON.stringify(entry), /DO_NOT_EXPORT/);
    assert.equal(entry.version, '1.0.12');
  }
});

test('timeline separates healthy observation badges and keeps raw numbers behind details', () => {
  const previousDocument = globalThis.document, previousNode = globalThis.Node;
  const dom = parseHTML('<html><body></body></html>');
  globalThis.document = dom.document; globalThis.Node = dom.Node;
  try {
    const ctx = {state: {ui: {}}, render() {}, desktopById: () => null, userByName: () => null, link: text => text};
    const normal = boardRow(ctx, row(values(243, 1)));
    const failed = boardRow(ctx, row(values(242, 501)));
    assert.equal(normal.classList.contains('is-failure'), false); assert.equal(normal.querySelector('td.col-result').textContent, '정상');
    assert.equal(failed.classList.contains('is-failure'), true); assert.equal(failed.querySelector('td.col-result').textContent, '실패');
    assert.doesNotMatch(normal.textContent, /error=1/, 'the raw line is not in the row');
    const facts = factsOf(ctx, row(values(243, 1)));
    assert.match(facts['진단 원본'], /error=1/);
    assert.doesNotMatch(facts['내용'], /error=1/);
  } finally {
    globalThis.document = previousDocument; globalThis.Node = previousNode;
  }
});

test('244 values 5-8: the real prompt in the stream, or the notice when it could not be shown', () => {
  const streamed = row(values(244, 5, {ok: true}));
  assert.equal(streamed.failure, false); assert.match(streamed.title, /보안 확인 창/);
  assert.match(streamed.detail.join(' '), /스트림에 보여 줌.*스트림 안에서 답할 수 있음\(연결 유지\)/);
  for (const [what, why] of [[6, /도우미를 시작하지 못함/], [7, /도우미가 도중에 끊김/], [8, /약 2초 안에 화면이 오지 않음/]]) {
    for (const ok of [true, false]) {
      const entry = row(values(244, what, {ok}));
      assert.equal(entry.failure, false, 'the stream is kept: nothing for the operator to fix');
      assert.match(entry.detail.join(' '), /스트림에 보여 주지 못해 안내 화면을 표시함/); assert.match(entry.detail.join(' '), why); assert.match(entry.detail.join(' '), /연결 유지/);
      assert.match(entry.rawDiagnostic, new RegExp('stage=244; error=' + what + ';'));
    }
  }
  assert.match(row(values(244, 9, {ok: true})).title, /확인 필요/, 'an unknown value is not given a meaning');
  assert.match(row(values(244, 6 | (3 << 8), {ok: true})).title, /확인 필요/, 'seconds belong to the return only');
});

test('245 GPU encoder return: attempt, named GPU code, and CPU encoder kept without a failed stream', () => {
  const pack = (kind, attempt = 0, gpu = 0) => kind | (attempt << 8) | (gpu << 16);
  const started = row(values(245, pack(1, 1, 41), {ok: true}));
  assert.equal(started.title, 'Host 작업 · GPU 인코더 복귀'); assert.equal(started.failure, false);
  assert.deepEqual(started.detail.slice(0, 3), ['GPU 인코더로 돌아가기를 시도함', '1번째 시도', 'GPU 출력 응답 시간 초과']);
  assert.match(row(values(245, pack(2, 2), {ok: true})).detail.join(' '), /GPU 인코더로 돌아옴 2번째 시도/);
  for (const [kind, text] of [[3, /다시 열지 못함 · CPU 인코더로 계속 스트리밍/], [4, /첫 화면이 실패함 · CPU 인코더로 계속 스트리밍/]]) {
    const entry = row(values(245, pack(kind, 3, 38)));
    assert.equal(entry.failure, false, 'x264 keeps the stream running'); assert.match(entry.detail.join(' '), text);
    assert.match(entry.detail.join(' '), /3번째 시도/); assert.match(entry.detail.join(' '), /GPU 초기화 실패/);
  }
  const stopped = row(values(245, pack(5, 3)));
  assert.equal(stopped.failure, false); assert.match(stopped.detail.join(' '), /더 시도하지 않음 · 이 연결은 끝날 때까지 CPU 인코더로 스트리밍/);
  // A GPU code outside the table is not guessed: it stays in the raw line.
  const unnamed = row(values(245, pack(3, 1, 99)));
  assert.deepEqual(unnamed.detail.slice(0, 2), ['GPU 인코더를 다시 열지 못함 · CPU 인코더로 계속 스트리밍', '1번째 시도']);
  assert.doesNotMatch(unnamed.detail.join(' '), /99/); assert.match(unnamed.rawDiagnostic, new RegExp('error=' + pack(3, 1, 99)));
  assert.match(row(values(245, pack(6, 1))).title, /GPU 인코더 복귀 · 확인 필요/);
  assert.match(row(values(245, pack(0))).title, /확인 필요/);
  assert.equal(streamGpuObservation({action: 200, stage: 246, error: 1, ok: true}), null);
});

test('245 and the new 244 values render as plain entries with the raw line folded', t => {
  const previous = {document: globalThis.document, Node: globalThis.Node};
  const window = parseHTML('<!doctype html><html><body></body></html>');
  Object.assign(globalThis, {document: window.document, Node: window.Node});
  t.after(() => Object.assign(globalThis, previous));
  const ctx = {state: {ui: {}}, render() {}, desktopById: () => null, userByName: () => null, link: (text) => window.document.createTextNode(String(text))};
  for (const value of [values(245, 5 | (3 << 8)), values(244, 7, {ok: true})]) {
    const node = boardRow(ctx, row(value));
    assert.equal(node.classList.contains('is-failure'), false); assert.equal(node.getAttribute('data-advice'), null);
    assert.doesNotMatch(node.textContent, /진단 원본|error=/);
    const facts = factsOf(ctx, row(value));
    assert.match(facts['진단 원본'], /error=/); assert.doesNotMatch(facts['내용'], /진단 원본|error=/);
  }
});
