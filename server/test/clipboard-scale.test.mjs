import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { createServer } from 'node:http';
import { WebSocket } from 'ws';
import { Relay } from '../src/relay.mjs';
import { Store } from '../src/store.mjs';
import { newToken,tokenHash } from '../src/security.mjs';
import { clipboardLimitsFormats,clipboardLimitsPull,clipboardLimitsScale } from '../src/clipboard.mjs';

// Scale revision (X-Multitap-Clipboard-Scale: 1, with limits, formats and pull): no size / total / top-level limit, 64 MiB of text,
// paths of 1024 characters on the wire (259 pasteable), an entry guard, no total duration, no rate-limit kill. Every other pair keeps
// the limits it had.
const MiB=1024*1024,GiB=1024*MiB;
const caps={service:true,interactiveSession:true,streaming:true,virtualDisplayReady:true,clipboard:true,clipboardSync:true};
const digest=bytes=>createHash('sha256').update(bytes).digest('hex');
const all={textToRemote:true,textToLocal:true,filesToRemote:true,filesToLocal:true};
async function socket(url,authorization,options={}){
  return new Promise((resolve,reject)=>{
    const ws=new WebSocket(url,{perMessageDeflate:false,...options,headers:{Authorization:authorization,...options.headers}}),queue=[],waiters=[];
    ws.on('message',(bytes,binary)=>{
      const value=binary?bytes:JSON.parse(bytes.toString());const index=waiters.findIndex(w=>w.test(value));
      if(index<0)queue.push(value);else{const w=waiters.splice(index,1)[0];clearTimeout(w.timer);w.resolve(value);}
      // The relay ending the pair is reported at once to anything still waiting for another message.
      if(value.type==='clipboard.error'&&value.requestId===undefined)for(const w of waiters.splice(0)){clearTimeout(w.timer);w.reject(new Error('pair ended: '+value.reason));}
    });
    ws.on('error',()=>{});ws.once('error',reject);
    ws.json=value=>ws.send(JSON.stringify(value));ws.queue=queue;
    ws.take=(test=()=>true,ms=30000)=>{
      const index=queue.findIndex(test);if(index>=0)return Promise.resolve(queue.splice(index,1)[0]);
      return new Promise((resolve,reject)=>{const w={test,resolve,reject,timer:setTimeout(()=>reject(new Error('clipboard_message_timeout')),ms)};waiters.push(w);});
    };
    ws.once('open',()=>resolve(ws));
  });
}
const formatsHeaders={'X-Multitap-Clipboard-Limits':'2','X-Multitap-Clipboard-Formats':'2'};
const flags=side=>side==='scale'?{...formatsHeaders,'X-Multitap-Clipboard-Pull':'1','X-Multitap-Clipboard-Scale':'1'}:side==='pull'?{...formatsHeaders,'X-Multitap-Clipboard-Pull':'1'}:side==='scale-only'?{'X-Multitap-Clipboard-Scale':'1'}:side==='scale-formats'?{...formatsHeaders,'X-Multitap-Clipboard-Scale':'1'}:undefined;
async function ready(t,{client='scale',host='scale',policy=all,relayOptions={}}={}){
  const store=new Store(':memory:'),tokens={},users={};
  for(const name of ['alice']){users[name]=store.addUser({name,passwordHash:'fixture-only'});tokens[name]=newToken();store.createSession(store.findUser(name),tokenHash(tokens[name]),Date.now(),3600000);}
  store.syncProxmox([811].map(vmid=>({vmid,node:'fixture',name:'Clipboard '+vmid,status:'running'})),'fixture');
  store.assign('alice','pve-811');store.updateAssignmentPolicy('alice','pve-811',{clipboardPolicy:policy},'fixture');
  const code=newToken(),uuid='12345678-1234-1234-1234-000000000811',hostToken=newToken();
  store.issueEnrollment('pve-811',uuid,tokenHash(code),'fixture');store.consumeEnrollment(tokenHash(code),uuid,{hostname:'Fixture host',version:'0.2.3',tokenHash:tokenHash(hostToken)});
  const relay=new Relay({store,publicOrigin:'https://clipboard-fixture.example',authorizationMs:10,heartbeatMs:1000,...relayOptions});
  const server=createServer();relay.attach(server);
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  const base='ws://127.0.0.1:'+server.address().port;relay.publicOrigin='https://127.0.0.1:'+server.address().port;
  t.after(async()=>{await new Promise(resolve=>server.close(resolve));store.close();});
  const url=path=>base+new URL(path).pathname;
  const main=await socket(base+'/api/v1/hosts/channel','Host '+hostToken);
  main.json({type:'host.capabilities',capabilities:caps});await main.take(v=>v.type==='host.capabilities.ack');
  const connection=relay.connect({id:users.alice},tokenHash(tokens.alice),'pve-811','stream');
  const media=await socket(url(connection.webSocketUrl),'Bearer '+tokens.alice);
  const start=await main.take(v=>v.type==='session.start');
  const c=await socket(url(connection.clipboardWebSocketUrl),'Bearer '+tokens.alice,{headers:flags(client)});
  const h=await socket(url(start.clipboardUrl),'Host '+hostToken,{headers:flags(host)});
  const clientReady=await c.take(v=>v.type==='clipboard.ready'),hostReady=await h.take(v=>v.type==='clipboard.ready');
  return{relay,store,client:c,host:h,clientReady,hostReady,media,connection,main};
}
const entry=(path,size)=>size===undefined?{path,kind:'dir',attrs:0,mtime:1761500000000}:{path,kind:'file',size,attrs:0x20,mtime:1761500000123};
function offerMessage(requestId,entries){const files=entries.filter(e=>e.kind==='file');return{type:'clipboard.offer',requestId,format:'tree',entries:entries.length,files:files.length,bytes:files.reduce((s,e)=>s+e.size,0),tops:entries.filter(e=>!e.path.includes('/')).length,skipped:0};}
async function sendEntries(source,requestId,entries){for(let at=0;at<entries.length;at+=200)source.json({type:'clipboard.entries',requestId,start:at,entries:entries.slice(at,at+200)});}
async function offer(p,requestId,entries){
  p.client.json(offerMessage(requestId,entries));
  assert.equal((await p.host.take(v=>v.type==='clipboard.offer')).requestId,requestId);
  await sendEntries(p.client,requestId,entries);
  for(let at=0;at<entries.length;at+=200)await p.host.take(v=>v.type==='clipboard.entries');
  p.host.json({type:'clipboard.applied',requestId});
  await p.client.take(v=>v.type==='clipboard.complete');await p.host.take(v=>v.type==='clipboard.complete');
}
async function reasonOf(ws,ms){return(await ws.take(v=>v.type==='clipboard.error',ms)).reason;}

test('the scale revision is advertised only to a paste-pull pair whose peers both announced it',async t=>{
  const both=await ready(t);
  assert.deepEqual(both.clientReady.limits,clipboardLimitsScale);assert.deepEqual(both.hostReady.limits,clipboardLimitsScale);
  assert.deepEqual(clipboardLimitsScale,{...clipboardLimitsPull,scaleProfile:1,maxFiles:200000,maxFileBytes:2**53-1,maxTotalBytes:2**53-1,maxTextUnits:64*MiB,maxTextBytes:64*MiB,maxEntries:200000,maxDepth:512,maxPathChars:1024,maxRichBytes:64*MiB});
  // One side without it, or the header without the pull profile: the pair keeps exactly the limits it had.
  for(const [client,host] of [['scale','pull'],['pull','scale']])await t.test(client+'/'+host,async t=>{
    const p=await ready(t,{client,host});assert.deepEqual(p.clientReady.limits,clipboardLimitsPull);assert.deepEqual(p.hostReady.limits,clipboardLimitsPull);
  });
  const noPull=await ready(t,{client:'scale-formats',host:'scale-formats'});
  assert.deepEqual(noPull.clientReady.limits,clipboardLimitsFormats);
  const released=await ready(t,{client:'scale-only',host:'scale'});assert.equal(released.clientReady.limits.scaleProfile,undefined);
});

test('an offer has no size, total or top-level limit: 301 top-level items with a file above 4 GiB, pulled at an offset beyond 4 GiB',async t=>{
  const p=await ready(t);
  const entries=[entry('big.bin',5*GiB+7),...Array.from({length:300},(_,n)=>entry('f'+n+'.txt',1))];
  await offer(p,'o1',entries);
  // The host pulls the last 7 bytes of the 5 GiB file.
  const tail=Buffer.from('TAIL123');
  p.host.json({type:'clipboard.pull',requestId:'pullh-1',offerId:'o1',index:0,offset:5*GiB});
  const request=await p.client.take(v=>v.type==='clipboard.pull');assert.equal(request.offset,5*GiB);
  p.client.json({type:'clipboard.chunk',requestId:'pullh-1',index:0,offset:5*GiB,data:tail.toString('base64')});
  const data=await p.host.take(v=>v.type==='clipboard.chunk');assert.equal(data.offset,5*GiB);
  p.host.json({type:'clipboard.chunk.ack',requestId:'pullh-1',index:0,offset:5*GiB+7});await p.client.take(v=>v.type==='clipboard.chunk.ack');
  p.client.json({type:'clipboard.item.end',requestId:'pullh-1',index:0,sha256:digest(tail)});
  assert.equal((await p.host.take(v=>v.type==='clipboard.item.end')).sha256,digest(tail));
  // 2^53 - 1 bytes in one file is still a number the peers can hold; one more byte is not an integer the pair can express.
  const q=await ready(t);await offer(q,'o2',[entry('huge.bin',2**53-1)]);
  const r=await ready(t);r.client.json(offerMessage('o3',[entry('x.bin',2**53)]));assert.match(await reasonOf(r.host),/^clipboard_(invalid_message|limits_exceeded)$/);
  // A pair with the pull profile alone still stops at 4 GiB per file.
  const old=await ready(t,{client:'pull',host:'pull'});old.client.json(offerMessage('o4',[entry('big.bin',5*GiB)]));
  await old.host.take(v=>v.type==='clipboard.offer');old.client.json({type:'clipboard.entries',requestId:'o4',start:0,entries:[entry('big.bin',5*GiB)]});
  assert.equal(await reasonOf(old.host),'clipboard_limits_exceeded');
});

test('the entry list is bounded by a memory guard of 200,000, not by 10,000; a pair without the revision keeps 10,000',async t=>{
  const p=await ready(t);
  const many=Array.from({length:20000},(_,n)=>entry('d/f'+n+'.txt',1));many.unshift(entry('d'));
  await offer(p,'o1',many);
  const guard=await ready(t);guard.client.json({...offerMessage('o2',[entry('a',1)]),entries:200001});
  assert.equal(await reasonOf(guard.host),'clipboard_limits_exceeded');
  const old=await ready(t,{client:'pull',host:'pull'});old.client.json({...offerMessage('o3',[entry('a',1)]),entries:10001});
  assert.equal(await reasonOf(old.host),'clipboard_limits_exceeded');
});

test('paths: 259 characters pass, 260 are refused with the precise reason; older pairs keep 160 and their reason',async t=>{
  const p=await ready(t);
  const ok=['a'.repeat(100)+'/'+'b'.repeat(100)+'/'+'c'.repeat(57)];assert.equal(ok[0].length,259);
  await offer(p,'o1',[entry('a'.repeat(100)),entry('a'.repeat(100)+'/'+'b'.repeat(100)),entry(ok[0],1)]);
  const longer=await ready(t);const entries=[entry('a'.repeat(100)),entry('a'.repeat(100)+'/'+'b'.repeat(100)),entry('a'.repeat(100)+'/'+'b'.repeat(100)+'/'+'c'.repeat(58),1)];
  longer.client.json(offerMessage('o2',entries));await longer.host.take(v=>v.type==='clipboard.offer');
  await sendEntries(longer.client,'o2',entries);
  assert.equal(await reasonOf(longer.host),'clipboard_path_too_long');
  assert.equal(await reasonOf(longer.client),'clipboard_path_too_long');
  // Components up to 255 characters (240 elsewhere).
  const wide=await ready(t);await offer(wide,'o3',[entry('x'.repeat(255),1)]);
  const narrow=await ready(t,{client:'pull',host:'pull'});narrow.client.json(offerMessage('o4',[entry('x'.repeat(241),1)]));
  await narrow.host.take(v=>v.type==='clipboard.offer');narrow.client.json({type:'clipboard.entries',requestId:'o4',start:0,entries:[entry('x'.repeat(241),1)]});
  assert.equal(await reasonOf(narrow.host),'clipboard_invalid_message');
  // A pair without the revision: 161 characters are a limit violation, as before.
  const old=await ready(t,{client:'pull',host:'pull'});const deep=[entry('p'.repeat(80)),entry('p'.repeat(80)+'/'+'q'.repeat(80)+'/r',1)];
  old.client.json(offerMessage('o5',deep));await old.host.take(v=>v.type==='clipboard.offer');old.client.json({type:'clipboard.entries',requestId:'o5',start:0,entries:deep});
  assert.equal(await reasonOf(old.host),'clipboard_limits_exceeded');
});

test('pushed content stays small in a scale pair: a bigger tree must be offered',async t=>{
  const p=await ready(t);
  p.client.json({type:'clipboard.begin',requestId:'t1',format:'tree',entries:2,files:1,bytes:16*MiB+1,tops:1,skipped:0});
  assert.equal(await reasonOf(p.host),'clipboard_limits_exceeded');
  const q=await ready(t);
  q.client.json({type:'clipboard.begin',requestId:'t2',format:'tree',entries:257,files:256,bytes:256,tops:1,skipped:0});
  assert.equal(await reasonOf(q.host),'clipboard_limits_exceeded');
  const r=await ready(t);
  r.client.json({type:'clipboard.begin',requestId:'t3',format:'files',files:Array.from({length:257},(_,n)=>({name:'f'+n,size:1}))});
  assert.equal(await reasonOf(r.host),'clipboard_limits_exceeded');
  const ok=await ready(t);
  ok.client.json({type:'clipboard.begin',requestId:'t4',format:'tree',entries:3,files:2,bytes:16*MiB,tops:1,skipped:0});
  assert.equal((await ok.host.take(v=>v.type==='clipboard.begin')).requestId,'t4');
});

test('specific-limit reasons are carried by a scale pair and downgraded for any other pair',async t=>{
  for(const reason of ['clipboard_path_too_long','clipboard_text_too_large','clipboard_entry_count_limit','clipboard_depth_limit','clipboard_image_too_large']){
    const p=await ready(t);p.host.json({type:'clipboard.error',requestId:'unused',reason});
    // No transfer is active: an error is a protocol violation, whatever its reason, but the reason must be a known one.
    assert.ok(await reasonOf(p.client));
    const q=await ready(t);q.client.json({type:'clipboard.read',requestId:'r1'});await q.host.take(v=>v.type==='clipboard.read');
    q.host.json({type:'clipboard.error',requestId:'r1',reason});
    assert.equal(await reasonOf(q.client),reason);assert.equal(await reasonOf(q.host),reason);
    const old=await ready(t,{client:'pull',host:'pull'});old.client.json({type:'clipboard.read',requestId:'r1'});await old.host.take(v=>v.type==='clipboard.read');
    old.host.json({type:'clipboard.error',requestId:'r1',reason});
    assert.equal(await reasonOf(old.client),'clipboard_limits_exceeded');
    // A peer that has no extended profile gets the closest released reason.
    const released=await ready(t,{client:null,host:'scale'});released.client.json({type:'clipboard.read',requestId:'r1'});await released.host.take(v=>v.type==='clipboard.read');
    released.host.json({type:'clipboard.error',requestId:'r1',reason});
    assert.equal(await reasonOf(released.client),'clipboard_transfer_failed');
  }
  const unknown=await ready(t);unknown.client.json({type:'clipboard.read',requestId:'r1'});await unknown.host.take(v=>v.type==='clipboard.read');
  unknown.host.json({type:'clipboard.error',requestId:'r1',reason:'clipboard_something_else'});
  assert.equal(await reasonOf(unknown.client),'clipboard_invalid_message');
});

// The sender pushes `size` bytes of ASCII text in chunks, at most 20 unacknowledged, the receiver acknowledges each batch.
async function pushText(p,requestId,size,{format='text'}={}){
  p.client.json({type:'clipboard.begin',requestId,format,size});
  await p.host.take(v=>v.type==='clipboard.begin'&&v.requestId===requestId);
  const chunk=Buffer.alloc(45*1024,'x'),hash=createHash('sha256');let offset=0;
  while(offset<size){
    const batch=[];
    for(let n=0;n<20&&offset<size;n++){const part=chunk.subarray(0,Math.min(chunk.length,size-offset));hash.update(part);p.client.json({type:'clipboard.chunk',requestId,index:0,offset,data:part.toString('base64')});offset+=part.length;batch.push(offset);}
    for(let n=0;n<batch.length;n++)await p.host.take(v=>v.type==='clipboard.chunk'&&v.requestId===requestId);
    p.host.json({type:'clipboard.chunk.ack',requestId,index:0,offset:batch.at(-1)});
    await p.client.take(v=>v.type==='clipboard.chunk.ack'&&v.requestId===requestId);
  }
  const sha=hash.digest('hex');
  p.client.json({type:'clipboard.item.end',requestId,index:0,sha256:sha});
  assert.equal((await p.host.take(v=>v.type==='clipboard.item.end')).sha256,sha);
  p.host.json({type:'clipboard.item.ack',requestId,index:0});await p.client.take(v=>v.type==='clipboard.item.ack');
  p.client.json({type:'clipboard.commit',requestId});await p.host.take(v=>v.type==='clipboard.commit');
  p.host.json({type:'clipboard.applied',requestId});
  await p.client.take(v=>v.type==='clipboard.complete');await p.host.take(v=>v.type==='clipboard.complete');
}

test('64 MiB of text crosses a scale pair at full speed: the rate limit slows the pair instead of ending it',{timeout:180000},async t=>{
  const p=await ready(t);
  const started=Date.now();
  await pushText(p,'big-text',64*MiB);
  // The pair is still usable afterwards, and a second 1 MiB text works.
  await pushText(p,'next-text',MiB);
  assert.ok(Date.now()-started<150000);
});

test('above the rate limit a scale pair is slowed (its sockets paused), an older windowed pair is ended',{timeout:120000},async t=>{
  // 256 KiB per second: a 1.5 MiB text needs several seconds but arrives intact.
  const p=await ready(t,{relayOptions:{clipboardOptions:{fastBytes:256*1024}}});
  const started=Date.now();await pushText(p,'slow-text',1536*1024);
  assert.ok(Date.now()-started>=2000,'the pair was not slowed: '+(Date.now()-started)+' ms');
  const old=await ready(t,{client:'pull',host:'pull',relayOptions:{clipboardOptions:{fastBytes:256*1024}}});
  old.client.json({type:'clipboard.begin',requestId:'old-text',format:'text',size:1024*1024});await old.host.take(v=>v.type==='clipboard.begin');
  for(let n=0;n<8;n++)old.client.json({type:'clipboard.chunk',requestId:'old-text',index:0,offset:n*45*1024,data:Buffer.alloc(45*1024,'x').toString('base64')});
  assert.equal(await reasonOf(old.host,5000),'clipboard_rate_limit');
});

test('text above 64 MiB is refused up front; older pairs keep 1 MiB',async t=>{
  const p=await ready(t);p.client.json({type:'clipboard.begin',requestId:'t1',format:'text',size:64*MiB+1});
  assert.equal(await reasonOf(p.host),'clipboard_limits_exceeded');
  const old=await ready(t,{client:'pull',host:'pull'});old.client.json({type:'clipboard.begin',requestId:'t2',format:'text',size:MiB+1});
  assert.equal(await reasonOf(old.host),'clipboard_limits_exceeded');
  // 64 MiB of rich text parts (text and HTML) are announced; 64 MiB + 1 of HTML is not.
  const rich=await ready(t);rich.client.json({type:'clipboard.begin',requestId:'t3',format:'richtext',items:[{kind:'text',size:10},{kind:'html',size:64*MiB}]});
  assert.equal((await rich.host.take(v=>v.type==='clipboard.begin')).requestId,'t3');
  const bad=await ready(t);bad.client.json({type:'clipboard.begin',requestId:'t4',format:'richtext',items:[{kind:'text',size:10},{kind:'html',size:64*MiB+1}]});
  assert.equal(await reasonOf(bad.host),'clipboard_limits_exceeded');
});

test('a scale pair has no total duration, only an idle limit; older pairs keep their total',async t=>{
  // The total of a pair without the revision: 30 ms here, so it ends although it is not idle.
  const older=await ready(t,{client:'pull',host:'pull',relayOptions:{clipboardOptions:{idleMs:5000,totalMs:30,extendedTotalMs:30}}});
  older.client.json({type:'clipboard.begin',requestId:'t0',format:'text',size:10});
  assert.equal(await reasonOf(older.host),'clipboard_timeout');
  // A scale pair keeps going while bytes move, however long the transfer has lasted.
  const p=await ready(t,{relayOptions:{clipboardOptions:{idleMs:5000,totalMs:30,extendedTotalMs:30}}});
  p.client.json({type:'clipboard.begin',requestId:'t1',format:'text',size:100});await p.host.take(v=>v.type==='clipboard.begin');
  for(let n=0;n<8;n++){
    await new Promise(resolve=>setTimeout(resolve,25));p.relay.revalidate();
    p.client.json({type:'clipboard.chunk',requestId:'t1',index:0,offset:n*5,data:Buffer.from('hello').toString('base64')});
    await p.host.take(v=>v.type==='clipboard.chunk');
    p.host.json({type:'clipboard.chunk.ack',requestId:'t1',index:0,offset:n*5+5});await p.client.take(v=>v.type==='clipboard.chunk.ack');
  }
  assert.equal(p.client.readyState,WebSocket.OPEN);
  // Idle still ends a scale transfer.
  const idle=await ready(t,{relayOptions:{clipboardOptions:{idleMs:30,totalMs:900000,extendedTotalMs:900000}}});
  idle.client.json({type:'clipboard.begin',requestId:'t2',format:'text',size:10});await idle.host.take(v=>v.type==='clipboard.begin');
  await new Promise(resolve=>setTimeout(resolve,80));idle.relay.revalidate();
  assert.equal(await reasonOf(idle.host),'clipboard_timeout');
});
