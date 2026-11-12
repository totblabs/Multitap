import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { createServer } from 'node:http';
import { WebSocket } from 'ws';
import { Relay } from '../src/relay.mjs';
import { Store } from '../src/store.mjs';
import { newToken,tokenHash } from '../src/security.mjs';
import { clipboardPullQueueProtocol,pullQueueDepth } from '../src/clipboard.mjs';

// Pull queue (X-Multitap-Clipboard-PullQueue: 1 on top of the on-paste revision): a paste reads files one after another and
// every pull is a round trip, one at a time per pair. With the queue a requester may have pullQueueDepth pulls outstanding, served in order by the
// offering peer with a 4 MiB window each; every other pair keeps one pull at a time and its 1 MiB window.
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
const onPaste={'X-Multitap-Clipboard-Limits':'2','X-Multitap-Clipboard-Formats':'2','X-Multitap-Clipboard-Pull':'1','X-Multitap-Clipboard-Scale':'1','X-Multitap-Clipboard-OnPaste':'1'};
const flags=side=>side==='queue'?{...onPaste,'X-Multitap-Clipboard-PullQueue':'1'}:side==='onpaste'?onPaste:side==='queue-only'?{'X-Multitap-Clipboard-PullQueue':'1'}:undefined;
async function ready(t,{client='queue',host='queue',relayOptions={}}={}){
  const store=new Store(':memory:'),tokens={},users={};
  for(const name of ['alice']){users[name]=store.addUser({name,passwordHash:'fixture-only'});tokens[name]=newToken();store.createSession(store.findUser(name),tokenHash(tokens[name]),Date.now(),3600000);}
  store.syncProxmox([811].map(vmid=>({vmid,node:'fixture',name:'Clipboard '+vmid,status:'running'})),'fixture');
  store.assign('alice','pve-811');store.updateAssignmentPolicy('alice','pve-811',{clipboardPolicy:all},'fixture');
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
// The VM copies files: the PC reads the copy, the Host offers it as a tree, the PC publishes it.
async function hostOffer(p,requestId,sizes){
  const entries=sizes.map((size,i)=>({path:'f'+i+'.bin',kind:'file',size,attrs:0x20,mtime:1761500000123}));
  p.client.json({type:'clipboard.read',requestId});await p.host.take(v=>v.type==='clipboard.read');
  p.host.json({type:'clipboard.offer',requestId,format:'tree',entries:entries.length,files:entries.length,bytes:sizes.reduce((s,n)=>s+n,0),tops:entries.length,skipped:0});
  await p.client.take(v=>v.type==='clipboard.offer');
  p.host.json({type:'clipboard.entries',requestId,start:0,entries});await p.client.take(v=>v.type==='clipboard.entries');
  p.client.json({type:'clipboard.applied',requestId});
  await p.client.take(v=>v.type==='clipboard.complete');await p.host.take(v=>v.type==='clipboard.complete');
}
const pullOf=(id,offerId,index)=>({type:'clipboard.pull',requestId:id,offerId,index,offset:0});
// The Host serves one whole small item: chunks of at most 45 KiB, then the item end; the PC receives it all.
async function serve(p,id,index,data){
  for(let at=0;at<data.length;at+=46080)p.host.json({type:'clipboard.chunk',requestId:id,index,offset:at,data:data.subarray(at,Math.min(data.length,at+46080)).toString('base64')});
  p.host.json({type:'clipboard.item.end',requestId:id,index,sha256:digest(data)});
  assert.equal((await p.client.take(v=>v.type==='clipboard.item.end'&&v.requestId===id)).sha256,digest(data));
}

test('the pull queue is advertised only to an on-paste pair whose peers both announced it',async t=>{
  const both=await ready(t);
  assert.equal(both.clientReady.clipboardPullQueue,clipboardPullQueueProtocol);assert.equal(both.hostReady.clipboardPullQueue,clipboardPullQueueProtocol);
  assert.equal(clipboardPullQueueProtocol,'pull-queue-4-v1');assert.equal(pullQueueDepth,4);
  for(const [client,host] of [['queue','onpaste'],['onpaste','queue'],['queue-only','queue']])await t.test(client+'/'+host,async t=>{
    const p=await ready(t,{client,host});
    assert.equal(p.clientReady.clipboardPullQueue,undefined);assert.equal(p.hostReady.clipboardPullQueue,undefined);
  });
});

test('a requester queues four pulls; the Host gets them all at once and serves them in order',async t=>{
  const p=await ready(t);await hostOffer(p,'o1',[1000,2000,3000,4000,5000]);
  for(let n=1;n<=4;n++)p.client.json(pullOf('pullc-'+n,'o1',n-1));
  for(let n=1;n<=4;n++)assert.equal((await p.host.take(v=>v.type==='clipboard.pull')).requestId,'pullc-'+n);
  // A fifth is one too many until one ends.
  p.client.json(pullOf('pullc-5','o1',4));assert.equal(await reasonOf(p.client),'clipboard_busy');
  for(let n=1;n<=4;n++)await serve(p,'pullc-'+n,n-1,Buffer.alloc(1000*n,n));
  p.client.json(pullOf('pullc-6','o1',4));assert.equal((await p.host.take(v=>v.type==='clipboard.pull')).requestId,'pullc-6');
  await serve(p,'pullc-6',4,Buffer.alloc(5000,6));
});

test('data only for the oldest queued pull; a queued pull cancelled before its turn is cancelled at the Host too',async t=>{
  const p=await ready(t);await hostOffer(p,'o1',[1000,2000,3000]);
  for(let n=1;n<=3;n++)p.client.json(pullOf('pullc-'+n,'o1',n-1));
  for(let n=1;n<=3;n++)await p.host.take(v=>v.type==='clipboard.pull');
  // The PC no longer wants the second file: the Host is told, so that it skips it.
  p.client.json({type:'clipboard.cancel',requestId:'pullc-2'});
  assert.equal((await p.host.take(v=>v.type==='clipboard.cancel')).requestId,'pullc-2');
  await serve(p,'pullc-1',0,Buffer.alloc(1000,1));
  await serve(p,'pullc-3',2,Buffer.alloc(3000,3));
  // Out of order: data of a later pull while an older one is still open ends the pair.
  const q=await ready(t);await hostOffer(q,'o2',[1000,2000]);
  q.client.json(pullOf('pullc-1','o2',0));q.client.json(pullOf('pullc-2','o2',1));
  await q.host.take(v=>v.requestId==='pullc-2');
  q.host.json({type:'clipboard.chunk',requestId:'pullc-2',index:1,offset:0,data:Buffer.alloc(100).toString('base64')});
  assert.equal(await reasonOf(q.client),'clipboard_out_of_order');
});

test('a queued pull has a 4 MiB window; an older pair keeps one pull at a time and 1 MiB',async t=>{
  const p=await ready(t);await hostOffer(p,'big',[6*MiB]);
  p.client.json(pullOf('pullc-1','big',0));await p.host.take(v=>v.type==='clipboard.pull');
  const chunk=Buffer.alloc(46080,7);let sent=0;
  for(;sent+chunk.length<=4*MiB;sent+=chunk.length)p.host.json({type:'clipboard.chunk',requestId:'pullc-1',index:0,offset:sent,data:chunk.toString('base64')});
  for(let n=0;n<Math.floor(4*MiB/chunk.length);n++)await p.client.take(v=>v.type==='clipboard.chunk');
  assert.ok(sent>3*MiB,'more than the released window crossed without an acknowledgement');
  const old=await ready(t,{client:'onpaste',host:'onpaste'});await hostOffer(old,'big',[6*MiB]);
  old.client.json(pullOf('pullc-1','big',0));await old.host.take(v=>v.type==='clipboard.pull');
  old.client.json(pullOf('pullc-2','big',0));assert.equal(await reasonOf(old.client),'clipboard_busy');
  for(let at=0;at<2*MiB;at+=chunk.length)old.host.json({type:'clipboard.chunk',requestId:'pullc-1',index:0,offset:at,data:chunk.toString('base64')});
  assert.equal(await reasonOf(old.client),'clipboard_out_of_order');
});

test('a pull-queue pair moves at its own per-socket rate; the shared budget still applies',async t=>{
  // 256 KiB/s for every other windowed pair (an older pair would need several seconds for this); the queue pair is not slowed.
  const p=await ready(t,{relayOptions:{clipboardOptions:{fastBytes:256*1024}}});await hostOffer(p,'o1',[3*MiB]);
  const started=Date.now();
  p.client.json(pullOf('pullc-1','o1',0));await p.host.take(v=>v.type==='clipboard.pull');
  const chunk=Buffer.alloc(46080,9);let at=0;
  for(;at+chunk.length<=3*MiB;at+=chunk.length)p.host.json({type:'clipboard.chunk',requestId:'pullc-1',index:0,offset:at,data:chunk.toString('base64')});
  for(let n=0;n<Math.floor(3*MiB/chunk.length);n++)await p.client.take(v=>v.type==='clipboard.chunk');
  assert.ok(Date.now()-started<2000,'the queue pair was slowed to the older pairs\' rate: '+(Date.now()-started)+' ms');
});

test('withdrawing an offer ends every queued pull of it',async t=>{
  const p=await ready(t);await hostOffer(p,'o1',[1000,2000,3000]);
  for(let n=1;n<=3;n++)p.client.json(pullOf('pullc-'+n,'o1',n-1));
  for(let n=1;n<=3;n++)await p.host.take(v=>v.type==='clipboard.pull');
  p.host.json({type:'clipboard.offer.withdraw',requestId:'o1'});
  const ended=new Set();for(let n=0;n<3;n++){const error=await p.client.take(v=>v.type==='clipboard.error');assert.equal(error.reason,'clipboard_offer_expired');ended.add(error.requestId);}
  assert.deepEqual([...ended].sort(),['pullc-1','pullc-2','pullc-3']);
  assert.equal(p.relay.clipboard.activeTransfers,0);
});
