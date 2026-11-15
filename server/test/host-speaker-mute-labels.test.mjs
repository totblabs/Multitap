import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, existsSync } from 'node:fs';
import { hostEntries } from '../admin/js/timeline.js';
import { hostActions, hostSpeakerMuteText, hostSpeakerMuteCauseText, hostSpeakerMuteStepText } from '../admin/js/labels.js';
import { safeDiagnosticReport } from '../src/host-diagnostics.mjs';

// hostAction 298 (Host speaker_mute.hpp): a local PC's own speakers while it is
// streamed. The administrator reads what happened and why in words; the tables
// follow the Host's own numbers.
const source = name => new URL(`../../apps/windows_host/src/${name}`, import.meta.url);
const hostSources = existsSync(source('speaker_mute.hpp'));
const entries = rows => hostEntries([{ desktopId: 'pc-fixture', diagnostics: { version: '1.1.22', events: rows.map((values, index) => ({ time: `2026-11-15T10:00:0${index}.000Z`, event: 'hostAction', values })) } }]);

test('The admin console says what happened to a local PC\'s speakers and why', () => {
  const [muted, again, restored, leftover, kept, pending, refused, unavailable, unknown] = entries([
    { action: 298, stage: 1, cause: 1, error: 0, items: 0, ok: true }, { action: 298, stage: 1, cause: 2, error: 0, items: 0, ok: true },
    { action: 298, stage: 2, cause: 4, error: 0, items: 0, ok: true }, { action: 298, stage: 2, cause: 6, error: 0, items: 0, ok: true },
    { action: 298, stage: 5, cause: 1, error: 0, items: 0, ok: true }, { action: 298, stage: 6, cause: 4, error: 0, items: 2, ok: true },
    { action: 298, stage: 3, cause: 6, error: 0x80070005, items: 0, ok: false }, { action: 298, stage: 4, cause: 3, error: 0x80070490, items: 0, ok: false },
    { action: 298, stage: 3, cause: 15, error: 0, items: 0, ok: false }]);
  assert.equal(muted.title, 'Host 작업 · 로컬 PC 스피커 음소거');
  assert.deepEqual(muted.detail, ['스피커를 음소거함', '스트리밍 시작']);
  assert.deepEqual(again.detail, ['스피커를 음소거함', '스트리밍 중 음소거가 풀림']);
  assert.deepEqual(restored.detail, ['스피커 소리를 되돌림', '스트리밍 종료']);
  assert.deepEqual(leftover.detail, ['스피커 소리를 되돌림', '이전 스트리밍이 남긴 음소거']);
  assert.deepEqual(kept.detail, ['이미 음소거되어 있어 그대로 둠', '스트리밍 시작']);
  assert.equal(pending.detail.length, 3); assert.match(pending.detail[2], /2$/);
  assert.deepEqual(refused.detail.slice(0, 2), ['스피커 음소거 단계 실패', 'Windows가 음소거를 거부함']);
  assert.match(refused.detail[2], /0x80070005$/);
  assert.equal(refused.failure, true);
  assert.equal(unavailable.detail[0], '음소거하지 않고 스트리밍(소리 캡처 방식 미지원)');
  assert.match(unavailable.detail[2], /0x80070490$/);
  assert.equal(unavailable.failure, true);
  assert.equal(muted.failure, false);
  assert.equal(unknown.detail.length, 2, 'an unknown step stays visible as its number');
  // Korean UI text never says "이 PC", "내 PC" or "이 컴퓨터".
  for (const text of [hostActions[298], ...Object.values(hostSpeakerMuteText), ...Object.values(hostSpeakerMuteCauseText), ...Object.values(hostSpeakerMuteStepText)]) assert.doesNotMatch(text, /이 PC|내 PC|이 컴퓨터/);
});

test('The broker keeps every field of a hostAction 298 row, HRESULT included', () => {
  const rows = [{ action: 298, ok: false, stage: 3, cause: 6, error: 0x80070005, items: 0 }, { action: 298, ok: true, stage: 6, cause: 4, error: 0, items: 2 }, { action: 298, ok: false, stage: 4, cause: 3, error: 0x800705B4, items: 0 }];
  const report = safeDiagnosticReport({ protocol: 'host-diagnostics-v1', source: 'host-service', version: '1.1.22', collectedAt: new Date().toISOString(), interactiveSession: true, consoleSession: true, captureAvailable: true,
    display: { observed: true, observationAgeMs: 0, installed: true, deviceReady: true, owned: true, activeDisplay: false, configurationPending: false, recoveryRequired: false, rebootRequired: false, remoteSession: false, monitorCount: 0, width: 0, height: 0, reason: '' },
    native: { available: true, installed: true, started: true, leaseActive: false, activationVerified: false, configurationPending: false, recoveryRequired: false, countControlAvailable: false, phase: 'stable', failureReason: '' },
    events: rows.map(values => ({ time: new Date().toISOString(), event: 'hostAction', values: { ...values, endpoint: 'DO_NOT_SEND' } })) });
  assert.ok(report, 'the fixture report is whole');
  assert.deepEqual(report.events.map(item => item.values), rows, 'the numbers stay, a device id would not');
});

test('The tables follow the Host\'s own numbers', { skip: !hostSources }, () => {
  const header = readFileSync(source('speaker_mute.hpp'), 'utf8');
  assert.match(header, /speakerMuteAction = 298;/);
  const numbers = name => { const body = new RegExp(`enum class ${name} : uint32_t \\{([^}]*)\\}`).exec(header); assert.ok(body, name); return [...body[1].matchAll(/([A-Za-z]+) = (\d+)/g)].map(match => Number(match[2])).filter(value => value > 0).map(String); };
  assert.deepEqual(Object.keys(hostSpeakerMuteText), numbers('SpeakerMuteEvent'));
  assert.deepEqual(Object.keys(hostSpeakerMuteCauseText), numbers('SpeakerMuteCause'));
  assert.deepEqual(Object.keys(hostSpeakerMuteStepText), numbers('SpeakerMuteStep'));
});
