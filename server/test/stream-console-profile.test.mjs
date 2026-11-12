import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { Proxmox } from '../src/proxmox.mjs';
import { HttpError } from '../src/errors.mjs';
import { standardConsoleModel, streamConsoleProfileProtocol } from '../src/stream-console-profile.mjs';

const uuid = '00000000-1234-1234-1234-000000000001';
const capabilities = { service:true,interactiveSession:true,streaming:true,virtualDisplayReady:true,
  monitors:[{id:'0',width:1920,height:1080,primary:false}], streamProfileProtocol:streamConsoleProfileProtocol };
test('Console model inference accepts only proven standard VGA configurations', () => {
  for (const ostype of ['wvista','w2k8','win7','win8','win10','win11']) assert.equal(standardConsoleModel({ostype}), 'std');
  for (const vga of ['std','type=std','std,memory=16','memory=128,type=std,clipboard=vnc']) assert.equal(standardConsoleModel({vga}), 'std');
  for (const config of [null,[],{}, {ostype:'wxp'}, {ostype:'l26'}, {ostype:'win11',arch:'aarch64'},
    {vga:'std',args:'-vga qxl'}, {vga:'std',hostpci0:'0000:01:00,x-vga=1'}, {vga:'std',hostpci0:'0000:01:00,x-vga=TRUE'}, {vga:'std',hostpci0:'0000:01:00,x-vga=On'}, {vga:'std',hostpci0:3},
    ...['','qxl','qxl2','virtio','std,std','type=std,type=std','std,memory=0','std,memory=513','std,foo=1','std,clipboard=spice','std, memory=16','std,memory=16=2'].map(vga=>({vga}))]) assert.equal(standardConsoleModel(config), null, JSON.stringify(config));
});
test('Proxmox proof reads current running VM configuration, never pending config', async () => {
  const adapter = new Proxmox(), calls=[];
  const target={node:'fixture',expectedUuid:uuid};
  adapter.call=async(_config,path)=>{calls.push(path);assert.equal(path.startsWith('/cluster/'),false);return path.endsWith('/status/current')?{status:'running'}:{ostype:'win11',smbios1:'uuid='+uuid};};
  assert.deepEqual(await adapter.consoleIdentity({},149,target),{machineUuid:uuid,model:'std'});
  assert.deepEqual(calls,['/nodes/fixture/qemu/149/config?current=1','/nodes/fixture/qemu/149/status/current']);
  adapter.call=async(_config,path)=>path.endsWith('/status/current')?{status:'stopped'}:{ostype:'win11',smbios1:'uuid='+uuid};
  await assert.rejects(adapter.consoleIdentity({},149,target),{code:'stream_profile_vm_unavailable'});
});
async function socket(url,authorization) {
  const ws=new WebSocket(url,{headers:{Authorization:authorization},perMessageDeflate:false}), queue=[],waiters=[];
  ws.on('message',(data,binary)=>{const value=binary?data:JSON.parse(data.toString());queue.push(value);for(const waiter of [...waiters])waiter();});
  ws.take=(predicate=()=>true)=>new Promise((resolve,reject)=>{
    const timer=setTimeout(()=>{waiters.splice(waiters.indexOf(check),1);reject(Error('message_timeout'));},3000);
    const check=()=>{const index=queue.findIndex(predicate);if(index>=0){clearTimeout(timer);const waiting=waiters.indexOf(check);if(waiting>=0)waiters.splice(waiting,1);resolve(queue.splice(index,1)[0]);}};
    waiters.push(check);check();
  });
  ws.json=value=>ws.send(JSON.stringify(value)); ws.messages=queue;
  await once(ws,'open');return ws;
}
async function fixture(t, caps=capabilities, {consoleIdentity=null,relayOptions={}}={}) {
  const store=new Store(':memory:'),vault=new Vault(randomBytes(32)),token=newToken(),hostToken=newToken();
  const user=store.addUser({name:'profile-user',passwordHash:'isolated-fixture-only',role:'user'});
  store.createSession(store.findUser('profile-user'),tokenHash(token),Date.now(),3600000,'client');
  store.syncProxmox([{vmid:149,node:'fixture',name:'Fixture',status:'running'}],'fixture');store.assign('profile-user','pve-149');
  const config={endpoint:'https://pve.example',tokenId:'fixture@pve!fixture',secret:'isolated-fixture-only'};
  store.saveSetting('proxmox',vault.seal(config),'fixture');
  const code=newToken();store.issueEnrollment('pve-149',uuid,tokenHash(code),'fixture');
  store.consumeEnrollment(tokenHash(code),uuid,{hostname:'Fixture',version:'0.2.31',tokenHash:tokenHash(hostToken)});
  let resolveProof,proofCalls=0;
  const proofPending=new Promise(resolve=>{resolveProof=resolve;});
  const proxmox={consoleIdentity:async(...args)=>{++proofCalls;return consoleIdentity?consoleIdentity(...args):proofPending;},inventory:async()=>[{vmid:149,node:'fixture',name:'Fixture',status:'running'}]};
  const server=await buildServer({store,vault,proxmox,publicOrigin:'https://profile.example',relayOptions:{authorizationMs:20,heartbeatMs:5000,...relayOptions},logger:{error:value=>assert.fail(value)}});
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));const base=`http://127.0.0.1:${server.address().port}`;
  t.after(async()=>{await new Promise(resolve=>server.close(resolve));store.close();});
  const call=async(path,data)=>{const response=await fetch(base+'/api/v1'+path,{method:data===undefined?'GET':'POST',headers:{Authorization:'Bearer '+token,...(data===undefined?{}:{'Content-Type':'application/json'})},body:data===undefined?undefined:JSON.stringify(data)});return{status:response.status,body:await response.json()};};
  const host=await socket(base.replace('http:','ws:')+'/api/v1/hosts/channel','Host '+hostToken);
  host.json({type:'host.capabilities',capabilities:caps});const accepted=(await host.take(v=>v.type==='host.capabilities.ack')).accepted;
  const attach=sessionId=>socket(base.replace('http:','ws:')+'/api/v1/sessions/'+sessionId,'Bearer '+token);
  return{store,vault,server,host,call,attach,accepted,resolveProof,get proofCalls(){return proofCalls;},
    async updateCapabilities(value){host.json({type:'host.capabilities',capabilities:value});assert.equal((await host.take(v=>v.type==='host.capabilities.ack')).accepted,true);},
    async connect(request={mode:'stream'}){const response=await call('/desktops/pve-149/connect',request);assert.equal(response.status,200);const client=await attach(response.body.sessionId);return{client,response};}};
}
async function proofBegan(f){for(let i=0;i<100;i++){if(f.proofCalls)return;await new Promise(resolve=>setTimeout(resolve,5));}assert.fail('Proof not requested');}
const dynamicCapabilities={dynamicResolution:true,resizeProtocol:'epoch-v1',viewportProvisioning:true,
  monitors:[{id:'0',width:1920,height:1080,primary:false,modes:[{width:1920,height:1080,refreshRate:60},{width:1280,height:720,refreshRate:60}]}]};
async function resizeRejectedWithoutMutation(f,client,requestId){
  client.json({type:'stream.resize',requestId,width:1280,height:720,fps:60,monitor:'0'});
  assert.deepEqual(await client.take(v=>v.type==='stream.resize.failed'),{type:'stream.resize.failed',requestId,reason:'dynamic_resolution_unavailable',unchanged:true});
  assert.equal(f.host.messages.some(v=>v.type==='stream.resize'),false);
  assert.equal(client.messages.some(v=>v.type==='stream.resizing'||v.type==='session.ended'),false);
}
test('Authenticated stream start carries private exact-session proof only after fresh VM identity', async t => {
  const f=await fixture(t);assert.equal(f.accepted,true);const {client,response}=await f.connect();await proofBegan(f);
  client.json({type:'input',kind:'move',x:1,y:1});
  assert.equal(f.host.messages.some(v=>v.type==='session.start'),false);
  f.resolveProof({machineUuid:uuid,model:'std'});
  const start=await f.host.take(v=>v.type==='session.start');
  assert.deepEqual(start.consoleProfile,{protocol:streamConsoleProfileProtocol,sessionId:response.body.sessionId,machineUuid:uuid,model:'std'});
  assert.equal(f.host.messages.some(v=>v.type==='input'),false);
  assert.equal(JSON.stringify(response).includes(uuid),false);
  assert.equal(JSON.stringify((await f.call('/desktops')).body).includes(uuid),false);
});
test('Legacy hosts stream without requesting console mutation proof', async t => {
  const {streamProfileProtocol:unused,...legacy}=capabilities;const f=await fixture(t,legacy);await f.connect();
  const start=await f.host.take(v=>v.type==='session.start');assert.equal(start.consoleProfile,undefined);assert.equal(f.proofCalls,0);
});
test('Unknown profile capability is rejected', async t => {
  const f=await fixture(t,{...capabilities,streamProfileProtocol:'console-profile-unknown'});assert.equal(f.accepted,false);
});
test('Profile and initial track lease provisioning cannot be combined or enabled after connect', async t => {
  const {streamProfileProtocol:unused,...legacy}=capabilities;
  const tracks={...legacy,videoTrackProtocol:'tracks-v1',trackViewportProvisioning:true};
  const request={mode:'stream',mediaProtocol:'tracks-v1',options:{maxWidth:1920,maxHeight:1080,fps:60,bitrate:12000000,audio:false,monitor:'0'},tracks:[{id:'0',width:1920,height:1080,x:0,y:0}]};
  const f=await fixture(t,tracks);
  const pending=await f.call('/desktops/pve-149/connect',request);assert.equal(pending.status,200);
  await f.updateCapabilities({...tracks,streamProfileProtocol:streamConsoleProfileProtocol});
  const client=await f.attach(pending.body.sessionId);
  assert.equal((await client.take(v=>v.type==='stream.unavailable')).reason,'track_streaming_unavailable');
  assert.equal((await client.take(v=>v.type==='session.ended')).reason,'track_streaming_unavailable');
  assert.equal(f.host.messages.some(v=>v.type==='session.start'),false);assert.equal(f.proofCalls,0);
  const rejected=await f.call('/desktops/pve-149/connect',request);assert.equal(rejected.status,409);assert.equal(rejected.body.error.code,'track_streaming_unavailable');
});
test('Console profile excludes dynamic resize negotiation and rejects resize without a host mutation',async t=>{
  const f=await fixture(t,{...capabilities,...dynamicCapabilities});assert.equal(f.accepted,true);
  const {client,response}=await f.connect({mode:'stream',resizeProtocol:'epoch-v1'});
  assert.equal(response.body.transport,'media-v1');assert.equal(response.body.resizeProtocol,undefined);assert.equal(response.body.viewportProvisioning,undefined);
  await proofBegan(f);f.resolveProof({machineUuid:uuid,model:'std'});const start=await f.host.take(v=>v.type==='session.start');
  assert.equal(start.consoleProfile.protocol,streamConsoleProfileProtocol);assert.equal(start.resizeProtocol,undefined);assert.equal(start.epoch,undefined);
  await resizeRejectedWithoutMutation(f,client,'profile-initial-resize');
});
test('Profile capability appearing between HTTP connect and WebSocket attach blocks negotiated resize',async t=>{
  const {streamProfileProtocol:unused,...legacy}=capabilities;
  const f=await fixture(t,{...legacy,...dynamicCapabilities});assert.equal(f.accepted,true);
  const pending=await f.call('/desktops/pve-149/connect',{mode:'stream',resizeProtocol:'epoch-v1'});
  assert.equal(pending.status,200);assert.equal(pending.body.transport,'media-epoch-v1');
  await f.updateCapabilities({...capabilities,...dynamicCapabilities});const client=await f.attach(pending.body.sessionId);
  await proofBegan(f);assert.equal(f.host.messages.some(v=>v.type==='session.start'),false);
  f.resolveProof({machineUuid:uuid,model:'std'});const start=await f.host.take(v=>v.type==='session.start');
  assert.equal(start.consoleProfile.sessionId,pending.body.sessionId);
  await resizeRejectedWithoutMutation(f,client,'profile-after-connect');
});
test('Current profile capability blocks resizing an already attached legacy stream',async t=>{
  const {streamProfileProtocol:unused,...legacy}=capabilities;
  const f=await fixture(t,{...legacy,...dynamicCapabilities});assert.equal(f.accepted,true);const {client,response}=await f.connect({mode:'stream',resizeProtocol:'epoch-v1'});
  assert.equal(response.body.transport,'media-epoch-v1');const start=await f.host.take(v=>v.type==='session.start');assert.equal(start.consoleProfile,undefined);assert.equal(start.resizeProtocol,'epoch-v1');
  await f.updateCapabilities({...capabilities,...dynamicCapabilities});
  await resizeRejectedWithoutMutation(f,client,'profile-after-start');assert.equal(f.proofCalls,0);
});
test('Withdrawing the capability cannot enable resizing after profile dispatch',async t=>{
  const f=await fixture(t,{...capabilities,...dynamicCapabilities});assert.equal(f.accepted,true);const {client}=await f.connect({mode:'stream',resizeProtocol:'epoch-v1'});
  await proofBegan(f);f.resolveProof({machineUuid:uuid,model:'std'});await f.host.take(v=>v.type==='session.start');
  const {streamProfileProtocol:unused,...legacy}=capabilities;await f.updateCapabilities({...legacy,...dynamicCapabilities});
  await resizeRejectedWithoutMutation(f,client,'profile-capability-removed');assert.equal(f.proofCalls,1);
});
test('Legacy dynamic resize still negotiates and dispatches without a console profile',async t=>{
  const {streamProfileProtocol:unused,...legacy}=capabilities;const f=await fixture(t,{...legacy,...dynamicCapabilities});assert.equal(f.accepted,true);
  const {client,response}=await f.connect({mode:'stream',resizeProtocol:'epoch-v1'});assert.equal(response.body.transport,'media-epoch-v1');assert.equal(response.body.resizeProtocol,'epoch-v1');
  const start=await f.host.take(v=>v.type==='session.start');assert.equal(start.resizeProtocol,'epoch-v1');assert.equal(start.consoleProfile,undefined);
  client.json({type:'stream.resize',requestId:'legacy-still-dynamic',width:1280,height:720,fps:60,monitor:'0'});
  assert.equal((await client.take(v=>v.type==='stream.resizing')).requestId,'legacy-still-dynamic');
  const resize=await f.host.take(v=>v.type==='stream.resize');assert.equal(resize.requestId,'legacy-still-dynamic');assert.equal(resize.sessionId,response.body.sessionId);assert.equal(f.proofCalls,0);
});
for(const change of ['client-close','authorization-revoked','uuid-mismatch','desktop-moved'])test('No stale profile dispatch after '+change,async t=>{
  const f=await fixture(t),{client}=await f.connect();await proofBegan(f);
  if(change==='client-close'){client.close();await once(client,'close');}
  if(change==='authorization-revoked')f.store.db.prepare('DELETE FROM assignments').run();
  if(change==='desktop-moved')f.store.db.prepare("UPDATE desktops SET pve_node='other' WHERE id='pve-149'").run();
  f.resolveProof({machineUuid:change==='uuid-mismatch'?'00000000-1234-1234-1234-000000000002':uuid,model:'std'});
  // The real reason, never virtual_display_required.
  const reason={'authorization-revoked':'authorization_revoked','uuid-mismatch':'host_machine_mismatch','desktop-moved':'desktop_changed'}[change];
  if(change!=='client-close')assert.equal((await client.take(v=>v.type==='session.ended')).reason,reason);
  if(change==='uuid-mismatch'||change==='desktop-moved')assert.equal(client.messages.concat([]).some(v=>v.type==='stream.unavailable'&&v.reason!==reason),false);
  await new Promise(resolve=>setTimeout(resolve,20));assert.equal(f.host.messages.some(v=>v.type==='session.start'),false);
  if(change==='uuid-mismatch'||change==='desktop-moved')assert.deepEqual(auditRows(f,'stream_console_identity_refused'),['pve-149:'+reason]);
  assert.deepEqual(auditRows(f,'stream_console_unverified'),[]);
});
const auditRows=(f,event)=>f.store.db.prepare('SELECT subject FROM audit_events WHERE event=? ORDER BY id').all(event).map(row=>row.subject);
// Every Proxmox display and every Proxmox
// read failure streams; the start simply carries no consoleProfile.
async function startsWithoutProof(t,{identity,code,request={mode:'stream'},caps=capabilities,relayOptions={}}){
  const f=await fixture(t,caps,{consoleIdentity:identity,relayOptions});assert.equal(f.accepted,true);
  const {client,response}=await f.connect(request);
  const start=await f.host.take(v=>v.type==='session.start');
  assert.equal(start.sessionId,response.body.sessionId);
  assert.equal(Object.hasOwn(start,'consoleProfile'),false);assert.equal(Object.hasOwn(start,'streamProfileTracksProtocol'),false);
  assert.equal(client.messages.some(v=>v.type==='stream.unavailable'||v.type==='session.ended'),false);
  assert.deepEqual(auditRows(f,'stream_console_unverified'),['pve-149:'+code]);
  assert.equal(JSON.stringify(auditRows(f,'stream_console_unverified')).includes(uuid),false);
  return {f,client,response,start};
}
// The real adapter (proxmox.mjs consoleIdentity) over a fake Proxmox API.
function proxmoxApi(config,{fail=null,status='running'}={}){
  const adapter=new Proxmox();
  adapter.call=async(_config,path)=>{if(fail)throw fail;return path.endsWith('/status/current')?{status}:{smbios1:'uuid='+uuid,...config};};
  return (...args)=>adapter.consoleIdentity(...args);
}
const displays={
  console_vga_other:[{ostype:'win11',vga:'qxl'},{ostype:'win11',vga:'virtio'},{ostype:'win11',vga:'vmware'},{ostype:'win11',vga:'cirrus'},{ostype:'win11',vga:'none'},{ostype:'win11',vga:'serial0'},{ostype:'win11',vga:'qxl2,memory=64'}],
  console_gpu_primary:[{ostype:'win11',vga:'std',hostpci0:'0000:01:00,pcie=1,x-vga=1'},{ostype:'win11',vga:'none',hostpci0:'0000:01:00,x-vga=on'}],
  console_qemu_args:[{ostype:'win11',vga:'std',args:'-device virtio-vga'}],
  console_vga_default:[{ostype:'other'},{ostype:'l26'}],
  console_config_other:[{ostype:'win11',vga:'std,memory=1024'},{ostype:'win11',arch:'aarch64'}],
};
for(const [code,configs] of Object.entries(displays))for(const config of configs)test(`A ${JSON.stringify(config)} display streams without a console proof (${code})`,async t=>{
  await startsWithoutProof(t,{identity:proxmoxApi(config),code});
});
for(const [error,code] of [['proxmox_unreachable','proxmox_unreachable'],['proxmox_permission_denied','proxmox_permission_denied'],['proxmox_certificate_error','proxmox_certificate_error'],
  ['proxmox_request_failed','proxmox_request_failed'],['proxmox_invalid_response','proxmox_invalid_response']])test(`A Proxmox read failure (${error}) streams without a console proof`,async t=>{
  await startsWithoutProof(t,{identity:proxmoxApi({ostype:'win11'},{fail:new HttpError(502,error)}),code});
});
test('A VM Proxmox reports as not running, or a broken read, streams without a console proof',async t=>{
  await startsWithoutProof(t,{identity:proxmoxApi({ostype:'win11'},{status:'stopped'}),code:'proxmox_vm_not_running'});
});
test('An unexpected read error streams without a console proof',async t=>{
  await startsWithoutProof(t,{identity:async()=>{throw new Error('socket hang up');},code:'proxmox_read_failed'});
});
test('Proxmox not answering in time streams without a console proof',async t=>{
  await startsWithoutProof(t,{identity:()=>new Promise(()=>{}),code:'proxmox_timeout',relayOptions:{consoleProofReadMs:50}});
});
test('A Proxmox reconfiguration during the read streams without a console proof',async t=>{
  const f=await fixture(t),{client}=await f.connect();await proofBegan(f);
  f.store.saveSetting('proxmox',f.vault.seal({endpoint:'https://changed.example'}),'fixture');
  f.resolveProof({machineUuid:uuid,model:'std'});
  const start=await f.host.take(v=>v.type==='session.start');assert.equal(Object.hasOwn(start,'consoleProfile'),false);
  assert.equal(client.messages.some(v=>v.type==='stream.unavailable'),false);
  assert.deepEqual(auditRows(f,'stream_console_unverified'),['pve-149:proxmox_settings_changed']);
});
test('An unrecognized model from the read streams without a console proof',async t=>{
  await startsWithoutProof(t,{identity:async()=>({machineUuid:uuid,model:'qxl'}),code:'console_config_other'});
});
test('Without Proxmox settings the stream starts without a console proof',async t=>{
  const f=await fixture(t,capabilities,{consoleIdentity:proxmoxApi({ostype:'win11'})});f.store.db.prepare("DELETE FROM settings WHERE key='proxmox'").run();
  await f.connect();const start=await f.host.take(v=>v.type==='session.start');assert.equal(Object.hasOwn(start,'consoleProfile'),false);
  assert.deepEqual(auditRows(f,'stream_console_unverified'),['pve-149:proxmox_not_configured']);
});
for(const [name,config,code] of [['another SMBIOS UUID',{smbios1:'uuid=00000000-1234-1234-1234-000000000099'},'host_machine_mismatch'],['no SMBIOS UUID',{smbios1:''},'host_machine_mismatch']])
  test(`Proxmox reporting ${name} for the registered VM still refuses the stream`,async t=>{
    const f=await fixture(t,capabilities,{consoleIdentity:proxmoxApi({ostype:'win11',vga:'qxl',...config})});const {client}=await f.connect();
    assert.equal((await client.take(v=>v.type==='stream.unavailable')).reason,code);
    assert.equal((await client.take(v=>v.type==='session.ended')).reason,code);
    await new Promise(resolve=>setTimeout(resolve,20));assert.equal(f.host.messages.some(v=>v.type==='session.start'),false);
    assert.deepEqual(auditRows(f,'stream_console_identity_refused'),['pve-149:'+code]);
  });
test('A desktop without a recorded VM UUID reads with the Host\'s UUID instead of a failed prefetch',async t=>{
  const f=await fixture(t,capabilities,{consoleIdentity:proxmoxApi({ostype:'win11',vga:'std'})});
  f.store.db.prepare("UPDATE desktops SET pve_machine_uuid=NULL WHERE id='pve-149'").run();
  const {response}=await f.connect();const start=await f.host.take(v=>v.type==='session.start');
  assert.deepEqual(start.consoleProfile,{protocol:streamConsoleProfileProtocol,sessionId:response.body.sessionId,machineUuid:uuid,model:'std'});
  assert.equal(f.proofCalls,1);
});
test('A standard VGA display still carries the exact proof through the real adapter',async t=>{
  const f=await fixture(t,capabilities,{consoleIdentity:proxmoxApi({ostype:'win11',vga:'std'})});const {response}=await f.connect();
  const start=await f.host.take(v=>v.type==='session.start');
  assert.deepEqual(start.consoleProfile,{protocol:streamConsoleProfileProtocol,sessionId:response.body.sessionId,machineUuid:uuid,model:'std'});
  assert.deepEqual(auditRows(f,'stream_console_unverified'),[]);
});
test('connect-timing-v1: the proof\'s Proxmox read starts at connect, beside the client\'s relay dial, and is used once',async t=>{
  const f=await fixture(t);assert.equal(f.accepted,true);
  const pending=await f.call('/desktops/pve-149/connect',{mode:'stream'});assert.equal(pending.status,200);
  assert.equal(f.proofCalls,1,'read before the relay socket opens');
  f.resolveProof({machineUuid:uuid,model:'std'});
  await new Promise(resolve=>setTimeout(resolve,20));
  assert.equal(f.host.messages.some(v=>v.type==='session.start'),false,'nothing starts before the client attaches');
  const client=await f.attach(pending.body.sessionId);
  const start=await f.host.take(v=>v.type==='session.start');
  assert.deepEqual(start.consoleProfile,{protocol:streamConsoleProfileProtocol,sessionId:pending.body.sessionId,machineUuid:uuid,model:'std'});
  assert.equal(f.proofCalls,1,'the attach reused the read');
  client.close();
});
test('connect-timing-v1: a Proxmox reconfiguration before attach reads the identity again',async t=>{
  const f=await fixture(t);
  const pending=await f.call('/desktops/pve-149/connect',{mode:'stream'});assert.equal(f.proofCalls,1);
  f.store.saveSetting('proxmox',f.vault.seal({endpoint:'https://changed.example',tokenId:'fixture@pve!fixture',secret:'isolated-fixture-only'}),'fixture');
  await f.attach(pending.body.sessionId);await proofBegan(f);
  for(let i=0;i<100&&f.proofCalls<2;i++)await new Promise(resolve=>setTimeout(resolve,5));
  assert.equal(f.proofCalls,2,'the old configuration\'s read is not used');
  f.resolveProof({machineUuid:uuid,model:'std'});
  assert.equal((await f.host.take(v=>v.type==='session.start')).consoleProfile.machineUuid,uuid);
});
