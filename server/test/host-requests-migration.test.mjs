import { test } from 'node:test';
import assert from 'node:assert/strict';
import { DatabaseSync } from 'node:sqlite';
import { mkdtemp,rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join,resolve,dirname,basename } from 'node:path';
import { Store } from '../src/store.mjs';
import { Store as Store18 } from './fixtures/mfa-store18.mjs';

async function oldDatabase(t){
  const directory=await mkdtemp(join(tmpdir(),'multitap-host-requests-'));
  t.after(async()=>{assert.equal(dirname(resolve(directory)),resolve(tmpdir()));assert.ok(basename(directory).startsWith('multitap-host-requests-'));await rm(directory,{recursive:true});});
  const path=join(directory,'fixture.sqlite'),store=new Store18(path);
  const owner=store.addUser({name:'retained-admin',role:'admin',passwordHash:'retained-password-hash',notes:'retained notes'});
  store.syncProxmox([{vmid:710,node:'retained-node',name:'Retained VM',status:'running'}],'fixture');store.assign('retained-admin','pve-710','fixture');
  store.saveSetting('retained-setting','retained-value','fixture');
  store.db.prepare('INSERT INTO hosts(id,desktop_id,token_hash,hostname,machine_uuid,version,created_at) VALUES(?,?,?,?,?,?,?)').run('retained-host','pve-710','retained-token-hash','retained-name','12345678-1234-1234-1234-123456789abc','0.2.10',1000);
  store.db.prepare('INSERT INTO sessions(token_hash,user_id,created_at,expires_at,purpose) VALUES(?,?,?,?,?)').run('retained-session',owner,1000,9999999999999,'client');
  store.db.prepare("INSERT INTO power_tasks(rowid,id,desktop_id,user_id,action,status,submitted_at,guard_until,config_stamp) VALUES(41,'retained-task','pve-710',?,'stop','running',1000,31000,'retained-stamp')").run(owner);
  const expected={};for(const table of ['users','assignments','hosts','sessions','settings','power_tasks'])expected[table]=store.db.prepare('SELECT rowid,* FROM '+table+' ORDER BY rowid').all();
  store.close();const db=new DatabaseSync(path);db.exec('DROP TABLE client_diagnostics;DROP TABLE host_diagnostics;DROP TABLE host_requests;ALTER TABLE desktops DROP COLUMN pve_machine_uuid;ALTER TABLE desktops DROP COLUMN pve_machine_checked_at;ALTER TABLE users DROP COLUMN clipboard_permissions;ALTER TABLE users DROP COLUMN rdp_redirections;ALTER TABLE assignments DROP COLUMN sound_card_add;ALTER TABLE assignments DROP COLUMN clipboard_permissions;ALTER TABLE assignments DROP COLUMN rdp_redirections;ALTER TABLE assignments DROP COLUMN favorite;ALTER TABLE assignments DROP COLUMN last_used_at;ALTER TABLE desktops DROP COLUMN pve_tags;ALTER TABLE desktops DROP COLUMN pve_pool;DROP TABLE session_end_reasons;DROP TABLE session_activity;DROP INDEX sessions_windows_owner;DROP INDEX sessions_replaced;ALTER TABLE sessions DROP COLUMN replaced_at;ALTER TABLE sessions DROP COLUMN issuer;PRAGMA user_version=8;');
  expected.users=db.prepare('SELECT rowid,* FROM users ORDER BY rowid').all();
  expected.desktops=db.prepare('SELECT rowid,* FROM desktops ORDER BY rowid').all();db.close();return{path,expected};
}
test('Schema 8→9 preserves existing accounts, assignments, hosts, sessions, settings and power work, adding only request state and nullable UUID cache',async t=>{
  const {path,expected}=await oldDatabase(t),store=new Store(path);
  try{
    assert.equal(store.db.prepare('PRAGMA user_version').get().user_version,19);
    // The registered Host keeps its assignment through the upgrade; the only new
    // setting is the mark of the one-time cleanup, which removed nothing.
    for(const table of ['assignments','hosts','sessions','settings','power_tasks'])assert.deepEqual(store.db.prepare('SELECT rowid,* FROM '+table+(table==='settings'?" WHERE key NOT IN ('hostless_assignments_cleanup','usage_backfill')":'')+' ORDER BY rowid').all(),expected[table]);
    assert.equal(store.setting('hostless_assignments_cleanup'),'0');assert.equal(store.db.prepare("SELECT count(*) AS n FROM audit_events WHERE event='hostless_assignments_removed'").get().n,0);
    assert.deepEqual(store.db.prepare('SELECT rowid,* FROM users ORDER BY rowid').all().map(({clipboard_permissions,rdp_redirections,authorization_revision,...row})=>{assert.equal(clipboard_permissions,0);assert.equal(rdp_redirections,0);assert.equal(authorization_revision,1);return row;}),expected.users.map(row=>({...row})));
    assert.deepEqual(store.db.prepare('SELECT rowid,* FROM desktops ORDER BY rowid').all().map(({pve_machine_uuid,pve_machine_checked_at,pve_tags,pve_pool,...rest})=>{assert.equal(pve_machine_uuid,null);assert.equal(pve_machine_checked_at,null);return rest;}),expected.desktops.map(row=>({...row})));
    assert.equal(store.db.prepare('SELECT count(*) AS n FROM host_requests').get().n,0);assert.deepEqual(store.db.prepare('PRAGMA foreign_key_check').all(),[]);
    assert.ok(store.db.prepare("SELECT name FROM sqlite_schema WHERE type='index' AND name='power_tasks_active_vm'").get());
  }finally{store.close();}
});
test('Schema 9 failure rolls back both new VM columns and tables, keeps schema 8 data and closes the failed connection',async t=>{
  const {path,expected}=await oldDatabase(t);let db=new DatabaseSync(path);db.exec('CREATE TABLE host_requests(unrelated_fixture TEXT);');db.close();
  assert.throws(()=>new Store(path),/already exists/);db=new DatabaseSync(path);
  try{
    assert.equal(db.prepare('PRAGMA user_version').get().user_version,8);assert.equal(db.prepare('PRAGMA table_info(desktops)').all().filter(value=>value.name.startsWith('pve_machine_')).length,0);
    assert.deepEqual(db.prepare('SELECT rowid,* FROM users ORDER BY rowid').all(),expected.users);assert.deepEqual(db.prepare('SELECT rowid,* FROM hosts ORDER BY rowid').all(),expected.hosts);
    assert.deepEqual(db.prepare('PRAGMA table_info(host_requests)').all().map(value=>value.name),['unrelated_fixture']);
    db.exec('BEGIN IMMEDIATE;ROLLBACK;');
  }finally{db.close();}
});
