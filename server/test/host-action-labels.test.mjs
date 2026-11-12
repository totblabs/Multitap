import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { hostEntries } from '../admin/js/timeline.js';
import * as labels from '../admin/js/labels.js';
import { safeDiagnosticReport } from '../src/host-diagnostics.mjs';

// The administrator's Host diagnostics must say what a record means in the product's words: a Console
// park (215), the input counts (200 stage 246) and the finish of an install (276) are not bare numbers, and the
// tables say 데스크톱 and 다시 시작, never "VM" for the remote machine or 재시작. These tests pin
// the tables to the Host's own definitions and the vocabulary to docs/i18n.md.
const read = path => readFileSync(new URL(path, import.meta.url), 'utf8');
const row = values => ({ time: '2026-11-10T06:49:42.000Z', event: 'hostAction', values });
const entriesOf = rows => hostEntries([{ desktopId: 'pve-174', diagnostics: { version: '1.0.53', events: rows } }]);
const shown = values => { const entry = entriesOf([row(values)])[0]; return { entry, text: entry.detail.join(' | ') }; };
const codesOf = (source, name) => {
  const table = new RegExp(`${name}[^{;]*\\{[\\s\\S]*?static const char\\* const codes\\[\\]=\\{([\\s\\S]*?)\\};`).exec(source);
  assert.ok(table, `${name} not found`);
  return [...table[1].matchAll(/"([a-z0-9_]*)"/g)].map(match => match[1]);
};

test('Every Host action that has its own table is named', () => {
  for (const action of [215, 276, 285]) assert.ok(labels.hostActions[action], `hostAction ${action} has no name`);
  assert.equal(labels.hostActions[215], 'Console 화면 넘김');
  assert.equal(labels.hostActions[276], '설치 마무리');
});

test('A Console park says what happened and why no worker could act', () => {
  const header = read('../../apps/windows_host/src/qemu_console_route.hpp');
  // The Host's numbers: the action, the refusals and the four causes.
  assert.match(header, /qemuConsoleParkAction = 215;/);
  const refusals = /enum class QemuConsoleParkRefusal : uint32_t \{([^}]*)\}/.exec(header)[1];
  assert.deepEqual([...refusals.matchAll(/([A-Za-z]+) = (\d+)/g)].map(match => [match[1], Number(match[2])]), [['none', 0], ['displayBusy', 1], ['cancelled', 2]]);
  assert.match(header, /return !facts\.console \? 1u : !facts\.unlocked \? 2u : !facts\.elevated \? 3u : 4u;/);
  for (const cause of [1, 2, 3, 4]) assert.ok(labels.hostConsoleParkCauseText[cause], `cause ${cause} has no label`);
  assert.deepEqual(Object.keys(labels.hostConsoleParkCauseText), ['1', '2', '3', '4']);
  assert.deepEqual(Object.keys(labels.hostConsoleParkRefusalText), ['1', '2']);
  assert.deepEqual(Object.keys(labels.hostConsoleParkText), ['1', '2']);

  const parked = shown({ action: 215, stage: 1, ok: true, cause: 2, error: 0, changed: true });
  assert.equal(parked.entry.title, 'Host 작업 · Console 화면 넘김');
  assert.equal(parked.text, 'Console에 화면을 넘김 | Windows 잠김'); assert.equal(parked.entry.failure, false);
  assert.equal(shown({ action: 215, stage: 1, ok: true, cause: 1, error: 0, changed: true }).text, 'Console에 화면을 넘김 | Windows에 로그인한 사람 없음');
  assert.equal(shown({ action: 215, stage: 1, ok: true, cause: 3, error: 0, changed: true }).text, 'Console에 화면을 넘김 | 일반 사용자 계정으로 로그인됨');
  assert.equal(shown({ action: 215, stage: 1, ok: true, cause: 4, error: 0, changed: true }).text, 'Console에 화면을 넘김 | 관리자 계정에 보안 확인 창이 떠 있음');
  // Refused before anything was stopped, the stop itself failed, and a park nobody waited for.
  const busy = shown({ action: 215, stage: 1, ok: false, cause: 1, error: 1, changed: false });
  assert.equal(busy.text, 'Console에 화면을 넘기지 못함 | 화면이 다른 작업에 쓰이는 중 | Windows에 로그인한 사람 없음'); assert.equal(busy.entry.failure, true);
  assert.equal(shown({ action: 215, stage: 1, ok: false, cause: 2, error: 1, changed: true }).text, 'Console에 화면을 넘기지 못함 | 가상 화면을 멈추지 못함 | Windows 잠김');
  assert.equal(shown({ action: 215, stage: 1, ok: false, cause: 3, error: 2, changed: true }).text, 'Console에 화면을 넘기지 못함 | 넘기는 중에 취소되거나 시간이 지남 | 일반 사용자 계정으로 로그인됨');
  const ended = shown({ action: 215, stage: 2, ok: true });
  assert.equal(ended.text, 'Console이 끝나 가상 화면을 되돌림'); assert.equal(ended.entry.failure, false);
  // A number this console does not know is shown as a number, never as another meaning.
  assert.match(shown({ action: 215, stage: 1, ok: false, cause: 9, error: 7, changed: false }).text, /오류 번호 7.*내부 원인 번호 9/);
});

test('Every documented input-count kind has a label, and every label is a documented kind', () => {
  const document = read('../../docs/system-input.md');
  const table = document.slice(document.indexOf('| kind | written by | value |'));
  const kinds = [...table.matchAll(/^\| (\d+) \|/gm)].map(match => Number(match[1]));
  assert.deepEqual(kinds, Array.from({ length: 25 }, (_, index) => index + 1), 'docs/system-input.md documents kinds 1 to 25');
  assert.deepEqual(Object.keys(labels.hostInputKindText).map(Number), kinds);
  // The Host's own constants are the same numbers.
  const helper = read('../../apps/windows_host/src/secure_desktop_scope.hpp'), injector = read('../../apps/windows_host/src/system_input.hpp');
  assert.match(helper, /inputRecordStage = 46;/);
  const constants = [...(helper + injector).matchAll(/(?:input246|record246)[A-Za-z]+ = (\d+)/g)].map(match => Number(match[1])).sort((a, b) => a - b);
  assert.deepEqual(constants, kinds);
});

test('Input counts are shown with their meaning, and stay observations', () => {
  const at = (kind, value) => shown({ action: 200, stage: 246, ok: true, error: kind | value << 8 });
  const received = at(11, 1423);
  assert.equal(received.entry.title, 'Host 작업 · 입력 기록');
  assert.equal(received.text, '시스템 입력 도우미가 받은 입력 | 1,423개'); assert.equal(received.entry.failure, false);
  assert.equal(at(1, 0).text, '보안 확인 창 도우미가 받은 입력 | 0개');
  assert.equal(at(16, 5).text, '마지막 SendInput 오류 | Windows 오류 5');
  assert.equal(at(6, 1400).text, '마지막 SendInput 오류 | Windows 오류 1400');
  assert.equal(at(10, 2 | 4 << 4).text, '앞에 있는 창의 권한이 더 높아 입력이 닿지 않음 | 작업 프로세스 보통 · 앞 창 시스템');
  assert.equal(at(10, 3 | 15 << 4).text, '앞에 있는 창의 권한이 더 높아 입력이 닿지 않음 | 작업 프로세스 관리자 · 앞 창 확인 못 함');
  assert.equal(at(18, 2 | 1 << 4).text, '시스템 입력 도우미를 시작함 | 작업 프로세스가 관리자 권한이 아님 | 작업 프로세스 권한 보통');
  assert.equal(at(19, 3 | 2 << 4).text, '시스템 입력 도우미를 시작하지 못함(작업 프로세스가 입력을 넣음) | 관리자 권한 작업 프로세스 · 확인 창이 사용자 바탕 화면에 뜸 | 작업 프로세스 권한 관리자');
  assert.equal(at(23, 7).text, '독립 모니터: 위치를 정하지 못해 넣지 않은 입력 | 7개');
  // The largest value the Host writes (24 bits) keeps its sign.
  assert.equal(shown({ action: 200, stage: 246, ok: true, error: 0xffffff00 + 12 }).text, 'Windows가 받아들인 입력 | 16,777,215개');
  assert.equal(at(77, 3).text, '종류 77 | 3개');
  // Another stage of action 200 is still the stream start, and a record without a number is left alone.
  assert.equal(shown({ action: 200, stage: 10, ok: true, admitMs: 12 }).entry.title, 'Host 작업 · 스트리밍 준비');
  assert.equal(shown({ action: 200, stage: 246, ok: true }).entry.title, 'Host 작업 · 스트리밍 준비');
});

test('The finish of an install follows the Host\'s error tables', () => {
  const display = codesOf(read('../../apps/windows_host/src/host_prerequisites.hpp'), 'inline unsigned errorNumber');
  const winfsp = codesOf(read('../../apps/windows_host/src/settings_rpc.hpp'), 'inline unsigned winfspErrorNumber');
  assert.deepEqual(labels.hostFinishDisplayCodes, display); assert.deepEqual(labels.hostFinishWinfspCodes, winfsp);
  assert.ok(display.length >= 22 && winfsp.length >= 12);
  for (const code of [...display, ...winfsp].filter(Boolean)) assert.ok(labels.hostFinishCodeText[code], `${code} has no label`);
  for (const code of Object.keys(labels.hostFinishCodeText)) assert.ok(display.includes(code) || winfsp.includes(code), `${code} is not a Host code`);
  assert.deepEqual(Object.keys(labels.hostFinishStepText), ['1', '2']);

  const first = shown({ action: 276, stage: 1, ok: true, error: 0, installed: true, started: true, reboot: false, changed: true, restartAttempted: false, running: true, recovery: false });
  assert.equal(first.entry.title, 'Host 작업 · 설치 마무리');
  assert.equal(first.text, '가상 화면 드라이버 | 설치돼 있고 장치가 시작됨 | 드라이버 인증서를 등록함 | Host 서비스가 장치를 사용 중'); assert.equal(first.entry.failure, false);
  assert.equal(shown({ action: 276, stage: 1, ok: true, error: 0, installed: true, started: false, reboot: true, changed: false, restartAttempted: false, running: false, recovery: false }).text,
    '가상 화면 드라이버 | 설치됨 · 장치는 아직 시작되지 않음 | Windows 다시 시작 필요');
  assert.equal(shown({ action: 276, stage: 1, ok: true, error: 0, installed: true, started: true, reboot: false, changed: false, restartAttempted: true, running: false, recovery: false }).text,
    '가상 화면 드라이버 | 설치돼 있고 장치가 시작됨 | Host 서비스를 다시 시작하려 함 | Host 서비스가 장치를 쓰는지 확인하지 못함');
  const failed = shown({ action: 276, stage: 1, ok: false, error: 17, installed: false, started: false, reboot: false, changed: false, restartAttempted: false, running: false, recovery: true });
  assert.equal(failed.text, '가상 화면 드라이버 | 설치하지 못함 | 드라이버를 설치하지 못함 | 실패한 설치를 모두 되돌림'); assert.equal(failed.entry.failure, true);
  assert.equal(shown({ action: 276, stage: 1, ok: false, error: 2, installed: false, started: false, reboot: false, changed: false, restartAttempted: false, running: false, recovery: false }).text,
    '가상 화면 드라이버 | 설치하지 못함 | 드라이버 서명 신뢰에 동의하지 않음');
  assert.match(shown({ action: 276, stage: 1, ok: false, error: 99, installed: false, started: false, reboot: false, changed: false, restartAttempted: false, running: false, recovery: false }).text, /설치하지 못함 \| 오류 번호 99/);
  assert.equal(shown({ action: 276, stage: 2, ok: true, error: 0, installed: true, reboot: false }).text, 'WinFsp | 설치함');
  assert.equal(shown({ action: 276, stage: 2, ok: true, error: 2, installed: false, reboot: false }).text, 'WinFsp | WinFsp가 이미 설치돼 있음');
  assert.equal(shown({ action: 276, stage: 2, ok: true, error: 0, installed: true, reboot: true }).text, 'WinFsp | 설치함 | Windows 다시 시작 필요');
  const blocked = shown({ action: 276, stage: 2, ok: false, error: 5, installed: false, reboot: false });
  assert.equal(blocked.text, 'WinFsp | 설치하지 못함 | Windows 정책이 설치를 막음'); assert.equal(blocked.entry.failure, true);
  assert.equal(shown({ action: 276, stage: 3, ok: true }).text, '단계 3');
});

test('The cleanup of a removed Host follows the Host\'s tables', () => {
  const header = read('../../apps/windows_host/src/host_removal.hpp');
  assert.deepEqual(labels.hostRemovalReasons, codesOf(header, 'inline unsigned reasonNumber'));
  assert.deepEqual(labels.hostRemovalCertificates, codesOf(header, 'inline unsigned certificateNumber'));
  for (const code of labels.hostRemovalReasons.filter(Boolean)) assert.ok(labels.hostRemovalReasonText[code], `${code} has no label`);
  for (const code of labels.hostRemovalCertificates) assert.ok(labels.hostRemovalCertificateText[code], `${code} has no label`);
  const cleaned = shown({ action: 285, stage: 1, ok: true, error: 0, items: 1, missing: 0, pending: false, dropped: 1, expected: 0, changed: true, cause: 0, owned: true });
  assert.equal(cleaned.entry.title, 'Host 작업 · Host 제거 정리');
  assert.equal(cleaned.text, 'Multitap이 설치한 것을 정리함 | 가상 화면 장치 제거 1 | 드라이버 패키지 제거 1 | 드라이버 인증서 제거 | 가상 화면 선택·상태 기록 삭제');
  assert.equal(shown({ action: 285, stage: 1, ok: true, error: 0, items: 1, missing: 0, pending: false, dropped: 0, expected: 1, changed: false, cause: 2, owned: true }).text,
    'Multitap이 설치한 것을 정리함 | 가상 화면 장치 제거 1 | 드라이버 패키지 제거 0 | 남긴 드라이버 패키지 1 | 드라이버 인증서 그대로 · 다른 장치나 드라이버가 사용 | 가상 화면 선택·상태 기록 삭제');
  const blocked = shown({ action: 285, stage: 1, ok: false, error: 6, items: 0, missing: 0, pending: false, dropped: 0, expected: 0, changed: false, cause: 1, owned: false });
  assert.equal(blocked.text, 'Host 서비스가 멈추지 않아 정리하지 않음'); assert.equal(blocked.entry.failure, true);
  assert.match(shown({ action: 285, stage: 1, ok: false, error: 0, items: 0, missing: 0, pending: true, dropped: 0, expected: 0, changed: false, cause: 3, owned: false }).text, /일부가 남음.*아직 남음.*가상 화면 장치가 남음.*기록 남음/);
});

test('The broker keeps the fields these records use', () => {
  const report = events => safeDiagnosticReport({ protocol: 'host-diagnostics-v1', source: 'host-service', version: '1.0.53', collectedAt: '2026-11-10T06:50:00.000Z', interactiveSession: true, consoleSession: true, captureAvailable: true,
    display: { observed: false, observationAgeMs: null, installed: true, deviceReady: true, owned: true, activeDisplay: true, configurationPending: false, recoveryRequired: false, rebootRequired: false, remoteSession: false, monitorCount: 1, width: 1920, height: 1080, reason: '' },
    native: { available: false }, events });
  for (const values of [{ action: 215, stage: 1, ok: false, cause: 2, error: 1, changed: true }, { action: 200, ok: true, stage: 246, error: 11 | 1423 << 8 },
    { action: 276, stage: 1, ok: true, error: 0, installed: true, started: true, reboot: false, changed: true, restartAttempted: false, running: true, recovery: false },
    { action: 276, stage: 2, ok: true, error: 2, installed: false, reboot: false },
    { action: 285, stage: 1, ok: true, error: 0, items: 1, missing: 0, pending: false, dropped: 1, expected: 0, changed: true, cause: 0, owned: true }]) {
    assert.deepEqual(report([row(values)]).events[0].values, values);
  }
});

// Product vocabulary (docs/i18n.md): the remote computer is 데스크톱 and the computer in front of
// the user is PC; power is 다시 시작; the Proxmox console is Console. "VM" stays only where the thing is a Proxmox VM.
test('The diagnostic tables use the product vocabulary', () => {
  const tables = { hostClipboardStageText: labels.hostClipboardStageText, hostClipboardNoteText: labels.hostClipboardNoteText, hostPrintText: labels.hostPrintText, hostPrintDefaultReasonText: labels.hostPrintDefaultReasonText,
    hostDriveText: labels.hostDriveText, hostDriveErrorText: labels.hostDriveErrorText, hostUsbText: labels.hostUsbText, clientUsbText: labels.clientUsbText, clientUsbBack: labels.clientUsbBack, clientCodes: labels.clientCodes,
    clientEvents: labels.clientEvents, hostActions: labels.hostActions, hostUpdateLabels: labels.hostUpdateLabels, hostUpdateCodeText: labels.hostUpdateCodeText, hostUpdateStateText: labels.hostUpdateStateText,
    updateLabels: labels.updateLabels, valueNames: labels.valueNames, hostInputKindText: labels.hostInputKindText, hostFinishCodeText: labels.hostFinishCodeText, hostConsoleParkCauseText: labels.hostConsoleParkCauseText,
    hostRemovalReasonText: labels.hostRemovalReasonText, hostRemovalCertificateText: labels.hostRemovalCertificateText, hostUpdateDescription: { text: labels.hostUpdateDescription } };
  const banned = [[/VM/, 'VM (the remote machine is 데스크톱)'], [/재시작|재부팅/, '재시작 (다시 시작)'], [/연결 종료/, '연결 종료 (연결이 끝나다 / 연결 끊김)'], [/설치 EXE/, '설치 EXE (설치 파일)'], [/검증/, '검증 (확인)'],
    [/Proxmox 콘솔|VM 콘솔/, 'the Proxmox console is Console'], [/이 PC|내 PC|이 컴퓨터/, '이 PC']];
  for (const [name, table] of Object.entries(tables)) for (const [key, text] of Object.entries(table)) for (const [pattern, why] of banned) {
    assert.doesNotMatch(String(text), pattern, `${name}.${key}: ${why}`);
  }
  // The words that replaced them.
  assert.equal(labels.hostUpdateLabels.waiting, '연결이 끝나면 시작'); assert.equal(labels.hostUpdateStateText.deferred, '연결이 끝나면 시작');
  assert.equal(labels.hostUpdateLabels.verifying, '파일 확인 중'); assert.equal(labels.updateLabels.restarting, '서버 다시 시작 중');
  assert.equal(labels.hostUpdateCodeText.host_not_returned, '설치 후 Host가 다시 연결되지 않았어요. Console로 데스크톱을 열어 확인하세요.');
  assert.equal(labels.hostUpdateCodeText.restart_required, 'Windows를 다시 시작해야 해요.\n자동으로 다시 시작하지 않아요.');
  assert.match(labels.hostUpdateCodeText.certificate_trust_required, /데스크톱에서 설치 파일을 직접 실행하세요\.$/);
  assert.match(labels.hostUpdateCodeText.driver_install_required, /데스크톱에서 설치 파일을 직접 실행하세요\.$/);
  assert.equal(labels.hostClipboardStageText[5], '데스크톱에서 받다가 실패'); assert.equal(labels.hostPrintText[1], '데스크톱에 프린터 준비'); assert.equal(labels.hostUsbText[10], '장치가 데스크톱에 연결됨');
  assert.equal(labels.hostUsbText[7], 'USB를 쓰려면 데스크톱을 다시 시작해야 함'); assert.equal(labels.hostActions[280], '데스크톱 미리보기 화면'); assert.equal(labels.hostActions[281], '로컬 PC 전원');
  // PC keeps meaning the user's own computer.
  assert.equal(labels.hostClipboardStageText[1], 'PC에서 받기 시작'); assert.equal(labels.hostPrintText[21], 'PC에서 인쇄함'); assert.equal(labels.hostUsbText[13], 'PC에서 장치 연결을 끊음');
});

test('The timeline shows those words too', () => {
  const source = read('../admin/js/timeline.js');
  const literals = [...source.matchAll(/'([^'\n]*)'|`([^`\n]*)`/g)].map(match => match[1] ?? match[2]);
  for (const text of literals) {
    assert.doesNotMatch(text, /재시작|재부팅|연결 종료 후|VM 미리보기|스트리밍·콘솔/, text);
  }
  const usb = shown({ action: 250, ok: false, stage: 7 });
  assert.match(usb.text, /데스크톱을 다시 시작해야 함/);
  const inventory = shown({ action: 273, stage: 1, ok: true, items: 2, missing: 1, error: 0 });
  assert.match(inventory.text, /다시 시작 대기 1/);
  assert.equal(shown({ action: 280, stage: 1, ok: true, kib: 12 }).entry.title, 'Host 작업 · 데스크톱 미리보기 화면');
});
