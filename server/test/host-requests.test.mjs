import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { HostRequests } from '../src/host-requests.mjs';
import { Proxmox } from '../src/proxmox.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { HttpError } from '../src/errors.mjs';
import { hostRequestCode } from '../src/verification-codes.mjs';

const uuid='12345678-1234-1234-1234-123456789abc',otherUuid='87654321-4321-4321-4321-cba987654321';
const metadata=(requestKey=newToken(),machineUuid=uuid)=>({requestKey,machineUuid,hostname:'격리 Windows VM',version:'0.2.12'});
const vm=(vmid=710,machineUuid=uuid)=>({vmid,node:'fixture-node',name:'Fixture VM '+vmid,status:'running',machineUuid});
function fixture(t,options={}){
  const store=new Store(':memory:'),vault=new Vault(randomBytes(32));let clock=Date.now(),reads=0,identity=uuid,hook,changes=0;
  const admin=store.addUser({name:'fixture-admin',role:'admin',passwordHash:'isolated-unused-password-hash'});
  store.addUser({name:'fixture-user',role:'user',passwordHash:'isolated-unused-password-hash'});
  store.syncProxmox([vm(),vm(711,otherUuid)],'fixture');
  store.saveSetting('proxmox',vault.seal({endpoint:'https://pve-fixture.invalid',tokenId:'fixture@pve!requests',secret:'isolated-not-real-secret'}),'fixture');
  const proxmox={inventory:async()=>[vm(),vm(711,otherUuid)],machineIdentity:async()=>{reads++;if(hook)await hook();if(identity instanceof Error)throw identity;return{machineUuid:identity};}};
  const requests=new HostRequests({store,vault,proxmox,now:()=>clock,changed:()=>changes++,...options});
  // These Hosts show the verification code unless a test says otherwise (false):
  // every approval needs that code compared and acknowledged.
  const create=requests.create.bind(requests);requests.create=(input,address,shows=true)=>create(input,address,shows);
  t.after(()=>store.close());
  return{store,vault,proxmox,requests,admin,authorize:()=>admin,advance:ms=>clock+=ms,setIdentity:value=>identity=value,setHook:value=>hook=value,get reads(){return reads;},get changes(){return changes;}};
}
const claim=(created,input)=>({requestId:created.requestId,requestKey:input.requestKey});
const error=code=>value=>value instanceof HttpError&&value.code===code;

test('Unverified requests issue no token; idempotency binds key, metadata and original expiry without extending TTL',t=>{
  const f=fixture(t),input=metadata(),created=f.requests.create(input,'address');
  assert.equal(created.status,'pending');assert.equal(created.retryAfterSeconds,3);assert.equal(Object.hasOwn(created,'credentials'),false);
  assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM hosts').get().n,0);
  f.advance(1000);assert.deepEqual(f.requests.create(input,'address'),created);
  assert.deepEqual(f.requests.status(claim(created,input),'address'),created);
  for(const altered of [{...input,hostname:'different'},{...input,version:'0.2.13'},{...input,machineUuid:otherUuid}])assert.throws(()=>f.requests.create(altered),error('host_request_conflict'));
  assert.equal(f.reads,0);assert.ok(!JSON.stringify(f.store.db.prepare('SELECT * FROM host_requests').all()).includes(input.requestKey));
  const listing=f.requests.pending();assert.equal(listing.pendingCount,1);assert.equal(listing.requests[0].suggestedDesktopId,'pve-710');assert.equal(listing.requests[0].candidates[0].hasExistingHost,false);
  assert.ok(!JSON.stringify(listing).includes(input.requestKey));assert.ok(!JSON.stringify(listing).includes('key_hash'));
});

test('Each request carries a verification code derived from its key hash, matching the Host-side derivation',t=>{
  // Shared vector with apps/windows_host/src/pending_connection_test.cpp.
  assert.equal(hostRequestCode(tokenHash('K'.repeat(43))),'49J2-HQP1');
  const f=fixture(t),input=metadata(),created=f.requests.create(input,'address',true),legacy=metadata(newToken(),otherUuid),old=f.requests.create(legacy,'address',false);
  const byId=id=>f.requests.pending().requests.find(value=>value.requestId===id),listed=byId(created.requestId),released=byId(old.requestId);
  assert.equal(listed.verificationCode,hostRequestCode(tokenHash(input.requestKey)));
  assert.match(listed.verificationCode,/^[0-9A-HJKMNP-TV-Z]{4}-[0-9A-HJKMNP-TV-Z]{4}$/);assert.equal(listed.codeDisplay,'shown');assert.deepEqual(listed.duplicateRequestIds,[]);
  // A released Host (no header) is listed as unable to show its code.
  assert.equal(released.codeDisplay,'unavailable');
  // The status poll alone teaches a restarted broker that the Host shows it.
  const restarted=new HostRequests({store:f.store,vault:f.vault,proxmox:f.proxmox}),again=()=>restarted.pending().requests.find(value=>value.requestId===created.requestId);
  assert.equal(again().codeDisplay,'unavailable');restarted.status(claim(created,input),'address',true);
  assert.equal(again().codeDisplay,'shown');
  assert.ok(!JSON.stringify(f.requests.pending()).includes(input.requestKey));
});

test('Several live claims on one VM are all listed, and approval needs a comparable code and the administrator\'s acknowledgement',async t=>{
  const f=fixture(t),real=metadata(),squat=metadata(),first=f.requests.create(squat,'attacker',true),second=f.requests.create(real,'vm',true);
  const listing=f.requests.pending().requests,a=listing.find(value=>value.requestId===first.requestId),b=listing.find(value=>value.requestId===second.requestId);
  assert.deepEqual(a.duplicateRequestIds,[second.requestId]);assert.deepEqual(b.duplicateRequestIds,[first.requestId]);assert.notEqual(a.verificationCode,b.verificationCode);
  await assert.rejects(f.requests.approve(second.requestId,{acknowledgeCode:true,desktopId:'pve-710'},f.authorize),error('host_request_duplicate'));
  assert.equal(f.reads,0);assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM hosts').get().n,0);
  // After comparing the code with the Host's window the administrator may approve the matching one.
  await f.requests.approve(second.requestId,{acknowledgeCode:true,desktopId:'pve-710',acknowledgeDuplicates:true},f.authorize);
  assert.equal(f.requests.status(claim(second,real)).status,'approved');
  // A claim from a Host that shows no code cannot be told apart from a rival.
  const g=fixture(t),oldHost=metadata(),rival=metadata(),unverifiable=g.requests.create(oldHost,'vm',false);g.requests.create(rival,'other',true);
  await assert.rejects(g.requests.approve(unverifiable.requestId,{acknowledgeCode:true,desktopId:'pve-710',acknowledgeDuplicates:true},g.authorize),error('host_request_code_required'));
  // Without a rival either: a Host that cannot show the code is updated before it is approved.
  const h=fixture(t),legacy=metadata(),only=h.requests.create(legacy,'vm',false);
  await assert.rejects(h.requests.approve(only.requestId,{acknowledgeCode:true,desktopId:'pve-710'},h.authorize),error('host_request_code_required'));
  assert.equal(h.store.db.prepare('SELECT count(*) AS n FROM hosts').get().n,0);
  // Claims per machine stay bounded; rejecting a rival lifts the guard.
  const k=fixture(t),claims=[metadata(),metadata(),metadata(),metadata()].map(value=>({value,created:k.requests.create(value,'many',true)}));
  assert.throws(()=>k.requests.create(metadata(),'many',true),error('host_request_pending'));
  for(const {created} of claims.slice(1))k.requests.reject(created.requestId,k.authorize);
  assert.equal((await k.requests.approve(claims[0].created.requestId,{acknowledgeCode:true,desktopId:'pve-710'},k.authorize)).status,'approved');
  await assert.rejects(f.requests.approve(second.requestId,{acknowledgeCode:true,desktopId:'pve-710',acknowledgeDuplicates:'yes'},f.authorize),value=>value instanceof HttpError&&value.status===400);
});

test('Two live claims showing the same code are never approvable',async t=>{
  // As if an attacker who saw the Host's code ground a key with the same code.
  const f=fixture(t,{code:()=>'AAAA-AAAA'}),created=f.requests.create(metadata(),'one',true);f.requests.create(metadata(),'two',true);
  assert.deepEqual(f.requests.pending().requests.map(value=>value.verificationCode),['AAAA-AAAA','AAAA-AAAA']);
  await assert.rejects(f.requests.approve(created.requestId,{acknowledgeCode:true,desktopId:'pve-710',acknowledgeDuplicates:true},f.authorize),error('host_request_code_collision'));
  assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM hosts').get().n,0);
});

test('Only explicit fresh-identity approval enrolls; lost responses recover the same encrypted credentials and approval retries never rotate them',async t=>{
  const f=fixture(t),input=metadata(),created=f.requests.create(input);
  const result=await f.requests.approve(created.requestId,{acknowledgeCode:true,desktopId:'pve-710',replaceExisting:false},f.authorize);
  assert.deepEqual(Object.keys(result).sort(),['desktopId','ok','requestId','status']);assert.equal(result.status,'approved');assert.equal(f.reads,1);
  const first=f.requests.status(claim(created,input)),second=f.requests.status(claim(created,input));
  assert.equal(first.status,'approved');assert.deepEqual(first.credentials,second.credentials);
  assert.equal(f.store.authenticateHost(tokenHash(first.credentials.hostToken)).desktop_id,'pve-710');
  const row=f.store.db.prepare('SELECT * FROM host_requests WHERE id=?').get(created.requestId);
  assert.ok(row.credentials_sealed);assert.ok(!JSON.stringify(row).includes(first.credentials.hostToken));assert.ok(!JSON.stringify(row).includes(input.requestKey));
  assert.equal(f.vault.open(row.credentials_sealed).credentials.hostToken,first.credentials.hostToken);
  assert.deepEqual(await f.requests.approve(created.requestId,{acknowledgeCode:true,desktopId:'pve-710',replaceExisting:false},f.authorize),result);
  assert.equal(f.reads,1);assert.deepEqual(f.requests.status(claim(created,input)).credentials,first.credentials);
  assert.equal(f.requests.create(input).status,'approved');assert.equal(Object.hasOwn(f.requests.create(input),'credentials'),false);
  await assert.rejects(f.requests.approve(created.requestId,{acknowledgeCode:true,desktopId:'pve-711'},f.authorize),error('host_request_conflict'));
  await assert.rejects(f.requests.approve(created.requestId,{acknowledgeCode:true,desktopId:'pve-710',replaceExisting:true},f.authorize),error('host_request_conflict'));
  assert.throws(()=>f.requests.cancel(claim(created,input)),error('host_request_already_approved'));
});

test('Wrong proof, metadata, body fields, missing UUIDs and uncommissioned brokers cannot claim or create a registration',t=>{
  const f=fixture(t),input=metadata(),created=f.requests.create(input);
  assert.throws(()=>f.requests.status({...claim(created,input),requestKey:newToken()}),error('host_request_not_found'));
  assert.throws(()=>f.requests.status({requestId:created.requestId,requestKey:input.requestKey,extra:true}),error('unexpected_field'));
  for(const altered of [{...metadata(),requestKey:'weak'},{...metadata(),machineUuid:'00000000-0000-0000-0000-000000000000'},{...metadata(),machineUuid:'not-a-uuid'},{...metadata(),version:'<b>'},{...metadata(),hostname:'x'.repeat(129)},{...metadata(),code:'unsupported'}])assert.throws(()=>f.requests.create(altered),value=>value instanceof HttpError&&value.status===400);
  f.store.saveSetting('proxmox_initialized','0','fixture');assert.throws(()=>f.requests.create(metadata()),error('proxmox_setup_required'));
});

test('Cancellation and rejection release pending UUID guards without revoking unrelated host registrations',t=>{
  const f=fixture(t),input=metadata(),created=f.requests.create(input);
  assert.equal(f.requests.cancel(claim(created,input)).status,'rejected');assert.equal(f.requests.cancel(claim(created,input)).status,'rejected');
  const replacement=metadata(),next=f.requests.create(replacement);assert.notEqual(next.requestId,created.requestId);
  assert.equal(f.requests.reject(next.requestId,f.authorize).status,'rejected');assert.equal(f.requests.reject(next.requestId,f.authorize).status,'rejected');
  assert.equal(f.requests.status(claim(next,replacement)).status,'rejected');assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM hosts').get().n,0);
});

test('Expiry clears encrypted delivery material while retaining an already issued host and never extending the original ten-minute window',async t=>{
  const f=fixture(t,{ttlMs:100}),input=metadata(),created=f.requests.create(input);
  await f.requests.approve(created.requestId,{acknowledgeCode:true,desktopId:'pve-710'},f.authorize);
  const token=f.requests.status(claim(created,input)).credentials.hostToken;f.advance(101);
  assert.equal(f.requests.status(claim(created,input)).status,'expired');assert.equal(f.requests.create(input).expiresAt,created.expiresAt);
  assert.equal(f.store.db.prepare('SELECT credentials_sealed FROM host_requests WHERE id=?').get(created.requestId).credentials_sealed,null);
  assert.equal(f.store.authenticateHost(tokenHash(token)).desktop_id,'pve-710');
  await assert.rejects(f.requests.approve(created.requestId,{acknowledgeCode:true,desktopId:'pve-710'},f.authorize),error('host_request_expired'));
});

test('Pending/global capacity and separate proof/address polling limits remain bounded without sharing the create namespace',t=>{
  const f=fixture(t,{maxPending:1,maxRows:2,createLimit:4,statusLimit:2,statusAddressLimit:4}),input=metadata(),created=f.requests.create(input,'one');
  assert.throws(()=>f.requests.create(metadata(newToken(),otherUuid),'two'),error('host_request_capacity'));
  f.requests.status(claim(created,input),'one');f.requests.status(claim(created,input),'one');
  assert.throws(()=>f.requests.status(claim(created,input),'other'),error('too_many_host_request_polls'));
  assert.equal(f.requests.create(input,'one').requestId,created.requestId);
  f.advance(60001);f.requests.cancel(claim(created,input),'one');
  const second=f.requests.create(metadata(newToken(),otherUuid),'two');f.requests.reject(second.requestId,f.authorize);
  assert.throws(()=>f.requests.create(metadata(),'one'),error('host_request_capacity'));
  const limits=fixture(t,{createLimit:1,statusAddressLimit:1});const owned=metadata(),a=limits.requests.create(owned,'one');
  assert.throws(()=>limits.requests.create(owned,'one'),error('too_many_host_requests'));
  limits.requests.status(claim(a,owned),'address');assert.throws(()=>limits.requests.status({...claim(a,owned),requestKey:newToken()},'address'),error('too_many_host_request_polls'));
});

test('Existing hosts require explicit replacement, old tokens are revoked, and stale encrypted delivery cannot reclaim the replacement',async t=>{
  const f=fixture(t),oldInput=metadata(),old=f.requests.create(oldInput);
  await f.requests.approve(old.requestId,{acknowledgeCode:true,desktopId:'pve-710'},f.authorize);const oldToken=f.requests.status(claim(old,oldInput)).credentials.hostToken;
  const input=metadata(),next=f.requests.create(input);assert.equal(f.requests.pending().requests[0].candidates[0].hasExistingHost,true);
  await assert.rejects(f.requests.approve(next.requestId,{acknowledgeCode:true,desktopId:'pve-710'},f.authorize),error('host_already_registered'));
  assert.ok(f.store.authenticateHost(tokenHash(oldToken)));
  await f.requests.approve(next.requestId,{acknowledgeCode:true,desktopId:'pve-710',replaceExisting:true},f.authorize);
  assert.equal(f.store.authenticateHost(tokenHash(oldToken)),undefined);assert.equal(f.requests.status(claim(old,oldInput)).status,'rejected');
  assert.equal(f.requests.status(claim(next,input)).status,'approved');
  const current=f.requests.status(claim(next,input)).credentials;
  f.store.issueEnrollment('pve-710',uuid,tokenHash(newToken()),'legacy');
  f.store.db.prepare('DELETE FROM hosts WHERE desktop_id=?').run('pve-710');
  assert.equal(f.requests.status(claim(next,input)).status,'rejected');assert.equal(f.store.authenticateHost(tokenHash(current.hostToken)),undefined);
});

test('Fresh UUID mismatch or Proxmox failure leaves pending state and existing host data unchanged',async t=>{
  const f=fixture(t),input=metadata(),created=f.requests.create(input);
  for(const identity of [otherUuid,null,new HttpError(502,'proxmox_unreachable')]){
    f.setIdentity(identity);await assert.rejects(f.requests.approve(created.requestId,{acknowledgeCode:true,desktopId:'pve-710'},f.authorize),value=>value instanceof HttpError&&[409,502].includes(value.status));
    assert.equal(f.requests.status(claim(created,input)).status,'pending');assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM hosts').get().n,0);
  }
});

test('Approval rechecks administrator authorization, config revision, VM identity, cancellation, expiry and concurrent host replacement after the network await',async t=>{
  for(const scenario of ['authorization','configuration','vm','cancellation','expiry','host'])await t.test(scenario,async sub=>{
    const f=fixture(sub),input=metadata(),created=f.requests.create(input);let allowed=true;
    f.setHook(async()=>{
      if(scenario==='authorization')allowed=false;
      if(scenario==='configuration')f.store.saveSetting('proxmox',f.vault.seal({endpoint:'https://changed.invalid',tokenId:'x',secret:'x'}),'fixture');
      if(scenario==='vm')f.store.db.prepare("UPDATE desktops SET pve_node='changed' WHERE id='pve-710'").run();
      if(scenario==='cancellation')f.requests.cancel(claim(created,input));
      if(scenario==='expiry')f.advance(600001);
      if(scenario==='host')f.store.db.prepare('INSERT INTO hosts(id,desktop_id,token_hash,hostname,machine_uuid,version,created_at) VALUES(?,?,?,?,?,?,?)').run('other-host','pve-710',tokenHash(newToken()),'unchanged',uuid,'0.2.1',Date.now());
    });
    await assert.rejects(f.requests.approve(created.requestId,{acknowledgeCode:true,desktopId:'pve-710'},()=>{if(!allowed)throw new HttpError(401,'authentication_required');return f.admin;}),value=>value instanceof HttpError&&[401,409].includes(value.status));
    assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM hosts').get().n,scenario==='host'?1:0);
  });
});

test('Concurrent approval retries preserve one host/token and one approval audit event',async t=>{
  const f=fixture(t),input=metadata(),created=f.requests.create(input);let release;const barrier=new Promise(resolve=>release=resolve);f.setHook(()=>barrier);
  const first=f.requests.approve(created.requestId,{acknowledgeCode:true,desktopId:'pve-710'},f.authorize),second=f.requests.approve(created.requestId,{acknowledgeCode:true,desktopId:'pve-710'},f.authorize);release();
  assert.deepEqual(await first,await second);assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM hosts').get().n,1);
  assert.equal(f.store.db.prepare("SELECT count(*) AS n FROM audit_events WHERE event='host_request_approved'").get().n,1);
});

test('Exact target UUID observations remain unverified registration suggestions and never discover LXC/templates or other VMs',async t=>{
  const proxmox=new Proxmox(),calls=[];proxmox.call=async(_cfg,path)=>{calls.push(path);if(path==='/nodes/fixture/qemu/710/status/current')return{status:'running'};assert.equal(path,'/nodes/fixture/qemu/710/config?current=1');return{name:'VM',ostype:'win11',smbios1:'uuid='+uuid};};
  const inventory=await proxmox.inventory({},{targets:[{node:'fixture',vmid:710,expectedUuid:uuid}]});assert.equal(inventory.length,1);assert.equal(inventory[0].machineUuid,uuid);assert.equal(calls.length,2);assert.ok(calls.every(path=>!path.startsWith('/cluster/')));
  const f=fixture(t);f.store.syncProxmox(inventory,'fixture');f.requests.create(metadata());assert.equal(f.requests.pending().requests[0].suggestedDesktopId,'pve-710');assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM hosts').get().n,0);
  f.store.syncProxmox([vm(710,uuid),vm(711,uuid)],'fixture');assert.equal(f.requests.pending().requests[0].matchState,'ambiguous');assert.equal(f.requests.pending().requests[0].suggestedDesktopId,null);
  f.store.syncProxmox([vm(710,null)],'fixture');assert.equal(f.requests.pending().requests[0].matchState,'unknown');
  assert.throws(()=>f.store.syncProxmox([vm(710,'invalid')],'fixture'),/invalid_proxmox_inventory/);
});

test('Actual HTTP request/approval/status flow enforces purpose, CSRF, commissioning, body limits and fresh administrator revocation',async t=>{
  const f=fixture(t),adminToken=newToken(),clientToken=newToken();
  f.store.createSession(f.store.findUser('fixture-admin'),tokenHash(adminToken),Date.now(),3600000,'admin');
  f.store.createSession(f.store.findUser('fixture-user'),tokenHash(clientToken),Date.now(),3600000,'client');
  const origin='https://host-request-fixture.example',errors=[];
  const server=await buildServer({store:f.store,vault:f.vault,proxmox:f.proxmox,publicOrigin:origin,logger:{error:value=>errors.push(value)}});
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));t.after(()=>new Promise(resolve=>server.close(resolve)));
  const base='http://127.0.0.1:'+server.address().port;
  const call=async(path,{method='POST',data,admin=false,csrf=true,headers={}}={})=>{
    const response=await fetch(base+'/api/v1'+path,{method,headers:{Origin:origin,...(data?{'Content-Type':'application/json'}:{}),...(admin?{Cookie:'__Secure-multitap-admin='+adminToken,...(csrf?{'X-CSRF-Token':f.vault.csrf(adminToken)}:{})}:{}),...headers},body:data?JSON.stringify(data):undefined});
    return{status:response.status,body:await response.json()};
  };
  const input=metadata(),created=await call('/hosts/requests',{data:input});assert.equal(created.status,201);assert.equal(created.body.status,'pending');
  const path='/admin/hosts/requests/'+created.body.requestId;
  assert.equal((await call('/admin/hosts/requests',{method:'GET',admin:true,csrf:false})).status,403);
  assert.equal((await call('/admin/hosts/requests',{method:'GET',headers:{Authorization:'Bearer '+clientToken}})).status,401);
  assert.equal((await call(path+'/approve',{data:{acknowledgeCode:true,desktopId:'pve-710'},admin:true,csrf:false})).status,403);
  assert.equal((await call('/hosts/requests/status',{data:{...claim(created.body,input),extra:true}})).status,400);
  assert.equal((await call('/hosts/requests/status',{data:{...claim(created.body,input),padding:'x'.repeat(600)}})).status,413);
  assert.equal((await call('/hosts/requests/status?key=invalid-marker',{data:claim(created.body,input)})).status,400);
  assert.equal((await call('/hosts/requests/status',{data:claim(created.body,input),headers:{Origin:'https://wrong.example'}})).status,403);
  const listed=await call('/admin/hosts/requests',{method:'GET',admin:true});assert.equal(listed.status,200);assert.equal(listed.body.pendingCount,1);
  // host-request-code-v1: a released Host (no header) is listed as unable to show
  // the code until a poll carries the header; the code itself is always listed.
  assert.equal(listed.body.requests[0].codeDisplay,'unavailable');assert.equal(listed.body.requests[0].verificationCode,hostRequestCode(tokenHash(input.requestKey)));
  assert.equal((await call('/hosts/requests/status',{data:claim(created.body,input),headers:{'X-Multitap-Host-Request-Code':'v1'}})).status,200);
  assert.equal((await call('/admin/hosts/requests',{method:'GET',admin:true})).body.requests[0].codeDisplay,'shown');
  // Over HTTP too: no tick, no approval (a VM the table knows, as here).
  for(const data of [{desktopId:'pve-710'},{desktopId:'pve-710',acknowledgeCode:false},{desktopId:'pve-710',replaceExisting:true}]){const refused=await call(path+'/approve',{data,admin:true});assert.equal(refused.status,409);assert.equal(refused.body.error.code,'host_request_code_required');}
  assert.equal(f.reads,0);
  f.store.saveSetting('proxmox_initialized','0','fixture');assert.equal((await call('/admin/hosts/requests',{method:'GET',admin:true})).status,428);f.store.saveSetting('proxmox_initialized','1','fixture');
  const approved=await call(path+'/approve',{data:{acknowledgeCode:true,desktopId:'pve-710',replaceExisting:false},admin:true});assert.equal(approved.status,200);assert.equal(Object.hasOwn(approved.body,'credentials'),false);
  const polled=await call('/hosts/requests/status',{data:claim(created.body,input)});assert.equal(polled.status,200);assert.equal(polled.body.status,'approved');
  assert.equal((await call('/hosts/requests/cancel',{data:claim(created.body,input)})).status,409);
  const hostToken=polled.body.credentials.hostToken;
  const ws=new WebSocket(base.replace(/^http/,'ws')+'/api/v1/hosts/channel',{headers:{Authorization:'Host '+hostToken,'X-Multitap-Host-Protocol':'capabilities-v1'}});ws.on('error',()=>{});await new Promise((resolve,reject)=>{ws.once('open',resolve);ws.once('error',reject);});t.after(()=>ws.terminate());
  assert.equal(f.store.authenticateHost(tokenHash(hostToken)).desktop_id,'pve-710');ws.close();
  const next=await call('/hosts/requests',{data:metadata(newToken(),otherUuid),headers:{'X-Multitap-Host-Request-Code':'v1'}});
  f.setHook(async()=>f.store.logout(tokenHash(adminToken)));
  assert.equal((await call('/admin/hosts/requests/'+next.body.requestId+'/approve',{data:{acknowledgeCode:true,desktopId:'pve-711'},admin:true})).status,401);
  assert.deepEqual(errors,[]);
});

test('Every approval path refuses without the administrator\'s explicit code tick, and replacing a Host needs it too',async t=>{
  const f=fixture(t),input=metadata(),created=f.requests.create(input);
  // A VM the table knows, the typed node/VM, and either with replaceExisting: none goes through without acknowledgeCode:true.
  for(const approval of [{desktopId:'pve-710'},{desktopId:'pve-710',acknowledgeCode:false},{desktopId:'pve-710',replaceExisting:true},{node:'fixture-node',vmid:710},{node:'fixture-node',vmid:710,replaceExisting:true,acknowledgeCode:false}]){
    await assert.rejects(f.requests.approve(created.requestId,approval,f.authorize),error('host_request_code_required'),JSON.stringify(approval));
  }
  assert.equal(f.reads,0,'refused before Proxmox is asked');assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM hosts').get().n,0);
  assert.equal(f.requests.status(claim(created,input)).status,'pending');
  // Replacing a registered Host: the same refusal, and the old Host stays.
  await f.requests.approve(created.requestId,{acknowledgeCode:true,desktopId:'pve-710'},f.authorize);
  const token=f.requests.status(claim(created,input)).credentials.hostToken,nextInput=metadata(),next=f.requests.create(nextInput);
  await assert.rejects(f.requests.approve(next.requestId,{desktopId:'pve-710',replaceExisting:true},f.authorize),error('host_request_code_required'));
  assert.ok(f.store.authenticateHost(tokenHash(token)),'the registered Host is untouched');
  assert.equal((await f.requests.approve(next.requestId,{desktopId:'pve-710',replaceExisting:true,acknowledgeCode:true},f.authorize)).status,'approved');
  assert.equal(f.store.authenticateHost(tokenHash(token)),undefined);
});
