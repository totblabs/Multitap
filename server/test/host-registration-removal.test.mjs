import test from 'node:test';
import assert from 'node:assert/strict';
import { once } from 'node:events';
import { request } from 'node:http';
import { setImmediate as nextTurn, setTimeout as delay } from 'node:timers/promises';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';

const origin='https://host-removal.fixture.invalid',target='pve-810';
async function fixture(t,{desktopEventOptions}={}){
 const store=new Store(':memory:'),vault=new Vault(Buffer.alloc(32,23)),token=newToken(),hostTokens={},hostIds={},sockets=[];
 const admin=store.addUser({name:'operator',role:'admin',passwordHash:'synthetic'}),user=store.addUser({name:'assigned',passwordHash:'synthetic'});
 store.createSession(store.findUser('operator'),tokenHash(token),Date.now(),60000,'admin');
 store.saveSetting('proxmox',vault.seal({endpoint:'https://fixture.invalid',tokenId:'fixture@pve!fixture',secret:'synthetic'}),'fixture');
 const vms=[810,811,812,813].map(vmid=>({vmid,node:'fixture',name:'VM'+vmid,status:'running'}));store.syncProxmox(vms,'fixture');
 function enroll(id){const code=newToken(),hostToken=newToken(),uuid='12345678-1234-4234-8234-'+id.slice(4).padStart(12,'0');store.issueEnrollment(id,uuid,tokenHash(code),admin);const value=store.consumeEnrollment(tokenHash(code),uuid,{hostname:'synthetic-host',version:'1.0.0',tokenHash:tokenHash(hostToken)});hostTokens[id]=hostToken;hostIds[id]=value.hostId;return value.hostId;}
 for(const id of [target,'pve-811','pve-813'])enroll(id);store.db.prepare("UPDATE hosts SET revoked=1 WHERE desktop_id='pve-813'").run();
 for(const vm of vms)store.assign('assigned','pve-'+vm.vmid,admin);
 const errors=[],server=await buildServer({store,vault,publicOrigin:origin,proxmox:{inventory:async()=>vms},relayOptions:{authorizationMs:60000,heartbeatMs:60000},desktopEventOptions,logger:{error:x=>errors.push(x)}});
 await new Promise(done=>server.listen(0,'127.0.0.1',done));const base='http://127.0.0.1:'+server.address().port;
 t.after(async()=>{for(const ws of sockets)ws.terminate();server.relay.sessions.clear();server.relay.displayBusy.clear();server.relay.console.grants.clear();server.relay.sessionStop.pending.clear();server.relay.hostUpdate.operations.clear();server.relay.vmConfigOperations.clear();await new Promise(done=>server.close(done));store.close();assert.deepEqual(errors,[]);});
 const call=async(path,{method='GET',data,cookie=token,csrf=true,foreign=false}={})=>{const response=await fetch(base+'/api/v1/admin'+path,{method,headers:{Origin:foreign?'https://other.invalid':origin,...(cookie?{Cookie:'__Secure-multitap-admin='+cookie}:{}),...(csrf?{'X-CSRF-Token':vault.csrf(cookie??token)}:{}),...(data===undefined?{}:{'Content-Type':'application/json'})},body:data===undefined?undefined:JSON.stringify(data)});return{status:response.status,body:await response.json()};};
 const input=()=>({expectedHostId:hostIds[target],confirm:true});
 const remove=(data=input(),options={})=>call('/desktops/'+target+'/host',{method:'DELETE',data,...options});
 const snapshot=()=>JSON.stringify(['desktops','hosts','assignments','host_enrollments','host_requests','audit_events'].map(table=>store.db.prepare('SELECT * FROM '+table+' ORDER BY rowid').all()));
 async function openHost(){const ws=new WebSocket(base.replace('http:','ws:')+'/api/v1/hosts/channel',{headers:{Authorization:'Host '+hostTokens[target],'X-Multitap-Host-Protocol':'capabilities-v1'}});sockets.push(ws);await once(ws,'open');await nextTurn();return ws;}
 function openEvents(clientToken){const ws=new WebSocket(base.replace('http:','ws:')+'/api/v1/events',{headers:{Origin:origin,Authorization:'Bearer '+clientToken}});sockets.push(ws);return ws;}
 return{store,vault,token,admin,user,hostIds,hostTokens,enroll,server,base,call,input,remove,snapshot,openHost,openEvents};
}

function nextEvent(ws){
 return new Promise((resolve,reject)=>{
  const cleanup=()=>{clearTimeout(timer);ws.off('message',message);ws.off('error',fail);ws.off('close',closed);};
  const fail=error=>{cleanup();reject(error);},closed=()=>fail(new Error('event_closed'));
  const message=(bytes,binary)=>{cleanup();try{assert.equal(binary,false);resolve(JSON.parse(bytes.toString()));}catch(error){reject(error);}};
  const timer=setTimeout(()=>fail(new Error('event_timeout')),3000);
  ws.once('message',message);ws.once('error',fail);ws.once('close',closed);
 });
}

test('admin inventory contains only registered Hosts, including offline; raw VM inventory/assignments remain complete',async t=>{
 const f=await fixture(t),view=await f.call('/inventory');assert.equal(view.status,200);assert.deepEqual(view.body.desktops.map(x=>x.id),[target,'pve-811']);
 assert.deepEqual(view.body.assignments.map(x=>x.desktop_id),[target,'pve-811']);assert.equal(f.store.inventory().desktops.length,4);assert.equal(f.store.userAssignments(f.user).length,4);
 assert.equal(view.body.users.find(x=>x.id===f.user).unavailableAssignmentCount,2);assert.equal(view.body.preservedUnavailableAssignments,2);
});
test('enrolled Host projection exposes immutable hostId and preserves offline records',async t=>{
 const f=await fixture(t),view=await f.call('/hosts');assert.deepEqual(view.body.hosts.map(x=>[x.desktopId,x.hostId,x.connected]),[[target,f.hostIds[target],false],['pve-811',f.hostIds['pve-811'],false]]);
});
// The removed Host's
// assignments, with their permissions and favourites, go in the same transaction.
test('idle removal deletes the Host record and enrollment, removes this desktop\'s assignments, keeps the VM row/accounts, and closes idle Host channel',async t=>{
 const f=await fixture(t),ws=await f.openHost(),hostId=f.hostIds[target],oldHash=tokenHash(f.hostTokens[target]);
 f.store.issueEnrollment(target,'12345678-1234-4234-8234-000000000810',tokenHash(newToken()),f.admin);
 const second=f.store.addUser({name:'second',passwordHash:'synthetic'});f.store.assign('second',target,f.admin);f.store.assign('second','pve-811',f.admin);
 f.store.updateAssignmentPolicy('assigned',target,{clipboardPolicy:{textToRemote:true,textToLocal:true,filesToRemote:false,filesToLocal:false},soundCardAdd:true},f.admin);assert.equal(f.store.setFavorite(f.user,target,true),true);
 f.store.db.prepare("UPDATE desktops SET description='typed by an administrator' WHERE id=?").run(target);const vmRows=()=>JSON.stringify(f.store.db.prepare('SELECT * FROM desktops ORDER BY id').all().map(row=>row.id===target?{...row,description:null}:row)),beforeVM=vmRows(),beforeUsers=JSON.stringify(f.store.db.prepare('SELECT * FROM users ORDER BY id').all());
 const others=()=>JSON.stringify(f.store.db.prepare('SELECT * FROM assignments WHERE desktop_id<>? ORDER BY user_id,desktop_id').all(target)),beforeOthers=others();
 const closed=once(ws,'close'),result=await f.remove();assert.equal(result.status,200);assert.deepEqual(result.body,{ok:true,desktopId:target,hostId,removedAssignments:2});await closed;
 assert.equal(f.store.authenticateHost(oldHash),undefined);assert.equal(f.store.db.prepare('SELECT count(*) n FROM hosts WHERE desktop_id=?').get(target).n,0,'nothing of the old Host is left to get in the way of adding it again');assert.equal(f.store.db.prepare('SELECT count(*) n FROM host_enrollments WHERE desktop_id=?').get(target).n,0);
 assert.equal(vmRows(),beforeVM);assert.equal(f.store.db.prepare('SELECT description FROM desktops WHERE id=?').get(target).description,'Proxmox VM 810','the typed display text is reset');assert.equal(JSON.stringify(f.store.db.prepare('SELECT * FROM users ORDER BY id').all()),beforeUsers);
 assert.equal(f.store.db.prepare('SELECT count(*) n FROM assignments WHERE desktop_id=?').get(target).n,0);assert.equal(others(),beforeOthers,'other desktops keep their assignments');
 assert.deepEqual(f.store.userAssignments(f.user).map(x=>x.desktopId),['pve-811','pve-812','pve-813']);assert.deepEqual(f.store.userAssignments(second).map(x=>x.desktopId),['pve-811']);
 // The count is part of the removal's own audit record (hostId:desktopId:count).
 assert.deepEqual(f.store.db.prepare("SELECT actor,subject FROM audit_events WHERE event='host_registration_removed'").all().map(row=>({...row})),[{actor:f.admin,subject:hostId+':'+target+':2'}]);
 assert.deepEqual((await f.call('/inventory')).body.desktops.map(x=>x.id),['pve-811']);
});
test('a Host registered again under the same VM number starts with nobody assigned',async t=>{
 const f=await fixture(t);assert.equal(f.store.setFavorite(f.user,target,true),true);
 assert.equal((await f.remove()).status,200);
 f.enroll(target);assert.ok(f.store.registeredHostIds().has(target));
 assert.equal(f.store.db.prepare('SELECT count(*) n FROM assignments WHERE desktop_id=?').get(target).n,0);
 assert.equal(f.store.listDesktops(f.user).some(row=>row.id===target),false,'the earlier user does not get the new Host');
 assert.deepEqual((await f.call('/inventory')).body.assignments.map(x=>x.desktop_id),['pve-811']);
 // Assigning again starts from denied permissions and no favourite.
 f.store.assign('assigned',target,f.admin);const again=f.store.db.prepare('SELECT clipboard_permissions,rdp_redirections,sound_card_add,favorite,last_used_at FROM assignments WHERE user_id=? AND desktop_id=?').get(f.user,target);
 assert.deepEqual({...again},{clipboard_permissions:0,rdp_redirections:0,sound_card_add:0,favorite:0,last_used_at:null});
});
test('a removal without assignments records a zero count and removes nothing else',async t=>{
 const f=await fixture(t);f.store.revoke('assigned',target,f.admin);const before=JSON.stringify(f.store.db.prepare('SELECT * FROM assignments ORDER BY user_id,desktop_id').all());
 const result=await f.remove();assert.equal(result.status,200);assert.equal(result.body.removedAssignments,0);
 assert.equal(JSON.stringify(f.store.db.prepare('SELECT * FROM assignments ORDER BY user_id,desktop_id').all()),before);
 assert.equal(f.store.db.prepare("SELECT subject FROM audit_events WHERE event='host_registration_removed'").get().subject,f.hostIds[target]+':'+target+':0');
});

test('client inventory removes a deleted favorite/recent Host with its assignment while preserving its VM',async t=>{
 const f=await fixture(t),clientToken=newToken();
 f.store.createSession(f.store.findUser('assigned'),tokenHash(clientToken),Date.now(),60000,'client');
 assert.equal(f.store.setFavorite(f.user,target,true),true);assert.equal(f.store.markUsed(f.user,target),true);
 f.store.db.prepare("UPDATE desktops SET description='typed by an administrator' WHERE id=?").run(target);const vmRows=()=>JSON.stringify(f.store.db.prepare('SELECT * FROM desktops ORDER BY id').all().map(row=>row.id===target?{...row,description:null}:row)),beforeVM=vmRows();
 const otherAssignments=()=>JSON.stringify(f.store.db.prepare('SELECT * FROM assignments WHERE desktop_id<>? ORDER BY user_id,desktop_id').all(target)),beforeAssignments=otherAssignments();
 const list=async()=>{
  const response=await fetch(f.base+'/api/v1/desktops',{headers:{Origin:origin,Authorization:'Bearer '+clientToken}});
  const body=await response.json();assert.equal(response.status,200,body.error?.code);return body.desktops;
 };
 const initial=await list();assert.ok(initial.some(row=>row.id===target&&row.favorite===true&&row.lastUsedAt));
 assert.ok(initial.some(row=>row.id==='pve-811'),'approved offline Host is listed before removal');
 assert.equal((await f.remove()).status,200);
 const after=await list();
 assert.equal(after.some(row=>row.id===target),false,'deleted Host must leave every client card/favorite/recent projection');
 assert.deepEqual(after.map(row=>row.id),['pve-811'],'approved offline remains; unregistered/revoked assignments stay hidden');
 assert.equal(vmRows(),beforeVM);assert.equal(f.store.db.prepare('SELECT description FROM desktops WHERE id=?').get(target).description,'Proxmox VM 810','the typed display text is reset');
 assert.equal(otherAssignments(),beforeAssignments);assert.equal(f.store.db.prepare('SELECT count(*) n FROM assignments WHERE desktop_id=?').get(target).n,0,'favourite and last use went with the assignment');
 const stale=await fetch(f.base+'/api/v1/desktops/'+target,{headers:{Origin:origin,Authorization:'Bearer '+clientToken}});
 assert.equal(stale.status,404);assert.equal((await stale.json()).error.code,'desktop_not_found');
 for(const [suffix,method] of [['/host-ready','GET'],['/connect','POST']]){
  const response=await fetch(f.base+'/api/v1/desktops/'+target+suffix,{method,headers:{Origin:origin,Authorization:'Bearer '+clientToken}});
  assert.equal(response.status,404,suffix);assert.equal((await response.json()).error.code,'desktop_not_found');
 }
});

test('client inventory rejects favorite changes for a deleted Host without recreating its assignment',async t=>{
 const f=await fixture(t),clientToken=newToken();
 f.store.createSession(f.store.findUser('assigned'),tokenHash(clientToken),Date.now(),60000,'client');
 assert.equal((await f.remove()).status,200);
 const before=JSON.stringify(f.store.db.prepare('SELECT * FROM assignments ORDER BY user_id,desktop_id').all());
 const response=await fetch(f.base+'/api/v1/desktops/'+target+'/favorite',{method:'PUT',headers:{Origin:origin,Authorization:'Bearer '+clientToken,'Content-Type':'application/json'},body:JSON.stringify({favorite:true})});
 assert.equal(response.status,404);assert.equal((await response.json()).error.code,'desktop_not_found');
 assert.equal(JSON.stringify(f.store.db.prepare('SELECT * FROM assignments ORDER BY user_id,desktop_id').all()),before);
});

test('client inventory live WS invalidation removes a deleted favorite/recent Host and retains an approved offline peer',async t=>{
 const f=await fixture(t,{desktopEventOptions:{tickMs:20,authorizationMs:1000,heartbeatMs:60000}}),clientToken=newToken();
 f.store.createSession(f.store.findUser('assigned'),tokenHash(clientToken),Date.now(),60000,'client');
 assert.equal(f.store.setFavorite(f.user,target,true),true);assert.equal(f.store.markUsed(f.user,target),true);
 f.store.db.prepare("UPDATE desktops SET description='typed by an administrator' WHERE id=?").run(target);const vmRows=()=>JSON.stringify(f.store.db.prepare('SELECT * FROM desktops ORDER BY id').all().map(row=>row.id===target?{...row,description:null}:row)),beforeVM=vmRows();
 const otherAssignments=()=>JSON.stringify(f.store.db.prepare('SELECT * FROM assignments WHERE desktop_id<>? ORDER BY user_id,desktop_id').all(target)),beforeAssignments=otherAssignments();
 const list=async()=>{const response=await fetch(f.base+'/api/v1/desktops',{headers:{Origin:origin,Authorization:'Bearer '+clientToken}});assert.equal(response.status,200);return(await response.json()).desktops;};
 const initial=await list();assert.ok(initial.some(row=>row.id===target&&row.favorite===true&&row.lastUsedAt));
 assert.ok(initial.some(row=>row.id==='pve-811'&&row.host.connected===false));
 const events=f.openEvents(clientToken);assert.deepEqual(await nextEvent(events),{type:'desktops.ready'});
 const changed=nextEvent(events);
 assert.equal((await f.remove()).status,200);
 const event=await changed;assert.deepEqual(Object.keys(event).sort(),['revision','type']);assert.equal(event.type,'desktops.changed');assert.ok(Number.isSafeInteger(event.revision)&&event.revision>0);
 // This is the same event-triggered list re-read used by released clients.
 const refreshed=await list();assert.deepEqual(refreshed.map(row=>row.id),['pve-811']);
 assert.equal(refreshed[0].host.connected,false);assert.equal(events.readyState,WebSocket.OPEN);
 assert.equal(vmRows(),beforeVM);assert.equal(f.store.db.prepare('SELECT description FROM desktops WHERE id=?').get(target).description,'Proxmox VM 810','the typed display text is reset');
 assert.equal(otherAssignments(),beforeAssignments);assert.equal(f.store.db.prepare('SELECT count(*) n FROM assignments WHERE desktop_id=?').get(target).n,0);
});
test('missing/malformed confirmation/identity and auth failures never write an audit or mutate registration',async t=>{
 const f=await fixture(t),before=f.snapshot();
 assert.equal((await f.remove({confirm:true})).status,428);
 for(const expectedHostId of [null,'',{},[],false,'bad id','x'.repeat(129)])assert.equal((await f.remove({expectedHostId,confirm:true})).status,400);
 for(const confirm of [undefined,false,1,'true'])assert.equal((await f.remove({expectedHostId:f.hostIds[target],confirm})).status,400);
 assert.equal((await f.remove(f.input(),{cookie:null})).status,401);assert.equal((await f.remove(f.input(),{csrf:false})).status,403);assert.equal((await f.remove(f.input(),{foreign:true})).status,403);assert.equal(f.snapshot(),before);
});
test('stale target after replacement cannot revoke the new Host or emit an audit',async t=>{
 const f=await fixture(t),old=f.input();f.enroll(target);const before=f.snapshot();const response=await f.remove(old);assert.equal(response.status,409);assert.equal(response.body.error.code,'host_registration_changed');assert.equal(f.snapshot(),before);
});
for(const kind of ['pending-ticket','attached-stream','display','console','cleanup','update','vm-config','clipboard'])test('removal rejects current '+kind+' activity without terminating it',async t=>{
 const f=await fixture(t),r=f.server.relay,session={id:'synthetic-session',desktopId:target,mode:'stream',ws:kind==='attached-stream'?{}:null};let clear;
 if(kind==='pending-ticket'||kind==='attached-stream'){r.sessions.set(session.id,session);clear=()=>r.sessions.delete(session.id);}
 else if(kind==='display'){r.displayBusy.set(target,{desktopId:target});clear=()=>r.displayBusy.delete(target);}
 else if(kind==='console'){r.console.grants.set('synthetic-grant',{desktopId:target});clear=()=>r.console.grants.delete('synthetic-grant');}
 else if(kind==='cleanup'){r.sessionStop.pending.set(target,{});clear=()=>r.sessionStop.pending.delete(target);}
 else if(kind==='update'){r.hostUpdate.operations.set(target,{status:'waiting'});clear=()=>r.hostUpdate.operations.delete(target);}
 else if(kind==='vm-config'){r.vmConfigOperations.set(target,{});clear=()=>r.vmConfigOperations.delete(target);}
 else{const pair={session,transfer:null,pulls:[]};r.clipboard.pairs.add(pair);clear=()=>r.clipboard.pairs.delete(pair);}
 // These intentionally partial records exercise the REAL activity producer,
 // route and transaction. Suppress only unrelated response-time relay sweep,
 // which expects fully constructed operations. Real live-session cases follow.
 const revalidate=r.revalidate;r.revalidate=()=>{};
 const before=f.snapshot();try{const result=await f.remove();assert.equal(result.status,409);assert.equal(result.body.error.code,'host_registration_busy');assert.equal(f.snapshot(),before);}finally{clear();r.revalidate=revalidate;}
});
test('unknown activity ownership is not idle',async t=>{
 const f=await fixture(t),r=f.server.relay;r.sessions.set('unknown',{mode:'stream',ws:null});const revalidate=r.revalidate;r.revalidate=()=>{};const before=f.snapshot();try{const result=await f.remove();assert.equal(result.status,503);assert.equal(result.body.error.code,'host_activity_unavailable');assert.equal(f.snapshot(),before);}finally{r.sessions.delete('unknown');r.revalidate=revalidate;}
});
test('public assignment batch preserves hidden grants/counts and rejects explicitly selected unregistered Hosts atomically',async t=>{
 const f=await fixture(t),data={username:'assigned',expectedAccountId:f.user,desktopIds:[target]};
 const saved=await f.call('/assignments/batch',{method:'PUT',data});assert.equal(saved.status,200);assert.equal(saved.body.preservedUnavailable,2);assert.deepEqual(f.store.userAssignments(f.user).map(x=>x.desktopId),[target,'pve-812','pve-813']);
 const before=f.snapshot(),response=await f.call('/assignments/batch',{method:'PUT',data:{...data,desktopIds:['pve-812']}});assert.equal(response.status,409);assert.equal(response.body.error.code,'host_not_registered');assert.equal(f.snapshot(),before);
});
for(const attached of [false,true])test('real issued '+(attached?'attached WS stream':'pending stream ticket')+' blocks removal without closing or revoking its owners',async t=>{
 const f=await fixture(t),host=await f.openHost(),messages=[];host.on('message',b=>messages.push(JSON.parse(b.toString())));
 host.send(JSON.stringify({type:'host.capabilities',capabilities:{service:true,interactiveSession:true,streaming:true,virtualDisplayReady:true}}));
 for(let i=0;i<200&&!messages.some(x=>x.type==='host.capabilities.ack');i++)await delay(5);
 assert.ok(messages.some(x=>x.type==='host.capabilities.ack'&&x.accepted===true));
 const token=newToken();f.store.createSession(f.store.findUser('assigned'),tokenHash(token),Date.now(),60000,'client');
 const response=await fetch(f.base+'/api/v1/desktops/'+target+'/connect',{method:'POST',headers:{Origin:origin,Authorization:'Bearer '+token,'Content-Type':'application/json'},body:'{}'});
 const grant=await response.json();assert.equal(response.status,200,grant.error?.code);const session=f.server.relay.sessions.get(grant.sessionId);assert.ok(session);
 let client;try{
  if(attached){client=new WebSocket(f.base.replace('http:','ws:')+'/api/v1/sessions/'+grant.sessionId,{headers:{Origin:origin,Authorization:'Bearer '+token}});await once(client,'open');await nextTurn();assert.ok(session.ws);}
  const result=await f.remove();assert.equal(result.status,409);assert.equal(result.body.error.code,'host_registration_busy');
  assert.equal(f.server.relay.sessions.get(grant.sessionId),session);assert.equal(host.readyState,WebSocket.OPEN);if(client)assert.equal(client.readyState,WebSocket.OPEN);
  assert.equal(f.store.authenticateHost(tokenHash(f.hostTokens[target])).id,f.hostIds[target]);
 }finally{client?.terminate();}
});
test('hidden/unregistered and unavailable overlap is counted once',async t=>{
 const f=await fixture(t);f.store.db.prepare("UPDATE desktops SET pve_available=0 WHERE id='pve-812'").run();
 const result=await f.call('/inventory');assert.equal(result.body.users.find(x=>x.id===f.user).unavailableAssignmentCount,2);assert.equal(result.body.preservedUnavailableAssignments,2);
 const saved=await f.call('/assignments/batch',{method:'PUT',data:{username:'assigned',expectedAccountId:f.user,desktopIds:[target]}});assert.equal(saved.status,200);assert.equal(saved.body.preservedUnavailable,2);
});
test('removal invalidates old approved credential delivery and requires genuine fresh code approval for re-enrollment',async t=>{
 const f=await fixture(t),requests=f.server.relay; // API approval uses the same current Store, never old credentials.
 const requestId='00000000-0000-4000-8000-000000000810';
 f.store.db.prepare("INSERT INTO host_requests(id,key_hash,hostname,machine_uuid,version,status,created_at,expires_at,desktop_id,host_id,credentials_sealed) VALUES(?,?,?,?,?,'approved',?,?,?,?,?)").run(requestId,tokenHash(newToken()),'synthetic','12345678-1234-4234-8234-000000000810','1.0.0',Date.now(),Date.now()+60000,target,f.hostIds[target],'synthetic-sealed');
 assert.equal((await f.remove()).status,200);const row=f.store.db.prepare('SELECT status,credentials_sealed FROM host_requests WHERE id=?').get(requestId);assert.equal(row.status,'rejected');assert.equal(row.credentials_sealed,null);
 assert.equal(f.store.db.prepare('SELECT count(*) n FROM hosts WHERE desktop_id=?').get(target).n,0);
 assert.equal(requests.hosts.has(target),false);
});
test('Store transaction rolls back revoke/enrollment/approval/assignment changes when the final audit fails',async t=>{
 const f=await fixture(t),before=f.snapshot(),audit=f.store.audit;f.store.audit=()=>{throw Error('injected_audit_failure');};
 try{assert.throws(()=>f.store.removeHostRegistration(target,f.hostIds[target],f.admin,()=>{}),/injected_audit_failure/);}finally{f.store.audit=audit;}
 assert.equal(f.snapshot(),before);assert.ok(f.store.authenticateHost(tokenHash(f.hostTokens[target])));
});
for(const change of ['replacement','busy','logout'])test('actual partial HTTP body rechecks '+change+' before removing Host',async t=>{
 const f=await fixture(t),body=JSON.stringify(f.input()),half=Math.floor(body.length/2);let resolve,reject;const response=new Promise((ok,no)=>{resolve=ok;reject=no;});
 const req=request({hostname:'127.0.0.1',port:f.server.address().port,method:'DELETE',path:'/api/v1/admin/desktops/'+target+'/host',headers:{Origin:origin,Cookie:'__Secure-multitap-admin='+f.token,'X-CSRF-Token':f.vault.csrf(f.token),'Content-Type':'application/json','Content-Length':Buffer.byteLength(body)}},res=>{const chunks=[];res.on('data',b=>chunks.push(b));res.on('end',()=>resolve({status:res.statusCode,body:JSON.parse(Buffer.concat(chunks).toString())}));});req.on('error',reject);req.write(body.slice(0,half));await nextTurn();await nextTurn();
 if(change==='replacement')f.enroll(target);else if(change==='logout')f.store.logout(tokenHash(f.token));else f.server.relay.displayBusy.set(target,{kind:'qemu-console',desktopId:target});
 const before=f.snapshot();try{req.end(body.slice(half));const result=await response;assert.equal(result.status,change==='logout'?401:409);assert.equal(f.snapshot(),before);}finally{f.server.relay.displayBusy.delete(target);}
});
