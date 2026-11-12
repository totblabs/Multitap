import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, existsSync } from 'node:fs';
import { safeDiagnosticReport, bootStageFields } from '../src/host-diagnostics.mjs';
import { hostEntries } from '../admin/js/timeline.js';
import { hostActions, hostBootStages } from '../admin/js/labels.js';

// boot-ready-v1: hostAction 274 (apps/windows_host/src/boot_timeline.hpp), one per Host service start.
const report = events => ({ protocol: 'host-diagnostics-v1', source: 'host-service', version: '0.2.146', collectedAt: '2026-11-03T00:00:30.000Z', interactiveSession: true, consoleSession: true, captureAvailable: true,
  display: { observed: true, observationAgeMs: 0, installed: true, deviceReady: true, owned: true, activeDisplay: true, configurationPending: false, recoveryRequired: false, rebootRequired: false, remoteSession: false, monitorCount: 1, width: 1920, height: 1080, reason: '' },
  native: { available: false }, events });
const ready = { action: 274, stage: 1, ok: true, uptimeMs: 9200, configMs: 4, brokerMs: 640, virtualDisplayMs: 410, logonMs: 21000, sessionWorkerMs: 26000, readyMs: 26100 };

test('The boot timeline keeps every stage through the diagnostics projection', () => {
  const safe = safeDiagnosticReport(report([{ time: '2026-11-03T00:00:27.000Z', event: 'hostAction', values: { ...ready, path: 'DO_NOT_SEND', readyMs2: 1 } }]));
  assert.deepEqual(safe.events[0].values, ready);
  for (const key of ['uptimeMs', ...bootStageFields]) {
    for (const bad of [-1, 1.5, 0x100000000, '26100']) {
      const values = { ...ready, [key]: bad };
      assert.equal(safeDiagnosticReport(report([{ time: '2026-11-03T00:00:27.000Z', event: 'hostAction', values }])).events[0].values[key], undefined, `${key} ${bad} was kept`);
    }
  }
});

test('The admin console names the boot stages in seconds after the service start', () => {
  const [done, stalled] = hostEntries([{ desktopId: 'pve-170', diagnostics: { version: '0.2.146', events: [
    { time: '2026-11-03T00:00:27.000Z', event: 'hostAction', values: ready },
    { time: '2026-11-03T00:10:01.000Z', event: 'hostAction', values: { action: 274, stage: 2, ok: false, uptimeMs: 8000, configMs: 3, brokerMs: 900, virtualDisplayMs: 500 } },
  ] } }]);
  assert.equal(hostActions[274], '시작부터 연결 준비까지');
  assert.equal(done.title, 'Host 작업 · 시작부터 연결 준비까지');
  assert.deepEqual(done.detail, ['연결 준비됨 보고', '서비스 시작: Windows 시작 9.2초 뒤', '설정 읽음 +0.0초', '서버 연결 +0.6초', '가상 화면 +0.4초', 'Windows 로그인 +21.0초', '세션 화면 확인 +26.0초', '준비됨 보고 +26.1초']);
  assert.equal(done.failure, false);
  assert.deepEqual(stalled.detail, ['10분 안에 준비되지 않음', '서비스 시작: Windows 시작 8.0초 뒤', '설정 읽음 +0.0초', '서버 연결 +0.9초', '가상 화면 +0.5초']);
  assert.equal(stalled.failure, true);
  assert.deepEqual(hostBootStages.map(([key]) => key), bootStageFields);
});

// The Host sources are not part of the Linux QA stage; this cross-check runs where apps/ has the header.
const header = new URL('../../apps/windows_host/src/boot_timeline.hpp', import.meta.url);
test('The broker allows exactly the fields the Host records', { skip: !existsSync(header) }, () => {
  const text = readFileSync(header, 'utf8');
  const fields = [...text.match(/bootStageFields\{([^}]*)\}/)[1].matchAll(/"(\w+)"/g)].map(match => match[1]);
  assert.deepEqual(fields, bootStageFields);
  assert.equal(text.match(/bootUptimeField\[\] = "(\w+)"/)[1], 'uptimeMs');
  assert.match(text, /bootReadyAction = 274;/);
});
