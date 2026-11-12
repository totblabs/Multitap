import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer, VERSION } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { safeClientDiagnosticReport } from '../src/client-diagnostics.mjs';
import { observabilityProtocol, requestedObservability, safeObservabilityMetrics, observabilityStats,
  observabilityTls, recordObservabilityForward, observabilityDiagnosticBounds,
  beginObservabilityForward, completeObservabilityForward, clearObservability,
  safeObservabilityVersion } from '../src/stream-observability.mjs';
import { requests, options, info, packet } from './track-fixture.mjs';

const origin = 'https://observability-fixture.example';
const uuid = '12345678-1234-1234-1234-123456789abd';
const baseCapabilities = { service:true, interactiveSession:true, streaming:true,   virtualDisplayReady:true };
const snapshot = metrics => ({ protocol:observabilityProtocol, metrics });
const baseStats = { type:'stream.stats', capturedFps:30, encodedFps:30, bitrateKbps:8000 };

test('Observability request is exact, optional and streaming only', () => {
  assert.equal(requestedObservability('stream', undefined), false);
  assert.equal(requestedObservability('stream', observabilityProtocol), true);
  for (const [mode, value] of [['rdp', observabilityProtocol], ['stream', null], ['stream', 'new-protocol']]) {
    assert.throws(() => requestedObservability(mode,value), { code:'invalid_observability_protocol', status:400 });
  }
});

test('Host observations are projected and cannot claim Relay or Client measurements', () => {
  assert.deepEqual(safeObservabilityMetrics({ hostQueuePackets:4, hostWriteUs:12, readbackUs:1.5,
    workerCpuPct10:1001, hostQueueOldestUs:-1, relayQueueBytes:1, token:'secret' }),
  { hostQueuePackets:4, hostWriteUs:12 });
  for (const value of [null, [], 'secret', { hostQueueBytes:Infinity }, { readbackUs:NaN }]) assert.equal(safeObservabilityMetrics(value), null);
});

test('Projection strips ungranted, stale, malformed and untrusted data without altering base statistics', () => {
  const session = { id:uuid, epoch:2, observability:false, ws:{bufferedAmount:31} };
  const value = {...baseStats,sessionId:uuid,epoch:2,observability:snapshot({hostQueuePackets:4,relayQueueBytes:1,relayForwardUs:1})};
  assert.deepEqual(observabilityStats(session,value), {...baseStats,sessionId:uuid,epoch:2});
  session.observability = true;
  const result = observabilityStats(session,value,10);
  assert.deepEqual(result.observability, snapshot({hostQueuePackets:4,relayQueueBytes:31}));
  assert.equal(value.observability.metrics.relayQueueBytes,1);
  for (const overrides of [{sessionId:'retired'},{epoch:1}]) assert.equal(observabilityStats(session,{...value,...overrides}).observability,undefined);
  session.resize = {};
  assert.equal(observabilityStats(session,value).observability,undefined);
  session.resize = null;
  for (const observation of [null,[],{protocol:'wrong',metrics:{hostQueuePackets:99}}]) {
    assert.deepEqual(observabilityStats(session,{...value,observability:observation}).observability,snapshot({relayQueueBytes:31}));
  }
});

test('Relay write observation is local, epoch bound and expires after three seconds', () => {
  const session={id:uuid,epoch:1,observability:true,ws:{bufferedAmount:0}};
  recordObservabilityForward(session,10,1,12);
  const value={...baseStats,sessionId:uuid,epoch:1};
  assert.equal(observabilityStats(session,value,13).observability.metrics.relayForwardUs,2000);
  assert.equal(observabilityStats(session,value,3013).observability.metrics.relayForwardUs,undefined);
  recordObservabilityForward(session,13,0,14);
  assert.equal(session.observabilityForward.us,2000);
  session.epoch=2;
  assert.equal(observabilityStats(session,{...value,epoch:2},15).observability.metrics.relayForwardUs,undefined);
});

test('Pending video write observation survives reorder/failure, resets on disconnect and stays bounded', () => {
  const session={id:uuid,epoch:1,observability:true,ws:{bufferedAmount:7}};
  const value={...baseStats,sessionId:uuid,epoch:1};
  const first=beginObservabilityForward(session,1,10),second=beginObservabilityForward(session,1,12);
  assert.deepEqual(observabilityStats(session,value,15).observability.metrics,
    {relayQueueBytes:7,relayVideoQueuePackets:2,relayVideoQueueOldestUs:5000});
  completeObservabilityForward(session,second,true,16);
  assert.equal(observabilityStats(session,value,17).observability.metrics.relayVideoQueuePackets,1);
  assert.equal(observabilityStats(session,value,17).observability.metrics.relayVideoQueueOldestUs,7000);
  completeObservabilityForward(session,first,false,18);
  assert.equal(observabilityStats(session,value,19).observability.metrics.relayVideoQueueOldestUs,0);
  assert.equal(observabilityStats(session,value,19).observability.metrics.relayForwardUs,4000);
  const old=beginObservabilityForward(session,1,20);
  session.epoch=2;
  const fresh=beginObservabilityForward(session,2,21);
  completeObservabilityForward(session,old,true,22);
  assert.equal(session.observabilityPending.pending.size,1);
  clearObservability(session);
  completeObservabilityForward(session,fresh,true,23);
  assert.equal(session.observabilityPending,null);assert.equal(session.observabilityForward,null);
  for(let n=0;n<4100;n++)beginObservabilityForward(session,2,24+n);
  assert.equal(session.observabilityPending.pending.size,4096);
  assert.equal(observabilityStats(session,{...value,epoch:2},5000).observability.metrics.relayVideoQueuePackets,undefined);
  assert.equal(observabilityStats(session,{...value,epoch:2},5000).observability.metrics.relayQueueBytes,7);
});

test('Path evidence uses resolved access context, never forwarded header claims alone', () => {
  const req={socket:{encrypted:false},headers:{'x-forwarded-proto':'https'}};
  assert.equal(observabilityTls(req,{origin,proxied:true}),'proxy');
  assert.equal(observabilityTls(req,{origin}),'unknown');
  assert.equal(observabilityTls(req,{origin:'http://fixture.invalid',proxied:false}),'plain');
  assert.equal(observabilityTls({...req,socket:{encrypted:true}},{origin,proxied:false}),'direct');
});

test('Version evidence accepts bounded release versions and represents unsupported reports as null', () => {
  assert.equal(safeObservabilityVersion('0.5.91'),'0.5.91');
  for(const value of [undefined,null,'','private-host-name','0.2.108+1','1234567.1.1',{},['0.2.108']]) {
    assert.equal(safeObservabilityVersion(value),null);
  }
});

test('Diagnostic projection accepts exact bounded metrics and still removes content', () => {
  const values=Object.fromEntries(Object.entries(observabilityDiagnosticBounds).map(([key,max])=>[key,max]));
  const report={protocol:'client-diagnostics-v1',runId:'a'.repeat(32),sequence:1,version:'0.4.130',events:[{
    event:'streamStats',time:'2026-10-30T09:00:00.000Z',values:{...values,token:'secret',readbackMaxUs:60000001,inputGateQueuePackets:129},
  }]};
  const safe=safeClientDiagnosticReport(report);
  assert.ok(safe);assert.equal(safe.events[0].values.token,undefined);
  delete values.readbackMaxUs;delete values.inputGateQueuePackets;
  assert.deepEqual(safe.events[0].values,values);
});

async function socket(url, authorization) {
  const ws=new WebSocket(url,{headers:{Authorization:authorization},perMessageDeflate:false});
  await new Promise((resolve,reject)=>{ws.once('open',resolve);ws.once('error',reject);});
  const pending=[],waiters=[];
  ws.on('message',(data,binary)=>{
    const value=binary?data:JSON.parse(data.toString('utf8'));
    const index=waiters.findIndex(row=>row.accept(value));
    if(index<0)pending.push(value);else{const row=waiters.splice(index,1)[0];clearTimeout(row.timer);row.resolve(value);}
  });
  ws.json=value=>ws.send(JSON.stringify(value));
  ws.take=(accept=()=>true)=>{
    const index=pending.findIndex(accept);if(index>=0)return Promise.resolve(pending.splice(index,1)[0]);
    return new Promise((resolve,reject)=>{const row={accept,resolve};row.timer=setTimeout(()=>reject(Error('fixture_timeout')),2000);waiters.push(row);});
  };
  return ws;
}

async function fixture(t,{capabilities=baseCapabilities}={}) {
  const store=new Store(':memory:'),vault=new Vault(randomBytes(32));
  store.addUser({name:'alice',passwordHash:'unused-fixture-password-hash'});
  store.syncProxmox([{vmid:170,node:'fixture',name:'Fixture',status:'running'}],'fixture');
  store.assign('alice','pve-170');
  const user=store.findUser('alice'),token=newToken(),hostToken=newToken(),code=newToken();
  store.createSession(user,tokenHash(token),Date.now(),60000);
  store.issueEnrollment('pve-170',uuid,tokenHash(code),'fixture');
  store.consumeEnrollment(tokenHash(code),uuid,{hostname:'Fixture',version:'0.2.108',tokenHash:tokenHash(hostToken)});
  const server=await buildServer({store,vault,publicOrigin:origin,relayOptions:{authorizationMs:100,heartbeatMs:10000},logger:{error:v=>assert.fail(v)}});
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  t.after(async()=>{await new Promise(resolve=>server.close(resolve));store.close();});
  const base=`http://127.0.0.1:${server.address().port}`;
  const local=path=>base.replace(/^http/,'ws')+new URL(path,origin).pathname;
  const host=await socket(local('/api/v1/hosts/channel'),'Host '+hostToken);
  host.json({type:'host.capabilities',capabilities});assert.equal((await host.take(v=>v.type==='host.capabilities.ack')).accepted,true);
  async function call(data){const response=await fetch(base+'/api/v1/desktops/pve-170/connect',{method:'POST',
    headers:{Authorization:'Bearer '+token,Origin:origin,'Content-Type':'application/json'},body:JSON.stringify(data)});
    return {status:response.status,body:await response.json()};}
  async function connect(data){const response=await call(data);assert.equal(response.status,200);const grant=response.body;
    const client=await socket(local(grant.webSocketUrl),'Bearer '+token);const start=await host.take(v=>v.type==='session.start');return {grant,client,start};}
  return {server,host,call,connect,local,token,hostToken};
}

test('REST and Host start negotiate snapshots only when requested and supported', async t => {
  for(const [support,request] of [[false,false],[true,false],[false,true],[true,true]]) await t.test(`${support}/${request}`,async t=>{
    const f=await fixture(t,{capabilities:{...baseCapabilities,...(support?{observabilityProtocol}:{})}});
    const c=await f.connect(request?{observabilityProtocol}:{});
    assert.equal(c.grant.observabilityProtocol,support&&request?observabilityProtocol:undefined);
    assert.equal(c.start.observabilityProtocol,c.grant.observabilityProtocol);
    assert.equal(c.grant.observabilityPath===undefined,!(support&&request));
    assert.equal(c.grant.observabilityVersions===undefined,!(support&&request));
    if(support&&request){
      assert.deepEqual(c.grant.observabilityPath,{clientTls:'unknown',hostTls:'unknown'});
      assert.deepEqual(c.grant.observabilityVersions,{broker:VERSION,host:'0.2.108'});
    }
  });
});

test('Negotiated grant does not serialize an invalid authenticated Host version', async t=>{
  const f=await fixture(t,{capabilities:{...baseCapabilities,observabilityProtocol}});
  f.server.relay.hosts.get('pve-170').identity.version='private-host-name';
  const c=await f.connect({observabilityProtocol});
  assert.deepEqual(c.grant.observabilityVersions,{broker:VERSION,host:null});
  assert.equal(JSON.stringify(c.grant).includes('private-host-name'),false);
});

test('REST rejects malformed and RDP requests before reserving a session', async t=>{
  const f=await fixture(t);
  for(const data of [{observabilityProtocol:null},{observabilityProtocol:'wrong'}]) {
    const result=await f.call(data);assert.equal(result.status,400);assert.equal(result.body.error.code,'invalid_observability_protocol');
  }
  // RDP is not offered: refused as such before its fields are read.
  const rdp=await f.call({mode:'rdp',observabilityProtocol});assert.equal(rdp.status,409);assert.equal(rdp.body.error.code,'rdp_not_supported');
  assert.equal(f.server.relay.sessions.size,0);
});

test('Single stream relays valid snapshots and ignores stale/malformed optional observations', async t=>{
  const f=await fixture(t,{capabilities:{...baseCapabilities,observabilityProtocol}});
  const c=await f.connect({observabilityProtocol});
  const stats={...baseStats,sessionId:c.grant.sessionId,epoch:0};
  f.host.json({...stats,observability:snapshot({hostQueuePackets:3,hostQueueBytes:-1,relayForwardUs:999})});
  const first=await c.client.take(v=>v.type==='stream.stats');
  assert.equal(first.observability.metrics.hostQueuePackets,3);assert.equal(first.observability.metrics.hostQueueBytes,undefined);
  assert.equal(first.observability.metrics.relayForwardUs,undefined);
  f.host.json({...stats,sessionId:uuid,observability:snapshot({hostQueuePackets:77})});
  assert.equal((await c.client.take(v=>v.type==='stream.stats')).observability,undefined);
  f.host.json({...stats,observability:'bad'});
  assert.equal((await c.client.take(v=>v.type==='stream.stats')).observability.metrics.relayQueueBytes,0);
  assert.equal(f.host.readyState,WebSocket.OPEN);assert.equal(c.client.readyState,WebSocket.OPEN);
});

test('Unrequested observations are removed while legacy statistics remain usable', async t=>{
  const f=await fixture(t,{capabilities:{...baseCapabilities,observabilityProtocol}});
  const c=await f.connect({});
  f.host.json({...baseStats,observability:snapshot({hostQueuePackets:3})});
  assert.deepEqual(await c.client.take(v=>v.type==='stream.stats'),baseStats);
});

test('A failed video write releases passive observations and ends only its controller', async t=>{
  const f=await fixture(t,{capabilities:{...baseCapabilities,observabilityProtocol}});
  const c=await f.connect({observabilityProtocol});
  f.host.json({type:'stream.info',sessionId:c.grant.sessionId,epoch:0,codec:'h264',width:1920,height:1080,
    sourceWidth:1920,sourceHeight:1080,fpsTarget:60,audio:{sampleRate:48000,channels:2,format:'s16le',available:false}});
  await c.client.take(v=>v.type==='stream.info');
  const active=f.server.relay.sessions.get(c.grant.sessionId),send=active.ws.send.bind(active.ws);
  active.ws.send=(payload,options,callback)=>{
    if(options?.binary)queueMicrotask(()=>callback(Error('fixture local write failure')));
    else send(payload,options,callback);
  };
  const closed=once(c.client,'close');
  const bytes=Buffer.alloc(13);bytes[0]=1;bytes[1]=1;f.host.send(bytes);
  await closed;
  await f.host.take(v=>v.type==='input.reset');
  assert.equal(active.observabilityPending,null);
  assert.equal(active.observabilityForward,null);
  assert.equal(f.server.relay.sessions.size,0);
  assert.equal(f.host.readyState,WebSocket.OPEN);
});

test('Independent tracks preserve strict base validation and negotiate their own snapshot', async t=>{
  const f=await fixture(t,{capabilities:{...baseCapabilities,observabilityProtocol,videoTrackProtocol:'tracks-v1',trackViewportProvisioning:true,
    monitors:requests.map(row=>({id:row.id,width:row.width,height:row.height,primary:row.id==='0'}))}});
  const c=await f.connect({mode:'stream',options,mediaProtocol:'tracks-v1',tracks:requests,observabilityProtocol});
  const source=await socket(f.local(c.start.tunnelUrl),'Host '+f.hostToken);
  source.json(info(c.grant.sessionId));await c.client.take(v=>v.type==='stream.tracks.info');
  source.send(packet());await c.client.take(Buffer.isBuffer);
  source.json({type:'stream.tracks.stats',sessionId:c.grant.sessionId,topologyEpoch:1,measuredMs:1000,
    tracks:requests.map(row=>({id:row.id,epoch:1,capturedFps:30,encodedFps:30,bitrateKbps:4000})),
    observability:snapshot({workerCpuPct10:200,relayQueueBytes:1})});
  const stats=await c.client.take(v=>v.type==='stream.tracks.stats');
  assert.equal(stats.observability.metrics.workerCpuPct10,200);
  assert.equal(stats.observability.metrics.relayQueueBytes,0);
  assert.equal(typeof stats.observability.metrics.relayForwardUs,'number');
  assert.equal(source.readyState,WebSocket.OPEN);
});
