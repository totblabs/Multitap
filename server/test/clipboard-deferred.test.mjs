import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { createServer } from 'node:http';
import { WebSocket } from 'ws';
import { Relay } from '../src/relay.mjs';
import { Store } from '../src/store.mjs';
import { newToken,tokenHash } from '../src/security.mjs';
import { clipboardDeferredProtocol } from '../src/clipboard.mjs';

// Deferred manifest (X-Multitap-Clipboard-Deferred: 1 on top of the on-paste revision, like RDP): a copy of files that is not small is
// offered without its list (clipboard.offer {deferred:true}); a paste asks for the list (clipboard.manifest, a pull id), the offering
// peer walks the copy then and answers with progress, the counts and the entry pages. Files are pulled as usual afterwards.
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
    ws.closed=new Promise(resolve=>ws.once('close',(code,reason)=>resolve(reason.toString())));
    ws.once('open',()=>resolve(ws));
  });
}
const onPaste={'X-Multitap-Clipboard-Limits':'2','X-Multitap-Clipboard-Formats':'2','X-Multitap-Clipboard-Pull':'1','X-Multitap-Clipboard-Scale':'1','X-Multitap-Clipboard-OnPaste':'1','X-Multitap-Clipboard-PullQueue':'1'};
const flags=side=>side==='deferred'?{...onPaste,'X-Multitap-Clipboard-Deferred':'1'}:side==='onpaste'?onPaste:side==='deferred-only'?{'X-Multitap-Clipboard-Deferred':'1'}:undefined;
async function ready(t,{client='deferred',host='deferred',relayOptions={}}={}){
  const store=new Store(':memory:'),tokens={},users={};
  for(const name of ['alice']){users[name]=store.addUser({name,passwordHash:'fixture-only'});tokens[name]=newToken();store.createSession(store.findUser(name),tokenHash(tokens[name]),Date.now(),3600000);}
  store.syncProxmox([812].map(vmid=>({vmid,node:'fixture',name:'Clipboard '+vmid,status:'running'})),'fixture');
  store.assign('alice','pve-812');store.updateAssignmentPolicy('alice','pve-812',{clipboardPolicy:all},'fixture');
  const code=newToken(),uuid='12345678-1234-1234-1234-000000000812',hostToken=newToken();
  store.issueEnrollment('pve-812',uuid,tokenHash(code),'fixture');store.consumeEnrollment(tokenHash(code),uuid,{hostname:'Fixture host',version:'0.2.3',tokenHash:tokenHash(hostToken)});
  const relay=new Relay({store,publicOrigin:'https://clipboard-fixture.example',authorizationMs:10,heartbeatMs:1000,...relayOptions});
  const server=createServer();relay.attach(server);
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  const base='ws://127.0.0.1:'+server.address().port;relay.publicOrigin='https://127.0.0.1:'+server.address().port;
  t.after(async()=>{await new Promise(resolve=>server.close(resolve));store.close();});
  const url=path=>base+new URL(path).pathname;
  const main=await socket(base+'/api/v1/hosts/channel','Host '+hostToken);
  main.json({type:'host.capabilities',capabilities:caps});await main.take(v=>v.type==='host.capabilities.ack');
  const connection=relay.connect({id:users.alice},tokenHash(tokens.alice),'pve-812','stream');
  const media=await socket(url(connection.webSocketUrl),'Bearer '+tokens.alice);
  const start=await main.take(v=>v.type==='session.start');
  const c=await socket(url(connection.clipboardWebSocketUrl),'Bearer '+tokens.alice,{headers:flags(client)});
  const h=await socket(url(start.clipboardUrl),'Host '+hostToken,{headers:flags(host)});
  const clientReady=await c.take(v=>v.type==='clipboard.ready'),hostReady=await h.take(v=>v.type==='clipboard.ready');
  return{relay,store,client:c,host:h,clientReady,hostReady,media,connection,main};
}
async function reasonOf(ws,ms){return(await ws.take(v=>v.type==='clipboard.error',ms)).reason;}
// The VM copies a large folder: the PC reads the copy, the Host offers it deferred, the PC publishes it.
async function hostDeferred(p,requestId){
  p.client.json({type:'clipboard.read',requestId});await p.host.take(v=>v.type==='clipboard.read');
  p.host.json({type:'clipboard.offer',requestId,format:'tree',deferred:true});
  const offer=await p.client.take(v=>v.type==='clipboard.offer');assert.deepEqual(offer,{type:'clipboard.offer',requestId,format:'tree',deferred:true});
  p.client.json({type:'clipboard.applied',requestId});
  await p.client.take(v=>v.type==='clipboard.complete');await p.host.take(v=>v.type==='clipboard.complete');
}
const entriesOf=sizes=>[{path:'Big',kind:'dir',attrs:0,mtime:1761500000000},...sizes.map((size,i)=>({path:'Big/f'+i+'.bin',kind:'file',size,attrs:0x20,mtime:1761500000123}))];
const beginOf=(id,offerId,sizes)=>({type:'clipboard.manifest.begin',requestId:id,offerId,entries:sizes.length+1,files:sizes.length,bytes:sizes.reduce((s,n)=>s+n,0),tops:1,skipped:0});
// The Host answers a list request: progress, the counts, the pages.
async function answer(p,id,offerId,sizes){
  p.host.json({type:'clipboard.manifest.progress',requestId:id,entries:1});
  assert.equal((await p.client.take(v=>v.type==='clipboard.manifest.progress')).entries,1);
  p.host.json(beginOf(id,offerId,sizes));await p.client.take(v=>v.type==='clipboard.manifest.begin');
  const entries=entriesOf(sizes);
  for(let at=0;at<entries.length;at+=200){p.host.json({type:'clipboard.entries',requestId:id,start:at,entries:entries.slice(at,at+200)});await p.client.take(v=>v.type==='clipboard.entries'&&v.start===at);}
}

test('the deferred manifest is advertised only to an on-paste pair whose peers both announced it',async t=>{
  const both=await ready(t);
  assert.equal(clipboardDeferredProtocol,'manifest-on-paste-v1');
  assert.equal(both.clientReady.clipboardDeferred,clipboardDeferredProtocol);assert.equal(both.hostReady.clipboardDeferred,clipboardDeferredProtocol);
  for(const [client,host] of [['deferred','onpaste'],['onpaste','deferred'],['deferred-only','deferred']])await t.test(client+'/'+host,async t=>{
    const p=await ready(t,{client,host});
    assert.equal(p.clientReady.clipboardDeferred,undefined);assert.equal(p.hostReady.clipboardDeferred,undefined);
  });
});

test('a deferred offer crosses without a list; the paste asks for it, then pulls the files',async t=>{
  const p=await ready(t);await hostDeferred(p,'o1');
  p.client.json({type:'clipboard.manifest',requestId:'pullc-1',offerId:'o1'});
  assert.deepEqual(await p.host.take(v=>v.type==='clipboard.manifest'),{type:'clipboard.manifest',requestId:'pullc-1',offerId:'o1'});
  await answer(p,'pullc-1','o1',[1000,2000,300]);
  // The list has passed: a file is pulled like any offered file, checked against the list's sizes.
  p.client.json({type:'clipboard.pull',requestId:'pullc-2',offerId:'o1',index:1,offset:0});
  await p.host.take(v=>v.type==='clipboard.pull');
  const data=Buffer.alloc(2000,7);
  p.host.json({type:'clipboard.chunk',requestId:'pullc-2',index:1,offset:0,data:data.toString('base64')});
  p.host.json({type:'clipboard.item.end',requestId:'pullc-2',index:1,sha256:digest(data)});
  assert.equal((await p.client.take(v=>v.type==='clipboard.item.end')).sha256,digest(data));
});

test('the PC copies a large folder: deferred offer from the client, the Host asks for the list',async t=>{
  const p=await ready(t);
  p.client.json({type:'clipboard.offer',requestId:'c1',format:'tree',deferred:true});
  await p.host.take(v=>v.type==='clipboard.offer');
  p.host.json({type:'clipboard.applied',requestId:'c1'});
  await p.host.take(v=>v.type==='clipboard.complete');await p.client.take(v=>v.type==='clipboard.complete');
  p.host.json({type:'clipboard.manifest',requestId:'pullh-1',offerId:'c1'});
  await p.client.take(v=>v.type==='clipboard.manifest');
  p.client.json(beginOf('pullh-1','c1',[5]));await p.host.take(v=>v.type==='clipboard.manifest.begin');
  p.client.json({type:'clipboard.entries',requestId:'pullh-1',start:0,entries:entriesOf([5])});await p.host.take(v=>v.type==='clipboard.entries');
  p.host.json({type:'clipboard.pull',requestId:'pullh-2',offerId:'c1',index:0,offset:0});
  await p.client.take(v=>v.type==='clipboard.pull');
});

test('a walk that meets a limit fails the request alone; the offer stays deferred and may be asked again',async t=>{
  const p=await ready(t);await hostDeferred(p,'o1');
  p.client.json({type:'clipboard.manifest',requestId:'pullc-1',offerId:'o1'});await p.host.take(v=>v.type==='clipboard.manifest');
  p.host.json({type:'clipboard.manifest.progress',requestId:'pullc-1',entries:150000});await p.client.take(v=>v.type==='clipboard.manifest.progress');
  p.host.json({type:'clipboard.error',requestId:'pullc-1',reason:'clipboard_entry_count_limit'});
  assert.equal(await reasonOf(p.client),'clipboard_entry_count_limit');
  // A progress message still in flight after the error is dropped, not a violation.
  p.host.json({type:'clipboard.manifest.progress',requestId:'pullc-1',entries:160000});
  p.client.json({type:'clipboard.manifest',requestId:'pullc-2',offerId:'o1'});await p.host.take(v=>v.type==='clipboard.manifest'&&v.requestId==='pullc-2');
  await answer(p,'pullc-2','o1',[10]);
});

test('the PC cancels a list request: the Host is told and its pages still in flight are dropped',async t=>{
  const p=await ready(t);await hostDeferred(p,'o1');
  p.client.json({type:'clipboard.manifest',requestId:'pullc-1',offerId:'o1'});await p.host.take(v=>v.type==='clipboard.manifest');
  p.host.json(beginOf('pullc-1','o1',[1,2,3]));await p.client.take(v=>v.type==='clipboard.manifest.begin');
  p.client.json({type:'clipboard.cancel',requestId:'pullc-1'});
  assert.equal((await p.host.take(v=>v.type==='clipboard.cancel')).requestId,'pullc-1');
  p.host.json({type:'clipboard.entries',requestId:'pullc-1',start:0,entries:entriesOf([1,2,3])});
  // The pair is still there: a new request goes through.
  p.client.json({type:'clipboard.manifest',requestId:'pullc-2',offerId:'o1'});await p.host.take(v=>v.type==='clipboard.manifest'&&v.requestId==='pullc-2');
  await answer(p,'pullc-2','o1',[1,2,3]);
});

// The receiving app may ask again for a list that already crossed (its own checks refused it, or its cancel crossed the last page):
// that is a request like the first, answered again, and the pair stays (until 1.1.6 it ended the pair as a violation).
test('a list asked for again after it crossed is answered again; the newer list is the one files are checked against',async t=>{
  const p=await ready(t);await hostDeferred(p,'o1');
  p.client.json({type:'clipboard.manifest',requestId:'pullc-1',offerId:'o1'});await p.host.take(v=>v.type==='clipboard.manifest');
  await answer(p,'pullc-1','o1',[1000]);
  p.client.json({type:'clipboard.manifest',requestId:'pullc-2',offerId:'o1'});
  assert.deepEqual(await p.host.take(v=>v.type==='clipboard.manifest'&&v.requestId==='pullc-2'),{type:'clipboard.manifest',requestId:'pullc-2',offerId:'o1'});
  await answer(p,'pullc-2','o1',[1000,64]);
  p.client.json({type:'clipboard.pull',requestId:'pullc-3',offerId:'o1',index:1,offset:0});
  await p.host.take(v=>v.type==='clipboard.pull'&&v.requestId==='pullc-3');
  const data=Buffer.alloc(64,9);
  p.host.json({type:'clipboard.chunk',requestId:'pullc-3',index:1,offset:0,data:data.toString('base64')});
  p.host.json({type:'clipboard.item.end',requestId:'pullc-3',index:1,sha256:digest(data)});
  assert.equal((await p.client.take(v=>v.type==='clipboard.item.end')).sha256,digest(data));
});

test('a list header with a field of its own is not accepted',async t=>{
  const p=await ready(t);await hostDeferred(p,'o1');
  p.client.json({type:'clipboard.manifest',requestId:'pullc-1',offerId:'o1'});await p.host.take(v=>v.type==='clipboard.manifest');
  p.host.json({...beginOf('pullc-1','o1',[4]),format:'tree'});
  assert.match(await p.client.closed,/./);
});

test('one list request per offer; a pull before the list or a malformed page ends the pair',async t=>{
  const p=await ready(t);await hostDeferred(p,'o1');
  p.client.json({type:'clipboard.manifest',requestId:'pullc-1',offerId:'o1'});await p.host.take(v=>v.type==='clipboard.manifest');
  p.client.json({type:'clipboard.manifest',requestId:'pullc-2',offerId:'o1'});
  assert.equal(await reasonOf(p.client),'clipboard_busy');
  const q=await ready(t);await hostDeferred(q,'o2');
  q.client.json({type:'clipboard.pull',requestId:'pullc-1',offerId:'o2',index:0,offset:0});
  assert.match(await q.client.closed,/./);
  const r=await ready(t);await hostDeferred(r,'o3');
  r.client.json({type:'clipboard.manifest',requestId:'pullc-1',offerId:'o3'});await r.host.take(v=>v.type==='clipboard.manifest');
  r.host.json(beginOf('pullc-1','o3',[4]));await r.client.take(v=>v.type==='clipboard.manifest.begin');
  // A page whose entry has no write time is not an offer's page.
  r.host.json({type:'clipboard.entries',requestId:'pullc-1',start:0,entries:[{path:'Big',kind:'dir',attrs:0}]});
  assert.match(await r.client.closed,/./);
});

test('a deferred offer withdrawn while its list is asked for ends the request with the expired reason',async t=>{
  const p=await ready(t);await hostDeferred(p,'o1');
  p.client.json({type:'clipboard.manifest',requestId:'pullc-1',offerId:'o1'});await p.host.take(v=>v.type==='clipboard.manifest');
  p.host.json({type:'clipboard.offer.withdraw',requestId:'o1'});
  assert.equal(await reasonOf(p.client),'clipboard_offer_expired');
  await p.client.take(v=>v.type==='clipboard.offer.withdraw');
});

test('a list request with no word from the walk for the idle bound is cancelled for both peers',async t=>{
  const p=await ready(t,{relayOptions:{clipboardOptions:{idleMs:300}}});await hostDeferred(p,'o1');
  p.client.json({type:'clipboard.manifest',requestId:'pullc-1',offerId:'o1'});await p.host.take(v=>v.type==='clipboard.manifest');
  assert.equal((await p.client.take(v=>v.type==='clipboard.cancel'&&v.requestId==='pullc-1',5000)).requestId,'pullc-1');
  assert.equal((await p.host.take(v=>v.type==='clipboard.cancel'&&v.requestId==='pullc-1',5000)).requestId,'pullc-1');
});

test('a pair without the deferred manifest refuses a deferred offer',async t=>{
  const p=await ready(t,{client:'onpaste',host:'onpaste'});
  p.client.json({type:'clipboard.offer',requestId:'c1',format:'tree',deferred:true});
  assert.match(await p.client.closed,/./);
});
