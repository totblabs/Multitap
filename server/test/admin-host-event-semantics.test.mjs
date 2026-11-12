import test from 'node:test';
import assert from 'node:assert/strict';
import {parseHTML} from 'linkedom';
import {hostEntries} from '../admin/js/timeline.js';
import {displayPanel} from '../admin/js/hosts.js';

const at = '2026-11-04T09:42:43.406Z';
const event = values => ({time: at, event: 'hostAction', values: Object.freeze(values)});
const entries = events => hostEntries([{desktopId: 'fixture-pc', diagnostics: {version: '1.0.12', events}}]);

test('render observations name adapter IDs without turning software composition into a failed operation', () => {
  const rows = [
    event({action: 211, ok: false, stage: 5140, expected: 140, error: 4318, missing: 6968}),
    event({action: 211, ok: true, stage: 4318, expected: 6968, error: 4318, missing: 6968}),
    event({action: 211, ok: false, stage: 0, expected: 0, error: 1, missing: 0}),
    event({action: 211, ok: false, stage: 0x23ab, expected: 0xffff, error: 0, missing: 0}),
  ];
  const original = JSON.stringify(rows);
  const rendered = entries(rows);
  assert.match(rendered[0].detail.join(' '), /Microsoft/);
  assert.match(rendered[0].detail.join(' '), /NVIDIA/);
  assert.match(rendered[0].detail.join(' '), /1B38/i);
  assert.match(rendered[2].detail.join(' '), /관찰 안 됨/);
  assert.match(rendered[2].detail.join(' '), /조회 못함/);
  assert.match(rendered[3].detail.join(' '), /23AB/i);
  for (const row of rendered) {
    assert.equal(row.failure, false, 'ok describes the observed compositor adapter, not operation failure');
    assert.match(row.title, /렌더링 GPU/);
    assert.doesNotMatch(row.detail.join(' '), /오류 번호|인코더 사용|H\.264 출력 확인/);
  }
  assert.equal(JSON.stringify(rows), original, 'original observations stay intact');
});

test('real drive, USB and display failures keep their failure status and cause', () => {
  const [drive, usb, display] = entries([
    event({action: 247, stage: 3, ok: false, error: 3, cause: 5}),
    event({action: 250, stage: 24, ok: false, error: 32, expected: 0x030001}),
    event({action: 214, stage: 8, ok: false, error: 87, missing: 1}),
  ]);
  assert.ok([drive, usb, display].every(row => row.failure === true));
  assert.match(drive.detail.join(' '), /드라이브 연결 실패/);
  assert.match(drive.detail.join(' '), /Windows 오류 5/);
  assert.match(usb.title, /USB/);
  assert.match(display.detail.join(' '), /87/);
});

test('Host diagnostic history uses the same semantic description and retains explicit raw fields', () => {
  const previous = globalThis.document;
  const previousNode = globalThis.Node;
  const {document, Node} = parseHTML('<html><body></body></html>');
  globalThis.document = document;
  globalThis.Node = Node;
  try {
    const events = [event({action: 211, ok: false, stage: 5140, expected: 140, error: 4318, missing: 6968}),
      event({action: 253, stage: 10, ok: true, error: 2, items: 0, format: 0, cause: 0, win32: 0})];
    const report = {version: '1.0.12', display: {observed: true, monitorCount: 1, width: 1920, height: 1080, reason: ''}, native: {available: false}, events};
    const ctx = {resource: () => ({value: {connected: true, supported: true, receivedAt: at, stale: false, report}})};
    const node = displayPanel(ctx, {id: 'fixture-pc', kind: 'pc', host: {capabilities: {}}, display: {}});
    const rows = node.querySelectorAll('.event-list > li');
    assert.equal(rows.length, 2);
    assert.match(rows[0].querySelector('strong').textContent, /렌더링 GPU/);
    const descriptions = [...rows].map(row => row.querySelector('.host-event-description').textContent);
    assert.match(descriptions[0], /Microsoft/);
    assert.match(descriptions[0], /NVIDIA/);
    assert.doesNotMatch(descriptions.join(' '), /오류 번호 4318|오류 번호: 4318|오류 번호 2|오류 번호: 2/);
    assert.ok([...rows].every(row => row.querySelector('details summary').textContent === '진단 원본'));
    assert.match(rows[0].querySelector('details code').textContent, /error=4318/);
  } finally {
    if (previous === undefined) delete globalThis.document;
    else globalThis.document = previous;
    if (previousNode === undefined) delete globalThis.Node;
    else globalThis.Node = previousNode;
  }
});
