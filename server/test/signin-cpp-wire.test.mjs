import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {createHash} from 'node:crypto';
import {parseSigninBinding,SigninServiceLease,SigninStreamPhase} from '../src/signin-stream.mjs';

// Unmodified stdout of the real compiled C++ production packet constructors.
// These are parser/policy tests; no native Windows proof is manufactured.
const raw=readFileSync(new URL('./fixtures/signin-cpp-wire-v1.json',import.meta.url));
const corpus=JSON.parse(raw), packets=corpus.packets;
test('native producer fixture is pinned byte-for-byte, not rewritten to JS schema',()=>{
 assert.equal(createHash('sha256').update(raw).digest('hex'),'0a6b62da36746d3414374bdfe251615384461805a6785f9b971d1fb66783fe22');
 assert.equal(corpus.nativeAdaptersInvoked,false);assert.equal(corpus.runtimeQualified,false);
});
test('actual C++ service authority packets survive strict Node parser and generation retirement',()=>{
 const host={}, first=packets.initialAuthority, next=packets.nextAuthority;
 let binding=first.binding;
 const lease=new SigninServiceLease({host,sessionId:first.sessionId,serviceInstance:first.serviceInstance,
  isCurrentHost:value=>value===host,authorized:()=>true,acceptsBinding:value=>JSON.stringify(value)===JSON.stringify(parseSigninBinding(binding)),now:()=>100});
 assert.deepEqual(parseSigninBinding(first.binding),first.binding);
 assert.equal(lease.observe(host,first),true);assert.equal(lease.current(first.binding),true);
 lease.retire(first.binding);assert.equal(lease.current(first.binding),false);
 binding=next.binding;assert.equal(lease.observe(host,next),true);assert.equal(lease.current(next.binding),true);
 assert.equal(lease.observe(host,first),false);
});
test('actual C++ desktop metadata retains strict envelope and waits for native proof plus decoder',()=>{
 const host={}, first=packets.initialAuthority, next=packets.nextAuthority;
 let proof=false;
 const phase=new SigninStreamPhase({sessionId:first.sessionId,hostBinding:host,binding:next.binding,
  authorized:()=>true,isCurrentHost:value=>value===host,isCurrentPhaseAuthority:()=>proof,
  validateInfo:info=>info.type==='stream.info'&&info.codec==='h264'&&info.width===1920&&info.height===1080,
  verifyHostFrame:()=>proof,validateInput:()=>false,now:()=>100});
 assert.equal(phase.hostInfo(packets.info,host),false);
 assert.equal(phase.state,'await_host_frame');
 proof=true;assert.equal(phase.hostInfo(packets.info,host),true);
 assert.equal(phase.state,'await_client_decode');
 assert.equal(phase.hostInfo({...packets.info,verified:true},host),false);
 assert.equal(phase.redirectsAllowed(),false);
});
