import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { Store } from '../src/store.mjs';
import { safeClientDiagnosticReport, receiveClientDiagnostics, clientDiagnostics, clientDiagnosticEvidence, clientDiagnosticRetentionMs, clientDiagnosticBatchesPerUser as PER } from '../src/client-diagnostics.mjs';
import { pruneStep, RetentionSweeper, clientDiagnosticBatch, auditRetentionMs, auditNewestKept } from '../src/retention.mjs';
import { hostEventHistoryMs } from '../src/host-diagnostics.mjs';

const DAY = 86400000, operationId = '614c03e4-5595-4d77-a075-f6a61cfc4e8b';
let sequence = 0;
// One upload of a given kind: routine statistics/polling, a failure, both, or an app-level record without a VM.
function batch(kind, desktopId = 'pve-149', time = Date.now()) {
  const at = new Date(time).toISOString(), events = {
    stats: [{ time: at, event: 'streamStats', desktopId, mode: 'stream', sessionId: operationId, values: { samples: 30, decodeFps10: 598 } }],
    inventory: [{ time: at, event: 'inventory', values: { ok: true } }],
    failure: [{ time: at, event: 'displayConfigure', desktopId, operationId, mode: 'stream', code: 'display_configuration_failed', values: { ok: false } }],
    mixed: [{ time: at, event: 'streamStats', desktopId, values: { samples: 1 } }, { time: at, event: 'streamEnd', desktopId, values: {} }, { time: at, event: 'inventory', code: 'network_error', values: { ok: false } }],
    generic: [{ time: at, event: 'appStarted', values: {} }],
  }[kind];
  return { protocol: 'client-diagnostics-v1', runId: 'b'.repeat(32), sequence: ++sequence, version: '0.4.160', events };
}
function fixture(store = new Store(':memory:')) {
  for (const name of ['alice', 'bob', 'carol']) store.addUser({ name, passwordHash: 'fixture' });
  store.syncProxmox([{ vmid: 1490, node: 'n', name: 'A', status: 'running' }, { vmid: 149, node: 'n', name: 'B', status: 'running' }, { vmid: 170, node: 'n', name: 'C', status: 'running' }], 'fixture');
  for (const name of ['alice', 'bob', 'carol']) for (const vm of ['pve-1490', 'pve-149', 'pve-170']) store.assign(name, vm);
  return store;
}
// The administrator read as it was before (every batch parsed), kept as the reference.
function fullScan(store, desktopId, now) {
  const rows = [];
  for (const batch of store.db.prepare('SELECT d.id,d.received_at,d.report,u.username FROM client_diagnostics d JOIN users u ON u.id=d.user_id WHERE d.received_at>=? ORDER BY d.id DESC').all(now - clientDiagnosticRetentionMs)) {
    const report = safeClientDiagnosticReport(JSON.parse(batch.report));
    for (const event of [...report.events].reverse()) {
      if (desktopId !== null && event.desktopId !== desktopId) continue;
      rows.push({ ...event, account: batch.username, version: report.version, runId: report.runId, sequence: report.sequence, receivedAt: new Date(batch.received_at).toISOString(), source: 'client' });
    }
  }
  const routine = event => !Object.hasOwn(event, 'code') && (event.event === 'streamStats' || event.event === 'streamStall' || event.event === 'inventory' && event.values.ok === true);
  const selected = new Set(rows.filter(event => !routine(event)).slice(0, 128));
  for (const event of rows) { if (selected.size === 128) break; selected.add(event); }
  return rows.filter(event => selected.has(event));
}

test('An upload into a full store ranks no rows through json_each and keeps the exact bound', () => {
  const store = fixture(), alice = store.findUser('alice'), now = Date.now();
  try {
    for (let i = 0; i < PER; i++) receiveClientDiagnostics(store, alice, batch(i % 7 ? 'stats' : 'failure'), now);
    const prepare = store.db.prepare.bind(store.db), statements = [];
    store.db.prepare = sql => { statements.push(sql); return prepare(sql); };
    receiveClientDiagnostics(store, alice, batch('stats'), now);
    delete store.db.prepare;
    assert.ok(statements.length > 0);
    assert.equal(statements.some(sql => sql.includes('json_each')), false, 'the batch kind is stored, not recomputed per row');
    assert.equal(store.db.prepare('SELECT count(*) n FROM client_diagnostics').get().n, PER);
    assert.equal(store.db.prepare('SELECT count(*) n FROM client_diagnostics WHERE evidence=1').get().n, Math.ceil(PER / 7), 'routine rows went first');
  } finally { store.close(); }
});

test('The administrator read parses only the batches it shows and returns what the full scan returned', t => {
  const store = fixture(), users = ['alice', 'bob', 'carol'].map(name => store.findUser(name)), now = Date.now();
  try {
    const kinds = ['stats', 'stats', 'inventory', 'stats', 'failure', 'stats', 'mixed', 'inventory', 'generic', 'stats', 'stats'], vms = ['pve-1490', 'pve-149', 'pve-170'];
    for (let i = 0; i < 1500; i++) receiveClientDiagnostics(store, users[i % 3], batch(kinds[i % kinds.length], vms[(i * 7) % 3], now - (1500 - i) * 1000), now - (1500 - i) * 1000);
    // Old rows outside the 7 days stay invisible to both.
    store.db.prepare("UPDATE client_diagnostics SET received_at=received_at-? WHERE id IN (SELECT id FROM client_diagnostics ORDER BY id LIMIT 50)").run(8 * DAY);
    for (const desktopId of [null, 'pve-1490', 'pve-149', 'pve-170']) assert.deepEqual(clientDiagnostics(store, desktopId, now).events, fullScan(store, desktopId, now), String(desktopId));
    // A failure-free stretch: the newest 128 events are routine, the failures sit further back.
    for (let i = 0; i < 300; i++) receiveClientDiagnostics(store, users[i % 3], batch('stats'), now);
    for (const desktopId of [null, 'pve-149']) assert.deepEqual(clientDiagnostics(store, desktopId, now).events, fullScan(store, desktopId, now), String(desktopId));
    const parse = t.mock.method(JSON, 'parse');
    const shown = clientDiagnostics(store, null, now).events;
    assert.equal(shown.length, 128);
    assert.ok(shown.some(event => event.code === 'display_configuration_failed'));
    assert.ok(parse.mock.callCount() <= 256, `parsed ${parse.mock.callCount()} of ${store.db.prepare('SELECT count(*) n FROM client_diagnostics').get().n} batches`);
  } finally { store.close(); }
});

test('Store start fills in the batch kind of rows an older broker wrote, and the older statements still run', async t => {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-retention-')), path = join(directory, 'broker.sqlite');
  t.after(() => rm(directory, { recursive: true, force: true }));
  let store = fixture(new Store(path));
  const alice = store.findUser('alice').id, reports = ['stats', 'failure', 'inventory', 'mixed', 'generic'].map(kind => safeClientDiagnosticReport(batch(kind)));
  // An older broker's own INSERT (no kind) and its ranking DELETE.
  for (const report of reports) store.db.prepare('INSERT INTO client_diagnostics(user_id,received_at,run_id,sequence,report) VALUES(?,?,?,?,?)').run(alice, Date.now(), report.runId, report.sequence, JSON.stringify(report));
  store.db.prepare("DELETE FROM client_diagnostics WHERE id NOT IN (SELECT id FROM client_diagnostics ORDER BY CASE WHEN EXISTS (SELECT 1 FROM json_each(report,'$.events') event WHERE json_type(event.value,'$.code') IS NOT NULL) THEN 1 ELSE 0 END DESC, id DESC LIMIT 4096)").run();
  assert.equal(store.db.prepare('SELECT count(*) n FROM client_diagnostics WHERE evidence IS NULL').get().n, reports.length);
  // A damaged row must not stop the broker from starting.
  store.db.prepare("INSERT INTO client_diagnostics(user_id,received_at,run_id,sequence,report) VALUES(?,?,'damaged',1,'{not json')").run(alice, Date.now());
  store.close();
  store = new Store(path);
  try {
    assert.deepEqual(store.db.prepare('SELECT evidence FROM client_diagnostics ORDER BY id').all().map(row => row.evidence), [...reports.map(clientDiagnosticEvidence), null]);
    assert.deepEqual(reports.map(clientDiagnosticEvidence), [0, 1, 0, 1, 1]);
    assert.ok(store.db.prepare("SELECT 1 FROM pragma_index_list('audit_events') WHERE name='audit_events_created'").get());
    assert.equal(store.db.prepare('PRAGMA user_version').get().user_version, 19, 'retention adds no version beyond the current authentication schema');
  } finally { store.close(); }
});

test('A sweep removes only rows the console no longer shows, a small batch per step', () => {
  const store = fixture(), alice = store.findUser('alice'), now = Date.now();
  try {
    // Client diagnostics: 40 batches past 7 days, 5 current.
    for (let i = 0; i < 45; i++) receiveClientDiagnostics(store, alice, batch('failure'), now);
    store.db.prepare('UPDATE client_diagnostics SET received_at=? WHERE id IN (SELECT id FROM client_diagnostics ORDER BY id LIMIT 40)').run(now - clientDiagnosticRetentionMs - 1);
    // Host reports: the enrolled Host's old report is still shown (as stale); a revoked Host's is not.
    const host = store.db.prepare('INSERT INTO hosts(id,desktop_id,token_hash,hostname,machine_uuid,version,created_at,revoked) VALUES(?,?,?,?,?,?,?,?)');
    host.run('host-on', 'pve-149', 'a'.repeat(64), 'ON', 'uuid-on', '0.2.1', now, 0); host.run('host-off', 'pve-170', 'b'.repeat(64), 'OFF', 'uuid-off', '0.2.1', now, 1);
    for (const id of ['host-on', 'host-off']) store.db.prepare('INSERT INTO host_diagnostics(host_id,received_at,report) VALUES(?,?,?)').run(id, now - 60 * DAY, '{}');
    const event = store.db.prepare('INSERT INTO host_diagnostic_events(host_id,received_at,version,row) VALUES(?,?,?,?)');
    event.run('host-on', now - hostEventHistoryMs - 1, '0.2.1', 'old'); event.run('host-on', now, '0.2.1', 'new'); event.run('host-off', now, '0.2.1', 'revoked');
    // Audit: 150 rows past 180 days, then 20 current; the newest 100 stay whatever their age.
    store.db.exec('DELETE FROM audit_events');
    for (let i = 0; i < 170; i++) store.audit('login_succeeded', alice.id, '', i < 150 ? now - auditRetentionMs - DAY : now);
    assert.equal(pruneStep(store, now), true);
    assert.equal(store.db.prepare('SELECT count(*) n FROM client_diagnostics').get().n, 45 - clientDiagnosticBatch, 'one step, one small batch');
    let steps = 1; while (pruneStep(store, now)) steps++;
    assert.ok(steps >= 5 && steps < 20, `${steps} steps`);
    assert.equal(store.db.prepare('SELECT count(*) n FROM client_diagnostics').get().n, 5);
    assert.deepEqual(store.db.prepare('SELECT host_id FROM host_diagnostics').all().map(row => row.host_id), ['host-on']);
    assert.deepEqual(store.db.prepare('SELECT row FROM host_diagnostic_events').all().map(row => row.row), ['new']);
    assert.equal(store.db.prepare('SELECT count(*) n FROM audit_events').get().n, auditNewestKept);
    assert.equal(store.db.prepare('SELECT count(*) n FROM audit_events WHERE created_at=?').get(now).n, 20);
    assert.equal(store.auditLog().length, 100, 'the console still has its newest 100');
    assert.equal(pruneStep(store, now), false);
  } finally { store.close(); }
});

test('The audit row cap removes the oldest ids but never one of the newest 100', () => {
  const store = new Store(':memory:'), now = Date.now();
  try {
    store.db.exec('DELETE FROM audit_events');
    const insert = store.db.prepare("INSERT INTO audit_events(id,created_at,event,actor,subject) VALUES(?,?,'login_succeeded','x','')");
    for (let id = 1; id <= 150; id++) insert.run(id, now);
    insert.run(600150, now); // max id 600150: everything at or below 100150 is over the cap
    while (pruneStep(store, now));
    const ids = store.db.prepare('SELECT id FROM audit_events ORDER BY id').all().map(row => row.id);
    assert.equal(ids.length, 100); assert.equal(ids[0], 52); assert.equal(ids.at(-1), 600150);
  } finally { store.close(); }
});

test('The sweeper waits for a moment without remote sessions, but at most a day', async () => {
  const store = fixture(), alice = store.findUser('alice');
  let clock = Date.now(), idle = false;
  const sweeper = new RetentionSweeper({ store, idle: () => idle, intervalMs: 3600000, now: () => clock });
  try {
    receiveClientDiagnostics(store, alice, batch('failure'), clock - clientDiagnosticRetentionMs - 1);
    const count = () => store.db.prepare('SELECT count(*) n FROM client_diagnostics').get().n;
    clock += 3600000; await sweeper.tick();
    assert.equal(count(), 1, 'a session is running: wait');
    idle = true; await sweeper.tick();
    assert.equal(count(), 0, 'no session: sweep');
    receiveClientDiagnostics(store, alice, batch('failure'), clock - clientDiagnosticRetentionMs - 1);
    idle = false; clock += 23 * 3600000; await sweeper.tick();
    assert.equal(count(), 1);
    clock += 3600000; await sweeper.tick();
    assert.equal(count(), 0, 'a day without an idle moment: sweep anyway');
  } finally { sweeper.close(); store.close(); }
});
