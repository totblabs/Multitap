import test from'node:test';import assert from'node:assert/strict';import{randomBytes,createHash}from'node:crypto';import{once}from'node:events';import{setTimeout as delay}from'node:timers/promises';import{WebSocket}from'ws';
import{Store}from'../src/store.mjs';import{Vault}from'../src/vault.mjs';import{buildServer}from'../src/http.mjs';import{newToken,tokenHash}from'../src/security.mjs';
import { Relay } from '../src/relay.mjs';

async function admissionTicket(t, { validForMs = 5000 } = {}) {
 const f = await fixture(t);
 const availability = await offer(f, { validForMs });
 const response = await f.connect({ signinProtocol: 'signin-stream-v1', signinAvailabilityId: availability.availabilityId });
 assert.equal(response.status, 200);
 return { ...f, grant: response.body };
}
async function attachAdmission(f) {
 const client = new WebSocket(f.base.replace('http:', 'ws:') + '/api/v1/sessions/' + f.grant.sessionId,
  { headers: { Authorization: 'Bearer ' + f.token } });
 f.sockets.push(client);
 await once(client, 'open');
 return client;
}
test('real Host start receives a fresh Broker-only admission bound to its current eligibility', async t => {
 const f = await admissionTicket(t);
 assert.equal(f.grant.signinAdmission, undefined);
 await attachAdmission(f);
 const start = await f.host.take(v => v.type === 'session.start');
 assert.equal(start.signinAdmission.protocol, 'signin-broker-admission-v1');
 assert.equal(start.signinAdmission.grantRevision, 1);
 assert.equal(start.signinAdmission.eligibilityRevision, 1);
 assert.equal(start.signinAdmission.serviceInstance, instance);
 assert.ok(start.signinAdmission.validForMs >= 1 && start.signinAdmission.validForMs <= 1000);
});
test('expiry between HTTP reservation and actual WebSocket attach dispatches no Host start', async t => {
 const f = await admissionTicket(t, { validForMs: 250 });
 let starts = 0;
 f.host.on('message', (data, binary) => { if (!binary && JSON.parse(data).type === 'session.start') starts++; });
 await delay(270);
 await attachAdmission(f);
 await delay(30);
 assert.equal(starts, 0);
 assert.equal(f.server.relay.sessions.size, 0);
});
test('a new Windows eligibility realm invalidates a reserved start instead of promoting the old ticket', async t => {
 const f = await admissionTicket(t);
 const newer = await offer(f, { revision: 2, windowsState: 'no-user' });
 assert.equal(newer.accepted, true);
 let starts = 0;
 f.host.on('message', (data, binary) => { if (!binary && JSON.parse(data).type === 'session.start') starts++; });
 await attachAdmission(f);
 await delay(30);
 assert.equal(starts, 0);
 assert.equal(f.server.relay.sessions.size, 0);
});
test('HTTP cannot supply its own privileged service admission', async t => {
 const f = await fixture(t), availability = await offer(f);
 const response = await f.connect({ signinProtocol: 'signin-stream-v1', signinAvailabilityId: availability.availabilityId,
  signinAdmission: { protocol: 'signin-broker-admission-v1', grantRevision: 123 } });
 assert.equal(response.status, 400);
 assert.equal(f.server.relay.sessions.size, 0);
});
test('exhausted service admission generation never wraps or dispatches', async t => {
 const f = await admissionTicket(t);
 const session = f.server.relay.sessions.get(f.grant.sessionId);
 f.server.relay.signinAdmissionRevisions.set(session.host, 0xffffffff);
 let starts = 0;
 f.host.on('message', (data, binary) => { if (!binary && JSON.parse(data).type === 'session.start') starts++; });
 await attachAdmission(f);
 await delay(30);
 assert.equal(starts, 0);
 assert.equal(f.server.relay.sessions.size, 0);
 assert.equal(Relay.prototype.signinStartAdmission.call(f.server.relay, session), null);
});
test('fresh same-realm eligibility refresh between reservation and attach starts exactly once', async t => {
 const f = await fixture(t), first = await offer(f);
 const response = await f.connect({ signinProtocol: 'signin-stream-v1', signinAvailabilityId: first.availabilityId });
 assert.equal(response.status, 200);
 const refreshed = await offer(f, { revision: 2 });
 assert.equal(refreshed.availabilityId, first.availabilityId);
 let starts = 0;
 f.host.on('message', (data, binary) => { if (!binary && JSON.parse(data).type === 'session.start') starts++; });
 await attachAdmission({ ...f, grant: response.body });
 const command = await f.host.take(v => v.type === 'session.start');
 assert.equal(command.signinAdmission.eligibilityRevision, 2);
 assert.equal(starts, 1);
});
test('discovery withdrawal preserves the existing view and rejects old reservation authority', async t => {
 const f = await started(t);
 const previous = f.server.relay.signinOffers.current(f.session.host);
 f.host.json({ type: 'host.signin.eligibility.withdraw', protocol: 'signin-service-v1', serviceInstance: instance, revision: 2 });
 const ack = await f.host.take(v => v.type === 'host.signin.eligibility.ack');
 assert.equal(ack.accepted, true);
 assert.equal(f.server.relay.sessions.get(f.grant.sessionId), f.session);
 assert.equal(f.client.readyState, WebSocket.OPEN);
 const refused = await f.connect({ signinProtocol: 'signin-stream-v1', signinAvailabilityId: previous.availabilityId });
 assert.equal(refused.status, 409); assert.equal(refused.body.error.code, 'signin_stream_unavailable');
 const replayed = await offer(f, { revision: 2 }); assert.equal(replayed.accepted, false);
 assert.equal(f.host.readyState, WebSocket.OPEN);
});
test('withdrawal before attach dispatches no secure worker start', async t => {
 const f = await admissionTicket(t);
 f.host.json({ type: 'host.signin.eligibility.withdraw', protocol: 'signin-service-v1', serviceInstance: instance, revision: 2 });
 await f.host.take(v => v.type === 'host.signin.eligibility.ack');
 let starts = 0;
 f.host.on('message', (data, binary) => { if (!binary && JSON.parse(data).type === 'session.start') starts++; });
 await attachAdmission(f); await delay(25);
 assert.equal(starts, 0); assert.equal(f.server.relay.sessions.size, 0);
});
const origin='https://signin-fixture.invalid',machine='12345678-1234-1234-1234-123456789abc',instance='a'.repeat(32);
const lockedCaps={service:true,interactiveSession:false,streaming:false,virtualDisplayReady:false,signinServiceProtocol:'signin-service-v1'};
async function fixture(t,enabled=true){
 const store=new Store(':memory:'),vault=new Vault(randomBytes(32));const admin=store.addUser({name:'signin-admin',role:'admin',passwordHash:'synthetic'});store.addUser({name:'signin-user',passwordHash:'synthetic'});store.syncProxmox([{vmid:247,node:'fixture',name:'fixture',status:'running',machineUuid:machine}],admin);store.replaceVmAssignments('signin-user',['pve-247'],admin);
 const token=newToken();store.createSession(store.findUser('signin-user'),tokenHash(token),Date.now(),3600000,'client');const code=newToken(),hostToken=newToken();store.issueEnrollment('pve-247',machine,tokenHash(code),admin);store.consumeEnrollment(tokenHash(code),machine,{hostname:'fixture',version:'1.0.0',tokenHash:tokenHash(hostToken)});
 const server=await buildServer({store,vault,publicOrigin:origin,proxmox:{},relayOptions:{heartbeatMs:10000,authorizationMs:20,...(enabled?{signinOptions:{enabled:true,qualifiedService:true,challengeMs:100}}:{})},logger:{error:value=>assert.fail(value)}});await new Promise(r=>server.listen(0,'127.0.0.1',r));const base='http://127.0.0.1:'+server.address().port,sockets=[];
 t.after(async()=>{for(const ws of sockets)ws.terminate();await new Promise(r=>server.close(r));store.close();});
 const host=new WebSocket(base.replace('http:','ws:')+'/api/v1/hosts/channel',{headers:{Authorization:'Host '+hostToken,'X-Multitap-Host-Protocol':'capabilities-v1'}});sockets.push(host);await once(host,'open');
 const queue=[],waiters=[];host.on('message',(bytes,binary)=>{const value=binary?bytes:JSON.parse(bytes.toString());const i=waiters.findIndex(w=>w.match(value));if(i>=0){const w=waiters.splice(i,1)[0];clearTimeout(w.timer);w.resolve(value);}else queue.push(value);});
 host.take=(match)=>{const i=queue.findIndex(match);if(i>=0)return Promise.resolve(queue.splice(i,1)[0]);return new Promise((resolve,reject)=>{const w={match,resolve,timer:setTimeout(()=>{waiters.splice(waiters.indexOf(w),1);reject(Error('fixture-message-timeout'));},1000)};waiters.push(w);});};t.after(()=>{for(const w of waiters)clearTimeout(w.timer);});host.json=v=>host.send(JSON.stringify(v));
 host.json({type:'host.capabilities',capabilities:lockedCaps});const ack=await host.take(v=>v.type==='host.capabilities.ack');
 const connect=async(extra={})=>{const response=await fetch(base+'/api/v1/desktops/pve-247/connect',{method:'POST',headers:{Authorization:'Bearer '+token,Origin:origin,'Content-Type':'application/json'},body:JSON.stringify({mode:'stream',resizeProtocol:'epoch-v1',options:{maxWidth:1280,maxHeight:800,fps:60,bitrate:12000000,audio:false,monitor:'0'},...extra})});return{status:response.status,body:await response.json()};};
 return{server,store,host,ack,connect,token,base,sockets};
}
test('actual HTTP explicit sign-in is unavailable by default and creates no assigned ticket',async t=>{const f=await fixture(t,false);assert.equal(f.ack.signinServiceProtocol,undefined);const r=await f.connect({signinProtocol:'signin-stream-v1',signinAvailabilityId:'b'.repeat(32)});assert.equal(r.status,409);assert.equal(r.body.error.code,'signin_stream_unavailable');assert.equal(f.server.relay.sessions.size,0);});
test('actual qualified injected Host negotiation leaves ordinary locked streaming unavailable',async t=>{const f=await fixture(t);assert.equal(f.ack.signinServiceProtocol,'signin-service-v1');const r=await f.connect();assert.equal(r.status,409);assert.equal(r.body.error.code,'stream_unavailable');});
test('actual Host eligibility then exact HTTP opt-in creates only scoped composite sign-in grant',async t=>{const f=await fixture(t);f.host.json({type:'host.signin.eligibility',protocol:'signin-service-v1',serviceInstance:instance,revision:1,windowsState:'locked',outputs:1,validForMs:5000});const ack=await f.host.take(v=>v.type==='host.signin.eligibility.ack');assert.equal(ack.accepted,true);const r=await f.connect({signinProtocol:'signin-stream-v1',signinAvailabilityId:ack.availabilityId});assert.equal(r.status,200);assert.equal(r.body.signin.phase,'signin');assert.equal(r.body.signin.mediaScope.epoch,1);assert.equal(r.body.transport,'media-epoch-v1');assert.equal(r.body.udpDirectProtocol,undefined);assert.equal(r.body.sessionResumeProtocol,undefined);});
async function started(t){const f=await fixture(t);f.host.json({type:'host.signin.eligibility',protocol:'signin-service-v1',serviceInstance:instance,revision:1,windowsState:'locked',outputs:1,validForMs:5000});const offer=await f.host.take(v=>v.type==='host.signin.eligibility.ack');const response=await f.connect({signinProtocol:'signin-stream-v1',signinAvailabilityId:offer.availabilityId});assert.equal(response.status,200);const grant=response.body,client=new WebSocket(f.base.replace('http:','ws:')+'/api/v1/sessions/'+grant.sessionId,{headers:{Authorization:'Bearer '+f.token}});f.sockets.push(client);const incoming=[];client.on('message',(bytes,binary)=>incoming.push(binary?Buffer.from(bytes):JSON.parse(bytes.toString())));await once(client,'open');const command=await f.host.take(v=>v.type==='session.start');assert.deepEqual(command.signin,grant.signin);return{...f,grant,client,incoming,session:f.server.relay.sessions.get(grant.sessionId)};}
const receipt=(f,revision=1,patch={})=>({type:'host.signin.authority',protocol:'signin-service-v1',sessionId:f.grant.sessionId,serviceInstance:instance,revision,binding:f.grant.signin,state:'current',validForMs:1000,renewAfterMs:500,...patch});
const wire=(f,type,extra={})=>({type,protocol:'signin-stream-v1',sessionId:f.grant.sessionId,workerGeneration:f.grant.signin.workerGeneration,transitionId:f.grant.signin.transitionId,mediaScope:f.grant.signin.mediaScope,...extra});
async function firstFrame(f,{wrongHash=false,revision=1}={}){
 f.host.json(receipt(f,revision));const resize=f.session.signinResize,firstFrameId=revision.toString(16).padStart(32,'c'),info={type:'stream.info',sessionId:f.grant.sessionId,epoch:f.grant.signin.mediaScope.epoch,codec:'h264',width:1280,height:800,sourceWidth:1280,sourceHeight:800,fpsTarget:60,audio:{sampleRate:48000,channels:2,format:'s16le',available:false},...(resize?{requestId:resize.requestId,resizeId:resize.resizeId}:{})};
 f.host.json(wire(f,'stream.phase.info',{phase:f.grant.signin.phase,firstFrameId,info}));const query=await f.host.take(v=>v.type==='host.signin.authority.query');assert.equal(query.operation,'first-frame');
 f.host.json({type:'host.signin.authority.result',protocol:'signin-service-v1',sessionId:f.grant.sessionId,binding:f.grant.signin,challengeId:query.challengeId,serviceInstance:instance,revision,state:'current'});
 const payload=Buffer.from([0,0,0,1,0x67,0,0,0,1,0x68,0,0,0,1,0x65]),packet=Buffer.alloc(12+payload.length);packet[0]=1;packet[1]=1;packet.writeUInt16LE(f.grant.signin.mediaScope.epoch,2);packet.writeBigUInt64LE(BigInt(revision),4);payload.copy(packet,12);
 f.host.json({type:'host.signin.first-frame',protocol:'signin-service-v1',sessionId:f.grant.sessionId,binding:f.grant.signin,challengeId:query.challengeId,serviceInstance:instance,revision,firstFrameId,sourceSequence:'1',ptsUs:String(revision),payloadSHA256:wrongHash?'0'.repeat(64):createHash('sha256').update(payload).digest('hex'),width:1280,height:800});f.host.send(packet);await delay(30);return{firstFrameId,packet,info,revision};
}
async function decoded(f,frame){f.client.send(JSON.stringify(wire(f,'stream.phase.decoded',{firstFrameId:frame.firstFrameId})));const q=await f.host.take(v=>v.type==='host.signin.authority.query');assert.equal(q.operation,'decoded');f.host.json({type:'host.signin.authority.result',protocol:'signin-service-v1',sessionId:f.grant.sessionId,binding:f.grant.signin,challengeId:q.challengeId,serviceInstance:instance,revision:frame.revision,state:'current'});return f.host.take(v=>v.type==='stream.phase.decoded');}
test('a Host refusing a sign-in start ends that session and keeps its channel',async t=>{const f=await started(t);f.host.json({type:'stream.unavailable',sessionId:f.grant.sessionId,reason:'signin_stream_unavailable'});await delay(40);assert.equal(f.host.readyState,WebSocket.OPEN);assert.equal(f.server.relay.sessions.has(f.grant.sessionId),false);assert.equal(f.store.db.prepare("SELECT COUNT(*) n FROM audit_events WHERE event='host_channel_closed'").get().n,0);});
test('a sign-in session ending with a fixed Host reason keeps the Host channel',async t=>{const f=await started(t);f.host.json({type:'stream.unavailable',sessionId:f.grant.sessionId,reason:'signin_restore_pending'});await delay(40);assert.equal(f.host.readyState,WebSocket.OPEN);assert.equal(f.server.relay.sessions.has(f.grant.sessionId),false);});
test('actual locked ticket dispatches sign-in start without relaxing ordinary readiness',async t=>{const f=await started(t);assert.equal(f.server.relay.modes(f.session.host).stream,false);assert.equal(f.session.hostDispatched,true);assert.equal(f.session.signinPolicy.state,'await_host_frame');});
test('actual first encoded packet is matched before unchanged wrapper then exact media, not a decoded-pixel proof',async t=>{const f=await started(t),frame=await firstFrame(f);assert.equal(f.incoming.length,2);assert.equal(f.incoming[0].type,'stream.phase.info');assert.deepEqual(f.incoming[1],frame.packet);assert.equal(f.session.signinPolicy.state,'await_client_decode');});
test('wrong actual first-packet digest never forwards wrapper/media',async t=>{const f=await started(t);await firstFrame(f,{wrongHash:true});assert.equal(f.incoming.some(v=>Buffer.isBuffer(v)||v.type==='stream.phase.info'),false);assert.equal(f.server.relay.sessions.size,0);});
test('actual Client decode ACK awaits one fresh check; subsequent scoped inputs add no challenge roundtrip',async t=>{const f=await started(t),frame=await firstFrame(f);f.client.send(JSON.stringify(wire(f,'stream.phase.decoded',{firstFrameId:frame.firstFrameId})));const query=await f.host.take(v=>v.type==='host.signin.authority.query');assert.equal(query.operation,'decoded');f.host.json({type:'host.signin.authority.result',protocol:'signin-service-v1',sessionId:f.grant.sessionId,binding:f.grant.signin,challengeId:query.challengeId,serviceInstance:instance,revision:1,state:'current'});await f.host.take(v=>v.type==='stream.phase.decoded');const control=wire(f,'input.phase',{input:{type:'input',event:'key',vk:65,scan:30,down:true,extended:false}});f.client.send(JSON.stringify(control));assert.deepEqual(await f.host.take(v=>v.type==='input.phase'),control);assert.equal(f.server.relay.signinChallenges.size,0);});
test('actual old generation input is never restamped or forwarded',async t=>{const f=await started(t);await firstFrame(f);f.client.send(JSON.stringify(wire(f,'input.phase',{workerGeneration:2,input:{type:'input.reset'}})));await delay(20);assert.equal(f.session.signinPolicy.state,'await_client_decode');assert.equal(f.server.relay.signinChallenges.size,0);});
test('actual service withdrawal closes current phase, not just future connect readiness',async t=>{const f=await started(t);f.host.json(receipt(f));f.host.json(receipt(f,2,{state:'withdrawn'}));await delay(30);assert.equal(f.server.relay.sessions.size,0);assert.ok(['closed','invalidated'].includes(f.session.signinPolicy.state));});
test('one actual WS pair transitions sign-in to desktop only after original worker exit and fresh frame/decode, with no redirection grant',async t=>{
 const f=await started(t),oldFrame=await firstFrame(f);await decoded(f,oldFrame);const oldControl=wire(f,'input.phase',{input:{type:'input.reset'}}),oldBinding=f.grant.signin;
 const next={...oldBinding,phase:'desktop',workerGeneration:2,transitionId:'d'.repeat(32),mediaScope:{kind:'composite',epoch:2}};
 f.host.json(wire(f,'stream.phase.prepare',{phase:'desktop',workerGeneration:2,transitionId:next.transitionId,mediaScope:next.mediaScope,timeoutMs:90000}));await delay(15);assert.equal(f.session.signinPolicy.state,'await_host_frame');f.grant.signin=next;
 f.host.json({type:'host.signin.previous-worker-exited',protocol:'signin-service-v1',sessionId:f.grant.sessionId,serviceInstance:instance,binding:next});const fresh=await firstFrame(f,{revision:2});await decoded(f,fresh);assert.equal(f.session.signinPolicy.state,'desktop_active');assert.equal(f.server.relay.sessions.get(f.grant.sessionId),f.session);
 f.client.send(JSON.stringify(oldControl));await delay(10);assert.equal(f.session.signinPolicy.state,'desktop_active');
 for(const key of ['clipboardWebSocketUrl','driveWebSocketUrl','usbWebSocketUrl','printWebSocketUrl','audioCodecProtocol'])assert.equal(f.grant[key],undefined);
 assert.equal(f.session.signinNoRedirects,true);assert.equal(f.server.relay.signinRedirectAvailable(f.session),false);
 // Even accidental mutable feature flags cannot turn the first-slice ban
 // into future desktop permissions. No installed resource is involved.
 Object.assign(f.session,{clipboardEnabled:true,driveEnabled:true,usb:true,print:true});for(const channel of[f.server.relay.drive,f.server.relay.usb,f.server.relay.print])assert.equal(channel.available(f.session),false);
});
test('same worker desktop resize advances only the media epoch and blocks old input until fresh ACK',async t=>{
 const f=await started(t),frame=await firstFrame(f);await decoded(f,frame);const initial=f.grant.signin,next={...initial,phase:'desktop',workerGeneration:2,transitionId:'e'.repeat(32),mediaScope:{kind:'composite',epoch:2}};
 f.host.json(wire(f,'stream.phase.prepare',{phase:'desktop',workerGeneration:2,transitionId:next.transitionId,mediaScope:next.mediaScope,timeoutMs:90000}));await delay(10);f.grant.signin=next;f.host.json({type:'host.signin.previous-worker-exited',protocol:'signin-service-v1',sessionId:f.grant.sessionId,serviceInstance:instance,binding:next});await decoded(f,await firstFrame(f,{revision:2}));
 f.client.send(JSON.stringify({type:'stream.resize',width:1280,height:800,fps:60,monitor:'0',requestId:'fixture-resize'}));const request=await f.host.take(v=>v.type==='stream.resize');assert.equal(request.signin.workerGeneration,2);assert.equal(request.epoch,3);assert.equal(f.session.signinPolicy.state,'await_host_frame');f.grant.signin={...next,mediaScope:{kind:'composite',epoch:3}};await decoded(f,await firstFrame(f,{revision:3}));assert.equal(f.session.signinPolicy.state,'desktop_active');
});
test('actual delayed authority reply after account logout cannot forward ACK or revoke a current Host channel',async t=>{const f=await started(t),frame=await firstFrame(f);f.client.send(JSON.stringify(wire(f,'stream.phase.decoded',{firstFrameId:frame.firstFrameId})));const q=await f.host.take(v=>v.type==='host.signin.authority.query');f.store.logout(tokenHash(f.token));await delay(30);f.host.json({type:'host.signin.authority.result',protocol:'signin-service-v1',sessionId:f.grant.sessionId,binding:f.grant.signin,challengeId:q.challengeId,serviceInstance:instance,revision:1,state:'current'});await delay(20);assert.equal(f.server.relay.sessions.size,0);assert.equal(f.server.relay.signinChallenges.size,0);assert.equal(f.host.readyState,WebSocket.OPEN);});
test('actual malformed first metadata is refused before a service proof can dereference it',async t=>{const f=await started(t);f.host.json(receipt(f));f.host.json(wire(f,'stream.phase.info',{phase:'signin',firstFrameId:'c'.repeat(32),info:null}));await delay(30);assert.equal(f.server.relay.sessions.size,0);assert.equal(f.server.relay.signinChallenges.size,0);});
test('actual positive lease expiry blocks video and scoped input without per-packet RPC',async t=>{const f=await started(t),frame=await firstFrame(f);await decoded(f,frame);f.host.json(receipt(f,2,{validForMs:80,renewAfterMs:40}));await delay(120);const old=f.incoming.filter(Buffer.isBuffer).length;f.host.send(frame.packet);f.client.send(JSON.stringify(wire(f,'input.phase',{input:{type:'input.reset'}})));await delay(20);assert.equal(f.incoming.filter(Buffer.isBuffer).length,old);assert.equal(f.server.relay.sessions.size,0);assert.equal(f.server.relay.signinChallenges.size,0);});
test('duplicate wrapped first metadata is idempotent and does not solicit another frame or decoder',async t=>{const f=await started(t),frame=await firstFrame(f),count=f.incoming.length;const info=wire(f,'stream.phase.info',{phase:'signin',firstFrameId:frame.firstFrameId,info:frame.info});f.host.json(info);await delay(15);assert.equal(f.incoming.length,count);assert.equal(f.server.relay.signinChallenges.size,0);await decoded(f,frame);f.host.json(info);await delay(15);assert.equal(f.session.signinPolicy.state,'signin_active');assert.equal(f.incoming.length,count);});
async function offer(f,patch={}){f.host.json({type:'host.signin.eligibility',protocol:'signin-service-v1',serviceInstance:instance,revision:1,windowsState:'locked',outputs:1,validForMs:5000,...patch});return f.host.take(v=>v.type==='host.signin.eligibility.ack');}
async function readDesktop(f,path='/desktops',header){const response=await fetch(f.base+'/api/v1'+path,{headers:{Authorization:'Bearer '+f.token,...(header===undefined?{}:{'X-Multitap-Signin-Protocol':header})}});return{status:response.status,body:await response.json()};}
test('aware assigned GET discovery returns exact wall-clock offer and its ID connects via actual HTTP',async t=>{const f=await fixture(t);await offer(f);const response=await readDesktop(f,'/desktops','signin-stream-v1');assert.equal(response.status,200);const d=response.body.desktops[0],availability=d.signinAvailability;assert.deepEqual(Object.keys(availability).sort(),['protocol','availabilityId','expiresAtMs','windowsState'].sort());assert.equal(availability.protocol,'signin-stream-v1');assert.equal(availability.windowsState,'locked');assert.ok(availability.expiresAtMs>Date.now()&&availability.expiresAtMs<=Date.now()+5000);assert.equal(d.connectionModes.stream,false);assert.equal(d.host.capabilities.signinServiceProtocol,undefined);const connection=await f.connect({signinProtocol:'signin-stream-v1',signinAvailabilityId:availability.availabilityId});assert.equal(connection.status,200);assert.equal(connection.body.signin.phase,'signin');});
test('legacy109/no-header and unknown-header assigned reads omit optional discovery without changing ordinary readiness',async t=>{const f=await fixture(t);await offer(f);for(const header of[undefined,'unknown-v1','SIGNIN-STREAM-V1','signin-stream-v1, signin-stream-v1']){const response=await readDesktop(f,'/desktops',header);assert.equal(response.status,200);assert.equal(response.body.desktops[0].signinAvailability,undefined);assert.equal(response.body.desktops[0].connectionModes.stream,false);}});
test('feature-OFF aware read has no offer and default current peers cannot enable it',async t=>{const f=await fixture(t,false);const response=await readDesktop(f,'/desktops','signin-stream-v1');assert.equal(response.status,200);assert.equal(response.body.desktops[0].signinAvailability,undefined);});
test('aware assigned detail reads do not renew or change an existing offer deadline',async t=>{const f=await fixture(t);await offer(f);const first=await readDesktop(f,'/desktops/pve-247','signin-stream-v1');await delay(10);const next=await readDesktop(f,'/desktops/pve-247','signin-stream-v1');assert.deepEqual(first.body.desktop.signinAvailability,next.body.desktop.signinAvailability);});
test('expired shorter service eligibility never survives a read-only discovery request',async t=>{const f=await fixture(t);await offer(f,{validForMs:20});await delay(35);const response=await readDesktop(f,'/desktops','signin-stream-v1');assert.equal(response.body.desktops[0].signinAvailability,undefined);});
test('malformed or missing current Host eligibility cannot be advertised by an aware request',async t=>{const f=await fixture(t);assert.equal((await offer(f,{outputs:2})).accepted,false);const malformed=await readDesktop(f,'/desktops','signin-stream-v1');assert.equal(malformed.body.desktops[0].signinAvailability,undefined);await offer(f,{revision:2});f.host.close();await once(f.host,'close');const gone=await readDesktop(f,'/desktops','signin-stream-v1');assert.equal(gone.body.desktops[0].signinAvailability,undefined);});
test('wrong assignment cannot discover an offer through an exact existing detail route',async t=>{const f=await fixture(t);await offer(f);f.store.syncProxmox([{vmid:247,node:'fixture',name:'fixture',status:'running',machineUuid:machine},{vmid:248,node:'fixture',name:'other-fixture',status:'running',machineUuid:'12345678-1234-1234-1234-123456789abd'}],'fixture');const response=await readDesktop(f,'/desktops/pve-248','signin-stream-v1');assert.equal(response.status,404);assert.equal(response.body.desktop,undefined);});
async function heldDiscovery(f){let entered,release;const enteredPromise=new Promise(r=>entered=r),pending=new Promise(r=>release=r),original=f.server.relay.hostUpdate.views.bind(f.server.relay.hostUpdate);f.server.relay.hostUpdate.views=async ids=>{entered();await pending;return original(ids);};const response=readDesktop(f,'/desktops','signin-stream-v1');await enteredPromise;return{response,release};}
test('actual aware discovery rechecks original account/hash after asynchronous list enrichment',async t=>{const f=await fixture(t);await offer(f);const work=await heldDiscovery(f);f.store.logout(tokenHash(f.token));work.release();const response=await work.response;assert.equal(response.status,401);assert.equal(response.body.desktops,undefined);});
test('actual aware discovery removes a revoked assignment after asynchronous list enrichment',async t=>{const f=await fixture(t);await offer(f);const work=await heldDiscovery(f);f.store.replaceVmAssignments('signin-user',[],'fixture');work.release();const response=await work.response;assert.equal(response.status,200);assert.deepEqual(response.body.desktops,[]);});
test('pending desktop resize never accepts an unrelated future epoch as existing worker authority',async t=>{const f=await started(t);await decoded(f,await firstFrame(f));const next={...f.grant.signin,phase:'desktop',workerGeneration:2,transitionId:'f'.repeat(32),mediaScope:{kind:'composite',epoch:2}};f.host.json(wire(f,'stream.phase.prepare',{phase:'desktop',workerGeneration:2,transitionId:next.transitionId,mediaScope:next.mediaScope,timeoutMs:90000}));await delay(10);f.grant.signin=next;f.host.json({type:'host.signin.previous-worker-exited',protocol:'signin-service-v1',sessionId:f.grant.sessionId,serviceInstance:instance,binding:next});await decoded(f,await firstFrame(f,{revision:2}));f.client.send(JSON.stringify({type:'stream.resize',width:1280,height:800,fps:60,monitor:'0',requestId:'strict-resize'}));await f.host.take(v=>v.type==='stream.resize');f.host.json(receipt(f,3,{binding:{...next,mediaScope:{kind:'composite',epoch:9}}}));await delay(20);assert.equal(f.server.relay.sessions.size,0);});
test('real ordinary feature-OFF capability updates never call optional signin withdrawal hooks',async t=>{const f=await fixture(t,false);f.server.relay.signinWithdraw=()=>assert.fail('ordinary feature-OFF path called signin withdrawal');f.host.json({type:'host.capabilities',capabilities:lockedCaps});const ack=await f.host.take(v=>v.type==='host.capabilities.ack');assert.equal(ack.accepted,true);assert.equal(ack.signinServiceProtocol,undefined);assert.equal(f.host.readyState,WebSocket.OPEN);});
test('real negotiated capability withdrawal immediately retires only its current signin session',async t=>{const f=await started(t),original=f.server.relay.signinWithdraw.bind(f.server.relay);let calls=0;f.server.relay.signinWithdraw=host=>{calls++;original(host);};const {signinServiceProtocol:_protocol,...ordinary}=lockedCaps;f.host.json({type:'host.capabilities',capabilities:ordinary});const ack=await f.host.take(v=>v.type==='host.capabilities.ack');assert.equal(calls,1);assert.equal(ack.signinServiceProtocol,undefined);assert.equal(f.server.relay.sessions.size,0);assert.equal(f.host.readyState,WebSocket.OPEN);});
