import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, existsSync } from 'node:fs';
import { safeDiagnosticReport } from '../src/host-diagnostics.mjs';
import { hostEntries } from '../admin/js/timeline.js';
import { hostActions, hostClipboardCauses, hostClipboardCauseText, hostClipboardReasons, hostClipboardNoteText, hostClipboardStageText, hostClipboardFormatBits, hostClipboardWin32Text } from '../admin/js/labels.js';

// The Host sources are not part of the Linux QA stage; those cross-checks run where apps/ exists.
const hostSources = existsSync(new URL('../../apps/windows_host/src/clipboard_diagnostic.hpp', import.meta.url));
const header = hostSources ? readFileSync(new URL('../../apps/windows_host/src/clipboard_diagnostic.hpp', import.meta.url), 'utf8') : '';
const quoted = text => [...text.matchAll(/"([^"]+)"/g)].map(match => match[1]);
const report = events => ({ protocol: 'host-diagnostics-v1', source: 'host-service', version: '0.2.78', collectedAt: new Date().toISOString(), interactiveSession: true, consoleSession: true, captureAvailable: true,
  display: { observed: true, observationAgeMs: 0, installed: true, deviceReady: true, owned: true, activeDisplay: true, configurationPending: false, recoveryRequired: false, rebootRequired: false, remoteSession: false, monitorCount: 1, width: 1920, height: 1080, reason: '' },
  native: { available: false }, events });

test('A Host clipboard transfer record (hostAction 253) keeps its numeric fields and nothing else', () => {
  const time = new Date().toISOString();
  const safe = safeDiagnosticReport(report([{ time, event: 'hostAction', values: { action: 253, stage: 5, ok: false, error: 7, cause: 12, format: 2, items: 3, kib: 100, name: 'secret.txt', text: 'clipboard text', path: 'C:\\x' } }]));
  assert.ok(safe);
  assert.deepEqual(safe.events[0].values, { action: 253, stage: 5, ok: false, error: 7, cause: 12, format: 2, items: 3, kib: 100 });
  assert.equal(JSON.stringify(safe).includes('secret'), false);
  // Numbers only, in range: strings, negatives and values above 32 bits are dropped, never coerced.
  const bad = safeDiagnosticReport(report([{ time, event: 'hostAction', values: { action: 253, stage: '5', error: -1, cause: 2 ** 32, format: 1.5, kib: 7 } }]));
  assert.deepEqual(bad.events[0].values, { action: 253, kib: 7 });
});

test('The admin console names a clipboard transfer: side, format, size and the fixed reason behind a failure', () => {
  assert.equal(hostActions[253], '클립보드 전송');
  const [begin, failed, peer, ok] = hostEntries([{ desktopId: 'pve-149', diagnostics: { version: '0.2.78', events: [
    { time: '2026-10-27T00:08:54.000Z', event: 'hostAction', values: { action: 253, stage: 1, ok: true, error: 0, cause: 0, format: 2, items: 2, kib: 90 } },
    { time: '2026-10-27T00:08:56.000Z', event: 'hostAction', values: { action: 253, stage: 5, ok: false, error: 5, cause: 1, format: 2, items: 2, kib: 90 } },
    { time: '2026-10-27T00:08:59.000Z', event: 'hostAction', values: { action: 253, stage: 7, ok: false, error: 7, cause: 0, format: 2, items: 1, kib: 100 } },
    { time: '2026-10-27T00:09:00.000Z', event: 'hostAction', values: { action: 253, stage: 4, ok: true, error: 0, cause: 0, format: 1, items: 1, kib: 1 } },
  ] } }]);
  assert.equal(begin.title, 'Host 작업 · 클립보드 전송');
  assert.equal(begin.failure, false);
  assert.ok(begin.detail.includes('PC에서 받기 시작') && begin.detail.includes('파일'));
  assert.equal(failed.failure, true);
  assert.ok(failed.detail.some(line => line.includes('clipboard_write_failed') || line.includes('쓰지')), failed.detail.join('|'));
  assert.equal(hostClipboardCauses[0], 'Invalid clipboard transfer'); assert.ok(failed.detail.includes('내부 원인: 잘못된 클립보드 전송'), 'a cause the Host words in English reads in Korean');
  assert.deepEqual(Object.keys(hostClipboardCauseText), hostClipboardCauses.filter(cause => /[A-Z ]/.test(cause)), 'every sentence cause has its Korean text');
  assert.ok(peer.detail.includes('PC(클라이언트)가 실패를 알림'));
  assert.ok(peer.detail.some(line => line.includes('clipboard_transfer_failed') || line.includes('전송')), peer.detail.join('|'));
  assert.equal(ok.failure, false);
});

test('A failed clipboard write preserves bounded Windows detail through the report and console', () => {
  const detail = 5 | (2 << 16) | (1 << 20);
  const safe = safeDiagnosticReport(report([{
    time: new Date().toISOString(), event: 'hostAction',
    values: { action: 253, stage: 5, ok: false, win32: detail, text: 'private clipboard content' },
  }]));
  assert.equal(safe.events[0].values.win32, detail);
  assert.equal(JSON.stringify(safe).includes('private clipboard content'), false);
  const [entry] = hostEntries([{ desktopId: 'pve-170', diagnostics: safe }]);
  assert.equal(entry.failure, true);
  assert.ok(entry.detail.includes(hostClipboardWin32Text(detail)));
  assert.match(hostClipboardWin32Text(detail), /Windows 오류 5/);
  assert.match(hostClipboardWin32Text(detail), /Host가 클립보드를 열어 둔 채였음/);
  assert.match(hostClipboardWin32Text(detail), /한 번 더 시도함/);
  for (const value of ['5', -1, 1.5, 2 ** 32]) {
    const invalid = safeDiagnosticReport(report([{
      time: new Date().toISOString(), event: 'hostAction', values: { action: 253, stage: 5, win32: value },
    }]));
    assert.equal(Object.hasOwn(invalid.events[0].values, 'win32'), false);
  }
  const maximum = safeDiagnosticReport(report([{
    time: new Date().toISOString(), event: 'hostAction', values: { action: 253, stage: 5, win32: 0xffffffff },
  }]));
  assert.equal(maximum.events[0].values.win32, 0xffffffff);
});

test('The console tables are the header tables (reasons and internal causes)', { skip: !hostSources }, () => {
  const causes = quoted(header.slice(header.indexOf('// BEGIN-CAUSES'), header.indexOf('// END-CAUSES')));
  assert.deepEqual(hostClipboardCauses, causes);
  const reasons = quoted(header.slice(header.indexOf('static const std::vector<std::string> reasons'), header.indexOf('if (reason.empty())')));
  assert.deepEqual(hostClipboardReasons, reasons);
  // Every reason a peer may send is in the table (the relay's peerErrors plus the two pull reasons).
  const relay = readFileSync(new URL('../src/clipboard.mjs', import.meta.url), 'utf8');
  const peerErrors = quoted(relay.slice(relay.indexOf('const peerErrors=new Set(['), relay.indexOf(']);', relay.indexOf('const peerErrors=new Set(['))));
  for (const reason of [...peerErrors, 'clipboard_selected_file_changed', 'clipboard_offer_expired']) assert.ok(hostClipboardReasons.includes(reason) || reason === 'clipboard_commit_uncertain', reason);
});

test('Every Host clipboard fixed field is allowed by the Host journal and the relay report', { skip: !hostSources }, () => {
  const journal = readFileSync(new URL('../../apps/windows_host/src/operational_log.hpp', import.meta.url), 'utf8');
  for (const field of ['format', 'items', 'kib', 'cause', 'win32']) {
    assert.equal([...journal.matchAll(new RegExp(`"${field}"`, 'g'))].length >= 2, true, `${field} is in both journal lists`);
  }
});

test('A copy the Host saw but did not send is named in the console: why, and what was on the clipboard', () => {
  const [announced, skipped, empty, dropped] = hostEntries([{ desktopId: 'pve-149', diagnostics: { version: '0.2.78', events: [
    { time: '2026-10-27T01:00:00.000Z', event: 'hostAction', values: { action: 253, stage: 11, ok: true, error: 0, cause: 0, format: 0, items: 3, kib: 0 } },
    { time: '2026-10-27T01:00:01.000Z', event: 'hostAction', values: { action: 253, stage: 10, ok: true, error: 2, cause: 0, format: 0, items: 0, kib: 0 } },
    { time: '2026-10-27T01:00:02.000Z', event: 'hostAction', values: { action: 253, stage: 12, ok: false, error: 0, cause: 0, format: 0, items: 6, kib: 0 } },
    { time: '2026-10-27T01:00:03.000Z', event: 'hostAction', values: { action: 253, stage: 14, ok: false, error: 4, cause: 0, format: 0, items: 0, kib: 0 } },
  ] } }]);
  assert.ok(announced.detail.includes(hostClipboardStageText[11]) && announced.detail.some(line => line.includes('파일 목록') && line.includes('셸 항목 목록')), announced.detail.join('|'));
  assert.equal(announced.failure, false);
  assert.equal(skipped.failure, false);
  assert.ok(skipped.detail.includes(hostClipboardNoteText[1]), skipped.detail.join('|'));
  assert.equal(empty.failure, true);
  assert.ok(empty.detail.some(line => line.includes('셸 항목 목록') && line.includes('가상 파일')), empty.detail.join('|'));
  assert.equal(dropped.failure, true);
  assert.ok(dropped.detail.includes(hostClipboardNoteText[3]), dropped.detail.join('|'));
});

test('The observation reasons and format bits are the header constants', { skip: !hostSources }, () => {
  const notes = header.slice(header.indexOf('namespace ClipboardNote'), header.indexOf('}', header.indexOf('namespace ClipboardNote')));
  const values = [...notes.matchAll(/(\w+)\s*=\s*(\d+)/g)].map(match => Number(match[2])).sort((a, b) => a - b);
  assert.deepEqual(values, hostClipboardNoteText.map((_, index) => index + 1));
  const bits = header.slice(header.indexOf('namespace ClipboardFormats'), header.indexOf('}', header.indexOf('namespace ClipboardFormats')));
  assert.deepEqual([...bits.matchAll(/(\w+)\s*=\s*(\d+)/g)].map(match => Number(match[2])), hostClipboardFormatBits.map(([bit]) => bit));
  const stages = header.slice(header.indexOf('changeSkipped = 10'), header.indexOf('}', header.indexOf('changeSkipped = 10')));
  assert.deepEqual([...stages.matchAll(/=\s*(\d+)/g)].map(match => Number(match[1])), [10, 11, 12, 13, 14]);
  for (let stage = 10; stage <= 14; stage++) assert.ok(hostClipboardStageText[stage], `stage ${stage}`);
});
