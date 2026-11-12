import { captureAssignmentInput, enrollFixtureHosts } from './fixtures/admin-assignment-input.mjs';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer, VERSION } from '../src/http.mjs';
import { hashPassword, initialPasswordHash, newToken, tokenHash } from '../src/security.mjs';
import { validateProxmox } from '../src/proxmox.mjs';

const origin = 'https://admin-tests.example';
const password = 'Admin-tests-only-password-71!';
async function setup(t, options = {}) {
  const store = new Store(':memory:'); const vault = new Vault(randomBytes(32));
  const hash = await hashPassword(password);
  store.addUser({ name: 'admin', displayName: 'Admin', passwordHash: hash, role: 'admin' });
  store.addUser({ name: 'alice', displayName: 'Alice', passwordHash: hash });
  // Account protocol fixtures represent a previously commissioned installation.
  store.saveSetting('proxmox_initialized', '1', 'protocol-test-fixture');
  const server = await buildServer({ store, vault, publicOrigin: origin, logger: { error: value => assert.fail(value) }, ...options });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  let cookie = '', csrf = '';
  async function call(path, method = 'GET', data, headers = {}) {
    data = captureAssignmentInput(store, path, data);
    // Capture this fixture client's intended immutable account before sending.
    const account = /^\/api\/v1\/admin\/users\/([^/]+)(?:\/password|\/enabled)?$/.exec(path);
    if (account && ['PUT', 'DELETE'].includes(method) && !Object.hasOwn(data ?? {}, 'expectedAccountId')) {
      let target; try { target = store.findUser(decodeURIComponent(account[1])); } catch { }
      if (target) data = { ...data, expectedAccountId: target.id };
    }
    if (path.startsWith('/api/v1/admin/') && !path.endsWith('/login') && data && Object.hasOwn(data, 'password') && !Object.hasOwn(data, 'passwordConfirmation')) data = { ...data, passwordConfirmation: data.password };
    const response = await fetch(`http://127.0.0.1:${server.address().port}` + path, { method, headers: { Cookie: cookie, ...(data !== undefined ? { 'Content-Type': 'application/json' } : {}), ...(method !== 'GET' ? { Origin: origin, 'X-CSRF-Token': csrf } : {}), ...headers }, body: data === undefined ? undefined : JSON.stringify(data) });
    return { status: response.status, headers: response.headers, body: await response.json() };
  }
  async function login(secret = password, name = 'admin') {
    const result = await call('/api/v1/admin/login', 'POST', { username: name, password: secret });
    assert.equal(result.status, 200); cookie = result.headers.get('set-cookie').split(';')[0]; csrf = result.body.csrfToken; return result;
  }
  return { store, vault, server, call, login };
}

test('Admin cookies are isolated from bearer sessions, role-gated, same-origin and CSRF protected', async t => {
  const f = await setup(t);
  assert.equal((await f.call('/api/v1/admin/inventory')).status, 401);
  assert.equal((await f.call('/api/v1/admin/login', 'POST', { username: 'alice', password })).status, 401);
  const client = await f.call('/api/v1/auth/login', 'POST', { username: 'admin', password });
  assert.equal((await f.call('/api/v1/admin/inventory', 'GET', undefined, { Authorization: 'Bearer ' + client.body.sessionToken })).status, 401);
  assert.equal((await f.call('/api/v1/admin/login', 'POST', { username: 'admin', password }, { Origin: '' })).status, 403);
  const login = await f.login();
  assert.match(login.headers.get('set-cookie'), /HttpOnly; Secure; SameSite=Strict/);
  assert.equal('sessionToken' in login.body, false);
  assert.equal((await f.call('/api/v1/auth/me', 'GET', undefined, { Authorization: 'Bearer ' + login.headers.get('set-cookie').split(/[=;]/)[1] })).status, 401);
  assert.equal((await f.call('/api/v1/admin/users', 'POST', {}, { 'X-CSRF-Token': '' })).status, 403);
  assert.equal((await f.call('/api/v1/admin/users', 'POST', {}, { Origin: 'https://evil.example' })).status, 403);
  assert.equal((await f.call('/api/v1/admin/logout', 'POST')).status, 200);
  assert.equal((await f.call('/api/v1/admin/inventory')).status, 401);
});

test('server update information reports actual package/config values and unconfigured apply stays unavailable',async t=>{
  const f=await setup(t,{deploymentKind:'docker'});
  assert.equal((await f.call('/api/v1/admin/server-info')).status,401);await f.login();
  const result=await f.call('/api/v1/admin/server-info');assert.equal(result.status,200);
  assert.equal(result.body.service,'multitap-broker');assert.equal(result.body.version,VERSION);
  assert.deepEqual(result.body.deployment,{kind:'docker',configured:true});
  assert.deepEqual(result.body.update,{state:'not_configured',feedConfigured:false,applySupported:false});
  assert.ok(Number.isFinite(Date.parse(result.body.checkedAt)));
  assert.equal(JSON.stringify(result.body).includes('vault'),false);
  assert.equal((await f.call('/api/v1/admin/server-info','POST',{})).status,405);
});

test('server updates require an administrator cookie, same origin, CSRF, commissioning and exact confirmation',async t=>{
  const calls=[],releaseId='a'.repeat(64),serverUpdate={status:async()=>({state:'available',feedConfigured:true,applySupported:true,latest:{version:'9.0.0'},releaseId}),check:async()=>{calls.push('check');return {accepted:true};},apply:async input=>{calls.push(input);return {accepted:true,operationId:'fixture'};}};
  const f=await setup(t,{serverUpdate});
  assert.equal((await f.call('/api/v1/admin/server-update/check','POST',{})).status,401);await f.login();
  assert.equal((await f.call('/api/v1/admin/server-update/check','POST',{}, {'X-CSRF-Token':''})).status,403);
  assert.equal((await f.call('/api/v1/admin/server-update/apply','POST',{releaseId,fromVersion:VERSION,confirm:true},{Origin:'https://foreign.example'})).status,403);
  assert.equal((await f.call('/api/v1/admin/server-update/check','POST',{command:'systemctl stop anything'})).status,400);
  assert.equal((await f.call('/api/v1/admin/server-update/check','POST',{})).status,202);
  for(const input of [{releaseId,fromVersion:VERSION},{releaseId,fromVersion:'0.0.0',confirm:true},{releaseId,fromVersion:VERSION,confirm:true,url:'https://foreign.example/a'}])assert.equal((await f.call('/api/v1/admin/server-update/apply','POST',input)).status,400);
  assert.equal((await f.call('/api/v1/admin/server-update/apply','POST',{releaseId,fromVersion:VERSION,confirm:true})).status,202);assert.deepEqual(calls,['check',{releaseId,fromVersion:VERSION}]);
  assert.equal((await f.call('/api/v1/admin/server-info')).body.update.applySupported,true);assert.ok(f.store.auditLog().some(v=>v.event==='server_update_requested'));
});
test('an admin session revoked while its update request body is awaited cannot enqueue a privileged action',async t=>{
  let queued=0;const f=await setup(t,{serverUpdate:{status:async()=>({}),check:async()=>{queued++;return {accepted:true};}}});const login=await f.login(),token=login.headers.get('set-cookie').split(/[=;]/)[1];
  const original=f.store.authenticate.bind(f.store);let revoke=true;
  f.store.authenticate=(...args)=>{const user=original(...args);if(revoke&&user){revoke=false;f.store.logout(tokenHash(token));}return user;};
  assert.equal((await f.call('/api/v1/admin/server-update/check','POST',{})).status,401);assert.equal(queued,0);
});

test('only an authenticated CSRF-protected admin can set exact directional clipboard permissions per assigned VM without changing account identity or credentials',async t=>{
  const f=await setup(t),deny={textToRemote:false,textToLocal:false,filesToRemote:false,filesToLocal:false};await f.login();
  f.store.syncProxmox([710,711].map(vmid=>({vmid,node:'fixture',name:'Policy VM '+vmid,status:'running'})),'fixture');f.store.assign('alice','pve-710');f.store.assign('alice','pve-711');
  enrollFixtureHosts(f.store,['pve-710','pve-711']);
  const before={...f.store.findUser('alice')};
  const get=await f.call('/api/v1/admin/users/alice');assert.deepEqual(get.body.user.assignments.map(a=>a.clipboardPolicy),[deny,deny]);
  assert.equal('password_hash' in get.body.user,false);assert.equal('clipboard_permissions' in get.body.user,false);assert.equal('clipboardPolicy' in get.body.user,false);
  const allowed={...deny,textToRemote:true,filesToLocal:true};
  const batch=data=>f.call('/api/v1/admin/assignments/batch','PUT',{username:'alice',...data});
  for(const headers of [{'X-CSRF-Token':''},{Origin:'https://other.example'}])assert.equal((await f.call('/api/v1/admin/assignments/batch','PUT',{username:'alice',assignments:[{desktopId:'pve-710',clipboardPolicy:allowed},{desktopId:'pve-711'}]},headers)).status,403);
  for(const policy of [null,{},[],true,{textToRemote:true},{...deny,textToRemote:'true'},{...deny,extra:false}]){
    const result=await batch({assignments:[{desktopId:'pve-710',clipboardPolicy:policy},{desktopId:'pve-711'}]});assert.equal(result.status,400);assert.equal(result.body.error.code,'invalid_clipboard_policy');
    const legacy=await f.call('/api/v1/admin/users/alice','PUT',{clipboardPolicy:policy});assert.equal(legacy.status,400);assert.equal(legacy.body.error.code,'invalid_clipboard_policy');
  }
  assert.equal((await batch({assignments:[{desktopId:'pve-710',clipboardPolicy:allowed},{desktopId:'pve-711'}]})).status,200);
  const after={...f.store.findUser('alice')};assert.deepEqual(after,before);
  assert.deepEqual((await f.call('/api/v1/admin/users/alice')).body.user.assignments,[{desktopId:'pve-710',clipboardPolicy:allowed,devicePolicy:Object.fromEntries(['drives','printers','usbDevices','smartCards','serialPorts'].map(key=>[key,false])),soundCardAdd:false},{desktopId:'pve-711',clipboardPolicy:deny,devicePolicy:Object.fromEntries(['drives','printers','usbDevices','smartCards','serialPorts'].map(key=>[key,false])),soundCardAdd:false}]);
  assert.deepEqual((await f.call('/api/v1/admin/inventory')).body.assignments.filter(a=>a.username==='alice').map(a=>[a.desktop_id,a.clipboardPolicy]),[['pve-710',allowed],['pve-711',deny]]);
  assert.equal('clipboardPolicy' in (await f.call('/api/v1/admin/users')).body.users.find(u=>u.username==='alice'),false);
  // Compatibility: the account-wide field now writes every current assignment.
  assert.equal((await f.call('/api/v1/admin/users/alice','PUT',{clipboardPolicy:{...deny,textToLocal:true}})).status,200);
  assert.deepEqual(['pve-710','pve-711'].map(id=>f.store.clipboardPolicy(before.id,id)),[{...deny,textToLocal:true},{...deny,textToLocal:true}]);
  assert.equal((await f.call('/api/v1/admin/users','POST',{username:'policy-user',password,clipboardPolicy:allowed})).status,201);
  f.store.assign('policy-user','pve-710');assert.deepEqual(f.store.clipboardPolicy(f.store.findUser('policy-user').id,'pve-710'),deny,'a new assignment starts denied');
  assert.equal((await f.call('/api/v1/admin/users','POST',{username:'default-deny',password})).status,201);
  assert.deepEqual(f.store.clipboardPolicy(f.store.findUser('default-deny').id,'pve-710'),deny);
  assert.ok(f.store.auditLog().some(event=>event.event==='clipboard_policy_changed'&&event.subject===before.id+':pve-710'));
});

test('an actual admin policy revocation stops paired clipboard and video sockets before reporting the saved change',async t=>{
  const f=await setup(t);await f.login();
  const policy={textToRemote:true,textToLocal:true,filesToRemote:true,filesToLocal:true};
  f.store.syncProxmox([{vmid:710,node:'fixture',name:'Policy VM',status:'running'}],'fixture');f.store.assign('alice','pve-710');
  f.store.updateUser('alice',{clipboardPolicy:policy},'fixture');
  const code=newToken(),hostToken=newToken(),uuid='12345678-1234-1234-1234-123456789abc';
  f.store.issueEnrollment('pve-710',uuid,tokenHash(code),'fixture');f.store.consumeEnrollment(tokenHash(code),uuid,{hostname:'Policy fixture',version:'0.2.16',tokenHash:tokenHash(hostToken)});
  const clientToken=(await f.call('/api/v1/auth/login','POST',{username:'alice',password})).body.sessionToken;
  async function peer(path,authorization){
    const ws=new WebSocket(`ws://127.0.0.1:${f.server.address().port}`+path,{headers:{Authorization:authorization}}),queue=[],waiting=[];
    ws.on('error',()=>{});
    ws.on('message',bytes=>{const value=JSON.parse(bytes.toString()),index=waiting.findIndex(w=>w.test(value));if(index<0)queue.push(value);else{const w=waiting.splice(index,1)[0];clearTimeout(w.timer);w.resolve(value);}});
    await new Promise((resolve,reject)=>{ws.once('open',resolve);ws.once('error',reject);});
    return{json:value=>ws.send(JSON.stringify(value)),take(test){const index=queue.findIndex(test);if(index>=0)return Promise.resolve(queue.splice(index,1)[0]);return new Promise((resolve,reject)=>{const w={test,resolve,timer:setTimeout(()=>reject(Error('policy_socket_timeout')),1500)};waiting.push(w);});}};
  }
  const host=await peer('/api/v1/hosts/channel','Host '+hostToken);
  host.json({type:'host.capabilities',capabilities:{service:true,interactiveSession:true,streaming:true,virtualDisplayReady:true,clipboard:true,clipboardSync:true}});
  assert.equal((await host.take(v=>v.type==='host.capabilities.ack')).accepted,true);
  const connected=await f.call('/api/v1/desktops/pve-710/connect','POST',{clipboardProtocol:'clipboard-sync-v1'}, {Authorization:'Bearer '+clientToken});assert.equal(connected.status,200);assert.equal(connected.body.clipboardProtocol,'clipboard-sync-v1');assert.equal(connected.body.clipboardSync,true);
  const connection=connected.body,media=await peer(new URL(connection.webSocketUrl).pathname,'Bearer '+clientToken),start=await host.take(v=>v.type==='session.start');
  const local=await peer(new URL(connection.clipboardWebSocketUrl).pathname,'Bearer '+clientToken),remote=await peer(new URL(start.clipboardUrl).pathname,'Host '+hostToken);
  await local.take(v=>v.type==='clipboard.ready');await remote.take(v=>v.type==='clipboard.ready');
  local.json({type:'clipboard.begin',requestId:'a'.repeat(32)+'-0000000000000001',format:'text',size:3});await remote.take(v=>v.type==='clipboard.begin');
  const saved=await f.call('/api/v1/admin/users/alice','PUT',{clipboardPolicy:{...policy,textToRemote:false}});
  assert.equal(saved.status,200);
  // Only the clipboard ends; the stream stays.
  assert.equal((await remote.take(v=>v.type==='clipboard.error')).reason,'clipboard_policy_changed');
  await assert.rejects(media.take(v=>v.type==='session.ended'),/policy_socket_timeout/);
  // No new clipboard pair under the old, wider grant.
  const again=await new Promise(resolve=>{const ws=new WebSocket(`ws://127.0.0.1:${f.server.address().port}`+new URL(connection.clipboardWebSocketUrl).pathname,{headers:{Authorization:'Bearer '+clientToken}});ws.on('error',()=>resolve('refused'));ws.on('unexpected-response',()=>resolve('refused'));ws.on('message',bytes=>{const value=JSON.parse(bytes.toString());if(value.type==='clipboard.ready')resolve('ready');});ws.on('close',()=>resolve('closed'));});
  assert.notEqual(again,'ready');
});

test('Initial admin/admin can only change both credentials; completing setup revokes every old session', async t => {
  const announced = [];
  const f = await setup(t, { initialSetupOptions: { announce: code => announced.push(code) } });
  f.store.db.prepare('UPDATE users SET password_hash=?,must_change_credentials=1 WHERE username=?').run(await initialPasswordHash(), 'admin');
  const login = await f.login('admin'); assert.equal(login.body.user.mustChangeCredentials, true);
  assert.equal((await f.call('/api/v1/admin/session')).body.user.mustChangeCredentials, true);
  assert.equal((await f.call('/api/v1/admin/inventory')).status, 428);
  assert.equal((await f.call('/api/v1/admin/desktops', 'POST', { id: 'bypass', name: 'Bypass' })).status, 428);
  assert.equal((await f.call('/api/v1/auth/login', 'POST', { username: 'admin', password: 'admin' })).status, 428);
  // Setup became pending after startup: the first attempt issues the one-time code to the server log.
  assert.equal((await f.call('/api/v1/admin/setup', 'POST', { username: 'owner', password: 'Replacement-password-71!' })).body.error.code, 'setup_code_required');
  assert.equal(announced.length, 1); const setupCode = announced[0];
  assert.equal((await f.call('/api/v1/admin/setup', 'POST', { username: 'admin', password: 'Replacement-password-71!', setupCode })).status, 409);
  assert.equal((await f.call('/api/v1/admin/setup', 'POST', { username: 'owner', password: 'short', setupCode })).status, 400);
  assert.equal((await f.call('/api/v1/admin/setup', 'POST', { username: 'alice', password: 'Replacement-password-71!', setupCode })).status, 409);
  assert.equal((await f.call('/api/v1/admin/setup', 'POST', { username: 'owner', password: 'Replacement-password-71!', setupCode })).status, 200);
  assert.equal((await f.call('/api/v1/admin/session')).status, 401);
  assert.equal((await f.call('/api/v1/admin/login', 'POST', { username: 'admin', password: 'admin' })).status, 401);
  const changed = await f.login('Replacement-password-71!', 'owner'); assert.equal(changed.body.user.mustChangeCredentials, false);
  assert.equal((await f.call('/api/v1/admin/inventory')).status, 200);
  assert.equal((await f.call('/api/v1/admin/setup', 'POST', { username: 'another', password: 'Replacement-password-71!' })).status, 409);
});

test('Admin account/desktop/assignment CRUD persists and changes existing client permissions', async t => {
  const f = await setup(t); await f.login();
  const client = (await f.call('/api/v1/auth/login', 'POST', { username: 'alice', password })).body.sessionToken;
  const list = () => f.call('/api/v1/desktops', 'GET', undefined, { Authorization: 'Bearer ' + client });
  assert.equal((await f.call('/api/v1/admin/users', 'POST', { username: 'charlie', displayName: 'Charlie', password, role: 'user' })).status, 201);
  for (const kind of ['pc', 'vm']) assert.equal((await f.call('/api/v1/admin/desktops', 'POST', { id: 'vm-one', name: 'VM One', kind })).status, 405);
  // Seed an imported VM record locally; this does not claim a real Proxmox import.
  f.store.addDesktop({ id: 'vm-one', name: 'VM One', kind: 'vm', vmid: 710 });
  enrollFixtureHosts(f.store, ['vm-one']);
  f.store.addDesktop({ id: 'legacy-pc', name: 'Legacy PC', kind: 'pc' });
  assert.equal((await f.call('/api/v1/admin/assignments', 'POST', { username: 'alice', desktopId: 'legacy-pc' })).status, 404);
  assert.equal((await f.call('/api/v1/admin/inventory')).body.desktops.some(d => d.kind === 'pc'), false);
  assert.equal((await f.call('/api/v1/admin/assignments', 'POST', { username: 'alice', desktopId: 'vm-one' })).status, 200);
  assert.deepEqual((await list()).body.desktops.map(d => d.id), ['vm-one']);
  assert.equal((await f.call('/api/v1/admin/desktops/vm-one', 'PUT', { name: 'Changed VM' })).status, 200);
  assert.equal((await list()).body.desktops[0].name, 'Changed VM');
  assert.equal((await f.call('/api/v1/admin/assignments', 'DELETE', { username: 'alice', desktopId: 'vm-one' })).status, 200);
  assert.deepEqual((await list()).body.desktops, []);
  await f.call('/api/v1/admin/assignments', 'POST', { username: 'alice', desktopId: 'vm-one' });
  assert.equal((await f.call('/api/v1/admin/desktops/vm-one', 'DELETE')).status, 405);
  assert.equal((await f.call('/api/v1/admin/desktops/vm-one/enabled', 'PUT', { enabled: false })).status, 405);
  assert.equal((await list()).body.desktops.length, 1);
  await f.call('/api/v1/admin/assignments', 'DELETE', { username: 'alice', desktopId: 'vm-one' });
  assert.deepEqual((await list()).body.desktops, []);
  assert.equal((await f.call('/api/v1/admin/users/charlie/password', 'PUT', { password: 'Replacement-password-189!' })).status, 200);
  assert.equal((await f.call('/api/v1/auth/login', 'POST', { username: 'charlie', password })).status, 401);
  assert.equal((await f.call('/api/v1/admin/users/charlie', 'DELETE')).status, 200);
  const inventory = (await f.call('/api/v1/admin/inventory')).body;
  assert.equal(JSON.stringify(inventory).includes('password_hash'), false);
  assert.ok(inventory.audit.some(e => e.event === 'assignment_revoked' && e.actor === f.store.findUser('admin').id));
});

test('Last admin, self deletion, malformed input and password-reset invalidation are enforced', async t => {
  const f = await setup(t); await f.login();
  assert.equal((await f.call('/api/v1/admin/users/admin/enabled', 'PUT', { enabled: false })).status, 409);
  assert.equal((await f.call('/api/v1/admin/users/admin', 'PUT', { displayName: 'Admin', role: 'user' })).status, 409);
  assert.equal((await f.call('/api/v1/admin/users/admin', 'DELETE')).status, 409);
  assert.equal((await f.call('/api/v1/admin/users/alice/enabled', 'PUT', { enabled: 'false' })).status, 400);
  assert.equal((await f.call('/api/v1/admin/users/alice/password', 'PUT', { password: 'short' })).status, 400);
  assert.equal((await f.call('/api/v1/admin/desktops', 'POST', { id: 'bad', name: 'Bad', vmid: 999 })).status, 405);
  assert.equal((await f.call('/api/v1/admin/users/admin/password', 'PUT', { password: 'Replacement-password-189!' })).status, 200);
  assert.equal((await f.call('/api/v1/admin/session')).status, 401);
});

test('The renamed primary administrator cannot be demoted, disabled or deleted even by another administrator', async t => {
  const f = await setup(t);
  const ownerId = f.store.findUser('admin').id;
  f.store.db.prepare('UPDATE users SET must_change_credentials=1 WHERE id=?').run(ownerId);
  f.store.completeInitialSetup(ownerId, '총관리자', await hashPassword(password));
  f.store.addUser({ name: 'second-admin', passwordHash: await hashPassword(password), role: 'admin' }, ownerId);
  const login = await f.login(password, 'second-admin'); assert.equal(login.body.user.isPrimaryAdmin, false);
  const path = '/api/v1/admin/users/' + encodeURIComponent('총관리자');
  for (const [suffix, method, data] of [['', 'PUT', { notes: '', role: 'user' }], ['/enabled', 'PUT', { enabled: false }], ['', 'DELETE', undefined]]) {
    const result = await f.call(path + suffix, method, data);
    assert.equal(result.status, 409); assert.equal(result.body.error.code, 'primary_admin_required');
  }
  assert.equal((await f.call(path, 'PUT', { notes: '운영 책임자' })).status, 200);
  const owner = f.store.findUser('총관리자');
  assert.equal(owner.id, ownerId); assert.equal(owner.is_primary_admin, 1); assert.equal(owner.role, 'admin'); assert.equal(owner.disabled, 0);
  assert.equal(owner.notes, '운영 책임자');
  assert.equal((await f.login(password, '총관리자')).body.user.isPrimaryAdmin, true);
  assert.equal((await f.call('/api/v1/admin/users/second-admin', 'PUT', { notes: '일반 관리자', role: 'user' })).status, 200);
});

test('Korean account ids authenticate and support encoded-path CRUD with optional notes rather than display names', async t => {
  const f = await setup(t); await f.login();
  const name = '테스트계정'; const path = '/api/v1/admin/users/' + encodeURIComponent(name);
  assert.equal((await f.call('/api/v1/admin/users', 'POST', { username: name.normalize('NFD'), password, role: 'user', notes: '회계팀\n담당자' })).status, 201);
  const client = await f.call('/api/v1/auth/login', 'POST', { username: name.normalize('NFD'), password });
  assert.equal(client.status, 200); assert.equal(client.body.user.username, name); assert.equal(client.body.user.displayName, name);
  const account = (await f.call('/api/v1/admin/inventory')).body.users.find(u => u.username === name);
  assert.equal(account.notes, '회계팀\n담당자'); assert.equal(account.display_name, name);
  assert.equal(account.is_primary_admin, 0);
  assert.equal((await f.call(path, 'PUT', { role: 'user', notes: '' })).status, 200);
  assert.equal(f.store.findUser(name).notes, '');
  assert.equal((await f.call(path, 'PUT', { role: 'user', notes: 'x'.repeat(501) })).status, 400);
  assert.equal((await f.call(path, 'PUT', { role: 'user', notes: null })).status, 400);
  assert.equal((await f.call('/api/v1/admin/users/%ZZ', 'DELETE')).status, 400);
  assert.equal((await f.call(path + '/password', 'PUT', { password: 'Korean-reset-password-71!' })).status, 200);
  assert.equal((await f.call('/api/v1/auth/me', 'GET', undefined, { Authorization: 'Bearer ' + client.body.sessionToken })).status, 401);
  assert.equal((await f.call(path + '/enabled', 'PUT', { enabled: false })).status, 200);
  assert.equal((await f.call('/api/v1/auth/login', 'POST', { username: name, password: 'Korean-reset-password-71!' })).status, 401);
  assert.equal((await f.call(path, 'DELETE')).status, 200);
  assert.equal(f.store.findUser(name), undefined);
});

test('A missing or mismatched password confirmation cannot change credentials or revoke current sessions', async t => {
  const f = await setup(t); await f.login();
  const token = (await f.call('/api/v1/auth/login', 'POST', { username: 'alice', password })).body.sessionToken;
  const oldHash = f.store.findUser('alice').password_hash;
  for (const confirmation of [null, 'mismatch']) {
    const response = await f.call('/api/v1/admin/users/alice/password', 'PUT', { password: 'New-password-test-71!', passwordConfirmation: confirmation });
    assert.equal(response.status, 400); assert.equal(response.body.error.code, 'password_confirmation_mismatch');
    assert.equal(f.store.findUser('alice').password_hash, oldHash);
    assert.equal((await f.call('/api/v1/auth/me', 'GET', undefined, { Authorization: 'Bearer ' + token })).status, 200);
  }
});

test('Proxmox orchestration uses encrypted credentials, synchronizes all returned VMs and reports task outcomes', async t => {
  const requests = []; const secret = 'test-token-secret-not-production';
  // This is an adapter fixture, not evidence of a real Proxmox deployment or VM power test.
  const adapter = {
    call: async () => ({}),
    inventory: async (cfg, { targets }) => { assert.equal(cfg.secret, secret); return targets.map(target => ({ vmid: target.vmid, node: target.node, machineUuid: target.expectedUuid, name: 'Windows Test', status: 'stopped' })); },
    power: async (cfg, vmid, action) => { requests.push({ vmid, action }); return { task: 'UPID:pve-a:0001:0002:0003:qmstart:710:user@pve:', node: 'pve-a', status: 'submitted' }; },
    task: async () => ({ status: 'stopped', exitStatus: 'OK' }),
  };
  const f = await setup(t, { proxmox: adapter }); await f.login();
  assert.equal((await f.call('/api/v1/admin/proxmox/vms')).status, 409);
  const saved = await f.call('/api/v1/admin/proxmox', 'PUT', { endpoint: 'https://pve.example:8006', tokenId: 'vdi@pve!broker', secret, ca: '' });
  assert.equal(saved.status, 200); assert.equal(JSON.stringify(saved.body).includes(secret), false);
  assert.equal(f.store.setting('proxmox').includes(secret), false);
  assert.equal(f.vault.open(f.store.setting('proxmox')).secret, secret);
  assert.equal((await f.call('/api/v1/admin/proxmox', 'PUT', { endpoint: 'https://other.example:8006', tokenId: 'vdi@pve!broker' })).status, 400);
  assert.equal((await f.call('/api/v1/admin/proxmox/import', 'POST', { vmid: 711 })).status, 405);
  f.store.syncProxmox([{ vmid: 710, node: 'pve-a', name: 'Windows Test', status: 'stopped' }], 'fixture-seed-only');
  enrollFixtureHosts(f.store, ['pve-710']);
  assert.equal((await f.call('/api/v1/admin/proxmox/sync', 'POST', {})).status, 200);
  assert.equal((await f.call('/api/v1/admin/proxmox/sync', 'POST', {})).body.added, 0);
  assert.equal((await f.call('/api/v1/admin/proxmox', 'PUT', { endpoint: 'https://other.example:8006', tokenId: 'vdi@pve!broker', secret })).status, 409);
  const result = await f.call('/api/v1/admin/desktops/pve-710/power', 'POST', { action: 'start' });
  assert.equal(result.status, 202); assert.equal(result.body.status, 'submitted');
  assert.deepEqual(requests, [{ vmid: 710, action: 'start' }]);
  assert.equal((await f.call('/api/v1/admin/desktops/pve-710/power', 'POST', { action: 'stop' })).body.error.code, 'power_in_progress');
  assert.equal((await f.call('/api/v1/admin/proxmox/task', 'POST', { node: result.body.node, task: result.body.task })).body.exitStatus, 'OK');
});

test('Vault detects modification and Proxmox settings reject credential-bearing URLs and malformed certificates', () => {
  const vault = new Vault(randomBytes(32)); const value = vault.seal({ secret: 'fixture-secret' });
  assert.equal(vault.open(value).secret, 'fixture-secret');
  const altered = Buffer.from(value, 'base64'); altered[29] ^= 1; assert.throws(() => vault.open(altered.toString('base64')));
  for (const endpoint of ['http://pve:8006', 'https://user:pass@pve:8006', 'https://pve:8006/api2/json']) assert.throws(() => validateProxmox({ endpoint, tokenId: 'vdi@pve!broker', secret: 'test-secret' }));
  assert.throws(() => validateProxmox({ endpoint: 'https://pve:8006', tokenId: 'vdi@pve!broker', secret: 'test-secret', ca: 'not a cert' }));
});

test('The Proxmox page data carries the SHA-256 of the exact setup script this address serves, and the script is served as inert text', async t => {
  const f = await setup(t);
  await f.login();
  const inventory = await f.call('/api/v1/admin/inventory');
  assert.equal(inventory.status, 200);
  const response = await fetch(`http://127.0.0.1:${f.server.address().port}/setup/proxmox.py`);
  assert.equal(response.status, 200);
  assert.equal(response.headers.get('x-content-type-options'), 'nosniff');
  assert.equal(response.headers.get('content-security-policy'), "default-src 'none'; sandbox; frame-ancestors 'none'");
  assert.match(response.headers.get('content-type'), /^text\/plain/);
  const bytes = Buffer.from(await response.arrayBuffer());
  assert.equal(Number(response.headers.get('content-length')), bytes.length);
  const { createHash } = await import('node:crypto');
  assert.equal(inventory.body.proxmox.setupScriptSha256, createHash('sha256').update(bytes).digest('hex'));
  assert.match(bytes.toString('utf8'), /^BROKER_ORIGIN = "https:\/\/admin-tests\.example"$/m);
});

test('Only the primary administrator (or this server itself) changes the primary administrator\'s password', async t => {
  const f = await setup(t);
  f.store.addUser({ name: 'deputy', passwordHash: await hashPassword(password), role: 'admin' });
  const primary = f.store.findUser('admin'), before = primary.password_hash;
  assert.equal(primary.is_primary_admin, 1);
  await f.login(password, 'deputy');
  const refused = await f.call('/api/v1/admin/users/admin/password', 'PUT', { password: 'Deputy-chosen-password-93!' });
  assert.equal(refused.status, 403); assert.equal(refused.body.error.code, 'primary_admin_protected');
  assert.equal(f.store.findUser('admin').password_hash, before, 'unchanged');
  // The deputy still resets an ordinary account's password.
  assert.equal((await f.call('/api/v1/admin/users/alice/password', 'PUT', { password: 'Alice-reset-password-17!' })).status, 200);
  // The primary administrator may set its own; the CLI (server-admin) may too; the Store refuses anyone else.
  await f.login(password, 'admin');
  assert.equal((await f.call('/api/v1/admin/users/admin/password', 'PUT', { password: 'Primary-own-password-58!' })).status, 200);
  const deputy = f.store.findUser('deputy');
  assert.throws(() => f.store.changePassword('admin', 'x'.repeat(20), deputy.id), /primary_admin_protected/);
  f.store.changePassword('admin', 'cli-set-hash-only-for-test', 'server-admin');
  assert.equal(f.store.findUser('admin').password_hash, 'cli-set-hash-only-for-test');
});
