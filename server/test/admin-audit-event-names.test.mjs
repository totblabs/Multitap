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
  // A save of the access settings on a Docker Compose install records through the hook http.mjs gives it (self-access.mjs record()).
  const selfAccess = await readFile(new URL('self-access.mjs', source), 'utf8');
  for (const call of selfAccess.matchAll(/\bthis\.record\('([a-z][a-z0-9_]*)'/g)) names.add(call[1]);
  for (const name of ['server_access_outside_started', 'server_access_outside_confirmed', 'server_access_outside_expired', 'server_access_rolled_back']) assert.ok(names.has(name), name);
  assert.ok(names.size > 40, 'the scan found the audit calls');
  const unnamed = [...names].filter(name => !Object.hasOwn(eventNames, name)).sort();
  assert.deepEqual(unnamed, []);
  for (const name of names) assert.match(eventLabel(name), /[가-힣]/, name);
});

// Browser check, round 2: a wrong password in the backup dialog was "원인 미확인" in 기록 (the record has no subject).
test('a wrong administrator password in the backup dialog has its own reason in 기록', async () => {
  const { auditEntry, causeCell } = await import('../admin/js/timeline.js');
  const ko = (await import('../admin/i18n/ko.js')).default, en = (await import('../admin/i18n/en.js')).default;
  const entry = auditEntry({ id: 7, created_at: Date.now(), event: 'backup_reauthentication_failed', actor: 'u1', subject: '' }, new Map([['u1', { id: 'u1', username: 'owner' }]]), new Map());
  assert.equal(entry.failure, true); assert.equal(entry.actor, 'owner');
  assert.equal(causeCell(entry), '관리자 비밀번호가 맞지 않았어요'); assert.notEqual(causeCell(entry), ko['labels.108']);
  assert.equal(en['timeline.backupPasswordWrong'], 'The administrator password was wrong');
});

test('an unknown event still shows its code rather than nothing', () => {
  assert.equal(eventLabel('some_future_event'), 'some_future_event');
});

test('a PC wake that sent nothing says why in 기록, not "원인 미확인"', async () => {
  const { auditEntry, causeCell } = await import('../admin/js/timeline.js');
  const ko = (await import('../admin/i18n/ko.js')).default;
  const desktops = new Map([['pc-fixture', { id: 'pc-fixture', name: 'PC' }]]);
  const entry = auditEntry({ id: 8, created_at: Date.now(), event: 'pc_wake_failed', actor: 'u1', subject: 'pc-fixture:EPERM' }, new Map([['u1', { id: 'u1', username: 'owner' }]]), desktops);
  assert.equal(entry.failure, true); assert.equal(entry.desktopId, 'pc-fixture'); assert.equal(entry.code, 'EPERM');
  assert.equal(causeCell(entry), '켜기 신호를 보내지 못함 (시스템 오류 EPERM)'); assert.notEqual(causeCell(entry), ko['labels.108']);
  const unknown = auditEntry({ id: 9, created_at: Date.now(), event: 'pc_wake_failed', actor: 'u1', subject: 'pc-fixture:unknown' }, new Map(), desktops);
  assert.equal(causeCell(unknown), '켜기 신호를 보내지 못함 (이유 모름)');
});
