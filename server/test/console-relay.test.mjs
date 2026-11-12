// Actual ConsoleRelay/Relay/Store, memory SQLite and EventEmitter transports.
// No listener, Proxmox request, Windows API, device or real credential access.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { createHash, randomBytes, randomUUID } from 'node:crypto';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { Relay } from '../src/relay.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { activitySnapshot } from '../src/activity.mjs';
import { safeClientDiagnosticReport } from '../src/client-diagnostics.mjs';
import { enrollFixtureHosts } from './fixtures/admin-assignment-input.mjs';

const machine = vmid => '12345678-1234-1234-1234-' + String(vmid).padStart(12,'0');
const settle = async () => { for(let n=0;n<20;n++) await Promise.resolve(); };
class Wire extends EventEmitter {
  readyState=1; sent=[]; terminations=0;
  paused=false; pauses=0; resumes=0;
  pause(){this.paused=true;++this.pauses;}
  resume(){this.paused=false;++this.resumes;}
  send(bytes,options,done){this.sent.push({binary:options.binary,bytes:Buffer.isBuffer(bytes)?Buffer.from(bytes):String(bytes)});done?.();}
  terminate(){if(this.readyState===3)return;this.readyState=3;++this.terminations;this.emit('close');}
  close(code,reason){this.closeCode=code;this.closeReason=reason;this.terminate();}
  ping(){}
  packet(value,binary=false){this.emit('message',binary?value:Buffer.from(JSON.stringify(value)),binary);}
}
function fixture(t){
  const store=new Store(':memory:'),vault=new Vault(randomBytes(32));
  store.addUser({name:'alice',passwordHash:'synthetic-only'});store.addUser({name:'bobby',passwordHash:'synthetic-only'});
  store.syncProxmox([149,150,151].map(vmid=>({vmid,node:'memory',name:'Memory VM',status:'running',machineUuid:machine(vmid)})),'fixture');
  for(const name of ['alice','bobby'])for(const vmid of [149,150,151])store.assign(name,'pve-'+vmid);
  const tokens={alice:newToken(),bobby:newToken()};
  for(const name of ['alice','bobby'])store.createSession(store.findUser(name),tokenHash(tokens[name]),Date.now(),3600000,'client');
  store.saveSetting('proxmox',vault.seal({endpoint:'https://memory.invalid',tokenId:'fixture@pve!fixture',secret:'SERVER_ONLY_SECRET'}),'fixture');
  const clock={time:Date.now()},calls=[];
  const proxmox={consoleSnapshot:async(_config,vmid)=>{calls.push(['snapshot',vmid]);return {node:'memory',vmid,machineUuid:machine(vmid)};},openConsole:async()=>{calls.push(['open']);return {socket:new Wire(),password:'SERVER_ONLY_VNC_PASSWORD'};}};
  const relay=new Relay({store,vault,proxmox,publicOrigin:'https://broker.invalid',authorizationMs:3600000,heartbeatMs:3600000,consoleOptions:{now:()=>clock.time}});
  const manager=relay.console;
  const grant=(vmid=149,name='alice',options)=>manager.create(store.findUser(name),tokenHash(tokens[name]),'pve-'+vmid,options);
  function claim(value,name='alice'){
    const wire=new Wire(),rejected=[];
    manager.wss.handleUpgrade=(_req,_socket,_head,done)=>done(wire);
    manager.upgrade({headers:{authorization:'Bearer '+tokens[name]}},{},Buffer.alloc(0),new URL('https://broker.invalid'+value.relayPath),(status)=>rejected.push(status));
    return {wire,rejected};
  }
  t.after(()=>{relay.close();store.close();});
  return {store,vault,clock,proxmox,relay,manager,tokens,calls,grant,claim};
}
test('Assigned VM grant works with no Host, contains only broker fields, and blocks a new ordinary connection',async t=>{
  const f=fixture(t),g=await f.grant();assert.equal(f.relay.hosts.size,0);
  assert.deepEqual(Object.keys(g).sort(),['expiresAt','id','relayPath','ticket']);
  assert.equal(JSON.stringify(g).includes('SERVER_ONLY'),false);assert.equal(f.calls.some(c=>c[0]==='open'),false);
  assert.throws(()=>f.relay.connect(f.store.findUser('alice'),tokenHash(f.tokens.alice),'pve-149'),e=>e.code==='console_busy');
  assert.deepEqual(f.manager.cancel(f.store.findUser('alice'),tokenHash(f.tokens.alice),'pve-149',g.id),{id:g.id,released:true});
  assert.equal(f.manager.grants.size,0);
});
test('browser console capability is VM-only and follows the resolved HTTPS origin',t=>{
  const f=fixture(t);enrollFixtureHosts(f.store,['pve-149']);
  const desktop=f.store.listDesktops(f.store.findUser('alice').id).find(d=>d.id==='pve-149');
  for(const origin of ['https://broker.invalid','https://secondary.invalid']) {
    const row=f.relay.decorate([desktop],'alice',origin)[0];
    assert.equal(row.consoleBrowserProtocol,'console-browser-v1');
    assert.equal(row.consoleRelayProtocol,'rfb-none-v1');
  }
  assert.equal(Object.hasOwn(f.relay.decorate([desktop],'alice','http://broker.invalid')[0],'consoleBrowserProtocol'),false);
  assert.equal(Object.hasOwn(f.relay.decorate([{...desktop,kind:'pc'}],'alice','https://broker.invalid')[0],'consoleBrowserProtocol'),false);
});
test('An existing stream reservation is refused without stopping or modifying it',async t=>{
  const f=fixture(t),sentinel={id:randomUUID(),desktopId:'pve-149'};f.relay.sessions.set(sentinel.id,sentinel);
  try{await assert.rejects(f.grant(),e=>e.code==='console_busy');assert.equal(f.relay.sessions.get(sentinel.id),sentinel);assert.equal(f.calls.length,0);}
  finally{f.relay.sessions.delete(sentinel.id);}
});
test('Both bearer and single-use ticket are required; a duplicate/foreign upgrade cannot terminate its owner',async t=>{
  const f=fixture(t),g=await f.grant(),first=f.claim(g);assert.deepEqual(first.rejected,[]);
  const duplicate=f.claim(g),foreign=f.claim(g,'bobby');assert.deepEqual(duplicate.rejected,[401]);assert.deepEqual(foreign.rejected,[401]);
  assert.equal(first.wire.readyState,1);assert.equal(f.manager.grants.get(g.id).closed,false);
  first.wire.packet({type:'console-auth',ticket:g.ticket});await settle();assert.equal(f.calls.filter(c=>c[0]==='open').length,1);
  first.wire.packet({type:'console-auth',ticket:g.ticket});assert.equal(first.wire.readyState,3);
});
test('An invalid first ticket never opens Proxmox and retires only that grant',async t=>{
  const f=fixture(t),g=await f.grant(),other=await f.grant(150,'bobby'),client=f.claim(g);
  client.wire.packet({type:'console-auth',ticket:newToken()});await settle();
  assert.equal(f.calls.some(c=>c[0]==='open'),false);assert.equal(f.manager.grants.has(g.id),false);assert.equal(f.manager.grants.has(other.id),true);
});
for(const revoke of ['logout','assignment','disabled'])test(`Actual Store ${revoke} revokes the console without touching another VM`,async t=>{
  const f=fixture(t),g=await f.grant(),other=await f.grant(150,'bobby'),client=f.claim(g);
  if(revoke==='logout')f.store.logout(tokenHash(f.tokens.alice));
  if(revoke==='assignment')f.store.revoke('alice','pve-149');
  if(revoke==='disabled')f.store.setUserEnabled('alice',false,'fixture');
  f.manager.revalidate();assert.equal(client.wire.readyState,3);assert.equal(f.manager.grants.has(g.id),false);assert.equal(f.manager.grants.has(other.id),true);
});
test('Expired grants, foreign cancellation, cross-origin and query-bearing upgrades fail closed',async t=>{
  const f=fixture(t),g=await f.grant();assert.throws(()=>f.manager.cancel(f.store.findUser('bobby'),tokenHash(f.tokens.bobby),'pve-149',g.id),e=>e.code==='console_session_not_found');
  for(const [url,origin] of [[g.relayPath+'?ticket=secret',undefined],[g.relayPath,'https://foreign.invalid']]){
    const rejected=[];f.manager.upgrade({headers:{authorization:'Bearer '+f.tokens.alice,...(origin?{origin}:{})}},{},Buffer.alloc(0),new URL(url,'https://broker.invalid'),code=>rejected.push(code));assert.deepEqual(rejected,[403]);
  }
  f.clock.time+=30001;f.manager.revalidate();assert.equal(f.manager.grants.size,0);
});
test('Per-user and per-VM bounds prevent overlapping/parallel reservations',async t=>{
  const f=fixture(t);await f.grant();await assert.rejects(f.grant(149,'bobby'),e=>e.code==='console_busy');await f.grant(150);
  await assert.rejects(f.grant(151),e=>e.code==='console_limit_reached');assert.equal(f.manager.grants.size,2);
});
test('Cancelled identity work retains its admission slot until the actual Promise settles',async t=>{
  const f=fixture(t);let resolve;f.proxmox.consoleSnapshot=()=>new Promise(done=>{resolve=done;});
  const abort=new AbortController(),pending=f.grant(149,'alice',{signal:abort.signal});await settle();assert.equal(f.manager.grants.size,1);
  assert.equal(activitySnapshot({relay:f.relay,store:f.store}).operations.display,1);
  assert.equal(activitySnapshot({relay:f.relay,store:f.store}).sessions.total,0);
  const rejected=assert.rejects(pending,e=>e.code==='console_cancelled');abort.abort();assert.equal(f.manager.grants.size,1);
  assert.equal(activitySnapshot({relay:f.relay,store:f.store}).operations.display,1);
  resolve({node:'memory',vmid:149,machineUuid:machine(149)});await rejected;assert.equal(f.manager.grants.size,0);
  assert.equal(activitySnapshot({relay:f.relay,store:f.store}).operations.display,0);
});
test('An opener completing after cancellation is terminated and cannot revive or strand its owner',async t=>{
  const f=fixture(t),g=await f.grant();let resolve;f.proxmox.openConsole=()=>new Promise(done=>{resolve=done;});
  const client=f.claim(g);client.wire.packet({type:'console-auth',ticket:g.ticket});await settle();assert.equal(typeof resolve,'function');
  f.manager.cancel(f.store.findUser('alice'),tokenHash(f.tokens.alice),'pve-149',g.id);assert.equal(f.manager.grants.size,1);
  const late=new Wire();resolve({socket:late,password:'SERVER_ONLY'});await settle();assert.equal(late.readyState,3);assert.equal(f.manager.grants.size,0);
});
test('Fresh identity replacement refuses an issued grant before any upstream dial',async t=>{
  const f=fixture(t),g=await f.grant();f.proxmox.consoleSnapshot=async()=>({node:'memory',vmid:149,machineUuid:machine(150)});
  const client=f.claim(g);client.wire.packet({type:'console-auth',ticket:g.ticket});await settle();assert.equal(client.wire.readyState,3);assert.equal(f.calls.some(c=>c[0]==='open'),false);
});
test('PC/unknown VM identity is refused even if a future assignment provider returns it',async t=>{
  const f=fixture(t),original=f.store.assignedDesktop.bind(f.store),row=original(f.store.findUser('alice').id,'pve-149');
  f.store.assignedDesktop=()=>({...row,kind:'pc'});await assert.rejects(f.grant(),e=>e.code==='desktop_not_found');
  f.store.assignedDesktop=()=>({...row,pve_machine_uuid:null});await assert.rejects(f.grant(),e=>e.code==='console_identity_unavailable');
});
test('Global8 actual owners cannot be exceeded by different authorized accounts',async t=>{
  const f=fixture(t),names=['alice','bobby','carol','david','elena'];
  f.store.syncProxmox(Array.from({length:9},(_,i)=>({vmid:149+i,node:'memory',name:'Memory VM',status:'running',machineUuid:machine(149+i)})),'fixture');
  for(const name of names){
    if(!f.tokens[name]){f.store.addUser({name,passwordHash:'synthetic-only'});f.tokens[name]=newToken();f.store.createSession(f.store.findUser(name),tokenHash(f.tokens[name]),Date.now(),3600000,'client');}
    for(let vmid=149;vmid<158;vmid++)f.store.assign(name,'pve-'+vmid);
  }
  for(let n=0;n<8;n++)await f.grant(149+n,names[Math.floor(n/2)]);
  assert.equal(f.manager.grants.size,8);await assert.rejects(f.grant(157,'elena'),e=>e.code==='console_limit_reached');assert.equal(f.manager.grants.size,8);
});
test('Rapid cancelled grants are still rate limited without retaining unbounded attempts',async t=>{
  const f=fixture(t);for(let n=0;n<6;n++){const g=await f.grant();f.manager.cancel(f.store.findUser('alice'),tokenHash(f.tokens.alice),'pve-149',g.id);}
  await assert.rejects(f.grant(),e=>e.code==='console_limit_reached');assert.equal(f.manager.grants.size,0);assert.equal(f.manager.attempts.size,1);
});
// '스트리밍 끊고 연결' asks again every second while the stream ends; counted, those refusals would use up the
// six attempts a minute and the takeover would end in console_limit_reached.
test('A refusal while the desktop is busy does not use up the attempt budget',async t=>{
  const f=fixture(t),alice=f.store.findUser('alice');
  f.relay.sessions.set('fixture-stream',{id:'fixture-stream',desktopId:'pve-149',userId:alice.id});
  for(let n=0;n<10;n++)await assert.rejects(f.grant(),e=>e.code==='console_busy');
  assert.equal(f.manager.attempts.size,0);assert.equal(f.manager.grants.size,0);
  f.relay.sessions.delete('fixture-stream');
  const g=await f.grant();assert.equal(f.manager.attempts.get(alice.id).count,1);
  f.manager.cancel(alice,tokenHash(f.tokens.alice),'pve-149',g.id);
});
test('Power/display/console-preparation operations cannot be interleaved under a reserved console',async t=>{
  const f=fixture(t),g=await f.grant();
  assert.throws(()=>f.relay.assertDisplayIdle('pve-149'),e=>e.code==='console_busy');
  assert.throws(()=>f.relay.assertDisplayIdle('pve-149','console'),e=>e.code==='console_busy');
  f.relay.powerBusy=()=>true;f.manager.revalidate();assert.equal(f.manager.grants.has(g.id),false);
  await assert.rejects(f.grant(),e=>e.code==='console_busy');
});
test('VM identity is checked again after dialing before a buffered banner can be authenticated or shown',async t=>{
  const f=fixture(t),g=await f.grant();let calls=0,resolve,upstream;
  f.proxmox.consoleSnapshot=async()=>{if(++calls===2)return new Promise(done=>{resolve=done;});return {node:'memory',vmid:149,machineUuid:machine(149)};};
  f.proxmox.openConsole=async()=>({socket:(upstream=new Wire()),password:'SERVER_ONLY'});
  const client=f.claim(g);client.wire.packet({type:'console-auth',ticket:g.ticket});await settle();assert.equal(typeof resolve,'function');
  upstream.packet(Buffer.from('RFB 003.008\n'),true);assert.equal(upstream.sent.length,0);assert.equal(client.wire.sent.some(v=>String(v.bytes).includes('console-ready')),false);
  resolve({node:'memory',vmid:149,machineUuid:machine(150)});await settle();assert.equal(upstream.readyState,3);assert.equal(client.wire.readyState,3);assert.equal(f.manager.grants.size,0);
});
test('Console diagnostics retain fixed outcomes but never arbitrary upstream error text',()=>{
  const raw={protocol:'client-diagnostics-v1',runId:'c'.repeat(32),sequence:1,version:'0.4.103+117',events:[{time:new Date().toISOString(),event:'brokerRequest',desktopId:'pve-149',requestId:randomUUID(),code:'console_permission_denied',values:{ok:false,ticket:'SERVER_ONLY_SECRET'}}]};
  const safe=safeClientDiagnosticReport(raw);assert.ok(safe);assert.equal(safe.events[0].code,'console_permission_denied');assert.equal(JSON.stringify(safe).includes('SERVER_ONLY_SECRET'),false);
  raw.events[0].code='upstream-secret-text';assert.equal(safeClientDiagnosticReport(raw).events[0].code,'unknown_failure');
});
test('Passive PVE console waits for actual stream profile restore and fresh capabilities, not just worker quiescence',async t=>{
  const f=fixture(t),secret=newToken(),enrollment=newToken();
  f.store.issueEnrollment('pve-149',machine(149),tokenHash(enrollment),'fixture');
  f.store.consumeEnrollment(tokenHash(enrollment),machine(149),{hostname:'MemoryHost',version:'9.0.0',tokenHash:tokenHash(secret)});
  const wire=new Wire();f.relay.attachHost(wire,f.store.authenticateHost(tokenHash(secret)),tokenHash(secret),true);
  const host=f.relay.hosts.get('pve-149');
  const ended={id:randomUUID(),desktopId:'pve-149',host,mode:'stream',hostDispatched:true,stopProtocol:'stop-v1'};
  const record=f.relay.sessionStop.begin(ended);assert.ok(record);
  await assert.rejects(f.grant(),e=>e.code==='console_busy');
  const result=restored=>({type:'host.session.stopped',sessionId:ended.id,sessionStopProtocol:'stop-v1',stopRequestId:record.requestId,workerQuiescent:true,profileRequired:true,profileRestored:restored});
  assert.equal(f.relay.sessionStop.result(host,result(false)),true);
  assert.equal(f.relay.sessionStop.blocked('pve-149','console'),false,'a confirmed console return remains available');
  assert.equal(f.relay.sessionStop.blocked('pve-149','stream'),true);
  await assert.rejects(f.grant(),e=>e.code==='console_busy');
  assert.equal(f.relay.sessionStop.result(host,result(true)),true);
  assert.equal(f.relay.sessionStop.pending.get('pve-149'),record);
  await assert.rejects(f.grant(),e=>e.code==='console_busy');
  assert.equal(f.calls.length,0,'no PVE read or proxy is admitted behind the actual cleanup barrier');
  wire.packet({type:'host.capabilities',capabilities:{service:true,interactiveSession:true,streaming:true,virtualDisplayReady:true,sessionStopProtocol:'stop-v1',virtualMonitorControl:true,monitors:[{id:'0',width:1280,height:720,primary:true,modes:[{width:1280,height:720,refreshRate:60}]}]}});
  assert.equal(f.relay.sessionStop.pending.has('pve-149'),false,'actual validated fresh capabilities complete the second barrier');
  const grant=await f.grant();assert.equal(f.manager.grants.has(grant.id),true);assert.equal(f.calls.length,1);
});

function consoleAudit(f) {
  return f.store.db.prepare("SELECT event,actor,subject,created_at FROM audit_events WHERE event IN ('console_granted','console_ready','console_ended') ORDER BY id").all()
    .map(row=>({...row,subject:JSON.parse(row.subject)}));
}
function serverInit(width=4,height=3) {
  const bytes=Buffer.alloc(24);bytes.writeUInt16BE(width,0);bytes.writeUInt16BE(height,2);
  Buffer.from([32,24,0,1,0,255,0,255,0,255,16,8,0,0,0,0]).copy(bytes,4);return bytes;
}
async function readyConsole(f,g,{width=4,height=3}={}) {
  const client=f.claim(g);client.wire.packet({type:'console-auth',ticket:g.ticket});await settle();
  const value=f.manager.grants.get(g.id),upstream=value.upstream;
  upstream.packet(Buffer.concat([Buffer.from('RFB 003.008\n'),Buffer.from([1,1]),Buffer.alloc(4),serverInit(width,height)]),true);
  await settle();return {client,value,upstream};
}

test('Ready advertises parser capability, not QEMU support, and old clients remain usable', async t => {
  const f = fixture(t), g = await f.grant();
  const { client, value, upstream } = await readyConsole(f, g);
  const ready = client.wire.sent.filter(v => !v.binary).map(v => {
    try { return JSON.parse(String(v.bytes)); } catch { return null; }
  }).find(v => v?.type === 'console-ready');
  assert.equal(ready?.keyboardProtocol, 'qemu-keyboard-v1');
  assert.equal(value.rfb.qemuAcknowledged, false);
  client.wire.packet(Buffer.concat([Buffer.from('RFB 003.008\n'), Buffer.from([1, 1])]), true);
  const oldEncodings = Buffer.alloc(20); oldEncodings[0] = 2; oldEncodings.writeUInt16BE(4, 2);
  [0, 1, -223, -239].forEach((e, i) => oldEncodings.writeInt32BE(e, 4 + i * 4));
  client.wire.packet(oldEncodings, true); await settle();
  assert.equal(value.rfb.qemuRequested, false); assert.equal(value.state, 'active');
  assert.ok(upstream.sent.some(v => Buffer.isBuffer(v.bytes) && v.bytes.equals(oldEncodings)));
});

test('Console access audit correlates grant, upstream-ready and first retirement without secrets or duplicate terminal rows',async t=>{
  const f=fixture(t),g=await f.grant(),actor=f.store.findUser('alice').id;
  // qemu: whether the console-only display was asked of the Host;
  // this fixture has no Host, so it reads host_unsupported.
  assert.deepEqual(consoleAudit(f),[{event:'console_granted',actor,subject:{desktopId:'pve-149',consoleSessionId:g.id,qemu:'host_unsupported'},created_at:f.clock.time}]);
  const {client,value,upstream}=await readyConsole(f,g);
  assert.equal(value.state,'rfb');assert.deepEqual(consoleAudit(f).map(row=>row.event),['console_granted','console_ready']);
  client.wire.packet(Buffer.concat([Buffer.from('RFB 003.008\n'),Buffer.from([1,1])]),true);await settle();
  assert.equal(value.state,'active');assert.equal(consoleAudit(f).filter(row=>row.event==='console_ready').length,1);
  f.manager.cancel(f.store.findUser('alice'),tokenHash(f.tokens.alice),'pve-149',g.id);
  f.manager.retire(value,'console_timeout');upstream.emit('close');await settle();
  const rows=consoleAudit(f);assert.deepEqual(rows.map(row=>row.event),['console_granted','console_ready','console_ended']);
  assert.deepEqual(rows[2],{event:'console_ended',actor,subject:{desktopId:'pve-149',consoleSessionId:g.id,code:'console_cancelled'},created_at:f.clock.time});
  assert.equal(f.manager.grants.has(g.id),false);assert.equal(client.wire.readyState,3);assert.equal(upstream.readyState,3);
  const text=JSON.stringify(rows);
  for(const secret of [g.ticket,f.tokens.alice,tokenHash(f.tokens.alice),'SERVER_ONLY','broker.invalid','memory.invalid','fixture@pve',machine(149)])assert.equal(text.includes(secret),false);
  for(const row of rows)assert.deepEqual(Object.keys(row.subject).sort(),row.event==='console_ended'?['code','consoleSessionId','desktopId']:['consoleSessionId','desktopId','qemu']);
});

test('Failed preparation audits only a bounded end code, never grant/ready or arbitrary upstream errors',async t=>{
  const f=fixture(t);f.proxmox.consoleSnapshot=async()=>{throw Object.assign(new Error('PRIVATE_UPSTREAM_URL_PASSWORD'),{code:'proxmox_permission_denied'});};
  await assert.rejects(f.grant(),error=>error.code==='console_permission_denied');
  const rows=consoleAudit(f);assert.equal(rows.length,1);assert.equal(rows[0].event,'console_ended');
  assert.equal(rows[0].actor,f.store.findUser('alice').id);assert.equal(rows[0].subject.desktopId,'pve-149');
  assert.equal(rows[0].subject.code,'console_permission_denied');assert.equal(JSON.stringify(rows).includes('PRIVATE_UPSTREAM'),false);
  assert.equal(f.manager.grants.size,0);assert.equal(f.calls.some(call=>call[0]==='open'),false);
});

test('Foreign upgrade and repeated old retirement cannot forge or overwrite another console security history',async t=>{
  const f=fixture(t),g=await f.grant(),other=await f.grant(150,'bobby'),value=f.manager.grants.get(g.id);
  assert.deepEqual(f.claim(g,'bobby').rejected,[401]);assert.equal(consoleAudit(f).some(row=>row.event==='console_ended'),false);
  const client=f.claim(g);client.wire.packet({type:'console-auth',ticket:newToken()});await settle();
  f.manager.retire(value,'authorization_revoked');
  const ended=consoleAudit(f).filter(row=>row.event==='console_ended');assert.equal(ended.length,1);
  assert.deepEqual(ended[0].subject,{desktopId:'pve-149',consoleSessionId:g.id,code:'console_auth_failed'});
  assert.equal(ended[0].actor,f.store.findUser('alice').id);assert.equal(f.manager.grants.has(other.id),true);
  assert.equal(consoleAudit(f).some(row=>row.event==='console_ready'),false);
  assert.equal(consoleAudit(f).filter(row=>row.subject.consoleSessionId===other.id).length,1);
});

test('Unknown retirement reasons are mapped to a fixed audit code rather than arbitrary text',async t=>{
  const f=fixture(t),g=await f.grant(),value=f.manager.grants.get(g.id);
  f.manager.retire(value,'console_PRIVATE_PASSWORD_ADDRESS');
  const ended=consoleAudit(f).find(row=>row.event==='console_ended');
  assert.equal(ended.subject.code,'console_unavailable');assert.equal(JSON.stringify(consoleAudit(f)).includes('PRIVATE'),false);
  assert.equal(f.manager.grants.size,0);
});

test('Audit storage failure cannot change authorization, upstream readiness or actual owner cleanup',async t=>{
  const f=fixture(t),attempted=[],original=f.store.audit.bind(f.store);
  f.store.audit=(event,...args)=>{if(['console_granted','console_ready','console_ended'].includes(event)){attempted.push(event);throw new Error('PRIVATE_AUDIT_IO_ERROR');}return original(event,...args);};
  const g=await f.grant(),{client,value,upstream}=await readyConsole(f,g);
  assert.equal(value.state,'rfb');assert.equal(value.closed,false);
  assert.deepEqual(f.manager.cancel(f.store.findUser('alice'),tokenHash(f.tokens.alice),'pve-149',g.id),{id:g.id,released:true});
  await settle();assert.equal(client.wire.readyState,3);assert.equal(upstream.readyState,3);assert.equal(f.manager.grants.size,0);
  assert.deepEqual(attempted,['console_granted','console_ready','console_ended']);assert.deepEqual(consoleAudit(f),[]);
});

test('End audit means admission retirement, while a cancelled actual opener still retains its slot',async t=>{
  const f=fixture(t),g=await f.grant();let resolve;f.proxmox.openConsole=()=>new Promise(done=>{resolve=done;});
  const client=f.claim(g);client.wire.packet({type:'console-auth',ticket:g.ticket});await settle();assert.equal(typeof resolve,'function');
  f.manager.cancel(f.store.findUser('alice'),tokenHash(f.tokens.alice),'pve-149',g.id);
  assert.equal(f.manager.grants.size,1);assert.deepEqual(consoleAudit(f).map(row=>row.event),['console_granted','console_ended']);
  const late=new Wire();resolve({socket:late,password:'PRIVATE_LATE_PASSWORD'});await settle();
  assert.equal(late.readyState,3);assert.equal(f.manager.grants.size,0);
  assert.deepEqual(consoleAudit(f).map(row=>row.event),['console_granted','console_ended']);
});

// Actual Relay/Store/RFB/WriteQueue seam with no listener or native socket.
function pacingClock(f) {
  let next=0;const timers=new Map();
  f.manager.readSetTimer=(fn,ms)=>{const timer={id:++next,at:f.clock.time+ms,fn,unref(){}};timers.set(timer.id,timer);return timer;};
  f.manager.readClearTimer=timer=>timers.delete(timer.id);
  return {timers,advance(ms){const target=f.clock.time+ms;for(let n=0;n<10000;n++){
    const due=[...timers.values()].filter(timer=>timer.at<=target).sort((a,b)=>a.at-b.at)[0];
    if(!due){f.clock.time=target;return;}f.clock.time=due.at;timers.delete(due.id);due.fn();
  }throw new Error('memory pacing clock did not settle');}};
}
async function activePacedConsole(f,g,options={width:1280,height:800}) {
  const result=await readyConsole(f,g,options);
  result.client.wire.packet(Buffer.concat([Buffer.from('RFB 003.008\n'),Buffer.from([1,1])]),true);
  await settle();assert.equal(result.value.state,'active');return result;
}
function rawFrame(fill=0) {
  const bytes=Buffer.alloc(16+1280*800*4,fill);bytes.fill(0,0,16);bytes.writeUInt16BE(1,2);
  bytes.writeUInt16BE(1280,8);bytes.writeUInt16BE(800,10);return bytes;
}
const endSubject=f=>consoleAudit(f).find(row=>row.event==='console_ended')?.subject;

for (const side of ['client', 'upstream']) test(`Console end diagnostics: ${side} close records only its numeric code and first phase`, async t => {
  const f=fixture(t),g=await f.grant(),{client,value,upstream}=await activePacedConsole(f,g);
  const wire=side==='client'?client.wire:upstream;
  wire.readyState=3;wire.emit('close',1011,Buffer.from('PRIVATE_URL_PASSWORD_OR_DESKTOP'));
  f.manager.retire(value,'console_timeout',undefined,{trigger:'session_deadline'});
  await settle();
  assert.deepEqual(endSubject(f),{desktopId:'pve-149',consoleSessionId:g.id,code:'console_transport_closed',phase:'active',trigger:side+'_close',transportCode:1011});
  assert.equal(JSON.stringify(consoleAudit(f)).includes('PRIVATE'),false);
  assert.equal(consoleAudit(f).filter(row=>row.event==='console_ended').length,1);
  assert.equal(f.manager.grants.has(g.id),false);
});

for (const side of ['client', 'upstream']) test(`Console end diagnostics: ${side} error never stores arbitrary exception text`, async t => {
  const f=fixture(t),g=await f.grant(),{client,upstream}=await activePacedConsole(f,g);
  (side==='client'?client.wire:upstream).emit('error',new Error('PRIVATE_UPSTREAM_URL_PASSWORD'));
  await settle();
  assert.equal(endSubject(f).trigger,side+'_error');assert.equal(endSubject(f).phase,'active');
  assert.equal(Object.hasOwn(endSubject(f),'transportCode'),false);
  assert.equal(JSON.stringify(consoleAudit(f)).includes('PRIVATE'),false);
});

for (const phase of ['issued','auth','opening','rfb','active']) test(`Console end diagnostics: ${phase} deadline is distinguishable without changing its deadline`, async t => {
  const f=fixture(t),g=await f.grant(),value=f.manager.grants.get(g.id);
  if(phase!=='issued')f.claim(g);
  value.state=phase;value.checkedAt=f.clock.time;
  if(phase==='issued')value.expiresAt=f.clock.time;
  else if(phase==='active'){value.sessionDeadline=f.clock.time;value.pongAt=f.clock.time;}
  else value.deadline=f.clock.time;
  f.manager.revalidate();await settle();
  assert.equal(endSubject(f).code,'console_timeout');assert.equal(endSubject(f).phase,phase);
  assert.equal(endSubject(f).trigger,phase==='issued'?'grant_deadline':phase==='active'?'session_deadline':phase==='auth'?'auth_deadline':'handshake_deadline');
});

test('Console end diagnostics: heartbeat expiry is distinct from the one-hour session ceiling',async t=>{
  const f=fixture(t),g=await f.grant(),{value}=await activePacedConsole(f,g);
  f.clock.time+=30001;value.checkedAt=f.clock.time;
  f.manager.revalidate();await settle();
  assert.equal(endSubject(f).trigger,'pong_deadline');assert.equal(endSubject(f).phase,'active');
  assert.equal(endSubject(f).code,'console_timeout');
});

test('Console end diagnostics: failed background identity refresh is identified and still fails closed',async t=>{
  const f=fixture(t),g=await f.grant(),{value}=await activePacedConsole(f,g);
  f.proxmox.consoleSnapshot=async()=>{throw new Error('PRIVATE_API_URL_PASSWORD');};
  f.clock.time+=5001;f.manager.revalidate();await settle();
  assert.equal(endSubject(f).trigger,'identity_check');assert.equal(endSubject(f).phase,'active');
  assert.equal(endSubject(f).code,'console_unavailable');assert.equal(value.closed,true);
  assert.equal(f.manager.grants.has(g.id),false);assert.equal(JSON.stringify(consoleAudit(f)).includes('PRIVATE'),false);
});

test('Console end diagnostics: unrecognized metadata and invalid close codes cannot reach audit storage',async t=>{
  const f=fixture(t),g=await f.grant(),value=f.manager.grants.get(g.id);
  f.manager.retire(value,'console_timeout',undefined,{trigger:'PRIVATE_ADDRESS',phase:'PRIVATE_STATE',transportCode:1011});
  assert.deepEqual(endSubject(f),{desktopId:'pve-149',consoleSessionId:g.id,code:'console_timeout'});
  for(const transportCode of [999,5000,NaN,'1011']){
    const h=fixture(t),grant=await h.grant(),v=h.manager.grants.get(grant.id);
    h.manager.retire(v,'console_transport_closed',undefined,{trigger:'upstream_close',transportCode});
    assert.equal(Object.hasOwn(endSubject(h),'transportCode'),false);assert.equal(endSubject(h).trigger,'upstream_close');
  }
});

test('Pacing actual 1280x800 raw frames preserves bytes above old 64MiB window without retiring the grant',async t=>{
  const f=fixture(t),clock=pacingClock(f),g=await f.grant(),{client,value,upstream}=await activePacedConsole(f,g);
  const start=client.wire.sent.length,expected=createHash('sha256'),actual=createHash('sha256');let expectedBytes=0;
  for(let n=0;n<18;n++){
    for(let step=0;upstream.paused&&step<1000;step++){clock.advance(16);await settle();}
    assert.equal(upstream.paused,false);
    const frame=rawFrame(n);expected.update(frame);expectedBytes+=frame.length;upstream.packet(frame,true);await settle();
    assert.equal(value.closed,false,'ordinary raw output must wait for credit');
  }
  for(let n=0;upstream.paused&&n<1000;n++){clock.advance(16);await settle();}
  assert.equal(upstream.paused,false);let actualBytes=0;
  for(const packet of client.wire.sent.slice(start)){assert.equal(packet.binary,true);actual.update(packet.bytes);actualBytes+=packet.bytes.length;}
  assert.ok(expectedBytes>64*1024*1024);assert.equal(actualBytes,expectedBytes);assert.equal(actual.digest('hex'),expected.digest('hex'));
  assert.ok(upstream.pauses>0);assert.ok(upstream.resumes>0);assert.equal(value.closed,false);assert.equal(endSubject(f),undefined);
});

test('Delayed pacing delivery rechecks revoked assignment and late timers cannot resume or repaint',async t=>{
  const f=fixture(t),clock=pacingClock(f),g=await f.grant(),{client,value,upstream}=await activePacedConsole(f,g);
  for(let n=0;n<5;n++){upstream.packet(rawFrame(n),true);await settle();}
  assert.equal(upstream.paused,true);const late=[...clock.timers.values()][0].fn;
  const before=client.wire.sent.filter(packet=>packet.binary).length,resumes=upstream.resumes;
  f.store.revoke('alice','pve-149');clock.advance(16);await settle();
  assert.equal(value.closed,true);assert.equal(endSubject(f).code,'authorization_revoked');
  late();clock.advance(1000);await settle();
  assert.equal(client.wire.sent.filter(packet=>packet.binary).length,before);assert.equal(upstream.resumes,resumes);
  assert.equal(clock.timers.size,0);assert.equal(upstream.readyState,3);assert.equal(f.manager.grants.size,0);
});

test('Paced actual slow downstream retains WriteQueue bound and records only output_queue scope',async t=>{
  const f=fixture(t),clock=pacingClock(f),g=await f.grant(),{client,value,upstream}=await activePacedConsole(f,g);
  const pending=[];client.wire.send=(bytes,options,done)=>{client.wire.sent.push({bytes:Buffer.from(bytes),binary:options.binary});pending.push(done);};
  for(let n=0;n<8&&!value.closed;n++){
    for(let step=0;upstream.paused&&!value.closed&&step<1000;step++){clock.advance(16);await settle();}
    if(!value.closed)assert.equal(upstream.paused,false);
    if(!value.closed)upstream.packet(rawFrame(n),true);await settle();
  }
  assert.equal(value.closed,true);assert.equal(endSubject(f).code,'console_backpressure');assert.equal(endSubject(f).limitScope,'output_queue');
  assert.equal(value.clientQueue.bytes,0);assert.equal(clock.timers.size,0);const sends=client.wire.sent.length;
  f.manager.retire(value,'console_cancelled');for(const done of pending)done?.();clock.advance(1000);await settle();
  assert.equal(client.wire.sent.length,sends);assert.equal(upstream.readyState,3);
});

test('Actual relay forwards a valid large first Raw update through a slow compliant sink without raising its16MiB bounds', async t => {
  const f=fixture(t),clock=pacingClock(f),g=await f.grant();
  const {client,value,upstream}=await activePacedConsole(f,g,{width:3840,height:2160});
  const frame=Buffer.alloc(16+3840*2160*4,0x43);frame.fill(0,0,16);frame.writeUInt16BE(1,2);
  frame.writeUInt16BE(3840,8);frame.writeUInt16BE(2160,10);
  const expected=createHash('sha256').update(frame).digest('hex'),actual=createHash('sha256'),pending=[],controls=[];
  let sentBytes=0,maxWriter=0,maxReader=0;
  client.wire.send=(bytes,options,done)=>{
    if(options.binary){actual.update(bytes);sentBytes+=bytes.length;}else controls.push(String(bytes));
    pending.push(done);
  };
  for(let at=0;at<frame.length&&!value.closed;at+=65536){
    for(let wait=0;upstream.paused&&!value.closed&&wait<4096;wait++){
      if(pending.length)pending.shift()();clock.advance(1);await settle();
    }
    assert.equal(upstream.paused,false);assert.equal(value.closed,false);
    upstream.packet(frame.subarray(at,at+65536),true);await settle();
    maxWriter=Math.max(maxWriter,value.clientQueue.bytes);maxReader=Math.max(maxReader,value.readBudget.bytes);
  }
  for(let step=0;(pending.length||value.readBudget.parts.length||value.readBudget.timer!==null)&&!value.closed&&step<4096;step++){
    if(pending.length)pending.shift()();clock.advance(1);await settle();
  }
  assert.equal(value.closed,false);assert.equal(sentBytes,frame.length);assert.equal(actual.digest('hex'),expected);
  assert.ok(upstream.pauses>0);assert.ok(upstream.resumes>0);
  assert.ok(maxWriter<=16*1024*1024);assert.ok(maxReader<=16*1024*1024);
  assert.equal(value.clientQueue.bytes,0);assert.equal(value.readBudget.bytes,0);assert.equal(clock.timers.size,0);
  assert.deepEqual(controls,[]);assert.equal(endSubject(f),undefined);
});

test('Actual assignment revocation on a sink-blocked write completion retires before another queued pixel send and retains its closing slot', async t => {
  const f=fixture(t),clock=pacingClock(f),g=await f.grant(),{client,value,upstream}=await activePacedConsole(f,g);
  const pending=[];let binarySends=0;
  client.wire.send=(bytes,options,done)=>{if(options.binary)binarySends++;pending.push(done);};
  for(let n=0;n<8&&!upstream.paused&&!value.closed;n++){upstream.packet(rawFrame(n),true);await settle();}
  assert.equal(value.closed,false);assert.equal(value.readBudget.sinkBlocked,true);assert.equal(pending.length,1);
  const before=binarySends,resumes=upstream.resumes;
  f.store.revoke('alice','pve-149');pending.shift()();await settle();
  assert.equal(value.closed,true);assert.equal(endSubject(f).code,'authorization_revoked');assert.equal(binarySends,before);
  assert.equal(upstream.resumes,resumes);assert.equal(upstream.readyState,3);assert.equal(clock.timers.size,0);
  assert.equal(f.manager.grants.has(g.id),true,'the actual client closing transport still owns its slot');
  for(const done of pending)done?.();await settle();
  assert.equal(client.wire.readyState,3);assert.equal(f.manager.grants.has(g.id),false);assert.equal(binarySends,before);
});

test('Input flood has a fixed input_rate scope without logging packets or changing other grants',async t=>{
  const f=fixture(t),g=await f.grant(),other=await f.grant(150,'bobby'),{client,value}=await activePacedConsole(f,g);
  for(let n=0;n<1001&&!value.closed;n++)client.wire.packet(Buffer.alloc(0),true);
  assert.equal(value.closed,true);assert.deepEqual(endSubject(f),{desktopId:'pve-149',consoleSessionId:g.id,code:'console_limits_exceeded',limitScope:'input_rate'});
  assert.equal(f.manager.grants.has(other.id),true);
});

test('Identity-pending bytes retain their 64KiB bound and fixed identity_buffer scope',async t=>{
  const f=fixture(t),g=await f.grant();let calls=0,resolve;
  f.proxmox.consoleSnapshot=async()=>{if(++calls===2)return new Promise(done=>{resolve=done;});return {node:'memory',vmid:149,machineUuid:machine(149)};};
  const client=f.claim(g);client.wire.packet({type:'console-auth',ticket:g.ticket});await settle();
  const value=f.manager.grants.get(g.id);value.upstream.packet(Buffer.alloc(65537),true);
  assert.equal(value.closed,true);assert.equal(endSubject(f).limitScope,'identity_buffer');assert.equal(value.pending,1);
  resolve({node:'memory',vmid:149,machineUuid:machine(149)});await settle();assert.equal(f.manager.grants.size,0);
});

test('Identity-pending empty messages consume no prefix entries and nonempty packets remain bounded',async t=>{
  const f=fixture(t),g=await f.grant();let calls=0,resolve;
  f.proxmox.consoleSnapshot=async()=>{if(++calls===2)return new Promise(done=>{resolve=done;});return {node:'memory',vmid:149,machineUuid:machine(149)};};
  const client=f.claim(g);client.wire.packet({type:'console-auth',ticket:g.ticket});await settle();
  const value=f.manager.grants.get(g.id);
  for(let n=0;n<1000;n++)value.upstream.packet(Buffer.alloc(0),true);
  assert.equal(value.closed,false);assert.equal(value.upstream.sent.length,0);
  for(let n=0;n<256;n++)value.upstream.packet(Buffer.from([0]),true);
  assert.equal(value.closed,false);
  value.upstream.packet(Buffer.from([0]),true);
  assert.equal(value.closed,true);assert.equal(endSubject(f).limitScope,'identity_buffer');assert.equal(value.pending,1);
  resolve({node:'memory',vmid:149,machineUuid:machine(149)});await settle();assert.equal(f.manager.grants.size,0);
});

test('Actual parser rectangle-count refusal gets rfb_parser scope rather than output-rate blame',async t=>{
  const f=fixture(t),g=await f.grant(),{value,upstream}=await activePacedConsole(f,g);
  upstream.packet(Buffer.from([0,0,0x10,1]),true);
  assert.equal(value.closed,true);assert.equal(endSubject(f).code,'console_limits_exceeded');assert.equal(endSubject(f).limitScope,'rfb_parser');
});

test('Oversized paced backlog has output_queue scope and unknown scope strings never enter audit',async t=>{
  const f=fixture(t),clock=pacingClock(f),g=await f.grant(),{value,upstream}=await activePacedConsole(f,g);
  for(let n=0;n<10&&!value.closed;n++)upstream.packet(rawFrame(n),true); // Deliberately ignore pause, unlike a compliant peer.
  assert.equal(value.closed,true);assert.equal(endSubject(f).limitScope,'output_queue');assert.equal(clock.timers.size,0);
  const other=await f.grant(150,'bobby'),otherValue=f.manager.grants.get(other.id);
  f.manager.retire(otherValue,'console_limits_exceeded','PRIVATE_KEYS_OR_PIXELS');
  const ended=consoleAudit(f).filter(row=>row.event==='console_ended').at(-1);assert.equal('limitScope' in ended.subject,false);
  assert.equal(JSON.stringify(consoleAudit(f)).includes('PRIVATE_KEYS_OR_PIXELS'),false);
});

function claimBrowser(f, grant, { name='alice', hash=tokenHash(f.tokens[name]), origin='https://broker.invalid', headerOrigin=origin, url='/console/ws/'+grant.id }={}) {
  const wire=new Wire(),rejected=[];
  f.manager.wss.handleUpgrade=(_req,_socket,_head,done)=>done(wire);
  f.manager.upgradeBrowser({url,brokerOrigin:origin,headers:{origin:headerOrigin}},{},Buffer.alloc(0),grant.id,status=>rejected.push(status),{user:f.store.findUser(name),hash,origin});
  return {wire,rejected};
}
function browserServerInit(){
  const bytes=Buffer.alloc(24);bytes.writeUInt16BE(2,0);bytes.writeUInt16BE(2,2);
  Buffer.from([32,24,0,1,0,255,0,255,0,255,16,8,0,0,0,0]).copy(bytes,4);return bytes;
}
async function activeBrowser(f, grant){
  grant??=await f.grant(149,'alice',{profile:'novnc'});
  const client=claimBrowser(f,grant);assert.deepEqual(client.rejected,[]);await settle();
  const value=f.manager.grants.get(grant.id),upstream=value.upstream;
  upstream.packet(Buffer.concat([Buffer.from('RFB 003.008\n'),Buffer.from([1,1]),Buffer.alloc(4),browserServerInit()]),true);await settle();
  assert.equal(value.state,'rfb');
  client.wire.packet(Buffer.concat([Buffer.from('RFB 003.008\n'),Buffer.from([1,1])]),true);await settle();
  assert.equal(value.state,'active');return {grant,client,value,upstream};
}
function browserText(text,client=true){const body=Buffer.from(text,'latin1'),header=Buffer.alloc(8);header[0]=client?6:3;header.writeUInt32BE(body.length,4);return Buffer.concat([header,body]);}
function textPolicy(textToRemote=false,textToLocal=false){return {textToRemote,textToLocal,filesToRemote:false,filesToLocal:false};}

test('Browser profile is explicit, private, origin/user/token bound and one use; native grants cannot be downgraded',async t=>{
  const f=fixture(t),g=await f.grant(149,'alice',{profile:'novnc'});
  assert.deepEqual(g.clipboardPolicy,{textToRemote:false,textToLocal:false});
  const value=f.manager.grants.get(g.id);assert.equal(value.profile,'novnc');assert.equal(value.ticketHash,tokenHash(g.ticket));
  assert.deepEqual(f.claim(g).rejected,[401]);assert.equal(value.state,'issued');
  for(const options of [{name:'bobby'},{hash:tokenHash(newToken())},{origin:'https://foreign.invalid'}]){
    assert.deepEqual(claimBrowser(f,g,options).rejected,[401]);assert.equal(value.state,'issued');assert.equal(value.closed,false);
  }
  for(const options of [{headerOrigin:'https://foreign.invalid'},{url:'/console/ws/'+g.id+'?ticket=PRIVATE'},{origin:'http://broker.invalid'}]){
    assert.deepEqual(claimBrowser(f,g,options).rejected,[403]);assert.equal(value.state,'issued');
  }
  const claimed=claimBrowser(f,g);assert.deepEqual(claimed.rejected,[]);assert.equal(value.ticketHash,null);assert.equal(value.state,'opening');
  assert.deepEqual(claimBrowser(f,g).rejected,[401]);assert.equal(claimed.wire.readyState,1);
  const native=await f.grant(150,'alice');assert.deepEqual(claimBrowser(f,native).rejected,[401]);assert.deepEqual(f.claim(native).rejected,[]);
});

test('Browser issued expiry and profile validation fail without spending an unrelated grant',async t=>{
  const f=fixture(t);await assert.rejects(f.grant(149,'alice',{profile:'untrusted'}),e=>e.code==='invalid_console_profile');
  const g=await f.grant(149,'alice',{profile:'novnc'});f.clock.time+=30001;
  assert.deepEqual(claimBrowser(f,g).rejected,[401]);assert.equal(f.calls.some(c=>c[0]==='open'),false);
  f.manager.revalidate();assert.equal(f.manager.grants.size,0);
});

test('Browser starts only binary RFB, never native auth/ready/end JSON, and close reasons are fixed',async t=>{
  const f=fixture(t),{client,value,upstream}=await activeBrowser(f);
  assert.ok(client.wire.sent.length);assert.ok(client.wire.sent.every(v=>v.binary));
  assert.equal(Buffer.concat(client.wire.sent.map(v=>v.bytes)).includes(Buffer.from('console-ready')),false);
  assert.deepEqual(upstream.sent.find(v=>v.bytes[0]===0)?.bytes.subarray(4),Buffer.from([32,24,0,1,0,255,0,255,0,255,0,8,16,0,0,0]));
  f.manager.retire(value,'PRIVATE_ARBITRARY_REASON');
  assert.equal(client.wire.closeCode,1008);assert.equal(client.wire.closeReason,'console_unavailable');
  assert.ok(client.wire.sent.every(v=>v.binary));assert.equal(f.manager.grants.size,0);
});

test('Browser JSON native preamble is refused without exposing a ticket or arbitrary end message',async t=>{
  const f=fixture(t),g=await f.grant(149,'alice',{profile:'novnc'}),client=claimBrowser(f,g);await settle();
  client.wire.packet({type:'console-auth',ticket:g.ticket});
  assert.equal(client.wire.closeReason,'console_protocol_error');assert.equal(client.wire.sent.some(v=>!v.binary),false);
  assert.equal(client.wire.sent.some(v=>String(v.bytes).includes(g.ticket)),false);
});

test('Browser default clipboard denial drops server text and refuses client paste; file permissions do not imply text',async t=>{
  const f=fixture(t);f.store.updateAssignmentPolicy('alice','pve-149',{clipboardPolicy:{...textPolicy(),filesToRemote:true,filesToLocal:true}},'fixture');
  const {client,upstream}=await activeBrowser(f),before=client.wire.sent.length;
  upstream.packet(Buffer.concat([browserText('PRIVATE_SERVER_TEXT',false),Buffer.from([2])]),true);await settle();
  assert.deepEqual(Buffer.concat(client.wire.sent.slice(before).map(v=>v.bytes)),Buffer.from([2]));
  client.wire.packet(browserText('PRIVATE_CLIENT_TEXT'),true);
  assert.equal(client.wire.closeReason,'console_clipboard_denied');assert.equal(upstream.sent.some(v=>v.bytes.includes(Buffer.from('PRIVATE_CLIENT_TEXT'))),false);
});

for(const toRemote of [true,false])test(`Browser per-assignment text direction is enforced and revoked on revalidation (toRemote=${toRemote})`,async t=>{
  const f=fixture(t);f.store.updateAssignmentPolicy('alice','pve-149',{clipboardPolicy:textPolicy(toRemote,!toRemote)},'fixture');
  const {grant,client,value,upstream}=await activeBrowser(f);
  assert.deepEqual(grant.clipboardPolicy,{textToRemote:toRemote,textToLocal:!toRemote});
  const text=browserText('LATIN1_\xff',toRemote),before=toRemote?upstream.sent.length:client.wire.sent.length;
  if(toRemote)client.wire.packet(text,true);else upstream.packet(text,true);await settle();
  const output=(toRemote?upstream:client.wire).sent.slice(before);assert.deepEqual(Buffer.concat(output.map(v=>v.bytes)),text);
  const other=await f.grant(150,'bobby',{profile:'novnc'});
  f.store.updateAssignmentPolicy('alice','pve-149',{clipboardPolicy:textPolicy()},'fixture');f.manager.revalidate();await settle();
  assert.equal(value.closed,true);assert.equal(client.wire.closeReason,'console_clipboard_denied');assert.equal(f.manager.grants.has(other.id),true);
});

test('Browser clipboard grant cannot grow in place and a revoked fragment cannot reach QEMU',async t=>{
  const f=fixture(t),first=await activeBrowser(f);
  f.store.updateAssignmentPolicy('alice','pve-149',{clipboardPolicy:textPolicy(true)},'fixture');
  assert.equal(first.value.clipboardPolicy.textToRemote,false);first.client.wire.packet(browserText('NOT_ADMITTED'),true);
  assert.equal(first.client.wire.closeReason,'console_clipboard_denied');await settle();
  const second=await activeBrowser(f),text=browserText('PRIVATE_FRAGMENT'),before=second.upstream.sent.length;
  second.client.wire.packet(text.subarray(0,9),true);assert.equal(second.upstream.sent.length,before);
  f.store.updateAssignmentPolicy('alice','pve-149',{clipboardPolicy:textPolicy()},'fixture');
  second.client.wire.packet(text.subarray(9),true);assert.equal(second.upstream.sent.length,before);assert.equal(second.client.wire.closeReason,'console_clipboard_denied');
});

test('Browser server-text revocation stops later upstream chunks before another client byte is queued',async t=>{
  const f=fixture(t);f.store.updateAssignmentPolicy('alice','pve-149',{clipboardPolicy:textPolicy(false,true)},'fixture');
  const {client,upstream}=await activeBrowser(f),text=browserText('A_PRIVATE_REMAINDER',false),before=client.wire.sent.length;
  upstream.packet(text.subarray(0,9),true);await settle();
  assert.deepEqual(Buffer.concat(client.wire.sent.slice(before).map(v=>v.bytes)),text.subarray(0,9));
  const admitted=client.wire.sent.length;
  f.store.updateAssignmentPolicy('alice','pve-149',{clipboardPolicy:textPolicy()},'fixture');
  upstream.packet(text.subarray(9),true);await settle();
  assert.equal(client.wire.sent.length,admitted);assert.equal(client.wire.closeReason,'console_clipboard_denied');
});

test('Browser late upstream opener cancellation retains its slot until actual close without a JSON end packet',async t=>{
  const f=fixture(t),g=await f.grant(149,'alice',{profile:'novnc'});let resolve;f.proxmox.openConsole=()=>new Promise(done=>{resolve=done;});
  const client=claimBrowser(f,g);await settle();assert.equal(typeof resolve,'function');
  f.manager.cancel(f.store.findUser('alice'),tokenHash(f.tokens.alice),'pve-149',g.id);assert.equal(f.manager.grants.size,1);assert.equal(client.wire.closeReason,'console_cancelled');
  const late=new Wire();resolve({socket:late,password:'PRIVATE_VNC_PASSWORD'});await settle();
  assert.equal(late.readyState,3);assert.equal(f.manager.grants.size,0);assert.equal(client.wire.sent.some(v=>!v.binary),false);
});

test('Browser reuses pong and session lifetime deadlines with automatic WebSocket control frames only',async t=>{
  const f=fixture(t),{client,value}=await activeBrowser(f);let pings=0;client.wire.ping=()=>++pings;
  f.clock.time+=10001;client.wire.emit('pong');f.manager.revalidate();await settle();assert.equal(pings,1);
  f.clock.time=value.sessionDeadline;client.wire.emit('pong');f.manager.revalidate();await settle();
  assert.equal(client.wire.closeReason,'console_timeout');assert.equal(client.wire.sent.some(v=>!v.binary),false);
});
