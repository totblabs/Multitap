import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { DatabaseSync } from 'node:sqlite';
import { Store } from '../src/store.mjs';
import { Store as Store18 } from './fixtures/mfa-store18.mjs';
import { devicePolicyKeys } from '../src/device-policy.mjs';

// SQLite fixtures only. No Proxmox client, network, VM hardware or OS device.
const inventory = [701, 702].map(vmid => ({ vmid, name: `Fixture ${vmid}`, node: 'fixture', status: 'stopped' }));
function fixture(t) {
  const store = new Store(':memory:'); t.after(() => store.close());
  const alice = store.addUser({ name: 'alice', passwordHash: 'synthetic-hash' });
  const bob = store.addUser({ name: 'bobby', passwordHash: 'synthetic-hash' });
  const admin = store.addUser({ name: 'admin', role: 'admin', passwordHash: 'synthetic-admin-hash' });
  store.syncProxmox(inventory, 'fixture');
  return { store, alice, bob, admin };
}
const snapshot = store => JSON.stringify({ rows: store.db.prepare('SELECT * FROM assignments ORDER BY user_id,desktop_id').all(), audit: store.db.prepare('SELECT * FROM audit_events ORDER BY id').all() });

test('sound-card permission defaults denied and observes current enabled assignment/VM', t => {
  const { store, alice, bob, admin } = fixture(t);
  store.replaceVmAssignments('alice', ['pve-701', 'pve-702']);
  store.assign('bobby', 'pve-701');
  assert.equal(store.soundCardAddAllowed(alice, 'pve-701'), false);
  assert.equal(store.soundCardAddAllowed(admin, 'pve-701'), false, 'action-layer admin bypass is separate');
  store.replaceVmAssignments('alice', [{ desktopId: 'pve-701', soundCardAdd: true }, 'pve-702']);
  assert.equal(store.soundCardAddAllowed(alice, 'pve-701'), true);
  assert.equal(store.soundCardAddAllowed(alice, 'pve-702'), false);
  assert.equal(store.soundCardAddAllowed(bob, 'pve-701'), false);
  assert.equal(store.soundCardAddAllowed(null, 'pve-701'), false);
  assert.equal(store.soundCardAddAllowed(alice, null), false);
  store.setUserEnabled('alice', false);
  assert.equal(store.soundCardAddAllowed(alice, 'pve-701'), false);
  store.setUserEnabled('alice', true);
  store.syncProxmox([inventory[1]], 'fixture');
  assert.equal(store.soundCardAddAllowed(alice, 'pve-701'), false, 'unavailable VM cannot carry an action grant');
  store.syncProxmox(inventory, 'fixture');
  assert.equal(store.soundCardAddAllowed(alice, 'pve-701'), true, 'availability never invents or clears stored permission');
});

test('omitted sound flag retains existing value, explicit false revokes, and the device policy is unchanged', t => {
  const { store, alice } = fixture(t);
  const redirects = Object.fromEntries(devicePolicyKeys.map(key => [key, key === 'drives']));
  const clipboard = { textToRemote: true, textToLocal: false, filesToRemote: false, filesToLocal: false };
  store.replaceVmAssignments('alice', [{ desktopId: 'pve-701', clipboardPolicy: clipboard, devicePolicy: redirects, soundCardAdd: true }]);
  assert.equal(store.replaceVmAssignments('alice', [{ desktopId: 'pve-701' }]).policiesChanged, 0);
  assert.equal(store.replaceVmAssignments('alice', ['pve-701']).policiesChanged, 0);
  store.updateAssignmentPolicy('alice', 'pve-701', { clipboardPolicy: clipboard });
  assert.equal(store.soundCardAddAllowed(alice, 'pve-701'), true);
  assert.deepEqual(store.clipboardPolicy(alice, 'pve-701'), clipboard);
  assert.deepEqual(store.devicePolicy(alice, 'pve-701'), redirects);
  assert.equal(Object.keys(store.devicePolicy(alice, 'pve-701')).length, devicePolicyKeys.length);
  assert.equal(store.userAssignments(alice)[0].soundCardAdd, true);
  assert.equal(store.inventory().assignments.find(row => row.username === 'alice').soundCardAdd, true);
  assert.equal(store.replaceVmAssignments('alice', [{ desktopId: 'pve-701', soundCardAdd: false }]).policiesChanged, 1);
  assert.equal(store.soundCardAddAllowed(alice, 'pve-701'), false);
  assert.deepEqual(store.devicePolicy(alice, 'pve-701'), redirects);
  assert.equal(store.auditLog().filter(row => row.event === 'sound_card_add_permission_changed').length, 2);
});

test('invalid sound flag cannot partially add/remove/change assignments or audit', t => {
  const { store } = fixture(t);
  store.replaceVmAssignments('alice', [{ desktopId: 'pve-701', soundCardAdd: true }]);
  for (const value of [null, 0, 1, 'true', {}, []]) {
    const before = snapshot(store);
    assert.throws(() => store.replaceVmAssignments('alice', [
      { desktopId: 'pve-702', soundCardAdd: true },
      { desktopId: 'pve-701', soundCardAdd: value },
    ]), /invalid_sound_card_add/);
    assert.throws(() => store.updateAssignmentPolicy('alice', 'pve-701', { soundCardAdd: value }), /invalid_sound_card_add/);
    assert.equal(snapshot(store), before);
  }
});

test('unassign deletes the sound grant; reassign and legacy policy updates remain denied', t => {
  const { store, alice } = fixture(t);
  store.replaceVmAssignments('alice', [{ desktopId: 'pve-701', soundCardAdd: true }]);
  store.replaceVmAssignments('alice', []);
  assert.equal(store.soundCardAddAllowed(alice, 'pve-701'), false);
  assert.equal(store.db.prepare('SELECT count(*) AS n FROM assignments WHERE user_id=?').get(alice).n, 0);
  store.assign('alice', 'pve-701');
  assert.equal(store.userAssignments(alice)[0].soundCardAdd, false);
  const everyDevice = Object.fromEntries(devicePolicyKeys.map(key => [key, true]));
  store.updateUser('alice', { devicePolicy: everyDevice }, 'fixture');
  assert.equal(store.soundCardAddAllowed(alice, 'pve-701'), false, 'an old account-wide redirection edit cannot grant hardware changes');
});

test('an approved physical PC cannot receive a VM sound-card grant; mixed batch is atomic', t => {
  const { store, alice } = fixture(t);
  const pc = 'pc-11111111-1111-4111-8111-111111111111', uuid = '22222222-2222-4222-8222-222222222222';
  store.addDesktop({ id: pc, name: 'Memory PC', kind: 'pc' });
  store.db.prepare('INSERT INTO hosts(id,desktop_id,token_hash,hostname,machine_uuid,version,created_at) VALUES(?,?,?,?,?,?,?)').run('fixture-pc-host', pc, 'synthetic-token-hash', 'fixture', uuid, 'fixture', 1);
  store.db.prepare('INSERT INTO local_pc_approvals(desktop_id,host_id,machine_uuid,approved_at,actor) VALUES(?,?,?,?,?)').run(pc, 'fixture-pc-host', uuid, 1, 'fixture');
  store.replaceVmAssignments('alice', ['pve-701', pc]);
  const before = snapshot(store);
  assert.throws(() => store.replaceVmAssignments('alice', [{ desktopId: 'pve-701', soundCardAdd: true }, { desktopId: pc, soundCardAdd: true }]), /sound_card_vm_only/);
  assert.throws(() => store.updateAssignmentPolicy('alice', pc, { soundCardAdd: true }), /sound_card_vm_only/);
  assert.equal(snapshot(store), before);
  assert.equal(store.soundCardAddAllowed(alice, pc), false);
  store.replaceVmAssignments('alice', ['pve-701', { desktopId: pc, soundCardAdd: false }]);
  assert.equal(store.soundCardAddAllowed(alice, pc), false);
});

test('schema15 migration preserves rows and all old permissions while adding only default-denied sound grant', async t => {
  const root = await mkdtemp(join(tmpdir(), 'multitap-sound-assignment-')), path = join(root, 'fixture.sqlite');
  let store = new Store18(path); t.after(async () => { store?.close(); await rm(root, { recursive: true, force: true }); });
  const alice = store.addUser({ name: 'alice', passwordHash: 'retained-synthetic-hash', notes: 'retained' });
  store.syncProxmox(inventory, 'fixture'); store.assign('alice', 'pve-701');
  // A registered Host: assignments of desktops without one are removed once at
  // the first start of this release (hostless-assignments-cleanup.test.mjs).
  store.db.prepare('INSERT INTO hosts(id,desktop_id,token_hash,hostname,machine_uuid,version,created_at) VALUES(?,?,?,?,?,?,?)').run('retained-host-701', 'pve-701', 'retained-token-701', 'retained', '12345678-1234-4234-8234-000000000701', '1.0.0', 1000);
  store.setFavorite(alice, 'pve-701', true); store.markUsed(alice, 'pve-701', 1234);
  store.updateAssignmentPolicy('alice', 'pve-701', {
    clipboardPolicy: { textToRemote: true, textToLocal: false, filesToRemote: true, filesToLocal: false },
    devicePolicy: Object.fromEntries(devicePolicyKeys.map(key => [key, key === 'drives'])),
  });
  store.close(); store = null;
  const previous = new DatabaseSync(path);
  previous.exec('ALTER TABLE assignments DROP COLUMN sound_card_add; DROP TABLE session_end_reasons;DROP TABLE session_activity;DROP INDEX sessions_windows_owner;DROP INDEX sessions_replaced;ALTER TABLE sessions DROP COLUMN replaced_at;ALTER TABLE sessions DROP COLUMN issuer;PRAGMA user_version=15;');
  const old = Object.fromEntries(['users', 'assignments', 'desktops', 'settings', 'audit_events'].map(table => [table, previous.prepare(`SELECT rowid,* FROM ${table} ORDER BY rowid`).all()]));
  previous.close(); store = new Store(path);
  assert.equal(store.db.prepare('PRAGMA user_version').get().user_version, 19);
  assert.equal(store.setting('hostless_assignments_cleanup'), '0', 'the one-time cleanup found nothing to remove');
  for (const table of ['users', 'desktops', 'settings', 'audit_events']) assert.deepEqual(store.db.prepare(`SELECT rowid,* FROM ${table}${table === 'settings' ? " WHERE key NOT IN ('hostless_assignments_cleanup','usage_backfill')" : ''} ORDER BY rowid`).all().map(row => ({ ...row })), old[table].map(row => table === 'users' ? { ...row, authorization_revision: 1 } : { ...row }));
  const upgraded = store.db.prepare('SELECT rowid,* FROM assignments ORDER BY rowid').all();
  assert.deepEqual(upgraded.map(({ sound_card_add, ...row }) => { assert.equal(sound_card_add, 0); return { ...row }; }), old.assignments.map(row => ({ ...row })));
  assert.throws(() => store.db.prepare('UPDATE assignments SET sound_card_add=2').run(), /CHECK/);
  store.updateAssignmentPolicy('alice', 'pve-701', { soundCardAdd: true });
  store.close(); store = null; store = new Store(path);
  assert.equal(store.soundCardAddAllowed(alice, 'pve-701'), true, 'reopening current schema is idempotent and does not reset an explicit grant');
});
