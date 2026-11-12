import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, existsSync } from 'node:fs';
import { hostEntries } from '../admin/js/timeline.js';
import { hostActions, hostUdpProbeText, hostUdpPathText, hostUdpPathReasonText } from '../admin/js/labels.js';

// The Host sources are not part of the Linux QA stage; those cross-checks run where apps/ exists.
const source = name => new URL(`../../apps/windows_host/src/${name}`, import.meta.url);
const hostSources = existsSync(source('udp_media_path.hpp'));

const entries = rows => hostEntries([{ desktopId: 'pve-245', diagnostics: { version: '0.2.128', events: rows.map((values, index) => ({ time: `2026-11-01T10:00:0${index}.000Z`, event: 'hostAction', values })) } }]);

test('The admin console names the UDP relay leg and the path that carries video', () => {
  const [bound, timeout, onUdp, onWss, unknown] = entries([
    { action: 270, stage: 1, error: 0, ok: true }, { action: 270, stage: 4, error: 1, ok: false },
    { action: 271, stage: 1, error: 1, ok: true }, { action: 271, stage: 2, error: 3, ok: false }, { action: 271, stage: 2, error: 99, ok: false }]);
  assert.equal(bound.title, 'Host 작업 · UDP 중계 확인');
  assert.deepEqual(bound.detail, ['중계 서버와 UDP 연결됨', '중계 1번째']);
  assert.deepEqual(timeout.detail, ['중계 서버 응답 없음', '중계 2번째']);
  assert.equal(timeout.failure, true);
  assert.equal(onUdp.title, 'Host 작업 · UDP 영상 경로');
  assert.deepEqual(onUdp.detail, ['UDP로 영상 전송', '양방향 도달 확인']);
  assert.deepEqual(onWss.detail, ['WebSocket으로 영상 전송', '수신 확인이 끊김']);
  assert.equal(onWss.failure, true);
  assert.equal(unknown.detail.length, 2, 'an unknown reason stays visible as its number');
});

test('Host codes without a detail formatter still get a name', () => {
  for (const action of [211, 217, 262, 263, 264]) assert.ok(hostActions[action], `hostAction ${action} has no label`);
});

test('The UDP labels cover every code the Host records', { skip: !hostSources }, () => {
  const path = readFileSync(source('udp_media_path.hpp'), 'utf8'), probe = readFileSync(source('udp_probe.hpp'), 'utf8');
  const reasons = [...path.match(/enum UdpPathReason:uint32_t\{([^}]*)\}/)[1].matchAll(/=(\d+)/g)].map(match => Number(match[1]));
  assert.ok(reasons.length >= 8);
  for (const reason of reasons) assert.ok(hostUdpPathReasonText[reason], `path reason ${reason} has no label`);
  for (const stage of [...path.match(/udpPathAction=271,([^;]*);/)[1].matchAll(/=(\d+)/g)].map(match => Number(match[1]))) assert.ok(hostUdpPathText[stage], `path stage ${stage} has no label`);
  for (const stage of [...probe.match(/udpProbeAction=270,([^;]*);/)[1].matchAll(/=(\d+)/g)].map(match => Number(match[1]))) assert.ok(hostUdpProbeText[stage], `probe stage ${stage} has no label`);
});
