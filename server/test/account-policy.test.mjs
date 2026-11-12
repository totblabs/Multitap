import { test } from 'node:test';
import assert from 'node:assert/strict';
import { DatabaseSync } from 'node:sqlite';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, dirname, basename, resolve } from 'node:path';
import { Store } from '../src/store.mjs';
import { Store as Store18 } from './fixtures/mfa-store18.mjs';
import { username } from '../src/security.mjs';

test('Account ids normalize Hangul and Latin case and reject whitespace, controls, separators and ambiguous symbols', () => {
  assert.equal(username('관리자'), '관리자');
  assert.equal(username('가나다'.normalize('NFD')), '가나다');
  assert.equal(username('  Main_관리자-01  '), 'main_관리자-01');
  assert.equal(username('계정.01'), '계정.01');
  for (const value of ['', 'a', '가', 'x'.repeat(65), '한 글', '가/나', '가\\나', '가\n나', '가\u200b나', '가😀나', '_관리자']) assert.throws(() => username(value), /invalid_username/);
});

test('Version 3 migration identifies the original bootstrap id, preserves legacy labels as notes and retains passwords', async t => {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-account-migration-'));
  t.after(async () => {
    assert.equal(dirname(resolve(directory)), resolve(tmpdir())); assert.ok(basename(directory).startsWith('multitap-account-migration-'));
    await rm(directory, { recursive: true });
  });
  const path = join(directory, 'broker.sqlite');
  let store = new Store18(path);
  // The QA actor must never become the main admin just because its row is older.
  store.addUser({ name: 'older-qa-admin', passwordHash: 'fixture-qa-hash', role: 'admin' }, 'deployment-qa');
  const original = store.addUser({ name: 'admin', passwordHash: 'fixture-owner-hash', role: 'admin' }, 'server-admin');
  const secondary = store.addUser({ name: 'other-admin', passwordHash: 'fixture-other-hash', role: 'admin' }, original);
  store.addDesktop({ id: 'pve-710', name: 'Fixture VM', vmid: 710 });
  store.assign('admin', 'pve-710', original);
  store.close();
  const old = new DatabaseSync(path);
  old.exec(`
    DROP INDEX users_single_primary_admin;
    ALTER TABLE users DROP COLUMN is_primary_admin;
    ALTER TABLE users DROP COLUMN notes;
    ALTER TABLE users DROP COLUMN clipboard_permissions;ALTER TABLE users DROP COLUMN rdp_redirections;
    ALTER TABLE assignments DROP COLUMN sound_card_add;ALTER TABLE assignments DROP COLUMN clipboard_permissions;ALTER TABLE assignments DROP COLUMN rdp_redirections;ALTER TABLE assignments DROP COLUMN favorite;ALTER TABLE assignments DROP COLUMN last_used_at;ALTER TABLE desktops DROP COLUMN pve_tags;ALTER TABLE desktops DROP COLUMN pve_pool;
    ALTER TABLE desktops DROP COLUMN pve_available;
    ALTER TABLE desktops DROP COLUMN pve_node;
    ALTER TABLE desktops DROP COLUMN pve_power_status;
    ALTER TABLE desktops DROP COLUMN pve_seen_at;
    ALTER TABLE desktops DROP COLUMN pve_os_type;
    ALTER TABLE desktops DROP COLUMN pve_os_family;
    ALTER TABLE desktops DROP COLUMN pve_os_source;
    ALTER TABLE desktops DROP COLUMN pve_power_checked_at;
    ALTER TABLE desktops DROP COLUMN pve_power_error;
    ALTER TABLE desktops DROP COLUMN pve_machine_uuid;
    ALTER TABLE desktops DROP COLUMN pve_machine_checked_at;
    DROP TABLE client_diagnostics;DROP TABLE host_diagnostics;DROP TABLE host_requests;
    DROP TABLE power_tasks;
    DROP TABLE hosts;
    DROP TABLE host_enrollments;
    DROP TABLE session_end_reasons;DROP TABLE session_activity;DROP INDEX sessions_windows_owner;DROP INDEX sessions_replaced;ALTER TABLE sessions DROP COLUMN replaced_at;ALTER TABLE sessions DROP COLUMN issuer;PRAGMA user_version=3;
  `);
  // Represents the old rename and role defect; identity is the same immutable id.
  old.prepare("UPDATE users SET username='chief',display_name='Chief',role='user' WHERE id=?").run(original);
  old.prepare("UPDATE users SET display_name='Other operator' WHERE id=?").run(secondary);
  old.close();
  store = new Store(path);
  try {
    assert.equal(store.db.prepare('PRAGMA user_version').get().user_version, 19);
    const primary = store.findUser('chief');
    assert.equal(primary.id, original); assert.equal(primary.is_primary_admin, 1); assert.equal(primary.role, 'admin');
    assert.equal(primary.notes, 'Chief'); assert.equal(primary.display_name, 'chief'); assert.equal(primary.password_hash, 'fixture-owner-hash');
    assert.equal(store.findUser('older-qa-admin').is_primary_admin, 0); assert.equal(store.findUser('other-admin').is_primary_admin, 0);
    assert.equal(store.findUser('other-admin').notes, 'Other operator');
    // Schema 3 had no Hosts at all: the one-time cleanup of the same start
    // (hostless-assignments-cleanup.test.mjs) removes the assignment of a desktop without one.
    assert.deepEqual(store.userAssignments(original).map(d => d.desktopId), []); assert.equal(store.setting('hostless_assignments_cleanup'), '1');
    assert.deepEqual(store.listDesktops(original), [], 'an unregistered Host is never published');
    assert.throws(() => store.updateUser('chief', { notes: '', role: 'user' }, secondary), /primary_admin_required/);
    assert.throws(() => store.setUserEnabled('chief', false, secondary), /primary_admin_required/);
    assert.throws(() => store.deleteUser('chief', secondary), /primary_admin_required/);
  } finally { store.close(); }
  const reopened = new Store(path);
  try { assert.equal(reopened.findUser('chief').is_primary_admin, 1); assert.equal(reopened.findUser('chief').notes, 'Chief'); }
  finally { reopened.close(); }
});

test('Ordinary administrators retain last-active-admin safeguards independently of the primary marker', () => {
  const store = new Store(':memory:');
  try {
    const owner = store.addUser({ name: 'owner', passwordHash: 'fixture-hash', role: 'admin' });
    store.addUser({ name: 'secondary', passwordHash: 'fixture-hash', role: 'admin' }, owner);
    // Simulate a legacy/recovery database without a primary marker to exercise the older guard.
    store.db.prepare('UPDATE users SET is_primary_admin=0 WHERE id=?').run(owner);
    store.setUserEnabled('secondary', false, 'server-admin');
    assert.throws(() => store.updateUser('owner', { role: 'user' }, 'server-admin'), /last_admin_required/);
    assert.throws(() => store.setUserEnabled('owner', false), /last_admin_required/);
    assert.throws(() => store.deleteUser('owner', 'server-admin'), /last_admin_required/);
  } finally { store.close(); }
});
