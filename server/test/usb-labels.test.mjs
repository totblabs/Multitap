import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { hostEntries, clientEntry } from '../admin/js/timeline.js';
import { hostUsbText, hostUsbReasons, clientUsbText, usbRequestClassText, usbErrnoLabel, clientCodes, valueNames } from '../admin/js/labels.js';
import { safeClientDiagnosticReport } from '../src/client-diagnostics.mjs';
import { safeDiagnosticReport } from '../src/host-diagnostics.mjs';

// Streaming USB diagnostics (docs/usb-redirection.md "Diagnostics"): the administrator's labels must follow the Host's
// stage numbers and reason table and the client's stage numbers, and the broker must keep exactly the fields and fixed
// codes the client sends. Otherwise no client USB record reaches the broker and Host stages show as bare numbers.
const read = path => readFileSync(new URL(path, import.meta.url), 'utf8');

test('Every Host USB stage has a label, and every label is a stage', () => {
  const header = read('../../apps/windows_host/src/usb_core.hpp');
  const body = /enum class UsbStage : uint32_t \{([^}]*)\}/.exec(header)[1].replace(/\/\/[^\n]*/g, '');
  const stages = Object.fromEntries([...body.matchAll(/([A-Za-z]+) = (\d+)/g)].map(match => [Number(match[2]), match[1]]));
  assert.ok(Object.keys(stages).length >= 25);
  for (const stage of Object.keys(stages)) assert.ok(hostUsbText[stage], `stage ${stage} (${stages[stage]}) has no label`);
  for (const stage of Object.keys(hostUsbText)) assert.ok(stages[stage], `label ${stage} is not a stage`);
});

test('The reason numbers of the Host log are the labels\' reasons', () => {
  const header = read('../../apps/windows_host/src/usb_core.hpp');
  const table = /static const char\* const reasons\[\] = \{([^}]*)\}/.exec(header)[1];
  assert.deepEqual([...table.matchAll(/"([a-z_]+)"/g)].map(match => match[1]), hostUsbReasons);
});

test('The client\'s USB stages and fixed codes are labelled and kept by the broker', () => {
  const dart = read('../../apps/windows_client/lib/data/usb_redirection.dart');
  const stages = new Set([...dart.matchAll(/'stage': (\d+)/g)].map(match => Number(match[1])));
  for (const stage of stages) assert.ok(clientUsbText[stage], `client stage ${stage} has no label`);
  const codes = read('../../apps/windows_client/lib/data/client_diagnostic_codes.dart');
  const usbCodes = [...codes.matchAll(/'(usb(?:dk)?_[a-z_]+)'/g)].map(match => match[1]);
  assert.ok(usbCodes.includes('usb_request_stuck') && usbCodes.includes('usb_device_not_returned') && usbCodes.includes('usb_capture_timeout'));
  for (const code of usbCodes) {
    const raw = { protocol: 'client-diagnostics-v1', runId: 'a'.repeat(32), sequence: 1, version: '0.4.95', events: [{ time: new Date().toISOString(), event: 'usb', code, values: { stage: 9 } }] };
    assert.equal(safeClientDiagnosticReport(raw).events[0].code, code, `${code} is not kept by the broker`);
  }
  for (const code of ['usb_request_stuck', 'usb_device_not_returned', 'usb_capture_timeout', 'usb_device_error', 'usb_prepare_timeout', 'usb_return_unknown']) assert.ok(clientCodes[code], `${code} has no label`);
});

test('A client USB summary keeps its counts and the first failing request, and nothing else', () => {
  const raw = { protocol: 'client-diagnostics-v1', runId: 'b'.repeat(32), sequence: 2, version: '0.4.95', events: [{ time: new Date().toISOString(), event: 'usb', desktopId: 'pve-170', mode: 'stream', code: 'usb_device_error',
    values: { stage: 9, ok: false, usbUrbs: 41, usbKib: 3, usbStalls: 1, usbFailed: 0, usbTimeouts: 1, usbSlow: 1, usbReq: 0xa1fe, error: 32, elapsedMs: 38000, usbBack: 1, serial: '00000000CY02', name: 'Alcor Micro', vidPid: '058F:6387' } }] };
  const safe = safeClientDiagnosticReport(raw);
  assert.deepEqual(safe.events[0].values, { stage: 9, ok: false, usbUrbs: 41, usbKib: 3, usbStalls: 1, usbFailed: 0, usbTimeouts: 1, usbSlow: 1, usbReq: 0xa1fe, error: 32, elapsedMs: 38000, usbBack: 1 });
  assert.ok(!JSON.stringify(safe).includes('CY02') && !JSON.stringify(safe).includes('Alcor') && !JSON.stringify(safe).includes('058F'));
  for (const key of ['usbStalls', 'usbFailed', 'usbTimeouts', 'usbSlow', 'usbReq', 'usbBack', 'usbUrbs', 'usbKib', 'usbDevices', 'usbClass']) assert.ok(valueNames[key], `${key} has no name`);
  const entry = clientEntry({ ...safe.events[0], runId: raw.runId, sequence: 2 });
  assert.equal(entry.title, 'USB 장치 · 스트리밍');
  assert.ok(entry.detail.some(text => text.includes('GET_MAX_LUN') && text.includes('스톨')), entry.detail.join(' | '));
  assert.ok(entry.failure && entry.codeLabel);
});

test('Request classes and errnos read in words', () => {
  assert.equal(usbRequestClassText(0xa1fe), '제어 요청 A1/FE (GET_MAX_LUN)');
  assert.equal(usbRequestClassText(0x21ff), '제어 요청 21/FF (대용량 저장소 리셋(BOT))');
  assert.equal(usbRequestClassText(0x8006), '제어 요청 80/06 (GET_DESCRIPTOR)');
  assert.equal(usbRequestClassText(0x10000 | 2 << 8 | 0x81), '벌크 IN 엔드포인트 1');
  assert.equal(usbRequestClassText(0x10000 | 3 << 8 | 0x83), '인터럽트 IN 엔드포인트 3');
  assert.equal(usbRequestClassText(0x10000 | 2 << 8 | 0x02), '벌크 OUT 엔드포인트 2');
  assert.equal(usbRequestClassText(-1), '');
  assert.equal(usbErrnoLabel(32), '스톨(EPIPE)');
  assert.equal(usbErrnoLabel(999), 'errno 999');
});

test('Host USB records are shown with their meaning, not as bare numbers', () => {
  const row = (values, ok = true) => ({ time: '2026-10-27T06:49:42.000Z', event: 'hostAction', values: { action: 250, ok, ...values } });
  const diagnostics = { version: '0.2.81', events: [
    row({ stage: 24, error: 32, expected: 0xa1fe }, false), row({ stage: 25, error: 1, expected: 0xa1fe }, false), row({ stage: 26, error: 7, expected: 0xa1fe }, false),
    row({ stage: 27, items: 41, kib: 3, cause: 1, expected: 2 }), row({ stage: 28, error: 10, expected: 46 }, false), row({ stage: 13, error: 10 }), row({ stage: 17, error: 5, expected: 0x2ff }, false)] };
  const text = hostEntries([{ desktopId: 'pve-170', diagnostics }]).map(entry => entry.detail.join(' | '));
  assert.ok(text[0].includes('스톨') && text[0].includes('GET_MAX_LUN'), text[0]);
  assert.ok(text[1].includes('확인 없이') && text[1].includes('GET_MAX_LUN'), text[1]);
  assert.ok(text[2].includes('7초'), text[2]);
  assert.ok(text[3].includes('요청 41개') && text[3].includes('취소 2'), text[3]);
  assert.ok(text[4].includes('문제 코드 10') && text[4].includes('46초'), text[4]);
  assert.ok(text[5].includes('사용자가 연결을 끊음'), text[5]);
  assert.ok(text[6].includes('제어 요청 02/FF'), text[6]);
  // The Host's report keeps the fields these stages use (items, kib, cause are already allowed for hostAction).
  const safe = safeDiagnosticReport({ protocol: 'host-diagnostics-v1', source: 'host-service', version: '0.2.81', collectedAt: '2026-10-27T06:50:00.000Z', interactiveSession: true, consoleSession: true, captureAvailable: true,
    display: { observed: false, observationAgeMs: null, installed: true, deviceReady: true, owned: true, activeDisplay: true, configurationPending: false, recoveryRequired: false, rebootRequired: false, remoteSession: false, monitorCount: 1, width: 1920, height: 1080, reason: '' },
    native: { available: false }, events: [row({ stage: 27, items: 41, kib: 3, cause: 1, expected: 2 })] });
  assert.deepEqual(safe.events[0].values, { action: 250, ok: true, stage: 27, items: 41, kib: 3, cause: 1, expected: 2 });
});

const detachFailureEntry = cause => {
  const values = Object.freeze({ action: 250, stage: 32, ok: false, ...(cause === undefined ? {} : { cause }) });
  const row = { time: '2026-11-04T12:00:00.000Z', event: 'hostAction', values };
  const entry = hostEntries([{ desktopId: 'fixture', diagnostics: { version: '1.0.14', events: [row] } }])[0];
  return { values, entry, text: entry.detail.join(' | ') };
};

test('Host USB native false is a VM detach request failure, with final state unverified', () => {
  const { values, entry, text } = detachFailureEntry(1);
  assert.equal(entry.failure, true);
  assert.match(text, /데스크톱.*USB.*분리 요청 실패/);
  assert.match(text, /컨트롤러.*실패.*반환/);
  assert.match(text, /분리 후 상태 미확인/);
  assert.doesNotMatch(text, /허용되지 않는|권한|Windows 오류|오류 번호|PC.*반환|포트.*남아|장치.*연결된 상태/);
  assert.deepEqual(values, { action: 250, stage: 32, ok: false, cause: 1 });
  assert.equal(Object.hasOwn(values, 'error'), false, 'cause must not become the old class-denial error index');
});

test('Host USB detach exception is distinct from false and retains real failure highlighting', () => {
  const { entry, text } = detachFailureEntry(2);
  assert.equal(entry.failure, true);
  assert.match(text, /분리 요청 처리 중 예외/);
  assert.match(text, /분리 후 상태 미확인/);
  assert.doesNotMatch(text, /권한|컨트롤러.*실패.*반환|Windows 오류|PC.*반환/);
});

test('Unknown and absent USB detach causes stay unverified instead of inventing a Windows error', () => {
  for (const cause of [undefined, 0, 99]) {
    const { entry, text } = detachFailureEntry(cause);
    assert.equal(entry.failure, true);
    assert.match(text, /원인 미확인/);
    assert.match(text, /분리 후 상태 미확인/);
    if (cause !== undefined) assert.ok(text.includes(String(cause)), text);
    assert.doesNotMatch(text, /권한|Windows 오류|오류 번호|처리 중 예외|컨트롤러.*실패.*반환/);
  }
});

test('USB detach diagnostic projection retains exact raw cause without synthesizing error', () => {
  const { values } = detachFailureEntry(1);
  const safe = safeDiagnosticReport({ protocol: 'host-diagnostics-v1', source: 'host-service', version: '1.0.14', collectedAt: '2026-11-04T12:00:00.000Z',
    interactiveSession: true, consoleSession: true, captureAvailable: true,
    display: { observed: false, observationAgeMs: null, installed: true, deviceReady: true, owned: true, activeDisplay: true, configurationPending: false, recoveryRequired: false, rebootRequired: false, remoteSession: false, monitorCount: 1, width: 1920, height: 1080, reason: '' },
    native: { available: false },
    events: [{ time: '2026-11-04T12:00:00.000Z', event: 'hostAction', values }] });
  assert.deepEqual(safe.events[0].values, values);
  assert.equal(Object.hasOwn(safe.events[0].values, 'error'), false);
  const { entry } = detachFailureEntry(1);
  assert.equal(entry.event, 'hostAction');
  assert.equal(entry.version, '1.0.14');
});

test('The Host\'s own devices (stages 33-38) read as their port, pad and preparation, never as a session reason', () => {
  // Stage 34/37's error is usb_driver.hpp's Prepare (1..7), 20 or 21, not an index into the session reason table.
  const row = (values, ok = true) => ({ time: '2026-11-14T09:00:00.000Z', event: 'hostAction', values: { action: 250, ok, ...values } });
  const diagnostics = { version: '1.1.11', events: [
    row({ stage: 33, error: 0, expected: 1 }), row({ stage: 34, error: 1, expected: 0 }, false), row({ stage: 34, error: 20, expected: 10048 }, false),
    row({ stage: 36, error: 0, expected: 2 }), row({ stage: 37, error: 4, expected: 0 }, false), row({ stage: 37, error: 21, expected: 1 }, false), row({ stage: 38, error: 0, expected: 3 }), row({ stage: 35 })] };
  const text = hostEntries([{ desktopId: 'pc-fixture', diagnostics }]).map(entry => entry.detail.join(' | '));
  const header = read('../../apps/windows_host/src/usb_driver.hpp');
  assert.match(header, /enum class Prepare \{ ready, packageMissing, platformUnsupported, signatureInvalid, otherInstallation, installFailed, restartRequired, protectionFailed \}/, 'the labels follow this order');
  assert.ok(text[0].includes('포트 1'), text[0]);
  assert.ok(text[1].includes('USB 드라이버 파일 없음'), text[1]);
  assert.ok(text[2].includes('10048'), text[2]);
  assert.ok(text[3].includes('패드 2'), text[3]);
  assert.ok(text[4].includes('다른 프로그램'), text[4]);
  assert.ok(text[5].includes('Windows가 장치를 연결하지 못함'), text[5]);
  assert.ok(text[6].includes('패드 3'), text[6]);
  for (const line of text) assert.doesNotMatch(line, /허용되지 않는|키보드·마우스|장치 수 한도|복원 대상|usb_restart_required/, line);
});
