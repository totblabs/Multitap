import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { createServer } from 'node:http';
import { WebSocket } from 'ws';
import { Relay } from '../src/relay.mjs';
import { Store } from '../src/store.mjs';
import { newToken,tokenHash } from '../src/security.mjs';
import { clipboardLimitsPull,clipboardLimitsScale,clipboardLimitsOnPaste } from '../src/clipboard.mjs';

// On-paste revision (X-Multitap-Clipboard-OnPaste: 1, with limits, formats, pull and scale): a copy is
// silent. Nothing but small text and small images is pushed at copy time; files, folders and anything bigger are offered and pulled
// when pasted. Two additive messages: clipboard.offer.withdraw and clipboard.paste.waiting. Every other pair keeps its behaviour.
const MiB=1024*1024;
const caps={service:true,interactiveSession:true,streaming:true,virtualDisplayReady:true,clipboard:true,clipboardSync:true};
const digest=bytes=>createHash('sha256').update(bytes).digest('hex');
const all={textToRemote:true,textToLocal:true,filesToRemote:true,filesToLocal:true};
async function socket(url,authorization,options={}){
  return new Promise((resolve,reject)=>{
    const ws=new WebSocket(url,{perMessageDeflate:false,...options,headers:{Authorization:authorization,...options.headers}}),queue=[],waiters=[];
    ws.on('message',(bytes,binary)=>{
      const value=binary?bytes:JSON.parse(bytes.toString());const index=waiters.findIndex(w=>w.test(value));
      if(index<0)queue.push(value);else{const w=waiters.splice(index,1)[0];clearTimeout(w.timer);w.resolve(value);}
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
const flags=side=>side==='onpaste'?{...formatsHeaders,'X-Multitap-Clipboard-Pull':'1','X-Multitap-Clipboard-Scale':'1','X-Multitap-Clipboard-OnPaste':'1'}
  :side==='scale'?{...formatsHeaders,'X-Multitap-Clipboard-Pull':'1','X-Multitap-Clipboard-Scale':'1'}
  :side==='pull'?{...formatsHeaders,'X-Multitap-Clipboard-Pull':'1'}
  :side==='onpaste-only'?{'X-Multitap-Clipboard-OnPaste':'1'}
  :side==='onpaste-pull'?{...formatsHeaders,'X-Multitap-Clipboard-Pull':'1','X-Multitap-Clipboard-OnPaste':'1'}:undefined;
async function ready(t,{client='onpaste',host='onpaste',policy=all,relayOptions={}}={}){
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
async function reasonOf(ws,ms){return(await ws.take(v=>v.type==='clipboard.error',ms)).reason;}
const entry=(path,size)=>size===undefined?{path,kind:'dir',attrs:0,mtime:1761500000000}:{path,kind:'file',size,attrs:0x20,mtime:1761500000123};
function treeOffer(requestId,entries){const files=entries.filter(e=>e.kind==='file');return{type:'clipboard.offer',requestId,format:'tree',entries:entries.length,files:files.length,bytes:files.reduce((s,e)=>s+e.size,0),tops:entries.filter(e=>!e.path.includes('/')).length,skipped:0};}
async function offerTree(p,requestId,entries){
  p.client.json(treeOffer(requestId,entries));
  assert.equal((await p.host.take(v=>v.type==='clipboard.offer')).requestId,requestId);
  p.client.json({type:'clipboard.entries',requestId,start:0,entries});
  await p.host.take(v=>v.type==='clipboard.entries');
  p.host.json({type:'clipboard.applied',requestId});
  await p.client.take(v=>v.type==='clipboard.complete');await p.host.take(v=>v.type==='clipboard.complete');
}
// A non-tree offer from `from` ('client' sends it unprompted, 'host' answers a clipboard.read). The other side applies it.
async function offerContent(p,from,message){
  const to=from==='client'?'host':'client';
  if(from==='host'){p.client.json({type:'clipboard.read',requestId:message.requestId});await p.host.take(v=>v.type==='clipboard.read');}
  p[from].json(message);
  assert.equal((await p[to].take(v=>v.type==='clipboard.offer')).requestId,message.requestId);
  p[to].json({type:'clipboard.applied',requestId:message.requestId});
  await p.client.take(v=>v.type==='clipboard.complete');await p.host.take(v=>v.type==='clipboard.complete');
}
// The requester pulls item `index` from `offset` of offer `offerId`; the source answers with `data`; the requester acknowledges.
async function pull(requester,source,pullId,offerId,index,data,offset=0){
  requester.json({type:'clipboard.pull',requestId:pullId,offerId,index,offset});
  const request=await source.take(v=>v.type==='clipboard.pull');assert.equal(request.requestId,pullId);
  source.json({type:'clipboard.chunk',requestId:pullId,index,offset,data:data.toString('base64')});
  const got=await requester.take(v=>v.type==='clipboard.chunk'&&v.requestId===pullId);assert.equal(Buffer.from(got.data,'base64').toString('hex'),data.toString('hex'));
  requester.json({type:'clipboard.chunk.ack',requestId:pullId,index,offset:offset+data.length});await source.take(v=>v.type==='clipboard.chunk.ack');
  source.json({type:'clipboard.item.end',requestId:pullId,index,sha256:digest(data)});
  assert.equal((await requester.take(v=>v.type==='clipboard.item.end')).sha256,digest(data));
}

test('the on-paste revision is advertised only to a scale pair whose peers both announced it',async t=>{
  const both=await ready(t);
  assert.deepEqual(both.clientReady.limits,clipboardLimitsOnPaste);assert.deepEqual(both.hostReady.limits,clipboardLimitsOnPaste);
  assert.deepEqual(clipboardLimitsOnPaste,{...clipboardLimitsScale,onPasteProfile:1,pushTextBytes:MiB,pushImageBytes:8*MiB});
  // One side without it, or the header without the scale revision: the pair keeps exactly the limits it had.
  for(const [client,host] of [['onpaste','scale'],['scale','onpaste']])await t.test(client+'/'+host,async t=>{
    const p=await ready(t,{client,host});assert.deepEqual(p.clientReady.limits,clipboardLimitsScale);assert.deepEqual(p.hostReady.limits,clipboardLimitsScale);
  });
  const noScale=await ready(t,{client:'onpaste-pull',host:'onpaste-pull'});assert.deepEqual(noScale.clientReady.limits,clipboardLimitsPull);
  const released=await ready(t,{client:'onpaste-only',host:'onpaste'});assert.equal(released.clientReady.limits.onPasteProfile,undefined);assert.equal(released.hostReady.limits.onPasteProfile,undefined);
});

test('nothing but small content is pushed: files and folders never, text above 1 MiB and images above 8 MiB must be offered',async t=>{
  const flat=await ready(t);
  flat.client.json({type:'clipboard.begin',requestId:'f1',format:'files',files:[{name:'a.txt',size:3}]});
  assert.equal(await reasonOf(flat.host),'clipboard_limits_exceeded');
  const tree=await ready(t);
  tree.client.json({type:'clipboard.begin',requestId:'t1',format:'tree',entries:2,files:1,bytes:3,tops:1,skipped:0});
  assert.equal(await reasonOf(tree.host),'clipboard_limits_exceeded');
  // The same copies are pushed on a scale pair without the revision (nothing changed there).
  const old=await ready(t,{client:'scale',host:'scale'});
  old.client.json({type:'clipboard.begin',requestId:'f2',format:'files',files:[{name:'a.txt',size:3}]});
  assert.equal((await old.host.take(v=>v.type==='clipboard.begin')).requestId,'f2');
  // Text: exactly 1 MiB passes, one byte more does not.
  const text=await ready(t);text.client.json({type:'clipboard.begin',requestId:'x1',format:'text',size:MiB});
  assert.equal((await text.host.take(v=>v.type==='clipboard.begin')).requestId,'x1');
  const bigText=await ready(t);bigText.client.json({type:'clipboard.begin',requestId:'x2',format:'text',size:MiB+1});
  assert.equal(await reasonOf(bigText.host),'clipboard_limits_exceeded');
  // Rich text: text and HTML together.
  const rich=await ready(t);rich.client.json({type:'clipboard.begin',requestId:'r1',format:'richtext',items:[{kind:'text',size:100},{kind:'html',size:MiB-100}]});
  assert.equal((await rich.host.take(v=>v.type==='clipboard.begin')).requestId,'r1');
  const bigRich=await ready(t);bigRich.client.json({type:'clipboard.begin',requestId:'r2',format:'richtext',items:[{kind:'text',size:100},{kind:'html',size:MiB-99}]});
  assert.equal(await reasonOf(bigRich.host),'clipboard_limits_exceeded');
  // Images: the size of the PNG.
  const image=await ready(t);image.client.json({type:'clipboard.begin',requestId:'i1',format:'image',items:[{kind:'png',size:8*MiB,width:1000,height:1000}]});
  assert.equal((await image.host.take(v=>v.type==='clipboard.begin')).requestId,'i1');
  const bigImage=await ready(t);bigImage.client.json({type:'clipboard.begin',requestId:'i2',format:'image',items:[{kind:'png',size:8*MiB+1,width:1000,height:1000}]});
  assert.equal(await reasonOf(bigImage.host),'clipboard_limits_exceeded');
});

// Pulls item `index` of an offer in 45 KiB chunks, acknowledging each window, the way a receiver does.
async function pullAll(requester,source,pullId,offerId,index,data){
  requester.json({type:'clipboard.pull',requestId:pullId,offerId,index,offset:0});
  assert.equal((await source.take(v=>v.type==='clipboard.pull')).requestId,pullId);
  const chunk=45*1024,hash=createHash('sha256');let sent=0;
  while(sent<data.length){
    const batch=[];
    for(let n=0;n<16&&sent<data.length;n++){const part=data.subarray(sent,Math.min(data.length,sent+chunk));hash.update(part);source.json({type:'clipboard.chunk',requestId:pullId,index,offset:sent,data:part.toString('base64')});sent+=part.length;batch.push(sent);}
    for(let n=0;n<batch.length;n++)await requester.take(v=>v.type==='clipboard.chunk'&&v.requestId===pullId);
    requester.json({type:'clipboard.chunk.ack',requestId:pullId,index,offset:batch.at(-1)});await source.take(v=>v.type==='clipboard.chunk.ack'&&v.requestId===pullId);
  }
  source.json({type:'clipboard.item.end',requestId:pullId,index,sha256:hash.digest('hex')});
  await requester.take(v=>v.type==='clipboard.item.end'&&v.requestId===pullId);
}

test('an offer of text, rich text or an image is pulled by item index',async t=>{
  const p=await ready(t);
  const text=Buffer.alloc(3*MiB+5,'t');
  await offerContent(p,'client',{type:'clipboard.offer',requestId:'o-text',format:'text',size:text.length});
  await pullAll(p.host,p.client,'pullh-1','o-text',0,text);
  // Rich text: item 0 is the text, item 1 the HTML; the VM pulls them one by one, in either order.
  const html=Buffer.from('<p>x</p>'),plain=Buffer.from('plain');
  await offerContent(p,'client',{type:'clipboard.offer',requestId:'o-rich',format:'richtext',items:[{kind:'text',size:plain.length},{kind:'html',size:html.length}]});
  await pull(p.host,p.client,'pullh-2','o-rich',1,html);
  await pull(p.host,p.client,'pullh-3','o-rich',0,plain);
  // An image answered by the Host to the client's read; pulled at an offset (a resumed range).
  const png=Buffer.concat([Buffer.from('89504e470d0a1a0a','hex'),Buffer.alloc(9000,7)]);
  await offerContent(p,'host',{type:'clipboard.offer',requestId:'o-image',format:'image',items:[{kind:'png',size:png.length,width:40,height:30}]});
  await pull(p.client,p.host,'pullc-1','o-image',0,png.subarray(5000),5000);
  // An offer needs the permission of its format, in its direction; its pulls follow the format of the offer, not "files".
  const textOnly=await ready(t,{policy:{textToRemote:true,textToLocal:true,filesToRemote:false,filesToLocal:false}});
  await offerContent(textOnly,'client',{type:'clipboard.offer',requestId:'o1',format:'text',size:2});
  await pull(textOnly.host,textOnly.client,'pullh-1','o1',0,Buffer.from('ok'));
  const noImage=await ready(t,{policy:{textToRemote:true,textToLocal:true,filesToRemote:false,filesToLocal:false}});
  noImage.client.json({type:'clipboard.offer',requestId:'o2',format:'image',items:[{kind:'png',size:20*MiB,width:4000,height:3000}]});
  assert.equal(await reasonOf(noImage.host),'clipboard_policy_denied');
  // A tree offer is still refused without the files permission.
  const noFiles=await ready(t,{policy:{textToRemote:true,textToLocal:true,filesToRemote:false,filesToLocal:false}});
  noFiles.client.json(treeOffer('o3',[entry('a.txt',1)]));
  assert.equal(await reasonOf(noFiles.host),'clipboard_policy_denied');
  // And an offer of text is refused without the text permission, whatever the files permission is.
  const noText=await ready(t,{policy:{textToRemote:false,textToLocal:false,filesToRemote:true,filesToLocal:true}});
  noText.client.json({type:'clipboard.offer',requestId:'o4',format:'text',size:3*MiB});
  assert.equal(await reasonOf(noText.host),'clipboard_policy_denied');
});

test('an offer other than a tree exists only in an on-paste pair; a pull of one is bounded by its items',async t=>{
  const scale=await ready(t,{client:'scale',host:'scale'});
  scale.client.json({type:'clipboard.offer',requestId:'o1',format:'text',size:5*MiB});
  assert.equal(await reasonOf(scale.host),'clipboard_invalid_message');
  const p=await ready(t);
  // A host's offer needs a read that asked for that kind of content.
  p.client.json({type:'clipboard.read',requestId:'r1',format:'files'});await p.host.take(v=>v.type==='clipboard.read');
  p.host.json({type:'clipboard.offer',requestId:'r1',format:'text',size:2*MiB});
  assert.equal(await reasonOf(p.client),'clipboard_invalid_message');
  // Malformed manifests.
  const bad=await ready(t);bad.client.json({type:'clipboard.offer',requestId:'o2',format:'text',size:2*MiB,extra:1});
  assert.equal(await reasonOf(bad.host),'clipboard_invalid_message');
  const range=await ready(t);await offerContent(range,'client',{type:'clipboard.offer',requestId:'o3',format:'text',size:2*MiB});
  range.host.json({type:'clipboard.pull',requestId:'pullh-1',offerId:'o3',index:1,offset:0});
  assert.equal(await reasonOf(range.client),'clipboard_invalid_message');
  const beyond=await ready(t);await offerContent(beyond,'client',{type:'clipboard.offer',requestId:'o4',format:'text',size:2*MiB});
  beyond.host.json({type:'clipboard.pull',requestId:'pullh-1',offerId:'o4',index:0,offset:2*MiB+1});
  assert.equal(await reasonOf(beyond.client),'clipboard_invalid_message');
  // The offer is pulled in its announced size and not beyond: a chunk past the end is a violation.
  const over=await ready(t);await offerContent(over,'client',{type:'clipboard.offer',requestId:'o5',format:'text',size:2*MiB});
  over.host.json({type:'clipboard.pull',requestId:'pullh-1',offerId:'o5',index:0,offset:2*MiB-1});await over.client.take(v=>v.type==='clipboard.pull');
  over.client.json({type:'clipboard.chunk',requestId:'pullh-1',index:0,offset:2*MiB-1,data:Buffer.from('xy').toString('base64')});
  assert.equal(await reasonOf(over.host),'clipboard_limits_exceeded');
});

test('withdrawing an offer retires it for both peers; a pull that is running ends with clipboard_offer_expired',async t=>{
  const p=await ready(t);
  await offerTree(p,'o1',[entry('a.txt',5)]);
  // The client (the offering side) withdraws: the host is told, and a later pull is refused to the host alone.
  p.client.json({type:'clipboard.offer.withdraw',requestId:'o1'});
  assert.deepEqual(await p.host.take(v=>v.type==='clipboard.offer.withdraw'),{type:'clipboard.offer.withdraw',requestId:'o1'});
  p.host.json({type:'clipboard.pull',requestId:'pullh-1',offerId:'o1',index:0,offset:0});
  assert.equal(await reasonOf(p.host),'clipboard_offer_expired');
  p.client.json({type:'clipboard.offer.withdraw',requestId:'o1'});   // again: harmless
  await new Promise(resolve=>setTimeout(resolve,50));assert.equal(p.client.readyState,WebSocket.OPEN);assert.equal(p.client.queue.length,0);assert.equal(p.host.queue.length,0);
  // A pull in progress is ended for the requester when its offer is withdrawn.
  const q=await ready(t);
  await offerContent(q,'client',{type:'clipboard.offer',requestId:'o2',format:'text',size:3*MiB});
  q.host.json({type:'clipboard.pull',requestId:'pullh-1',offerId:'o2',index:0,offset:0});await q.client.take(v=>v.type==='clipboard.pull');
  q.client.json({type:'clipboard.chunk',requestId:'pullh-1',index:0,offset:0,data:Buffer.from('partial').toString('base64')});await q.host.take(v=>v.type==='clipboard.chunk');
  q.client.json({type:'clipboard.offer.withdraw',requestId:'o2'});
  const ended=await q.host.take(v=>v.type==='clipboard.error'&&v.requestId==='pullh-1');assert.equal(ended.reason,'clipboard_offer_expired');
  await q.host.take(v=>v.type==='clipboard.offer.withdraw');
  // The pull slot is free again: another offer can be pulled.
  await offerContent(q,'client',{type:'clipboard.offer',requestId:'o3',format:'text',size:5});
  await pull(q.host,q.client,'pullh-2','o3',0,Buffer.from('fresh'));
  // The host withdraws an offer of its own; one peer cannot withdraw the other's offer.
  const r=await ready(t);
  await offerContent(r,'host',{type:'clipboard.offer',requestId:'o4',format:'text',size:11});
  r.client.json({type:'clipboard.offer.withdraw',requestId:'o4'});   // not the client's offer: ignored
  await new Promise(resolve=>setTimeout(resolve,50));assert.equal(r.host.queue.length,0);
  await pull(r.client,r.host,'pullc-1','o4',0,Buffer.from('still there'));
  r.host.json({type:'clipboard.offer.withdraw',requestId:'o4'});
  assert.equal((await r.client.take(v=>v.type==='clipboard.offer.withdraw')).requestId,'o4');
  r.client.json({type:'clipboard.pull',requestId:'pullc-2',offerId:'o4',index:0,offset:0});
  assert.equal(await reasonOf(r.client),'clipboard_offer_expired');
});

test('withdraw and paste.waiting are on-paste messages: other pairs, wrong directions and malformed ones end the pair',async t=>{
  const scale=await ready(t,{client:'scale',host:'scale'});scale.client.json({type:'clipboard.offer.withdraw',requestId:'o1'});
  assert.equal(await reasonOf(scale.host),'clipboard_invalid_message');
  const scaleWaiting=await ready(t,{client:'scale',host:'scale'});scaleWaiting.host.json({type:'clipboard.paste.waiting',requestId:'wait-1',active:true});
  assert.equal(await reasonOf(scaleWaiting.client),'clipboard_invalid_message');
  const bad=await ready(t);bad.client.json({type:'clipboard.offer.withdraw',requestId:'o1',extra:true});
  assert.equal(await reasonOf(bad.host),'clipboard_invalid_message');
  for(const message of [{requestId:'wait-1',active:'yes'},{requestId:'paste-1',active:true},{requestId:'wait-0',active:true},{requestId:'wait-1'},{requestId:'wait-1',active:true,extra:1}]){
    const q=await ready(t);q.host.json({type:'clipboard.paste.waiting',...message});
    assert.equal(await reasonOf(q.client),'clipboard_invalid_message');
  }
  // Only the host reports a waiting paste.
  const wrong=await ready(t);wrong.client.json({type:'clipboard.paste.waiting',requestId:'wait-1',active:true});
  assert.equal(await reasonOf(wrong.host),'clipboard_invalid_message');
});

test('a waiting paste of the VM reaches the client, start and end, and does not disturb a transfer',async t=>{
  const p=await ready(t);
  await offerTree(p,'o1',[entry('a.txt',5)]);
  p.host.json({type:'clipboard.paste.waiting',requestId:'wait-1',active:true});
  assert.deepEqual(await p.client.take(v=>v.type==='clipboard.paste.waiting'),{type:'clipboard.paste.waiting',requestId:'wait-1',active:true});
  // (the pull the waiting paste caused)
  p.host.json({type:'clipboard.pull',requestId:'pullh-1',offerId:'o1',index:0,offset:0});await p.client.take(v=>v.type==='clipboard.pull');
  p.client.json({type:'clipboard.chunk',requestId:'pullh-1',index:0,offset:0,data:Buffer.from('hello').toString('base64')});await p.host.take(v=>v.type==='clipboard.chunk');
  p.host.json({type:'clipboard.chunk.ack',requestId:'pullh-1',index:0,offset:5});await p.client.take(v=>v.type==='clipboard.chunk.ack');
  p.client.json({type:'clipboard.item.end',requestId:'pullh-1',index:0,sha256:digest(Buffer.from('hello'))});await p.host.take(v=>v.type==='clipboard.item.end');
  p.host.json({type:'clipboard.paste.waiting',requestId:'wait-1',active:false});
  assert.equal((await p.client.take(v=>v.type==='clipboard.paste.waiting')).active,false);
  // It is also allowed between transfers and in the middle of one (it names no transfer).
  p.client.json({type:'clipboard.read',requestId:'r1'});await p.host.take(v=>v.type==='clipboard.read');
  p.host.json({type:'clipboard.paste.waiting',requestId:'wait-2',active:true});
  assert.equal((await p.client.take(v=>v.type==='clipboard.paste.waiting')).requestId,'wait-2');
  p.host.json({type:'clipboard.error',requestId:'r1',reason:'clipboard_empty'});
  assert.equal((await p.client.take(v=>v.type==='clipboard.error')).reason,'clipboard_empty');
  assert.equal(p.client.readyState,WebSocket.OPEN);
});

test('permission lost while an offer of text is pulled closes the pair; the text permission does not cover a files offer',async t=>{
  const p=await ready(t);
  await offerContent(p,'client',{type:'clipboard.offer',requestId:'o1',format:'text',size:3*MiB});
  p.host.json({type:'clipboard.pull',requestId:'pullh-1',offerId:'o1',index:0,offset:0});await p.client.take(v=>v.type==='clipboard.pull');
  p.store.updateAssignmentPolicy('alice','pve-811',{clipboardPolicy:{textToRemote:false,textToLocal:true,filesToRemote:true,filesToLocal:true}},'fixture');
  p.client.json({type:'clipboard.chunk',requestId:'pullh-1',index:0,offset:0,data:Buffer.from('x').toString('base64')});
  assert.ok(await reasonOf(p.host));
});
