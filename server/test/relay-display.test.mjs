import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';

const origin = 'https://display-fixture.example';
const uuid = '00000000-1234-1234-1234-000000000001';
const caps = (count = 2, control = true) => ({ service:true,interactiveSession:true,streaming:true,virtualDisplayReady:true,virtualMonitorControl:control,
  monitors:Array.from({length:count},(_,id)=>({id:String(id),width:1920,height:1080,primary:id===0})) });
async function fixture(t, relayOptions = {}) {
  const store=new Store(':memory:'),vault=new Vault(randomBytes(32)),tokens={},users={};
  for(const name of ['admin','alice','bobby']) {
    users[name]=store.addUser({name,passwordHash:'display-test-only',role:name==='admin'?'admin':'user'});
    tokens[name]=newToken();store.createSession(store.findUser(name),tokenHash(tokens[name]),Date.now(),3600000,'client');
  }
  const vms=[610,611].map(vmid=>({vmid,node:'fixture',name:'VM '+vmid,status:'running',machineUuid:uuid}));
  store.syncProxmox(vms,'fixture');store.assign('alice','pve-610');store.assign('bobby','pve-611');
  store.saveSetting('proxmox',vault.seal({endpoint:'https://pve-fixture.example',tokenId:'test@pve!fixture',secret:'isolated-fixture-only'}),'fixture');
  const code=newToken(),hostToken=newToken();store.issueEnrollment('pve-610',uuid,tokenHash(code),'fixture');
  store.consumeEnrollment(tokenHash(code),uuid,{hostname:'Display fixture',version:'0.2.2',tokenHash:tokenHash(hostToken)});
  const powerPosts=[];
  const proxmox={inventory:async(_cfg,{targets})=>targets.map(target=>{assert.deepEqual(target,{node:'fixture',vmid:610,expectedUuid:uuid});return vms.find(vm=>vm.vmid===target.vmid);}),power:async(_cfg,vmid,action,{beforeSubmit,node,expectedUuid}={})=>{assert.equal(node,'fixture');assert.equal(expectedUuid,uuid);beforeSubmit?.();powerPosts.push({vmid,action});return{node:'fixture',task:`UPID:fixture:1:2:3:qm${action}:${vmid}:fixture@pve:`};},task:async()=>({status:'running',exitStatus:null})};
  const server=await buildServer({store,vault,proxmox,publicOrigin:origin,relayOptions:{authorizationMs:10,heartbeatMs:1000,...relayOptions},logger:{error:value=>assert.fail(value)}});
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  const base=`http://127.0.0.1:${server.address().port}`;
  t.after(async()=>{await new Promise(resolve=>server.close(resolve));store.close();});
  async function call(path,{method='GET',as='alice',data}={}) {
    const response=await fetch(base+'/api/v1'+path,{method,headers:{...(as?{Authorization:'Bearer '+tokens[as]}:{}),...(data===undefined?{}:{'Content-Type':'application/json'})},body:data===undefined?undefined:JSON.stringify(data)});
    return{status:response.status,body:await response.json()};
  }
  const wsUrl=path=>base.replace('http:','ws:')+path;
  async function host(capabilities=caps()) {
    const ws=await socket(wsUrl('/api/v1/hosts/channel'),'Host '+hostToken);
    if(capabilities){ws.json({type:'host.capabilities',capabilities});assert.equal((await ws.take(v=>v.type==='host.capabilities.ack')).accepted,true);}
    return ws;
  }
  async function controller(as='alice') {
    const response=await call('/desktops/pve-610/connect',{as,method:'POST',data:{mode:'stream'}});assert.equal(response.status,200);
    return socket(wsUrl('/api/v1/sessions/'+response.body.sessionId),'Bearer '+tokens[as]);
  }
  return{store,server,call,host,controller,tokens,users,powerPosts};
}
async function socket(url,authorization) {
  const ws=new WebSocket(url,{headers:{Authorization:authorization},perMessageDeflate:false});
  await once(ws,'open');
  const queue=[],waiting=[];
  ws.on('message',(data,binary)=>{
    const value=binary?data:JSON.parse(data.toString());const index=waiting.findIndex(w=>w.predicate(value));
    if(index<0)queue.push(value);else{const w=waiting.splice(index,1)[0];clearTimeout(w.timer);w.resolve(value);}
  });
  ws.json=value=>ws.send(JSON.stringify(value));
  ws.take=(predicate=()=>true)=>{
    const index=queue.findIndex(predicate);if(index>=0)return Promise.resolve(queue.splice(index,1)[0]);
    return new Promise((resolve,reject)=>{const w={predicate,resolve,timer:setTimeout(()=>reject(new Error('message_timeout')),2000)};waiting.push(w);});
  };
  return ws;
}
const path='/desktops/pve-610/display';
const post=(f,count=1,as='alice')=>f.call(path,{method:'POST',as,data:{count}});
const result=(host,request,overrides={})=>host.json({type:'host.display.result',operationId:request.operationId,nonce:request.nonce,ok:true,count:request.count,capabilities:caps(request.count),...overrides});
async function completed(f,id) {
  for(let i=0;i<40;i++){const response=await f.call(path+'/operations/'+id);if(response.body.display?.operation.status!=='pending')return response;await new Promise(resolve=>setTimeout(resolve,5));}
  assert.fail('Operation did not finish');
}

test('Virtual-monitor control requires explicit host capability and exact assigned VM; legacy host stays unsupported',async t=>{
  const f=await fixture(t),host=await f.host(null);
  assert.equal((await f.call(path,{as:null})).status,401);
  assert.equal((await f.call(path,{as:'bobby'})).status,404);
  assert.equal((await post(f)).body.error.code,'display_control_unavailable');
  assert.equal((await f.call(path)).body.display.supported,false);
  for(const count of [0,3,true,'1',null])assert.equal((await post(f,count)).body.error.code,'invalid_display_count');
  assert.equal((await f.call(path,{method:'DELETE'})).status,405);
  host.json({type:'host.capabilities',capabilities:caps(2,'true')});assert.equal((await host.take(v=>v.type==='host.capabilities.ack')).accepted,false);
  host.json({type:'host.capabilities',capabilities:{...caps(),virtualDisplayReady:false}});assert.equal((await host.take(v=>v.type==='host.capabilities.ack')).accepted,false);
  assert.equal((await f.call('/desktops/pve-610')).body.desktop.display.supported,false);
});

test('Actual WSS count operation resets the same user stream, blocks overlap, and succeeds only after correlated fresh topology',async t=>{
  const f=await fixture(t),host=await f.host(),client=await f.controller();
  await host.take(v=>v.type==='session.start');
  const response=await post(f);assert.equal(response.status,202);
  const operation=response.body.display.operation;assert.equal(operation.status,'pending');assert.equal(response.body.display.busy,true);assert.equal(response.body.display.stale,true);
  assert.equal((await host.take()).type,'input.reset');assert.equal((await host.take()).reason,'display_reconfigure');
  const request=await host.take(v=>v.type==='host.display.configure');assert.equal(request.operationId,operation.id);assert.match(request.nonce,/^[A-Za-z0-9_-]{43}$/);
  assert.equal((await client.take(v=>v.type==='session.ended')).reason,'display_reconfigure');
  assert.equal((await post(f)).body.error.code,'display_in_progress');
  assert.equal((await f.call('/desktops/pve-610/connect',{method:'POST',data:{}})).body.error.code,'display_in_progress');
  assert.equal((await f.call('/desktops/pve-610/power',{method:'POST',data:{action:'shutdown'}})).body.error.code,'display_in_progress');assert.equal(f.powerPosts.length,0);
  f.store.assign('bobby','pve-610');
  assert.equal((await f.call(path+'/operations/'+operation.id,{as:'bobby'})).status,404);
  assert.equal((await f.call(path,{as:'bobby'})).body.display.operation,null);
  assert.equal((await f.call(path,{as:'bobby'})).body.display.busy,true);
  result(host,request,{nonce:newToken()});host.json({type:'host.heartbeat'});await host.take(v=>v.type==='host.heartbeat.ack');
  assert.equal((await f.call(path)).body.display.operation.status,'pending');
  result(host,request);const final=await completed(f,operation.id);assert.equal(final.body.display.operation.status,'succeeded');assert.equal(final.body.display.activeCount,1);assert.equal(final.body.display.stale,false);assert.equal(final.body.display.busy,false);
  const desktop=(await f.call('/desktops/pve-610')).body.desktop;assert.equal(desktop.display.activeCount,1);assert.equal(desktop.display.operation.id,operation.id);assert.equal(desktop.connectionModes.stream,true);
  const serialized=JSON.stringify(desktop);assert.equal(serialized.includes(request.nonce),false);assert.equal(serialized.includes('token_hash'),false);assert.equal(serialized.includes(uuid),false);
});

test('Another user controller prevents monitor reconfiguration without stealing the session; RDP is refused',async t=>{
  const f=await fixture(t),host=await f.host();f.store.assign('bobby','pve-610');
  const other=await f.controller('bobby');await host.take(v=>v.type==='session.start');
  assert.equal((await post(f)).body.error.code,'desktop_busy');
  other.json({type:'session.stop'});await host.take(v=>v.type==='session.stop');
  // RDP is not offered: no RDP controller can exist to hold the display.
  const rdp=await f.call('/desktops/pve-610/connect',{method:'POST',data:{mode:'rdp'}});
  assert.equal(rdp.status,409);assert.equal(rdp.body.error.code,'rdp_not_supported');
  assert.equal((await f.call(path)).body.display.busy,false);
});

test('Timeout remains unknown and blocks new operations until exact late worker completion, never late-success',async t=>{
  const f=await fixture(t,{displayReadyMs:40}),host=await f.host();
  const response=await post(f),request=await host.take(v=>v.type==='host.display.configure');
  const cancel=await host.take(v=>v.type==='host.display.cancel');assert.equal(cancel.reason,'display_timeout');
  const unknown=(await f.call(path)).body.display;assert.equal(unknown.operation.status,'unknown');assert.equal(unknown.busy,true);assert.equal(unknown.stale,true);
  host.json({type:'host.heartbeat',capabilities:caps(1)});await host.take(v=>v.type==='host.heartbeat.ack');
  assert.equal((await f.call(path)).body.display.busy,true);
  result(host,request);host.json({type:'host.heartbeat'});await host.take(v=>v.type==='host.heartbeat.ack');
  const final=(await f.call(path+'/operations/'+response.body.display.operation.id)).body.display;
  assert.equal(final.operation.status,'unknown');assert.equal(final.operation.error,'display_timeout');assert.equal(final.busy,false);assert.equal(final.activeCount,1);
});

test('Revoking assignment cancels an in-flight operation; late completion refreshes observation without granting success',async t=>{
  const f=await fixture(t),host=await f.host();const response=await post(f),request=await host.take(v=>v.type==='host.display.configure');
  f.store.revoke('alice','pve-610');assert.equal((await host.take(v=>v.type==='host.display.cancel')).reason,'authorization_revoked');
  assert.equal((await f.call(path+'/operations/'+response.body.display.operation.id)).status,404);
  result(host,request);host.json({type:'host.heartbeat'});await host.take(v=>v.type==='host.heartbeat.ack');
  f.store.assign('alice','pve-610');const final=(await f.call(path)).body.display;
  assert.equal(final.operation.status,'failed');assert.equal(final.operation.error,'authorization_revoked');assert.equal(final.busy,false);assert.equal(final.activeCount,1);
});

test('Mismatched count or unconfirmed worker result never succeeds; host loss releases only the concurrency guard',async t=>{
  const f=await fixture(t),host=await f.host();const response=await post(f),request=await host.take(v=>v.type==='host.display.configure');
  result(host,request,{count:2,capabilities:caps(2)});
  assert.equal((await host.take(v=>v.type==='host.display.cancel')).reason,'display_result_invalid');
  const bad=(await f.call(path)).body.display;assert.equal(bad.operation.status,'unknown');assert.equal(bad.busy,true);
  const closed=once(host,'close');host.close();await closed;
  const after=(await f.call(path+'/operations/'+response.body.display.operation.id)).body.display;assert.equal(after.operation.status,'unknown');assert.equal(after.activeCount,null);assert.equal(after.stale,true);assert.equal(after.busy,false);assert.equal(after.supported,false);
});

test('An active power task blocks topology dispatch; failed display ACK reports actual final topology without unchanged claims',async t=>{
  const f=await fixture(t),host=await f.host();
  assert.equal((await f.call('/desktops/pve-610/power',{method:'POST',data:{action:'reboot'}})).status,202);
  assert.equal((await post(f)).body.error.code,'power_in_progress');
  f.store.db.prepare("UPDATE power_tasks SET active=0,status='stopped',exit_status='OK'").run();
  const response=await post(f),request=await host.take(v=>v.type==='host.display.configure');
  result(host,request,{ok:false,reason:'display_configuration_failed',capabilities:caps(2)});
  const final=(await completed(f,response.body.display.operation.id)).body.display;
  assert.equal(final.operation.status,'failed');assert.equal(final.operation.error,'display_configuration_failed');assert.equal(final.activeCount,2);assert.equal(final.busy,false);assert.equal(Object.hasOwn(final,'unchanged'),false);
});

// A mode transition has already retired its stream when display preparation
// begins. A lost Host still needs a bounded close audit with zero ended sessions.
const closeAuditRows = f => f.store.db.prepare("SELECT actor,subject FROM audit_events WHERE event='host_channel_closed' ORDER BY id").all();

test('Pending display preparation preserves a Host close audit after its stream has already ended',async t=>{
  const f=await fixture(t),host=await f.host();
  assert.equal(f.server.relay.sessions.size,0);
  const response=await post(f),request=await host.take(v=>v.type==='host.display.configure');
  const closed=once(host,'close');host.close();await closed;
  const after=(await completed(f,response.body.display.operation.id)).body.display;
  assert.equal(after.operation.status,'unknown');assert.equal(after.operation.error,'host_disconnected');assert.equal(after.busy,false);
  const rows=closeAuditRows(f);assert.equal(rows.length,1);
  assert.equal(rows[0].subject,'pve-610:host_disconnected::0');
  assert.equal(rows[0].subject.split(':').length,4);
  assert.equal(rows[0].actor,f.store.db.prepare('SELECT id FROM hosts WHERE desktop_id=?').get('pve-610').id);
  assert.equal(JSON.stringify(rows).includes(request.nonce),false);
});

test('An idle Host close without sessions, pending display work or detail stays unaudited',async t=>{
  const f=await fixture(t),host=await f.host();
  assert.equal(f.server.relay.sessions.size,0);assert.equal(f.server.relay.displayBusy.size,0);
  const current=f.server.relay.hosts.get('pve-610'),closed=once(host,'close');
  f.server.relay.dropHost(current,'host_disconnected');await closed;
  assert.deepEqual(closeAuditRows(f),[]);assert.equal(f.server.relay.hosts.has('pve-610'),false);
});

test('Pending-display close audits stay rate-limited and retired Hosts cannot affect a successor operation',async t=>{
  const f=await fixture(t),first=await f.host(),retired=f.server.relay.hosts.get('pve-610');
  const initial=await post(f);await first.take(v=>v.type==='host.display.configure');
  const firstClosed=once(first,'close');first.close();await firstClosed;
  assert.equal((await completed(f,initial.body.display.operation.id)).body.display.operation.error,'host_disconnected');
  assert.equal(closeAuditRows(f).length,1);
  const second=await f.host(),current=f.server.relay.hosts.get('pve-610');
  const next=await post(f);await second.take(v=>v.type==='host.display.configure');
  f.server.relay.dropHost(retired,'host_disconnected');
  assert.equal(f.server.relay.hosts.get('pve-610'),current);
  const pending=(await f.call(path+'/operations/'+next.body.display.operation.id)).body.display;
  assert.equal(pending.operation.status,'pending');assert.equal(pending.busy,true);
  assert.equal(closeAuditRows(f).length,1);
  const auditedAt=f.server.relay.hostCloseAudits.get('pve-610');
  assert.ok(Date.now()-auditedAt<60000);
  const secondClosed=once(second,'close');second.close();await secondClosed;
  const after=(await completed(f,next.body.display.operation.id)).body.display;
  assert.equal(after.operation.status,'unknown');assert.equal(after.operation.error,'host_disconnected');assert.equal(after.busy,false);
  assert.equal(closeAuditRows(f).length,1);
  assert.equal(f.server.relay.hostCloseAudits.get('pve-610'),auditedAt);
});
