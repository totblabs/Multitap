import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { createServer } from 'node:http';
import { WebSocket } from 'ws';
import { Relay } from '../src/relay.mjs';
import { Store } from '../src/store.mjs';
import { newToken,tokenHash } from '../src/security.mjs';
import { clipboardLimitsFormats,clipboardLimitsV2,clipboardLimits } from '../src/clipboard.mjs';
import { clipboardTransferAllowed,clipboardFormatPermission } from '../src/clipboard-policy.mjs';

const caps={service:true,interactiveSession:true,streaming:true,virtualDisplayReady:true,clipboard:true,clipboardSync:true};
const digest=bytes=>createHash('sha256').update(bytes).digest('hex');
const all={textToRemote:true,textToLocal:true,filesToRemote:true,filesToLocal:true};
const deny={textToRemote:false,textToLocal:false,filesToRemote:false,filesToLocal:false};
const chunk=limits=>limits.chunkBytes;
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
const flags=side=>side==='formats'?{'X-Multitap-Clipboard-Limits':'2','X-Multitap-Clipboard-Formats':'2'}:side==='limits'?{'X-Multitap-Clipboard-Limits':'2'}:side==='formats-only'?{'X-Multitap-Clipboard-Formats':'2'}:undefined;
// client / host: 'formats', 'limits' (extended limits only), 'formats-only' (header without limits), undefined (released).
async function ready(t,{client,host,policy=all}={}){
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
  return{relay,client:c,host:h,clientReady,hostReady,media,connection,main};
}
async function drain(ws,count,type){const out=[];for(let i=0;i<count;i++)out.push(await ws.take(v=>v.type===type));return out;}
const chunkMsg=(requestId,index,offset,bytes)=>({type:'clipboard.chunk',requestId,index,offset,data:bytes.toString('base64')});

test('policy mapping: rich text needs the text permission; images, files and folder trees need the files permission',()=>{
  assert.deepEqual({...clipboardFormatPermission},{text:'text',richtext:'text',files:'files',image:'files',tree:'files'});
  for(const [format,key] of [['text','text'],['richtext','text'],['files','files'],['image','files'],['tree','files']])for(const from of ['client','host']){
    const direction=key+(from==='client'?'ToRemote':'ToLocal');
    assert.equal(clipboardTransferAllowed({...deny,[direction]:true},format,from),true,format+' '+from);
    for(const other of Object.keys(deny))if(other!==direction)assert.equal(clipboardTransferAllowed({...deny,[other]:true},format,from),false,format+' '+from+' with '+other);
  }
  assert.equal(clipboardTransferAllowed(all,'bitmap','client'),false);assert.equal(clipboardTransferAllowed(all,'toString','client'),false);
});

test('only two peers that announce the formats profile are told about it; every other pairing keeps the previous limits exactly',async t=>{
  const both=await ready(t,{client:'formats',host:'formats'});
  assert.deepEqual(both.clientReady.limits,clipboardLimitsFormats);assert.deepEqual(both.hostReady.limits,clipboardLimitsFormats);
  assert.equal(clipboardLimitsFormats.chunkBytes,46080);
  for(const [client,host] of [['formats','limits'],['limits','formats'],['limits','limits']])await t.test(client+'/'+host,async t=>{
    const p=await ready(t,{client,host});assert.deepEqual(p.clientReady.limits,clipboardLimitsV2);assert.deepEqual(p.hostReady.limits,clipboardLimitsV2);
  });
  for(const [client,host] of [['formats',undefined],['formats-only','formats'],['formats','formats-only']])await t.test('released '+client+'/'+host,async t=>{
    const p=await ready(t,{client,host});
    for(const [side,released] of [[p.clientReady,client===undefined||client==='formats-only'],[p.hostReady,host===undefined||host==='formats-only']]){
      assert.deepEqual(side.limits,released?clipboardLimits:{...clipboardLimits,maxTextBytes:128*1024,limitsProfile:2});
    }
  });
});

test('a pair without the formats profile refuses rich text, images and trees as invalid and keeps the old shapes working',async t=>{
  for(const message of [
    {type:'clipboard.begin',requestId:'r',format:'richtext',items:[{kind:'text',size:1}]},
    {type:'clipboard.begin',requestId:'r',format:'image',items:[{kind:'png',size:10,width:1,height:1}]},
    {type:'clipboard.begin',requestId:'r',format:'tree',entries:1,files:0,bytes:0,tops:1,skipped:0},
  ])await t.test(message.format,async t=>{
    const p=await ready(t,{client:'limits',host:'formats'});p.client.json(message);
    assert.equal((await p.host.take(v=>v.type==='clipboard.error')).reason,'clipboard_invalid_message');assert.equal(p.media.readyState,WebSocket.OPEN);
  });
});

async function send(sender,receiver,requestId,index,bytes,limits){
  // A whole item with chunks pushed back to back (no waiting for acks) and acknowledged cumulatively.
  const parts=[];for(let offset=0;offset<bytes.length;offset+=chunk(limits))parts.push([offset,bytes.subarray(offset,offset+chunk(limits))]);
  for(const [offset,part] of parts)sender.json(chunkMsg(requestId,index,offset,part));
  for(const [offset,part] of parts){const m=await receiver.take(v=>v.type==='clipboard.chunk');assert.equal(m.offset,offset);assert.deepEqual(Buffer.from(m.data,'base64'),part);}
  if(parts.length){const [offset,part]=parts.at(-1);receiver.json({type:'clipboard.chunk.ack',requestId,index,offset:offset+part.length});assert.equal((await sender.take(v=>v.type==='clipboard.chunk.ack')).offset,offset+part.length);}
  sender.json({type:'clipboard.item.end',requestId,index,sha256:digest(bytes)});
  assert.equal((await receiver.take(v=>v.type==='clipboard.item.end')).sha256,digest(bytes));
  receiver.json({type:'clipboard.item.ack',requestId,index});await sender.take(v=>v.type==='clipboard.item.ack');
}
async function commit(sender,receiver,requestId){
  sender.json({type:'clipboard.commit',requestId});await receiver.take(v=>v.type==='clipboard.commit');
  receiver.json({type:'clipboard.applied',requestId});await sender.take(v=>v.type==='clipboard.complete');await receiver.take(v=>v.type==='clipboard.complete');
}

test('rich text moves as plain text plus HTML and RTF, windowed, and needs only the text permission',async t=>{
  const p=await ready(t,{client:'formats',host:'formats',policy:{...deny,textToRemote:true}});
  const text=Buffer.from('안녕 text'),html=Buffer.alloc(120000,'h'),rtf=Buffer.from('{\\rtf1 hi}');
  p.client.json({type:'clipboard.begin',requestId:'rich1',format:'richtext',items:[{kind:'text',size:text.length},{kind:'html',size:html.length},{kind:'rtf',size:rtf.length}]});
  assert.equal((await p.host.take(v=>v.type==='clipboard.begin')).format,'richtext');
  await send(p.client,p.host,'rich1',0,text,clipboardLimitsFormats);await send(p.client,p.host,'rich1',1,html,clipboardLimitsFormats);await send(p.client,p.host,'rich1',2,rtf,clipboardLimitsFormats);
  await commit(p.client,p.host,'rich1');
  for(const message of [
    {type:'clipboard.begin',requestId:'rich2',format:'richtext',items:[{kind:'html',size:5}]},
    {type:'clipboard.begin',requestId:'rich2',format:'richtext',items:[{kind:'text',size:1},{kind:'rtf',size:1},{kind:'html',size:1}]},
    {type:'clipboard.begin',requestId:'rich2',format:'richtext',items:[{kind:'text',size:1},{kind:'html',size:8*1024*1024+1}]},
    {type:'clipboard.begin',requestId:'rich2',format:'richtext',items:[{kind:'text',size:1},{kind:'html',size:0}]},
    {type:'clipboard.begin',requestId:'rich2',format:'richtext',items:[{kind:'text',size:1},{kind:'text',size:1}]},
    {type:'clipboard.begin',requestId:'rich2',format:'richtext',items:[{kind:'text',size:1,extra:1}]},
  ]){
    const q=await ready(t,{client:'formats',host:'formats'});q.client.json(message);
    assert.match((await q.host.take(v=>v.type==='clipboard.error')).reason,/^clipboard_(invalid_message|limits_exceeded)$/);assert.equal(q.host.queue.some(v=>v.type==='clipboard.begin'),false);
  }
});

test('images are files-permission content with bounded size and pixels',async t=>{
  const textOnly=await ready(t,{client:'formats',host:'formats',policy:{...deny,textToRemote:true}});
  textOnly.client.json({type:'clipboard.begin',requestId:'img',format:'image',items:[{kind:'png',size:10,width:1,height:1}]});
  assert.equal((await textOnly.host.take(v=>v.type==='clipboard.error')).reason,'clipboard_policy_denied');
  const filesOnly=await ready(t,{client:'formats',host:'formats',policy:{...deny,filesToRemote:true}});
  filesOnly.client.json({type:'clipboard.begin',requestId:'rich',format:'richtext',items:[{kind:'text',size:1}]});
  assert.equal((await filesOnly.host.take(v=>v.type==='clipboard.error')).reason,'clipboard_policy_denied');
  const ok=await ready(t,{client:'formats',host:'formats',policy:{...deny,filesToRemote:true}});
  const png=Buffer.concat([Buffer.from([0x89,0x50,0x4e,0x47]),Buffer.alloc(100000,7)]);
  ok.client.json({type:'clipboard.begin',requestId:'img1',format:'image',items:[{kind:'png',size:png.length,width:100,height:50}]});
  await ok.host.take(v=>v.type==='clipboard.begin');await send(ok.client,ok.host,'img1',0,png,clipboardLimitsFormats);await commit(ok.client,ok.host,'img1');
  for(const [label,item] of [['pixels',{kind:'png',size:10,width:8001,height:8000}],['bytes',{kind:'png',size:256*1024*1024+1,width:1,height:1}],['zero',{kind:'png',size:0,width:1,height:1}],['kind',{kind:'bmp',size:1,width:1,height:1}],['width',{kind:'png',size:1,width:0,height:1}]]){
    const q=await ready(t,{client:'formats',host:'formats'});q.client.json({type:'clipboard.begin',requestId:'img2',format:'image',items:[item]});
    assert.match((await q.host.take(v=>v.type==='clipboard.error')).reason,/^clipboard_(invalid_message|limits_exceeded)$/,label);
  }
});

const entry=(path,size)=>size===undefined?{path,kind:'dir',attrs:0}:{path,kind:'file',size,attrs:0x20};
function treeBegin(requestId,entries){const files=entries.filter(e=>e.kind==='file');return{type:'clipboard.begin',requestId,format:'tree',entries:entries.length,files:files.length,bytes:files.reduce((s,e)=>s+e.size,0),tops:entries.filter(e=>!e.path.includes('/')).length,skipped:0};}

test('folder trees keep relative paths, empty folders and same-named files; items are addressed by file ordinal',async t=>{
  const p=await ready(t,{client:'formats',host:'formats',policy:{...deny,filesToRemote:true}});
  const a=Buffer.from('first'),b=Buffer.from('second one');
  const entries=[entry('Docs'),entry('Docs/a.txt',a.length),entry('Docs/Empty'),entry('Docs/Sub'),entry('Docs/Sub/a.txt',b.length),entry('top.bin',0)];
  p.client.json(treeBegin('tree1',entries));await p.host.take(v=>v.type==='clipboard.begin');
  p.client.json({type:'clipboard.entries',requestId:'tree1',start:0,entries:entries.slice(0,3)});p.client.json({type:'clipboard.entries',requestId:'tree1',start:3,entries:entries.slice(3)});
  assert.equal((await p.host.take(v=>v.type==='clipboard.entries')).start,0);assert.equal((await p.host.take(v=>v.type==='clipboard.entries')).start,3);
  await send(p.client,p.host,'tree1',0,a,clipboardLimitsFormats);await send(p.client,p.host,'tree1',1,b,clipboardLimitsFormats);
  p.client.json({type:'clipboard.item.end',requestId:'tree1',index:2,sha256:digest(Buffer.alloc(0))});await p.host.take(v=>v.type==='clipboard.item.end');
  p.host.json({type:'clipboard.item.ack',requestId:'tree1',index:2});await p.client.take(v=>v.type==='clipboard.item.ack');
  await commit(p.client,p.host,'tree1');
});

test('folder tree manifests are validated entry by entry before anything reaches the receiver',async t=>{
  const tops=Array.from({length:129},(_,i)=>entry('d'+i));
  const cases=[
    ['traversal',[entry('..'),entry('../x',1)]],
    ['absolute',[entry('/etc'),entry('/etc/x',1)]],
    ['drive',[entry('C:'),entry('C:/x',1)]],
    ['backslash',[entry('a\\b',1)]],
    ['empty component',[entry('a'),entry('a//b',1)]],
    ['parent missing',[entry('a/b.txt',1)]],
    ['file as parent',[entry('a',1),entry('a/b',1)]],
    ['duplicate folded',[entry('A'),entry('a')]],
    ['device name',[entry('NUL')]],
    ['too deep',Array.from({length:33},(_,i)=>entry(Array.from({length:i+1},()=>'d').join('/')))],
    ['path too long',[entry('x'.repeat(100)),entry('x'.repeat(100)+'/'+'y'.repeat(100),1)]],
    ['too many tops',tops],
  ];
  for(const [label,entries] of cases)await t.test(label,async t=>{
    const p=await ready(t,{client:'formats',host:'formats',policy:{...deny,filesToRemote:true}});
    p.client.json(treeBegin('bad',entries));
    // A declared top-level count that disagrees with the listing is refused at once (tops counts paths without '/').
    const first=await p.host.take(v=>v.type==='clipboard.begin'||v.type==='clipboard.error');
    if(first.type==='clipboard.error'){assert.match(first.reason,/^clipboard_(invalid_message|limits_exceeded)$/);return;}
    p.client.json({type:'clipboard.entries',requestId:'bad',start:0,entries});
    assert.match((await p.host.take(v=>v.type==='clipboard.error')).reason,/^clipboard_(invalid_message|limits_exceeded)$/);
    assert.equal(p.host.queue.some(v=>v.type==='clipboard.entries'),false);
  });
  await t.test('declared totals must be met',async t=>{
    const p=await ready(t,{client:'formats',host:'formats',policy:{...deny,filesToRemote:true}});
    const entries=[entry('a',5)];p.client.json({...treeBegin('bad',entries),bytes:6});await p.host.take(v=>v.type==='clipboard.begin');
    p.client.json({type:'clipboard.entries',requestId:'bad',start:0,entries});assert.equal((await p.host.take(v=>v.type==='clipboard.error')).reason,'clipboard_invalid_message');
  });
  await t.test('limits and attrs',async t=>{
    for(const begin of [{entries:10001,files:0,bytes:0,tops:1},{entries:1,files:0,bytes:8*1024*1024*1024+1,tops:1}]){
      const p=await ready(t,{client:'formats',host:'formats',policy:{...deny,filesToRemote:true}});
      p.client.json({type:'clipboard.begin',requestId:'bad',format:'tree',skipped:0,...begin});assert.equal((await p.host.take(v=>v.type==='clipboard.error')).reason,'clipboard_limits_exceeded');
    }
    const p=await ready(t,{client:'formats',host:'formats',policy:{...deny,filesToRemote:true}});
    p.client.json({type:'clipboard.begin',requestId:'bad',format:'tree',entries:1,files:0,bytes:0,tops:1,skipped:0});await p.host.take(v=>v.type==='clipboard.begin');
    p.client.json({type:'clipboard.entries',requestId:'bad',start:0,entries:[{path:'a',kind:'dir',attrs:0x10}]});
    assert.equal((await p.host.take(v=>v.type==='clipboard.error')).reason,'clipboard_invalid_message');
  });
});

test('the sliding window is bounded by messages and bytes and acknowledgements must name a sent unit',async t=>{
  const limits=clipboardLimitsFormats,size=2*1024*1024,big=Buffer.alloc(size,1);
  const begin=async()=>{const p=await ready(t,{client:'formats',host:'formats',policy:{...deny,filesToRemote:true}});p.client.json({type:'clipboard.begin',requestId:'w',format:'files',files:[{name:'big.bin',size}]});await p.host.take(v=>v.type==='clipboard.begin');return p;};
  await t.test('bytes in flight',async t=>{
    const p=await begin(),fit=Math.floor(limits.windowBytes/limits.chunkBytes);
    for(let i=0;i<fit;i++)p.client.json(chunkMsg('w',0,i*limits.chunkBytes,big.subarray(i*limits.chunkBytes,(i+1)*limits.chunkBytes)));
    await drain(p.host,fit,'clipboard.chunk');assert.equal(p.host.queue.some(v=>v.type==='clipboard.error'),false);
    p.client.json(chunkMsg('w',0,fit*limits.chunkBytes,big.subarray(fit*limits.chunkBytes,(fit+1)*limits.chunkBytes)));
    assert.equal((await p.host.take(v=>v.type==='clipboard.error')).reason,'clipboard_out_of_order');
  });
  await t.test('cumulative ack releases earlier chunks',async t=>{
    const p=await begin(),n=10;
    for(let i=0;i<n;i++)p.client.json(chunkMsg('w',0,i*limits.chunkBytes,big.subarray(i*limits.chunkBytes,(i+1)*limits.chunkBytes)));
    await drain(p.host,n,'clipboard.chunk');
    p.host.json({type:'clipboard.chunk.ack',requestId:'w',index:0,offset:5*limits.chunkBytes});await p.client.take(v=>v.type==='clipboard.chunk.ack');
    p.host.json({type:'clipboard.chunk.ack',requestId:'w',index:0,offset:5*limits.chunkBytes});
    assert.equal((await p.client.take(v=>v.type==='clipboard.error')).reason,'clipboard_out_of_order');
  });
  await t.test('an ack for data never sent',async t=>{
    const p=await begin();p.client.json(chunkMsg('w',0,0,big.subarray(0,limits.chunkBytes)));await p.host.take(v=>v.type==='clipboard.chunk');
    p.host.json({type:'clipboard.chunk.ack',requestId:'w',index:0,offset:limits.chunkBytes*2});
    assert.equal((await p.client.take(v=>v.type==='clipboard.error')).reason,'clipboard_out_of_order');
  });
  await t.test('more than 32 units in flight',async t=>{
    const p=await ready(t,{client:'formats',host:'formats',policy:{...deny,filesToRemote:true}});
    p.client.json({type:'clipboard.begin',requestId:'w',format:'files',files:Array.from({length:40},(_,i)=>({name:'f'+i,size:0}))});await p.host.take(v=>v.type==='clipboard.begin');
    for(let i=0;i<32;i++)p.client.json({type:'clipboard.item.end',requestId:'w',index:i,sha256:digest(Buffer.alloc(0))});
    await drain(p.host,32,'clipboard.item.end');assert.equal(p.host.queue.some(v=>v.type==='clipboard.error'),false);
    p.client.json({type:'clipboard.item.end',requestId:'w',index:32,sha256:digest(Buffer.alloc(0))});
    assert.equal((await p.host.take(v=>v.type==='clipboard.error')).reason,'clipboard_out_of_order');
  });
  await t.test('commit waits for every ack',async t=>{
    const p=await ready(t,{client:'formats',host:'formats',policy:{...deny,filesToRemote:true}});
    p.client.json({type:'clipboard.begin',requestId:'w',format:'files',files:[{name:'a',size:0}]});await p.host.take(v=>v.type==='clipboard.begin');
    p.client.json({type:'clipboard.item.end',requestId:'w',index:0,sha256:digest(Buffer.alloc(0))});await p.host.take(v=>v.type==='clipboard.item.end');
    p.client.json({type:'clipboard.commit',requestId:'w'});assert.equal((await p.host.take(v=>v.type==='clipboard.error')).reason,'clipboard_out_of_order');
  });
});

test('a pair that is not windowed stays stop-and-wait',async t=>{
  const p=await ready(t,{client:'limits',host:'limits',policy:{...deny,filesToRemote:true}});
  p.client.json({type:'clipboard.begin',requestId:'s',format:'files',files:[{name:'a',size:70000}]});await p.host.take(v=>v.type==='clipboard.begin');
  p.client.json(chunkMsg('s',0,0,Buffer.alloc(100)));await p.host.take(v=>v.type==='clipboard.chunk');
  p.client.json(chunkMsg('s',0,100,Buffer.alloc(100)));assert.equal((await p.host.take(v=>v.type==='clipboard.error')).reason,'clipboard_out_of_order');
});

test('cancelling a windowed transfer drops the messages already in flight instead of closing the pair, and a new transfer can follow',async t=>{
  const p=await ready(t,{client:'formats',host:'formats',policy:{...deny,filesToRemote:true}});
  const big=Buffer.alloc(300000,2);
  p.client.json({type:'clipboard.begin',requestId:'c1',format:'files',files:[{name:'big.bin',size:big.length}]});await p.host.take(v=>v.type==='clipboard.begin');
  p.client.json(chunkMsg('c1',0,0,big.subarray(0,46080)));await p.host.take(v=>v.type==='clipboard.chunk');
  p.host.json({type:'clipboard.cancel',requestId:'c1'});
  await p.client.take(v=>v.type==='clipboard.cancel');
  // Sent before the cancel reached the sender: dropped, not an error.
  p.client.json(chunkMsg('c1',0,46080,big.subarray(46080,92160)));p.client.json({type:'clipboard.item.end',requestId:'c1',index:0,sha256:'0'.repeat(64)});
  p.host.json({type:'clipboard.chunk.ack',requestId:'c1',index:0,offset:46080});
  const small=Buffer.from('after');
  p.client.json({type:'clipboard.begin',requestId:'c2',format:'files',files:[{name:'a.txt',size:small.length}]});
  assert.equal((await p.host.take(v=>v.type==='clipboard.begin')).requestId,'c2');
  assert.equal(p.host.queue.some(v=>v.type==='clipboard.error'||v.type==='clipboard.chunk'),false);
  await send(p.client,p.host,'c2',0,small,clipboardLimitsFormats);await commit(p.client,p.host,'c2');
});

test('the receiving direction is windowed too and a host-originated image honours a read request class',async t=>{
  const p=await ready(t,{client:'formats',host:'formats',policy:{...deny,filesToLocal:true}});
  p.client.json({type:'clipboard.read',requestId:'rd1',format:'files'});await p.host.take(v=>v.type==='clipboard.read');
  p.host.json({type:'clipboard.begin',requestId:'rd1',format:'image',items:[{kind:'png',size:3,width:1,height:1}]});
  assert.equal((await p.client.take(v=>v.type==='clipboard.begin')).format,'image');
  await send(p.host,p.client,'rd1',0,Buffer.from([1,2,3]),clipboardLimitsFormats);await commit(p.host,p.client,'rd1');
  const q=await ready(t,{client:'formats',host:'formats',policy:{...deny,filesToLocal:true,textToLocal:true}});
  q.client.json({type:'clipboard.read',requestId:'rd2',format:'text'});await q.host.take(v=>v.type==='clipboard.read');
  q.host.json({type:'clipboard.begin',requestId:'rd2',format:'image',items:[{kind:'png',size:3,width:1,height:1}]});
  assert.equal((await q.client.take(v=>v.type==='clipboard.error')).reason,'clipboard_invalid_message');
});
