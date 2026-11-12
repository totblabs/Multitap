// Real HTTP/WS broker routes with synthetic Host completion evidence. This
// does not prove an installed worker exit or Windows topology restoration.
import {test} from 'node:test';
import assert from 'node:assert/strict';
import {randomBytes,randomUUID} from 'node:crypto';
import {WebSocket} from 'ws';
import {Store} from '../src/store.mjs';
import {Vault} from '../src/vault.mjs';
import {buildServer} from '../src/http.mjs';
import {newToken,tokenHash} from '../src/security.mjs';
import {readFileSync} from 'node:fs';

const origin='https://stop-barrier-fixture.example';
const baseCaps={service:true,interactiveSession:true,streaming:true,virtualDisplayReady:true, virtualMonitorControl:true,monitors:[{id:'0',width:1920,height:1080,primary:true}]};
async function until(predicate){const end=Date.now()+2500;while(!predicate()){assert.ok(Date.now()<end,'fixture_timeout');await new Promise(resolve=>setTimeout(resolve,5));}}
async function fixture(t,{negotiate=true,stopReadyMs=45000}={}){
 const store=new Store(':memory:'),vault=new Vault(randomBytes(32)),tokens={alice:newToken(),host:newToken()},sockets=[];
 store.addUser({name:'alice',passwordHash:'unused-fixture'});store.syncProxmox([{vmid:149,node:'qa',name:'VM',status:'running'}],'fixture');store.assign('alice','pve-149');
 store.createSession(store.findUser('alice'),tokenHash(tokens.alice),Date.now(),3600000,'client');const code=newToken(),uuid='12345678-1234-1234-1234-123456789abc';
 store.issueEnrollment('pve-149',uuid,tokenHash(code),'fixture');store.consumeEnrollment(tokenHash(code),uuid,{hostname:'Fixture',version:'9.0.0',tokenHash:tokenHash(tokens.host)});
 const server=await buildServer({store,vault,publicOrigin:origin,relayOptions:{authorizationMs:10,heartbeatMs:10000,stopReadyMs,stopPollMs:20},logger:{error:value=>assert.fail(value)}});
 await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));const base='http://127.0.0.1:'+server.address().port;
 t.after(async()=>{for(const ws of sockets)ws.terminate();await new Promise(resolve=>server.close(resolve));store.close();});
 async function open(path,as='alice'){
  const ws=new WebSocket(base.replace('http:','ws:')+path,{headers:{Authorization:(as==='host'?'Host ':'Bearer ')+tokens[as]},perMessageDeflate:false});
  ws.messages=[];ws.json=value=>ws.send(JSON.stringify(value));ws.on('message',b=>ws.messages.push(JSON.parse(b.toString('utf8'))));ws.on('error',()=>{});sockets.push(ws);
  await new Promise((resolve,reject)=>{ws.once('open',resolve);ws.once('error',reject);});return ws;
 }
 async function call(path,{method='GET',data}={}){const response=await fetch(base+'/api/v1'+path,{method,headers:{Authorization:'Bearer '+tokens.alice,...(data===undefined?{}:{'Content-Type':'application/json'})},body:data===undefined?undefined:JSON.stringify(data)});return{status:response.status,body:await response.json()};}
 let host=await open('/api/v1/hosts/channel','host');
 const caps={...baseCaps,...(negotiate?{sessionStopProtocol:'stop-v1'}:{})};
 async function advertise(value=caps){const before=host.messages.filter(v=>v.type==='host.capabilities.ack').length;host.json({type:'host.capabilities',capabilities:value});await until(()=>host.messages.filter(v=>v.type==='host.capabilities.ack').length>before);return host.messages.filter(v=>v.type==='host.capabilities.ack').at(-1);}
 assert.equal((await advertise()).accepted,true);
 async function create(mode='stream'){return call('/desktops/pve-149/connect',{method:'POST',data:{mode}});}
 async function start(){const response=await create();assert.equal(response.status,200);const ticket=response.body,client=await open('/api/v1/sessions/'+ticket.sessionId);await until(()=>host.messages.some(v=>v.type==='session.start'&&v.sessionId===ticket.sessionId));return{ticket,client};}
 async function end(session){session.client.close();await until(()=>host.messages.some(v=>v.type==='session.stop'&&v.sessionId===session.ticket.sessionId));return host.messages.find(v=>v.type==='session.stop'&&v.sessionId===session.ticket.sessionId);}
 const cleanup=async()=>(await call('/desktops')).body.desktops[0].sessionCleanup;
 function ack(stop,changes={}){host.json({type:'host.session.stopped',sessionId:stop.sessionId,sessionStopProtocol:'stop-v1',stopRequestId:stop.stopRequestId,workerQuiescent:true,profileRequired:true,profileRestored:true,...changes});}
 return {store,call,create,start,end,cleanup,advertise,ack,get host(){return host;},async reconnectHost(){host.terminate();await new Promise(resolve=>setTimeout(resolve,40));host=await open('/api/v1/hosts/channel','host');await advertise();}};
}

test('Stopped client cannot reopen or reconfigure outputs before exact Host completion',async t=>{
 const f=await fixture(t),session=await f.start(),stop=await f.end(session);
 assert.equal(stop.sessionStopProtocol,'stop-v1');assert.match(stop.stopRequestId,/^[0-9a-f-]{36}$/);
 assert.equal((await f.create()).body.error.code,'session_cleanup_pending');
 assert.equal((await f.call('/desktops/pve-149/display',{method:'POST',data:{count:2}})).body.error.code,'session_cleanup_pending');
 assert.equal((await f.cleanup()).pending,true);
 assert.equal((await f.call('/desktops')).body.desktops[0].display.stale,true);
});

test('A stream cleanup waiting for a display restore never refuses VM power (a reboot ends it)',async t=>{
 const f=await fixture(t),session=await f.start(),stop=await f.end(session);
 // The Host answers that its capture stopped but the display state is not
 // restored yet (while the restore keeps failing, the reboot that
 // would clear it must not be refused with session_cleanup_pending).
 f.ack(stop,{profileRestored:false});
 await until(async()=>true);
 assert.equal((await f.create()).body.error.code,'session_cleanup_pending');
 const power=await f.call('/desktops/pve-149/power',{method:'POST',data:{action:'reboot'}});
 assert.notEqual(power.body.error?.code,'session_cleanup_pending');
 assert.notEqual(power.body.error?.code,'display_in_progress');
 // It got past the barrier to the power state check (this fixture has no
 // Proxmox link); that refusal before Proxmox is audited with its code.
 assert.equal(power.body.error?.code,'power_status_unavailable');
 assert.ok(f.store.db.prepare("SELECT subject FROM audit_events WHERE event='power_refused'").all().some(row=>row.subject==='pve-149:reboot:power_status_unavailable'));
});

test('Count reconfiguration cannot bypass negotiated teardown by terminating an active stream inline',async t=>{
 const f=await fixture(t),session=await f.start();
 assert.equal((await f.call('/desktops/pve-149/display',{method:'POST',data:{count:2}})).body.error.code,'desktop_busy');
 assert.equal(session.client.readyState,WebSocket.OPEN);
 assert.equal(f.host.messages.some(v=>v.type==='session.stop'||v.type==='host.display.configure'),false);
});

test('Malformed or contradictory completion cannot manufacture capture-exit proof',async t=>{
 const f=await fixture(t),session=await f.start(),stop=await f.end(session);
 for(const change of [{workerQuiescent:'true'},{profileRestored:1},{workerQuiescent:false,profileRestored:true},{extra:true}]){
  f.ack(stop,change);await f.advertise();assert.equal((await f.create()).status,409);
  assert.equal((await f.cleanup()).workerQuiescent,false);
 }
});

test('Old readiness and wrong stop correlation cannot release the barrier; fresh post-ACK capabilities are required',async t=>{
 const f=await fixture(t),session=await f.start(),stop=await f.end(session);
 await f.advertise();assert.equal((await f.create()).status,409);
 f.ack(stop,{stopRequestId:randomUUID()});await new Promise(resolve=>setTimeout(resolve,20));assert.equal((await f.create()).status,409);
 f.ack(stop,{sessionId:randomUUID()});await new Promise(resolve=>setTimeout(resolve,20));assert.equal((await f.create()).status,409);
 await until(()=>f.host.messages.some(v=>v.type==='host.session.stop.status'&&v.stopRequestId===stop.stopRequestId));
 f.ack(stop);await new Promise(resolve=>setTimeout(resolve,20));
 assert.equal((await f.create()).status,409);
 assert.equal((await f.cleanup()).phase,'awaiting_capabilities');
 await f.advertise();assert.equal((await f.create()).status,200);
});

test('Recovery-required completion remains blocked and accepts only a later matching verified completion',async t=>{
 const f=await fixture(t),session=await f.start(),stop=await f.end(session);
 f.ack(stop,{profileRestored:false});await new Promise(resolve=>setTimeout(resolve,20));
 assert.equal((await f.cleanup()).phase,'recovery_required');await f.advertise();assert.equal((await f.create()).status,409);
 f.ack(stop);await new Promise(resolve=>setTimeout(resolve,20));await f.advertise();assert.equal((await f.create()).status,200);
});

test('Actual Host48 stop observations keep count admission fenced until owned output control returns',async t=>{
 const observed=JSON.parse(readFileSync(new URL('./fixtures/host48-stop-readiness.json',import.meta.url),'utf8'));
 assert.equal(observed.hostVersion,'0.2.48');
 const f=await fixture(t),session=await f.start(),stop=await f.end(session);
 f.ack(stop);await new Promise(resolve=>setTimeout(resolve,20));
 // Replay only the captured readiness fields; authentication/transport and
 // remaining optional capabilities belong to this isolated route fixture.
 for(const sample of observed.samples.filter(value=>value.elapsedMs>=260)){
  assert.equal((await f.advertise({...baseCaps,...sample.capabilities,sessionStopProtocol:'stop-v1'})).accepted,true);
  const ready=sample.capabilities.virtualMonitorControl===true&&sample.display.supported&&!sample.display.stale;
  assert.equal((await f.cleanup()).pending,!ready,'post-stop sample at '+sample.elapsedMs+'ms');
  if(!ready){
   assert.equal((await f.create()).body.error.code,'session_cleanup_pending');
   assert.equal((await f.call('/desktops/pve-149/display',{method:'POST',data:{count:2}})).body.error.code,'session_cleanup_pending');
  }
 }
 assert.equal((await f.create()).status,200);
});

test('A verified restore after the user signed out releases the barrier without a user display',async t=>{
 const f=await fixture(t),session=await f.start(),stop=await f.end(session);
 const signedOut={...baseCaps,sessionStopProtocol:'stop-v1',interactiveSession:false,streaming:false,virtualDisplayReady:false,virtualMonitorControl:false,monitors:[]};
 // Not before the Host verified its restore.
 await f.advertise(signedOut);assert.equal((await f.cleanup()).pending,true);
 f.ack(stop,{profileRestored:false});await new Promise(resolve=>setTimeout(resolve,20));
 await f.advertise(signedOut);assert.equal((await f.cleanup()).phase,'recovery_required');
 f.ack(stop);await new Promise(resolve=>setTimeout(resolve,20));
 await f.advertise(signedOut);assert.equal((await f.cleanup()).pending,false);
});
test('A verified restore while Windows is locked releases the barrier without a user display',async t=>{
 const f=await fixture(t),session=await f.start(),stop=await f.end(session);
 const locked={...baseCaps,sessionStopProtocol:'stop-v1',consoleRecoverySupported:true,consoleRecoveryAvailable:false,consoleRecoveryReason:'windows_session_locked',streaming:false,virtualDisplayReady:false,virtualMonitorControl:false,monitors:[]};
 await f.advertise(locked);assert.equal((await f.cleanup()).pending,true,'not before the Host verified its restore');
 f.ack(stop,{profileRestored:false});await new Promise(resolve=>setTimeout(resolve,20));
 await f.advertise(locked);assert.equal((await f.cleanup()).phase,'recovery_required');
 f.ack(stop);await new Promise(resolve=>setTimeout(resolve,20));
 await f.advertise(locked);assert.equal((await f.cleanup()).pending,false);
});
// A stream ends while a UAC prompt shows on the secure desktop. The session is unlocked, so
// neither the signed-out nor the locked release applies; the Host's display cannot come back until the prompt is answered, and
// without this release the Console that answers it is refused (console_cleanup_pending) until the VM is restarted.
test('A verified restore under a secure desktop releases the barrier without a user display',async t=>{
 for(const report of [{secureDesktop:true},{viewportFailureReason:'no_interactive_session'},{secureDesktop:true,viewportFailureReason:'no_interactive_session'}]){
  const f=await fixture(t),session=await f.start(),stop=await f.end(session);
  const prompt={...baseCaps,sessionStopProtocol:'stop-v1',consoleRecoverySupported:true,consoleRecoveryAvailable:false,consoleRecoveryReason:'console_ready',streaming:false,virtualDisplayReady:false,virtualMonitorControl:false,monitors:[],...report};
  assert.equal((await f.advertise(prompt)).accepted,true);assert.equal((await f.cleanup()).pending,true,'not before the Host verified its restore');
  f.ack(stop,{profileRestored:false});await new Promise(resolve=>setTimeout(resolve,20));
  await f.advertise(prompt);assert.equal((await f.cleanup()).phase,'recovery_required');
  f.ack(stop);await new Promise(resolve=>setTimeout(resolve,20));
  await f.advertise(prompt);assert.equal((await f.cleanup()).pending,false);
  // Only the barrier is gone: the Host's own readiness still refuses a stream and a display change.
  const refused=await f.create();assert.equal(refused.status,409);assert.notEqual(refused.body.error.code,'session_cleanup_pending');
  assert.equal((await f.call('/desktops/pve-149/display',{method:'POST',data:{count:2}})).body.error.code,'display_control_unavailable');
 }
});
test('An unlocked session without a secure desktop keeps the barrier until its display returns',async t=>{
 const f=await fixture(t),session=await f.start(),stop=await f.end(session);
 f.ack(stop);await new Promise(resolve=>setTimeout(resolve,20));
 const unready={...baseCaps,sessionStopProtocol:'stop-v1',streaming:false,virtualDisplayReady:false,virtualMonitorControl:false,monitors:[]};
 for(const report of [{},{viewportFailureReason:'viewport_probe_pending'},{secureDesktop:false},{secureDesktop:'true'},{secureDesktop:true,streaming:true,virtualDisplayReady:true}]){
  assert.equal((await f.advertise({...unready,...report})).accepted,true);assert.equal((await f.cleanup()).pending,true,JSON.stringify(report));
 }
 await f.advertise();assert.equal((await f.cleanup()).pending,false);
});
test('A stop without an owned display profile preserves the separate completion contract',async t=>{
 const f=await fixture(t),session=await f.start(),stop=await f.end(session);
 f.ack(stop,{profileRequired:false});await new Promise(resolve=>setTimeout(resolve,20));
 await f.advertise({...baseCaps,sessionStopProtocol:'stop-v1',streaming:false,virtualDisplayReady:false,virtualMonitorControl:false,monitors:[]});
 assert.equal((await f.cleanup()).pending,false);
 // No cleanup barrier remains: a connect is refused only for the unready outputs.
 const refused=await f.create();assert.equal(refused.status,409);assert.equal(refused.body.error.code,'stream_unavailable');assert.equal(refused.body.error.details.reason,'virtual_display_required');
});

test('A repeated stop status cannot downgrade the exact session display-profile requirement',async t=>{
 const f=await fixture(t),session=await f.start(),stop=await f.end(session);
 f.ack(stop,{profileRestored:false});await new Promise(resolve=>setTimeout(resolve,20));
 f.ack(stop,{profileRequired:false});await new Promise(resolve=>setTimeout(resolve,20));
 await f.advertise();
 assert.equal((await f.cleanup()).pending,true);
 assert.equal((await f.create()).body.error.code,'session_cleanup_pending');
 f.ack(stop);await new Promise(resolve=>setTimeout(resolve,20));await f.advertise();
 assert.equal((await f.create()).status,200);
});

test('Cleanup timeout never invents success; a late valid reply is fenced to its exact session',async t=>{
 const f=await fixture(t,{stopReadyMs:25}),session=await f.start(),stop=await f.end(session);
 await new Promise(resolve=>setTimeout(resolve,60));assert.equal((await f.cleanup()).phase,'unknown');assert.equal((await f.create()).status,409);
 f.ack(stop,{workerQuiescent:false,profileRestored:false});await new Promise(resolve=>setTimeout(resolve,20));assert.equal((await f.create()).status,409);
 f.ack(stop);await new Promise(resolve=>setTimeout(resolve,20));await f.advertise();const next=await f.start();
 f.ack(stop,{profileRestored:false});await new Promise(resolve=>setTimeout(resolve,20));assert.equal(next.client.readyState,WebSocket.OPEN);
});

test('An unstarted cancellation never opens a Host completion barrier',async t=>{
 const f=await fixture(t),ticket=(await f.create()).body;assert.equal((await f.call('/sessions/'+ticket.sessionId,{method:'DELETE'})).status,200);
 assert.equal(f.host.messages.some(v=>v.type==='session.start'||v.type==='session.stop'),false);assert.equal((await f.create()).status,200);
});

test('An unnegotiated released Host preserves legacy behavior and never receives invented stop extensions',async t=>{
 const f=await fixture(t,{negotiate:false}),session=await f.start(),stop=await f.end(session);assert.equal(stop.sessionStopProtocol,undefined);assert.equal((await f.create()).status,200);
});

test('Host replacement removes stale correlation but fresh Host readiness is still required',async t=>{
 const f=await fixture(t),session=await f.start(),stop=await f.end(session);await f.reconnectHost();
 f.ack(stop);await new Promise(resolve=>setTimeout(resolve,20));assert.equal((await f.create()).status,200);
});
