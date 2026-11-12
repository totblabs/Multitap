import test from 'node:test';
import assert from 'node:assert/strict';
import { summarize, findings } from '../admin/js/perf.js';

const window = (time, values) => ({ time, receivedAt: time, values: { samples: 30, windowMs: 30000, ...values } });

test('Host stage maxima and client display fields are summarised and named in findings', () => {
  const session = { windows: [
    window('2026-10-26T00:37:12.762Z', { textureFps10: 275, decodeFps10: 296, hostWorkUs: 3321, hostP95Us: 31680, captureMaxUs: 9000, encodeMaxUs: 21000, textureWaitUs: 9000, textureUploadUs: 160, skippedFrames: 33, displayHz: 180, gpuHardware: true, remoteSession: false }),
    window('2026-10-26T00:38:17.762Z', { textureFps10: 188, decodeFps10: 199, hostWorkUs: 4066, hostP95Us: 37840, topologyMaxUs: 1200, captureMaxUs: 12000, convertMaxUs: 3000, encodeMaxUs: 18000, textureWaitUs: 11000, textureUploadUs: 360, skippedFrames: 32, displayHz: 180, gpuHardware: true }),
  ] };
  const s = summarize(session);
  assert.equal(s.encodeMax, 21000);
  assert.equal(s.captureMax, 12000);
  assert.equal(s.topologyMax, 1200);
  assert.equal(s.dropped, 33);
  assert.equal(s.displayHz, 180);
  assert.equal(s.gpuHardware, true);
  assert.equal(s.remoteSession, false);
  assert.equal(s.textureWait, 10000);
  const text = findings(s).map(note => note.text).join('\n');
  assert.match(text, /인코딩 단계 21 ms/);
  assert.match(text, /표시되기까지 평균 10 ms/);
  assert.doesNotMatch(text, /소프트웨어 그래픽|원격 데스크톱/);
});

test('Older clients and Hosts without the new fields add no invented values', () => {
  const s = summarize({ windows: [window('2026-10-26T00:10:13.399Z', { decodeFps10: 83, hostWorkUs: 663, hostP95Us: 61970 })] });
  for (const key of ['topologyMax', 'captureMax', 'convertMax', 'encodeMax', 'textureWait', 'textureUpload', 'dropped', 'displayHz', 'gpuHardware']) assert.equal(s[key], null, key);
  assert.equal(s.remoteSession, false);
  assert.doesNotMatch(findings(s).map(note => note.text).join('\n'), /가장 느린 한 프레임|표시되기까지/);
});

test('A client inside a remote session or on software graphics is noted', () => {
  const remote = summarize({ windows: [window('2026-10-26T00:10:13.399Z', { decodeFps10: 300, remoteSession: true, gpuHardware: false })] });
  assert.match(findings(remote).map(note => note.text).join('\n'), /원격 데스크톱\(RDP\) 세션/);
  const software = summarize({ windows: [window('2026-10-26T00:10:13.399Z', { decodeFps10: 300, gpuHardware: false })] });
  assert.match(findings(software).map(note => note.text).join('\n'), /소프트웨어 그래픽/);
});
