import test from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { ClipboardRelay, clipboardSettledProtocol, clipboardIntentProtocol } from '../src/clipboard.mjs';

// Actual relay state machine, substituted peers and policy. No listener, token,
// clipboard, native input, database or real endpoint is opened by this fixture.
class Peer extends EventEmitter {
  readyState=1;bufferedAmount=0;sent=[];
  send(bytes,done){this.sent.push(JSON.parse(String(bytes)));done?.();}
  close(){if(this.readyState===3)return;this.readyState=3;this.emit('close');}
  terminate(){this.close();} ping(){}
  packet(value){this.emit('message',Buffer.from(JSON.stringify(value)),false);}
}
const all={textToRemote:true,textToLocal:true,filesToRemote:true,filesToLocal:true};
const id=n=>'a'.repeat(32)+'-'+BigInt(n).toString(16).padStart(16,'0');
function fixture(t,{client=true,host=true,intent=false,clientIntent=intent,hostIntent=intent,sync=true,onPaste=true,policy=all}={}){
  const session={id:'synthetic-session',userId:'synthetic-user',desktopId:'synthetic-vm',mode:'stream',clipboardEnabled:true,
    clipboardSync:sync,clipboardProtocol:sync?'clipboard-sync-v1':undefined,clipboardPolicy:policy,
    host:{capabilities:{clipboard:true,clipboardSync:true}},ws:{readyState:1}};
  const relay={store:{clipboardPolicy:()=>policy},current:()=>true,authorized:()=>true};
  const core=new ClipboardRelay(relay),c=new Peer(),h=new Peer();
  core.attach(c,session,'client',true,true,true,true,onPaste,client,clientIntent);
  core.attach(h,session,'host',true,true,true,true,onPaste,host,hostIntent);
  t.after(()=>core.close());return{core,c,h,pair:session.clipboardPair,session};
}
test('settled capability is exact and requires both peers, sync and on-paste',t=>{
  for(const options of [{},{client:false},{host:false},{sync:false},{onPaste:false}]){
    const f=fixture(t,options),expected=Object.keys(options).length===0;
    for(const p of [f.c,f.h]){const ready=p.sent.find(v=>v.type==='clipboard.ready');
      assert.equal(Object.hasOwn(ready,'clipboardSettled'),expected);
      if(expected)assert.equal(ready.clipboardSettled,clipboardSettledProtocol);
    }
  }
});
test('negotiated hint is forwarded as one source-bound object while legacy shapes stay exact',t=>{
  const f=fixture(t);f.c.packet({type:'clipboard.watch',sessionId:f.session.id,enabled:true});
  const hint={type:'clipboard.changed',sessionId:f.session.id,sequence:0xffffffff,settled:clipboardSettledProtocol};
  f.h.packet(hint);assert.deepEqual(f.c.sent.at(-1),hint);
  f.c.packet({type:'clipboard.read',requestId:id(1),expectedSequence:0xffffffff,textImageOnly:true});
  assert.deepEqual(f.h.sent.at(-1),{type:'clipboard.read',requestId:id(1),expectedSequence:0xffffffff,textImageOnly:true});
  assert.equal(f.pair.transfer.textImageOnly,true);
  const old=fixture(t,{host:false});old.c.packet({type:'clipboard.read',requestId:id(1)});
  assert.deepEqual(old.h.sent.at(-1),{type:'clipboard.read',requestId:id(1)});
  assert.equal(Object.hasOwn(old.c.sent[0],'clipboardSettled'),false);
});
test('unnegotiated, malformed and wrong-role source hints/reads close only their pair',t=>{
  for(const options of [{host:false},{client:false}]){
    const f=fixture(t,options);f.c.packet({type:'clipboard.watch',sessionId:f.session.id,enabled:true});
    f.h.packet({type:'clipboard.changed',sessionId:f.session.id,sequence:1,settled:clipboardSettledProtocol});
    assert.equal(f.pair.closed,true);
  }
  for(const fields of [{expectedSequence:1,textImageOnly:false},{expectedSequence:-1,textImageOnly:true},
    {expectedSequence:0x100000000,textImageOnly:true},{expectedSequence:1,textImageOnly:true,format:'files'}]){
    const f=fixture(t);f.c.packet({type:'clipboard.read',requestId:id(1),...fields});assert.equal(f.pair.closed,true);
  }
  const wrong=fixture(t);wrong.h.packet({type:'clipboard.read',requestId:id(1),expectedSequence:1,textImageOnly:true});assert.equal(wrong.pair.closed,true);
});
test('text-image admission cannot be answered with file/tree data before any paste',t=>{
  for(const response of [{type:'clipboard.begin',format:'files',files:[{name:'synthetic.bin',size:1}]},
    {type:'clipboard.offer',format:'tree',entries:1,files:1,bytes:1,tops:1,skipped:0}]){
    const f=fixture(t);f.c.packet({type:'clipboard.read',requestId:id(1),expectedSequence:41,textImageOnly:true});
    const before=f.c.sent.length;f.h.packet({requestId:id(1),...response});
    assert.equal(f.pair.closed,true);
    assert.equal(f.c.sent.slice(before).some(v=>v.type==='clipboard.begin'||v.type==='clipboard.offer'),false);
  }
});
test('settled read retains image binary permission and revocation checks',t=>{
  const f=fixture(t,{policy:{...all,filesToLocal:false}});
  f.c.packet({type:'clipboard.read',requestId:id(1),expectedSequence:1,textImageOnly:true});
  assert.equal(f.pair.closed,false);assert.equal(f.pair.transfer.textImageOnly,true);
  f.h.packet({type:'clipboard.begin',requestId:id(1),format:'image',items:[{kind:'png',size:1,width:1,height:1}]});
  assert.equal(f.pair.closed,true);
});
test('intent requires both explicit profiles and forwards metadata without any data read',t=>{
  for(const options of [{intent:true},{intent:true,host:false},{intent:true,sync:false},{intent:true,clientIntent:false},{intent:true,hostIntent:false}]){
    const f=fixture(t,options),enabled=options.host!==false&&options.sync!==false&&options.clientIntent!==false&&options.hostIntent!==false;
    assert.equal(f.c.sent[0].clipboardIntent,enabled?clipboardIntentProtocol:undefined);
    if(!enabled)continue;
    const watch={type:'clipboard.watch',sessionId:f.session.id,enabled:true,watchEpoch:1};
    f.c.packet(watch);assert.deepEqual(f.h.sent.at(-1),watch);
    const hint={type:'clipboard.intent',sessionId:f.session.id,sequence:0,kind:'text',watchEpoch:1};
    f.h.packet(hint);assert.deepEqual(f.c.sent.at(-1),hint);
    assert.equal(f.pair.transfer,null);
    assert.equal(f.h.sent.some(v=>v.type==='clipboard.read'||v.type==='clipboard.pull'),false);
    const n=f.c.sent.length;f.h.packet(hint);assert.equal(f.c.sent.length,n);
  }
});
test('intent watch generations reject stale disable/hints and bind reads to the live epoch',t=>{
  const f=fixture(t,{intent:true});
  f.c.packet({type:'clipboard.watch',sessionId:f.session.id,enabled:true,watchEpoch:1});
  f.c.packet({type:'clipboard.watch',sessionId:f.session.id,enabled:true,watchEpoch:2});
  f.c.packet({type:'clipboard.watch',sessionId:f.session.id,enabled:false,watchEpoch:1});
  const n=f.c.sent.length;
  f.h.packet({type:'clipboard.intent',sessionId:f.session.id,sequence:41,kind:'text',watchEpoch:1});
  assert.equal(f.c.sent.length,n);assert.equal(f.pair.watchEnabled,true);
  const hint={type:'clipboard.intent',sessionId:f.session.id,sequence:42,kind:'other',watchEpoch:2};
  f.h.packet(hint);assert.deepEqual(f.c.sent.at(-1),hint);
  const read={type:'clipboard.read',requestId:id(1),expectedSequence:42,textImageOnly:false,watchEpoch:2};
  f.c.packet(read);assert.deepEqual(f.h.sent.at(-1),read);
  assert.equal(f.pair.transfer.textImageOnly,false);assert.equal(f.pair.closed,false);
});
test('intent denies future epochs, unknown payload fields, wrong roles and stale bound reads',t=>{
  for(const mutate of [v=>({...v,watchEpoch:2}),v=>({...v,text:'private content'}),v=>({...v,kind:'files'})]){
    const f=fixture(t,{intent:true});
    f.c.packet({type:'clipboard.watch',sessionId:f.session.id,enabled:true,watchEpoch:1});
    f.h.packet(mutate({type:'clipboard.intent',sessionId:f.session.id,sequence:41,kind:'text',watchEpoch:1}));
    assert.equal(f.pair.closed,true);
  }
  const f=fixture(t,{intent:true});f.c.packet({type:'clipboard.watch',sessionId:f.session.id,enabled:true,watchEpoch:2});
  f.c.packet({type:'clipboard.read',requestId:id(1),expectedSequence:41,textImageOnly:true,watchEpoch:1});
  assert.equal(f.pair.closed,true);
  const role=fixture(t,{intent:true});role.c.packet({type:'clipboard.intent',sessionId:role.session.id,sequence:41,kind:'text',watchEpoch:1});
  assert.equal(role.pair.closed,true);
});
