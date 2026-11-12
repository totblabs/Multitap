import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { createServer as createHttpsServer } from 'node:https';
import { once } from 'node:events';
import { randomBytes } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { mkdtemp,readFile,rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join,resolve,dirname,basename } from 'node:path';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { Relay } from '../src/relay.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword,newToken,tokenHash } from '../src/security.mjs';
import { HttpError } from '../src/errors.mjs';
import { requests,options,packet,sound,info,acknowledgement,pointer,keyboard,resize } from './track-fixture.mjs';

const origin='https://track-fixture.example';
const passwordHash=await hashPassword('Only-track-fixture-password!');
const capabilities={service:true,interactiveSession:true,streaming:true,rdpPort:3389,
  virtualDisplayReady:true,videoTrackProtocol:'tracks-v1',trackViewportProvisioning:true,
  monitors:requests.map(t=>({id:t.id,width:t.width,height:t.height,primary:t.id==='0'}))};
const machineUuid='12345678-1234-1234-1234-123456789abc';

async function socket(url,authorization,options={}) {
  return new Promise((resolve,reject)=>{
    const ws=new WebSocket(url,{headers:{Authorization:authorization},perMessageDeflate:false,...options});
    ws.once('error',reject);
    ws.once('unexpected-response',(_req,res)=>{res.resume();reject(Object.assign(new Error('upgrade_rejected'),{status:res.statusCode}));});
    ws.once('open',()=>{
      const queue=[],waiters=[];ws.messages=[];
      ws.on('message',(data,binary)=>{
        const value=binary?data:JSON.parse(data.toString('utf8'));ws.messages.push(value);
        const index=waiters.findIndex(w=>w.predicate(value));
        if(index<0)queue.push(value);else{const waiter=waiters.splice(index,1)[0];clearTimeout(waiter.timer);waiter.resolve(value);}
      });
      ws.take=(predicate=()=>true)=>{
        const index=queue.findIndex(predicate);if(index>=0)return Promise.resolve(queue.splice(index,1)[0]);
        return new Promise((resolve,reject)=>{const waiter={predicate,resolve};
          waiter.timer=setTimeout(()=>{waiters.splice(waiters.indexOf(waiter),1);reject(new Error('message_timeout'));},2000);waiters.push(waiter);});
      };
      ws.json=value=>ws.send(JSON.stringify(value));resolve(ws);
    });
  });
}

async function fixture(t,{tls,api=false,profile=false,relayOptions={}}={}) {
  const store=new Store(':memory:'),vault=new Vault(randomBytes(32));
  if(profile){store.saveSetting('proxmox',vault.seal({endpoint:'https://profile-fixture.invalid',tokenId:'fixture@pve!fixture',secret:'fixture-only'}),'fixture');relayOptions={proxmox:{consoleIdentity:async()=>({machineUuid,model:'std'})},...relayOptions};}
  store.addUser({name:'alice',passwordHash});store.addUser({name:'bobby',passwordHash});
  store.syncProxmox([170,149].map(vmid=>({vmid,node:'pve',name:`Track fixture ${vmid}`,status:'running'})),'fixture');
  store.assign('alice','pve-170');store.assign('bobby','pve-149');
  const aliceToken=newToken(),bobbyToken=newToken(),hostToken=newToken(),otherHostToken=newToken();
  store.createSession(store.findUser('alice'),tokenHash(aliceToken),Date.now(),60000);
  store.createSession(store.findUser('bobby'),tokenHash(bobbyToken),Date.now(),60000);
  for(const [vm,token] of [[170,hostToken],[149,otherHostToken]]){
    const code=newToken();store.issueEnrollment('pve-'+vm,machineUuid,tokenHash(code),'fixture');
    store.consumeEnrollment(tokenHash(code),machineUuid,{hostname:'Track fixture',version:'9.0.0',tokenHash:tokenHash(token)});
  }
  let relay,server;
  if(api)server=await buildServer({store,vault,publicOrigin:origin,relayOptions:{authorizationMs:10,heartbeatMs:10000,...relayOptions},logger:{error:v=>assert.fail(v)}});
  else {relay=new Relay({store,vault,publicOrigin:origin,authorizationMs:10,heartbeatMs:10000,...relayOptions});
    server=tls?createHttpsServer(tls):createServer();relay.attach(server);}
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  t.after(async()=>{await new Promise(resolve=>server.close(resolve));store.close();});
  const base=`${tls?'https':'http'}://127.0.0.1:${server.address().port}`;
  if(tls)relay.publicOrigin=base;
  const socketOptions=tls?{ca:tls.cert,rejectUnauthorized:true}:{};
  const localUrl=path=>base.replace(/^http/,'ws')+new URL(path,origin).pathname;
  const host=await socket(localUrl('/api/v1/hosts/channel'),'Host '+hostToken,socketOptions);
  async function advertise(value=capabilities){host.json({type:'host.capabilities',capabilities:value});return host.take(v=>v.type==='host.capabilities.ack');}
  assert.equal((await advertise()).accepted,true);
  async function call(data,token=aliceToken){const response=await fetch(base+'/api/v1/desktops/pve-170/connect',{
    method:'POST',headers:{Authorization:'Bearer '+token,Origin:origin,'Content-Type':'application/json'},body:JSON.stringify(data)});
    return {status:response.status,body:await response.json()};}
  const body={mode:'stream',options,mediaProtocol:'tracks-v1',tracks:requests};
  function create(data=body){return relay.connect(store.findUser('alice'),tokenHash(aliceToken),'pve-170',data.mode,data.options,data.resizeProtocol,undefined,{mediaProtocol:data.mediaProtocol,tracks:data.tracks,streamProfileTracksProtocol:data.streamProfileTracksProtocol});}
  async function connect(data=body){const response=api?await call(data):{status:200,body:create(data)};assert.equal(response.status,200);const session=response.body;
    const client=await socket(localUrl(session.webSocketUrl),'Bearer '+aliceToken,socketOptions);
    const start=await host.take(v=>v.type==='session.start');return {client,session,start};}
  async function tunnel(start,token=hostToken,extra={}){return socket(localUrl(start.tunnelUrl),'Host '+token,{...socketOptions,...extra});}
  return {store,relay,host,server,base,socketOptions,aliceToken,bobbyToken,hostToken,otherHostToken,localUrl,advertise,call,create,connect,tunnel};
}
async function prepare(f,c,metadata=info(c.session.sessionId)){const source=await f.tunnel(c.start);
  source.json(metadata);assert.deepEqual(await c.client.take(v=>v.type==='stream.tracks.info'),metadata);
  return {source,metadata};}
async function ready(f,c){const result=await prepare(f,c);for(const track of ['1','0']){const bytes=packet({track});result.source.send(bytes);assert.deepEqual(await c.client.take(Buffer.isBuffer),bytes);}
  const ack=acknowledgement(result.metadata);c.client.json(ack);assert.deepEqual(await result.source.take(v=>v.type==='stream.tracks.ready'),{...ack,sessionId:c.session.sessionId});return result;}
async function hostStillOnline(f){f.host.json({type:'host.heartbeat'});await f.host.take(v=>v.type==='host.heartbeat.ack');assert.equal(f.host.readyState,WebSocket.OPEN);}
async function barrier(source,client,id){source.json({type:'pong',id});await client.take(v=>v.type==='pong'&&v.id===id);}

async function certificate(t){
  if(spawnSync('openssl',['version']).error?.code==='ENOENT')return undefined;
  const directory=await mkdtemp(join(tmpdir(),'multitap-track-tls-'));
  t.after(async()=>{assert.equal(dirname(resolve(directory)),resolve(tmpdir()));assert.ok(basename(directory).startsWith('multitap-track-tls-'));await rm(directory,{recursive:true});});
  const generated=spawnSync('openssl',['req','-x509','-newkey','rsa:2048','-nodes','-keyout',join(directory,'key.pem'),'-out',join(directory,'cert.pem'),'-days','1','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost,IP:127.0.0.1'],{encoding:'utf8'});assert.equal(generated.status,0);
  return {cert:await readFile(join(directory,'cert.pem')),key:await readFile(join(directory,'key.pem'))};
}

test('Track REST negotiation preserves legacy clients and requires explicit capabilities/authorized VM, never client destinations',async t=>{
  const f=await fixture(t,{api:true});
  for(const data of [{mediaProtocol:'tracks-v2',tracks:requests},{mediaProtocol:'tracks-v1',tracks:requests,options:{...options,monitor:'0'}},
    {...{mediaProtocol:'tracks-v1',tracks:requests,options},resizeProtocol:'epoch-v1'},
    {mediaProtocol:'tracks-v1',tracks:requests,options,host:'127.0.0.1'}])assert.equal((await f.call(data)).status,400);
  // RDP is not offered: refused as such before its fields are read.
  const rdp=await f.call({mode:'rdp',mediaProtocol:'tracks-v1',tracks:requests});assert.equal(rdp.status,409);assert.equal(rdp.body.error.code,'rdp_not_supported');
  assert.equal((await f.call({mediaProtocol:'tracks-v1',tracks:requests,options},f.bobbyToken)).status,404);
  await f.advertise({...capabilities,videoTrackProtocol:undefined,trackViewportProvisioning:undefined});
  assert.equal((await f.call({mediaProtocol:'tracks-v1',tracks:requests,options})).status,409);
  const legacy=await f.connect({mode:'stream'});assert.equal(legacy.session.transport,'media-v1');assert.equal(legacy.start.mediaProtocol,undefined);
  legacy.client.json({type:'session.stop'});await f.host.take(v=>v.type==='session.stop');
  await f.advertise();const current=await f.connect();assert.equal(current.session.transport,'media-tracks-v1');
  assert.equal(current.session.mediaProtocol,'tracks-v1');assert.deepEqual(current.session.tracks,requests);
});

test('Session-bound track socket checks host identity, user token, origin and single controller; configured origin supplies URLs',async t=>{
  const f=await fixture(t),c=await f.connect();assert.equal(c.start.mediaProtocol,'tracks-v1');assert.deepEqual(c.start.tracks,requests.map(t=>({...t,epoch:1})));
  assert.equal(new URL(c.start.tunnelUrl).origin,origin.replace('https:','wss:'));assert.equal(new URL(c.start.tunnelUrl).search,'');
  assert.throws(()=>f.create(),e=>e.code==='desktop_busy');
  await assert.rejects(f.tunnel(c.start,f.otherHostToken),e=>e.status===401);
  await assert.rejects(socket(f.localUrl(c.start.tunnelUrl),'Bearer '+f.aliceToken),e=>e.status===401);
  await assert.rejects(socket(f.localUrl(c.session.webSocketUrl),'Bearer '+f.bobbyToken),e=>e.status===401);
  await assert.rejects(f.tunnel(c.start,f.hostToken,{headers:{Authorization:'Host '+f.hostToken,Origin:'https://foreign.example'}}),e=>e.status===403);
  await assert.rejects(socket(f.localUrl(c.start.tunnelUrl)+'?token=not-allowed','Host '+f.hostToken),e=>e.status===403);
  await f.tunnel(c.start);await assert.rejects(f.tunnel(c.start),e=>e.status===409);await hostStillOnline(f);
});

test('Actual WS routes separate tracks, gates input/audio on both keys plus acknowledgement and rejects control-channel frame fallback',async t=>{
  const f=await fixture(t),c=await f.connect(),{source,metadata}=await prepare(f,c);
  c.client.json(pointer());c.client.json({type:'ping',id:'before-ready'});await source.take(v=>v.id==='before-ready');
  assert.equal(source.messages.some(v=>v.type==='input'),false);
  source.send(sound());source.send(packet({track:'1'}));assert.deepEqual(await c.client.take(Buffer.isBuffer),packet({track:'1'}));
  f.host.send(packet({track:'0'}));await barrier(source,c.client,'shared-channel-ignored');
  assert.equal(c.client.messages.filter(Buffer.isBuffer).length,1);
  source.send(packet({track:'0'}));await c.client.take(Buffer.isBuffer);source.send(sound());await barrier(source,c.client,'before-ack');
  assert.equal(c.client.messages.filter(Buffer.isBuffer).length,2);
  c.client.json(acknowledgement(metadata));await source.take(v=>v.type==='stream.tracks.ready');
  c.client.json(pointer());assert.deepEqual(await source.take(v=>v.type==='input'),{...pointer(),sessionId:c.session.sessionId});
  c.client.json(keyboard());assert.deepEqual(await source.take(v=>v.event==='key'),{...keyboard(),sessionId:c.session.sessionId});
  source.send(sound());assert.deepEqual(await c.client.take(Buffer.isBuffer),sound());await hostStillOnline(f);
});

test('Actual WS viewport transaction isolates stale media/input/ACK, preserves track0 epoch and requires two new keys',async t=>{
  const f=await fixture(t),c=await f.connect(),{source,metadata}=await ready(f,c);
  c.client.json(resize());const command=await source.take(v=>v.type==='stream.tracks.resize');
  assert.deepEqual(command.tracks.map(t=>[t.id,t.epoch]),[['0',1],['1',2]]);
  assert.equal((await c.client.take(v=>v.type==='stream.tracks.resizing')).resizeId,command.resizeId);
  c.client.json(pointer());c.client.json(acknowledgement(metadata));c.client.json({type:'ping',id:'in-transition'});await source.take(v=>v.id==='in-transition');
  assert.equal(source.messages.filter(v=>v.type==='input').length,0);
  source.json(info(c.session.sessionId,command));await c.client.take(v=>v.type==='stream.tracks.info'&&v.topologyEpoch===2);
  const before=c.client.messages.filter(Buffer.isBuffer).length;source.send(packet());source.send(packet({track:'1',topology:2,epoch:1}));
  await barrier(source,c.client,'stale-frames-dropped');assert.equal(c.client.messages.filter(Buffer.isBuffer).length,before);
  for(const track of ['0','1']){const bytes=packet({track,topology:2,epoch:track==='0'?1:2});source.send(bytes);assert.deepEqual(await c.client.take(Buffer.isBuffer),bytes);}
  c.client.json(acknowledgement(info(c.session.sessionId,command)));await source.take(v=>v.type==='stream.tracks.ready'&&v.topologyEpoch===2);
  c.client.json(pointer());c.client.json(pointer({epoch:2,topologyEpoch:2}));
  assert.deepEqual(await source.take(v=>v.type==='input'),{...pointer({epoch:2,topologyEpoch:2}),sessionId:c.session.sessionId});
  source.json(metadata);await barrier(source,c.client,'late-metadata-ignored');await hostStillOnline(f);
  // Once the previous request's throttle window is over, a delayed old client
  // request must still never apply its obsolete viewport to generation2.
  f.relay.sessions.get(c.session.sessionId).lastTrackResizeAt=0;
  c.client.json(resize());const rejected=await c.client.take(v=>v.type==='stream.tracks.resize.failed');
  assert.equal(rejected.reason,'stale_topology');assert.equal(rejected.unchanged,true);
  c.client.json({type:'ping',id:'stale-resize-rejected'});await source.take(v=>v.id==='stale-resize-rejected');
  assert.equal(source.messages.filter(v=>v.type==='stream.tracks.resize').length,1);
});

test('Malformed applied geometry, TVT1 and ready/input/telemetry terminate only their session; registered host can reconnect',async t=>{
  const f=await fixture(t);
  const cases=[
    async(c,s)=>s.json({...info(c.session.sessionId),tracks:[{id:{toString:0}},null]}),
    async(c,s)=>{const bytes=packet();bytes[0]|=128;s.send(bytes);},
    async(c,s)=>{s.json(info(c.session.sessionId));await c.client.take(v=>v.type==='stream.tracks.info');c.client.json(acknowledgement());},
    async(c,s)=>{s.json(info(c.session.sessionId));await c.client.take(v=>v.type==='stream.tracks.info');s.send(packet({payload:Buffer.from([0,0,1,0x41,1])}));},
  ];
  for(const act of cases){const c=await f.connect(),source=await f.tunnel(c.start);await act(c,source);
    await c.client.take(v=>v.type==='session.ended');await f.host.take(v=>v.type==='session.stop');await hostStillOnline(f);}
  for(const act of [c=>c.client.json(pointer({track:'console'})),c=>c.client.json(keyboard({vk:0})),
    (c,s)=>s.json({type:'stream.tracks.stats',sessionId:c.session.sessionId,topologyEpoch:1,measuredMs:1,tracks:[null,{id:{toString:0}}]})]){
    const c=await f.connect(),{source}=await ready(f,c);await act(c,source);await c.client.take(v=>v.type==='session.ended');
    await f.host.take(v=>v.type==='session.stop');await hostStillOnline(f);
  }
});

test('Retired source socket and shared control frames cannot cross a successor session with topology1',async t=>{
  const f=await fixture(t),old=await f.connect(),{source}=await ready(f,old);
  old.client.json({type:'session.stop'});await f.host.take(v=>v.type==='session.stop');
  const next=await f.connect();await assert.rejects(f.tunnel(old.start),e=>e.status===401);
  // A closing socket may still accept a queued local send; it must never find
  // its successor by reused output epochs. The new endpoint has another UUID.
  if(source.readyState===WebSocket.OPEN)source.send(packet());
  const current=await prepare(f,next);f.host.send(packet());await barrier(current.source,next.client,'new-session');
  assert.equal(next.client.messages.filter(Buffer.isBuffer).length,0);assert.notEqual(old.start.tunnelUrl,next.start.tunnelUrl);await hostStillOnline(f);
});

test('Assignment revocation closes both tracks, resets remote pressed input and refuses reconnect',async t=>{
  const f=await fixture(t),c=await f.connect(),{source}=await ready(f,c);
  c.client.json(keyboard());await source.take(v=>v.event==='key');
  const clientClosed=once(c.client,'close'),sourceClosed=once(source,'close');f.store.revoke('alice','pve-170');
  assert.equal((await c.client.take(v=>v.type==='session.ended')).reason,'authorization_revoked');
  await source.take(v=>v.type==='input.reset');await Promise.all([clientClosed,sourceClosed]);
  assert.throws(()=>f.create(),e=>e.status===404);await assert.rejects(f.tunnel(c.start),e=>e.status===401);await hostStillOnline(f);
});

test('Preparation and actual client readiness timeouts end only the pending session',async t=>{
  for(const prepared of [false,true]){
    const f=await fixture(t,{relayOptions:{viewportReadyMs:80,resizeReadyMs:80}}),c=await f.connect();if(prepared)await prepare(f,c);
    const failure=await c.client.take(v=>v.type==='stream.unavailable');assert.equal(failure.reason,prepared?'tracks_ready_timeout':'tracks_prepare_timeout');await hostStillOnline(f);
  }
});

test('Lost track source and injected queue backpressure cannot leave an authorized frozen controller',async t=>{
  const f=await fixture(t);let c=await f.connect(),result=await ready(f,c);
  result.source.close();assert.equal((await c.client.take(v=>v.type==='session.ended')).reason,'tracks_host_disconnected');await hostStillOnline(f);
  c=await f.connect();result=await ready(f,c);const serverClient=f.relay.sessions.get(c.session.sessionId).ws;
  // Deterministic queue pressure injection; this is not a measured slow network.
  Object.defineProperty(serverClient,'bufferedAmount',{configurable:true,get:()=>9*1024*1024});
  const closed=once(c.client,'close');result.source.send(packet({pts:1}));await closed;
  assert.equal(f.relay.sessions.has(c.session.sessionId),false);await hostStillOnline(f);
});

test('Capability loss after readiness ends both outputs; capabilities stay explicit and invalid candidates do not replace valid metadata',async t=>{
  const f=await fixture(t),c=await f.connect();await ready(f,c);
  assert.equal((await f.advertise({...capabilities,monitors:[{id:'console',width:640,height:360,primary:true}]})).accepted,false);
  assert.equal(f.relay.sessions.has(c.session.sessionId),true);
  await f.advertise({...capabilities,videoTrackProtocol:undefined,trackViewportProvisioning:false});
  assert.equal((await c.client.take(v=>v.type==='stream.unavailable')).reason,'track_streaming_unavailable');await hostStillOnline(f);
});

test('A stalled second track times out despite live socket pong, first-track packets and shared PCM',async t=>{
  const f=await fixture(t,{relayOptions:{trackMediaMs:100}}),c=await f.connect(),{source}=await ready(f,c);
  let pts=0;const timer=setInterval(()=>{if(source.readyState===WebSocket.OPEN){++pts;source.send(packet({track:'0',pts}));source.send(sound(1,pts));}},10);
  t.after(()=>clearInterval(timer));
  assert.equal((await c.client.take(v=>v.type==='stream.unavailable')).reason,'tracks_source_stalled');await hostStillOnline(f);
});

test('Verified local WSS carries both track generations and denies an untrusted endpoint certificate',async t=>{
  const tls=await certificate(t);if(!tls){t.skip('OpenSSL unavailable; actual verified WSS is required in Linux QA.');return;}
  const f=await fixture(t,{tls});await assert.rejects(socket(f.localUrl('/api/v1/hosts/channel'),'Host '+f.hostToken,{rejectUnauthorized:true}));
  const c=await f.connect(),{source}=await ready(f,c);assert.equal(new URL(c.start.tunnelUrl).origin,f.base.replace('https:','wss:'));
  c.client.json(resize());const command=await source.take(v=>v.type==='stream.tracks.resize');source.json(info(c.session.sessionId,command));await c.client.take(v=>v.type==='stream.tracks.info'&&v.topologyEpoch===2);
  for(const track of ['1','0']){const data=packet({track,topology:2,epoch:track==='1'?2:1});source.send(data);assert.deepEqual(await c.client.take(Buffer.isBuffer),data);}
  c.client.json(acknowledgement(info(c.session.sessionId,command)));await source.take(v=>v.type==='stream.tracks.ready'&&v.topologyEpoch===2);
  c.client.json(pointer({topologyEpoch:2,epoch:2}));await source.take(v=>v.type==='input');source.send(sound(2));assert.deepEqual(await c.client.take(Buffer.isBuffer),sound(2));
});

test('Actual C++ TVT1 packets from two Microsoft CPU encoders traverse authenticated WS/WSS unchanged across resize',async t=>{
  const directory=process.env.MULTITAP_TRACK_VECTOR_DIRECTORY;
  if(!directory){t.skip('Opt-in actual Windows MFT vectors; protocol markers do not substitute for these bytes.');return;}
  const manifest=JSON.parse(await readFile(join(directory,'manifest.json'),'utf8'));
  assert.equal(manifest.format,'multitap-native-track-fixture-v1');assert.equal(manifest.actualMicrosoftCpuEncoder,true);
  assert.equal(manifest.syntheticPixels,true);assert.equal(manifest.windowsDisplaysChanged,false);assert.equal(manifest.frames.length,32);
  const tls=await certificate(t),f=await fixture(t,{tls}),c=await f.connect();
  const metadataFor=(topology,command={})=>{
    if(manifest.sourcePipeline!==true)return info(c.session.sessionId,topology===1?undefined:command);
    assert.equal(manifest.sourceMetadata.length,2);
    const value=structuredClone(manifest.sourceMetadata[topology-1]);
    // The source's production metadata writer is exercised unchanged. Only
    // the isolated broker's newly allocated authorization/operation IDs bind
    // the exported synthetic capture session to this separate WSS fixture.
    value.sessionId=c.session.sessionId;
    if(topology===2){value.resizeId=command.resizeId;value.requestId=command.requestId;}
    return value;
  };
  const {source}=await prepare(f,c,metadataFor(1));
  const seen=new Set();let command={topologyEpoch:1};
  for(const topology of [1,2]){
    if(topology===2){c.client.json(resize());command=await source.take(v=>v.type==='stream.tracks.resize');
      source.json(metadataFor(2,command));await c.client.take(v=>v.type==='stream.tracks.info'&&v.topologyEpoch===2);}
    const frames=manifest.frames.filter(v=>v.topologyEpoch===topology);assert.equal(frames.length,16);
    for(const frame of frames){
      assert.match(frame.file,/^topology-[12]-track-[01]-frame-[0-7]\.tvt1$/);assert.equal(seen.has(frame.file),false);seen.add(frame.file);
      const bytes=await readFile(join(directory,frame.file));
      // Import is deliberately independent of the JavaScript test packet
      // writer: byte equality proves native writer/relay interoperability.
      assert.equal(bytes.subarray(0,4).toString('hex'),'54565431');assert.equal(bytes[6],Number(frame.track));
      assert.equal(bytes.readUInt32LE(8),topology);assert.equal(bytes.readUInt32LE(12),frame.epoch);
      assert.equal(bytes.readBigUInt64LE(16),BigInt(frame.ptsUs));assert.equal(Boolean(bytes[5]&1),frame.key);
      source.send(bytes);assert.deepEqual(await c.client.take(Buffer.isBuffer),bytes);
    }
    const metadata=metadataFor(topology,command);
    c.client.json(acknowledgement(metadata));await source.take(v=>v.type==='stream.tracks.ready'&&v.topologyEpoch===topology);
  }
  assert.equal(seen.size,32);await hostStillOnline(f);
  t.diagnostic(JSON.stringify({nativePackets:32,productionSourcePipeline:manifest.sourcePipeline===true,sourceMetadataBindingsReboundInQa:manifest.sourcePipeline===true,transport:tls?'verified-local-wss':'local-ws',actualWindowsCapture:false,actualLocalPresentation:false,publicRelay:false}));
});

function groupProfileCapabilities(){return{...capabilities,streamProfileProtocol:'console-profile-v1',streamProfileTracksProtocol:'group-v1',dynamicResolution:true,viewportProvisioning:true,resizeProtocol:'epoch-v1',monitors:capabilities.monitors.map(m=>({...m,primary:false,modes:[{width:m.width,height:m.height,refreshRate:60}]}))};}
const groupProfileRequest=()=>({mode:'stream',options,mediaProtocol:'tracks-v1',tracks:requests,streamProfileTracksProtocol:'group-v1'});
function restoredProfileViewport(sessionId,command){return{type:'stream.tracks.resize.restored',sessionId,requestId:command.requestId,resizeId:command.resizeId,topologyEpoch:command.topologyEpoch,fps:options.fps,tracks:requests.map(t=>({...t,epoch:1})),reason:'unsupported_display_mode'};}

test('Explicit group profile REST negotiation keeps the new generation proof private and rejects legacy/partial tuples',async t=>{
  const f=await fixture(t,{api:true,profile:true});const caps=groupProfileCapabilities();assert.equal((await f.advertise(caps)).accepted,true);
  assert.equal((await f.call({mode:'stream',options,mediaProtocol:'tracks-v1',tracks:requests})).status,409);
  assert.equal((await f.call({...groupProfileRequest(),streamProfileTracksProtocol:'group-v2'})).status,400);
  const rdp=await f.call({...groupProfileRequest(),mode:'rdp'});assert.equal(rdp.status,409);assert.equal(rdp.body.error.code,'rdp_not_supported'); // RDP is not offered.
  assert.equal((await f.call(groupProfileRequest(),f.bobbyToken)).status,404);
  for(const key of['videoTrackProtocol','trackViewportProvisioning','viewportProvisioning','streamProfileProtocol','resizeProtocol']){
    const partial={...caps};delete partial[key];assert.equal((await f.advertise(partial)).accepted,false);
  }
  const c=await f.connect(groupProfileRequest());assert.equal(c.session.streamProfileTracksProtocol,'group-v1');assert.equal(c.session.consoleProfile,undefined);
  assert.equal(c.start.streamProfileTracksProtocol,'group-v1');assert.deepEqual(c.start.consoleProfile,{protocol:'console-profile-v1',sessionId:c.session.sessionId,machineUuid,model:'std'});
  assert.equal(c.start.initialViewport,undefined);assert.equal(c.start.resizeProtocol,undefined);assert.equal(c.start.profileOperation,undefined);assert.equal(c.start.topologyEpoch,1);
  c.client.json({type:'session.stop'});await f.host.take(v=>v.type==='session.stop');await hostStillOnline(f);
});

// All monitors on a VM whose Proxmox
// display is not standard VGA (or while Proxmox cannot be read) start as the
// Host's plain v2 tracks start: no consoleProfile, no group marker.
for(const [name,identity] of [['QXL display',async()=>({machineUuid,model:null,issue:'console_vga_other'})],['Proxmox token error',async()=>{throw new HttpError(502,'proxmox_permission_denied');}]])
  test(`All monitors with a ${name} start without a console proof and keep their resize`,async t=>{
    const f=await fixture(t,{profile:true,relayOptions:{proxmox:{consoleIdentity:identity}}});assert.equal((await f.advertise(groupProfileCapabilities())).accepted,true);
    const c=await f.connect(groupProfileRequest());assert.equal(c.session.streamProfileTracksProtocol,'group-v1');
    assert.equal(Object.hasOwn(c.start,'consoleProfile'),false);assert.equal(Object.hasOwn(c.start,'streamProfileTracksProtocol'),false);
    assert.equal(c.start.mediaProtocol,'tracks-v1');assert.equal(c.start.topologyEpoch,1);
    const{source}=await ready(f,c);
    c.client.json(resize());const command=await source.take(v=>v.type==='stream.tracks.resize');assert.equal(command.topologyEpoch,2);
    c.client.json({type:'session.stop'});await f.host.take(v=>v.type==='session.stop');await hostStillOnline(f);
  });

test('Authenticated group restore carries only the previous viewport in a new topology and preserves the session after fresh keys/ready',async t=>{
  const f=await fixture(t,{profile:true});assert.equal((await f.advertise(groupProfileCapabilities())).accepted,true);const c=await f.connect(groupProfileRequest());const{source}=await ready(f,c);
  c.client.json(resize());const command=await source.take(v=>v.type==='stream.tracks.resize');const restored=restoredProfileViewport(c.session.sessionId,command);
  source.json(restored);assert.deepEqual(await c.client.take(v=>v.type==='stream.tracks.resize.restored'),restored);
  source.json(restored);await barrier(source,c.client,'duplicate-restore');assert.equal(c.client.messages.filter(v=>v.type==='stream.tracks.resize.restored').length,1);
  c.client.json(pointer());await barrier(source,c.client,'pending-restore-input');assert.equal(source.messages.filter(v=>v.type==='input').length,0);
  const metadata=info(c.session.sessionId,restored);source.json(metadata);assert.deepEqual(await c.client.take(v=>v.type==='stream.tracks.info'&&v.topologyEpoch===2),metadata);
  for(const track of['0','1']){const bytes=packet({track,topology:2,epoch:1,pts:0});source.send(bytes);assert.deepEqual(await c.client.take(Buffer.isBuffer),bytes);}
  const ack=acknowledgement(metadata);c.client.json(ack);await source.take(v=>v.type==='stream.tracks.ready'&&v.topologyEpoch===2);
  c.client.json(pointer());c.client.json(pointer({topologyEpoch:2,epoch:1}));assert.deepEqual(await source.take(v=>v.type==='input'),{...pointer({topologyEpoch:2,epoch:1}),sessionId:c.session.sessionId});
  f.relay.sessions.get(c.session.sessionId).lastTrackResizeAt=0;c.client.json(resize({topologyEpoch:2}));const next=await source.take(v=>v.type==='stream.tracks.resize'&&v.topologyEpoch===3);assert.equal(next.tracks[1].epoch,2);
  c.client.json({type:'session.stop'});await f.host.take(v=>v.type==='session.stop');await hostStillOnline(f);
});

test('Forged group rollback dimensions end only their session; a lost tuple rejects later viewport mutation',async t=>{
  const f=await fixture(t,{profile:true});await f.advertise(groupProfileCapabilities());const c=await f.connect(groupProfileRequest());const{source}=await ready(f,c);
  c.client.json(resize());const command=await source.take(v=>v.type==='stream.tracks.resize');const restored=restoredProfileViewport(c.session.sessionId,command);restored.tracks[0].width+=2;source.json(restored);
  await c.client.take(v=>v.type==='session.ended');await hostStillOnline(f);
  const next=await f.connect(groupProfileRequest());const live=await ready(f,next);await f.advertise({...capabilities,streamProfileProtocol:'console-profile-v1'});
  next.client.json(resize());const rejected=await next.client.take(v=>v.type==='stream.tracks.resize.failed');assert.equal(rejected.unchanged,true);assert.equal(rejected.reason,'track_streaming_unavailable');await barrier(live.source,next.client,'lost-group-tuple');assert.equal(live.source.messages.filter(v=>v.type==='stream.tracks.resize').length,0);
});

test('A group tuple changed during Proxmox identity verification cannot dispatch a stale private profile command',async t=>{
  let release,started;const waiting=new Promise(resolve=>{started=resolve;});const f=await fixture(t,{profile:true,relayOptions:{proxmox:{consoleIdentity:async()=>{started();return new Promise(resolve=>{release=resolve;});}}}});
  await f.advertise(groupProfileCapabilities());const session=f.create(groupProfileRequest());const client=await socket(f.localUrl(session.webSocketUrl),'Bearer '+f.aliceToken);await waiting;
  await f.advertise({...capabilities,streamProfileProtocol:'console-profile-v1'});release({machineUuid,model:'std'});
  assert.equal((await client.take(v=>v.type==='stream.unavailable')).reason,'track_streaming_unavailable');assert.equal(f.host.messages.filter(v=>v.type==='session.start').length,0);await hostStillOnline(f);
});

test('A group source with current frame/decoder proof survives transient generic readiness while later requests and authorization stay gated',async t=>{
  const f=await fixture(t,{profile:true});await f.advertise(groupProfileCapabilities());const c=await f.connect(groupProfileRequest());const{source}=await ready(f,c);
  assert.equal((await f.advertise({service:true,interactiveSession:true,streaming:false,rdpPort:3389,virtualDisplayReady:false})).accepted,true);
  f.relay.revalidate();assert.equal(f.relay.sessions.has(c.session.sessionId),true);
  for(const track of['0','1']){const bytes=packet({track,pts:1});source.send(bytes);assert.deepEqual(await c.client.take(Buffer.isBuffer),bytes);}
  c.client.json(resize());const failure=await c.client.take(v=>v.type==='stream.tracks.resize.failed');assert.equal(failure.unchanged,true);assert.equal(failure.reason,'track_streaming_unavailable');assert.equal(source.messages.filter(v=>v.type==='stream.tracks.resize').length,0);
  f.store.revoke('alice','pve-170');f.relay.revalidate();assert.equal((await c.client.take(v=>v.type==='session.ended')).reason,'authorization_revoked');await hostStillOnline(f);
});

test('A group readiness transition cannot hide a stalled second video behind live first-output traffic and PCM',async t=>{
  const f=await fixture(t,{profile:true,relayOptions:{trackMediaMs:100}});await f.advertise(groupProfileCapabilities());const c=await f.connect(groupProfileRequest());const{source}=await ready(f,c);
  await f.advertise({service:true,interactiveSession:true,streaming:false,rdpPort:3389,virtualDisplayReady:false});
  let pts=0;const timer=setInterval(()=>{if(source.readyState===WebSocket.OPEN){source.send(packet({track:'0',pts:++pts}));source.send(sound(1,pts));}},10);t.after(()=>clearInterval(timer));
  assert.equal((await c.client.take(v=>v.type==='stream.unavailable')).reason,'tracks_source_stalled');await hostStillOnline(f);
});
