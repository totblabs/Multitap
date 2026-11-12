import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { DatabaseSync } from 'node:sqlite';
import { Store } from '../src/store.mjs';
import { Store as Store18 } from './fixtures/mfa-store18.mjs';
import { clipboardPolicyFromMask, clipboardPolicyMask, clipboardTransferAllowed, intersectClipboardPolicy } from '../src/clipboard-policy.mjs';

const deny = { textToRemote:false, textToLocal:false, filesToRemote:false, filesToLocal:false };
test('clipboard policy accepts exactly four boolean permissions and defaults closed for every unknown mask', () => {
  for (let mask=0;mask<16;mask++) assert.equal(clipboardPolicyMask(clipboardPolicyFromMask(mask)),mask);
  for (const value of [null,undefined,[],{},true,{...deny,textToRemote:'true'},{...deny,extra:true},{textToRemote:true}]) assert.throws(()=>clipboardPolicyMask(value),/invalid_clipboard_policy/);
  for (const mask of [undefined,null,-1,16,1.5,'15',NaN]) assert.deepEqual(clipboardPolicyFromMask(mask),deny);
  const toRemote={...deny,textToRemote:true};
  assert.equal(clipboardTransferAllowed(toRemote,'text','client'),true);
  for(const [format,from] of [['files','client'],['text','host'],['files','host'],['unknown','client']])assert.equal(clipboardTransferAllowed(toRemote,format,from),false);
  assert.deepEqual(intersectClipboardPolicy(toRemote,{...toRemote,filesToRemote:true}),toRemote);
});

test('schema9→10 defaults every retained account to deny and preserves identities, hashes, notes, roles, sessions and assignments',async t=>{
  const root=await mkdtemp(join(tmpdir(),'multitap-clipboard-policy-'));
  const path=join(root,'fixture.sqlite');let store=new Store18(path);
  t.after(async()=>{store.close();await rm(root,{recursive:true,force:true});});
  const admin=store.addUser({name:'메인관리자',passwordHash:'retained-admin-hash',notes:'retained admin notes',role:'admin'});
  const member=store.addUser({name:'사용자',passwordHash:'retained-member-hash',notes:'retained member notes'});
  store.syncProxmox([{vmid:710,node:'fixture',name:'retained VM',status:'running'}],'fixture');store.assign('사용자','pve-710');
  // A registered Host: assignments of desktops without one are removed once at the first start of this release (hostless-assignments-cleanup.test.mjs).
  store.db.prepare('INSERT INTO hosts(id,desktop_id,token_hash,hostname,machine_uuid,version,created_at) VALUES(?,?,?,?,?,?,?)').run('retained-host-710','pve-710','retained-token-710','retained','12345678-1234-4234-8234-000000000710','1.0.0',1000);
  store.createSession(store.findUser('사용자'),'retained-session-hash',1000,9999999999999);
  store.close();const previous=new DatabaseSync(path);
  previous.exec('DROP TABLE client_diagnostics;DROP TABLE host_diagnostics;ALTER TABLE users DROP COLUMN clipboard_permissions;ALTER TABLE users DROP COLUMN rdp_redirections;ALTER TABLE assignments DROP COLUMN sound_card_add;ALTER TABLE assignments DROP COLUMN clipboard_permissions;ALTER TABLE assignments DROP COLUMN rdp_redirections;ALTER TABLE assignments DROP COLUMN favorite;ALTER TABLE assignments DROP COLUMN last_used_at;ALTER TABLE desktops DROP COLUMN pve_tags;ALTER TABLE desktops DROP COLUMN pve_pool;DROP TABLE session_end_reasons;DROP TABLE session_activity;DROP INDEX sessions_windows_owner;DROP INDEX sessions_replaced;ALTER TABLE sessions DROP COLUMN replaced_at;ALTER TABLE sessions DROP COLUMN issuer;PRAGMA user_version=9;');
  const before=Object.fromEntries(['users','sessions','assignments','desktops','settings'].map(table=>[table,previous.prepare('SELECT rowid,* FROM '+table+' ORDER BY rowid').all().map(row=>({...row}))]));previous.close();
  store=new Store(path);
  assert.equal(store.db.prepare('PRAGMA user_version').get().user_version,19);
  const users=store.db.prepare('SELECT rowid,* FROM users ORDER BY rowid').all().map(({clipboard_permissions,rdp_redirections,favorite,last_used_at,sound_card_add,authorization_revision,...row})=>{assert.equal(clipboard_permissions,0);assert.equal(rdp_redirections,0);assert.equal(authorization_revision,1);return row;});
  assert.deepEqual(users,before.users);
  assert.deepEqual(store.db.prepare('SELECT rowid,* FROM assignments ORDER BY rowid').all().map(({clipboard_permissions,rdp_redirections,favorite,last_used_at,sound_card_add,...row})=>{assert.equal(clipboard_permissions,0);assert.equal(rdp_redirections,0);return row;}),before.assignments);
  assert.equal(store.setting('hostless_assignments_cleanup'),'0');
  for(const table of ['sessions','desktops','settings'])assert.deepEqual(store.db.prepare('SELECT rowid,* FROM '+table+(table==='settings'?" WHERE key NOT IN ('hostless_assignments_cleanup','usage_backfill')":'')+' ORDER BY rowid').all().map(({pve_tags,pve_pool,issuer,replaced_at,...row})=>({...row})),before[table]);
  for(const id of [admin,member])assert.deepEqual(store.clipboardPolicy(id,'pve-710'),deny);
  // The account-wide compatibility field applies to the account's current assignments only.
  const account=store.findUser('사용자');store.updateUser('사용자',{clipboardPolicy:{...deny,textToRemote:true}},admin);
  assert.deepEqual({...store.findUser('사용자')},{...account},'the account row (identity, hash, notes, legacy masks) is untouched');
  assert.deepEqual(store.clipboardPolicy(member,'pve-710'),{...deny,textToRemote:true});
  assert.deepEqual(store.clipboardPolicy(member,'pve-711'),deny,'a VM without an assignment never carries a grant');
  assert.deepEqual(store.clipboardPolicy(member),deny,'a check without a VM never carries a grant');
  assert.equal(store.db.prepare('SELECT count(*) AS n FROM sessions').get().n,1,'a policy grant must not revoke account login');
  store.assign('메인관리자','pve-710',admin);
  assert.throws(()=>store.updateUser('메인관리자',{role:'user',clipboardPolicy:{...deny,textToLocal:true}},admin),/primary_admin_required/);
  assert.deepEqual(store.clipboardPolicy(admin,'pve-710'),deny,'failed combined updates are atomic');
});
