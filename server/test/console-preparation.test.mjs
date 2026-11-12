import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';

const origin = 'https://console-fixture.example';
const uuid = '00000000-1234-1234-1234-000000000001';
const caps = (count = 2, control = true) => ({ service:true,interactiveSession:true,streaming:true,virtualDisplayReady:true,virtualMonitorControl:control,
  monitors:Array.from({length:count},(_,id)=>({id:String(id),width:1920,height:1080,primary:id===0})) });
async function fixture(t, relayOptions = {}) {
  const store=new Store(':memory:'),vault=new Vault(randomBytes(32)),tokens={},users={};
  for(const name of ['admin','alice','bobby']) {
    users[name]=store.addUser({name,passwordHash:'display-test-only',role:name==='admin'?'admin':'user'});
    tokens[name]=newToken();store.createSession(store.findUser(name),tokenHash(tokens[name]),Date.now(),3600000,'client');
  }
  const vms=[610,611].map(vmid=>({vmid,node:'fixture',name:'VM '+vmid,status:'running'}));
  store.syncProxmox(vms,'fixture');store.assign('alice','pve-610');store.assign('bobby','pve-611');
  store.saveSetting('proxmox',vault.seal({endpoint:'https://pve-fixture.example',tokenId:'test@pve!fixture',secret:'isolated-fixture-only'}),'fixture');
  const code=newToken(),hostToken=newToken();store.issueEnrollment('pve-610',uuid,tokenHash(code),'fixture');
  store.consumeEnrollment(tokenHash(code),uuid,{hostname:'Display fixture',version:'0.2.2',tokenHash:tokenHash(hostToken)});
  const powerPosts=[];
  const proxmox={inventory:async()=>vms,power:async(_cfg,vmid,action,{beforeSubmit}={})=>{beforeSubmit?.();powerPosts.push({vmid,action});return{node:'fixture',task:`UPID:fixture:1:2:3:qm${action}:${vmid}:fixture@pve:`};},task:async()=>({status:'running',exitStatus:null})};
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
  return{store,server,call,host,tokens,users,powerPosts};
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
const path='/desktops/pve-610/console-preparation';
const recoveryCaps=(extra={})=>({...caps(0,false),interactiveSession:false,streaming:false,virtualDisplayReady:false,consoleRecoverySupported:true,consoleRecoveryAvailable:true,consoleRecoveryReason:'rdp_session_disconnected',...extra});
const post=(f,data={confirmation:'resume-windows-session'},as='alice')=>f.call(path,{method:'POST',data,as});
const result=(host,request,extra={})=>host.json({type:'host.console.result',operationId:request.operationId,nonce:request.nonce,ok:true,attempted:true,reason:'',...extra});
async function state(f,id){for(let i=0;i<60;i++){const response=await f.call(path+'/operations/'+id);if(response.body.consolePreparation?.operation.status!=='pending')return response.body.consolePreparation;await new Promise(resolve=>setTimeout(resolve,5));}assert.fail('Console preparation did not finish');}

test('Assigned client explicitly resumes a Windows session, shares mutation locks and waits for actual streaming readiness',async t=>{
 const f=await fixture(t),host=await f.host(recoveryCaps());
 assert.equal((await f.call(path)).body.consolePreparation.available,true);
 const response=await post(f);assert.equal(response.status,202);const op=response.body.consolePreparation.operation;assert.equal(op.status,'pending');assert.equal(op.attempted,null);
 const request=await host.take(v=>v.type==='host.console.prepare');assert.deepEqual(Object.keys(request).sort(),['nonce','operationId','type','userConfirmed']);assert.equal(request.userConfirmed,true);assert.equal(request.operationId,op.id);assert.match(request.nonce,/^[A-Za-z0-9_-]{43}$/);
 assert.equal((await f.call('/desktops/pve-610/connect',{method:'POST',data:{}})).body.error.code,'display_in_progress');
 assert.equal((await f.call('/desktops/pve-610/display',{method:'POST',data:{count:1}})).status,409);
 assert.equal((await f.call('/desktops/pve-610/power',{method:'POST',data:{action:'shutdown'}})).body.error.code,'display_in_progress');assert.equal(f.powerPosts.length,0);
 assert.equal((await post(f)).body.error.code,'display_in_progress');
 result(host,request);const finished=await state(f,op.id);assert.equal(finished.operation.status,'succeeded');assert.equal(finished.operation.attempted,true);
 const desktop=(await f.call('/desktops')).body.desktops.find(d=>d.id==='pve-610');assert.equal(desktop.connectionModes.stream,false);assert.equal(desktop.host.capabilities.virtualDisplayReady,false);assert.equal(desktop.host.capabilities.interactiveSession,false);
 assert.equal((await f.call('/desktops/pve-610/connect',{method:'POST',data:{}})).status,409);
 host.json({type:'host.capabilities',capabilities:{...caps(),consoleRecoverySupported:true,consoleRecoveryAvailable:false,consoleRecoveryReason:'console_ready'}});assert.equal((await host.take(v=>v.type==='host.capabilities.ack')).accepted,true);
 assert.equal((await f.call('/desktops/pve-610/connect',{method:'POST',data:{}})).status,200);
});

test('Console handoff requires assignment, authenticated confirmation and a strict body without Windows IDs',async t=>{
 const f=await fixture(t),host=await f.host(recoveryCaps());
 assert.equal((await post(f,undefined,null)).status,401);
 assert.equal((await post(f,undefined,'bobby')).status,404);
 for(const data of [{},{confirmation:'yes'},{confirmation:true},{confirmation:'resume-windows-session',sourceSessionId:2},{confirmation:'resume-windows-session',username:'Administrator'}])assert.equal((await post(f,data)).status,400);
 assert.equal((await f.call(path)).body.consolePreparation.operation,null);
 host.json({type:'host.heartbeat'});await host.take(v=>v.type==='host.heartbeat.ack');
});

test('Polling and stale or absent capability never trigger automatic session transfer',async t=>{
 const f=await fixture(t),host=await f.host();
 for(let i=0;i<3;i++)assert.equal((await f.call(path)).body.consolePreparation.available,false);
 assert.equal((await post(f)).status,409);
 host.json({type:'host.capabilities',capabilities:recoveryCaps({consoleRecoveryAvailable:false,consoleRecoveryReason:'windows_session_locked'})});assert.equal((await host.take(v=>v.type==='host.capabilities.ack')).accepted,true);
 assert.equal((await f.call(path)).body.consolePreparation.reason,'windows_session_locked');assert.equal((await post(f)).status,409);
});

test('Nonce mismatch cannot complete, timeout retains uncertainty and a late reply cannot resurrect success',async t=>{
 const f=await fixture(t,{consoleReadyMs:300}),host=await f.host(recoveryCaps());const response=await post(f),id=response.body.consolePreparation.operation.id,request=await host.take(v=>v.type==='host.console.prepare');
 result(host,request,{nonce:newToken()});host.json({type:'host.heartbeat'});await host.take(v=>v.type==='host.heartbeat.ack');assert.equal((await f.call(path)).body.consolePreparation.operation.status,'pending');
 await host.take(v=>v.type==='host.console.cancel');
 const timedOut=(await f.call(path)).body.consolePreparation;assert.equal(timedOut.operation.status,'unknown');assert.equal(timedOut.busy,true);assert.equal(timedOut.operation.attempted,null);
 result(host,request);const finished=await state(f,id);assert.equal(finished.operation.status,'unknown');assert.equal(finished.operation.error,'console_timeout');assert.equal(finished.busy,false);
});

test('Only the requesting user can inspect the operation and assignment revocation cancels without restoring authority',async t=>{
 const f=await fixture(t),host=await f.host(recoveryCaps());const response=await post(f),id=response.body.consolePreparation.operation.id,request=await host.take(v=>v.type==='host.console.prepare');
 f.store.assign('bobby','pve-610');assert.equal((await f.call(path+'/operations/'+id,{as:'bobby'})).status,404);assert.equal((await f.call(path,{as:'bobby'})).body.consolePreparation.operation,null);
 f.store.db.prepare('DELETE FROM assignments WHERE user_id=? AND desktop_id=?').run(f.users.alice,'pve-610');
 await host.take(v=>v.type==='host.console.cancel');result(host,request);host.json({type:'host.heartbeat'});await host.take(v=>v.type==='host.heartbeat.ack');
 assert.equal((await f.call(path+'/operations/'+id)).status,404);assert.equal((await f.call(path,{as:'bobby'})).body.consolePreparation.busy,false);
});

test('Malformed console capability and free-form host result cannot grant recovery or leak data',async t=>{
 const f=await fixture(t),host=await f.host(recoveryCaps());
 for(const extra of [{consoleRecoveryAvailable:'true'},{consoleRecoverySupported:false},{consoleRecoveryReason:'PRIVATE_USER_SID'},{consoleRecoveryReason:'console_ready'}]){
   host.json({type:'host.capabilities',capabilities:recoveryCaps(extra)});assert.equal((await host.take(v=>v.type==='host.capabilities.ack')).accepted,false);
 }
 host.json({type:'host.capabilities',capabilities:recoveryCaps()});assert.equal((await host.take(v=>v.type==='host.capabilities.ack')).accepted,true);
 const response=await post(f),id=response.body.consolePreparation.operation.id,request=await host.take(v=>v.type==='host.console.prepare');
 result(host,request,{ok:false,reason:'PRIVATE_USER_SID'});await once(host,'close');const finished=await state(f,id);assert.equal(finished.operation.status,'unknown');assert.equal(finished.busy,false);assert.ok(!JSON.stringify(finished).includes('PRIVATE_USER_SID'));
});

test('Original numeric Windows failure reaches administrator audit without entering client data',async t=>{
 const f=await fixture(t),host=await f.host(recoveryCaps());
 for(const systemError of [5,0xffffffff]){
   const response=await post(f),id=response.body.consolePreparation.operation.id,request=await host.take(v=>v.type==='host.console.prepare');
   result(host,request,{ok:false,reason:'console_transfer_failed',systemError});
   const finished=await state(f,id);assert.equal(finished.operation.status,'failed');assert.equal(finished.operation.error,'console_transfer_failed');assert.equal(Object.hasOwn(finished.operation,'systemError'),false);
   const audit=f.store.auditLog().find(row=>row.event==='console_resume_completed');assert.equal(audit.subject,`pve-610:failed:console_transfer_failed:windows=${systemError}`);
 }
});

test('Malformed or contradictory Windows error numbers cannot be trusted or copied into audit',async t=>{
 for(const systemError of [null,'5',-1,1.5,0x100000000,'PRIVATE_USER_SID']){
   const f=await fixture(t),host=await f.host(recoveryCaps()),response=await post(f),id=response.body.consolePreparation.operation.id,request=await host.take(v=>v.type==='host.console.prepare');
   result(host,request,{ok:false,reason:'console_transfer_failed',systemError});await once(host,'close');assert.equal((await state(f,id)).operation.status,'unknown');assert.equal(f.store.auditLog().filter(row=>row.event==='console_resume_completed').length,0);
 }
 const f=await fixture(t),host=await f.host(recoveryCaps()),response=await post(f),id=response.body.consolePreparation.operation.id,request=await host.take(v=>v.type==='host.console.prepare');
 result(host,request,{systemError:5});await once(host,'close');assert.equal((await state(f,id)).operation.status,'unknown');
});
