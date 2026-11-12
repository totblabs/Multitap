import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { createServer } from 'node:https';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { Proxmox } from '../src/proxmox.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { HttpError } from '../src/errors.mjs';
import { PowerControl } from '../src/power.mjs';

const origin = 'https://client-power.example';
const config = { endpoint:'https://pve.example:8006',tokenId:'fixture@pve!power',secret:'power-fixture-secret',ca:'' };
const vm = (vmid, status='stopped') => ({vmid,status,name:`VM ${vmid}`,node:'fixture-node'});
const machineUuid = vmid => `12345678-1234-4234-8234-${String(vmid).padStart(12,'0')}`;
async function fixture(t, adapter, extra={}) {
  const store=new Store(':memory:'),vault=new Vault(randomBytes(32));
  const users={},tokens={};
  for(const name of ['alice','bobby','admin']) {
    const id=store.addUser({name,passwordHash:'isolated-power-hash',role:name==='admin'?'admin':'user'});
    users[name]=id;tokens[name]=newToken();store.createSession(store.findUser(name),tokenHash(tokens[name]),Date.now(),3600000,name==='admin'?'admin':'client');
  }
  store.syncProxmox([vm(710),vm(711),vm(712,'running')],'fixture');
  for(const vmid of [710,711,712]) {
    const code=newToken(); store.issueEnrollment(`pve-${vmid}`,machineUuid(vmid),tokenHash(code),'fixture');
    store.consumeEnrollment(tokenHash(code),machineUuid(vmid),{hostname:'fixture-host',version:'1.0.14',tokenHash:tokenHash(newToken())});
  }
  store.assign('alice','pve-710','fixture');store.assign('alice','pve-712','fixture');store.assign('bobby','pve-711','fixture');
  store.saveSetting('proxmox',vault.seal(config),'fixture');
  const server=await buildServer({store,vault,proxmox:adapter,publicOrigin:origin,logger:{error:value=>assert.fail(value)},...extra});
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  t.after(async()=>{await new Promise(resolve=>server.close(resolve));store.close();});
  async function call(path,{method='GET',as='alice',data}={}) {
    const response=await fetch(`http://127.0.0.1:${server.address().port}/api/v1`+path,{method,headers:{...(as?{Authorization:'Bearer '+tokens[as]}:{}),...(data!==undefined?{'Content-Type':'application/json'}:{})},body:data===undefined?undefined:JSON.stringify(data)});
    return {status:response.status,body:await response.json()};
  }
  async function adminPower(action) {
    const response=await fetch(`http://127.0.0.1:${server.address().port}/api/v1/admin/desktops/pve-712/power`,{method:'POST',headers:{Cookie:'__Secure-multitap-admin='+tokens.admin,Origin:origin,'X-CSRF-Token':vault.csrf(tokens.admin),'Content-Type':'application/json'},body:JSON.stringify({action})});
    return {status:response.status,body:await response.json()};
  }
  return {store,vault,users,tokens,server,call,adminPower};
}
function administratorContext(f) {
  f.tokens.adminClient=newToken();f.store.createSession(f.store.findUser('admin'),tokenHash(f.tokens.adminClient),Date.now(),3600000,'client');
  f.store.assign('admin','pve-712','fixture');
  return {submit:f.adminPower,view:task=>f.call('/desktops/pve-712/power'+(task?'/tasks/'+task:''),{as:'adminClient'})};
}
function adapterFixture() {
  let inventory=[vm(710),vm(711),vm(712,'running')],result={status:'running',exitStatus:null};
  const submissions=[];let reads=0;
  return {submissions,get reads(){return reads;},setInventory(value){inventory=value;},set result(value){result=value;},
    async inventory(_config,{targets}={}){reads++;if(inventory instanceof Error)throw inventory;assert.ok(Array.isArray(targets),'the registered target scope is mandatory');return targets.map(target=>inventory.find(row=>row.vmid===target.vmid)??{...target,status:'unknown',powerError:'proxmox_vm_not_found'});},
    async power(cfg,vmid,action,{beforeSubmit}={}){beforeSubmit?.();submissions.push({vmid,action});return {node:'fixture-node',task:`UPID:fixture-node:1:2:3:qm${action}:${vmid}:fixture@pve:`};},
    async task(){return result;}};
}

test('Assigned resource observations share power inventory, preserve failed samples and isolate integration changes',async t=>{
  let now=Date.now();const adapter=adapterFixture();
  const measured=(id,cpu=.026)=>({...vm(id,'running'),cpuUsage:cpu,vcpus:4,memoryUsedBytes:9*1024**3,memoryBytes:8*1024**3,uptimeSeconds:11520});
  adapter.setInventory([measured(710),measured(711,.8),{...vm(712),uptimeSeconds:300}]);
  const f=await fixture(t,adapter,{powerOptions:{now:()=>now}});
  const replies=await Promise.all(Array.from({length:8},()=>f.call('/desktops')));
  assert.equal(adapter.reads,1);
  const listed=replies[0].body.desktops;
  assert.deepEqual(listed.map(d=>d.id),['pve-710','pve-712']);
  assert.deepEqual(listed[0].resources,{source:'proxmox',checkedAt:new Date(now).toISOString(),cpuUsagePercent:2.6,vcpus:4,memoryUsedBytes:9*1024**3,memoryTotalBytes:8*1024**3,uptimeSeconds:11520,stale:false});
  assert.equal(listed[1].resources.cpuUsagePercent,null);
  // Uptime only while the VM runs: a stopped VM has none, whatever is reported.
  assert.equal(listed[1].resources.uptimeSeconds,null);
  assert.equal(JSON.stringify(replies).includes(config.secret),false);
  assert.equal((await f.call('/desktops/pve-711')).status,404);
  const sample=listed[0].resources;
  now+=5001;adapter.setInventory(new HttpError(502,'proxmox_unreachable'));
  const failed=(await f.call('/desktops')).body.desktops[0].resources;
  assert.deepEqual(failed,{...sample,stale:true});
  // A new Proxmox configuration must never inherit another installation's data.
  f.store.saveSetting('proxmox',f.vault.seal({...config,endpoint:'https://new-pve.example:8006'}),'fixture');
  assert.equal((await f.call('/desktops')).body.desktops[0].resources,null);
  now+=5001;adapter.setInventory([{...measured(710),cpuUsage:NaN,vcpus:-1,memoryUsedBytes:'0',memoryBytes:Infinity,uptimeSeconds:-5},vm(711),vm(712)]);
  const recovered=(await f.call('/desktops')).body.desktops[0];
  assert.equal(recovered.powerStale,false);
  assert.deepEqual(recovered.resources,{source:'proxmox',checkedAt:new Date(now).toISOString(),cpuUsagePercent:null,vcpus:null,memoryUsedBytes:null,memoryTotalBytes:null,uptimeSeconds:null,stale:false});
  for(const uptimeSeconds of [1.5,'60',Infinity,101*366*86400]){
    now+=5001;adapter.setInventory([{...measured(710),uptimeSeconds},vm(711),vm(712)]);
    assert.equal((await f.call('/desktops')).body.desktops[0].resources.uptimeSeconds,null,String(uptimeSeconds));
  }
  const priorMissing=(await f.call('/desktops')).body.desktops[0].resources;
  now+=5001;adapter.setInventory([]);
  const missing=(await f.call('/desktops')).body.desktops[0];
  assert.deepEqual(missing.resources,{...priorMissing,stale:true});
  assert.equal(missing.powerState,'unknown');assert.equal(missing.powerError,'proxmox_vm_not_found');
  assert.equal(f.store.inventory().assignments.length,3);
});

test('Proxmox optional resource values do not coerce missing/string/invalid observations into measured zeroes',async()=>{
  const adapter=new Proxmox();
  const observations=[
    {vmid:710,node:'fixture',type:'qemu',status:'running',maxcpu:4,maxmem:8*1024**3,cpu:.026,mem:5*1024**3,uptime:11520},
    {vmid:711,node:'fixture',type:'qemu',status:'running',cpu:null,mem:null,uptime:null},
    {vmid:712,node:'fixture',type:'qemu',status:'running',cpu:'0',mem:'0',uptime:'0'},
    {vmid:713,node:'fixture',type:'qemu',status:'running',cpu:Infinity,mem:-1,uptime:-1},
    {vmid:714,node:'fixture',type:'qemu',status:'running',cpu:0,mem:0,uptime:0},
  ];
  adapter.call=async(_config,path)=>{const found=/\/qemu\/(\d+)\/status\/current$/.exec(path);assert.ok(found);return observations.find(row=>row.vmid===Number(found[1]));};
  const result=await Promise.all(observations.map(row=>adapter.vmStatus(config,'fixture',row.vmid)));
  assert.equal(result[0].cpuUsage,.026);assert.equal(result[0].memoryUsedBytes,5*1024**3);assert.equal(result[0].uptimeSeconds,11520);
  for(const row of result.slice(1,4)){assert.equal(row.cpuUsage,null);assert.equal(row.memoryUsedBytes,null);assert.equal(row.uptimeSeconds,null);}
  assert.equal(result[4].cpuUsage,0);assert.equal(result[4].memoryUsedBytes,0);assert.equal(result[4].uptimeSeconds,0);
});

test('Assigned users can request only supported VM actions; opaque tasks stay bound to VM and initiating user',async t=>{
  let now=Date.now();const adapter=adapterFixture();const f=await fixture(t,adapter,{powerOptions:{now:()=>now}});
  assert.equal((await f.call('/desktops/pve-710/power',{as:null})).status,401);
  assert.equal((await f.call('/desktops/pve-710/connect',{method:'POST',data:{}})).body.error.details.reason,'host_offline');
  assert.equal((await f.call('/desktops/pve-711/power',{method:'POST',data:{action:'start'}})).status,404);
  for(const action of ['reset','destroy'])assert.equal((await f.call('/desktops/pve-710/power',{method:'POST',data:{action}})).status,400);
  // Stop is a client action, but only for a running VM.
  assert.equal((await f.call('/desktops/pve-710/power',{method:'POST',data:{action:'stop'}})).body.error.code,'invalid_power_state');
  assert.equal((await f.call('/desktops/pve-710/power',{method:'POST',data:{action:'start',node:'arbitrary'}})).status,400);
  assert.equal((await f.call('/desktops/pve-712/power',{method:'POST',data:{action:'start'}})).body.error.code,'invalid_power_state');
  const started=await f.call('/desktops/pve-710/power',{method:'POST',data:{action:'start'}});
  assert.equal(started.status,202);assert.match(started.body.powerTask.id,/^[0-9a-f-]{36}$/);assert.equal(started.body.powerTask.status,'submitted');
  assert.equal(started.body.powerBusy,true);assert.equal(JSON.stringify(started.body).includes('UPID:'),false);assert.equal(JSON.stringify(started.body).includes('fixture-node'),false);assert.equal(JSON.stringify(started.body).includes(config.secret),false);
  assert.equal((await f.call('/desktops/pve-710/power',{method:'POST',data:{action:'start'}})).body.error.code,'power_in_progress');
  const adminResponse=await fetch(`http://127.0.0.1:${f.server.address().port}/api/v1/admin/desktops/pve-710/power`,{method:'POST',headers:{Cookie:'__Secure-multitap-admin='+f.tokens.admin,Origin:origin,'X-CSRF-Token':f.vault.csrf(f.tokens.admin),'Content-Type':'application/json'},body:JSON.stringify({action:'start'})});
  assert.equal(adminResponse.status,409);assert.equal((await adminResponse.json()).error.code,'power_in_progress');
  f.store.assign('bobby','pve-710','fixture');
  const other=await f.call('/desktops/pve-710/power',{as:'bobby'});assert.equal(other.body.powerTask,null);assert.equal(other.body.powerBusy,true);
  const taskPath='/desktops/pve-710/power/tasks/'+started.body.powerTask.id;
  assert.equal((await f.call(taskPath,{as:'bobby'})).status,404);
  assert.equal((await f.call('/desktops/pve-712/power/tasks/'+started.body.powerTask.id)).status,404);
  now+=1100;adapter.result={status:'stopped',exitStatus:'OK'};adapter.setInventory([vm(710,'running'),vm(711),vm(712,'running')]);
  const finished=await f.call(taskPath);assert.equal(finished.body.powerTask.status,'stopped');assert.equal(finished.body.powerTask.exitStatus,'OK');assert.equal(finished.body.power.state,'running');assert.equal(finished.body.powerBusy,false);
  assert.deepEqual(adapter.submissions,[{vmid:710,action:'start'}]);
  f.store.revoke('alice','pve-710','fixture');assert.equal((await f.call(taskPath)).status,404);
});

test('A finished start whose result the lagging Proxmox snapshot does not show yet keeps the VM busy instead of allowing a second start',async t=>{
  // Otherwise a second start seconds after the first one finished reaches a
  // running VM (Proxmox answers "already running") and the client shows power_task_failed.
  let now=Date.now();const adapter=adapterFixture();const f=await fixture(t,adapter,{powerOptions:{now:()=>now}});
  const started=await f.call('/desktops/pve-710/power',{method:'POST',data:{action:'start'}});assert.equal(started.status,202);
  now+=1100;adapter.result={status:'stopped',exitStatus:'OK'};
  const finished=await f.call('/desktops/pve-710/power/tasks/'+started.body.powerTask.id);
  assert.equal(finished.body.powerTask.exitStatus,'OK');assert.equal(finished.body.power.state,'stopped');assert.equal(finished.body.powerBusy,true);
  now+=5000;
  const listed=(await f.call('/desktops')).body.desktops.find(d=>d.id==='pve-710');assert.equal(listed.powerState,'stopped');assert.equal(listed.powerBusy,true);
  f.store.assign('bobby','pve-710','fixture');
  assert.equal((await f.call('/desktops/pve-710/power',{as:'bobby'})).body.powerBusy,true,'Settling is the VM state, not the initiating account');
  assert.equal((await f.call('/desktops/pve-710/power',{method:'POST',data:{action:'start'}})).body.error.code,'power_in_progress');
  assert.deepEqual(adapter.submissions,[{vmid:710,action:'start'}]);
  // The snapshot catches up: the VM is idle and running, so start is refused as such.
  now+=2001;adapter.setInventory([vm(710,'running'),vm(711),vm(712,'running')]);
  const settled=await f.call('/desktops/pve-710/power');assert.equal(settled.body.power.state,'running');assert.equal(settled.body.powerBusy,false);
  assert.equal((await f.call('/desktops/pve-710/power',{method:'POST',data:{action:'start'}})).body.error.code,'invalid_power_state');
  // A start that never shows (the VM stopped again) is not held forever.
  now+=2001;adapter.setInventory([vm(710),vm(711),vm(712,'running')]);
  assert.equal((await f.call('/desktops/pve-710/power')).body.powerBusy,true);
  now+=20000;
  assert.equal((await f.call('/desktops/pve-710/power')).body.powerBusy,false);
  assert.equal((await f.call('/desktops/pve-710/power',{method:'POST',data:{action:'start'}})).status,202);
  assert.deepEqual(adapter.submissions,[{vmid:710,action:'start'},{vmid:710,action:'start'}]);
  // A failed task does not hold the VM.
  now+=1100;adapter.result={status:'stopped',exitStatus:'VM 710 already running'};
  assert.equal((await f.call('/desktops/pve-710/power')).body.powerBusy,false);
});

test('Power snapshots use a bounded shared cache and label failed or missing observations without deleting assignments',async t=>{
  let now=Date.now();const adapter=adapterFixture();const f=await fixture(t,adapter,{powerOptions:{now:()=>now}});
  const results=await Promise.all(Array.from({length:8},()=>f.call('/desktops')));assert.equal(adapter.reads,1);
  assert.equal(results[0].body.desktops[0].powerState,'stopped');assert.equal(results[0].body.desktops[0].powerStale,false);
  now+=5001;adapter.setInventory([vm(710,'running'),vm(711),vm(712)]);
  const updated=await f.call('/desktops/pve-710');assert.equal(updated.body.desktop.powerState,'running');const checked=updated.body.desktop.powerCheckedAt;
  now+=5001;adapter.setInventory(new HttpError(502,'proxmox_unreachable'));
  const failed=await f.call('/desktops/pve-710/power');assert.equal(failed.body.power.state,'running');assert.equal(failed.body.power.stale,true);assert.equal(failed.body.power.checkedAt,checked);assert.equal(failed.body.power.error,'proxmox_unreachable');
  assert.equal((await f.call('/desktops/pve-710/power',{method:'POST',data:{action:'shutdown'}})).body.error.code,'power_status_unavailable');
  now+=5001;adapter.setInventory([]);
  const missing=await f.call('/desktops/pve-710/power');assert.equal(missing.body.power.state,'unknown');assert.equal(missing.body.power.error,'proxmox_vm_not_found');
  assert.equal(f.store.inventory().assignments.length,3);assert.equal(adapter.submissions.length,0);
});

test('A lost submission response keeps a bounded duplicate guard and never reports a successful task',async t=>{
  let now=Date.now();const adapter=adapterFixture();adapter.power=async()=>{throw new HttpError(502,'proxmox_unreachable');};
  const f=await fixture(t,adapter,{powerOptions:{now:()=>now}});
  assert.equal((await f.call('/desktops/pve-710/power',{method:'POST',data:{action:'start'}})).status,502);
  const unknown=await f.call('/desktops/pve-710/power');assert.equal(unknown.body.powerTask.status,'unknown');assert.equal(unknown.body.powerBusy,true);assert.equal(unknown.body.powerTask.exitStatus,null);
  assert.equal((await f.call('/desktops/pve-710/power',{method:'POST',data:{action:'start'}})).body.error.code,'power_in_progress');
  now+=30001;const expired=await f.call('/desktops/pve-710/power');assert.equal(expired.body.powerBusy,false);assert.equal(expired.body.powerTask.status,'unknown');
});

test('Proxmox HA worker IDs are accepted only for the requested action and exact VM',async t=>{
  const adapter=adapterFixture();adapter.power=async()=>({node:'fixture-node',task:'UPID:fixture-node:1:2:3:hastart:710:fixture@pve:'});
  const f=await fixture(t,adapter);
  assert.equal((await f.call('/desktops/pve-710/power',{method:'POST',data:{action:'start'}})).status,202);
  adapter.power=async()=>({node:'fixture-node',task:'UPID:fixture-node:1:2:3:hastop:999:fixture@pve:'});
  assert.equal((await f.call('/desktops/pve-712/power',{method:'POST',data:{action:'shutdown'}})).body.error.code,'proxmox_invalid_response');
});

test('Assigned users may stop (정지) a running VM; reset, suspension and other actions stay refused',async t=>{
  // 종료 and 정지 like Proxmox: stop is not administrator-only.
  let now=Date.now();const adapter=adapterFixture();const f=await fixture(t,adapter,{powerOptions:{now:()=>now}});
  const listing=await f.call('/desktops');
  assert.equal(listing.body.desktops.find(d=>d.id==='pve-712').powerCanForceStop,true,'A running idle VM can be stopped');
  assert.equal(listing.body.desktops.find(d=>d.id==='pve-710').powerCanForceStop,false,'A stopped VM cannot');
  assert.equal((await f.call('/desktops/pve-712/power',{method:'POST',as:null,data:{action:'stop'}})).status,401);
  assert.equal((await f.call('/desktops/pve-711/power',{method:'POST',data:{action:'stop'}})).status,404,'Not assigned to alice');
  assert.equal((await f.call('/desktops/pve-712/power',{method:'POST',as:'bobby',data:{action:'stop'}})).status,404,'Not assigned to bobby');
  assert.equal((await f.call('/desktops/pve-712/power',{method:'POST',data:{action:'stop',skiplock:true}})).status,400);
  for(const action of ['reset','suspend','resume','pause','destroy','Stop','stop?skiplock=1'])assert.equal((await f.call('/desktops/pve-712/power',{method:'POST',data:{action}})).body.error.code,'invalid_power_action',action);
  assert.equal((await f.call('/desktops/pve-712/power',{method:'POST',data:{action:'stop',legacyAdmin:true}})).status,400);
  const privatePower=new PowerControl({store:f.store,vault:f.vault,proxmox:adapter});let authorizationCalls=0;
  await assert.rejects(privatePower.submit(()=>{authorizationCalls++;return f.store.assignedDesktop(f.users.alice,'pve-712');},f.users.alice,'reset'),error=>error.code==='invalid_power_action');
  assert.equal(authorizationCalls,0,'An unsupported action is refused before authorization');
  assert.equal(adapter.submissions.length,0);assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM power_tasks').get().n,0);
  assert.equal(f.store.db.prepare("SELECT count(*) AS n FROM audit_events WHERE event='power_refused' AND subject='pve-712:reset:invalid_power_action'").get().n,1);
  const stopped=await f.call('/desktops/pve-712/power',{method:'POST',data:{action:'stop'}});
  assert.equal(stopped.status,202);assert.equal(stopped.body.powerTask.action,'stop');assert.equal(stopped.body.powerBusy,true);assert.equal(stopped.body.canForceStop,false);
  for(const secret of [config.secret,'UPID:','fixture-node'])assert.equal(JSON.stringify(stopped.body).includes(secret),false,secret);
  assert.equal((await f.call('/desktops/pve-712/power',{method:'POST',data:{action:'stop'}})).body.error.code,'power_in_progress','A stop does not replace a stop');
  now+=1100;adapter.result={status:'stopped',exitStatus:'OK'};adapter.setInventory([vm(710),vm(711),vm(712)]);
  const done=await f.call('/desktops/pve-712/power/tasks/'+stopped.body.powerTask.id);
  assert.equal(done.body.powerTask.exitStatus,'OK');assert.equal(done.body.power.state,'stopped');assert.equal(done.body.canForceStop,false);
  assert.deepEqual(adapter.submissions,[{vmid:712,action:'stop'}]);
  assert.equal(f.store.db.prepare("SELECT count(*) AS n FROM audit_events WHERE event='power_submitted' AND actor=? AND subject='pve-712:stop'").get(f.users.alice).n,1);
  assert.equal(f.store.db.prepare('SELECT action,user_id FROM power_tasks WHERE id=?').get(stopped.body.powerTask.id).user_id,f.users.alice);
});

test('The authenticated admin route still stops a VM; an administrator\'s client session sees the same assigned-VM stop permission',async t=>{
  let now=Date.now();const adapter=adapterFixture();const f=await fixture(t,adapter,{powerOptions:{now:()=>now}});
  const admin=administratorContext(f);
  assert.equal((await admin.view()).body.canForceStop,true);
  const stopped=await admin.submit('stop');
  assert.equal(stopped.status,202);assert.equal(stopped.body.powerTask.action,'stop');assert.equal(stopped.body.powerBusy,true);assert.equal(stopped.body.canForceStop,false);
  assert.equal(JSON.stringify(stopped.body).includes(config.secret),false);
  now+=1100;adapter.result={status:'stopped',exitStatus:'OK'};adapter.setInventory([vm(710),vm(711),vm(712)]);
  const done=await admin.view(stopped.body.powerTask.id);
  assert.equal(JSON.stringify(done.body).includes('UPID:'),false);
  assert.equal(done.body.powerTask.exitStatus,'OK');assert.equal(done.body.power.state,'stopped');assert.equal(done.body.canForceStop,false);
  assert.deepEqual(adapter.submissions,[{vmid:712,action:'stop'}]);
  assert.equal(f.store.db.prepare("SELECT count(*) AS n FROM audit_events WHERE event='power_submitted' AND actor=? AND subject='pve-712:stop'").get(f.users.admin).n,1);
});

test('An assigned user replaces its own stuck shutdown (종료) with stop (정지) atomically; another account cannot',async t=>{
  let now=Date.now();const adapter=adapterFixture(),options=[];const original=adapter.power;
  adapter.power=async(cfg,id,action,opt)=>{options.push({action,overrule:opt.overruleShutdown});return original(cfg,id,action,opt);};
  const f=await fixture(t,adapter,{powerOptions:{now:()=>now}});
  f.store.assign('bobby','pve-712','fixture');
  const shutdown=await f.call('/desktops/pve-712/power',{method:'POST',data:{action:'shutdown'}});
  assert.equal(shutdown.status,202);assert.equal(shutdown.body.powerBusy,true);assert.equal(shutdown.body.canForceStop,true,'Its own qmshutdown may be replaced');
  assert.equal((await f.call('/desktops')).body.desktops.find(d=>d.id==='pve-712').powerCanForceStop,true);
  const other=await f.call('/desktops/pve-712/power',{as:'bobby'});
  assert.equal(other.body.powerTask,null);assert.equal(other.body.powerBusy,true);assert.equal(other.body.canForceStop,false);
  assert.equal((await f.call('/desktops/pve-712/power',{method:'POST',as:'bobby',data:{action:'stop'}})).body.error.code,'power_in_progress','Never another account\'s shutdown');
  assert.equal((await f.call('/desktops/pve-712/power',{method:'POST',data:{action:'reboot'}})).body.error.code,'power_in_progress','Only stop replaces a shutdown');
  now+=1100;
  const stop=await f.call('/desktops/pve-712/power',{method:'POST',data:{action:'stop'}});
  assert.equal(stop.status,202);assert.equal(stop.body.powerTask.action,'stop');assert.equal(stop.body.canForceStop,false);
  assert.deepEqual(options,[{action:'shutdown',overrule:false},{action:'stop',overrule:true}]);
  const previous=f.store.db.prepare('SELECT * FROM power_tasks WHERE id=?').get(shutdown.body.powerTask.id);
  assert.equal(previous.status,'superseded');assert.equal(previous.active,0);assert.equal(previous.exit_status,null);
  assert.equal((await f.call('/desktops/pve-712/power/tasks/'+previous.id)).body.powerTask.status,'superseded');
  assert.equal(f.store.db.prepare("SELECT count(*) AS n FROM audit_events WHERE event='power_superseded' AND actor=?").get(f.users.alice).n,1);
  assert.equal(f.store.db.prepare("SELECT count(*) AS n FROM audit_events WHERE event='power_submitted' AND actor=? AND subject='pve-712:stop'").get(f.users.alice).n,1);
});

test('Administrator can atomically replace its own confirmed qmshutdown while another assigned account cannot',async t=>{
  const adapter=adapterFixture(),options=[];const original=adapter.power;
  adapter.power=async(cfg,id,action,opt)=>{options.push({action,overrule:opt.overruleShutdown});return original(cfg,id,action,opt);};
  const f=await fixture(t,adapter),admin=administratorContext(f);
  const shutdown=await admin.submit('shutdown');
  assert.equal(shutdown.status,202);assert.equal(shutdown.body.powerBusy,true);assert.equal(shutdown.body.canForceStop,true);
  f.store.assign('bobby','pve-712','fixture');
  const other=await f.call('/desktops/pve-712/power',{as:'bobby'});
  assert.equal(other.body.powerTask,null);assert.equal(other.body.powerBusy,true);assert.equal(other.body.canForceStop,false);
  assert.equal((await f.call('/desktops/pve-712/power',{method:'POST',as:'bobby',data:{action:'stop'}})).body.error.code,'power_in_progress');
  const responses=await Promise.all([1,2].map(()=>admin.submit('stop')));
  assert.deepEqual(responses.map(r=>r.status).sort(),[202,409]);
  const stop=responses.find(r=>r.status===202);
  assert.notEqual(stop.body.powerTask.id,shutdown.body.powerTask.id);assert.equal(stop.body.canForceStop,false);
  assert.deepEqual(options,[{action:'shutdown',overrule:false},{action:'stop',overrule:true}]);
  const previous=f.store.db.prepare('SELECT * FROM power_tasks WHERE id=?').get(shutdown.body.powerTask.id);
  assert.equal(previous.status,'superseded');assert.equal(previous.active,0);assert.equal(previous.exit_status,null);
  assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM power_tasks WHERE desktop_id=? AND active=1').get('pve-712').n,1);
  assert.equal((await admin.view(previous.id)).body.powerTask.status,'superseded');
  assert.equal((await f.call('/desktops/pve-712/power/tasks/'+previous.id,{as:'bobby'})).status,404);
  assert.equal(f.store.db.prepare("SELECT count(*) AS n FROM audit_events WHERE event='power_superseded' AND actor=?").get(f.users.admin).n,1);
});

test('A rejected force stop restores the shutdown guard; an uncertain response keeps an unknown stop guard',async t=>{
  for(const errorCode of ['proxmox_permission_denied','proxmox_unreachable','proxmox_invalid_response'])await t.test(errorCode,async sub=>{
    let now=Date.now();const adapter=adapterFixture(),original=adapter.power;
    adapter.power=async(cfg,id,action,opt)=>{if(action==='stop'){opt.beforeSubmit();throw new HttpError(502,errorCode);}return original(cfg,id,action,opt);};
    const f=await fixture(sub,adapter,{powerOptions:{now:()=>now}}),admin=administratorContext(f);
    const shutdown=await admin.submit('shutdown');
    assert.equal((await admin.submit('stop')).body.error.code,errorCode);
    const old=f.store.db.prepare('SELECT * FROM power_tasks WHERE id=?').get(shutdown.body.powerTask.id);
    const view=await admin.view();assert.equal(view.body.powerBusy,true);
    if(errorCode==='proxmox_permission_denied') {
      // The restored shutdown is the same account's again: stop may be retried.
      assert.equal(old.active,1);assert.equal(old.status,'submitted');assert.equal(view.body.canForceStop,true);
      assert.equal(view.body.powerTask.exitStatus,'request_rejected');
    } else {
      assert.equal(old.active,0);assert.equal(old.status,'unknown');assert.equal(old.exit_status,null);
      assert.equal(view.body.powerTask.status,'unknown');assert.equal(view.body.powerTask.exitStatus,null);assert.equal(view.body.canForceStop,false);
      assert.equal((await admin.submit('stop')).body.error.code,'power_in_progress');
      now+=30001;const expired=await admin.view();assert.equal(expired.body.powerBusy,false);assert.equal(expired.body.powerTask.status,'unknown');
    }
    assert.equal(f.store.db.prepare("SELECT count(*) AS n FROM audit_events WHERE event='power_superseded'").get().n,0);
  });
});

test('Force stop cannot bypass start, reboot, HA shutdown, unknown, unsubmitted, wrong-VM or changed-configuration guards',async t=>{
  for(const scenario of ['start','reboot','ha','unknown','unsubmitted','wrong-vm','changed-config'])await t.test(scenario,async sub=>{
    const adapter=adapterFixture();const f=await fixture(sub,adapter),admin=administratorContext(f);
    const shutdown=await admin.submit('shutdown');
    const id=shutdown.body.powerTask.id;
    if(scenario==='start'||scenario==='reboot')f.store.db.prepare('UPDATE power_tasks SET action=?,pve_task=? WHERE id=?').run(scenario,`UPID:fixture-node:1:2:3:qm${scenario}:712:fixture@pve:`,id);
    if(scenario==='ha')f.store.db.prepare("UPDATE power_tasks SET pve_task='UPID:fixture-node:1:2:3:hastop:712:fixture@pve:' WHERE id=?").run(id);
    if(scenario==='unknown')f.store.db.prepare("UPDATE power_tasks SET status='unknown' WHERE id=?").run(id);
    if(scenario==='unsubmitted')f.store.db.prepare('UPDATE power_tasks SET pve_task=NULL WHERE id=?').run(id);
    if(scenario==='wrong-vm')f.store.db.prepare("UPDATE power_tasks SET pve_task='UPID:fixture-node:1:2:3:qmshutdown:999:fixture@pve:' WHERE id=?").run(id);
    if(scenario==='changed-config')f.store.saveSetting('proxmox',f.vault.seal({...config,endpoint:'https://other-pve.example:8006'}),'fixture');
    assert.equal((await f.call('/desktops/pve-712/power')).body.canForceStop,false);
    assert.equal((await admin.submit('stop')).body.error.code,'power_in_progress');
    assert.deepEqual(adapter.submissions,[{vmid:712,action:'shutdown'}]);
  });
});

test('Force-stop UPIDs must be qmstop or hastop for the exact VM and node, including the legacy admin route',async t=>{
  for(const kind of ['qmstop','hastop','qmshutdown','qmreset','wrong-vm','wrong-node'])await t.test(kind,async sub=>{
    const adapter=adapterFixture();
    adapter.power=async(cfg,id,action,{beforeSubmit})=>{beforeSubmit();return {node:'fixture-node',task:`UPID:${kind==='wrong-node'?'other-node':'fixture-node'}:1:2:3:${['wrong-vm','wrong-node'].includes(kind)?'qmstop':kind}:${kind==='wrong-vm'?999:id}:fixture@pve:`};};
    const f=await fixture(sub,adapter);
    const response=await fetch(`http://127.0.0.1:${f.server.address().port}/api/v1/admin/desktops/pve-712/power`,{method:'POST',headers:{Cookie:'__Secure-multitap-admin='+f.tokens.admin,Origin:origin,'X-CSRF-Token':f.vault.csrf(f.tokens.admin),'Content-Type':'application/json'},body:JSON.stringify({action:'stop'})});
    const body=await response.json();
    if(['qmstop','hastop'].includes(kind)){assert.equal(response.status,202);assert.equal(body.action,'stop');assert.equal(body.powerTask.action,'stop');}
    else{assert.equal(response.status,502);assert.equal(body.error.code,'proxmox_invalid_response');assert.equal(f.store.db.prepare('SELECT status,active FROM power_tasks').get().status,'unknown');}
  });
});

test('Administrator session revocation immediately before stop restores the old guard and never submits the stop',async t=>{
  let release,reached;const gate=new Promise(resolve=>release=resolve),began=new Promise(resolve=>reached=resolve);
  const adapter=adapterFixture(),original=adapter.power;let forcePosts=0;
  adapter.power=async(cfg,id,action,opt)=>{if(action==='stop'){reached();await gate;opt.beforeSubmit();forcePosts++;}return original(cfg,id,action,opt);};
  const f=await fixture(t,adapter),admin=administratorContext(f);
  const shutdown=await admin.submit('shutdown');
  const stop=admin.submit('stop');await began;
  f.store.db.prepare('UPDATE users SET disabled=1 WHERE id=?').run(f.users.admin);release();assert.equal((await stop).status,401);assert.equal(forcePosts,0);
  const previous=f.store.db.prepare('SELECT * FROM power_tasks WHERE id=?').get(shutdown.body.powerTask.id);
  assert.equal(previous.active,1);assert.equal(previous.status,'submitted');
});

test('A late shutdown poll cannot reactivate its guard after the force-stop reservation',async t=>{
  let now=Date.now();const adapter=adapterFixture();const f=await fixture(t,adapter);
  const power=new PowerControl({store:f.store,vault:f.vault,proxmox:adapter,now:()=>now,cacheMs:5});
  administratorContext(f);
  const authorize=()=>{const desktop=f.store.assignedDesktop(f.users.admin,'pve-712');if(!desktop)throw new HttpError(404,'desktop_not_found');return desktop;};
  const shutdown=await power.submit(authorize,f.users.admin,'shutdown',{legacyAdmin:true});
  now+=6;
  let releaseInventory,reachedInventory,releasePoll,reachedPoll,releaseStop,reachedStop;
  const inventoryGate=new Promise(r=>releaseInventory=r),inventoryBegan=new Promise(r=>reachedInventory=r);
  const pollGate=new Promise(r=>releasePoll=r),pollBegan=new Promise(r=>reachedPoll=r);
  const stopGate=new Promise(r=>releaseStop=r),stopBegan=new Promise(r=>reachedStop=r);
  const originalInventory=adapter.inventory,originalPower=adapter.power;
  adapter.inventory=async(config,options)=>{reachedInventory();await inventoryGate;return originalInventory(config,options);};
  adapter.power=async(cfg,id,action,opt)=>{reachedStop();await stopGate;return originalPower(cfg,id,action,opt);};
  const stopping=power.submit(authorize,f.users.admin,'stop',{legacyAdmin:true});await inventoryBegan;
  now+=1100;adapter.task=async()=>{reachedPoll();await pollGate;return {status:'running',exitStatus:null};};
  const latePoll=power.poll(power.active('pve-712'));await pollBegan;
  releaseInventory();await stopBegan;
  releasePoll();const late=await latePoll;assert.equal(late.active,0);assert.equal(late.id,shutdown.powerTask.id);
  assert.equal(power.active('pve-712').action,'stop');
  releaseStop();assert.equal((await stopping).powerTask.action,'stop');
  assert.equal(f.store.db.prepare('SELECT status FROM power_tasks WHERE id=?').get(shutdown.powerTask.id).status,'superseded');
});

test('Actual verified TLS Proxmox transport rechecks assignment immediately before the power POST',async t=>{
  if(spawnSync('openssl',['version']).error?.code==='ENOENT'){t.skip('OpenSSL unavailable; mandatory Linux QA exercises verified TLS.');return;}
  const directory=await mkdtemp(join(tmpdir(),'multitap-client-power-'));t.after(()=>rm(directory,{recursive:true}));
  const generated=spawnSync('openssl',['req','-x509','-newkey','rsa:2048','-nodes','-keyout',join(directory,'key.pem'),'-out',join(directory,'cert.pem'),'-days','1','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost,IP:127.0.0.1'],{encoding:'utf8'});assert.equal(generated.status,0);
  const cert=await readFile(join(directory,'cert.pem')),key=await readFile(join(directory,'key.pem'));let targetReads=0,powerPosts=0,release,started;
  const reached=new Promise(resolve=>started=resolve),pending=new Promise(resolve=>release=resolve);
  const tls=createServer({cert,key},async(req,res)=>{
    assert.equal(req.headers.authorization,'PVEAPIToken=fixture@pve!power=power-fixture-secret');
    assert.notEqual(req.url,'/api2/json/cluster/resources?type=vm');
    const target=/^\/api2\/json\/nodes\/fixture-node\/qemu\/(710|711|712)\/(config\?current=1|status\/current)$/.exec(req.url);
    if(target){
      const vmid=Number(target[1]);
      if(target[2]==='config?current=1'){res.end(JSON.stringify({data:{smbios1:'uuid='+machineUuid(vmid)}}));return;}
      if(vmid===710&&++targetReads===2){started();await pending;}
      res.end(JSON.stringify({data:{status:vmid===712?'running':'stopped'}}));return;
    }
    if(req.method==='POST'){powerPosts++;res.end(JSON.stringify({data:'UPID:fixture-node:1:2:3:qmstart:710:fixture@pve:'}));return;}
    res.writeHead(404);res.end('{}');
  });
  await new Promise(resolve=>tls.listen(0,'127.0.0.1',resolve));t.after(()=>new Promise(resolve=>tls.close(resolve)));
  const f=await fixture(t,new Proxmox());f.store.saveSetting('proxmox',f.vault.seal({...config,endpoint:`https://127.0.0.1:${tls.address().port}`,ca:cert.toString()}),'fixture');
  const request=f.call('/desktops/pve-710/power',{method:'POST',data:{action:'start'}});await reached;
  f.store.revoke('alice','pve-710','fixture');release();assert.equal((await request).status,404);assert.equal(powerPosts,0);
});
