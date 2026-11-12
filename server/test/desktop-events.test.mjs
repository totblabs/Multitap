import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { createServer } from 'node:http';
import { createServer as createTlsServer } from 'node:https';
import { spawnSync } from 'node:child_process';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, dirname, resolve, basename } from 'node:path';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { DesktopEvents, desktopFingerprint } from '../src/desktop-events.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { enrollFixtureHosts } from './fixtures/admin-assignment-input.mjs';

const origin='https://events-fixture.example';
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
const machineUuid=vmid=>'12345678-1234-4234-8234-'+String(vmid).padStart(12,'0');
const vm=(vmid,status='running')=>({vmid,node:'fixture-node',name:`VM${vmid}`,status,machineUuid:machineUuid(vmid)});
function users(store) {
  const ids={},tokens={};
  for(const name of ['alice','bobby','carol','admin']) {
    ids[name]=store.addUser({name,passwordHash:'isolated-events-password-fixture',role:name==='admin'?'admin':'user'});
    tokens[name]=newToken();store.createSession(store.findUser(name),tokenHash(tokens[name]),Date.now(),3600000,name==='admin'?'admin':'client');
  }
  store.syncProxmox([vm(710),vm(711)],'fixture');store.assign('alice','pve-710','fixture');store.assign('bobby','pve-711','fixture');
  enrollFixtureHosts(store,['pve-710','pve-711']);
  return {ids,tokens};
}
async function connect(url,token,options={}) {
  const ws=new WebSocket(url,{headers:{Authorization:'Bearer '+token,Origin:origin},perMessageDeflate:false,...options});
  const queue=[],waiters=[];
  ws.closed=new Promise(resolve=>ws.once('close',(code,reason)=>resolve({code,reason:reason.toString()})));
  ws.on('message',(bytes,binary)=>{
    const value=binary?bytes:JSON.parse(bytes.toString());const waiter=waiters.shift();
    if(waiter){clearTimeout(waiter.timer);waiter.resolve(value);}else queue.push(value);
  });
  ws.take=()=>queue.length?Promise.resolve(queue.shift()):new Promise((resolve,reject)=>{
    const waiter={resolve,timer:setTimeout(()=>{const index=waiters.indexOf(waiter);if(index>=0)waiters.splice(index,1);reject(new Error('event_timeout'));},3000)};waiters.push(waiter);
  });
  ws.queue=queue;
  await new Promise((resolve,reject)=>{ws.once('open',resolve);ws.once('error',reject);ws.once('unexpected-response',(_req,response)=>{response.resume();reject(Object.assign(new Error('upgrade_rejected'),{status:response.statusCode}));});});
  return ws;
}
async function fixture(t,{eventOptions={},standalone=false,tls}={}) {
  const store=new Store(':memory:'),vault=new Vault(randomBytes(32)),identity=users(store),sockets=[];
  let now=Date.now(),inventory=[vm(710),vm(711)],reads=0,taskResult={status:'running',exitStatus:null};
  store.saveSetting('proxmox',vault.seal({endpoint:'https://isolated-pve.example',tokenId:'fixture@pve!events',secret:'isolated-events-token-fixture'}),'fixture');
  const proxmox={inventory:async(_cfg,{targets})=>{reads++;assert.ok(Array.isArray(targets));return targets.map(target=>{assert.deepEqual(target,{node:'fixture-node',vmid:target.vmid,expectedUuid:machineUuid(target.vmid)});const row=inventory.find(row=>row.vmid===target.vmid);assert.ok(row);return{...row};});},power:async(_cfg,id,action,{beforeSubmit,node,expectedUuid})=>{assert.equal(node,'fixture-node');assert.equal(expectedUuid,machineUuid(id));beforeSubmit();return{node:'fixture-node',task:`UPID:fixture-node:1:2:3:qm${action}:${id}:fixture@pve:`};},task:async()=>taskResult};
  let events,server;
  if(standalone) {
    events=new DesktopEvents({store,publicOrigin:origin,refresh:async()=>{},snapshot:async hash=>store.listDesktops(store.authenticate(hash).id),tickMs:20,authorizationMs:20,heartbeatMs:1000,...eventOptions});
    server=tls?createTlsServer(tls):createServer();
    server.on('upgrade',(req,socket,head)=>{const reject=(status,reason)=>{socket.end(`HTTP/1.1 ${status} ${reason}\r\nConnection:close\r\nContent-Length:0\r\n\r\n`);};if(!events.upgrade(req,socket,head,new URL(req.url,'http://fixture'),reject))reject(404,'Not Found');});
  } else server=await buildServer({store,vault,proxmox,publicOrigin:origin,powerOptions:{now:()=>now},vmStateOptions:{syncMs:2000},relayOptions:{heartbeatMs:1000,authorizationMs:20},desktopEventOptions:{tickMs:20,authorizationMs:20,heartbeatMs:1000,...eventOptions},logger:{error:value=>assert.fail(value)}});
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  t.after(async()=>{events?.close();for(const ws of sockets)ws.terminate();await new Promise(resolve=>server.close(resolve));store.close();});
  const base=`http${tls?'s':''}://127.0.0.1:${server.address().port}`;
  const url=(path='/api/v1/events')=>base.replace(/^http/,'ws')+path;
  const open=async(name='alice',options={},path='/api/v1/events')=>{const ws=await connect(url(path),identity.tokens[name],options);sockets.push(ws);return ws;};
  const call=async(path,{as='alice',method='GET',data}={})=>{const response=await fetch(base+'/api/v1'+path,{method,headers:{Authorization:'Bearer '+identity.tokens[as],...(data?{'Content-Type':'application/json'}:{})},body:data?JSON.stringify(data):undefined});return{status:response.status,body:await response.json()};};
  return {store,...identity,events,open,call,url,advance:()=>{now+=5001;},setInventory:value=>{inventory=value;},setTask:value=>{taskResult=value;},get reads(){return reads;}};
}
const changed=async ws=>{const event=await ws.take();assert.deepEqual(Object.keys(event).sort(),['revision','type']);assert.equal(event.type,'desktops.changed');assert.ok(Number.isSafeInteger(event.revision)&&event.revision>0);return event;};

test('Desktop fingerprints ignore observation timestamps while preserving meaningful task, host and power changes',()=>{
  const first=[{id:'pve-710',name:'VM710',powerState:'running',powerCheckedAt:'old',host:{connected:true,lastSeenAt:'old'},powerTask:{id:'task',status:'running',checkedAt:'old',submittedAt:'old'}}];
  const later=structuredClone(first);later[0].powerCheckedAt='new';later[0].host.lastSeenAt='new';later[0].powerTask.checkedAt='new';
  assert.equal(desktopFingerprint(first),desktopFingerprint(later));later[0].powerTask.status='stopped';assert.notEqual(desktopFingerprint(first),desktopFingerprint(later));
  assert.throws(()=>desktopFingerprint([{id:'x',number:NaN}]),/snapshot_bounds/);
  assert.throws(()=>desktopFingerprint([{id:'x',data:'x'.repeat(2*1024*1024)}]),/snapshot_bounds/);
});

test('Actual WS invalidations follow only the authorized VM scope and contain no VM details or credentials',async t=>{
  const f=await fixture(t),alice=await f.open();assert.deepEqual(await alice.take(),{type:'desktops.ready'});
  const initial=await f.call('/desktops');assert.deepEqual(initial.body.desktops.map(d=>d.id),['pve-710']);
  assert.equal(f.reads,1,'HTTP GET and event initialization must share the same cached Proxmox snapshot.');
  f.setInventory([vm(710),vm(711,'stopped')]);f.advance();await delay(100);assert.deepEqual(alice.queue,[]);
  f.setInventory([vm(710,'stopped'),vm(711,'stopped')]);f.advance();const first=await changed(alice);
  assert.equal((await f.call('/desktops')).body.desktops[0].powerState,'stopped');
  f.store.assign('alice','pve-711','fixture');const second=await changed(alice);assert.ok(second.revision>first.revision);
  f.store.revoke('alice','pve-710','fixture');f.store.revoke('alice','pve-711','fixture');await changed(alice);
  assert.deepEqual((await f.call('/desktops')).body.desktops,[]);await delay(80);assert.deepEqual(alice.queue,[]);
});

test('A watcher sees host capability/readiness and disconnect changes, without heartbeat timestamp spam',async t=>{
  const f=await fixture(t),watch=await f.open();await watch.take();
  // The same machine enrolls again (a reinstall). A different UUID here would be
  // another machine under the VM number, which drops the assignment.
  const hostToken=newToken(),code=newToken(),uuid=machineUuid(710);
  f.store.issueEnrollment('pve-710',uuid,tokenHash(code),'fixture');f.store.consumeEnrollment(tokenHash(code),uuid,{hostname:'Isolated VM',version:'0.2.10',tokenHash:tokenHash(hostToken)});
  const host=await connect(f.url('/api/v1/hosts/channel'),hostToken,{headers:{Authorization:'Host '+hostToken,'X-Multitap-Host-Protocol':'capabilities-v1'}});t.after(()=>host.terminate());
  await changed(watch);
  const capabilities={service:true,interactiveSession:true,streaming:true,virtualDisplayReady:true,monitors:[{id:'0',width:1920,height:1080,primary:true}]};
  host.send(JSON.stringify({type:'host.capabilities',capabilities}));await changed(watch);
  assert.equal((await f.call('/desktops')).body.desktops[0].connectionModes.stream,true);
  for(let i=0;i<3;i++){host.send(JSON.stringify({type:'host.heartbeat',capabilities}));await delay(25);}assert.deepEqual(watch.queue,[]);
  host.close();await changed(watch);assert.equal((await f.call('/desktops')).body.desktops[0].host.connected,false);
});

test('CPU and memory changes invalidate only assigned VM watchers using the shared inventory',async t=>{
  const f=await fixture(t);
  const measured=(id,cpu)=>({...vm(id),cpuUsage:cpu,vcpus:4,memoryUsedBytes:2*1024**3,memoryBytes:8*1024**3});
  f.setInventory([measured(710,.02),measured(711,.6)]);
  const watch=await f.open();await watch.take();
  assert.equal((await f.call('/desktops')).body.desktops[0].resources.cpuUsagePercent,2);
  assert.equal(f.reads,1);
  f.setInventory([measured(710,.02),measured(711,.9)]);f.advance();
  await delay(100);assert.deepEqual(watch.queue,[]);
  f.setInventory([measured(710,.05),measured(711,.9)]);f.advance();
  await changed(watch);
  const resources=(await f.call('/desktops')).body.desktops[0].resources;
  assert.equal(resources.cpuUsagePercent,5);assert.equal(resources.memoryUsedBytes,2*1024**3);
  assert.equal(resources.stale,false);
});

test('Power task completion is polled by the broker and pushed without a client task-status polling request',async t=>{
  const f=await fixture(t),watch=await f.open();await watch.take();
  assert.equal((await f.call('/desktops/pve-710/power',{method:'POST',data:{action:'shutdown'}})).status,202);await changed(watch);
  f.setTask({status:'stopped',exitStatus:'OK'});f.setInventory([vm(710,'stopped'),vm(711)]);f.advance();await changed(watch);
  const desktop=(await f.call('/desktops')).body.desktops[0];assert.equal(desktop.powerTask.status,'stopped');assert.equal(desktop.powerBusy,false);assert.equal(desktop.powerState,'stopped');
});

test('Event upgrades reject missing/wrong origins, query credentials, wrong-purpose tokens and duplicate connections',async t=>{
  const f=await fixture(t);
  for(const options of [{headers:{Authorization:'Bearer '+f.tokens.alice}},{headers:{Authorization:'Bearer '+f.tokens.alice,Origin:'https://other.example'}},{headers:{Origin:origin}},{headers:{Authorization:'Host '+f.tokens.alice,Origin:origin}},{headers:{Authorization:'Bearer '+newToken(),Origin:origin}}])await assert.rejects(f.open('alice',options),error=>[401,403].includes(error.status));
  await assert.rejects(f.open('admin'),error=>error.status===401);
  await assert.rejects(f.open('alice',{headers:{Authorization:['Bearer '+f.tokens.alice,'Bearer '+f.tokens.bobby],Origin:origin}}),error=>error.status===403);
  await assert.rejects(f.open('alice',{headers:{Authorization:'Bearer '+f.tokens.alice,Origin:origin,'Sec-WebSocket-Protocol':'not-supported'}}),error=>error.status===403);
  await assert.rejects(f.open('alice',{},'/api/v1/events?token='+f.tokens.alice),error=>error.status===403);
  await assert.rejects(f.open('alice',{},'/api/v1/unknown-events'),error=>error.status===401);
  const watch=await f.open();await watch.take();await assert.rejects(f.open(),error=>error.status===409);watch.close();await watch.closed;await delay(20);
  const replacement=await f.open();assert.deepEqual(await replacement.take(),{type:'desktops.ready'});
});

test('Disabled accounts, revoked sessions, password-change revocation and credential-setup gates close watchers with 1008',async t=>{
  for(const scenario of ['disable','logout','password','expired','setup'])await t.test(scenario,async sub=>{
    const f=await fixture(sub),watch=await f.open();await watch.take();
    if(scenario==='disable')f.store.setUserEnabled('alice',false,'fixture');
    if(scenario==='logout')f.store.logout(tokenHash(f.tokens.alice));
    if(scenario==='password')f.store.changeOwnPassword(f.ids.alice,'isolated-events-password-fixture','replaced-fixture-hash');
    if(scenario==='expired')f.store.db.prepare('UPDATE sessions SET expires_at=? WHERE token_hash=?').run(Date.now()-1,tokenHash(f.tokens.alice));
    if(scenario==='setup')f.store.db.prepare('UPDATE users SET must_change_credentials=1 WHERE id=?').run(f.ids.alice);
    const closed=await watch.closed;assert.equal(closed.code,1008);assert.equal(closed.reason,'authorization_revoked');
  });
});

test('Event output-only protocol rejects application data, oversize frames and excessive control frames',async t=>{
  for(const scenario of ['text','binary','oversize','control-rate'])await t.test(scenario,async sub=>{
    const f=await fixture(sub,{standalone:true}),watch=await f.open();await watch.take();
    if(scenario==='text')watch.send('{"type":"power.stop","vmid":711}');
    if(scenario==='binary')watch.send(Buffer.from([1,2,3]));
    if(scenario==='oversize')watch.send(Buffer.alloc(2048));
    if(scenario==='control-rate')for(let i=0;i<17;i++)watch.pong();
    assert.equal((await watch.closed).code,scenario==='oversize'?1009:1008);await delay(20);assert.equal(f.events.clients.size,0);
  });
});

test('Event connections, heartbeat, snapshot deadlines and outgoing queue guard stay bounded',async t=>{
  await t.test('connection cap and cleanup',async sub=>{
    const f=await fixture(sub,{standalone:true,eventOptions:{maxClients:2}}),a=await f.open(),b=await f.open('bobby');await a.take();await b.take();
    await assert.rejects(f.open('carol'),error=>error.status===503);a.close();await a.closed;await delay(20);const c=await f.open('carol');await c.take();assert.equal(f.events.clients.size,2);
  });
  await t.test('missing pong',async sub=>{
    const f=await fixture(sub,{standalone:true,eventOptions:{heartbeatMs:25}}),watch=await f.open('alice',{autoPong:false});await watch.take();assert.equal((await watch.closed).code,1006);await delay(20);assert.equal(f.events.clients.size,0);
  });
  await t.test('snapshot deadline',async sub=>{
    const f=await fixture(sub,{standalone:true,eventOptions:{refresh:()=>new Promise(()=>{}),refreshTimeoutMs:40}}),watch=await f.open();assert.equal((await watch.closed).code,1013);
  });
  await t.test('queue guard and shared snapshot',async sub=>{
    let reads=0;const f=await fixture(sub,{standalone:true,eventOptions:{snapshot:async()=>{reads++;return[{id:'pve-710',name:String(reads<3?'same':'changed')}];}}}),watch=await f.open();await watch.take();
    const entry=[...f.events.clients.values()][0];Object.defineProperty(entry.ws,'bufferedAmount',{get:()=>20000});
    assert.equal((await watch.closed).code,1013);
  });
});

test('Multiple tokens for one user share a per-tick semantic snapshot and closing cancels pending refresh work',async t=>{
  await t.test('shared user snapshot',async sub=>{
    let reads=0;const f=await fixture(sub,{standalone:true,eventOptions:{snapshot:async()=>{reads++;return[{id:'pve-710',name:'unchanged'}];}}});
    const another=newToken();f.store.createSession(f.store.findUser('alice'),tokenHash(another),Date.now(),3600000,'client');
    const first=await f.open(),second=await f.open('alice',{headers:{Authorization:'Bearer '+another,Origin:origin}});await first.take();await second.take();
    clearInterval(f.events.timer);await delay(25);const before=reads;await f.events.tick();assert.equal(reads-before,1);assert.equal(f.events.clients.size,2);
  });
  await t.test('close while refresh pending',async sub=>{
    let release,reads=0;const pending=new Promise(resolve=>release=resolve);
    const f=await fixture(sub,{standalone:true,eventOptions:{refresh:()=>pending,snapshot:async()=>{reads++;return[];}}}),watch=await f.open();
    await delay(10);f.events.close();assert.equal((await watch.closed).code,1001);release();await delay(20);
    assert.equal(reads,0);assert.equal(f.events.clients.size,0);assert.equal(f.events.ticking,false);assert.equal(f.events.timer._destroyed,true);assert.equal(f.events.authorization._destroyed,true);assert.equal(f.events.heartbeat._destroyed,true);
  });
});

test('Actual verified WSS transports event ready and scoped invalidation through an isolated TLS fixture',async t=>{
  if(spawnSync('openssl',['version']).error?.code==='ENOENT'){t.skip('OpenSSL unavailable; mandatory Linux QA exercises verified WSS.');return;}
  const directory=await mkdtemp(join(tmpdir(),'multitap-events-tls-'));t.after(async()=>{assert.equal(dirname(resolve(directory)),resolve(tmpdir()));assert.ok(basename(directory).startsWith('multitap-events-tls-'));await rm(directory,{recursive:true});});
  const certPath=join(directory,'cert.pem'),keyPath=join(directory,'key.pem');assert.equal(spawnSync('openssl',['req','-x509','-newkey','rsa:2048','-nodes','-keyout',keyPath,'-out',certPath,'-days','1','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost,IP:127.0.0.1'],{encoding:'utf8'}).status,0);
  const cert=await readFile(certPath),key=await readFile(keyPath),f=await fixture(t,{standalone:true,tls:{cert,key}});
  await assert.rejects(f.open(),error=>/CERT|SELF_SIGNED/.test(error.code??''));
  const watch=await f.open('alice',{ca:cert});assert.deepEqual(await watch.take(),{type:'desktops.ready'});f.store.revoke('alice','pve-710','fixture');await changed(watch);
});
