import test from 'node:test';
import assert from 'node:assert/strict';
import { hostEntries } from '../admin/js/timeline.js';

test('Encoder usage history names actual vendors without promoting initialization or CPU fallback to GPU', () => {
  const values = [
    { stage: 3, format: 0x10de, owned: true }, { stage: 4, format: 0x1002, owned: true },
    { stage: 5, format: 0x8086, owned: true }, { stage: 1, format: 0, owned: false, fallback: 43 },
    { stage: 2, format: 0, owned: false }, { stage: 3, format: 0x10de, owned: false },
    { stage: 3, format: 0x1002, owned: true }, { stage: 3, format: 0x10de, owned: true, active: false },
    { stage: '3', format: 0x10de, owned: true }, { stage: 3, format: 0x10de, owned: true, fallback: 31 },
  ];
  const entries = hostEntries([{ desktopId: 'fixture-pc', diagnostics: { version: 'fixture', events: values.map(v => ({
    time: '2026-10-30T01:02:03.004Z', event: 'hostAction', values: { action: 254, ok: true, active: true, fallback: 0, error: 0, items: 0, ...v },
  })) } }]);
  assert.deepEqual(entries.slice(0, 5).map(v => v.title), ['인코더 사용 · NVIDIA NVENC', '인코더 사용 · AMD AMF', '인코더 사용 · Intel QSV', '인코더 사용 · CPU x264', '인코더 사용 · CPU Windows MFT']);
  assert.ok(entries[3].detail.includes('GPU 장치 연결이 끊김'));
  assert.ok(entries[3].detail.includes('당시 CPU H.264 출력 확인'));
  assert.equal(entries[3].failure, false, 'successful CPU fallback is not a failed frame or GPU success');
  for (const entry of entries.slice(5)) {
    assert.match(entry.title, /출력 미확인/);
    assert.ok(!entry.title.startsWith('인코더 사용'));
    assert.ok(!entry.detail.includes('당시 GPU H.264 출력 확인'));
  }
  assert.ok(entries.every(v => v.detail.includes('현재 연결 상태를 뜻하지 않아요.')));
  assert.equal(entries[0].time, Date.parse('2026-10-30T01:02:03.004Z'));
});

test('Host action codes are named and the display restore detail is readable', () => {
  const [named, unknown] = hostEntries([{ desktopId: 'pve-149', diagnostics: { version: '0.2.63', events: [
    { time: '2026-10-26T00:40:02.309Z', event: 'hostAction', values: { action: 214, ok: false, stage: 7, missing: 1, consoleExpected: true } },
    { time: '2026-10-26T00:41:02.309Z', event: 'hostAction', values: { action: 999, ok: true } },
  ] } }]);
  assert.equal(named.title, 'Host 작업 · 화면 구성 복원');
  assert.deepEqual(named.detail, ['성공 아니요', '단계 7', '미복원 1', '콘솔 복원 필요 예']);
  assert.equal(named.failure, true);
  // An unknown code keeps the generic title and its number.
  assert.equal(unknown.title, 'Host 작업');
  assert.ok(unknown.detail.includes('작업 999'));
});

test('A remote Host update record names its state and reason', () => {
  const [rejected] = hostEntries([{ desktopId: 'pve-149', diagnostics: { version: '0.2.62', events: [
    { time: '2026-10-26T01:14:14.000Z', event: 'hostAction', values: { action: 230, error: 4, ok: false, stage: 8 } },
  ] } }]);
  assert.equal(rejected.title, 'Host 작업 · 원격 Host 업데이트');
  assert.deepEqual(rejected.detail, ['거절', '이미 같은 버전이에요.']);
});

test('Print records are told apart from media drops, also as the 240 that Hosts 0.2.73-0.2.76 sent', () => {
  const [legacy, current, dropped] = hostEntries([{ desktopId: 'pve-170', diagnostics: { version: '0.2.76', events: [
    { time: '2026-10-26T21:20:52.826Z', event: 'hostAction', values: { action: 240, error: 0, expected: 0, ok: false, stage: 30 } },
    { time: '2026-10-26T21:21:52.826Z', event: 'hostAction', values: { action: 242, error: 0, expected: 0, ok: true, stage: 21 } },
    { time: '2026-10-26T21:22:52.826Z', event: 'hostAction', values: { action: 240, stage: 1, dropped: 12 } },
  ] } }]);
  assert.equal(legacy.title, 'Host 작업 · 인쇄');
  assert.equal(legacy.detail[0], '기본 프린터 지정 실패');
  assert.equal(legacy.failure, true);
  assert.equal(current.title, 'Host 작업 · 인쇄');
  assert.equal(current.detail[0], 'PC에서 인쇄함');
  assert.equal(dropped.title, 'Host 작업 · 전송 지연 · 프레임 건너뜀');
});

test('A failed default printer names its reason and Windows error; drive records name stage, reason and cause', () => {
  const [refused, unlisted, mapped, failed] = hostEntries([{ desktopId: 'pve-170', diagnostics: { version: '0.2.80', events: [
    { time: '2026-10-27T01:00:00.000Z', event: 'hostAction', values: { action: 242, error: 5, expected: 0, ok: false, stage: 30, cause: 1801 } },
    { time: '2026-10-27T01:00:01.000Z', event: 'hostAction', values: { action: 242, error: 99, expected: 0, ok: false, stage: 30 } },
    { time: '2026-10-27T01:00:02.000Z', event: 'hostAction', values: { action: 247, stage: 2, ok: true, expected: 1, missing: 0, error: 0 } },
    { time: '2026-10-27T01:00:03.000Z', event: 'hostAction', values: { action: 247, stage: 3, ok: false, expected: 2, missing: 1, error: 3, cause: 1222 } },
  ] } }]);
  assert.deepEqual(refused.detail.slice(0, 2), ['기본 프린터 지정 실패', 'Windows가 기본 프린터 지정을 거부함']);
  assert.equal(refused.detail.at(-1), 'Windows 오류 1801');
  assert.equal(unlisted.detail[0], '기본 프린터 지정 실패');
  assert.ok(unlisted.detail[1], 'an unknown reason still shows its number');
  assert.equal(mapped.title, 'Host 작업 · 공유 폴더');
  assert.equal(mapped.detail[0], '드라이브 연결됨');
  assert.equal(failed.detail[0], '드라이브 연결 실패');
  assert.ok(failed.detail.includes('드라이브 연결 실패'), 'the reason code 3 is named');
  assert.equal(failed.detail.at(-1), 'Windows 오류 1222');
  assert.equal(failed.failure, true);
});

test('Host display adapter cleanup and USB controller records are named with their counts', () => {
  const [inventory, cleanup, adopted, replaced, unavailable] = hostEntries([{ desktopId: 'pve-170', diagnostics: { version: '0.2.139', events: [
    { time: '2026-11-02T01:00:00.000Z', event: 'hostAction', values: { action: 273, stage: 0, expected: 1, stale: 2, missingOwned: 0, fallback: 1, items: 1, ok: false } },
    { time: '2026-11-02T01:00:01.000Z', event: 'hostAction', values: { action: 273, stage: 1, items: 2, missing: 1, error: 0 } },
    { time: '2026-11-02T01:00:02.000Z', event: 'hostAction', values: { action: 250, stage: 29 } },
    { time: '2026-11-02T01:00:03.000Z', event: 'hostAction', values: { action: 250, stage: 30, expected: 2 } },
    { time: '2026-11-02T01:00:04.000Z', event: 'hostAction', values: { action: 250, stage: 8, expected: 5 } },
  ] } }]);
  assert.equal(inventory.title, 'Host 작업 · 가상 화면 장치 정리');
  assert.deepEqual(inventory.detail, ['가상 화면 장치 목록 바뀜', '사용 중 1', '이전 버전 2', '중복 0', '확인 안 됨 1', '다른 제품 1']);
  assert.deepEqual(cleanup.detail, ['자동 정리', '제거 2', '다시 시작 대기 1', '실패 0']);
  assert.equal(adopted.detail[0], 'Multitap USB 컨트롤러를 넘겨받음');
  assert.deepEqual(replaced.detail.slice(0, 2), ['Host 설정에서 USB 컨트롤러 교체', '제거 2개']);
  assert.deepEqual(unavailable.detail.slice(0, 2), ['USB 컨트롤러 사용 불가', '컨트롤러: 다른 프로그램 것']);
});

test('WinFsp drive stages are named; a failed WinFsp mount shows the NTSTATUS behind the fallback to WebDAV', () => {
  const [mounted, fallback, unusable, helper] = hostEntries([{ desktopId: 'pve-170', diagnostics: { version: '0.2.80', events: [
    { time: '2026-10-27T02:00:00.000Z', event: 'hostAction', values: { action: 247, stage: 13, ok: true, expected: 2, missing: 0, error: 0 } },
    { time: '2026-10-27T02:00:01.000Z', event: 'hostAction', values: { action: 247, stage: 14, ok: false, expected: 1, missing: 1, error: 3, cause: 3221225506 } },
    { time: '2026-10-27T02:00:02.000Z', event: 'hostAction', values: { action: 247, stage: 15, ok: false, expected: 1, missing: 1 } },
    { time: '2026-10-27T02:00:03.000Z', event: 'hostAction', values: { action: 247, stage: 16, ok: false, error: 12 } },
  ] } }]);
  assert.equal(mounted.detail[0], 'WinFsp 드라이브 연결');
  assert.equal(fallback.detail[0], 'WinFsp 연결 실패 · 기본 방식(WebDAV)으로 전환');
  assert.equal(fallback.detail.at(-1), 'Windows 오류 3221225506');
  assert.equal(fallback.failure, true);
  assert.equal(unusable.detail[0], 'WinFsp 사용 불가 · 기본 방식(WebDAV)으로 연결');
  assert.equal(helper.detail[0], 'WinFsp 도우미 종료 · 공유 폴더 끝남');
  assert.ok(helper.detail.includes('드라이브 도우미 실패'));
});

test('two-step sign-in failures and blocks read as failures with their count, minutes or the daily cap', async () => {
  const { auditEntry } = await import('../admin/js/timeline.js');
  const id = '11111111-2222-4333-8444-555555555555', users = new Map([[id, { id, username: 'alice' }]]), desktops = new Map();
  const entry = (event, subject) => auditEntry({ id: 1, created_at: 0, event, actor: id, subject }, users, desktops);
  const failed = entry('mfa_failed', id + ':5');
  assert.deepEqual([failed.title, failed.account, failed.failure, failed.detail], ['2단계 인증 실패', 'alice', true, ['실패 5회']]);
  assert.deepEqual(entry('mfa_locked', id + ':4').detail, ['4분 동안 막힘']); assert.equal(entry('mfa_locked', id + ':4').title, '2단계 인증 막힘');
  assert.deepEqual(entry('mfa_locked', id + ':day').detail, ['오늘은 복구 코드로만 로그인']);
  assert.deepEqual(entry('mfa_locked', id + ':<b>').detail, [], 'anything else adds nothing');
});
