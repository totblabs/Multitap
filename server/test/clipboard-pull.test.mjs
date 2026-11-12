import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { createServer } from 'node:http';
import { WebSocket } from 'ws';
import { Relay } from '../src/relay.mjs';
import { Store } from '../src/store.mjs';
import { newToken,tokenHash } from '../src/security.mjs';
import { clipboardLimitsFormats,clipboardLimitsPull,clipboardLimitsV2,clipboardLimits } from '../src/clipboard.mjs';

// Paste-pull profile: a copy announces only a manifest (clipboard.offer + clipboard.entries); the receiving peer
// pulls one item at a time (clipboard.pull) when the user pastes.
const caps={service:true,interactiveSession:true,streaming:true,virtualDisplayReady:true,clipboard:true,clipboardSync:true};
const digest=bytes=>createHash('sha256').update(bytes).digest('hex');
const all={textToRemote:true,textToLocal:true,filesToRemote:true,filesToLocal:true};
const deny={textToRemote:false,textToLocal:false,filesToRemote:false,filesToLocal:false};
async function socket(url,authorization,options={}){
  return new Promise((resolve,reject)=>{
    const ws=new WebSocket(url,{perMessageDeflate:false,...options,headers:{Authorization:authorization,...options.headers}}),queue=[],waiters=[];
    ws.on('message',(bytes,binary)=>{
      const value=binary?bytes:JSON.parse(bytes.toString());const index=waiters.findIndex(w=>w.test(value));
      if(index<0)queue.push(value);else{const w=waiters.splice(index,1)[0];clearTimeout(w.timer);w.resolve(value);}
    });
    ws.on('error',()=>{});ws.once('error',reject);
    ws.json=value=>ws.send(JSON.stringify(value));ws.queue=queue;
    ws.take=(test=()=>true)=>{
      const index=queue.findIndex(test);if(index>=0)return Promise.resolve(queue.splice(index,1)[0]);
      return new Promise((resolve,reject)=>{const w={test,resolve,timer:setTimeout(()=>reject(new Error('clipboard_message_timeout')),1500)};waiters.push(w);});
    };
    ws.once('open',()=>resolve(ws));
  });
}
const formatsHeaders={'X-Multitap-Clipboard-Limits':'2','X-Multitap-Clipboard-Formats':'2'};
const flags=side=>side==='pull'?{...formatsHeaders,'X-Multitap-Clipboard-Pull':'1'}:side==='formats'?formatsHeaders:side==='pullh-only'?{'X-Multitap-Clipboard-Pull':'1'}:undefined;
async function ready(t,{client='pull',host='pull',policy=all}={}){
  const store=new Store(':memory:'),tokens={},users={};
  for(const name of ['alice']){users[name]=store.addUser({name,passwordHash:'fixture-only'});tokens[name]=newToken();store.createSession(store.findUser(name),tokenHash(tokens[name]),Date.now(),3600000);}
  store.syncProxmox([810].map(vmid=>({vmid,node:'fixture',name:'Clipboard '+vmid,status:'running'})),'fixture');
  store.assign('alice','pve-810');store.updateAssignmentPolicy('alice','pve-810',{clipboardPolicy:policy},'fixture');
  const code=newToken(),uuid='12345678-1234-1234-1234-000000000810',hostToken=newToken();
  store.issueEnrollment('pve-810',uuid,tokenHash(code),'fixture');store.consumeEnrollment(tokenHash(code),uuid,{hostname:'Fixture host',version:'0.2.3',tokenHash:tokenHash(hostToken)});
  const relay=new Relay({store,publicOrigin:'https://clipboard-fixture.example',authorizationMs:10,heartbeatMs:1000});
  const server=createServer();relay.attach(server);
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  const base='ws://127.0.0.1:'+server.address().port;relay.publicOrigin='https://127.0.0.1:'+server.address().port;
  t.after(async()=>{await new Promise(resolve=>server.close(resolve));store.close();});
  const url=path=>base+new URL(path).pathname;
  const main=await socket(base+'/api/v1/hosts/channel','Host '+hostToken);
  main.json({type:'host.capabilities',capabilities:caps});await main.take(v=>v.type==='host.capabilities.ack');
  const connection=relay.connect({id:users.alice},tokenHash(tokens.alice),'pve-810','stream');
  const media=await socket(url(connection.webSocketUrl),'Bearer '+tokens.alice);
  const start=await main.take(v=>v.type==='session.start');
  const c=await socket(url(connection.clipboardWebSocketUrl),'Bearer '+tokens.alice,{headers:flags(client)});
  const h=await socket(url(start.clipboardUrl),'Host '+hostToken,{headers:flags(host)});
  const clientReady=await c.take(v=>v.type==='clipboard.ready'),hostReady=await h.take(v=>v.type==='clipboard.ready');
  return{relay,store,client:c,host:h,clientReady,hostReady,media,connection,main};
}
const entry=(path,size)=>size===undefined?{path,kind:'dir',attrs:0,mtime:1761500000000}:{path,kind:'file',size,attrs:0x20,mtime:1761500000123};
function offerMessage(requestId,entries){const files=entries.filter(e=>e.kind==='file');return{type:'clipboard.offer',requestId,format:'tree',entries:entries.length,files:files.length,bytes:files.reduce((s,e)=>s+e.size,0),tops:entries.filter(e=>!e.path.includes('/')).length,skipped:0};}
// The client announces `entries` and the host publishes it: the offer is then registered at the relay.
async function offer(p,requestId,entries,{from='client'}={}){
  const [source,target]=from==='client'?[p.client,p.host]:[p.host,p.client];
  if(from==='host'){p.client.json({type:'clipboard.read',requestId,format:'files'});await p.host.take(v=>v.type==='clipboard.read');}
  source.json(offerMessage(requestId,entries));
  assert.equal((await target.take(v=>v.type==='clipboard.offer')).requestId,requestId);
  source.json({type:'clipboard.entries',requestId,start:0,entries});
  await target.take(v=>v.type==='clipboard.entries');
  target.json({type:'clipboard.applied',requestId});
  await source.take(v=>v.type==='clipboard.complete');await target.take(v=>v.type==='clipboard.complete');
}
const chunkMsg=(requestId,index,offset,bytes)=>({type:'clipboard.chunk',requestId,index,offset,data:bytes.toString('base64')});
async function closed(ws){if(ws.readyState===WebSocket.CLOSED)return;await new Promise(resolve=>ws.once('close',resolve));}
async function reasonOf(ws){return(await ws.take(v=>v.type==='clipboard.error')).reason;}
// The host pulls item `index` of the client's offer from `offset` and the client answers with `bytes`.
async function pullFrom(p,id,offerId,index,bytes,offset=0){
  p.host.json({type:'clipboard.pull',requestId:id,offerId,index,offset});
  const request=await p.client.take(v=>v.type==='clipboard.pull');assert.equal(request.requestId,id);assert.equal(request.offset,offset);
  const body=bytes.subarray(offset),size=clipboardLimitsPull.chunkBytes,parts=[];
  for(let at=0;at<body.length;at+=size)parts.push([offset+at,body.subarray(at,at+size)]);
  for(const [at,part] of parts)p.client.json(chunkMsg(id,index,at,part));
  for(const [at,part] of parts){const m=await p.host.take(v=>v.type==='clipboard.chunk');assert.equal(m.offset,at);assert.deepEqual(Buffer.from(m.data,'base64'),part);}
  if(parts.length){const [at,part]=parts.at(-1);p.host.json({type:'clipboard.chunk.ack',requestId:id,index,offset:at+part.length});await p.client.take(v=>v.type==='clipboard.chunk.ack');}
  p.client.json({type:'clipboard.item.end',requestId:id,index,sha256:digest(body)});
  assert.equal((await p.host.take(v=>v.type==='clipboard.item.end')).sha256,digest(body));
}

test('the pull profile is advertised only when both peers announced it with the formats profile',async t=>{
  const both=await ready(t);
  assert.deepEqual(both.clientReady.limits,clipboardLimitsPull);assert.deepEqual(both.hostReady.limits,clipboardLimitsPull);
  assert.equal(clipboardLimitsPull.pullProfile,1);assert.equal(clipboardLimitsPull.maxOffers,2);
  for(const [client,host] of [['pull','formats'],['formats','pull']])await t.test(client+'/'+host,async t=>{
    const p=await ready(t,{client,host});assert.deepEqual(p.clientReady.limits,clipboardLimitsFormats);assert.deepEqual(p.hostReady.limits,clipboardLimitsFormats);
  });
  // The pull header without the formats headers changes nothing (a released relay ignores it the same way).
  const released=await ready(t,{client:'pullh-only',host:'pull'});
  assert.deepEqual(released.clientReady.limits,clipboardLimits);assert.deepEqual(released.hostReady.limits,{...clipboardLimitsV2,limitsProfile:2,chunkBytes:32768,maxFileBytes:256*1024*1024,maxTotalBytes:512*1024*1024,maxTextUnits:32768,maxTextBytes:128*1024});
});

test('a pair without the pull profile refuses offers and pulls as invalid',async t=>{
  for(const message of [offerMessage('o1',[entry('a.txt',3)]),{type:'clipboard.pull',requestId:'pullh-1',offerId:'o1',index:0,offset:0}]){
    const p=await ready(t,{client:'formats',host:'pull'});p.client.json(message);
    assert.equal(await reasonOf(p.host),'clipboard_invalid_message');
  }
});

test('an offer is a manifest only, applied by the receiver, and needs the files permission of its direction',async t=>{
  const p=await ready(t);
  const entries=[entry('Folder'),entry('Folder/a.txt',5),entry('b.bin',70000)];
  await offer(p,'o1',entries);
  // The offer left no transfer behind: the next copy can start at once.
  await offer(p,'o2',[entry('c.txt',1)]);
  // Host to client through a read, answered by an offer.
  await offer(p,'o3',[entry('d.txt',2)],{from:'host'});
  for(const [policy,from] of [[{...deny,textToRemote:true,filesToLocal:true},'client'],[{...deny,textToLocal:true,filesToRemote:true},'host']]){
    const q=await ready(t,{policy});
    const [source,target]=from==='client'?[q.client,q.host]:[q.host,q.client];
    if(from==='host'){q.client.json({type:'clipboard.read',requestId:'r1'});await q.host.take(v=>v.type==='clipboard.read');}
    source.json(offerMessage('o9',[entry('a.txt',1)]));
    assert.equal(await reasonOf(target),'clipboard_policy_denied');
  }
});

test('offer entries carry a write time; other manifests are validated like folder trees',async t=>{
  for(const bad of [undefined,-1,1.5,'1',253402300800000]){
    const p=await ready(t);const entries=[{...entry('a.txt',1),mtime:bad}];if(bad===undefined)delete entries[0].mtime;
    p.client.json(offerMessage('o1',entries));await p.host.take(v=>v.type==='clipboard.offer');
    p.client.json({type:'clipboard.entries',requestId:'o1',start:0,entries});
    assert.equal(await reasonOf(p.host),'clipboard_invalid_message','mtime '+bad);
  }
  // A pushed tree still has no write time (the released shape).
  const pushed=await ready(t,{client:'formats',host:'formats'});
  pushed.client.json({type:'clipboard.begin',requestId:'t1',format:'tree',entries:1,files:1,bytes:1,tops:1,skipped:0});await pushed.host.take(v=>v.type==='clipboard.begin');
  pushed.client.json({type:'clipboard.entries',requestId:'t1',start:0,entries:[entry('a.txt',1)]});
  assert.equal(await reasonOf(pushed.host),'clipboard_invalid_message');
});

test('offer manifests are validated like folder trees',async t=>{
  for(const message of [
    {...offerMessage('o1',[entry('a.txt',1)]),format:'files'},
    {...offerMessage('o1',[entry('a.txt',1)]),extra:1},
    {...offerMessage('o1',[entry('a.txt',1)]),entries:0},
  ]){
    const p=await ready(t);p.client.json(message);
    assert.match(await reasonOf(p.host),/^clipboard_(invalid_message|limits_exceeded)$/);
  }
  const p=await ready(t);const bad=[entry('top',1),entry('a/../b.txt',1)];p.client.json(offerMessage('o2',bad));await p.host.take(v=>v.type==='clipboard.offer');
  p.client.json({type:'clipboard.entries',requestId:'o2',start:0,entries:bad});
  assert.equal(await reasonOf(p.host),'clipboard_invalid_message');
  // Data cannot be sent without a transfer or a pull: a chunk under an unknown id is out of place.
  const q=await ready(t);await offer(q,'o3',[entry('a.txt',3)]);
  q.client.json(chunkMsg('zz',0,0,Buffer.from('abc')));
  assert.equal(await reasonOf(q.host),'clipboard_invalid_message');
});

test('a pull streams one item windowed under its own id and is verified by the relay',async t=>{
  const p=await ready(t);
  const big=Buffer.alloc(300000,9),small=Buffer.from('hello');
  await offer(p,'o1',[entry('Folder'),entry('Folder/big.bin',big.length),entry('small.txt',small.length),entry('empty.txt',0)]);
  // File ordinals count files only: 0 = big, 1 = small, 2 = empty.
  p.host.json({type:'clipboard.pull',requestId:'pullh-1',offerId:'o1',index:0,offset:0});
  const request=await p.client.take(v=>v.type==='clipboard.pull');assert.deepEqual(request,{type:'clipboard.pull',requestId:'pullh-1',offerId:'o1',index:0,offset:0});
  const size=clipboardLimitsPull.chunkBytes,parts=[];
  for(let at=0;at<big.length;at+=size)parts.push([at,big.subarray(at,at+size)]);
  // The window holds 1 MiB: all seven chunks fit and are acknowledged cumulatively.
  for(const [at,part] of parts)p.client.json(chunkMsg('pullh-1',0,at,part));
  for(const [at,part] of parts){const m=await p.host.take(v=>v.type==='clipboard.chunk');assert.equal(m.requestId,'pullh-1');assert.equal(m.offset,at);assert.deepEqual(Buffer.from(m.data,'base64'),part);}
  const [lastAt,last]=parts.at(-1);
  p.host.json({type:'clipboard.chunk.ack',requestId:'pullh-1',index:0,offset:lastAt+last.length});
  assert.equal((await p.client.take(v=>v.type==='clipboard.chunk.ack')).offset,lastAt+last.length);
  p.client.json({type:'clipboard.item.end',requestId:'pullh-1',index:0,sha256:digest(big)});
  assert.equal((await p.host.take(v=>v.type==='clipboard.item.end')).sha256,digest(big));
  // The final ack races the end of the pull: it is dropped, not a violation.
  p.host.json({type:'clipboard.item.ack',requestId:'pullh-1',index:0});
  // The next pull is a new id, with no other transfer in the way, and a small item and an empty one work.
  await pullFrom(p,'pullh-2','o1',1,small);
  await pullFrom(p,'pullh-3','o1',2,Buffer.alloc(0));
  assert.equal(p.media.readyState,WebSocket.OPEN);assert.equal(p.client.readyState,WebSocket.OPEN);assert.equal(p.host.readyState,WebSocket.OPEN);
});

test('a pull may start at an offset; the hash covers the delivered range',async t=>{
  const p=await ready(t);const data=Buffer.from('0123456789abcdef');
  await offer(p,'o1',[entry('a.bin',data.length)]);
  await pullFrom(p,'pullh-1','o1',0,data,6);
  // A chunk that does not start at the pull's offset is out of order.
  const q=await ready(t);await offer(q,'o1',[entry('a.bin',data.length)]);
  q.host.json({type:'clipboard.pull',requestId:'pullh-1',offerId:'o1',index:0,offset:6});await q.client.take(v=>v.type==='clipboard.pull');
  q.client.json(chunkMsg('pullh-1',0,0,data.subarray(0,4)));
  assert.equal(await reasonOf(q.host),'clipboard_out_of_order');
});

test('the requester pulls from the peer that offered; ids rise; ranges are checked',async t=>{
  const entries=[entry('a.bin',10)];
  for(const bad of [
    {requestId:'pullh-0'},{requestId:'pullh-01'},{requestId:'pullx'},{requestId:'pullc-1'},{index:1},{index:-1},{offset:11},{offset:1.5},{offerId:'bad id'},{extra:1},
  ]){
    const p=await ready(t);await offer(p,'o1',entries);
    p.host.json({type:'clipboard.pull',requestId:'pullh-1',offerId:'o1',index:0,offset:0,...bad});
    assert.equal(await reasonOf(p.host),'clipboard_invalid_message',JSON.stringify(bad));
  }
  // The offering side cannot pull its own offer.
  const own=await ready(t);await offer(own,'o1',entries);
  own.client.json({type:'clipboard.pull',requestId:'pullc-1',offerId:'o1',index:0,offset:0});
  assert.equal(await reasonOf(own.host),'clipboard_invalid_message');
  // The two requesters count separately: the client's pullc-1 and the Host's pullh-1 may both be the first.
  const both=await ready(t);await offer(both,'o1',entries);await offer(both,'h1',[entry('b.bin',1)],{from:'host'});
  await pullFrom(both,'pullh-1','o1',0,Buffer.alloc(10,1));
  both.client.json({type:'clipboard.pull',requestId:'pullc-1',offerId:'h1',index:0,offset:0});await both.host.take(v=>v.type==='clipboard.pull');
  // A repeated or lower id is a violation.
  const rising=await ready(t);await offer(rising,'o1',entries);
  await pullFrom(rising,'pullh-5','o1',0,Buffer.alloc(10,1));
  rising.host.json({type:'clipboard.pull',requestId:'pullh-5',offerId:'o1',index:0,offset:0});
  assert.equal(await reasonOf(rising.host),'clipboard_invalid_message');
});

test('an unknown or retired offer, and a busy pair, are answered to the requester alone',async t=>{
  const p=await ready(t);
  p.host.json({type:'clipboard.pull',requestId:'pullh-1',offerId:'nothing',index:0,offset:0});
  const unknown=await p.host.take(v=>v.type==='clipboard.error');assert.deepEqual(unknown,{type:'clipboard.error',requestId:'pullh-1',reason:'clipboard_offer_expired'});
  // The latest two offers per source stay; the third retires the first.
  await offer(p,'o1',[entry('a',1)]);await offer(p,'o2',[entry('b',1)]);await offer(p,'o3',[entry('c',1)]);
  p.host.json({type:'clipboard.pull',requestId:'pullh-2',offerId:'o1',index:0,offset:0});
  assert.equal((await p.host.take(v=>v.type==='clipboard.error')).reason,'clipboard_offer_expired');
  assert.equal(p.client.queue.some(v=>v.type==='clipboard.pull'),false);
  // Only one pull is active; a second request is refused as busy and the first is unharmed.
  p.host.json({type:'clipboard.pull',requestId:'pullh-3',offerId:'o2',index:0,offset:0});await p.client.take(v=>v.type==='clipboard.pull');
  p.host.json({type:'clipboard.pull',requestId:'pullh-4',offerId:'o3',index:0,offset:0});
  assert.deepEqual(await p.host.take(v=>v.type==='clipboard.error'),{type:'clipboard.error',requestId:'pullh-4',reason:'clipboard_busy'});
  p.client.json(chunkMsg('pullh-3',0,0,Buffer.from('x')));await p.host.take(v=>v.type==='clipboard.chunk');
  p.client.json({type:'clipboard.item.end',requestId:'pullh-3',index:0,sha256:digest(Buffer.from('x'))});await p.host.take(v=>v.type==='clipboard.item.end');
  await pullFrom(p,'pullh-5','o3',0,Buffer.from('y'));
  assert.equal(p.host.readyState,WebSocket.OPEN);
});

test('a host offer is pulled by the client',async t=>{
  const p=await ready(t);const data=Buffer.from('from the VM');
  await offer(p,'h1',[entry('vm.txt',data.length)],{from:'host'});
  p.client.json({type:'clipboard.pull',requestId:'pullc-1',offerId:'h1',index:0,offset:0});
  await p.host.take(v=>v.type==='clipboard.pull');
  p.host.json(chunkMsg('pullc-1',0,0,data));assert.deepEqual(Buffer.from((await p.client.take(v=>v.type==='clipboard.chunk')).data,'base64'),data);
  p.client.json({type:'clipboard.chunk.ack',requestId:'pullc-1',index:0,offset:data.length});await p.host.take(v=>v.type==='clipboard.chunk.ack');
  p.host.json({type:'clipboard.item.end',requestId:'pullc-1',index:0,sha256:digest(data)});await p.client.take(v=>v.type==='clipboard.item.end');
});

test('window, size and hash violations of a pull close the channel; failures and cancels end only the pull',async t=>{
  const data=Buffer.alloc(100,3);
  const violation=async(label,run,reason)=>{
    const p=await ready(t);await offer(p,'o1',[entry('a.bin',data.length)]);
    p.host.json({type:'clipboard.pull',requestId:'pullh-1',offerId:'o1',index:0,offset:0});await p.client.take(v=>v.type==='clipboard.pull');
    run(p);assert.equal(await reasonOf(p.host),reason,label);
  };
  await violation('bad hash',p=>{p.client.json(chunkMsg('pullh-1',0,0,data));p.client.json({type:'clipboard.item.end',requestId:'pullh-1',index:0,sha256:'0'.repeat(64)});},'clipboard_hash_mismatch');
  await violation('short item',p=>p.client.json({type:'clipboard.item.end',requestId:'pullh-1',index:0,sha256:digest(Buffer.alloc(0))}),'clipboard_out_of_order');
  await violation('too long',p=>p.client.json(chunkMsg('pullh-1',0,0,Buffer.alloc(101))),'clipboard_limits_exceeded');
  await violation('wrong item',p=>p.client.json(chunkMsg('pullh-1',1,0,data)),'clipboard_out_of_order');
  await violation('unlisted ack',p=>p.host.json({type:'clipboard.chunk.ack',requestId:'pullh-1',index:0,offset:50}),'clipboard_out_of_order');
  await violation('requester sends data',p=>p.host.json(chunkMsg('pullh-1',0,0,data)),'clipboard_out_of_order');
  await violation('unknown reason',p=>p.client.json({type:'clipboard.error',requestId:'pullh-1',reason:'whatever'}),'clipboard_invalid_message');
  // The offering side reports a changed file: the requester hears it and the pair lives on.
  const p=await ready(t);await offer(p,'o1',[entry('a.bin',data.length)]);
  p.host.json({type:'clipboard.pull',requestId:'pullh-1',offerId:'o1',index:0,offset:0});await p.client.take(v=>v.type==='clipboard.pull');
  p.client.json({type:'clipboard.error',requestId:'pullh-1',reason:'clipboard_selected_file_changed'});
  assert.deepEqual(await p.host.take(v=>v.type==='clipboard.error'),{type:'clipboard.error',requestId:'pullh-1',reason:'clipboard_selected_file_changed'});
  // The changed-file reason exists only for pull ids; a push transfer cannot use it.
  p.client.json({type:'clipboard.read',requestId:'r1'}); // (not allowed from a client in this fixture before ready; ignored below)
  await pullFrom(p,'pullh-2','o1',0,data);
  // The requester cancels mid-pull: the source hears it; what was in flight is dropped.
  const c=await ready(t);await offer(c,'o1',[entry('a.bin',data.length)]);
  c.host.json({type:'clipboard.pull',requestId:'pullh-1',offerId:'o1',index:0,offset:0});await c.client.take(v=>v.type==='clipboard.pull');
  c.client.json(chunkMsg('pullh-1',0,0,data.subarray(0,10)));await c.host.take(v=>v.type==='clipboard.chunk');
  c.host.json({type:'clipboard.cancel',requestId:'pullh-1'});
  c.client.json(chunkMsg('pullh-1',0,10,data.subarray(10,20)));c.client.json({type:'clipboard.item.end',requestId:'pullh-1',index:0,sha256:digest(data)});
  assert.deepEqual(await c.client.take(v=>v.type==='clipboard.cancel'),{type:'clipboard.cancel',requestId:'pullh-1'});
  await pullFrom(c,'pullh-2','o1',0,data);
  assert.equal(c.host.readyState,WebSocket.OPEN);
});

test('pulls are checked against the current policy and end with the channel when it shrinks',async t=>{
  const p=await ready(t);await offer(p,'o1',[entry('a.bin',3)]);
  p.store.updateAssignmentPolicy('alice','pve-810',{clipboardPolicy:{...all,filesToRemote:false}},'fixture');
  p.host.json({type:'clipboard.pull',requestId:'pullh-1',offerId:'o1',index:0,offset:0});
  await Promise.race([closed(p.host),reasonOf(p.host)]);
  assert.equal(p.client.queue.some(v=>v.type==='clipboard.pull'),false);
});

test('an idle pull is cancelled for both peers and the pair continues',async t=>{
  const p=await ready(t);p.relay.clipboard.pullIdleMs=50;await offer(p,'o1',[entry('a.bin',3)]);
  p.host.json({type:'clipboard.pull',requestId:'pullh-1',offerId:'o1',index:0,offset:0});await p.client.take(v=>v.type==='clipboard.pull');
  await new Promise(resolve=>setTimeout(resolve,80));p.relay.clipboard.revalidate();
  assert.equal((await p.client.take(v=>v.type==='clipboard.cancel')).requestId,'pullh-1');await p.host.take(v=>v.type==='clipboard.cancel');
  p.relay.clipboard.pullIdleMs=60000;await pullFrom(p,'pullh-2','o1',0,Buffer.from('abc'));
});

test('a cancelled or failed offer is retired before it can be pulled',async t=>{
  const p=await ready(t);
  p.client.json(offerMessage('o1',[entry('a.bin',3)]));await p.host.take(v=>v.type==='clipboard.offer');
  p.client.json({type:'clipboard.entries',requestId:'o1',start:0,entries:[entry('a.bin',3)]});await p.host.take(v=>v.type==='clipboard.entries');
  p.host.json({type:'clipboard.error',requestId:'o1',reason:'clipboard_write_failed'});
  await p.client.take(v=>v.type==='clipboard.error');
  p.host.json({type:'clipboard.pull',requestId:'pullh-1',offerId:'o1',index:0,offset:0});
  assert.equal((await p.host.take(v=>v.type==='clipboard.error'&&v.requestId==='pullh-1')).reason,'clipboard_offer_expired');
});
