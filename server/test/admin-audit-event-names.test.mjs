// Every audit event the broker records has a Korean name in the admin console. One without a name
// (console, 2-step sign-in, idle sign-out, permission and sound card changes are easy to miss) shows its raw code in
// 활동 기록 because eventLabel falls back to the code.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readdir, readFile } from 'node:fs/promises';
import { eventNames, eventLabel } from '../admin/js/labels.js';

const source = new URL('../src/', import.meta.url);

test('every audit event recorded in server/src has a name', async () => {
  const names = new Set();
  for (const file of (await readdir(source)).filter(name => name.endsWith('.mjs'))) {
    const text = await readFile(new URL(file, source), 'utf8');
    // The first argument of audit(...) up to its first comma: a literal or a choice between literals.
    for (const call of text.matchAll(/\baudit\(([^,()]*),/g)) for (const literal of call[1].matchAll(/'([a-z][a-z0-9_]*)'/g)) names.add(literal[1]);
  }
  // Console relay events reach audit() through a variable (console-relay.mjs).
  for (const name of ['console_granted', 'console_ready', 'console_ended', 'console_taken_over']) names.add(name);
  assert.ok(names.size > 40, 'the scan found the audit calls');
  const unnamed = [...names].filter(name => !Object.hasOwn(eventNames, name)).sort();
  assert.deepEqual(unnamed, []);
  for (const name of names) assert.match(eventLabel(name), /[가-힣]/, name);
});

test('an unknown event still shows its code rather than nothing', () => {
  assert.equal(eventLabel('some_future_event'), 'some_future_event');
});
