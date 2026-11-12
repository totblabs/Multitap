import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { once } from 'node:events';
import { createServer } from 'node:http';
import { createServer as createTlsServer } from 'node:https';
import { mkdtemp,readFile,rm } from 'node:fs/promises';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { spawnSync } from 'node:child_process';
import { WebSocket } from 'ws';
import { Relay } from '../src/relay.mjs';
import { Store } from '../src/store.mjs';
import { newToken,tokenHash } from '../src/security.mjs';

const caps={service:true,interactiveSession:true,streaming:true,virtualDisplayReady:true,clipboard:true,clipboardSync:true};
const info={type:'stream.info',codec:'h264',width:1280,height:720,sourceWidth:1920,sourceHeight:1080,fpsTarget:60,audio:{sampleRate:48000,channels:2,format:'s16le'}};
const digest=bytes=>createHash('sha256').update(bytes).digest('hex');
const allowedPolicy={textToRemote:true,textToLocal:true,filesToRemote:true,filesToLocal:true};
const syncRequest=(counter,prefix='a'.repeat(32))=>prefix+'-'+BigInt(counter).toString(16).padStart(16,'0');
async function socket(url,authorization,options={}){
  return new Promise((resolve,reject)=>{
    const ws=new WebSocket(url,{perMessageDeflate:false,...options,headers:{Authorization:authorization,...options.headers}}),queue=[],waiters=[];
    ws.on('message',(bytes,binary)=>{
      const value=binary?bytes:JSON.parse(bytes.toString());const index=waiters.findIndex(w=>w.test(value));
      if(index<0)queue.push(value);else{const w=waiters.splice(index,1)[0];clearTimeout(w.timer);w.resolve(value);}
    });
    ws.on('error',()=>{});ws.once('error',reject);
    ws.once('unexpected-response',(_req,response)=>{response.resume();reject(Object.assign(new Error('upgrade_rejected'),{status:response.statusCode}));});
    ws.json=value=>ws.send(JSON.stringify(value));ws.queue=queue;
    ws.take=(test=()=>true)=>{
      const index=queue.findIndex(test);if(index>=0)return Promise.resolve(queue.splice(index,1)[0]);
      return new Promise((resolve,reject)=>{const w={test,resolve,timer:setTimeout(()=>reject(new Error('clipboard_message_timeout')),1500)};waiters.push(w);});
    };
    ws.once('open',()=>resolve(ws));
  });
}
async function fixture(t,{tls,relayOptions={},policy=allowedPolicy,sync=false}={}){
  const store=new Store(':memory:'),tokens={},users={};
  for(const name of ['alice','bobby']){users[name]=store.addUser({name,passwordHash:'fixture-only'});tokens[name]=newToken();store.createSession(store.findUser(name),tokenHash(tokens[name]),Date.now(),3600000);}
  store.syncProxmox([810,811].map(vmid=>({vmid,node:'fixture',name:'Clipboard '+vmid,status:'running'})),'fixture');
  store.assign('alice','pve-810');store.assign('bobby','pve-811');
  // Clipboard permission belongs to each assignment (user x VM).
  if(policy!==null)for(const [name,id] of [['alice','pve-810'],['bobby','pve-811']])store.updateAssignmentPolicy(name,id,{clipboardPolicy:policy},'fixture');
  const hostTokens={};for(const vmid of [810,811]){
    const code=newToken(),uuid='12345678-1234-1234-1234-'+String(vmid).padStart(12,'0');hostTokens[vmid]=newToken();
    store.issueEnrollment('pve-'+vmid,uuid,tokenHash(code),'fixture');store.consumeEnrollment(tokenHash(code),uuid,{hostname:'Fixture host',version:'0.2.3',tokenHash:tokenHash(hostTokens[vmid])});
  }
  const relay=new Relay({store,publicOrigin:'https://clipboard-fixture.example',authorizationMs:10,heartbeatMs:1000,...relayOptions});
  const server=tls?createTlsServer(tls):createServer();relay.attach(server);
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  const base=(tls?'wss:':'ws:')+'//127.0.0.1:'+server.address().port;relay.publicOrigin='https://127.0.0.1:'+server.address().port;
  const options=tls?{ca:tls.cert,rejectUnauthorized:true}:{};
  t.after(async()=>{await new Promise(resolve=>server.close(resolve));store.close();});
  async function host(capabilities=caps){
    const ws=await socket(base+'/api/v1/hosts/channel','Host '+hostTokens[810],options);
    if(capabilities){ws.json({type:'host.capabilities',capabilities});assert.equal((await ws.take(v=>v.type==='host.capabilities.ack')).accepted,true);}
    return ws;
  }
  function url(path){return base+new URL(path).pathname;}
  async function session(host,mode='stream',clipboardProtocol=mode==='stream'&&sync?'clipboard-sync-v1':false){
    const connection=relay.connect({id:users.alice},tokenHash(tokens.alice),'pve-810',mode,undefined,undefined,clipboardProtocol===false?undefined:clipboardProtocol);
    const media=await socket(url(connection.webSocketUrl),'Bearer '+tokens.alice,options);
    const start=await host.take(v=>v.type==='session.start');return{connection,media,start};
  }
  const limitsHeader={'X-Multitap-Clipboard-Limits':'2'};
  async function pair(session,extended={}){
    const client=await socket(url(session.connection.clipboardWebSocketUrl),'Bearer '+tokens.alice,{...options,headers:extended.client?limitsHeader:undefined});
    const host=await socket(url(session.start.clipboardUrl),'Host '+hostTokens[810],{...options,headers:extended.host?limitsHeader:undefined});
    const clientReady=await client.take(v=>v.type==='clipboard.ready'),hostReady=await host.take(v=>v.type==='clipboard.ready');
    assert.equal(clientReady.protocol,'clipboard-v1');assert.equal(hostReady.limits.chunkBytes,32768);
    if(session.connection.clipboardProtocol){assert.deepEqual(clientReady.clipboardPolicy,session.connection.clipboardPolicy);assert.equal(clientReady.clipboardSync,session.connection.clipboardSync);}
    else if(!extended.client)assert.deepEqual(Object.keys(clientReady).sort(),['limits','protocol','sessionId','type']);
    assert.deepEqual(hostReady.clipboardPolicy,session.connection.clipboardPolicy);assert.equal(hostReady.clipboardSync,session.connection.clipboardSync===true);return{client,host,clientReady,hostReady};
  }
  return{store,tokens,users,hostTokens,relay,server,base,options,url,host,session,pair};
}
async function sendItem(sender,receiver,requestId,index,bytes){
  for(let offset=0;offset<bytes.length;){
    const part=bytes.subarray(offset,offset+32768);sender.json({type:'clipboard.chunk',requestId,index,offset,data:part.toString('base64')});
    const value=await receiver.take(v=>v.type==='clipboard.chunk');assert.deepEqual(Buffer.from(value.data,'base64'),part);offset+=part.length;
    receiver.json({type:'clipboard.chunk.ack',requestId,index,offset});assert.equal((await sender.take(v=>v.type==='clipboard.chunk.ack')).offset,offset);
  }
  sender.json({type:'clipboard.item.end',requestId,index,sha256:digest(bytes)});
  assert.equal((await receiver.take(v=>v.type==='clipboard.item.end')).sha256,digest(bytes));
  receiver.json({type:'clipboard.item.ack',requestId,index});await sender.take(v=>v.type==='clipboard.item.ack');
}
async function commit(sender,receiver,requestId){
  sender.json({type:'clipboard.commit',requestId});await receiver.take(v=>v.type==='clipboard.commit');
  assert.equal(sender.queue.some(v=>v.type==='clipboard.complete'),false);
  receiver.json({type:'clipboard.applied',requestId});await sender.take(v=>v.type==='clipboard.complete');await receiver.take(v=>v.type==='clipboard.complete');
}
async function transferText(sender,receiver,requestId,text){
  const bytes=Buffer.from(text);sender.json({type:'clipboard.begin',requestId,format:'text',size:bytes.length});await receiver.take(v=>v.type==='clipboard.begin');
  await sendItem(sender,receiver,requestId,0,bytes);await commit(sender,receiver,requestId);
}
async function ready(t,options){const f=await fixture(t,options),host=await f.host(),session=await f.session(host),pair=await f.pair(session);return{...f,mainHost:host,...session,...pair};}

test('Streaming clipboard capability is explicit, typed and independent of RDP or legacy stream availability',async t=>{
  const f=await fixture(t),host=await f.host(null);let session=await f.session(host);
  assert.equal(session.connection.clipboardWebSocketUrl,undefined);assert.equal(session.start.clipboardUrl,undefined);
  f.relay.stop(f.relay.sessions.get(session.connection.sessionId));await host.take(v=>v.type==='session.stop');
  host.json({type:'host.capabilities',capabilities:{...caps,clipboard:'true'}});assert.equal((await host.take(v=>v.type==='host.capabilities.ack')).accepted,false);
  host.json({type:'host.capabilities',capabilities:caps});await host.take(v=>v.type==='host.capabilities.ack');
  // RDP is not offered: refused before any session or clipboard channel.
  assert.throws(()=>f.relay.connect({id:f.users.alice},tokenHash(f.tokens.alice),'pve-810','rdp'),error=>error.status===409&&error.code==='rdp_not_supported');
  assert.equal(f.relay.sessions.size,0);
});

test('default-denied accounts receive no clipboard URLs and no user-visible clipboard capability',async t=>{
  const f=await fixture(t,{policy:null}),host=await f.host(),session=await f.session(host);
  const deny={textToRemote:false,textToLocal:false,filesToRemote:false,filesToLocal:false};
  assert.deepEqual(session.connection.clipboardPolicy,deny);assert.deepEqual(session.start.clipboardPolicy,deny);
  assert.equal(session.connection.clipboardWebSocketUrl,undefined);assert.equal(session.start.clipboardUrl,undefined);
  const decorated=f.relay.decorate(f.store.listDesktops(f.users.alice),f.users.alice);
  assert.deepEqual(decorated[0].clipboardPolicy,deny);assert.equal(decorated[0].host.capabilities.clipboard,false);
  await assert.rejects(socket(f.base+'/api/v1/sessions/'+session.connection.sessionId+'/clipboard','Bearer '+f.tokens.alice),error=>error.status===401);
});

test('new and legacy client/host combinations retain manual clipboard compatibility and negotiate automatic watch only mutually',async t=>{
  for(const syncHost of [false,true])for(const syncClient of [false,true])await t.test(`host=${syncHost},client=${syncClient}`,async t=>{
    const f=await fixture(t),{clipboardSync,...oldCaps}=caps,host=await f.host(syncHost?caps:oldCaps);
    const session=await f.session(host,'stream',syncClient?'clipboard-sync-v1':false),pair=await f.pair(session);
    assert.equal(session.connection.clipboardProtocol,syncClient?'clipboard-sync-v1':undefined);
    assert.equal(session.connection.clipboardSync,syncClient?syncHost:undefined);
    if(syncClient){assert.equal(pair.clientReady.clipboardSync,syncHost);assert.deepEqual(pair.clientReady.clipboardPolicy,allowedPolicy);}
    else assert.deepEqual(Object.keys(pair.clientReady).sort(),['limits','protocol','sessionId','type']);
    assert.deepEqual(pair.hostReady.clipboardPolicy,allowedPolicy);
    await transferText(pair.client,pair.host,syncHost&&syncClient?'a'.repeat(32)+'-0000000000000001':'manual-compatible','한글 manual');
    pair.client.json({type:'clipboard.watch',sessionId:session.connection.sessionId,enabled:true});
    if(syncHost&&syncClient){await pair.host.take(v=>v.type==='clipboard.watch');pair.host.json({type:'clipboard.changed',sessionId:session.connection.sessionId,sequence:0xffffffff});assert.equal((await pair.client.take(v=>v.type==='clipboard.changed')).sequence,0xffffffff);}
    else{assert.equal((await pair.host.take(v=>v.type==='clipboard.error')).reason,'clipboard_invalid_message');assert.equal(pair.host.queue.some(v=>v.type==='clipboard.watch'),false);assert.equal(session.media.readyState,WebSocket.OPEN);}
  });
});

test('clipboard sync opt-in is stream-only and its optional host capability is strictly boolean and dependency-checked',async t=>{
  const f=await fixture(t),host=await f.host();
  for(const protocol of ['unknown',true,null,{},[]])assert.throws(()=>f.relay.connect({id:f.users.alice},tokenHash(f.tokens.alice),'pve-810','stream',undefined,undefined,protocol),/invalid_clipboard_protocol/);
  // RDP is not offered: refused as such before its clipboard field is read.
  assert.throws(()=>f.relay.connect({id:f.users.alice},tokenHash(f.tokens.alice),'pve-810','rdp',undefined,undefined,'clipboard-sync-v1'),error=>error.status===409&&error.code==='rdp_not_supported');
  for(const capabilities of [{...caps,clipboardSync:'true'},{...caps,clipboardSync:1},{...caps,clipboard:false}]){
    host.json({type:'host.capabilities',capabilities});assert.equal((await host.take(v=>v.type==='host.capabilities.ack')).accepted,false);
  }
  host.json({type:'host.capabilities',capabilities:{...caps,clipboardSync:false}});assert.equal((await host.take(v=>v.type==='host.capabilities.ack')).accepted,true);
});

test('loss of an advertised sync capability closes old automatic clipboard peers while leaving video usable',async t=>{
  const f=await ready(t,{sync:true});f.client.json({type:'clipboard.watch',sessionId:f.connection.sessionId,enabled:true});await f.host.take(v=>v.type==='clipboard.watch');
  f.mainHost.json({type:'host.capabilities',capabilities:{...caps,clipboardSync:false}});await f.mainHost.take(v=>v.type==='host.capabilities.ack');
  assert.equal((await f.client.take(v=>v.type==='clipboard.error')).reason,'clipboard_unavailable');assert.equal(f.media.readyState,WebSocket.OPEN);
});

test('each administrator permission allows only its exact text/file transfer direction',async t=>{
  const deny={textToRemote:false,textToLocal:false,filesToRemote:false,filesToLocal:false};
  for(const key of Object.keys(deny))await t.test(key,async t=>{
    const policy={...deny,[key]:true},f=await ready(t,{policy});
    assert.deepEqual(f.connection.clipboardPolicy,policy);assert.deepEqual(f.start.clipboardPolicy,policy);
    const sender=key.endsWith('ToRemote')?f.client:f.host,receiver=sender===f.client?f.host:f.client;
    if(sender===f.host){f.client.json({type:'clipboard.read',requestId:key});await f.host.take(v=>v.type==='clipboard.read');}
    if(key.startsWith('text'))await transferText(sender,receiver,key,'한글 ACL');
    else{sender.json({type:'clipboard.begin',requestId:key,format:'files',files:[{name:'allowed.txt',size:3}]});await receiver.take(v=>v.type==='clipboard.begin');await sendItem(sender,receiver,key,0,Buffer.from('ACL'));await commit(sender,receiver,key);}
    const requestId=key+'-blocked',format=key.startsWith('text')?'files':'text';
    if(sender===f.host){f.client.json({type:'clipboard.read',requestId});await f.host.take(v=>v.type==='clipboard.read');}
    sender.json(format==='text'?{type:'clipboard.begin',requestId,format,size:0}:{type:'clipboard.begin',requestId,format,files:[{name:'must-not-forward.txt',size:0}]});
    assert.equal((await receiver.take(v=>v.type==='clipboard.error')).reason,'clipboard_policy_denied');
    assert.equal(receiver.queue.some(v=>v.type==='clipboard.begin'&&v.requestId===requestId),false);
    assert.equal(f.media.readyState,WebSocket.OPEN,'format denial closes clipboard, not the video');
  });
});

test('direction denial is enforced before a read request or manifest reaches the other endpoint',async t=>{
  const deny={textToRemote:false,textToLocal:false,filesToRemote:false,filesToLocal:false};
  await t.test('remote read not authorized',async t=>{
    const f=await ready(t,{policy:{...deny,textToRemote:true}});
    f.client.json({type:'clipboard.read',requestId:'forbidden-read'});
    assert.equal((await f.host.take(v=>v.type==='clipboard.error')).reason,'clipboard_policy_denied');
    assert.equal(f.host.queue.some(v=>v.type==='clipboard.read'),false);
  });
  await t.test('local push not authorized',async t=>{
    const f=await ready(t,{policy:{...deny,textToLocal:true}});
    f.client.json({type:'clipboard.begin',requestId:'forbidden-push',format:'text',size:0});
    assert.equal((await f.host.take(v=>v.type==='clipboard.error')).reason,'clipboard_policy_denied');
    assert.equal(f.host.queue.some(v=>v.type==='clipboard.begin'),false);
  });
});

test('policy grants never expand an old session, while revocation immediately aborts a pending transfer and stream',async t=>{
  const textOnly={textToRemote:true,textToLocal:false,filesToRemote:false,filesToLocal:false};
  await t.test('grant requires a new session',async t=>{
    const f=await ready(t,{policy:textOnly});
    f.store.updateUser('alice',{clipboardPolicy:allowedPolicy},'fixture-admin');f.relay.clipboardPolicyChanged(f.users.alice);
    f.client.json({type:'clipboard.begin',requestId:'old-session-files',format:'files',files:[{name:'blocked.txt',size:0}]});
    assert.equal((await f.host.take(v=>v.type==='clipboard.error')).reason,'clipboard_policy_denied');
    assert.equal(f.media.readyState,WebSocket.OPEN);
    f.relay.stop(f.relay.sessions.get(f.connection.sessionId));await f.mainHost.take(v=>v.type==='session.stop');
    const fresh=await f.session(f.mainHost);assert.deepEqual(fresh.connection.clipboardPolicy,allowedPolicy);
  });
  await t.test('pending transfer revoked',async t=>{
    const f=await ready(t);
    f.client.json({type:'clipboard.begin',requestId:'pending-policy',format:'text',size:3});await f.host.take(v=>v.type==='clipboard.begin');
    f.store.updateUser('alice',{clipboardPolicy:{...allowedPolicy,textToRemote:false}},'fixture-admin');
    f.relay.clipboardPolicyChanged(f.users.alice);
    // Only the clipboard ends; the stream stays, like USB, printers and drives.
    assert.equal(f.relay.sessions.size,1);assert.equal(f.relay.clipboard.activeTransfers,0);
    assert.equal((await f.host.take(v=>v.type==='clipboard.error')).reason,'clipboard_policy_changed');
    await new Promise(resolve=>setTimeout(resolve,100));
    assert.equal(f.media.readyState,WebSocket.OPEN);
    // The periodic sweep does not end it either.
    f.relay.revalidate();
    assert.equal(f.relay.sessions.size,1);
  });
});

test('authorized clipboard watch forwards content-free DWORD changes with latest-only bounded coalescing and wrap support',async t=>{
  const f=await ready(t,{sync:true}),sessionId=f.connection.sessionId;
  f.client.json({type:'clipboard.watch',sessionId,enabled:true});assert.deepEqual(await f.host.take(v=>v.type==='clipboard.watch'),{type:'clipboard.watch',sessionId,enabled:true});
  f.host.json({type:'clipboard.changed',sessionId,sequence:0});assert.deepEqual(await f.client.take(v=>v.type==='clipboard.changed'),{type:'clipboard.changed',sessionId,sequence:0});
  for(const sequence of [0,1,2,0xffffffff,1])f.host.json({type:'clipboard.changed',sessionId,sequence});
  const coalesced=await f.client.take(v=>v.type==='clipboard.changed');assert.deepEqual(coalesced,{type:'clipboard.changed',sessionId,sequence:1});
  assert.equal(f.client.queue.some(v=>v.type==='clipboard.changed'),false);
  assert.equal(f.relay.clipboard.activeTransfers,0,'watch notifications never reserve file transfers');
  assert.equal(f.media.readyState,WebSocket.OPEN);
  f.host.json({type:'clipboard.changed',sessionId,sequence:2});
  f.client.json({type:'clipboard.watch',sessionId,enabled:false});await f.host.take(v=>v.type==='clipboard.watch'&&!v.enabled);
  await new Promise(resolve=>setTimeout(resolve,120));
  assert.equal(f.client.queue.some(v=>v.type==='clipboard.changed'),false,'disable discards a coalesced pending notification');
});

test('clipboard watch requires ToLocal permission while disabling remains permitted',async t=>{
  const policy={textToRemote:true,textToLocal:false,filesToRemote:false,filesToLocal:false},f=await ready(t,{policy,sync:true}),sessionId=f.connection.sessionId;
  f.client.json({type:'clipboard.watch',sessionId,enabled:false});assert.equal((await f.host.take(v=>v.type==='clipboard.watch')).enabled,false);
  f.client.json({type:'clipboard.watch',sessionId,enabled:true});
  assert.equal((await f.host.take(v=>v.type==='clipboard.error')).reason,'clipboard_policy_denied');
  assert.equal(f.host.queue.some(v=>v.type==='clipboard.watch'&&v.enabled),false);
  assert.equal(f.media.readyState,WebSocket.OPEN);
});

test('watch/changed reject wrong peer, session, extra content and malformed uint32 without ending video',async t=>{
  const cases=[
    ['host-watch','host',id=>({type:'clipboard.watch',sessionId:id,enabled:true})],
    ['client-changed','client',id=>({type:'clipboard.changed',sessionId:id,sequence:1})],
    ['foreign-session','client',()=>({type:'clipboard.watch',sessionId:'different-session',enabled:true})],
    ['watch-type','client',id=>({type:'clipboard.watch',sessionId:id,enabled:'true'})],
    ['extra-content','host',id=>({type:'clipboard.changed',sessionId:id,sequence:1,data:'must not forward'})],
    ...[-1,1.5,0x100000000,'1',null].map((sequence,index)=>['sequence-'+index,'host',id=>({type:'clipboard.changed',sessionId:id,sequence})]),
  ];
  for(const [name,role,message]of cases)await t.test(name,async t=>{
    const f=await ready(t,{sync:true});f.client.json({type:'clipboard.watch',sessionId:f.connection.sessionId,enabled:true});await f.host.take(v=>v.type==='clipboard.watch');
    f[role].json(message(f.connection.sessionId));
    const error=await f[role==='host'?'client':'host'].take(v=>v.type==='clipboard.error');assert.equal(error.reason,'clipboard_invalid_message');
    assert.equal(f.media.readyState,WebSocket.OPEN);
  });
});

test('clipboard notification timers are fenced by pair generation and permission revocation',async t=>{
  const f=await ready(t,{sync:true}),sessionId=f.connection.sessionId;
  f.client.json({type:'clipboard.watch',sessionId,enabled:true});await f.host.take(v=>v.type==='clipboard.watch');
  f.host.json({type:'clipboard.changed',sessionId,sequence:1});await f.client.take(v=>v.type==='clipboard.changed');
  f.host.json({type:'clipboard.changed',sessionId,sequence:2});
  const old=f.relay.sessions.get(sessionId).clipboardPair;f.relay.clipboard.closePair(old,'clipboard_transport_closed');
  const fresh=await f.pair({connection:f.connection,start:f.start});
  fresh.client.json({type:'clipboard.watch',sessionId,enabled:true});await fresh.host.take(v=>v.type==='clipboard.watch');
  await new Promise(resolve=>setTimeout(resolve,120));
  assert.equal(fresh.client.queue.some(v=>v.type==='clipboard.changed'),false);
  fresh.host.json({type:'clipboard.changed',sessionId,sequence:3});await fresh.client.take(v=>v.type==='clipboard.changed');
  fresh.host.json({type:'clipboard.changed',sessionId,sequence:4});
  f.store.updateUser('alice',{clipboardPolicy:{...allowedPolicy,textToLocal:false}},'fixture');f.relay.clipboardPolicyChanged(f.users.alice);
  // The stream stays (only the clipboard ends); the pair's timers are fenced all the same.
  assert.equal(f.relay.sessions.size,1);assert.ok(f.relay.sessions.get(sessionId).clipboardPair==null);assert.equal(old.changedTimer,null);assert.equal(old.pendingChanged,null);
  await new Promise(resolve=>setTimeout(resolve,120));
  assert.equal(fresh.client.queue.some(v=>v.type==='clipboard.changed'),false,'revocation cannot emit a pending changed event');
});

test('negotiated sync completes over 300 real transfers with bounded replay state and preserves current work against retired acknowledgements',async t=>{
  const f=await ready(t,{sync:true});
  for(let i=1;i<=320;i++){
    await transferText(f.client,f.host,syncRequest(i),'한글 transfer '+i);
    // Keep actual socket traffic below the unchanged 512 messages/second guard.
    await new Promise(resolve=>setTimeout(resolve,10));
  }
  const pair=f.relay.sessions.get(f.connection.sessionId).clipboardPair;
  assert.equal(pair.seen,null);assert.equal(pair.prefix,'a'.repeat(32));assert.equal(pair.highWater,320n);assert.equal(pair.terminal.size,256);
  assert.equal(pair.terminal.has(syncRequest(1)),false);assert.equal(pair.terminal.has(syncRequest(320)),true);
  const current=syncRequest(321);
  f.client.json({type:'clipboard.begin',requestId:current,format:'text',size:1});await f.host.take(v=>v.type==='clipboard.begin');
  f.host.json({type:'clipboard.chunk.ack',requestId:syncRequest(1),index:0,offset:1});
  f.host.json({type:'clipboard.item.ack',requestId:syncRequest(2),index:0});
  f.host.json({type:'clipboard.applied',requestId:syncRequest(3)});
  f.client.json({type:'clipboard.cancel',requestId:syncRequest(4)});
  f.client.json({type:'clipboard.error',requestId:syncRequest(5),reason:'clipboard_changed'});
  await sendItem(f.client,f.host,current,0,Buffer.from('x'));await commit(f.client,f.host,current);
  assert.equal(pair.transfer,null);assert.equal(f.relay.clipboard.activeTransfers,0);
  assert.equal(pair.highWater,321n);assert.equal(pair.terminal.size,256);assert.equal(f.media.readyState,WebSocket.OPEN);
  f.client.json({type:'clipboard.cancel',requestId:syncRequest(320)});
  assert.equal((await f.client.take(v=>v.type==='clipboard.complete')).requestId,syncRequest(320));
  const uncertain=syncRequest(322);
  f.client.json({type:'clipboard.begin',requestId:uncertain,format:'text',size:1});await f.host.take(v=>v.type==='clipboard.begin');await sendItem(f.client,f.host,uncertain,0,Buffer.from('u'));
  f.client.json({type:'clipboard.commit',requestId:uncertain});await f.host.take(v=>v.type==='clipboard.commit');
  f.client.json({type:'clipboard.cancel',requestId:uncertain});
  assert.equal((await f.client.take(v=>v.type==='clipboard.error')).reason,'clipboard_commit_uncertain');await f.host.take(v=>v.type==='clipboard.error');
  const next=syncRequest(323);f.client.json({type:'clipboard.begin',requestId:next,format:'text',size:1});await f.host.take(v=>v.type==='clipboard.begin');
  f.host.json({type:'clipboard.applied',requestId:uncertain});
  await sendItem(f.client,f.host,next,0,Buffer.from('n'));await commit(f.client,f.host,next);
  assert.equal(pair.terminal.get(uncertain).status,'uncertain');assert.equal(f.client.queue.some(v=>v.type==='clipboard.complete'&&v.requestId===uncertain),false);
  f.client.json({type:'clipboard.begin',requestId:syncRequest(1),format:'text',size:0});
  assert.equal((await f.host.take(v=>v.type==='clipboard.error')).reason,'clipboard_invalid_message','replay cannot reserve a new transfer');
  assert.equal(f.media.readyState,WebSocket.OPEN);
});

test('sync counters allow gaps, retain safe overwrite failures and restart with a fresh prefix on a new pair',async t=>{
  const f=await ready(t,{sync:true});
  await transferText(f.client,f.host,syncRequest(1),'first');
  const failed=syncRequest(3);f.client.json({type:'clipboard.begin',requestId:failed,format:'text',size:1});await f.host.take(v=>v.type==='clipboard.begin');
  f.host.json({type:'clipboard.error',requestId:failed,reason:'clipboard_changed'});
  assert.equal((await f.client.take(v=>v.type==='clipboard.error')).reason,'clipboard_changed');await f.host.take(v=>v.type==='clipboard.error');
  const next=syncRequest(7);await transferText(f.client,f.host,next,'after safe failure');
  const session=f.relay.sessions.get(f.connection.sessionId),old=session.clipboardPair;assert.equal(old.highWater,7n);
  f.relay.clipboard.closePair(old,'clipboard_transport_closed');
  const fresh=await f.pair({connection:f.connection,start:f.start});
  await transferText(fresh.client,fresh.host,syncRequest(1,'b'.repeat(32)),'new pair');
  assert.equal(session.clipboardPair.prefix,'b'.repeat(32));assert.equal(session.clipboardPair.highWater,1n);assert.equal(session.clipboardPair.terminal.size,1);
  assert.equal(f.relay.clipboard.activeTransfers,0);assert.equal(f.media.readyState,WebSocket.OPEN);
});

test('sync replay fence rejects future, foreign, malformed and nonpositive identities without forwarding content',async t=>{
  const cases=[
    ['future acknowledgement',()=>({type:'clipboard.applied',requestId:syncRequest(2)})],
    ['foreign prefix',()=>({type:'clipboard.cancel',requestId:syncRequest(1,'b'.repeat(32))})],
    ['zero counter',()=>({type:'clipboard.read',requestId:syncRequest(0)})],
    ['overflow counter',()=>({type:'clipboard.read',requestId:'a'.repeat(32)+'-8000000000000000'})],
    ['uppercase identity',()=>({type:'clipboard.read',requestId:syncRequest(2,'A'.repeat(32))})],
    ['legacy identity',()=>({type:'clipboard.read',requestId:'not-monotonic'})],
    ['extra retired fields',()=>({type:'clipboard.applied',requestId:syncRequest(1),data:'forbidden'})],
    ['replayed read',()=>({type:'clipboard.read',requestId:syncRequest(1)})],
  ];
  for(const [name,message]of cases)await t.test(name,async t=>{
    const f=await ready(t,{sync:true});await transferText(f.client,f.host,syncRequest(1),'complete');
    f.client.json(message());assert.equal((await f.host.take(v=>v.type==='clipboard.error')).reason,'clipboard_invalid_message');
    assert.equal(f.host.queue.some(v=>v.type==='clipboard.read'),false);assert.equal(f.media.readyState,WebSocket.OPEN);
  });
});

test('Actual paired clipboard sockets preserve Unicode text and multiple regular file payloads in both explicit directions',async t=>{
  const f=await ready(t);assert.equal(f.connection.clipboardWebSocketUrl.includes(f.tokens.alice),false);
  await transferText(f.client,f.host,'push-text','가'.repeat(24000)+'😀\r\n');
  const files=[{name:'empty.txt',bytes:Buffer.alloc(0)},{name:'테스트.bin',bytes:Buffer.alloc(65539,0x5a)}];
  f.client.json({type:'clipboard.read',requestId:'pull-files'});await f.host.take(v=>v.type==='clipboard.read');
  f.host.json({type:'clipboard.begin',requestId:'pull-files',format:'files',files:files.map(f=>({name:f.name,size:f.bytes.length}))});await f.client.take(v=>v.type==='clipboard.begin');
  for(let i=0;i<files.length;i++)await sendItem(f.host,f.client,'pull-files',i,files[i].bytes);await commit(f.host,f.client,'pull-files');
  f.client.json({type:'clipboard.read',requestId:'pull-text'});await f.host.take(v=>v.type==='clipboard.read');await transferText(f.host,f.client,'pull-text','원격 → 로컬');
  f.client.json({type:'clipboard.begin',requestId:'push-files',format:'files',files:[{name:'local.txt',size:3}]});await f.host.take(v=>v.type==='clipboard.begin');await sendItem(f.client,f.host,'push-files',0,Buffer.from('abc'));await commit(f.client,f.host,'push-files');
  assert.equal(f.relay.clipboard.activeTransfers,0);assert.equal(f.relay.clipboard.pairs.size,1);
});

test('Clipboard upgrades bind both credentials, exact controller, VM, role, origin and one socket per side',async t=>{
  const f=await fixture(t),host=await f.host(),session=await f.session(host);
  const clientUrl=f.url(session.connection.clipboardWebSocketUrl),hostUrl=f.url(session.start.clipboardUrl);
  await assert.rejects(socket(clientUrl,'Bearer '+f.tokens.bobby),e=>e.status===401);
  const anotherLogin=newToken();f.store.createSession(f.store.findUser('alice'),tokenHash(anotherLogin),Date.now(),60000);
  await assert.rejects(socket(clientUrl,'Bearer '+anotherLogin),e=>e.status===401);
  await assert.rejects(socket(clientUrl,'Host '+f.hostTokens[810]),e=>e.status===401);
  await assert.rejects(socket(hostUrl,'Host '+f.hostTokens[811]),e=>e.status===401);
  await assert.rejects(socket(hostUrl,'Bearer '+f.tokens.alice),e=>e.status===401);
  await assert.rejects(socket(clientUrl+'?token='+f.tokens.alice,'Bearer '+f.tokens.alice),e=>e.status===403);
  await assert.rejects(socket(clientUrl,'Bearer '+f.tokens.alice,{headers:{Authorization:'Bearer '+f.tokens.alice,Origin:'https://other.example'}}),e=>e.status===403);
  const pair=await f.pair(session);await assert.rejects(socket(clientUrl,'Bearer '+f.tokens.alice),e=>e.status===409);
  await assert.rejects(socket(hostUrl,'Host '+f.hostTokens[810]),e=>e.status===409);
  pair.host.json({type:'clipboard.read',requestId:'unsolicited'});assert.equal((await pair.client.take(v=>v.type==='clipboard.error')).reason,'clipboard_invalid_message');
  assert.equal(host.readyState,WebSocket.OPEN);assert.equal(session.media.readyState,WebSocket.OPEN);
});

test('Malformed paths, duplicates, bounds, hashes and ordering are rejected before unsafe completion reaches the receiver',async t=>{
  const cases=[
    ['path traversal',{type:'clipboard.begin',requestId:'bad',format:'files',files:[{name:'../secret',size:0}]}],
    ['DOS alias',{type:'clipboard.begin',requestId:'bad',format:'files',files:[{name:'COM¹.txt',size:0}]}],
    ['alternate stream',{type:'clipboard.begin',requestId:'bad',format:'files',files:[{name:'a:secret',size:0}]}],
    ['case collision',{type:'clipboard.begin',requestId:'bad',format:'files',files:[{name:'A.txt',size:0},{name:'a.txt',size:0}]}],
    ['file size',{type:'clipboard.begin',requestId:'bad',format:'files',files:[{name:'large.bin',size:268435457}]}],
    ['total size',{type:'clipboard.begin',requestId:'bad',format:'files',files:[{name:'a',size:268435456},{name:'b',size:268435456},{name:'c',size:1}]}],
    ['file count',{type:'clipboard.begin',requestId:'bad',format:'files',files:Array.from({length:129},(_,i)=>({name:String(i),size:0}))}],
    ['out of order',{type:'clipboard.chunk',requestId:'bad',index:0,offset:1,data:'YQ=='}],
    ['noncanonical base64',{type:'clipboard.chunk',requestId:'bad',index:0,offset:0,data:'YR=='}],
    ['early commit',{type:'clipboard.commit',requestId:'bad'}],
    ['wrong hash',{type:'clipboard.item.end',requestId:'bad',index:0,sha256:'0'.repeat(64)}],
  ];
  for(const [label,message]of cases)await t.test(label,async t=>{
    const f=await ready(t);
    if(message.type!=='clipboard.begin'){
      f.client.json({type:'clipboard.begin',requestId:'bad',format:'text',size:message.type==='clipboard.item.end'?0:1});await f.host.take(v=>v.type==='clipboard.begin');
    }
    f.client.json(message);const error=await f.host.take(v=>v.type==='clipboard.error');assert.match(error.reason,/^clipboard_/);
    assert.equal(f.host.queue.some(v=>v.type==='clipboard.commit'||v.type==='clipboard.item.end'),false);assert.equal(f.relay.clipboard.activeTransfers,0);
  });
});

test('A sender cannot outrun receiver ACKs, impersonate receiver completion or inject invalid UTF8/NUL/oversized text',async t=>{
  for(const kind of ['no-ack','fake-applied','invalid-utf8','nul','text-units'])await t.test(kind,async t=>{
    const f=await ready(t),bytes=kind==='invalid-utf8'?Buffer.from([0xc0,0xaf]):kind==='nul'?Buffer.from('a\0b'):kind==='text-units'?Buffer.from('a'.repeat(32769)):Buffer.from('ab');
    f.client.json({type:'clipboard.begin',requestId:'bad',format:'text',size:bytes.length});await f.host.take(v=>v.type==='clipboard.begin');
    if(kind==='fake-applied')f.client.json({type:'clipboard.applied',requestId:'bad'});
    else if(kind==='no-ack'){
      f.client.json({type:'clipboard.chunk',requestId:'bad',index:0,offset:0,data:'YQ=='});await f.host.take(v=>v.type==='clipboard.chunk');
      f.client.json({type:'clipboard.chunk',requestId:'bad',index:0,offset:1,data:'Yg=='});
    }else{
      for(let offset=0;offset<bytes.length;){const part=bytes.subarray(offset,offset+32768);f.client.json({type:'clipboard.chunk',requestId:'bad',index:0,offset,data:part.toString('base64')});await f.host.take(v=>v.type==='clipboard.chunk');offset+=part.length;f.host.json({type:'clipboard.chunk.ack',requestId:'bad',index:0,offset});await f.client.take(v=>v.type==='clipboard.chunk.ack');}
      f.client.json({type:'clipboard.item.end',requestId:'bad',index:0,sha256:digest(bytes)});
    }
    const error=await f.host.take(v=>v.type==='clipboard.error');assert.match(error.reason,/^clipboard_/);assert.equal(f.host.queue.some(v=>v.type==='clipboard.item.end'||v.type==='clipboard.commit'),false);
  });
});

test('Cancellation and bounded peer errors discard a transfer without automatic clipboard reads or ending video',async t=>{
  const f=await ready(t);f.client.json({type:'clipboard.read',requestId:'cancel'});await f.host.take(v=>v.type==='clipboard.read');
  f.client.json({type:'clipboard.cancel',requestId:'cancel'});await f.host.take(v=>v.type==='clipboard.cancel');await f.client.take(v=>v.type==='clipboard.cancel');assert.equal(f.relay.clipboard.activeTransfers,0);
  f.client.json({type:'clipboard.read',requestId:'empty'});await f.host.take(v=>v.type==='clipboard.read');
  f.host.json({type:'clipboard.error',requestId:'empty',reason:'clipboard_empty'});await f.client.take(v=>v.type==='clipboard.error');await f.host.take(v=>v.type==='clipboard.error');
  assert.equal(f.mainHost.readyState,WebSocket.OPEN);assert.equal(f.media.readyState,WebSocket.OPEN);
  await transferText(f.client,f.host,'after-cancel','works');
  f.client.json({type:'clipboard.read',requestId:'cancel'});assert.equal((await f.host.take(v=>v.type==='clipboard.error')).reason,'clipboard_invalid_message');
});

test('Cancellation after commit stays uncertain and a late applied ACK cannot contaminate the next transfer',async t=>{
  const f=await ready(t);f.client.json({type:'clipboard.begin',requestId:'committing',format:'text',size:1});await f.host.take(v=>v.type==='clipboard.begin');
  await sendItem(f.client,f.host,'committing',0,Buffer.from('a'));
  f.client.json({type:'clipboard.commit',requestId:'committing'});await f.host.take(v=>v.type==='clipboard.commit');
  f.client.json({type:'clipboard.cancel',requestId:'committing'});
  assert.equal((await f.client.take(v=>v.type==='clipboard.error')).reason,'clipboard_commit_uncertain');assert.equal((await f.host.take(v=>v.type==='clipboard.error')).reason,'clipboard_commit_uncertain');
  f.client.json({type:'clipboard.begin',requestId:'next',format:'text',size:1});await f.host.take(v=>v.type==='clipboard.begin');
  f.host.json({type:'clipboard.applied',requestId:'committing'});await sendItem(f.client,f.host,'next',0,Buffer.from('b'));await commit(f.client,f.host,'next');
  assert.equal(f.client.queue.some(v=>v.type==='clipboard.complete'&&v.requestId==='committing'),false);
  f.client.json({type:'clipboard.cancel',requestId:'next'});assert.equal((await f.client.take(v=>v.type==='clipboard.complete')).requestId,'next');
  assert.equal(f.relay.clipboard.activeTransfers,0);assert.equal(f.media.readyState,WebSocket.OPEN);
});

test('Revocation and stopped sessions close both clipboard sides; queued old bytes cannot contaminate a new controller',async t=>{
  const f=await ready(t);f.client.json({type:'clipboard.read',requestId:'pending'});await f.host.take(v=>v.type==='clipboard.read');
  const clientClosed=once(f.client,'close'),hostClosed=once(f.host,'close');f.store.revoke('alice','pve-810');await clientClosed;await hostClosed;
  assert.equal(f.relay.clipboard.activeTransfers,0);assert.equal(f.relay.clipboard.pairs.size,0);
  f.store.assign('alice','pve-810');await f.mainHost.take(v=>v.type==='session.stop');
  // A re-created assignment starts denied; the administrator grants it again.
  assert.deepEqual(f.store.clipboardPolicy(f.users.alice,'pve-810'),{textToRemote:false,textToLocal:false,filesToRemote:false,filesToLocal:false});
  f.store.updateAssignmentPolicy('alice','pve-810',{clipboardPolicy:allowedPolicy},'fixture');
  const next=await f.session(f.mainHost),pair=await f.pair(next);await transferText(pair.client,pair.host,'fresh-controller','fresh');
  const oldSession=f.relay.sessions.get(next.connection.sessionId);f.relay.stop(oldSession);
  if(pair.client.readyState===WebSocket.OPEN)pair.client.json({type:'clipboard.read',requestId:'late'});
  assert.equal(f.relay.clipboard.pairs.size,0);
  await f.mainHost.take(v=>v.type==='session.stop');
  const newest=await f.session(f.mainHost),newPair=await f.pair(newest);await transferText(newPair.client,newPair.host,'newest','clean');
  assert.equal(newPair.host.queue.some(v=>v.requestId==='late'||v.requestId==='pending'),false);
});

test('Transfer deadlines preserve the pair; pair, protocol and output failures close clipboard only',async t=>{
  await t.test('pair timeout',async t=>{
    const f=await fixture(t,{relayOptions:{clipboardOptions:{pairReadyMs:30}}}),host=await f.host(),session=await f.session(host);
    const client=await socket(f.url(session.connection.clipboardWebSocketUrl),'Bearer '+f.tokens.alice);assert.equal((await client.take(v=>v.type==='clipboard.error')).reason,'clipboard_pair_timeout');
  });
  await t.test('idle timeout',async t=>{
    const f=await ready(t,{relayOptions:{clipboardOptions:{idleMs:30}}});f.client.json({type:'clipboard.read',requestId:'idle'});await f.host.take(v=>v.type==='clipboard.read');
    const error=await f.client.take(v=>v.type==='clipboard.error');assert.equal(error.reason,'clipboard_timeout');assert.equal(error.requestId,'idle');
    assert.equal(f.media.readyState,WebSocket.OPEN);assert.equal(f.client.readyState,WebSocket.OPEN);assert.equal(f.host.readyState,WebSocket.OPEN);
    await f.host.take(v=>v.type==='clipboard.error');
    // Legacy stop-and-wait packets/cancel can cross the deadline on both sockets.
    f.host.json({type:'clipboard.begin',requestId:'idle',format:'text',size:1});
    f.host.json({type:'clipboard.chunk',requestId:'idle',index:0,offset:0,data:'YQ=='});
    f.host.json({type:'clipboard.item.end',requestId:'idle',index:0,sha256:digest(Buffer.from('a'))});
    f.client.json({type:'clipboard.cancel',requestId:'idle'});
    await transferText(f.client,f.host,'after-idle','fresh');
  });
  await t.test('absolute transfer timeout',async t=>{
    const f=await ready(t,{relayOptions:{clipboardOptions:{idleMs:1000,totalMs:30}}});f.client.json({type:'clipboard.read',requestId:'absolute'});await f.host.take(v=>v.type==='clipboard.read');
    assert.equal((await f.client.take(v=>v.type==='clipboard.error')).reason,'clipboard_timeout');assert.equal(f.relay.clipboard.activeTransfers,0);
    await f.host.take(v=>v.type==='clipboard.error');
    await transferText(f.client,f.host,'after-absolute','fresh');
  });
  await t.test('late commit stays uncertain without closing or corrupting the next copy',async t=>{
    const f=await ready(t,{relayOptions:{clipboardOptions:{idleMs:50}}});
    f.client.json({type:'clipboard.begin',requestId:'late-commit',format:'text',size:1});await f.host.take(v=>v.type==='clipboard.begin');
    await sendItem(f.client,f.host,'late-commit',0,Buffer.from('a'));
    f.client.json({type:'clipboard.commit',requestId:'late-commit'});await f.host.take(v=>v.type==='clipboard.commit');
    const error=await f.client.take(v=>v.type==='clipboard.error');assert.equal(error.reason,'clipboard_commit_uncertain');assert.equal(error.requestId,'late-commit');
    await f.host.take(v=>v.type==='clipboard.error');
    f.host.json({type:'clipboard.applied',requestId:'late-commit'});
    await transferText(f.client,f.host,'after-uncertain','fresh');
    assert.equal(f.client.readyState,WebSocket.OPEN);assert.equal(f.host.readyState,WebSocket.OPEN);
  });
  await t.test('binary invalid',async t=>{
    const f=await ready(t);f.client.send(Buffer.alloc(256*1024+1));await once(f.host,'close');assert.equal(f.media.readyState,WebSocket.OPEN);assert.equal(f.relay.clipboard.pairs.size,0);
  });
  await t.test('bounded output',async t=>{
    const f=await ready(t,{relayOptions:{clipboardOptions:{maxQueue:1024}}});f.client.json({type:'clipboard.begin',requestId:'queue',format:'files',files:[{name:'data.bin',size:32768}]});await f.host.take(v=>v.type==='clipboard.begin');
    f.client.json({type:'clipboard.chunk',requestId:'queue',index:0,offset:0,data:Buffer.alloc(32768).toString('base64')});
    assert.equal((await f.client.take(v=>v.type==='clipboard.error')).reason,'clipboard_backpressure');assert.equal(f.relay.clipboard.activeTransfers,0);
  });
});

test('Expired login and live capability loss close clipboard promptly without claiming clipboard access remains available',async t=>{
  await t.test('expired session',async t=>{
    const f=await ready(t),clientClosed=once(f.client,'close'),hostClosed=once(f.host,'close');
    f.store.db.prepare('UPDATE sessions SET expires_at=? WHERE token_hash=?').run(Date.now()+30,tokenHash(f.tokens.alice));
    await clientClosed;await hostClosed;assert.equal(f.relay.clipboard.activeTransfers,0);assert.equal(f.relay.sessions.size,0);
  });
  await t.test('clipboard capability revoked',async t=>{
    const f=await ready(t);f.mainHost.json({type:'host.capabilities',capabilities:{...caps,clipboard:false,clipboardSync:false}});await f.mainHost.take(v=>v.type==='host.capabilities.ack');
    assert.equal((await f.client.take(v=>v.type==='clipboard.error')).reason,'clipboard_unavailable');assert.equal(f.media.readyState,WebSocket.OPEN);
  });
});

test('Authorization expiring between media and clipboard revalidation retires the whole same session immediately',async t=>{
  const f=await ready(t),authenticate=f.store.authenticate.bind(f.store);
  const hash=tokenHash(f.tokens.alice);let observations=0;
  // The first media check is still authorized. Expiry is observed by the
  // clipboard check within the same synchronous sweep, as at a wall-clock edge.
  f.store.authenticate=(value,...args)=>value===hash&&++observations>=2?null:authenticate(value,...args);
  try {
    f.relay.revalidate();
    assert.ok(observations>=2);
    assert.equal(f.relay.clipboard.pairs.size,0);
    assert.equal(f.relay.sessions.size,0);
  } finally {f.store.authenticate=authenticate;}
});

test('A clipboard peer heartbeat loss and old pair callbacks cannot close a fresh clipboard pair or the main stream',async t=>{
  await t.test('independent heartbeat',async t=>{
    const f=await fixture(t,{relayOptions:{heartbeatMs:25}}),host=await f.host(),session=await f.session(host);
    const client=await socket(f.url(session.connection.clipboardWebSocketUrl),'Bearer '+f.tokens.alice),clipboardHost=await socket(f.url(session.start.clipboardUrl),'Host '+f.hostTokens[810],{autoPong:false});
    await client.take(v=>v.type==='clipboard.ready');await clipboardHost.take(v=>v.type==='clipboard.ready');
    assert.equal((await client.take(v=>v.type==='clipboard.error')).reason,'clipboard_transport_closed');assert.equal(host.readyState,WebSocket.OPEN);assert.equal(session.media.readyState,WebSocket.OPEN);
  });
  await t.test('same-session pair generation',async t=>{
    const f=await ready(t),session=f.relay.sessions.get(f.connection.sessionId),old=session.clipboardPair;
    f.client.json({type:'clipboard.read',requestId:'retired'});await f.host.take(v=>v.type==='clipboard.read');
    f.relay.clipboard.closePair(old,'clipboard_transport_closed');
    if(f.host.readyState===WebSocket.OPEN)f.host.json({type:'clipboard.begin',requestId:'retired',format:'text',size:0});
    const next=await f.pair({connection:f.connection,start:f.start});await transferText(next.client,next.host,'new-pair','clean');
    assert.equal(next.client.queue.some(v=>v.requestId==='retired'),false);assert.equal(f.media.readyState,WebSocket.OPEN);
  });
});

test('Oversized JSON and excessive control rate cannot grow clipboard queues or terminate the video channel',async t=>{
  await t.test('message bound',async t=>{
    const f=await ready(t);f.client.send(JSON.stringify({type:'clipboard.begin',requestId:'huge',format:'text',size:0,padding:'x'.repeat(256*1024)}));await once(f.host,'close');assert.equal(f.relay.clipboard.pairs.size,0);assert.equal(f.media.readyState,WebSocket.OPEN);
  });
  await t.test('rate bound',async t=>{
    const f=await ready(t);for(let i=0;i<257;i++){f.client.json({type:'clipboard.read',requestId:'r'+i});f.client.json({type:'clipboard.cancel',requestId:'r'+i});}
    const reasons=[];while(true){const value=await f.host.take(v=>v.type==='clipboard.error');reasons.push(value.reason);if(value.reason==='clipboard_rate_limit'||value.reason==='clipboard_invalid_message')break;}
    assert.equal(f.relay.clipboard.activeTransfers,0);assert.equal(f.media.readyState,WebSocket.OPEN);
  });
});

test('Clipboard channels use actual verified WSS and preserve authenticated text/file sequencing through TLS',async t=>{
  if(spawnSync('openssl',['version']).error?.code==='ENOENT'){t.skip('OpenSSL unavailable; Linux QA runs verified TLS.');return;}
  const directory=await mkdtemp(join(tmpdir(),'multitap-clipboard-tls-'));t.after(()=>rm(directory,{recursive:true}));
  assert.equal(spawnSync('openssl',['req','-x509','-newkey','rsa:2048','-nodes','-keyout',join(directory,'key.pem'),'-out',join(directory,'cert.pem'),'-days','1','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost,IP:127.0.0.1'],{encoding:'utf8'}).status,0);
  const tls={cert:await readFile(join(directory,'cert.pem')),key:await readFile(join(directory,'key.pem'))};
  const f=await fixture(t,{tls}),host=await f.host(),session=await f.session(host);
  await assert.rejects(socket(f.url(session.connection.clipboardWebSocketUrl),'Bearer '+f.tokens.alice,{rejectUnauthorized:true}));
  const pair=await f.pair(session);await transferText(pair.client,pair.host,'verified-text','TLS 한글');
  pair.client.json({type:'clipboard.read',requestId:'verified-file'});await pair.host.take(v=>v.type==='clipboard.read');
  pair.host.json({type:'clipboard.begin',requestId:'verified-file',format:'files',files:[{name:'tls.txt',size:3}]});await pair.client.take(v=>v.type==='clipboard.begin');await sendItem(pair.host,pair.client,'verified-file',0,Buffer.from('TLS'));await commit(pair.host,pair.client,'verified-file');
});

// A clipboard peer that sent anything since the previous heartbeat is alive and
// is not pinged: the Host's WinHTTP could write its pong inside a large chunk
// frame it is sending (relay.mjs checkHeartbeat).
test('Clipboard peers with traffic since the last heartbeat are not pinged; silent ones still are',()=>{
  const relay=new Relay({store:new Store(':memory:'),publicOrigin:'https://clipboard-fixture.example',authorizationMs:3600000,heartbeatMs:3600000});
  try{
    const clipboard=relay.clipboard,pings=[],closed=[];
    const peer=role=>({readyState:WebSocket.OPEN,ping:()=>pings.push(role)});
    clipboard.closePair=(pair,reason)=>closed.push(reason);
    const pair={client:peer('client'),host:peer('host'),clientAlive:true,hostAlive:true,hostTraffic:true};
    clipboard.pairs.add(pair);
    clipboard.heartbeat();
    assert.deepEqual(pings,['client']);assert.equal(pair.hostAlive,true);assert.equal(pair.hostTraffic,false);
    pair.clientAlive=true; // its pong
    clipboard.heartbeat();
    assert.deepEqual(pings,['client','client','host']);assert.deepEqual(closed,[]);
    clipboard.heartbeat(); // neither answered
    assert.deepEqual(closed,['clipboard_transport_closed']);
  }finally{relay.close();relay.store.close();}
});
const mib=1024*1024,gib=1024*mib;
const legacyLimits={chunkBytes:32768,maxFiles:128,maxFileBytes:256*mib,maxTotalBytes:512*mib,maxTextUnits:32768};
const extendedLimits={chunkBytes:32768,maxFiles:128,maxFileBytes:4*gib,maxTotalBytes:8*gib,maxTextUnits:mib,maxTextBytes:mib,limitsProfile:2};
async function limitsPair(t,extended){const f=await fixture(t),host=await f.host(),session=await f.session(host),pair=await f.pair(session,extended);return{...f,...session,...pair};}
const begin=(requestId,files)=>({type:'clipboard.begin',requestId,format:'files',files});

test('two peers that announce the extended limits are told 4 GiB per file, 8 GiB per transfer and 1 MiB of text, and may use them',async t=>{
  const p=await limitsPair(t,{client:true,host:true});
  assert.deepEqual(p.clientReady.limits,extendedLimits);assert.deepEqual(p.hostReady.limits,extendedLimits);
  // Text above the released 128 KiB bound (and 32768 units) now passes.
  await transferText(p.client,p.host,'big-text','가'.repeat(300000/3)+'a'.repeat(300000));
  // 3 GiB + 4 GiB is within 4 GiB per file and 8 GiB per transfer: forwarded, nothing buffered.
  p.client.json(begin('big-files',[{name:'a.bin',size:3*gib},{name:'b.bin',size:4*gib}]));
  assert.equal((await p.host.take(v=>v.type==='clipboard.begin')).files[1].size,4*gib);
  p.client.json({type:'clipboard.cancel',requestId:'big-files'});await p.host.take(v=>v.type==='clipboard.cancel');
});

test('the extended limits still refuse a file above 4 GiB, a transfer above 8 GiB, more than 128 files and text above 1 MiB',async t=>{
  for(const [name,message] of [['file',begin('x1',[{name:'a.bin',size:4*gib+1}])],['total',begin('x2',[{name:'a.bin',size:4*gib},{name:'b.bin',size:4*gib},{name:'c.bin',size:1}])],['count',begin('x3',Array.from({length:129},(_,i)=>({name:'f'+i+'.bin',size:1})))],['text',{type:'clipboard.begin',requestId:'x4',format:'text',size:mib+1}]])await t.test(name,async t=>{
    const p=await limitsPair(t,{client:true,host:true});p.client.json(message);
    assert.equal((await p.client.take(v=>v.type==='clipboard.error')).reason,'clipboard_limits_exceeded');
  });
});

test('a pair with one released peer stays on the released limits; the extended peer is told so and the released peer sees the exact released shape',async t=>{
  for(const extended of [{client:true,host:false},{client:false,host:true}])await t.test(JSON.stringify(extended),async t=>{
    const p=await limitsPair(t,extended);
    const released=extended.client?p.hostReady:p.clientReady,newer=extended.client?p.clientReady:p.hostReady;
    assert.deepEqual(released.limits,legacyLimits);
    assert.deepEqual(newer.limits,{...legacyLimits,maxTextBytes:128*1024,limitsProfile:2});
    p.client.json(begin('old-1',[{name:'a.bin',size:256*mib+1}]));
    assert.equal((await p.client.take(v=>v.type==='clipboard.error')).reason,'clipboard_limits_exceeded');
  });
});

test('two released peers keep the released limits and shape',async t=>{
  const p=await limitsPair(t,{});assert.deepEqual(p.hostReady.limits,legacyLimits);assert.deepEqual(p.clientReady.limits,legacyLimits);
  await transferText(p.client,p.host,'old-text','한글 manual');
  p.client.json({type:'clipboard.begin',requestId:'old-big',format:'text',size:128*1024+1});
  assert.equal((await p.client.take(v=>v.type==='clipboard.error')).reason,'clipboard_limits_exceeded');
});

test('newer failure reasons reach a released peer as the closest released reason and an extended peer unchanged',async t=>{
  for(const [reason,legacy] of [['clipboard_disk_full','clipboard_write_failed'],['clipboard_file_locked','clipboard_file_unavailable'],['clipboard_limits_exceeded','clipboard_transfer_failed']])await t.test(reason,async t=>{
    const p=await limitsPair(t,{client:false,host:true});
    p.client.json(begin('err-'+reason.length,[{name:'a.bin',size:10}]));await p.host.take(v=>v.type==='clipboard.begin');
    p.host.json({type:'clipboard.error',requestId:'err-'+reason.length,reason});
    assert.equal((await p.client.take(v=>v.type==='clipboard.error')).reason,legacy);
    assert.equal((await p.host.take(v=>v.type==='clipboard.error')).reason,reason);
  });
});
