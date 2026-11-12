// Real broker HTTP/WebSocket routing with synthetic media envelopes. This is
// protocol/authorization evidence, not an installed Windows display test.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { Relay } from '../src/relay.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { HttpError } from '../src/errors.mjs';
import { requestedInitialViewport, initialViewportSupported, profileViewportSupported, prepareInitialViewport } from '../src/initial-viewport.mjs';

const origin='https://initial-viewport-fixture.example';
const options={maxWidth:1280,maxHeight:720,fps:60,bitrate:8000000,audio:false,monitor:'0'};
const viewport={monitor:'0',width:2560,height:1440,fps:60};
const body={mode:'stream',options,resizeProtocol:'epoch-v1',initialViewportProtocol:'epoch-v1',initialViewport:viewport};
const capabilities=()=>({service:true,interactiveSession:true,streaming:true,virtualDisplayReady:true,  dynamicResolution:true,viewportProvisioning:true,virtualMonitorControl:true,resizeProtocol:'epoch-v1',initialViewportProtocol:'epoch-v1',
  monitors:[{id:'0',width:1920,height:1080,primary:true,modes:[{width:1920,height:1080,refreshRate:60}]}]});
const combinedCapabilities=()=>({...capabilities(),streamProfileProtocol:'console-profile-v1',streamProfileViewportProtocol:'epoch-v1',
  monitors:capabilities().monitors.map(m=>({...m,primary:false}))});

async function socket(url,authorization,headers={}){
  return new Promise((resolve,reject)=>{
    const ws=new WebSocket(url,{headers:{Authorization:authorization,...headers},perMessageDeflate:false});
    ws.once('error',reject);ws.once('open',()=>{
      const queue=[],waiters=[];ws.messages=[];
      ws.on('message',(bytes,binary)=>{const value=binary?bytes:JSON.parse(bytes.toString('utf8'));ws.messages.push(value);
        const at=waiters.findIndex(w=>w.predicate(value));if(at<0)queue.push(value);else{const w=waiters.splice(at,1)[0];clearTimeout(w.timer);w.resolve(value);}});
      ws.take=(predicate=()=>true)=>{const at=queue.findIndex(predicate);if(at>=0)return Promise.resolve(queue.splice(at,1)[0]);
        return new Promise((resolve,reject)=>{const w={predicate,resolve};w.timer=setTimeout(()=>{waiters.splice(waiters.indexOf(w),1);reject(new Error('fixture_message_timeout'));},2000);waiters.push(w);});};
      ws.json=value=>ws.send(JSON.stringify(value));resolve(ws);
    });
  });
}
async function fixture(t,{api=false,caps=capabilities(),deferredProof=false,accepted=true,identity=null}={}){
  const store=new Store(':memory:');for(const name of ['alice','bobby'])store.addUser({name,passwordHash:'isolated-initial-fixture'});
  store.syncProxmox([170,149].map(vmid=>({vmid,node:'fixture',name:'VM '+vmid,status:'running'})),'fixture');store.assign('alice','pve-170');store.assign('bobby','pve-149');
  const token=newToken(),otherToken=newToken(),hostToken=newToken(),code=newToken();
  store.createSession(store.findUser('alice'),tokenHash(token),Date.now(),3600000);store.createSession(store.findUser('bobby'),tokenHash(otherToken),Date.now(),3600000);
  store.issueEnrollment('pve-170','12345678-1234-1234-1234-123456789abc',tokenHash(code),'fixture');
  store.consumeEnrollment(tokenHash(code),'12345678-1234-1234-1234-123456789abc',{hostname:'Initial fixture',version:'9.0.0',tokenHash:tokenHash(hostToken)});
  const vault=new Vault(randomBytes(32));store.saveSetting('proxmox',vault.seal({endpoint:'https://fixture.invalid',tokenId:'fixture@pve!fixture',secret:'fixture-only'}),'fixture');
  let proofCalls=0,resolveProof;const proof={machineUuid:'12345678-1234-1234-1234-123456789abc',model:'std'};
  const pendingProof=new Promise(resolve=>{resolveProof=()=>resolve(proof);});const proxmox={consoleIdentity:async()=>{++proofCalls;return deferredProof?pendingProof:identity?identity():proof;}};
  let relay,server;if(api)server=await buildServer({store,vault,proxmox,publicOrigin:origin,relayOptions:{authorizationMs:10,heartbeatMs:10000},logger:{error:v=>assert.fail(v)}});
  else{relay=new Relay({store,vault,proxmox,publicOrigin:origin,authorizationMs:10,heartbeatMs:10000});server=createServer();relay.attach(server);}
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));t.after(async()=>{await new Promise(resolve=>server.close(resolve));store.close();});
  const base='http://127.0.0.1:'+server.address().port,local=path=>base.replace('http:','ws:')+new URL(path,origin).pathname;
  const host=await socket(local('/api/v1/hosts/channel'),'Host '+hostToken,{'X-Multitap-Host-Protocol':'capabilities-v1'});
  async function advertise(caps=capabilities()){host.json({type:'host.capabilities',capabilities:caps});return host.take(v=>v.type==='host.capabilities.ack');}
  assert.equal((await advertise(caps)).accepted,accepted);
  function create(data=body,user='alice'){return relay.connect(store.findUser(user),tokenHash(user==='alice'?token:otherToken),'pve-170',data.mode,data.options,data.resizeProtocol,undefined,data);}
  async function call(data=body,as=token){const response=await fetch(base+'/api/v1/desktops/pve-170/connect',{method:'POST',headers:{Authorization:'Bearer '+as,Origin:origin,'Content-Type':'application/json'},body:JSON.stringify(data)});return {status:response.status,body:await response.json()};}
  async function connect(data=body){const session=api?(await call(data)).body:create(data);const client=await socket(local(session.webSocketUrl),'Bearer '+token);const start=await host.take(v=>v.type==='session.start');return {session,client,start};}
  return {store,relay,host,advertise,create,call,connect,otherToken,token,local,resolveProof,get proofCalls(){return proofCalls;}};
}
const info=(c,changes={})=>({type:'stream.info',codec:'h264',...c.session.initialViewport,width:1280,height:720,sourceWidth:2560,sourceHeight:1440,fpsTarget:60,
  audio:{sampleRate:48000,channels:2,format:'s16le',available:false},...changes});
function frame({epoch=1,key=true,kind=1}={}){const b=Buffer.alloc(16);b[0]=kind;b[1]=key?1:0;b.writeUInt16LE(epoch,2);return b;}
const pointer={type:'input',event:'move',x:.5,y:.5};
const limitedCapabilities=()=>({...capabilities(),viewportEncoderLimitsProtocol:'separate-v1'});
const limitedBody=()=>({...body,viewportEncoderLimitsProtocol:'separate-v1'});
async function barrier(f,c,id){c.client.json({type:'ping',id});await f.host.take(v=>v.type==='ping'&&v.id===id);f.host.json({type:'pong',id});await c.client.take(v=>v.type==='pong'&&v.id===id);}

test('Encoder ceilings require exact three-peer negotiation, including HTTP and capability freshness',async t=>{
  const f=await fixture(t,{api:true,caps:limitedCapabilities()});
  for(const delta of [{viewportEncoderLimitsProtocol:'unknown'},{viewportEncoderLimitsProtocol:true},{initialViewportProtocol:undefined,initialViewport:undefined}])
    assert.equal((await f.call({...limitedBody(),...delta})).status,400);
  // RDP is not offered: refused as such before its fields are read.
  const rdp=await f.call({...limitedBody(),mode:'rdp'});assert.equal(rdp.status,409);assert.equal(rdp.body.error.code,'rdp_not_supported');
  await f.advertise();assert.equal((await f.call(limitedBody())).status,409);
  for(const delta of [{viewportEncoderLimitsProtocol:'unknown'},{initialViewportProtocol:undefined},{dynamicResolution:false},{monitors:[...capabilities().monitors,{...capabilities().monitors[0],id:'1',primary:false}]}])
    assert.equal((await f.advertise({...limitedCapabilities(),...delta})).accepted,false);
  await f.advertise(limitedCapabilities());const c=await f.connect(limitedBody());
  assert.equal(c.session.viewportEncoderLimitsProtocol,'separate-v1');assert.equal(c.start.viewportEncoderLimitsProtocol,'separate-v1');
  assert.deepEqual(c.session.options,options);assert.equal(c.start.width,1280);assert.equal(c.start.initialViewport.width,2560);
});

test('Ordinary resize preserves encoding ceilings through small, 4K fullscreen and portrait epochs',async t=>{
  const f=await fixture(t,{caps:limitedCapabilities()}),c=await f.connect(limitedBody()),state=f.relay.sessions.get(c.session.sessionId);
  f.host.json(info(c));await c.client.take(v=>v.type==='stream.info');f.host.send(frame());await c.client.take(Buffer.isBuffer);
  for(const [index,width,height,encodedWidth,encodedHeight] of [[0,960,540,960,540],[1,3840,2160,1280,720],[2,720,1280,404,720],[3,320,2160,106,720]]){
    state.resizeWindow=Date.now()-1001;
    c.client.json({type:'stream.resize',requestId:'limits-'+index,width,height,fps:60,monitor:'0'});
    const pending=await c.client.take(v=>v.type==='stream.resizing'),request=await f.host.take(v=>v.type==='stream.resize');
    assert.equal(request.width,width);assert.equal(request.height,height);
    // Retired metadata/media cannot commit the new physical or codec epoch.
    f.host.json(info(c));f.host.send(frame());await barrier(f,c,'stale-'+index);assert.equal(state.streamReady,false);
    const metadata={...info(c),...pending,sessionId:c.session.sessionId,type:'stream.info',width:encodedWidth,height:encodedHeight,sourceWidth:width,sourceHeight:height};
    f.host.json(metadata);const applied=await c.client.take(v=>v.type==='stream.info');assert.equal(applied.width,encodedWidth);
    assert.deepEqual(state.options,options);assert.equal(state.awaitingKeyFrame,true);
    f.host.send(frame({epoch:pending.epoch,key:false}));await barrier(f,c,'delta-'+index);assert.equal(state.awaitingKeyFrame,true);
    f.host.send(frame({epoch:pending.epoch}));await c.client.take(Buffer.isBuffer);assert.equal(state.awaitingKeyFrame,false);
  }
});

for(const delta of [{width:1920,height:1080},{sourceWidth:1920},{sourceHeight:1080},{fpsTarget:30},{width:1279}]){
  test('Negotiated ordinary resize rejects unbounded or incorrect source metadata '+JSON.stringify(delta),async t=>{
    const f=await fixture(t,{caps:limitedCapabilities()}),c=await f.connect(limitedBody());
    f.host.json(info(c));await c.client.take(v=>v.type==='stream.info');f.host.send(frame());await c.client.take(Buffer.isBuffer);
    c.client.json({type:'stream.resize',requestId:'limits-invalid',width:3840,height:2160,fps:60,monitor:'0'});
    const pending=await c.client.take(v=>v.type==='stream.resizing');await f.host.take(v=>v.type==='stream.resize');
    f.host.json({...info(c),...pending,sessionId:c.session.sessionId,type:'stream.info',width:1280,height:720,sourceWidth:3840,sourceHeight:2160,...delta});
    if(delta.width===1279)await f.host.take(v=>v.type==='session.stop');
    else assert.equal((await c.client.take(v=>v.type==='stream.resize.failed')).reason,'invalid_resize_info');
    assert.equal(f.relay.sessions.size,0);
  });
}

test('No marker retains legacy exact-size resize and a lost negotiated capability prevents a new transaction',async t=>{
  const f=await fixture(t,{caps:limitedCapabilities()}),c=await f.connect();
  assert.equal(c.session.viewportEncoderLimitsProtocol,undefined);assert.equal(c.start.viewportEncoderLimitsProtocol,undefined);
  f.host.json(info(c));await c.client.take(v=>v.type==='stream.info');f.host.send(frame());await c.client.take(Buffer.isBuffer);
  c.client.json({type:'stream.resize',requestId:'legacy-size',width:1920,height:1080,fps:60,monitor:'0'});
  const pending=await c.client.take(v=>v.type==='stream.resizing');await f.host.take(v=>v.type==='stream.resize');
  f.host.json({...info(c),...pending,sessionId:c.session.sessionId,type:'stream.info',width:1920,height:1080,sourceWidth:1920,sourceHeight:1080});
  await c.client.take(v=>v.type==='stream.info');assert.equal(f.relay.sessions.get(c.session.sessionId).options.maxWidth,1920);
  c.client.json({type:'session.stop'});await f.host.take(v=>v.type==='session.stop');
  const next=await f.connect(limitedBody());f.host.json(info(next));await next.client.take(v=>v.type==='stream.info');f.host.send(frame());await next.client.take(Buffer.isBuffer);
  await f.advertise();next.client.json({type:'stream.resize',requestId:'capability-lost',width:3840,height:2160,fps:60,monitor:'0'});
  assert.equal((await next.client.take(v=>v.type==='stream.resize.failed')).reason,'dynamic_resolution_unavailable');
  assert.equal(f.relay.sessions.get(next.session.sessionId).resize,null);
});

test('Explicit request validation is independent from encoder caps and rejects unbound geometry',()=>{
  assert.deepEqual(requestedInitialViewport('stream','epoch-v1',viewport,options,'epoch-v1',null),viewport);
  assert.equal(requestedInitialViewport('stream',undefined,undefined,options,'epoch-v1',null),null);
  for(const bad of [{...viewport,width:2559},{...viewport,height:0},{...viewport,monitor:'1'},{...viewport,fps:30},{...viewport,sessionId:'client-selected'},{...viewport,width:3842}])
    assert.throws(()=>requestedInitialViewport('stream','epoch-v1',bad,options,'epoch-v1',null));
  for(const args of [['rdp','epoch-v1',viewport,options,'epoch-v1',null],['stream','epoch-v2',viewport,options,'epoch-v1',null],['stream','epoch-v1',viewport,options,undefined,null],['stream','epoch-v1',viewport,options,'epoch-v1',[]]])assert.throws(()=>requestedInitialViewport(...args));
  const host={capabilities:capabilities(),capabilitiesAt:100};assert.equal(initialViewportSupported(host,200,100),true);
  assert.equal(initialViewportSupported(host,99,100),false);assert.equal(initialViewportSupported(host,401,100),false);
  delete host.capabilities.initialViewportProtocol;assert.equal(initialViewportSupported(host,200,100),false);
  const ids=['aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa','bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb'];
  const request=prepareInitialViewport('cccccccc-cccc-cccc-cccc-cccccccccccc',viewport,()=>ids.shift());assert.equal(Object.keys(request).length,9);assert.equal(request.epoch,1);
});

test('HTTP forwards explicit fields, preserves caps and enforces assignment without silent legacy downgrade',async t=>{
  const f=await fixture(t,{api:true});assert.equal((await f.call(body,f.otherToken)).status,404);
  for(const input of [{...body,initialViewport:{...viewport,host:'untrusted'}},{...body,initialViewportProtocol:'epoch-v2'}])assert.equal((await f.call(input)).status,400);
  // RDP is not offered: refused as such before its fields are read.
  const rdp=await f.call({...body,mode:'rdp'});assert.equal(rdp.status,409);assert.equal(rdp.body.error.code,'rdp_not_supported');
  const old=capabilities();delete old.initialViewportProtocol;await f.advertise(old);assert.equal((await f.call()).status,409);
  await f.advertise();const c=await f.connect();assert.equal(c.session.transport,'media-epoch-v1');assert.deepEqual(c.session.options,options);
  assert.deepEqual(c.start.initialViewport,c.session.initialViewport);assert.equal(c.start.epoch,0);assert.equal(c.start.width,1280);assert.equal(c.start.initialViewport.width,2560);
  c.client.json({type:'session.stop'});await f.host.take(v=>v.type==='session.stop');
});

test('Pending initial viewport exists before dispatch; only correlated source proof and epoch1 IDR admit media/input',async t=>{
  const f=await fixture(t),c=await f.connect(),state=f.relay.sessions.get(c.session.sessionId);
  assert.equal(state.resize.initial,true);assert.equal(state.nextEpoch,1);assert.equal(state.streamReady,false);
  c.client.json(pointer);f.host.send(frame({epoch:0}));f.host.json(info(c,{epoch:0}));f.host.json(info(c,{requestId:'retired-request'}));await barrier(f,c,'before-proof');
  assert.equal(state.streamReady,false);assert.equal(f.host.messages.some(v=>v.type==='input'),false);assert.equal(c.client.messages.some(Buffer.isBuffer),false);
  f.host.json(info(c));await c.client.take(v=>v.type==='stream.info');assert.equal(state.epoch,1);assert.equal(state.resize,null);assert.deepEqual(state.options,options);
  c.client.json(pointer);f.host.send(frame({key:false}));f.host.send(frame({kind:2,key:false}));await barrier(f,c,'before-idr');
  assert.equal(f.host.messages.some(v=>v.type==='input'),false);assert.equal(c.client.messages.some(Buffer.isBuffer),false);
  f.host.send(frame());assert.deepEqual(await c.client.take(Buffer.isBuffer),frame());c.client.json(pointer);assert.deepEqual(await f.host.take(v=>v.type==='input'),pointer);
});

test('Wrong physical source fails closed even when encoded dimensions match caps; initial timeout never declares readiness',async t=>{
  const f=await fixture(t),c=await f.connect();f.host.json(info(c,{sourceWidth:1920}));
  const failed=await c.client.take(v=>v.type==='stream.resize.failed');assert.equal(failed.unchanged,false);assert.equal(failed.resizeId,c.session.initialViewport.resizeId);
  await f.host.take(v=>v.type==='session.stop');assert.equal(f.relay.sessions.size,0);
  const next=await f.connect(),state=f.relay.sessions.get(next.session.sessionId);state.resize.deadline=Date.now()-1;f.relay.revalidate();
  assert.equal((await next.client.take(v=>v.type==='stream.resize.failed')).reason,'resize_timeout');assert.equal(state.streamReady,false);assert.equal(f.relay.sessions.size,0);
});

test('Capability loss before attach blocks dispatch; omission preserves original legacy start',async t=>{
  const f=await fixture(t),session=f.create(),old=capabilities();delete old.initialViewportProtocol;await f.advertise(old);
  assert.equal(f.relay.sessions.get(session.sessionId).streamReady,false);
  // The WebSocket is authenticated, but no display transaction may be sent to
  // a host that no longer advertises this explicit initial contract.
  const client=await socket(f.local(session.webSocketUrl),'Bearer '+f.token);
  assert.equal((await client.take(v=>v.type==='stream.unavailable')).reason,'virtual_display_required');
  assert.equal(f.host.messages.some(v=>v.type==='session.start'),false);
  assert.equal(f.relay.sessions.size,0);
  const c=await f.connect({mode:'stream',options,resizeProtocol:'epoch-v1'});
  assert.equal(c.session.initialViewport,undefined);assert.equal(c.start.initialViewport,undefined);assert.equal(c.start.epoch,0);assert.equal(c.start.width,1280);
  assert.equal(f.host.messages.filter(v=>v.type==='session.start').length,1);
});

test('Late source proof cannot beat the preparation watchdog and metadata without an IDR cannot retain input ownership',async t=>{
  const f=await fixture(t);clearInterval(f.relay.timer);
  const c=await f.connect(),state=f.relay.sessions.get(c.session.sessionId);state.resize.deadline=Date.now()-1;
  f.host.json(info(c));assert.equal((await c.client.take(v=>v.type==='stream.resize.failed')).reason,'resize_timeout');
  assert.equal(state.streamReady,false);await f.host.take(v=>v.type==='session.stop');
  const next=await f.connect(),pending=f.relay.sessions.get(next.session.sessionId);f.host.json(info(next));await next.client.take(v=>v.type==='stream.info');
  pending.initialFrameDeadline=Date.now()-1;f.relay.revalidate();
  assert.equal((await next.client.take(v=>v.type==='stream.unavailable')).reason,'capture_failed');assert.equal(f.relay.sessions.size,0);
});

test('Combined profile capability is explicit and complete; count control is entry-only while active resize requires owned primary',()=>{
  const host={capabilities:combinedCapabilities(),capabilitiesAt:100};
  assert.equal(initialViewportSupported(host,200,100),true);assert.equal(profileViewportSupported(host,200,100,true),true);
  assert.equal(profileViewportSupported(host,200,100),false);
  for(const key of ['streamProfileProtocol','streamProfileViewportProtocol','initialViewportProtocol','resizeProtocol','dynamicResolution','viewportProvisioning','service','interactiveSession','streaming','virtualDisplayReady']){
    const partial={...host,capabilities:{...host.capabilities}};delete partial.capabilities[key];assert.equal(initialViewportSupported(partial,200,100),false,key);assert.equal(profileViewportSupported(partial,200,100),false,key);
  }
  host.capabilities={...host.capabilities,virtualMonitorControl:false,monitors:host.capabilities.monitors.map(m=>({...m,primary:true}))};
  assert.equal(initialViewportSupported(host,200,100),false);assert.equal(profileViewportSupported(host,200,100),true);
  assert.equal(profileViewportSupported(host,401,100),false);host.capabilities.streamProfileViewportProtocol='epoch-v2';assert.equal(profileViewportSupported(host,200,100),false);
});

test('Fresh Host cannot dispatch a combined session with any missing or mismatched capability',async t=>{
  const cases=[];
  for(const key of ['streamProfileProtocol','streamProfileViewportProtocol','initialViewportProtocol','resizeProtocol','dynamicResolution','viewportProvisioning','service','interactiveSession','streaming','virtualDisplayReady']){
    const caps=combinedCapabilities();delete caps[key];cases.push([key,caps]);
  }
  cases.push(['unknown combined protocol',{...combinedCapabilities(),streamProfileViewportProtocol:'epoch-v2'}]);
  for(const [name,caps] of cases)await t.test(name,async child=>{
    const f=await fixture(child,{api:true,caps,accepted:false});const result=await f.call();
    assert.equal(result.status,409);assert.equal(f.host.messages.some(v=>v.type==='session.start'),false);assert.equal(f.proofCalls,0);
  });
  await t.test('entry count control unavailable',async child=>{
    const caps=combinedCapabilities();delete caps.virtualMonitorControl;
    const f=await fixture(child,{api:true,caps});assert.equal((await f.call()).status,409);
    assert.equal(f.host.messages.some(v=>v.type==='session.start'),false);assert.equal(f.proofCalls,0);
  });
});

test('Combined secondary-owned entry retains private console proof, then permits epoch resize only under fresh primary/profile capability',async t=>{
  const f=await fixture(t,{caps:combinedCapabilities()}),c=await f.connect(),state=f.relay.sessions.get(c.session.sessionId);
  assert.equal(c.session.transport,'media-epoch-v1');assert.equal(state.profileViewport,true);assert.equal(f.proofCalls,1);
  assert.deepEqual(c.start.consoleProfile,{protocol:'console-profile-v1',sessionId:c.session.sessionId,machineUuid:'12345678-1234-1234-1234-123456789abc',model:'std'});
  assert.deepEqual(c.start.initialViewport,c.session.initialViewport);assert.equal(JSON.stringify(c.session).includes('machineUuid'),false);
  const active={...combinedCapabilities(),virtualMonitorControl:false,monitors:[{...capabilities().monitors[0],width:2560,height:1440,primary:true}]};
  assert.equal((await f.advertise(active)).accepted,true);f.host.json(info(c));await c.client.take(v=>v.type==='stream.info');f.host.send(frame());await c.client.take(Buffer.isBuffer);
  c.client.json({type:'stream.resize',requestId:'combined-next',monitor:'0',width:1920,height:1080,fps:60});
  const resizing=await c.client.take(v=>v.type==='stream.resizing'),request=await f.host.take(v=>v.type==='stream.resize');
  assert.equal(resizing.epoch,2);assert.equal(request.epoch,2);assert.equal(request.sessionId,c.session.sessionId);
  f.host.json({...info(c),requestId:request.requestId,resizeId:request.resizeId,epoch:2,width:1920,height:1080,sourceWidth:1920,sourceHeight:1080});
  await c.client.take(v=>v.type==='stream.info'&&v.epoch===2);f.host.send(frame({epoch:2}));await c.client.take(Buffer.isBuffer);
  const legacy={...active};delete legacy.initialViewportProtocol;delete legacy.streamProfileViewportProtocol;await f.advertise(legacy);
  const before=f.host.messages.filter(v=>v.type==='stream.resize').length;c.client.json({type:'stream.resize',requestId:'revoked-combined',monitor:'0',width:1280,height:720,fps:60});
  assert.equal((await c.client.take(v=>v.type==='stream.resize.failed')).reason,'dynamic_resolution_unavailable');
  assert.equal(f.host.messages.filter(v=>v.type==='stream.resize').length,before);
});

// A VirtIO/QXL display or an unreadable
// Proxmox streams the same initial viewport without a console proof, and the
// stream keeps its ordinary epoch resize.
for(const [name,identity] of [['VirtIO display',()=>({machineUuid:'12345678-1234-1234-1234-123456789abc',model:null,issue:'console_vga_other'})],
  ['Proxmox read failure',()=>{throw new HttpError(502,'proxmox_unreachable');}]])
  test(`Combined entry with a ${name} starts the initial viewport without a proof and still resizes`,async t=>{
    const f=await fixture(t,{caps:combinedCapabilities(),identity}),c=await f.connect(),state=f.relay.sessions.get(c.session.sessionId);
    assert.equal(c.session.transport,'media-epoch-v1');assert.equal(state.profileViewport,true);assert.equal(f.proofCalls,1);
    assert.equal(Object.hasOwn(c.start,'consoleProfile'),false);assert.deepEqual(c.start.initialViewport,c.session.initialViewport);
    const active={...combinedCapabilities(),virtualMonitorControl:false,monitors:[{...capabilities().monitors[0],width:2560,height:1440,primary:true}]};
    assert.equal((await f.advertise(active)).accepted,true);f.host.json(info(c));await c.client.take(v=>v.type==='stream.info');f.host.send(frame());await c.client.take(Buffer.isBuffer);
    c.client.json({type:'stream.resize',requestId:'no-proof-next',monitor:'0',width:1920,height:1080,fps:60});
    const resizing=await c.client.take(v=>v.type==='stream.resizing'),request=await f.host.take(v=>v.type==='stream.resize');
    assert.equal(resizing.epoch,2);assert.equal(request.epoch,2);assert.equal(request.sessionId,c.session.sessionId);
  });

test('Combined capability withdrawal during fresh Proxmox proof prevents all Host session dispatch',async t=>{
  const f=await fixture(t,{caps:combinedCapabilities(),deferredProof:true}),session=f.create();
  const client=await socket(f.local(session.webSocketUrl),'Bearer '+f.token);
  for(let i=0;!f.proofCalls&&i<100;i++)await new Promise(resolve=>setTimeout(resolve,5));assert.equal(f.proofCalls,1);
  const legacy={...combinedCapabilities()};delete legacy.initialViewportProtocol;delete legacy.streamProfileViewportProtocol;assert.equal((await f.advertise(legacy)).accepted,true);
  f.resolveProof();assert.equal((await client.take(v=>v.type==='stream.unavailable')).reason,'virtual_display_required');
  assert.equal(f.host.messages.some(v=>v.type==='session.start'),false);
});

test('Profile appearing after standalone negotiation cannot be skipped; old clients retain profile-only legacy semantics',async t=>{
  const f=await fixture(t),session=f.create();await f.advertise(combinedCapabilities());
  const client=await socket(f.local(session.webSocketUrl),'Bearer '+f.token);assert.equal((await client.take(v=>v.type==='stream.unavailable')).reason,'virtual_display_required');
  assert.equal(f.proofCalls,0);assert.equal(f.host.messages.some(v=>v.type==='session.start'),false);
  const c=await f.connect({mode:'stream',options,resizeProtocol:'epoch-v1'});assert.equal(c.session.transport,'media-v1');assert.equal(c.start.initialViewport,undefined);assert.equal(c.start.resizeProtocol,undefined);assert.equal(c.start.consoleProfile.protocol,'console-profile-v1');
  c.client.json({type:'stream.resize',requestId:'old-profile-client',monitor:'0',width:1280,height:720,fps:60});assert.equal((await c.client.take(v=>v.type==='stream.resize.failed')).reason,'dynamic_resolution_unavailable');
  assert.equal(f.host.messages.some(v=>v.type==='stream.resize'),false);
});

test('Removing profile support after combined negotiation cannot silently become standalone initial capture',async t=>{
  const f=await fixture(t,{caps:combinedCapabilities()}),session=f.create();await f.advertise(capabilities());
  const client=await socket(f.local(session.webSocketUrl),'Bearer '+f.token);
  assert.equal((await client.take(v=>v.type==='stream.unavailable')).reason,'virtual_display_required');
  // connect-timing-v1: the identity read starts when the profile connection is
  // granted (one read); it never becomes a proof or a start.
  assert.equal(f.proofCalls,1);assert.equal(f.host.messages.some(v=>v.type==='session.start'),false);
});
