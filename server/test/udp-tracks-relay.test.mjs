// Real HTTP/WS/UDP worker; synthetic H.264 markers, no Windows capture/input.
import test from 'node:test';
import assert from 'node:assert/strict';
import {randomBytes} from 'node:crypto';
import {once} from 'node:events';
import {setTimeout as delay} from 'node:timers/promises';
import {WebSocket} from 'ws';
import {Store} from '../src/store.mjs';
import {Vault} from '../src/vault.mjs';
import {buildServer} from '../src/http.mjs';
import {newToken,tokenHash} from '../src/security.mjs';
import {requests,options,info,packet,acknowledgement,pointer,resize} from './track-fixture.mjs';
import {udpTrackFeedback,udpTrackScope} from '../src/udp-tracks.mjs';

const protocol='udp-tracks-v1';
const base={mode:'stream',options,mediaProtocol:'tracks-v1',tracks:requests,udpTracksProtocol:protocol,udpMediaProtocol:'udp-media-v1',udpDirectProtocol:'udp-direct-v1'};
const caps={service:true,interactiveSession:true,streaming:true,virtualDisplayReady:true,
  videoTrackProtocol:'tracks-v1',trackViewportProvisioning:true,udpMediaProtocol:'udp-media-v1',udpDirectHostProtocol:'udp-direct-v1',udpTracksHostProtocol:protocol,
  monitors:requests.map(t=>({id:t.id,width:t.width,height:t.height,primary:t.id==='0'}))};
const until=async predicate=>{const end=Date.now()+4000;while(!predicate()){assert.ok(Date.now()<end,'fixture_timeout');await delay(3);}};
async function fixture(t,{capabilities=caps,media=true,udp=true,allowPeerDirect=false}={}){
  const store=new Store(':memory:'),vault=new Vault(randomBytes(32)),sockets=[];
  store.addUser({name:'alice',passwordHash:'unused'});
  store.syncProxmox([{vmid:149,node:'fixture',name:'VM149',status:'running'}],'fixture');store.assign('alice','pve-149');
  const userToken=newToken(),hostToken=newToken(),code=newToken(),machine='12345678-1234-1234-1234-123456789abc';
  store.createSession(store.findUser('alice'),tokenHash(userToken),Date.now(),3600000);
  store.issueEnrollment('pve-149',machine,tokenHash(code),'fixture');
  store.consumeEnrollment(tokenHash(code),machine,{hostname:'fixture',version:'9.0.0',tokenHash:tokenHash(hostToken)});
  const server=await buildServer({store,vault,publicOrigin:'https://fixture.example',relayOptions:{authorizationMs:20,heartbeatMs:10000,udp:udp?{port:0,host:'127.0.0.1',media,allowPeerDirect,relays:{host:[{host:'fixture.example',port:18481}],client:[{host:'fixture.example',port:18481}]},logger:{warn(){}}}:null},logger:{error:v=>assert.fail(v)}});
  await new Promise(done=>server.listen(0,'127.0.0.1',done));
  t.after(async()=>{for(const ws of sockets)ws.terminate();await new Promise(done=>server.close(done));store.close();});
  const origin='http://127.0.0.1:'+server.address().port;
  async function open(path,authorization){
    const ws=new WebSocket(origin.replace('http','ws')+new URL(path,origin).pathname,{headers:{Authorization:authorization},perMessageDeflate:false});sockets.push(ws);
    ws.messages=[];ws.frames=[];ws.on('message',(data,binary)=>binary?ws.frames.push(data):ws.messages.push(JSON.parse(data)));
    ws.on('error',()=>{});await once(ws,'open');ws.json=value=>ws.send(JSON.stringify(value));return ws;
  }
  const host=await open('/api/v1/hosts/channel','Host '+hostToken);
  host.json({type:'host.capabilities',capabilities});await until(()=>host.messages.some(v=>v.type==='host.capabilities.ack'));
  assert.equal(host.messages.find(v=>v.type==='host.capabilities.ack').accepted,true);
  if(udp)await until(()=>server.relay.udp.running);
  const call=async(body=base)=>{const response=await fetch(origin+'/api/v1/desktops/pve-149/connect',{method:'POST',headers:{Authorization:'Bearer '+userToken,'Content-Type':'application/json'},body:JSON.stringify(body)});return {status:response.status,body:await response.json()};};
  async function connect(body=base){const reply=await call(body);assert.equal(reply.status,200);const id=reply.body.sessionId;
    const client=await open(reply.body.webSocketUrl,'Bearer '+userToken);await until(()=>host.messages.some(v=>v.type==='session.start'&&v.sessionId===id));
    const start=host.messages.find(v=>v.type==='session.start'&&v.sessionId===id),source=await open(start.tunnelUrl,'Host '+hostToken),metadata=info(id,start);
    source.json(metadata);await until(()=>client.messages.some(v=>v.type==='stream.tracks.info'));
    return {id,client,source,start,metadata,reply:reply.body,state:server.relay.sessions.get(id)};
  }
  async function ready(c){for(const track of ['0','1'])c.source.send(packet({track}));await until(()=>c.client.frames.length===2);c.client.json(acknowledgement(c.metadata));await until(()=>c.state.trackState.ready);}
  return {server,relay:server.relay,store,host,call,connect,ready};
}
const grants=ws=>ws.messages.filter(v=>v.type==='udp.tracks.grant');

test('relay-only policy: tracks retain grants and normal controls without direct fields even after forged state', async t => {
  const f = await fixture(t), c = await f.connect(); await f.ready(c); await until(() => grants(c.client).length === 1);
  assert.equal(c.state.udpDirect, false);
  for (const envelope of [grants(c.client)[0], grants(c.source)[0]]) for (const row of envelope.tracks) { assert.equal(row.grant.keys, true); assert.equal(row.grant.directProtocol, undefined); assert.equal(row.grant.stun, undefined); }
  assert.equal(f.relay.udp.direct.size, 0);
  c.state.udpDirect = true; f.relay.retireUdpTracks(c.state); f.relay.maybeGrantUdpTracks(c.state); await until(() => grants(c.client).length === 2);
  for (const row of grants(c.client)[1].tracks) assert.equal(row.grant.directProtocol, undefined);
  const scope = c.state.udpTrackScopes.get('0'); f.relay.udp.onPeer(c.id, 'host', [{ address: '192.168.1.2', port: 1234 }], scope); await delay(20);
  assert.equal(c.client.messages.some(v => v.type === 'udp.tracks.peer'), false); assert.equal(f.relay.udp.direct.size, 0);
  let locals = 0; const local = f.relay.udp.local.bind(f.relay.udp); f.relay.udp.local = (...args) => { locals++; return local(...args); };
  const hint = { type: 'udp.tracks.local', protocol: 'udp-direct-v1', sessionId: c.id, ...scope, address: '192.168.1.2', port: 1234 };
  c.client.json(hint); c.source.json(hint); await delay(20); assert.equal(locals, 0, 'neither channel admits peer hints under disabled policy');
  c.client.json({ type: 'input.reset' }); await until(() => c.source.messages.some(v => v.type === 'input.reset'));
  c.client.json({ type: 'input.direct', protocol: 'udp-direct-v1', sequence: 1, epoch: 1, input: { type: 'input.reset' } });
  await until(() => !f.relay.sessions.has(c.id)); assert.equal(c.source.messages.some(v => v.type === 'input.direct'), false);
});

test('dual grants require both real WSS IDRs and decoder ACK; controls use only the track tunnel',async t=>{
  const f=await fixture(t),c=await f.connect();
  assert.equal(c.reply.udpTracksProtocol,protocol);assert.equal(c.start.udpTracksProtocol,protocol);
  c.source.send(packet({track:'0'}));
  await until(()=>c.client.frames.length===1);await delay(50);assert.equal(grants(c.client).length,0);
  c.source.send(packet({track:'1'}));await until(()=>c.client.frames.length===2);await delay(40);assert.equal(grants(c.client).length,0);
  c.client.json(acknowledgement(c.metadata));await until(()=>grants(c.client).length===1&&grants(c.source).length===1);
  const cg=grants(c.client)[0],hg=grants(c.source)[0];assert.equal(cg.tracks.length,2);assert.equal(cg.generation,1);
  assert.equal(cg.tracks.filter(v=>v.primary).length,1);assert.equal(cg.tracks[0].bitrateTarget+cg.tracks[1].bitrateTarget,options.bitrate);
  const secrets=[...cg.tracks,...hg.tracks].flatMap(v=>[v.grant.id,v.grant.ticket]);assert.equal(new Set(secrets).size,8);
  assert.equal(grants(f.host).length,0);assert.ok(!JSON.stringify(cg).includes(hg.tracks[0].grant.ticket));
  const scope={sessionId:c.id,track:'1',topologyEpoch:1,epoch:1,generation:1};
  const key={type:'udp.tracks.key',protocol:'udp-media-v1',...scope,publicKey:randomBytes(32).toString('base64')};
  c.client.json(key);await until(()=>c.source.messages.some(v=>v.type===key.type));assert.deepEqual(c.source.messages.find(v=>v.type===key.type),key);
  assert.equal(f.host.messages.some(v=>v.type===key.type),false);
  c.source.json({...key,publicKey:randomBytes(32).toString('base64')});await until(()=>c.client.messages.some(v=>v.type===key.type));
  await until(()=>c.source.messages.some(v=>v.type==='udp.tracks.lease'));
  const before=c.source.messages.filter(v=>v.type===key.type).length;
  for(const bad of [{generation:2},{track:'0',epoch:2},{topologyEpoch:2},{sessionId:'foreign'}])c.client.json({...key,...bad});
  await delay(50);assert.equal(c.source.messages.filter(v=>v.type===key.type).length,before);assert.equal(c.client.readyState,WebSocket.OPEN);
});

for(const [name,settings,request] of [
  ['old Host',{capabilities:{...caps,udpTracksHostProtocol:undefined}},base],
  ['scope disabled',{media:false},base],['no UDP worker',{udp:false},base],
  ['old client',{},Object.fromEntries(Object.entries(base).filter(([key])=>key!=='udpTracksProtocol'))],
])test(`${name}: independent outputs retain WSS without any dual grant`,async t=>{
  const f=await fixture(t,settings),c=await f.connect(request);await f.ready(c);await delay(60);
  assert.equal(c.reply.udpTracksProtocol,undefined);assert.equal(c.start.udpTracksProtocol,undefined);
  assert.equal(grants(c.client).length+grants(c.source).length,0);assert.equal(c.client.frames.length,2);assert.equal(c.client.readyState,WebSocket.OPEN);
});

test('trusted constructor-only dormant direct resize retires legs, retains key releases, and rekeys epochs',async t=>{
  const f=await fixture(t,{allowPeerDirect:true}),c=await f.connect();await f.ready(c);await until(()=>grants(c.client).length===1);
  c.client.json(resize());await until(()=>c.state.trackState.pending);assert.equal(c.state.udpGranted,false);assert.equal(f.relay.udp.grants.has(c.id),false);
  c.client.json({type:'input.direct',protocol:'udp-direct-v1',sequence:1,epoch:1,input:{type:'input.reset'}});
  await until(()=>c.source.messages.some(v=>v.type==='input.direct'));assert.equal(c.client.readyState,WebSocket.OPEN);
  c.client.json({type:'input.direct',protocol:'udp-direct-v1',sequence:2,epoch:1,input:{type:'input.fence'}});
  await until(()=>c.source.messages.some(v=>v.type==='input.direct'&&v.sequence===2&&v.input.type==='input.fence'));
  const command=c.source.messages.find(v=>v.type==='stream.tracks.resize');c.source.json(info(c.id,command));
  const metadata=info(c.id,command);await until(()=>c.client.messages.filter(v=>v.type==='stream.tracks.info').length===2);
  for(const row of metadata.tracks)c.source.send(packet({track:row.id,topology:metadata.topologyEpoch,epoch:row.epoch}));
  await until(()=>c.client.frames.length===4);c.client.json(acknowledgement(metadata));
  await until(()=>grants(c.client).length===2);const grant=grants(c.client)[1];assert.equal(grant.generation,2);
  assert.equal(grant.topologyEpoch,metadata.topologyEpoch);assert.deepEqual(grant.tracks.map(row=>[row.track,row.epoch]),metadata.tracks.map(row=>[row.id,row.epoch]));
  assert.equal(grant.tracks.find(row=>row.track==='1').epoch,2);
  const input=pointer({epoch:metadata.tracks[1].epoch,topologyEpoch:metadata.topologyEpoch});
  c.client.json({type:'input.direct',protocol:'udp-direct-v1',sequence:3,epoch:input.epoch,input});
  await until(()=>c.source.messages.some(v=>v.type==='input.direct'&&v.sequence===3));
  f.store.revoke('alice','pve-149');await until(()=>!f.relay.sessions.has(c.id));assert.equal(f.relay.udp.grants.has(c.id),false);
});

test('actual-arrival reports refresh only existing scope watchdogs; old scopes cannot revive readiness',()=>{
  const video=new Map(['0','1'].map(track=>[track,{epoch:3,key:true,pts:100,lastAt:1}]));
  const scopes=new Map(['0','1'].map(track=>[track,{track,topologyEpoch:2,epoch:3,generation:4}]));
  const session={id:'fixture',udpTracks:true,udpGranted:true,udpTrackScopes:scopes,udpTrackGeneration:4,trackState:{ready:true,info:{topologyEpoch:2},video}};
  const value={type:'udp.tracks.feedback',protocol,topologyEpoch:2,generation:4,tracks:[{track:'1',epoch:3,pts:200,frames:2,bytes:200}]};
  assert.equal(udpTrackFeedback(session,value,10),true);assert.equal(video.get('1').lastAt,10);assert.equal(video.get('0').lastAt,1);assert.equal(video.get('1').pts,100);
  assert.equal(udpTrackFeedback(session,value,20),true);assert.equal(video.get('1').lastAt,10,'duplicate PTS does not keep a dead path alive');
  for(const bad of [{generation:3},{topologyEpoch:1},{tracks:[{...value.tracks[0],epoch:2}]},{tracks:[value.tracks[0],value.tracks[0]]}])assert.equal(udpTrackFeedback(session,{...value,...bad},30),false);
  session.trackState.ready=false;assert.equal(udpTrackFeedback(session,value,40),false);assert.equal(video.get('1').lastAt,10);
  session.udpGranted=false;assert.equal(udpTrackScope(session,{sessionId:'fixture',...scopes.get('1')}),null);
});
