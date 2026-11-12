import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { once } from 'node:events';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { NoticeService, noticeText, noticeTitleMax, noticeBodyMax, noticeGeneralLifetimeMs, noticeOperationGraceMs, noticeRetentionMs, noticeRowsKept, noticeClientMax } from '../src/notices.mjs';
import { pruneStep } from '../src/retention.mjs';
import { backupIncludedTables, backupExcludedTables } from '../src/backup-database.mjs';
import { eventNames, messages } from '../admin/js/labels.js';

// 공지 보내기 (src/notices.mjs, notice-v1): sending, who is addressed, how long
// a notice shows, delivery after a sign-in, the received and seen counts,
// authorization, rate limits and what a released client sees (nothing).
const origin = 'https://notices.invalid';
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const until = async (check, what) => { for (let i = 0; i < 300; i++) { const value = await check(); if (value) return value; await delay(10); } assert.fail('Timed out: ' + what); };

// The service alone on a store, with a clock the test moves.
function service({ relay = null } = {}) {
  const store = new Store(':memory:'), pushes = [], clock = { now: Date.parse('2026-11-10T09:00:00Z') };
  const users = {};
  for (const [name, role] of [['admin', 'admin'], ['alice', 'user'], ['bob', 'user'], ['carol', 'user']]) { store.addUser({ name, passwordHash: 'fixture', role }); users[name] = store.findUser(name); }
  const signIn = name => { const token = newToken(); store.createSession(users[name], tokenHash(token), clock.now, 8 * 3600000, 'client'); return tokenHash(token); };
  const notices = new NoticeService({ store, relay, changed: value => pushes.push(value === null ? null : [...value].sort()), now: () => clock.now });
  return { store, notices, users, clock, pushes, signIn, admin: users.admin };
}
const general = (title = '오늘 오후 6시 서버 점검 안내', extra = {}) => ({ kind: 'general', title, body: '저장하고 연결을 끊어 주세요.', target: { kind: 'all' }, ...extra });
const code = action => { try { action(); } catch (error) { return error.code; } return null; };

test('a notice keeps its text as text and refuses what could mislead', () => {
  assert.deepEqual(noticeText({ title: '  점검   안내 ', body: 'https://example.invalid/a 를 열지 마세요.\r\n\r\n\r\n\r\n둘째 줄  ' }), { title: '점검 안내', body: 'https://example.invalid/a 를 열지 마세요.\n\n둘째 줄' });
  assert.deepEqual(noticeText({ title: '제목만' }), { title: '제목만', body: '' });
  assert.deepEqual(noticeText({ title: '<b>굵게</b>', body: '<script>alert(1)</script>' }), { title: '<b>굵게</b>', body: '<script>alert(1)</script>' }, 'markup is only characters');
  for (const [input, expected] of [[{ title: '' }, 'invalid_notice_title'], [{ title: '   ' }, 'invalid_notice_title'], [{ title: '가'.repeat(noticeTitleMax + 1) }, 'invalid_notice_title'], [{ title: '방향‮뒤집기' }, 'invalid_notice_title'],
    [{ title: 'a', body: '나'.repeat(noticeBodyMax + 1) }, 'invalid_notice_body'], [{ title: 'a', body: Array(14).fill('줄').join('\n') }, 'invalid_notice_body'], [{ title: 'a', body: '종\u0007' }, 'invalid_notice_body'],
    [{ title: 5 }, 'invalid_notice_text'], [{ title: 'a', body: {} }, 'invalid_notice_text'], [{}, 'invalid_notice_text']]) assert.equal(code(() => noticeText(input)), expected, JSON.stringify(input));
  assert.equal(noticeText({ title: '가'.repeat(noticeTitleMax), body: '나'.repeat(noticeBodyMax) }).body.length, noticeBodyMax, 'the limits count characters, not bytes');
});

test('everyone signed in: received by each app once, seen once per person, also after a later sign-in', () => {
  const f = service(), alicePc = f.signIn('alice'), alicePhone = f.signIn('alice'), bob = f.signIn('bob');
  const sent = f.notices.send(general(), f.admin).notice;
  assert.deepEqual([sent.kind, sent.target, sent.by, sent.addressed, sent.apps, sent.people, sent.seen, sent.active, sent.source], ['general', { kind: 'all', label: '' }, 'admin', 2, 0, 0, 0, true, 'manual']);
  assert.equal(Date.parse(sent.expiresAt) - Date.parse(sent.sentAt), noticeGeneralLifetimeMs, '일반 shows for 24 hours');
  assert.deepEqual(f.pushes, [null], 'everyone is told to read');
  const first = f.notices.clientList(f.users.alice, alicePc);
  assert.deepEqual(first.notices.map(n => [n.id, n.kind, n.title, n.body, n.seen, n.scheduledAt]), [[sent.id, 'general', '오늘 오후 6시 서버 점검 안내', '저장하고 연결을 끊어 주세요.', false, null]]);
  assert.equal(first.protocol, 'notice-v1');
  f.notices.clientList(f.users.alice, alicePc); f.notices.clientList(f.users.alice, alicePc);
  const counts = () => { const row = f.notices.adminList().notices[0]; return [row.apps, row.people, row.seen]; };
  assert.deepEqual(counts(), [1, 1, 0], 'the same app asking again is one app');
  f.notices.clientList(f.users.alice, alicePhone);
  assert.deepEqual(counts(), [2, 1, 0], 'a second device of the same person: two apps, one person');
  f.notices.clientList(f.users.bob, bob);
  assert.deepEqual(counts(), [3, 2, 0]);
  // Carol was not signed in when it was sent: she gets it when she signs in, while it shows.
  f.clock.now += 3 * 3600000;
  const carol = f.signIn('carol');
  assert.deepEqual(f.notices.clientList(f.users.carol, carol).notices.map(n => n.id), [sent.id]);
  assert.deepEqual(counts(), [4, 3, 0]);
  // Seen: once per person, whichever device says so, and the other devices are told.
  f.pushes.length = 0;
  assert.deepEqual(f.notices.markSeen(f.users.alice, { ids: [sent.id] }), { ok: true, marked: 1 });
  assert.deepEqual(f.notices.markSeen(f.users.alice, { ids: [sent.id] }), { ok: true, marked: 0 });
  assert.deepEqual(f.pushes, [[f.users.alice.id]]);
  assert.deepEqual(counts(), [4, 3, 1]);
  assert.equal(f.notices.clientList(f.users.alice, alicePhone).notices[0].seen, true, 'seen on the PC is seen on the phone');
  assert.equal(f.notices.clientList(f.users.bob, bob).notices[0].seen, false);
  // Expired: nobody is given it any more and the history says it ended.
  f.clock.now += noticeGeneralLifetimeMs;
  assert.deepEqual(f.notices.clientList(f.users.bob, bob).notices, []);
  assert.equal(f.notices.adminList().notices[0].active, false);
  f.store.close();
});

test('a notice somebody did not receive cannot be marked seen, and ids are checked', () => {
  const f = service(), alice = f.signIn('alice');
  const sent = f.notices.send(general('앨리스만', { target: { kind: 'user', username: 'alice' } }), f.admin).notice;
  assert.deepEqual(f.notices.markSeen(f.users.bob, { ids: [sent.id] }), { ok: true, marked: 0 }, 'not addressed to bob');
  assert.deepEqual(f.notices.markSeen(f.users.alice, { ids: [sent.id] }), { ok: true, marked: 0 }, 'addressed but not delivered yet');
  f.notices.clientList(f.users.alice, alice);
  assert.deepEqual(f.notices.markSeen(f.users.alice, { ids: [sent.id, 'f'.repeat(32)] }), { ok: true, marked: 1 });
  for (const ids of [[], ['x'], [sent.id, sent.id], 'nope', Array.from({ length: noticeClientMax + 1 }, (_, i) => i.toString(16).padStart(32, '0'))]) assert.equal(code(() => f.notices.markSeen(f.users.alice, { ids })), 'invalid_notice_ids');
  assert.equal(code(() => f.notices.markSeen(f.users.alice, { ids: [sent.id], extra: 1 })), 'unexpected_field');
  f.store.close();
});

test('one person: only that account, online or not; a stopped or unknown account is refused', () => {
  const f = service(), bob = f.signIn('bob');
  const sent = f.notices.send(general('밥에게', { target: { kind: 'user', username: 'alice' } }), f.admin).notice;
  assert.deepEqual([sent.target, sent.addressed], [{ kind: 'user', label: 'alice' }, 1]);
  assert.deepEqual(f.pushes, [[f.users.alice.id]], 'only that account is told');
  assert.deepEqual(f.notices.clientList(f.users.bob, bob).notices, []);
  // Alice was offline: it waits for her next sign-in.
  f.clock.now += 3600000;
  assert.deepEqual(f.notices.clientList(f.users.alice, f.signIn('alice')).notices.map(n => n.title), ['밥에게']);
  assert.equal(code(() => f.notices.send(general('x', { target: { kind: 'user', username: 'nobody' } }), f.admin)), 'user_not_found');
  assert.equal(code(() => f.notices.send(general('x', { target: { kind: 'user', username: 'a b' } }), f.admin)), 'invalid_notice_target');
  f.store.setUserEnabled('carol', false, f.admin.id, f.users.carol.id);
  assert.equal(code(() => f.notices.send(general('x', { target: { kind: 'user', username: 'carol' } }), f.admin)), 'notice_no_recipients');
  for (const target of [undefined, null, 'all', [], { kind: 'everyone' }, { kind: 'all', username: 'alice' }, { kind: 'user' }, { kind: 'desktop', desktopIds: [] }, { kind: 'desktop', desktopIds: ['pve-1', 'pve-1'] }, { kind: 'desktop', desktopIds: ['../x'] }])
    assert.ok(['invalid_notice_target', 'unexpected_field'].includes(code(() => f.notices.send(general('x', { target }), f.admin))), JSON.stringify(target));
  f.store.close();
});

test('the people using a desktop: who is connected at that moment, by stream or Console', () => {
  const sessions = new Map(), grants = new Map();
  const f = service({ relay: { sessions, console: { grants } } });
  f.store.syncProxmox([{ vmid: 149, node: 'n', name: '디자인 1', status: 'running' }, { vmid: 150, node: 'n', name: '개발 1', status: 'running' }, { vmid: 151, node: 'n', name: '미등록', status: 'running' }], 'fixture');
  for (const vmid of [149, 150]) { const enroll = newToken(), uuid = `00000000-1234-1234-1234-000000000${vmid}`; f.store.issueEnrollment('pve-' + vmid, uuid, tokenHash(enroll), 'fixture'); f.store.consumeEnrollment(tokenHash(enroll), uuid, { hostname: 'h', version: '1.0.52', tokenHash: tokenHash(newToken()) }); }
  const alice = f.signIn('alice'), bob = f.signIn('bob'), carol = f.signIn('carol');
  assert.equal(code(() => f.notices.send(general('x', { target: { kind: 'desktop', desktopIds: ['pve-149'] } }), f.admin)), 'notice_no_recipients', 'nobody is connected');
  assert.equal(code(() => f.notices.send(general('x', { target: { kind: 'desktop', desktopIds: ['pve-151'] } }), f.admin)), 'desktop_not_found', 'not a registered desktop');
  sessions.set('s1', { id: 's1', desktopId: 'pve-149', userId: f.users.alice.id, ws: {} });
  sessions.set('s2', { id: 's2', desktopId: 'pve-149', userId: f.users.bob.id, ws: null }); // a ticket nobody attached to
  grants.set('g1', { desktopId: 'pve-150', userId: f.users.carol.id, closed: false });
  grants.set('g2', { desktopId: 'pve-150', userId: f.users.bob.id, closed: true });
  const one = f.notices.send(general('디자인 1을 다시 시작해요', { target: { kind: 'desktop', desktopIds: ['pve-149'] } }), f.admin).notice;
  assert.deepEqual([one.target, one.addressed], [{ kind: 'desktop', label: '디자인 1' }, 1]);
  assert.deepEqual(f.pushes.at(-1), [f.users.alice.id]);
  assert.deepEqual([f.notices.clientList(f.users.alice, alice).notices.length, f.notices.clientList(f.users.bob, bob).notices.length, f.notices.clientList(f.users.carol, carol).notices.length], [1, 0, 0]);
  const both = f.notices.send(general('Host를 업데이트해요', { target: { kind: 'desktop', desktopIds: ['pve-149', 'pve-150'] } }), f.admin).notice;
  assert.deepEqual([both.target, both.addressed], [{ kind: 'desktop', label: '데스크톱 2대' }, 2]);
  assert.deepEqual(f.pushes.at(-1), [f.users.alice.id, f.users.carol.id].sort());
  assert.deepEqual(f.notices.clientList(f.users.carol, carol).notices.map(n => n.title), ['Host를 업데이트해요']);
  // Alice's stream ends and she signs in again: the notice addressed to her is still hers.
  sessions.clear();
  assert.equal(f.notices.clientList(f.users.alice, f.signIn('alice')).notices.length, 2);
  f.store.close();
});

test('점검 예고 shows until its time; 먼저 알리기 until ten minutes after the wait', () => {
  const f = service(), alice = f.signIn('alice');
  const at = f.clock.now + 2 * 3600000;
  const sent = f.notices.send({ kind: 'maintenance', title: '서버 점검', body: '', target: { kind: 'all' }, scheduledAt: new Date(at).toISOString() }, f.admin).notice;
  assert.deepEqual([sent.kind, Date.parse(sent.scheduledAt), Date.parse(sent.expiresAt)], ['maintenance', at, at]);
  assert.equal(f.notices.clientList(f.users.alice, alice).notices[0].scheduledAt, new Date(at).toISOString());
  for (const scheduledAt of [undefined, null, 'soon', 5, new Date(f.clock.now - 1000).toISOString(), new Date(f.clock.now + 31 * 86400000).toISOString()])
    assert.equal(code(() => f.notices.send({ kind: 'maintenance', title: 'x', target: { kind: 'all' }, scheduledAt }, f.admin)), 'invalid_notice_schedule', String(scheduledAt));
  assert.equal(code(() => f.notices.send(general('x', { scheduledAt: new Date(at).toISOString() }), f.admin)), 'invalid_notice_schedule', '일반 has no time');
  assert.equal(code(() => f.notices.send({ ...general('x'), kind: 'urgent' }, f.admin)), 'invalid_notice_kind');
  const operation = f.notices.send({ title: '서버를 업데이트해요', body: '1분 뒤 연결이 잠시 끊겨요.', target: { kind: 'all' }, operation: { waitSeconds: 60 } }, f.admin).notice;
  assert.deepEqual([operation.kind, operation.source, Date.parse(operation.scheduledAt) - f.clock.now, Date.parse(operation.expiresAt) - f.clock.now], ['maintenance', 'operation', 60000, 60000 + noticeOperationGraceMs]);
  for (const bad of [{ waitSeconds: -1 }, { waitSeconds: 1.5 }, { waitSeconds: 3601 }, {}, { waitSeconds: 60, more: 1 }, 'now']) assert.ok(['invalid_notice_schedule', 'unexpected_field'].includes(code(() => f.notices.send({ title: 'x', target: { kind: 'all' }, operation: bad }, f.admin))), JSON.stringify(bad));
  assert.equal(code(() => f.notices.send({ kind: 'general', title: 'x', target: { kind: 'all' }, operation: { waitSeconds: 0 } }, f.admin)), 'invalid_notice_schedule');
  f.clock.now = at;
  assert.deepEqual(f.notices.clientList(f.users.alice, alice).notices, [], 'both ended');
  f.store.close();
});

test('withdrawing stops a notice at once and keeps it in the history', () => {
  const f = service(), alice = f.signIn('alice');
  const sent = f.notices.send(general('잘못 보낸 공지', { target: { kind: 'user', username: 'alice' } }), f.admin).notice;
  f.notices.clientList(f.users.alice, alice);
  f.pushes.length = 0;
  const ended = f.notices.withdraw(sent.id, f.admin).notice;
  assert.deepEqual([ended.active, typeof ended.withdrawnAt, ended.apps], [false, 'string', 1]);
  assert.deepEqual(f.pushes, [[f.users.alice.id]], 'her apps drop it');
  assert.deepEqual(f.notices.clientList(f.users.alice, alice).notices, []);
  assert.equal(code(() => f.notices.withdraw(sent.id, f.admin)), 'notice_already_ended');
  assert.equal(code(() => f.notices.withdraw('0'.repeat(32), f.admin)), 'notice_not_found');
  assert.equal(code(() => f.notices.withdraw('nope', f.admin)), 'notice_not_found');
  assert.deepEqual(f.store.db.prepare("SELECT event,actor,subject FROM audit_events WHERE event LIKE 'notice_%' ORDER BY id").all().map(row => [row.event, row.actor, row.subject]),
    [['notice_sent', f.admin.id, `${f.users.alice.id}:general:1`], ['notice_withdrawn', f.admin.id, 'user:general']], 'the record names who and to whom, never the text');
  f.store.close();
});

test('sending is limited per administrator and in all, reading per account', () => {
  const store = new Store(':memory:');
  for (const [name, role] of [['admin', 'admin'], ['second', 'admin'], ['alice', 'user']]) store.addUser({ name, passwordHash: 'fixture', role });
  const clock = { now: 1_800_000_000_000 }, notices = new NoticeService({ store, now: () => clock.now, sendsPerMinute: 3, readsPerMinute: 4 });
  const admin = store.findUser('admin'), second = store.findUser('second'), alice = store.findUser('alice');
  for (let i = 0; i < 3; i++) notices.send(general('공지 ' + i), admin);
  assert.equal(code(() => notices.send(general('넷째'), admin)), 'too_many_notices');
  for (let i = 0; i < 3; i++) notices.send(general('둘째 관리자 ' + i), second);
  clock.now += 61000;
  notices.send(general('일 분 뒤'), admin);
  const hash = tokenHash(newToken());
  for (let i = 0; i < 4; i++) notices.clientList(alice, hash);
  assert.equal(code(() => notices.clientList(alice, hash)), 'too_many_notice_requests');
  assert.equal(store.db.prepare('SELECT count(*) n FROM notices').get().n, 7, 'a refused send writes nothing');
  store.close();
});

test('a client is given at most the newest twenty', () => {
  const f = service(), alice = f.signIn('alice');
  const notices = new NoticeService({ store: f.store, now: () => f.clock.now, sendsPerMinute: 1000 });
  for (let i = 0; i < noticeClientMax + 5; i++) notices.send(general('공지 ' + i), f.admin);
  const list = notices.clientList(f.users.alice, alice).notices;
  assert.deepEqual([list.length, list[0].title, list.at(-1).title], [noticeClientMax, '공지 24', '공지 5']);
  f.store.close();
});

test('retention: ended notices go after 180 days or beyond the newest rows, one that still shows never', () => {
  const f = service(), notices = new NoticeService({ store: f.store, now: () => f.clock.now, sendsPerMinute: 100000 });
  const old = notices.send(general('오래된 공지'), f.admin).notice;
  notices.clientList(f.users.alice, f.signIn('alice'));
  assert.equal(f.store.db.prepare('SELECT count(*) n FROM notice_deliveries').get().n, 1);
  // 31 days after its end the per-account rows go, the notice and its counts stay.
  f.clock.now += noticeGeneralLifetimeMs + 31 * 86400000;
  while (pruneStep(f.store, f.clock.now));
  assert.deepEqual([f.store.db.prepare('SELECT count(*) n FROM notice_deliveries').get().n, f.store.db.prepare('SELECT count(*) n FROM notice_recipients').get().n, notices.adminList().notices.map(n => [n.id, n.apps, n.people])], [0, 0, [[old.id, 1, 1]]]);
  f.clock.now += noticeRetentionMs;
  const kept = notices.send({ kind: 'maintenance', title: '아직 보이는 예고', target: { kind: 'all' }, scheduledAt: new Date(f.clock.now + 86400000).toISOString() }, f.admin).notice;
  while (pruneStep(f.store, f.clock.now));
  assert.deepEqual(notices.adminList().notices.map(n => n.id), [kept.id], 'the old one is gone');
  // Beyond the row limit the oldest ended ones go; the one that still shows stays whatever its age.
  const insert = f.store.db.prepare("INSERT INTO notices(public_id,kind,title,body,target_kind,target_label,created_at,expires_at,actor,actor_name) VALUES(?,'general','t','','all','',?,?,?,'admin')");
  f.store.transaction(() => { for (let i = 0; i < noticeRowsKept + 30; i++) insert.run(randomBytes(16).toString('hex'), f.clock.now, f.clock.now - 1, f.admin.id); });
  while (pruneStep(f.store, f.clock.now));
  const left = f.store.db.prepare('SELECT count(*) n,min(id) first FROM notices').get();
  assert.ok(left.n <= noticeRowsKept + 1 && left.n >= noticeRowsKept - 1, String(left.n));
  assert.equal(f.store.db.prepare('SELECT count(*) n FROM notices WHERE public_id=?').get(kept.id).n, 1);
  f.store.close();
});

test('the tables are additive, classified for backups, and an account takes its rows with it', () => {
  const f = service(), alice = f.signIn('alice');
  for (const table of ['notices', 'notice_recipients', 'notice_deliveries']) { assert.ok(backupExcludedTables.includes(table), table); assert.equal(backupIncludedTables.includes(table), false); }
  assert.equal(f.store.db.prepare('PRAGMA user_version').get().user_version, 19, 'no schema version change');
  const sent = f.notices.send(general(), f.admin).notice;
  f.notices.clientList(f.users.alice, alice); f.notices.markSeen(f.users.alice, { ids: [sent.id] });
  f.store.deleteUser('alice', f.admin.id, f.users.alice.id);
  assert.deepEqual([f.store.db.prepare('SELECT count(*) n FROM notice_recipients').get().n, f.store.db.prepare('SELECT count(*) n FROM notice_deliveries').get().n], [0, 0]);
  const row = f.notices.adminList().notices[0];
  assert.deepEqual([row.apps, row.people, row.seen], [1, 1, 1], 'the counts stay with the notice');
  for (const name of ['notice_sent', 'notice_withdrawn']) assert.match(eventNames[name], /[가-힣]/);
  for (const name of ['invalid_notice_title', 'invalid_notice_body', 'invalid_notice_target', 'invalid_notice_schedule', 'invalid_notice_kind', 'invalid_notice_text', 'notice_no_recipients', 'too_many_notices', 'notice_not_found', 'notice_already_ended', 'notices_unavailable']) assert.match(messages[name] ?? '', /[가-힣]/, name);
  f.store.close();
});

// ---- The real HTTP surface, the live channel and a stream's control socket.
async function broker(t) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32)), tokens = {};
  for (const [name, role, purpose] of [['admin', 'admin', 'admin'], ['alice', 'user', 'client'], ['bob', 'user', 'client']]) {
    store.addUser({ name, passwordHash: 'fixture', role });
    tokens[name] = newToken(); store.createSession(store.findUser(name), tokenHash(tokens[name]), Date.now(), 3600000, purpose, purpose === 'admin' ? { issuer: 'admin' } : {});
  }
  // Bob is signed in on a second device too.
  tokens.bobPhone = newToken(); store.createSession(store.findUser('bob'), tokenHash(tokens.bobPhone), Date.now(), 3600000, 'client');
  const vms = [{ vmid: 149, node: 'fixture', name: 'TESTER', status: 'running' }];
  store.syncProxmox(vms, 'fixture'); store.assign('alice', 'pve-149');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve.invalid', tokenId: 'fixture@pve!fixture', secret: 'fixture' }), 'fixture');
  const enroll = newToken(), machine = '00000000-1234-1234-1234-000000000149', hostToken = newToken();
  store.issueEnrollment('pve-149', machine, tokenHash(enroll), 'fixture');
  store.consumeEnrollment(tokenHash(enroll), machine, { hostname: 'fixture', version: '1.0.52', tokenHash: tokenHash(hostToken) });
  const server = await buildServer({ store, vault, publicOrigin: origin, proxmox: { inventory: async () => vms }, relayOptions: { authorizationMs: 20, heartbeatMs: 1000 }, desktopEventOptions: { tickMs: 20, authorizationMs: 20, heartbeatMs: 1000 }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const sockets = [];
  t.after(async () => { for (const ws of sockets) ws.terminate(); await new Promise(resolve => server.close(resolve)); try { store.close(); } catch { /* closed by the test */ } });
  const base = `http://127.0.0.1:${server.address().port}`, wsBase = base.replace('http:', 'ws:');
  const admin = async (method, path, data, { cookie = tokens.admin, csrf = true, sameOrigin = true } = {}) => {
    const write = method !== 'GET';
    const response = await fetch(base + '/api/v1/admin' + path, { method, headers: { ...(cookie ? { Cookie: '__Secure-multitap-admin=' + cookie } : {}), ...(write ? { ...(sameOrigin ? { Origin: origin } : {}), ...(csrf && cookie ? { 'X-CSRF-Token': vault.csrf(cookie) } : {}), 'Content-Type': 'application/json' } : {}) }, body: write ? JSON.stringify(data ?? {}) : undefined });
    return { status: response.status, text: await response.text(), get body() { return JSON.parse(this.text); } };
  };
  const client = async (as, method, path, body) => {
    const response = await fetch(base + '/api/v1' + path, { method, headers: { Authorization: 'Bearer ' + tokens[as], ...(body !== undefined ? { 'Content-Type': 'application/json' } : {}) }, body: body === undefined ? undefined : JSON.stringify(body) });
    return { status: response.status, body: await response.json() };
  };
  const socket = async (path, headers) => {
    const ws = new WebSocket(wsBase + path, { headers, perMessageDeflate: false }); sockets.push(ws);
    ws.queue = []; ws.on('message', (data, binary) => { if (!binary) ws.queue.push(JSON.parse(data.toString())); }); ws.on('error', () => {});
    await once(ws, 'open'); return ws;
  };
  // The launcher's live channel; `live` is the X-ToTB-Live header (a released client: 'resources,catalog').
  const events = async (as, live) => { const ws = await socket('/api/v1/events', { Authorization: 'Bearer ' + tokens[as], Origin: origin, ...(live ? { 'X-ToTB-Live': live } : {}) }); await until(() => ws.queue.some(m => m.type === 'desktops.ready'), 'ready'); return ws; };
  const host = await socket('/api/v1/hosts/channel', { Authorization: 'Host ' + hostToken, 'X-Multitap-Host-Protocol': 'capabilities-v1' });
  host.send(JSON.stringify({ type: 'host.capabilities', capabilities: { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true, monitors: [{ id: '0', width: 1920, height: 1080, primary: true }] } }));
  await until(() => host.queue.some(m => m.type === 'host.capabilities.ack'), 'capabilities');
  // A stream's control socket; `live` as above.
  const stream = async (as, live) => {
    const connect = await client(as, 'POST', '/desktops/pve-149/connect', {});
    assert.equal(connect.status, 200, JSON.stringify(connect.body));
    const ws = await socket('/api/v1/sessions/' + connect.body.sessionId, { Authorization: 'Bearer ' + tokens[as], ...(live ? { 'X-ToTB-Live': live } : {}) });
    await until(() => host.queue.some(m => m.type === 'session.start' && m.sessionId === connect.body.sessionId), 'session.start');
    return ws;
  };
  return { store, server, base, tokens, admin, client, events, stream, hostToken };
}
const noticesOf = ws => ws.queue.filter(m => m.type === 'notices');

test('only an administrator sends, reads the history or withdraws', async t => {
  const f = await broker(t), valid = general();
  assert.equal((await f.admin('GET', '/notices', undefined, { cookie: null })).status, 401);
  assert.equal((await f.admin('GET', '/notices', undefined, { cookie: f.tokens.alice })).status, 401, 'a client token is not an administrator session');
  assert.equal((await fetch(f.base + '/api/v1/admin/notices', { headers: { Authorization: 'Bearer ' + f.tokens.alice } })).status, 401);
  assert.equal((await f.admin('POST', '/notices', valid, { cookie: f.tokens.alice })).status, 401);
  assert.equal((await f.admin('POST', '/notices', valid, { csrf: false })).status, 403, 'a write needs the CSRF token');
  assert.equal((await f.admin('POST', '/notices', valid, { sameOrigin: false })).status, 403, 'and the same origin');
  assert.equal((await f.client('alice', 'POST', '/notices', valid)).status, 405, 'the client route only reads');
  assert.equal((await f.admin('PUT', '/notices', valid)).status, 405);
  assert.equal((await f.admin('GET', '/notices?x=1')).status, 400);
  // An administrator who became an ordinary user while the request was read is refused before anything is written.
  f.store.db.prepare("UPDATE users SET role='user' WHERE username='admin'").run();
  assert.ok([401, 403].includes((await f.admin('POST', '/notices', valid)).status));
  f.store.db.prepare("UPDATE users SET role='admin' WHERE username='admin'").run();
  assert.equal(f.store.db.prepare('SELECT count(*) n FROM notices').get().n, 0, 'none of the refused requests wrote');
  const sent = await f.admin('POST', '/notices', valid);
  assert.equal(sent.status, 201, sent.text);
  assert.deepEqual(Object.keys(sent.body.notice).sort(), ['active', 'addressed', 'apps', 'body', 'by', 'expiresAt', 'id', 'kind', 'people', 'scheduledAt', 'seen', 'sentAt', 'source', 'target', 'title', 'withdrawnAt']);
  const list = await f.admin('GET', '/notices');
  assert.equal(list.status, 200);
  assert.deepEqual([list.body.protocol, list.body.limits.title, list.body.limits.body, list.body.limits.generalHours, list.body.notices.length, list.body.audience.people, list.body.audience.apps, list.body.audience.logins.map(row => row.account).sort()], ['notice-v1', 60, 500, 24, 1, 2, 3, ['alice', 'bob', 'bob']]);
  for (const secret of [f.tokens.admin, f.tokens.alice, tokenHash(f.tokens.alice), f.hostToken]) assert.equal(list.text.includes(secret), false, 'no token or hash in the answer');
  assert.equal((await f.admin('GET', '/notices')).text, list.text, 'an unchanged audience and history is an unchanged answer (the console re-renders only on a change)');
  for (const [input, expected] of [[{ ...valid, title: '' }, 'invalid_notice_title'], [{ ...valid, extra: 1 }, 'unexpected_field'], [{ ...valid, target: { kind: 'user', username: 'nobody' } }, 'user_not_found'], [{ ...valid, target: { kind: 'desktop', desktopIds: ['pve-149'] } }, 'notice_no_recipients']]) {
    const refused = await f.admin('POST', '/notices', input); assert.equal(refused.body.error?.code, expected, refused.text);
  }
  assert.equal((await f.admin('POST', `/notices/${sent.body.notice.id}/withdraw`, {}, { cookie: f.tokens.alice })).status, 401);
  assert.equal((await f.admin('POST', `/notices/${sent.body.notice.id}/withdraw`, { more: 1 })).body.error.code, 'unexpected_field');
  assert.equal((await f.admin('POST', `/notices/${sent.body.notice.id}/withdraw`, {})).status, 200);
  assert.equal((await f.admin('POST', `/notices/${sent.body.notice.id}/withdraw`, {})).body.error.code, 'notice_already_ended');
  // The page's own files are on the server's short list of static files.
  for (const file of ['/admin/js/notices.js', '/admin/notices.css']) assert.equal((await fetch(f.base + file)).status, 200, file);
});

test('delivery: a push on the live channel for those who asked, then the read; a released client hears and reads nothing', async t => {
  const f = await broker(t);
  const current = await f.events('alice', 'resources,catalog,notices'), released = await f.events('bob', 'resources,catalog');
  const sent = (await f.admin('POST', '/notices', general())).body.notice;
  await until(() => noticesOf(current).length === 1, 'push');
  assert.deepEqual(Object.keys(noticesOf(current)[0]).sort(), ['revision', 'type'], 'the push carries no text');
  await delay(80);
  assert.deepEqual(released.queue.map(m => m.type).filter(type => !['desktops.ready', 'desktops.changed', 'resources'].includes(type)), [], 'a released client gets nothing new');
  assert.deepEqual((await f.admin('GET', '/notices')).body.notices.map(n => [n.apps, n.people, n.addressed]), [[0, 0, 2]], 'a push is not a delivery');
  const read = await f.client('alice', 'GET', '/notices');
  assert.deepEqual([read.status, read.body.protocol, read.body.notices.map(n => [n.id, n.title, n.seen])], [200, 'notice-v1', [[sent.id, sent.title, false]]]);
  // The original keys of the answer plus `features` (an older client reads protocol and notices only).
  assert.deepEqual(Object.keys(read.body).sort(), ['features', 'notices', 'protocol', 'serverTime']);
  assert.deepEqual(read.body.features, { remove: 1 });
  assert.deepEqual(Object.keys(read.body.notices[0]).sort(), ['body', 'expiresAt', 'id', 'kind', 'scheduledAt', 'seen', 'sentAt', 'title'], 'a notice is shaped as before');
  assert.deepEqual((await f.admin('GET', '/notices')).body.notices.map(n => [n.apps, n.people, n.seen]), [[1, 1, 0]]);
  // Seen: counted, and this account's apps are told (another device drops it from its unread count).
  assert.deepEqual((await f.client('alice', 'POST', '/notices/seen', { ids: [sent.id] })).body, { ok: true, marked: 1 });
  await until(() => noticesOf(current).length === 2, 'seen push');
  assert.deepEqual((await f.admin('GET', '/notices')).body.notices.map(n => [n.apps, n.people, n.seen]), [[1, 1, 1]]);
  assert.equal((await f.client('alice', 'POST', '/notices/seen', { ids: ['nope'] })).body.error.code, 'invalid_notice_ids');
  assert.equal((await f.client('alice', 'GET', '/notices?all=1')).status, 400);
  assert.equal((await fetch(f.base + '/api/v1/notices')).status, 401, 'no notice without a sign-in');
  // One person: only her connection hears it.
  const other = await f.events('bobPhone', 'notices'); // bob's second device, a current client
  await f.admin('POST', '/notices', general('앨리스에게', { target: { kind: 'user', username: 'alice' } }));
  await until(() => noticesOf(current).length === 3, 'her push');
  await delay(80);
  assert.equal(noticesOf(other).length, 0, 'not addressed to bob');
  assert.deepEqual((await f.client('bob', 'GET', '/notices')).body.notices.map(n => n.title), [sent.title]);
  // Withdrawn: her app is told and no longer given it.
  await f.admin('POST', `/notices/${sent.id}/withdraw`, {});
  await until(() => noticesOf(current).length === 4 && noticesOf(other).length === 1, 'withdraw push');
  assert.deepEqual((await f.client('alice', 'GET', '/notices')).body.notices.map(n => n.title), ['앨리스에게']);
});

test('delivery over a stream: its control socket hears it only when the client asked on that socket', async t => {
  const f = await broker(t);
  const asked = await f.stream('alice', 'notices');
  const sent = (await f.admin('POST', '/notices', { title: 'TESTER를 다시 시작해요', body: '1분 뒤 연결이 끊겨요.', target: { kind: 'desktop', desktopIds: ['pve-149'] }, operation: { waitSeconds: 60 } })).body.notice;
  assert.deepEqual([sent.target, sent.addressed, sent.kind], [{ kind: 'desktop', label: 'TESTER' }, 1, 'maintenance']);
  await until(() => noticesOf(asked).length === 1, 'stream push');
  assert.deepEqual(Object.keys(noticesOf(asked)[0]).sort(), ['revision', 'type']);
  const read = await f.client('alice', 'GET', '/notices');
  assert.deepEqual(read.body.notices.map(n => [n.title, n.kind, typeof n.scheduledAt]), [['TESTER를 다시 시작해요', 'maintenance', 'string']]);
  assert.deepEqual((await f.client('bob', 'GET', '/notices')).body.notices, [], 'bob is not on that desktop');
  assert.equal(asked.readyState, WebSocket.OPEN, 'a notice does not end the stream');
  asked.close(); await once(asked, 'close');
  // A released client's stream (no header): nothing arrives on its socket.
  const released = await until(async () => { try { return await f.stream('alice'); } catch { return null; } }, 'second stream');
  await f.admin('POST', '/notices', general('모두에게'));
  await delay(120);
  assert.deepEqual(released.queue.filter(m => m.type === 'notices'), []);
  assert.equal(released.readyState, WebSocket.OPEN);
});

// ---- 지우기: a person removes a notice from their own list (per account, kept by the broker).
test('지우기: gone for that account on every device and after the next sign-in; the history keeps it and counts it as seen', () => {
  const f = service(), alicePc = f.signIn('alice'), alicePhone = f.signIn('alice'), bob = f.signIn('bob');
  const first = f.notices.send(general('첫 공지'), f.admin).notice, second = f.notices.send(general('둘째 공지'), f.admin).notice;
  const titles = (user, hash) => f.notices.clientList(user, hash).notices.map(n => n.title);
  assert.deepEqual([titles(f.users.alice, alicePc), titles(f.users.alice, alicePhone), titles(f.users.bob, bob)], [['둘째 공지', '첫 공지'], ['둘째 공지', '첫 공지'], ['둘째 공지', '첫 공지']]);
  assert.deepEqual(f.notices.clientList(f.users.alice, alicePc).features, { remove: 1 }, 'the answer says this broker can remove');
  f.notices.markSeen(f.users.alice, { ids: [second.id] });
  f.pushes.length = 0;
  // One she had not seen and one she had: both go, the unseen one becomes seen.
  assert.deepEqual(f.notices.remove(f.users.alice, { ids: [first.id, second.id] }), { ok: true, removed: 2 });
  assert.deepEqual(f.pushes, [[f.users.alice.id]], 'her other devices are told');
  assert.deepEqual([titles(f.users.alice, alicePc), titles(f.users.alice, alicePhone)], [[], []], 'gone on the device that removed it and on the other');
  assert.deepEqual(titles(f.users.bob, bob), ['둘째 공지', '첫 공지'], 'nobody else lost it');
  // The next sign-in, hours later while the notice still shows: still gone.
  f.clock.now += 5 * 3600000;
  assert.deepEqual(titles(f.users.alice, f.signIn('alice')), []);
  // 보낸 공지 keeps both; the removed-unseen one was counted as seen, the already seen one not twice.
  const history = Object.fromEntries(f.notices.adminList().notices.map(n => [n.title, [n.active, n.people, n.seen]]));
  assert.deepEqual(history, { '첫 공지': [true, 2, 1], '둘째 공지': [true, 2, 1] });
  // Again: nothing left to remove, nobody told.
  f.pushes.length = 0;
  assert.deepEqual(f.notices.remove(f.users.alice, { ids: [first.id] }), { ok: true, removed: 0 });
  assert.deepEqual(f.pushes, []);
  // A new notice still reaches her.
  f.notices.send(general('셋째 공지'), f.admin);
  assert.deepEqual(titles(f.users.alice, alicePc), ['셋째 공지']);
  assert.deepEqual(f.store.db.prepare("SELECT count(*) n FROM audit_events WHERE event LIKE 'notice_%' AND event NOT IN ('notice_sent')").get().n, 0, 'removing from one\'s own list is not an administration record');
  f.store.close();
});

test('지우기: only one\'s own delivered notices; another account\'s or an unknown id changes nothing and answers the same', () => {
  const f = service(), alice = f.signIn('alice'), bob = f.signIn('bob');
  const hers = f.notices.send(general('앨리스만', { target: { kind: 'user', username: 'alice' } }), f.admin).notice;
  const everyone = f.notices.send(general('모두에게'), f.admin).notice;
  f.notices.clientList(f.users.alice, alice);
  // Bob tries her notice, a notice that does not exist, and one addressed to all that he has not read yet.
  const foreign = f.notices.remove(f.users.bob, { ids: [hers.id] }), unknown = f.notices.remove(f.users.bob, { ids: ['f'.repeat(32)] });
  assert.deepEqual(foreign, { ok: true, removed: 0 }); assert.deepEqual(foreign, unknown, 'the answer does not tell whether the id exists');
  assert.deepEqual(f.notices.remove(f.users.bob, { ids: [everyone.id] }), { ok: true, removed: 0 }, 'not delivered to him yet: nothing to remove');
  assert.deepEqual(f.pushes.filter(push => push?.includes(f.users.bob.id) && push.length === 1), [], 'nothing changed, nobody is told');
  assert.deepEqual(f.notices.clientList(f.users.alice, alice).notices.map(n => n.title), ['모두에게', '앨리스만'], 'her list is untouched');
  assert.deepEqual(f.notices.clientList(f.users.bob, bob).notices.map(n => n.title), ['모두에게']);
  assert.deepEqual(f.store.db.prepare('SELECT count(*) n FROM notice_recipients WHERE removed_at IS NOT NULL').get().n, 0);
  // Mixed with her own: only hers counts.
  assert.deepEqual(f.notices.remove(f.users.alice, { ids: [hers.id, 'e'.repeat(32)] }), { ok: true, removed: 1 });
  assert.deepEqual(f.notices.clientList(f.users.alice, alice).notices.map(n => n.title), ['모두에게']);
  for (const ids of [[], ['x'], [hers.id, hers.id], 'nope', null, Array.from({ length: noticeClientMax + 1 }, (_, i) => i.toString(16).padStart(32, '0'))]) assert.equal(code(() => f.notices.remove(f.users.alice, { ids })), 'invalid_notice_ids', JSON.stringify(ids));
  assert.equal(code(() => f.notices.remove(f.users.alice, { ids: [hers.id], all: true })), 'unexpected_field');
  f.store.close();
});

test('지우기: a withdrawn or expired notice is taken without an error and shows nowhere; the limit is the reading one', () => {
  const store = new Store(':memory:');
  for (const [name, role] of [['admin', 'admin'], ['alice', 'user']]) store.addUser({ name, passwordHash: 'fixture', role });
  const clock = { now: 1_800_000_000_000 }, notices = new NoticeService({ store, now: () => clock.now, readsPerMinute: 6 });
  const admin = store.findUser('admin'), alice = store.findUser('alice'), hash = tokenHash(newToken());
  const withdrawn = notices.send(general('회수한 공지'), admin).notice, short = notices.send({ kind: 'maintenance', title: '곧 끝나는 예고', target: { kind: 'all' }, scheduledAt: new Date(clock.now + 60000).toISOString() }, admin).notice;
  notices.clientList(alice, hash);
  notices.withdraw(withdrawn.id, admin); clock.now += 120000;
  assert.deepEqual(notices.clientList(alice, hash).notices, [], 'both ended');
  // Her app still held them (it had not read again): removing is accepted.
  assert.deepEqual(notices.remove(alice, { ids: [withdrawn.id, short.id] }), { ok: true, removed: 2 });
  assert.deepEqual(notices.adminList().notices.map(n => [n.title, n.active, n.seen]), [['곧 끝나는 예고', false, 1], ['회수한 공지', false, 1]]);
  assert.deepEqual(notices.clientList(alice, hash).notices, []);
  // Reads, seen reports and removals share one budget per account and minute.
  clock.now += 61000;
  for (let i = 0; i < 6; i++) notices.remove(alice, { ids: [withdrawn.id] });
  assert.equal(code(() => notices.remove(alice, { ids: [withdrawn.id] })), 'too_many_notice_requests');
  assert.equal(code(() => notices.clientList(alice, hash)), 'too_many_notice_requests');
  store.close();
});

test('지우기: the column is added to an older database in place, stays out of backups and goes with the per-account rows', () => {
  const directory = mkdtempSync(join(tmpdir(), 'multitap-notice-remove-')), file = join(directory, 'broker.sqlite');
  try {
    // A database from before the column: the notice tables without removed_at, with a delivered notice.
    let store = new Store(file);
    for (const [name, role] of [['admin', 'admin'], ['alice', 'user']]) store.addUser({ name, passwordHash: 'fixture', role });
    store.db.exec('ALTER TABLE notice_recipients DROP COLUMN removed_at');
    const clock = { now: Date.parse('2026-11-10T09:00:00Z') }, hash = tokenHash(newToken());
    const old = store.db.prepare("INSERT INTO notices(public_id,kind,title,body,target_kind,target_label,created_at,expires_at,actor,actor_name,addressed,apps,people) VALUES(?,'general','1.0.45에서 보낸 공지','','all','',?,?,?,'admin',1,1,1)")
      .run('a'.repeat(32), clock.now, clock.now + noticeGeneralLifetimeMs, store.findUser('admin').id).lastInsertRowid;
    store.db.prepare('INSERT INTO notice_recipients(notice_id,user_id,delivered_at) VALUES(?,?,?)').run(old, store.findUser('alice').id, clock.now);
    assert.deepEqual(store.db.prepare("SELECT name FROM pragma_table_info('notice_recipients')").all().map(row => row.name), ['notice_id', 'user_id', 'delivered_at', 'seen_at']);
    store.close();
    // The new broker opens it: the column appears, nothing else changes.
    store = new Store(file);
    assert.deepEqual(store.db.prepare("SELECT name FROM pragma_table_info('notice_recipients')").all().map(row => row.name), ['notice_id', 'user_id', 'delivered_at', 'seen_at', 'removed_at']);
    assert.equal(store.db.prepare('PRAGMA user_version').get().user_version, 19, 'no schema version change');
    const notices = new NoticeService({ store, now: () => clock.now }), alice = store.findUser('alice');
    assert.deepEqual(notices.clientList(alice, hash).notices.map(n => n.title), ['1.0.45에서 보낸 공지']);
    assert.deepEqual(notices.remove(alice, { ids: ['a'.repeat(32)] }), { ok: true, removed: 1 });
    assert.deepEqual(notices.clientList(alice, hash).notices, []);
    // What a server from before the column writes still fits (it names its columns), should it run on this file again.
    const other = store.db.prepare("INSERT INTO notices(public_id,kind,title,body,target_kind,target_label,created_at,expires_at,actor,actor_name) VALUES(?,'general','t','','all','',?,?,?,'admin')").run('b'.repeat(32), clock.now, clock.now + 1000, alice.id).lastInsertRowid;
    assert.equal(store.db.prepare('INSERT OR IGNORE INTO notice_recipients(notice_id,user_id) VALUES(?,?)').run(other, alice.id).changes, 1);
    // Opening it once more adds nothing twice.
    store.close(); store = new Store(file);
    assert.equal(store.db.prepare("SELECT count(*) n FROM pragma_table_info('notice_recipients') WHERE name='removed_at'").get().n, 1);
    // Backups: the tables stay excluded, so a removal never travels.
    for (const table of ['notices', 'notice_recipients', 'notice_deliveries']) { assert.ok(backupExcludedTables.includes(table), table); assert.equal(backupIncludedTables.includes(table), false, table); }
    // Retention: while the notice shows the removal is kept; 30 days after its end the per-account rows go.
    while (pruneStep(store, clock.now + 23 * 3600000));
    assert.equal(store.db.prepare('SELECT count(*) n FROM notice_recipients WHERE removed_at IS NOT NULL').get().n, 1);
    while (pruneStep(store, clock.now + noticeGeneralLifetimeMs + 31 * 86400000));
    assert.equal(store.db.prepare('SELECT count(*) n FROM notice_recipients').get().n, 0);
    assert.equal(store.db.prepare('SELECT seen FROM notices WHERE public_id=?').get('a'.repeat(32)).seen, 1, 'the count stays on the notice');
    store.close();
  } finally { rmSync(directory, { recursive: true, force: true }); }
});

test('지우기 over HTTP: the signed-in account only, its other devices are told, and the administrator\'s history is unchanged', async t => {
  const f = await broker(t);
  const phone = await f.events('bobPhone', 'notices');
  const sent = (await f.admin('POST', '/notices', general())).body.notice;
  await until(() => noticesOf(phone).length === 1, 'send push');
  assert.equal((await fetch(f.base + '/api/v1/notices/remove', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ ids: [sent.id] }) })).status, 401, 'no sign-in, no removal');
  assert.equal((await f.client('bob', 'GET', '/notices/remove')).status, 405);
  assert.equal((await f.client('bob', 'POST', '/notices/remove', { ids: ['nope'] })).body.error.code, 'invalid_notice_ids');
  assert.equal((await f.client('bob', 'POST', '/notices/remove', { ids: [sent.id], extra: 1 })).body.error.code, 'unexpected_field');
  // Not read yet by bob: nothing to remove. Then he reads it on one device and removes it there.
  assert.deepEqual((await f.client('bob', 'POST', '/notices/remove', { ids: [sent.id] })).body, { ok: true, removed: 0 });
  assert.deepEqual((await f.client('bob', 'GET', '/notices')).body.notices.map(n => n.id), [sent.id]);
  assert.deepEqual((await f.client('alice', 'GET', '/notices')).body.notices.map(n => n.id), [sent.id]);
  const removed = await f.client('bob', 'POST', '/notices/remove', { ids: [sent.id] });
  assert.deepEqual([removed.status, removed.body], [200, { ok: true, removed: 1 }]);
  await until(() => noticesOf(phone).length === 2, 'his other device is told');
  assert.deepEqual((await f.client('bobPhone', 'GET', '/notices')).body.notices, [], 'gone on his phone too');
  assert.deepEqual((await f.client('alice', 'GET', '/notices')).body.notices.map(n => n.id), [sent.id], 'alice still has it');
  // Alice cannot remove it for bob, and bob's removal did not touch the history.
  const history = (await f.admin('GET', '/notices')).body.notices;
  assert.deepEqual(history.map(n => [n.id, n.active, n.apps, n.people, n.seen]), [[sent.id, true, 2, 2, 1]], 'still in 보낸 공지; bob counts as seen');
  assert.equal(JSON.stringify(history).includes('removed'), false, 'the history has no per-person removal in it');
  assert.equal((await f.admin('POST', '/notices/remove', { ids: [sent.id] })).status, 404, 'there is no administrator route of that name');
});
