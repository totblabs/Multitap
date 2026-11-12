// One-time cleanup at broker start: an older database can hold the
// assignments a removed or replaced Host left behind, and they would
// come back with the next Host registered under the same VM number. Synthetic
// database files only.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { Store } from '../src/store.mjs';
import { newToken, tokenHash } from '../src/security.mjs';

const uuid = vmid => '12345678-1234-4234-8234-' + String(vmid).padStart(12, '0');
const rows = (store, sql, ...values) => store.db.prepare(sql).all(...values).map(row => ({ ...row }));
function enroll(store, vmid) {
  const code = tokenHash(newToken());
  store.issueEnrollment('pve-' + vmid, uuid(vmid), code, 'fixture');
  return store.consumeEnrollment(code, uuid(vmid), { hostname: 'synthetic-host', version: '1.0.44', tokenHash: tokenHash(newToken()) }).hostId;
}
// A database as a broker before this release left it: assignments on a
// registered Host (820), a removed Host (821), a VM row that never had a Host
// (822, like an imported VM row) and a quarantined but registered VM (823).
function released(t) {
  const directory = mkdtempSync(join(tmpdir(), 'multitap-hostless-'));
  const path = join(directory, 'fixture.db'); let store = new Store(path);
  t.after(() => { try { store.close(); } catch { /* Already closed. */ } rmSync(directory, { recursive: true, force: true }); });
  const admin = store.addUser({ name: 'operator', role: 'admin', passwordHash: 'synthetic' });
  const alice = store.addUser({ name: 'alice', passwordHash: 'synthetic' }), bob = store.addUser({ name: 'bob', passwordHash: 'synthetic' });
  store.syncProxmox([820, 821, 822, 823].map(vmid => ({ vmid, node: 'fixture', name: 'VM' + vmid, status: 'running', machineUuid: uuid(vmid) })), 'fixture');
  for (const vmid of [820, 821, 823]) enroll(store, vmid);
  for (const vmid of [820, 821, 822, 823]) store.assign('alice', 'pve-' + vmid, admin);
  store.assign('bob', 'pve-821', admin); store.assign('bob', 'pve-820', admin);
  store.db.prepare('UPDATE assignments SET favorite=1,last_used_at=123,clipboard_permissions=5,rdp_redirections=3,sound_card_add=1').run();
  // The released removal only revoked the Host and kept the assignments.
  store.db.prepare("UPDATE hosts SET revoked=1 WHERE desktop_id='pve-821'").run();
  store.db.prepare("UPDATE desktops SET pve_available=0 WHERE id='pve-823'").run();
  // The mark does not exist in a database written by a released broker.
  store.db.prepare("DELETE FROM settings WHERE key='hostless_assignments_cleanup'").run();
  const reopen = () => { store.close(); store = new Store(path); return store; };
  return { get store() { return store; }, reopen, admin, alice, bob };
}
const kept = store => JSON.stringify(['users', 'desktops', 'hosts'].map(table => rows(store, 'SELECT * FROM ' + table + ' ORDER BY rowid')));

test('the first start removes assignments of desktops without a current Host, once, and records the count', t => {
  const f = released(t), before = kept(f.store);
  const registered = rows(f.store, "SELECT * FROM assignments WHERE desktop_id IN ('pve-820','pve-823') ORDER BY user_id,desktop_id");
  assert.equal(rows(f.store, 'SELECT 1 FROM assignments').length, 6);
  let store = f.reopen();
  assert.deepEqual(rows(store, "SELECT * FROM assignments WHERE desktop_id IN ('pve-821','pve-822')"), [], 'removed Host and never-registered VM lose their assignments');
  assert.deepEqual(rows(store, 'SELECT * FROM assignments ORDER BY user_id,desktop_id'), registered, 'registered Hosts keep every assignment, permission and favourite, also while the VM is unavailable');
  assert.equal(kept(store), before, 'accounts, desktops and Host registrations are untouched');
  assert.deepEqual(rows(store, "SELECT event,actor,subject FROM audit_events WHERE event='hostless_assignments_removed'"), [{ event: 'hostless_assignments_removed', actor: 'server-start', subject: '3' }]);
  assert.equal(store.setting('hostless_assignments_cleanup'), '3');
  assert.equal(store.db.prepare('PRAGMA user_version').get().user_version, 19, 'no schema version change');

  // Idempotent: later starts change nothing and record nothing.
  const after = JSON.stringify(rows(store, 'SELECT * FROM assignments ORDER BY user_id,desktop_id')), audits = rows(store, 'SELECT count(*) AS n FROM audit_events')[0].n;
  store = f.reopen(); store = f.reopen();
  assert.equal(JSON.stringify(rows(store, 'SELECT * FROM assignments ORDER BY user_id,desktop_id')), after);
  assert.equal(rows(store, 'SELECT count(*) AS n FROM audit_events')[0].n, audits);
  assert.equal(rows(store, "SELECT count(*) AS n FROM audit_events WHERE event='hostless_assignments_removed'")[0].n, 1);

  // One time only: it is not a rule that keeps deleting at every start.
  store.assign('bob', 'pve-822', f.admin); store = f.reopen();
  assert.equal(rows(store, "SELECT count(*) AS n FROM assignments WHERE desktop_id='pve-822'")[0].n, 1);
});

test('a Host of another machine found at this start loses its assignments through the identity repair, not twice', t => {
  const f = released(t);
  // The released state "Host of machine A beside a cached UUID B" on 820.
  f.store.db.prepare("UPDATE desktops SET pve_machine_uuid=? WHERE id='pve-820'").run(uuid(999));
  const store = f.reopen();
  assert.deepEqual(rows(store, "SELECT * FROM assignments WHERE desktop_id='pve-820'"), []);
  assert.deepEqual(rows(store, "SELECT event,subject FROM audit_events WHERE event IN ('host_identity_changed','desktop_assignments_removed','hostless_assignments_removed') ORDER BY id"),
    [{ event: 'host_identity_changed', subject: 'pve-820' }, { event: 'desktop_assignments_removed', subject: 'pve-820:2' }, { event: 'hostless_assignments_removed', subject: '3' }]);
  assert.deepEqual(rows(store, 'SELECT desktop_id FROM assignments ORDER BY desktop_id'), [{ desktop_id: 'pve-823' }]);
});

test('a new database is marked without an audit record and nothing is ever removed from it', t => {
  const store = new Store(':memory:'); t.after(() => store.close());
  assert.equal(store.setting('hostless_assignments_cleanup'), '0');
  assert.equal(rows(store, "SELECT count(*) AS n FROM audit_events WHERE event='hostless_assignments_removed'")[0].n, 0);
});

test('a database without leftovers is marked without an audit record', t => {
  const f = released(t);
  f.store.db.prepare("DELETE FROM assignments WHERE desktop_id IN ('pve-821','pve-822')").run();
  const before = JSON.stringify(rows(f.store, 'SELECT * FROM assignments ORDER BY user_id,desktop_id')), store = f.reopen();
  assert.equal(JSON.stringify(rows(store, 'SELECT * FROM assignments ORDER BY user_id,desktop_id')), before);
  assert.equal(store.setting('hostless_assignments_cleanup'), '0');
  assert.equal(rows(store, "SELECT count(*) AS n FROM audit_events WHERE event='hostless_assignments_removed'")[0].n, 0);
});
