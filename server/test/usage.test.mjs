import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { readFileSync } from 'node:fs';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { DatabaseSync } from 'node:sqlite';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { pruneStep } from '../src/retention.mjs';
import { UsageRecorder, usageLive, usageEvents, usageSummary, usageSummaryShared, noteLogin, noteClientReport, settleLogins, installUsage, dayStart, dayKey,
  usageRetentionMs, usageRetentionDays, usageRowsKept, usagePageSize, usageMaxRangeDays, usageAllEventsDays } from '../src/usage.mjs';

// 모니터링 (src/usage.mjs): sign-in and connection history, its one-time
// backfill, the recorder the relay calls, the administrator reads and retention.
// Days are the broker's local days; the tests pin the zone.
process.env.TZ = 'Asia/Seoul';
const MIN = 60000, HOUR = 3600000, DAY = 86400000;
const at = (day, time = '00:00') => new Date(`${day}T${time}:00+09:00`).getTime();
const uuid = n => `00000000-0000-4000-8000-${String(n).padStart(12, '0')}`;
const hash = n => String(n).padStart(64, 'a');

function fixture() {
  const store = new Store(':memory:');
  for (const [name, role] of [['admin', 'admin'], ['alice', 'user'], ['bob', 'user']]) store.addUser({ name, passwordHash: 'fixture', role });
  store.syncProxmox([{ vmid: 149, node: 'n', name: '디자인 1', status: 'running' }, { vmid: 150, node: 'n', name: '개발 1', status: 'running' }], 'fixture');
  store.assign('alice', 'pve-149'); store.assign('bob', 'pve-150'); store.assign('alice', 'pve-150');
  const id = name => store.findUser(name).id;
  return { store, id };
}
// A relay as the recorder and the reads see it: the session map only.
const fakeRelay = () => ({ sessions: new Map() });
const relaySession = (f, n, account, desktopId, extra = {}) => ({ id: uuid(n), userId: f.id(account), desktopId, tokenHash: hash(n), mode: 'stream', ws: {}, streamReady: true, ...extra });
const connection = (store, n, userId, desktopId, start, end, extra = {}) => store.db.prepare(`INSERT INTO usage_connections(session_id,user_id,desktop_id,desktop_name,mode,started_at,last_seen_at,ended_at,end_reason,estimated)
  VALUES(?,?,?,?,'stream',?,?,?,?,?)`).run(uuid(n), userId, desktopId, extra.name ?? desktopId, start, end ?? extra.seen ?? start, end, end === null ? null : 'client_stopped', extra.estimated ?? 0);

test('a database of server 1.0.39 gains the usage tables, keeps its data and is backfilled once', async t => {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-usage-migration-'));
  let store = null;
  t.after(async () => { try { store?.close(); } catch { /* already closed */ } await rm(directory, { recursive: true, force: true }); });
  const path = join(directory, 'broker.sqlite'), snapshot = JSON.parse(readFileSync(new URL('./fixtures/schema-1.0.39.json', import.meta.url), 'utf8'));
  const now = Date.now(), old = new DatabaseSync(path);
  for (const type of ['table', 'index']) for (const object of snapshot.objects) if (object.type === type && object.sql) old.exec(object.sql);
  old.exec(`PRAGMA user_version=${snapshot.userVersion}`);
  const user = (id, name, role) => old.prepare("INSERT INTO users(id,username,display_name,password_hash,role,created_at) VALUES(?,?,?,'kept-hash',?,1)").run(id, name, name, role);
  user('u-admin', 'owner', 'admin'); user('u-alice', 'alice', 'user'); user('u-bob', 'bob', 'user');
  old.prepare("INSERT INTO desktops(id,name,kind,description,os,created_at,pve_vmid) VALUES('pve-149','디자인 1','vm','','Windows',1,149)").run();
  old.prepare("INSERT INTO assignments(user_id,desktop_id) VALUES('u-alice','pve-149')").run();
  old.prepare("INSERT INTO hosts(id,desktop_id,token_hash,hostname,version,machine_uuid,created_at) VALUES('h-1','pve-149',?,'WIN','1.0.45','00000000-1234-1234-1234-000000000149',1)").run(hash(90));
  old.prepare("INSERT INTO settings(key,value) VALUES('hostless_assignments_cleanup','0')").run();
  const session = (n, userId, created, purpose, issuer) => old.prepare('INSERT INTO sessions(token_hash,user_id,created_at,expires_at,purpose,issuer) VALUES(?,?,?,?,?,?)').run(hash(n), userId, created, now + HOUR, purpose, issuer);
  session(1, 'u-alice', now - 2 * HOUR, 'client', 'windows-pc-v1'); session(2, 'u-admin', now - HOUR, 'admin', 'admin');
  const audit = (offset, event, actor, subject = '') => old.prepare('INSERT INTO audit_events(created_at,event,actor,subject) VALUES(?,?,?,?)').run(now - offset, event, actor, subject);
  audit(3 * DAY, 'login_succeeded', 'u-bob'); audit(3 * DAY - 2 * HOUR, 'logout', 'u-bob');          // paired: 2 hours
  audit(2 * DAY, 'login_succeeded', 'u-bob'); audit(2 * DAY - HOUR, 'client_idle_logout', 'u-bob');    // paired: idle
  audit(DAY, 'login_succeeded', 'u-bob');                                                              // never closed
  audit(5 * HOUR, 'client_login_replaced', 'u-alice', '1'); audit(5 * HOUR, 'login_succeeded', 'u-alice'); // an earlier sign-in of alice
  audit(2 * HOUR, 'login_succeeded', 'u-alice');                                                       // the live one (same instant as its session)
  audit(HOUR, 'login_succeeded', 'u-admin'); audit(4 * HOUR, 'login_failed', 'bob');                   // an administrator's is not told apart
  const report = (sequence, events) => old.prepare('INSERT INTO client_diagnostics(user_id,received_at,run_id,sequence,report,evidence) VALUES(?,?,?,?,?,1)')
    .run('u-alice', now - 20 * MIN, 'c'.repeat(32), sequence, JSON.stringify({ protocol: 'client-diagnostics-v1', runId: 'c'.repeat(32), sequence, version: '1.0.41', events }));
  const time = offset => new Date(now - offset).toISOString();
  report(1, [{ time: time(90 * MIN), event: 'streamReady', mode: 'stream', desktopId: 'pve-149', sessionId: uuid(7), values: { ok: true } }, { time: time(60 * MIN), event: 'streamStats', mode: 'stream', desktopId: 'pve-149', sessionId: uuid(7), values: { samples: 30 } }]);
  report(2, [{ time: time(30 * MIN), event: 'streamEnd', mode: 'stream', desktopId: 'pve-149', sessionId: uuid(7), values: {} }, { time: time(25 * MIN), event: 'streamStart', mode: 'stream', desktopId: 'pve-149', sessionId: uuid(8), code: 'host_offline', values: { ok: false } }]);
  old.close();

  store = new Store(path);
  assert.equal(store.db.prepare('PRAGMA user_version').get().user_version, 19, 'additive: the schema version stays, so server 1.0.39 can still open the file');
  assert.deepEqual(store.db.prepare('SELECT id,username,role,password_hash FROM users ORDER BY id').all().map(row => ({ ...row })), [['u-admin', 'owner', 'admin'], ['u-alice', 'alice', 'user'], ['u-bob', 'bob', 'user']].map(([id, username, role]) => ({ id, username, role, password_hash: 'kept-hash' })));
  assert.equal(store.db.prepare('SELECT count(*) n FROM assignments').get().n, 1); assert.equal(store.db.prepare('SELECT count(*) n FROM sessions').get().n, 2); assert.equal(store.db.prepare('SELECT count(*) n FROM audit_events').get().n, 10);
  const logins = () => store.db.prepare('SELECT user_id,started_at,ended_at,end_reason,estimated,source,platform,token_hash IS NOT NULL live FROM usage_logins ORDER BY started_at').all().map(row => ({ ...row }));
  const first = logins();
  assert.deepEqual(first.map(row => [row.user_id, now - row.started_at, row.ended_at === null ? null : now - row.ended_at, row.end_reason, row.estimated, row.source, row.live]), [
    ['u-bob', 3 * DAY, 3 * DAY - 2 * HOUR, 'logout', 1, 'audit', 0], ['u-bob', 2 * DAY, 2 * DAY - HOUR, 'session_idle_timeout', 1, 'audit', 0], ['u-bob', DAY, null, null, 0, 'audit', 0],
    ['u-alice', 5 * HOUR, null, null, 0, 'audit', 0], ['u-alice', 2 * HOUR, null, null, 0, 'server', 1]], 'user accounts only; the live sign-in once, from its session');
  assert.equal(first.at(-1).platform, 'windows');
  const connections = () => store.db.prepare('SELECT session_id,user_id,desktop_id,desktop_name,started_at,ended_at,estimated,client_version,source FROM usage_connections').all().map(row => ({ ...row }));
  assert.deepEqual(connections(), [{ session_id: uuid(7), user_id: 'u-alice', desktop_id: 'pve-149', desktop_name: '디자인 1', started_at: now - 90 * MIN, ended_at: now - 30 * MIN, estimated: 1, client_version: '1.0.41', source: 'client' }],
    'a reported connection from its first to its last report, marked estimated; a failed start is not one');
  assert.deepEqual(JSON.parse(store.setting('usage_backfill')), { at: JSON.parse(store.setting('usage_backfill')).at, liveLogins: 1, auditLogins: 4, clientConnections: 1 });
  store.close();
  store = new Store(path);
  assert.deepEqual(logins(), first, 'the next start changes nothing'); assert.equal(connections().length, 1);
  installUsage(store); assert.deepEqual(logins(), first);
});

test('every client sign-in is recorded from start to end, and an administrator sign-in is not', () => {
  const { store, id } = fixture(), alice = store.findUser('alice'), now = Date.now();
  const rows = () => store.db.prepare('SELECT user_id,started_at,ended_at,end_reason,estimated,issuer,platform,address,client_version,token_hash FROM usage_logins ORDER BY id').all().map(row => ({ ...row }));
  store.createSession(store.findUser('admin'), hash(1), now, HOUR, 'admin', { issuer: 'admin' });
  assert.deepEqual(rows(), []);
  store.createSession(alice, hash(2), now - 30 * MIN, HOUR, 'client', { issuer: 'legacy' });
  noteLogin(store, hash(2), { address: '203.0.113.7' }); noteLogin(store, hash(2), { address: 'unknown' }); noteLogin(store, 'not-a-hash', { address: '198.51.100.1' });
  noteClientReport(store, hash(2), id('alice'), { version: '1.0.28', events: [] }); noteClientReport(store, hash(2), id('bob'), { version: '9.9.9', events: [] }); noteClientReport(store, hash(2), id('alice'), { version: '<b>', events: [] });
  assert.deepEqual(rows(), [{ user_id: id('alice'), started_at: now - 30 * MIN, ended_at: null, end_reason: null, estimated: 0, issuer: 'legacy', platform: 'android', address: '203.0.113.7', client_version: '1.0.28', token_hash: hash(2) }]);
  store.logout(hash(2));
  let [row] = rows();
  assert.equal(row.end_reason, 'logout'); assert.equal(row.token_hash, null, 'the token hash goes with the sign-in'); assert.ok(row.ended_at >= now && row.ended_at <= Date.now() + 1, 'SQLite rounds to the nearest millisecond, so it may be half a millisecond ahead');
  // Idle sign-out and replacement name their reason in session_end_reasons.
  store.createSession(alice, hash(3), now, HOUR, 'client');
  store.transaction(() => { store.db.prepare('DELETE FROM sessions WHERE token_hash=?').run(hash(3)); store.db.prepare("INSERT INTO session_end_reasons(token_hash,user_id,reason,ended_at,retain_until) VALUES(?,?,'session_idle_timeout',?,?)").run(hash(3), id('alice'), now, now + DAY); });
  assert.equal(rows()[1].end_reason, 'session_idle_timeout');
  // An expired sign-in ends when it expired, whenever its row is removed.
  store.db.prepare('INSERT INTO sessions(token_hash,user_id,created_at,expires_at,purpose,issuer) VALUES(?,?,?,?,?,?)').run(hash(4), id('alice'), now - 3 * HOUR, now - HOUR, 'client', 'windows-pc-v1');
  store.db.prepare('INSERT INTO sessions(token_hash,user_id,created_at,expires_at,purpose,issuer) VALUES(?,?,?,?,?,?)').run(hash(5), id('alice'), now - 3 * HOUR, now - 2 * HOUR, 'client', 'browser-console');
  store.pruneReplacedSessions(now, 1); // removes one of the two expired rows
  settleLogins(store, now);
  const expired = rows().slice(2);
  assert.deepEqual(expired.map(entry => [entry.platform, now - entry.ended_at, entry.end_reason, entry.token_hash]), [['windows', HOUR, 'expired', null], ['web', 2 * HOUR, 'expired', null]]);
  // A password change, a disabled account or a deleted one leaves nothing running.
  store.createSession(alice, hash(6), now, HOUR, 'client');
  store.db.prepare('DELETE FROM sessions WHERE user_id=?').run(id('alice'));
  row = rows().at(-1); assert.equal(row.end_reason, 'ended'); assert.ok(row.ended_at >= now);
  store.db.prepare('DELETE FROM usage_logins WHERE id=(SELECT max(id) FROM usage_logins)').run();
  store.db.prepare("INSERT INTO usage_logins(user_id,token_hash,started_at) VALUES(?,?,?)").run(id('alice'), hash(7), now - HOUR); // no session row at all
  settleLogins(store, now);
  assert.deepEqual([rows().at(-1)].map(entry => [entry.end_reason, entry.estimated, entry.ended_at, entry.token_hash]), [['unknown', 1, now, null]]);
  store.db.prepare('DELETE FROM users WHERE id=?').run(id('alice'));
  assert.deepEqual(rows(), [], 'an account takes its history with it');
  store.close();
});

test('a connection is timed from the attached socket to the moment the relay lets go', () => {
  const f = fixture(), { store } = f, relay = fakeRelay();
  let clock = at('2026-11-10', '09:00');
  const usage = new UsageRecorder({ store, relay, now: () => clock });
  const rows = () => store.db.prepare('SELECT session_id,user_id,desktop_id,desktop_name,mode,started_at,last_seen_at,ended_at,end_reason,estimated,platform,client_version,address FROM usage_connections ORDER BY id').all().map(row => ({ ...row }));
  store.createSession(store.findUser('alice'), hash(1), clock, DAY, 'client', { issuer: 'legacy' });
  store.db.prepare("UPDATE usage_logins SET issuer='windows-pc-v1',platform='windows'").run();
  noteLogin(store, hash(1), { address: '203.0.113.7' }); noteClientReport(store, hash(1), f.id('alice'), { version: '1.0.41', events: [] });
  const one = relaySession(f, 1, 'alice', 'pve-149');
  usage.ended(one, 'client_stopped'); assert.deepEqual(rows(), [], 'a ticket that never attached is not a connection');
  relay.sessions.set(one.id, one); usage.requested(one.id, '198.51.100.20'); usage.attached(one);
  clock += 5 * MIN; usage.attached(one); // session-resume-v1: the same connection on a new socket
  assert.deepEqual(rows(), [{ session_id: uuid(1), user_id: f.id('alice'), desktop_id: 'pve-149', desktop_name: '디자인 1', mode: 'stream', started_at: at('2026-11-10', '09:00'), last_seen_at: at('2026-11-10', '09:00'), ended_at: null, end_reason: null, estimated: 0, platform: 'windows', client_version: '1.0.41', address: '198.51.100.20' }]);
  usage.tick(); assert.equal(rows()[0].last_seen_at, clock);
  noteClientReport(store, hash(1), f.id('alice'), { version: '1.0.42', events: [{ sessionId: uuid(1) }, { sessionId: 'x' }] });
  clock += 55 * MIN; relay.sessions.delete(one.id); usage.ended(one, 'client_stopped'); usage.ended(one, 'client_stopped');
  assert.deepEqual(rows().map(row => [row.ended_at - row.started_at, row.end_reason, row.estimated, row.client_version]), [[HOUR, 'client_stopped', 0, '1.0.42']]);

  // The client vanished: the relay held the session for it and gave up later.
  const two = relaySession(f, 2, 'alice', 'pve-150', { touchInput: true, tokenHash: hash(1) });
  relay.sessions.set(two.id, two); usage.attached(two);
  clock += 20 * MIN; two.ws = null; two.detached = { reason: 'client_timeout', at: clock };
  clock += 15000; relay.sessions.delete(two.id); usage.ended(two, 'client_timeout');
  assert.deepEqual(rows().slice(1).map(row => [row.ended_at - row.started_at, row.end_reason, row.estimated, row.platform, row.address]), [[20 * MIN, 'client_timeout', 1, 'android', '203.0.113.7']], 'ends when it was lost, not when the wait ran out; marked estimated');

  // Released without a word to the recorder: its last heartbeat ends it.
  const three = relaySession(f, 3, 'bob', 'pve-150');
  relay.sessions.set(three.id, three); usage.attached(three);
  clock += 10 * MIN; usage.tick(); clock += 20000; relay.sessions.delete(three.id); clock += 20000; usage.tick(); clock += HOUR; usage.tick();
  assert.deepEqual(rows().slice(2).map(row => [row.ended_at - row.started_at, row.end_reason, row.estimated]), [[10 * MIN, 'unknown', 1]]);

  // The broker died with a connection open: the next start ends it at its last heartbeat.
  const four = relaySession(f, 4, 'bob', 'pve-150');
  relay.sessions.set(four.id, four); usage.attached(four); clock += 40 * MIN; usage.tick(); clock += 9000;
  const restarted = new UsageRecorder({ store, relay: fakeRelay(), now: () => clock + 6 * HOUR });
  restarted.recover();
  assert.deepEqual(rows().slice(3).map(row => [row.ended_at - row.started_at, row.end_reason, row.estimated]), [[40 * MIN, 'server_restart', 1]], 'six hours of downtime are not connected time');
  // A failing store never reaches the relay.
  const five = relaySession(f, 5, 'bob', 'pve-150');
  store.close();
  assert.doesNotThrow(() => { usage.attached(five); usage.ended(four, 'x'); usage.tick(); restarted.recover(); });
});

test('summary: connected time per account and desktop in local days, split at midnight, running ones up to now', async () => {
  const f = fixture(), { store } = f, alice = f.id('alice'), bob = f.id('bob'), relay = fakeRelay();
  connection(store, 1, alice, 'pve-149', at('2026-11-08', '09:00'), at('2026-11-08', '12:30'), { name: '옛 이름' });
  connection(store, 2, alice, 'pve-149', at('2026-11-08', '23:00'), at('2026-11-09', '01:00'), { name: '디자인 1' });       // crosses midnight
  connection(store, 3, bob, 'pve-149', at('2026-11-09', '10:00'), at('2026-11-09', '10:20'));
  connection(store, 4, bob, 'pve-150', at('2026-11-07', '22:00'), at('2026-11-08', '00:30'), { estimated: 1 });             // began before the range
  connection(store, 5, bob, 'pve-150', at('2026-11-10', '08:00'), null, { seen: at('2026-11-10', '08:10') });                // running
  connection(store, 6, alice, 'pve-150', at('2026-11-10', '07:00'), null, { seen: at('2026-11-10', '07:05') });              // open, but the relay no longer has it
  connection(store, 7, alice, 'pve-150', at('2026-11-01', '07:00'), at('2026-11-01', '08:00'));                              // outside
  relay.sessions.set(uuid(5), { id: uuid(5), ws: {} });
  const login = (userId, time) => store.db.prepare('INSERT INTO usage_logins(user_id,started_at,ended_at,end_reason) VALUES(?,?,?,?)').run(userId, time, time + HOUR, 'logout');
  login(alice, at('2026-11-08', '08:55')); login(alice, at('2026-11-10', '06:59')); login(bob, at('2026-11-09', '09:59')); login(bob, at('2026-11-11', '09:00'));
  const now = at('2026-11-10', '09:00'), summary = await usageSummary(store, relay, { from: '2026-11-08', to: '2026-11-10' }, now);
  assert.deepEqual({ zone: summary.timeZone, offset: summary.utcOffsetMinutes, days: summary.days, truncated: summary.truncated, retentionDays: summary.retentionDays }, { zone: 'Asia/Seoul', offset: 540, days: ['2026-11-08', '2026-11-09', '2026-11-10'], truncated: false, retentionDays: 730 });
  const hours = list => list.map(value => value / HOUR);
  assert.deepEqual(summary.users.map(user => [user.account, user.logins, user.connections, user.connectedMs / MIN, hours(user.daily), user.dailyConnections, user.firstAt, user.lastAt, user.activeMs]), [
    ['alice', 2, 3, 335, [4.5, 1, 5 / 60], [2, 0, 1], new Date(at('2026-11-08', '08:55')).toISOString(), new Date(at('2026-11-10', '07:05')).toISOString(), null],
    ['bob', 1, 2, 110, [0.5, 20 / 60, 1], [0, 1, 1], new Date(at('2026-11-08', '00:00')).toISOString(), new Date(now).toISOString(), null]]);
  assert.deepEqual(summary.desktops.map(desktop => [desktop.desktopId, desktop.desktopName, desktop.connections, desktop.connectedMs / MIN, hours(desktop.daily), desktop.users]), [
    ['pve-149', 'pve-149', 3, 350, [4.5, 1 + 20 / 60, 0], [{ account: 'alice', connections: 2, connectedMs: 330 * MIN }, { account: 'bob', connections: 1, connectedMs: 20 * MIN }]],
    ['pve-150', 'pve-150', 2, 95, [0.5, 0, 1 + 5 / 60], [{ account: 'bob', connections: 1, connectedMs: 90 * MIN }, { account: 'alice', connections: 1, connectedMs: 5 * MIN }]]]);
  assert.equal('logins' in summary.desktops[0], false);
  assert.deepEqual([summary.totals.logins, summary.totals.connections, summary.totals.connectedMs / MIN, hours(summary.totals.daily)], [3, 5, 445, [5, 1 + 20 / 60, 1 + 5 / 60]]);
  // One day; an empty range; refused ranges.
  assert.deepEqual((await usageSummary(store, relay, { from: '2026-11-09', to: '2026-11-09' }, now)).users.map(user => [user.account, user.connectedMs / MIN, user.connections]), [['alice', 60, 0], ['bob', 20, 1]]);
  const empty = await usageSummary(store, relay, { from: '2026-10-01', to: '2026-10-07' }, now);
  assert.deepEqual([empty.users, empty.desktops, empty.totals.connectedMs, empty.days.length], [[], [], 0, 7]);
  for (const [from, to] of [['2026-11-10', '2026-11-09'], ['2026-02-30', '2026-03-01'], ['2026-1-1', '2026-01-02'], ['2025-01-01', '2026-06-01'], ['x', 'y']]) assert.equal(await usageSummary(store, relay, { from, to }, now), null, `${from}..${to}`);
  assert.equal((await usageSummary(store, relay, { from: '2025-11-10', to: '2026-11-10' }, now)).days.length, usageMaxRangeDays);
  // Read a few rows at a time with a pause between: the same sums, and the broker's other work runs in between.
  let pauses = 0;
  const chunked = await usageSummary(store, relay, { from: '2026-11-08', to: '2026-11-10' }, now, { chunk: 2, pause: async () => { pauses++; } });
  assert.deepEqual(chunked, summary); assert.ok(pauses >= 2, 'paused between chunks');
  // The shared read answers the same range once for a few seconds; a refused range is not kept as an answer for another.
  const shared = usageSummaryShared(store, relay, { from: '2026-11-08', to: '2026-11-10' });
  assert.equal(usageSummaryShared(store, relay, { from: '2026-11-08', to: '2026-11-10' }), shared); assert.notEqual(usageSummaryShared(store, relay, { from: '2026-11-09', to: '2026-11-10' }), shared);
  assert.equal((await shared).users.length, 2); assert.equal(await usageSummaryShared(store, relay, { from: 'x', to: 'y' }), null);
  assert.notEqual(usageSummaryShared(store, relay, { from: '2026-11-08', to: '2026-11-10' }, 0), shared, 'an expired answer is computed again');
  // Still running since just before the 31-day look-back: found, and counted once, for the whole day.
  connection(store, 8, bob, 'pve-150', at('2026-11-08') - 31 * DAY - 1, null); relay.sessions.set(uuid(8), { id: uuid(8), ws: {} });
  assert.deepEqual((await usageSummary(store, relay, { from: '2026-11-08', to: '2026-11-08' }, now)).users.map(user => [user.account, user.connectedMs / MIN]), [['bob', 30 + 24 * 60], ['alice', 270]]);
  assert.equal(dayStart('2026-11-10'), at('2026-11-10')); assert.equal(dayKey(at('2026-11-10', '23:59')), '2026-11-10'); assert.equal(dayStart('2026-11-10', 1), at('2026-11-11'));
  store.close();
});

test('events: a range of days newest first with per-account counts, one account or one desktop, in pages', () => {
  const f = fixture(), { store } = f, alice = f.id('alice'), bob = f.id('bob'), relay = fakeRelay(), now = at('2026-11-10', '12:00');
  connection(store, 1, alice, 'pve-149', at('2026-11-10', '09:00'), at('2026-11-10', '10:00'), { name: '디자인 1' });
  connection(store, 2, bob, 'pve-150', at('2026-11-10', '11:00'), null, { name: '개발 1', seen: at('2026-11-10', '11:30') });
  connection(store, 3, alice, 'pve-150', at('2026-11-10', '11:10'), null, { name: '개발 1', seen: at('2026-11-10', '11:20') });
  connection(store, 4, alice, 'pve-149', at('2026-11-09', '23:50'), at('2026-11-10', '00:10'));
  relay.sessions.set(uuid(2), { id: uuid(2), ws: {} });
  store.createSession(store.findUser('alice'), hash(1), at('2026-11-10', '08:55'), 400 * DAY, 'client', { issuer: 'legacy' });
  noteLogin(store, hash(1), { address: '2001:db8:1:2::/64' });
  store.db.prepare("INSERT INTO usage_logins(user_id,started_at,ended_at,end_reason,estimated,source) VALUES(?,?,?,?,1,'audit')").run(bob, at('2026-11-10', '10:55'), at('2026-11-10', '11:40'), 'logout');
  store.db.prepare("INSERT INTO usage_logins(user_id,started_at,source) VALUES(?,?,'audit')").run(bob, at('2026-11-10', '07:00'));
  const day = usageEvents(store, relay, { from: '2026-11-10', to: '2026-11-10' }, now);
  assert.deepEqual([day.timeZone, day.page, day.pageSize, day.more, day.counts, day.accounts], ['Asia/Seoul', 1, usagePageSize, false, { logins: 3, connections: 3 },
    [{ account: 'bob', logins: 2, connections: 1 }, { account: 'alice', logins: 1, connections: 2 }].sort((a, b) => a.account.localeCompare(b.account))]);
  const clockOf = value => value === null ? null : new Date(value).toLocaleTimeString('en-GB', { hour: '2-digit', minute: '2-digit', timeZone: 'Asia/Seoul' });
  assert.deepEqual(day.events.map(e => [e.kind, e.account, clockOf(e.startedAt), clockOf(e.endedAt), e.live, e.durationMs === null ? null : e.durationMs / MIN, e.estimated, e.endReason, e.desktopName ?? null]), [
    ['connection', 'alice', '11:10', '11:20', false, 10, true, 'unknown', '개발 1'],      // open in the table, gone from the relay: its last heartbeat
    ['connection', 'bob', '11:00', null, true, 60, false, null, '개발 1'],
    ['login', 'bob', '10:55', '11:40', false, 45, true, 'logout', null],
    ['connection', 'alice', '09:00', '10:00', false, 60, false, 'client_stopped', '디자인 1'],
    ['login', 'alice', '08:55', null, true, 185, false, null, null],
    ['login', 'bob', '07:00', null, false, null, false, null, null]]);                    // an old record without an end
  assert.deepEqual(day.events[4], { kind: 'login', account: 'alice', startedAt: new Date(at('2026-11-10', '08:55')).toISOString(), endedAt: null, live: true, durationMs: 185 * MIN, estimated: false, endReason: null, platform: 'android', version: null, address: '2001:db8:1:2::/64', source: 'server' });
  assert.deepEqual(Object.keys(day.events[1]).sort(), ['account', 'activeMs', 'address', 'desktopId', 'desktopName', 'durationMs', 'endReason', 'endedAt', 'estimated', 'kind', 'live', 'mode', 'platform', 'sessionId', 'source', 'startedAt', 'version'].filter(key => key !== 'sessionId'));
  assert.equal(JSON.stringify(day).includes(hash(1)), false, 'no token hash leaves the broker');
  const mine = usageEvents(store, relay, { from: '2026-11-09', to: '2026-11-10', account: 'alice' }, now);
  assert.deepEqual([mine.events.map(e => e.kind), mine.counts, mine.accounts.length], [['connection', 'connection', 'login', 'connection'], { logins: 1, connections: 3 }, 1]);
  const desk = usageEvents(store, relay, { from: '2026-11-09', to: '2026-11-10', desktopId: 'pve-149' }, now);
  assert.deepEqual([desk.events.map(e => [e.kind, e.account]), desk.counts], [[['connection', 'alice'], ['connection', 'alice']], { logins: 0, connections: 2 }]);
  assert.deepEqual(usageEvents(store, relay, { from: '2026-11-10', to: '2026-11-10', account: 'nobody' }, now).events, []);
  for (const page of [0, 1.5, 501, '2']) assert.equal(usageEvents(store, relay, { from: '2026-11-10', to: '2026-11-10', page }, now), null);
  assert.equal(usageEvents(store, relay, { from: '2026-11-11', to: '2026-11-10' }, now), null);
  // Everyone's events at once: a month at most. One account or one desktop may span the whole range.
  assert.equal(usageAllEventsDays, 31);
  assert.equal(usageEvents(store, relay, { from: '2026-10-10', to: '2026-11-10' }, now), null); assert.ok(usageEvents(store, relay, { from: '2026-10-11', to: '2026-11-10' }, now));
  assert.equal(usageEvents(store, relay, { from: '2025-11-10', to: '2026-11-10', account: 'alice' }, now).events.length, 4); assert.equal(usageEvents(store, relay, { from: '2025-11-10', to: '2026-11-10', desktopId: 'pve-149' }, now).events.length, 2);
  // Paging: newest first, each row once.
  for (let n = 0; n < usagePageSize + 30; n++) connection(store, 1000 + n, bob, 'pve-150', at('2026-11-05', '00:00') + n * MIN, at('2026-11-05', '00:00') + n * MIN + 30000);
  const one = usageEvents(store, relay, { from: '2026-11-05', to: '2026-11-05' }, now), two = usageEvents(store, relay, { from: '2026-11-05', to: '2026-11-05', page: 2 }, now);
  assert.deepEqual([one.events.length, one.more, two.events.length, two.more, one.counts.connections], [usagePageSize, true, 30, false, usagePageSize + 30]);
  assert.equal(new Set([...one.events, ...two.events].map(e => e.startedAt)).size, usagePageSize + 30); assert.ok(one.events[0].startedAt > one.events.at(-1).startedAt && one.events.at(-1).startedAt > two.events[0].startedAt);
  store.close();
});

test('live: who is signed in and who is connected now, today and the running totals', () => {
  const f = fixture(), { store } = f, relay = fakeRelay(), now = Date.now();
  const usage = new UsageRecorder({ store, relay, now: () => now - 10 * MIN });
  store.createSession(store.findUser('alice'), hash(1), now - 3 * HOUR, DAY, 'client', { issuer: 'legacy' });
  store.createSession(store.findUser('bob'), hash(2), now - HOUR, DAY, 'client', { issuer: 'legacy' });
  store.createSession(store.findUser('bob'), hash(3), now - 2 * HOUR, DAY, 'client', { issuer: 'legacy' });
  store.createSession(store.findUser('admin'), hash(4), now, DAY, 'admin', { issuer: 'admin' });
  store.db.prepare("UPDATE sessions SET issuer='windows-pc-v1' WHERE token_hash=?").run(hash(1)); store.db.prepare("UPDATE usage_logins SET issuer='windows-pc-v1',platform='windows' WHERE token_hash=?").run(hash(1));
  noteLogin(store, hash(1), { address: '203.0.113.7' }); noteClientReport(store, hash(1), f.id('alice'), { version: '1.0.41', events: [] });
  store.db.prepare('INSERT INTO session_activity(token_hash,tracking_started_at,last_input_at) VALUES(?,?,?)').run(hash(1), now - 3 * HOUR, now - 2 * MIN);   // used by the idle sign-out only
  const attached = relaySession(f, 1, 'alice', 'pve-149'), ticket = relaySession(f, 2, 'bob', 'pve-150', { ws: null, streamReady: false });
  relay.sessions.set(attached.id, attached); relay.sessions.set(ticket.id, ticket); usage.attached(attached);
  store.logout(hash(3));
  const live = usageLive(store, relay, now);
  // "Today" is the local day of `now`: just after midnight the sign-ins 1–3 hours ago are yesterday's.
  const todayFrom = dayStart(dayKey(now)), todays = starts => starts.filter(at => at >= todayFrom).length;
  assert.deepEqual([live.protocol, live.timeZone, live.people, live.today, live.totals, live.retentionDays], ['usage-v1', 'Asia/Seoul', 2,
    { date: dayKey(now), logins: todays([now - 3 * HOUR, now - HOUR, now - 2 * HOUR]), connections: todays([now - 10 * MIN]) }, { logins: 3, connections: 1 }, usageRetentionDays]);
  assert.ok(Number.isFinite(Date.parse(live.recordingSince)));
  assert.deepEqual(live.logins, [
    { account: 'bob', since: new Date(now - HOUR).toISOString(), platform: 'android', version: null, address: null, connections: 0 },
    { account: 'alice', since: new Date(now - 3 * HOUR).toISOString(), platform: 'windows', version: '1.0.41', address: '203.0.113.7', connections: 1 }], 'who, with what, from where, since when; nothing about their input');
  assert.doesNotMatch(JSON.stringify(live), /input/i);
  assert.deepEqual(live.connections, [{ sessionId: uuid(1), account: 'alice', desktopId: 'pve-149', desktopName: '디자인 1', mode: 'stream', since: new Date(now - 10 * MIN).toISOString(), ready: true, platform: 'windows', version: '1.0.41', address: '203.0.113.7', activeMs: null }], 'a ticket no client attached to is not a connection');
  for (const secret of [hash(1), hash(2)]) assert.equal(JSON.stringify(live).includes(secret), false);
  store.close();
});

test('usage history is kept for two years and at most two million rows; what still runs stays', () => {
  const f = fixture(), { store } = f, alice = f.id('alice'), now = Date.now(), old = now - usageRetentionMs - DAY;
  assert.equal(usageRetentionDays, 730); assert.equal(usageRetentionMs, 730 * DAY); assert.equal(usageRowsKept, 2000000);
  connection(store, 1, alice, 'pve-149', old, old + HOUR); connection(store, 2, alice, 'pve-149', old, null); connection(store, 3, alice, 'pve-149', now - 729 * DAY, now - 729 * DAY + HOUR);
  const login = (start, token = null) => store.db.prepare('INSERT INTO usage_logins(user_id,token_hash,started_at,ended_at) VALUES(?,?,?,?)').run(alice, token, start, token ? null : start + HOUR);
  login(old); login(old, hash(1)); login(now - 729 * DAY);
  while (pruneStep(store, now));
  assert.deepEqual(store.db.prepare('SELECT session_id FROM usage_connections ORDER BY id').all().map(row => row.session_id), [uuid(2), uuid(3)]);
  assert.deepEqual(store.db.prepare('SELECT started_at,token_hash FROM usage_logins ORDER BY id').all().map(row => [row.started_at, row.token_hash]), [[old, hash(1)], [now - 729 * DAY, null]]);
  // The row cap: ids beyond the newest two million go, oldest first.
  store.db.prepare("INSERT INTO usage_connections(id,session_id,user_id,desktop_id,desktop_name,mode,started_at,last_seen_at,ended_at) VALUES(?,?,?,'pve-149','x','stream',?,?,?)").run(usageRowsKept + 3, uuid(9), alice, now, now, now);
  store.db.prepare('INSERT INTO usage_logins(id,user_id,started_at,ended_at) VALUES(?,?,?,?)').run(usageRowsKept + 3, alice, now, now);
  while (pruneStep(store, now));
  assert.deepEqual(store.db.prepare('SELECT session_id FROM usage_connections ORDER BY id').all().map(row => row.session_id), [uuid(2), uuid(9)], 'the running connection stays');
  assert.deepEqual(store.db.prepare('SELECT id FROM usage_logins ORDER BY id').all().map(row => row.id), [2, usageRowsKept + 3], 'the live sign-in stays');
  store.close();
});

// The real relay and HTTP surface: one Host, one client, one administrator.
async function broker(t) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32)), origin = 'https://usage.invalid', tokens = {};
  for (const [name, role, purpose] of [['admin', 'admin', 'admin'], ['alice', 'user', 'client']]) {
    store.addUser({ name, passwordHash: 'fixture', role });
    tokens[name] = newToken(); store.createSession(store.findUser(name), tokenHash(tokens[name]), Date.now(), 3600000, purpose, purpose === 'admin' ? { issuer: 'admin' } : {});
  }
  const vms = [{ vmid: 149, node: 'fixture', name: 'TESTER', status: 'running' }];
  store.syncProxmox(vms, 'fixture'); store.assign('alice', 'pve-149');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve.invalid', tokenId: 'fixture@pve!fixture', secret: 'fixture' }), 'fixture');
  const code = newToken(), machine = '00000000-1234-1234-1234-000000000149', hostToken = newToken();
  store.issueEnrollment('pve-149', machine, tokenHash(code), 'fixture');
  store.consumeEnrollment(tokenHash(code), machine, { hostname: 'fixture', version: '1.0.45', tokenHash: tokenHash(hostToken) });
  const server = await buildServer({ store, vault, publicOrigin: origin, proxmox: { inventory: async () => vms }, relayOptions: { authorizationMs: 20, heartbeatMs: 1000 }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); try { store.close(); } catch { /* closed by the test */ } });
  const base = `http://127.0.0.1:${server.address().port}`;
  const admin = async (method, path, { cookie = tokens.admin, write = method !== 'GET' } = {}) => {
    const response = await fetch(base + '/api/v1/admin' + path, { method, headers: { ...(cookie ? { Cookie: '__Secure-multitap-admin=' + cookie } : {}), ...(write ? { Origin: origin, 'X-CSRF-Token': vault.csrf(cookie), 'Content-Type': 'application/json' } : {}) }, body: write ? '{}' : undefined });
    return { status: response.status, text: await response.text(), get body() { return JSON.parse(this.text); } };
  };
  const client = async (method, path, body) => {
    const response = await fetch(base + '/api/v1' + path, { method, headers: { Authorization: 'Bearer ' + tokens.alice, ...(body !== undefined ? { 'Content-Type': 'application/json' } : {}) }, body: body === undefined ? undefined : JSON.stringify(body) });
    return { status: response.status, body: await response.json() };
  };
  const host = new WebSocket(base.replace('http:', 'ws:') + '/api/v1/hosts/channel', { headers: { Authorization: 'Host ' + hostToken, 'X-Multitap-Host-Protocol': 'capabilities-v1' }, perMessageDeflate: false });
  await once(host, 'open'); t.after(() => host.close());
  const replies = []; host.on('message', data => replies.push(JSON.parse(data.toString())));
  const reply = async type => { for (let i = 0; i < 300; i++) { const index = replies.findIndex(value => value.type === type); if (index >= 0) return replies.splice(index, 1)[0]; await new Promise(resolve => setTimeout(resolve, 10)); } assert.fail('No ' + type); };
  host.send(JSON.stringify({ type: 'host.capabilities', capabilities: { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true, monitors: [{ id: '0', width: 1920, height: 1080, primary: true }] } }));
  await reply('host.capabilities.ack');
  return { store, server, base, tokens, hostToken, admin, client, reply };
}
const until = async (check, what) => { for (let i = 0; i < 300; i++) { const value = await check(); if (value) return value; await new Promise(resolve => setTimeout(resolve, 10)); } assert.fail('Timed out: ' + what); };

test('the usage reads are for administrators only, read-only, bounded and take no query', async t => {
  const f = await broker(t), today = dayKey(Date.now());
  for (const path of ['/usage/live', `/usage/summary/${today}/${today}`, `/usage/events/${today}/${today}`, `/usage/events/${today}/${today}/user/alice`, `/usage/events/${today}/${today}/desktop/pve-149/page/2`, `/usage/events/${today}/${today}/page/1`]) {
    assert.equal((await f.admin('GET', path, { cookie: null })).status, 401, path);
    assert.equal((await fetch(f.base + '/api/v1/admin' + path, { headers: { Authorization: 'Bearer ' + f.tokens.alice } })).status, 401, 'a client token is not an administrator session');
    assert.equal((await f.admin('GET', path, { cookie: f.tokens.alice })).status, 401);
    assert.equal((await f.admin('POST', path)).status, 405, path);
    const ok = await f.admin('GET', path);
    assert.equal(ok.status, 200, path); assert.equal(ok.body.protocol, 'usage-v1');
    for (const secret of [f.tokens.admin, f.tokens.alice, tokenHash(f.tokens.alice), f.hostToken]) assert.equal(ok.text.includes(secret), false, path);
  }
  for (const path of ['/usage', '/usage/', '/usage/live/x', '/usage/summary/' + today, `/usage/events/${today}/${today}/user`, `/usage/events/${today}/${today}/page/0`, `/usage/events/${today}/${today}/page/12345`, `/usage/events/${today}/${today}/other/x`]) assert.equal((await f.admin('GET', path)).status, 404, path);
  for (const path of [`/usage/summary/${today}/2020-01-01`, '/usage/summary/2026-02-30/2026-03-01', '/usage/summary/2024-01-01/2026-01-01', `/usage/events/${today}/${today}/user/%E0%A4%A`, `/usage/events/${today}/${today}/page/501`, `/usage/events/2026-01-01/2026-03-01`]) {
    const refused = await f.admin('GET', path); assert.deepEqual([refused.status, refused.body.error.code], [400, 'invalid_usage_range'], path);
  }
  assert.equal((await f.admin('GET', '/usage/live?x=1')).status, 400);
  // The page's own files are on the server's short list of static files.
  for (const file of ['/admin/js/usage.js', '/admin/usage.css']) assert.equal((await fetch(f.base + file)).status, 200, file);
  const mine = (await f.admin('GET', `/usage/events/${today}/${today}/user/${encodeURIComponent('alice')}`)).body;
  assert.deepEqual([mine.counts, mine.events.map(e => [e.kind, e.account, e.live])], [{ logins: 1, connections: 0 }, [['login', 'alice', true]]]);
});

test('a relayed connection is recorded with its address and the client version it reports, and ends with the broker', async t => {
  const f = await broker(t);
  const connect = await f.client('POST', '/desktops/pve-149/connect', {});
  assert.equal(connect.status, 200);
  assert.deepEqual((await f.admin('GET', '/usage/live')).body.connections, [], 'a ticket is not a connection');
  const ws = new WebSocket(f.base.replace('http:', 'ws:') + '/api/v1/sessions/' + connect.body.sessionId, { headers: { Authorization: 'Bearer ' + f.tokens.alice }, perMessageDeflate: false });
  await once(ws, 'open'); await f.reply('session.start');
  const live = await until(async () => { const value = (await f.admin('GET', '/usage/live')).body; return value.connections.length ? value : null; }, 'live connection');
  assert.deepEqual(live.connections.map(c => [c.sessionId, c.account, c.desktopId, c.desktopName, c.mode, c.address, c.version, c.ready]), [[connect.body.sessionId, 'alice', 'pve-149', 'TESTER', 'stream', '127.0.0.1', null, false]]);
  assert.ok(Math.abs(Date.now() - Date.parse(live.connections[0].since)) < 5000);
  assert.deepEqual([live.people, live.logins.map(login => [login.account, login.connections]), live.today.connections, live.totals.connections], [1, [['alice', 1]], 1, 1]);
  const report = await f.client('POST', '/client/diagnostics', { protocol: 'client-diagnostics-v1', runId: 'd'.repeat(32), sequence: 1, version: '1.0.42', events: [{ time: new Date().toISOString(), event: 'streamReady', mode: 'stream', desktopId: 'pve-149', sessionId: connect.body.sessionId, values: { ok: true } }] });
  assert.equal(report.status, 202);
  const versions = (await f.admin('GET', '/usage/live')).body;
  assert.deepEqual([versions.connections[0].version, versions.logins[0].version], ['1.0.42', '1.0.42']);
  // The client says goodbye: an exact end.
  ws.close(); await once(ws, 'close');
  const today = dayKey(Date.now());
  const ended = await until(async () => { const value = (await f.admin('GET', `/usage/events/${today}/${today}/desktop/pve-149`)).body; return value.events[0]?.live === false ? value : null; }, 'ended connection');
  assert.deepEqual(ended.events.map(e => [e.kind, e.account, e.estimated, e.endReason, e.address, e.version, e.source]), [['connection', 'alice', false, 'client_disconnected', '127.0.0.1', '1.0.42', 'server']]);
  assert.ok(ended.events[0].durationMs >= 0 && ended.events[0].durationMs < 5000);
  assert.deepEqual((await f.admin('GET', '/usage/live')).body.connections, []);
  // A second connection is still running when the broker stops: it ends there, exactly.
  const again = await f.client('POST', '/desktops/pve-149/connect', {});
  assert.equal(again.status, 200, JSON.stringify(again.body));
  const second = new WebSocket(f.base.replace('http:', 'ws:') + '/api/v1/sessions/' + again.body.sessionId, { headers: { Authorization: 'Bearer ' + f.tokens.alice }, perMessageDeflate: false });
  second.on('error', () => {}); await once(second, 'open');
  await until(async () => (await f.admin('GET', '/usage/live')).body.connections.length === 1, 'second connection');
  // The sign-out is recorded with its reason.
  assert.equal((await f.client('POST', '/auth/logout', {})).status, 200);
  await new Promise(resolve => f.server.close(resolve));
  const rows = f.store.db.prepare('SELECT session_id,ended_at IS NOT NULL ended,end_reason,estimated FROM usage_connections ORDER BY id').all().map(row => ({ ...row }));
  assert.deepEqual(rows.map(row => [row.session_id, row.ended, row.estimated]), [[connect.body.sessionId, 1, 0], [again.body.sessionId, 1, 0]]);
  assert.ok(['authorization_revoked', 'server_stopping', 'client_disconnected'].includes(rows[1].end_reason), rows[1].end_reason);
  assert.deepEqual(f.store.db.prepare('SELECT end_reason,token_hash FROM usage_logins').all().map(row => ({ ...row })), [{ end_reason: 'logout', token_hash: null }]);
  f.store.close();
});

// An earlier broker made usage_login_ended with CAST(...) alone, which can record a
// sign-out one millisecond early; the next start replaces that trigger with the rounding one.
test('the sign-out trigger of an earlier broker is replaced by the rounding one on start', () => {
  const { store } = fixture();
  const sql = () => store.db.prepare("SELECT sql FROM sqlite_master WHERE type='trigger' AND name='usage_login_ended'").get().sql;
  assert.match(sql(), /round\(unixepoch\('subsec'\)\*1000\)/);
  const old = sql().replaceAll("CAST(round(unixepoch('subsec')*1000) AS INTEGER)", "CAST(unixepoch('subsec')*1000 AS INTEGER)");
  store.db.exec('DROP TRIGGER usage_login_ended'); store.db.exec(old);
  assert.doesNotMatch(sql(), /round\(/);
  installUsage(store);
  assert.match(sql(), /round\(unixepoch\('subsec'\)\*1000\)/);
  installUsage(store);
  assert.equal(store.db.prepare("SELECT count(*) AS n FROM sqlite_master WHERE type='trigger' AND name LIKE 'usage_login_%'").get().n, 3);
});