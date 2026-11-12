import { test } from 'node:test';
import assert from 'node:assert/strict';
import { DatabaseSync } from 'node:sqlite';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, dirname, basename, resolve } from 'node:path';
import { Store } from '../src/store.mjs';
import { Store as Store18 } from './fixtures/mfa-store18.mjs';

async function legacy(t,{invalid=false}={}) {
  const directory=await mkdtemp(join(tmpdir(),'multitap-power-migration-'));
  t.after(async()=>{assert.equal(dirname(resolve(directory)),resolve(tmpdir()));assert.ok(basename(directory).startsWith('multitap-power-migration-'));await rm(directory,{recursive:true});});
  const path=join(directory,'broker.sqlite');let store=new Store18(path);
  const owner=store.addUser({name:'fixture-admin',passwordHash:'retained-password-fixture',role:'admin'});
  store.syncProxmox([{vmid:710,node:'fixture-node',name:'VM710',status:'running'}],'fixture');
  store.assign('fixture-admin','pve-710','fixture');store.saveSetting('fixture-preserved','retained-setting','fixture');
  store.db.prepare(`INSERT INTO power_tasks(rowid,id,desktop_id,user_id,action,status,active,submitted_at,checked_at,guard_until,config_stamp,pve_node,pve_task,exit_status)
    VALUES(41,'old-active','pve-710',?,'shutdown','running',1,1000,1100,31000,'retained-stamp','fixture-node','UPID:fixture-node:1:2:3:qmshutdown:710:fixture@pve:',NULL)`).run(owner);
  store.db.prepare(`INSERT INTO power_tasks(rowid,id,desktop_id,user_id,action,status,active,submitted_at,checked_at,guard_until,config_stamp,pve_node,pve_task,exit_status)
    VALUES(12,'old-finished','pve-710',?,'start','stopped',0,500,600,30500,'prior-stamp','fixture-node','UPID:fixture-node:1:2:3:qmstart:710:fixture@pve:','OK')`).run(owner);
  const expected=store.db.prepare('SELECT rowid,* FROM power_tasks ORDER BY rowid').all();store.close();
  const db=new DatabaseSync(path);
  db.exec(`BEGIN;
    CREATE TABLE legacy_power (
      id TEXT PRIMARY KEY,desktop_id TEXT NOT NULL REFERENCES desktops(id) ON DELETE CASCADE,user_id TEXT NOT NULL,
      action TEXT NOT NULL CHECK(action IN ('start','shutdown','reboot')),status TEXT NOT NULL CHECK(status IN ('submitted','running','stopped','unknown')),
      active INTEGER NOT NULL DEFAULT 1,submitted_at INTEGER NOT NULL,checked_at INTEGER,guard_until INTEGER NOT NULL,config_stamp TEXT NOT NULL,pve_node TEXT,pve_task TEXT,exit_status TEXT);
    INSERT INTO legacy_power(rowid,id,desktop_id,user_id,action,status,active,submitted_at,checked_at,guard_until,config_stamp,pve_node,pve_task,exit_status) SELECT rowid,* FROM power_tasks;
    DROP TABLE power_tasks;ALTER TABLE legacy_power RENAME TO power_tasks;
    CREATE UNIQUE INDEX power_tasks_active_vm ON power_tasks(desktop_id) WHERE active=1;
    CREATE INDEX power_tasks_owner ON power_tasks(user_id,desktop_id,submitted_at);
    DROP TABLE client_diagnostics;DROP TABLE host_diagnostics;DROP TABLE host_requests;ALTER TABLE desktops DROP COLUMN pve_machine_uuid;ALTER TABLE desktops DROP COLUMN pve_machine_checked_at;ALTER TABLE users DROP COLUMN clipboard_permissions;ALTER TABLE users DROP COLUMN rdp_redirections;
    ALTER TABLE assignments DROP COLUMN sound_card_add;ALTER TABLE assignments DROP COLUMN clipboard_permissions;ALTER TABLE assignments DROP COLUMN rdp_redirections;ALTER TABLE assignments DROP COLUMN favorite;ALTER TABLE assignments DROP COLUMN last_used_at;ALTER TABLE desktops DROP COLUMN pve_tags;ALTER TABLE desktops DROP COLUMN pve_pool;
    DROP TABLE session_end_reasons;DROP TABLE session_activity;DROP INDEX sessions_windows_owner;DROP INDEX sessions_replaced;ALTER TABLE sessions DROP COLUMN replaced_at;ALTER TABLE sessions DROP COLUMN issuer;PRAGMA user_version=7;COMMIT;`);
  if(invalid){db.exec('PRAGMA ignore_check_constraints=ON;');db.prepare("UPDATE power_tasks SET action='invalid-fixture' WHERE id='old-finished'").run();}
  db.close();return {path,owner,expected};
}
test('Schema 7 to 8 preserves power rows, ordering, guards, indexes, FK cascades and accounts',async t=>{
  const {path,owner,expected}=await legacy(t);const store=new Store(path);
  try {
  assert.equal(store.db.prepare('PRAGMA user_version').get().user_version,19);
  assert.deepEqual(store.db.prepare('SELECT rowid,* FROM power_tasks ORDER BY rowid').all(),expected);
  assert.equal(store.findUser('fixture-admin').id,owner);assert.equal(store.findUser('fixture-admin').password_hash,'retained-password-fixture');
  // This fixture VM never had a Host: the one-time cleanup of the same start
  // (hostless-assignments-cleanup.test.mjs) removes its assignment.
  // host-requests-migration.test.mjs covers an upgrade that keeps a registered Host's assignment.
  assert.deepEqual(store.userAssignments(owner).map(d=>d.desktopId),[]);assert.equal(store.setting('hostless_assignments_cleanup'),'1');
  assert.deepEqual(store.listDesktops(owner),[],'migration never publishes an unregistered Host');assert.equal(store.setting('fixture-preserved'),'retained-setting');
  assert.deepEqual(store.db.prepare('PRAGMA foreign_key_check').all(),[]);
  const indexes=store.db.prepare("SELECT name FROM sqlite_schema WHERE type='index' AND tbl_name='power_tasks'").all().map(i=>i.name);
  assert.ok(indexes.includes('power_tasks_active_vm'));assert.ok(indexes.includes('power_tasks_owner'));
  assert.throws(()=>store.db.prepare("INSERT INTO power_tasks(id,desktop_id,user_id,action,status,submitted_at,guard_until,config_stamp) VALUES('duplicate','pve-710',?,'stop','submitted',2000,32000,'stamp')").run(owner),/UNIQUE/);
  store.db.prepare("UPDATE power_tasks SET active=0,status='superseded' WHERE id='old-active'").run();
  store.db.prepare("INSERT INTO power_tasks(id,desktop_id,user_id,action,status,submitted_at,guard_until,config_stamp) VALUES('new-stop','pve-710',?,'stop','submitted',2000,32000,'stamp')").run(owner);
  assert.throws(()=>store.db.prepare("UPDATE power_tasks SET action='reset' WHERE id='new-stop'").run(),/CHECK/);
  assert.throws(()=>store.db.prepare("INSERT INTO power_tasks(id,desktop_id,user_id,action,status,submitted_at,guard_until,config_stamp) VALUES('missing-vm','missing',?,'stop','submitted',2000,32000,'stamp')").run(owner),/FOREIGN KEY/);
  store.db.prepare("DELETE FROM desktops WHERE id='pve-710'").run();assert.equal(store.db.prepare('SELECT count(*) AS n FROM power_tasks').get().n,0);
  } finally {store.close();}
});
test('An invalid legacy power row rolls back the complete schema 8 migration and closes the failed handle',async t=>{
  const {path}=await legacy(t,{invalid:true});assert.throws(()=>new Store(path),/CHECK/);
  const db=new DatabaseSync(path);
  try {
  assert.equal(db.prepare('PRAGMA user_version').get().user_version,7);
  assert.equal(db.prepare("SELECT action FROM power_tasks WHERE id='old-finished'").get().action,'invalid-fixture');
  assert.equal(db.prepare("SELECT active FROM power_tasks WHERE id='old-active'").get().active,1);
  assert.equal(db.prepare("SELECT count(*) AS n FROM sqlite_schema WHERE name='power_tasks_next'").get().n,0);
  assert.equal(db.prepare("SELECT count(*) AS n FROM sqlite_schema WHERE name IN ('power_tasks_active_vm','power_tasks_owner')").get().n,2);
  } finally {db.close();}
});
