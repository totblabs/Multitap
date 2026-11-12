import test from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { TakeoverContexts, existingLoginView, requestedSessionContext } from '../src/session-context.mjs';

const actor='actor',auth='a'.repeat(64),target={id:'pve-170',name:'Actual VM'};
function fixture(options={}){
 let clock=1700000000000;
 const api=new TakeoverContexts({now:()=>clock,...options});
 const session={id:'session-original',userId:actor,tokenHash:auth,desktopId:target.id,createdAt:clock};
 const issue=(sessions=[session],userId=actor,tokenHash=auth)=>api.issue({userId,tokenHash,desktop:target,sessions,usernameForSession:()=> 'actual-user'});
 const consume=(offer,sessions=[session])=>api.consume({context:offer.context,userId:actor,tokenHash:auth,desktopId:target.id,sessions});
 return{api,session,issue,consume,advance:ms=>{clock+=ms;}};
}

test('context rejects a successor object even when every public identifier is reused',()=>{
 const f=fixture(),offer=f.issue(),successor={...f.session};
 assert.throws(()=>f.consume(offer,[successor]),{code:'takeover_context_stale'});
 assert.throws(()=>f.consume(offer),{code:'takeover_context_stale'},'failed consent cannot be replayed');
});

test('same object with changed account, token or desktop is not the reviewed occupant',()=>{
 for(const [key,value]of [['userId','successor'],['tokenHash','b'.repeat(64)],['desktopId','pve-171']]){
  const f=fixture(),offer=f.issue();f.session[key]=value;
  assert.throws(()=>f.consume(offer),{code:'takeover_context_stale'});
 }
});

test('TTL has an exact boundary and ordinary current-session progress does not change identity',()=>{
 const f=fixture(),offer=f.issue();f.session.streamReady=true;f.session.nextEpoch=12;f.advance(119999);assert.doesNotThrow(()=>f.consume(offer));
 const next=f.issue();f.advance(120000);assert.throws(()=>f.consume(next),{code:'takeover_context_expired'});
});

test('per-account offer bound retires only the oldest consent, leaving latest reviews usable',()=>{
 const f=fixture(),offers=Array.from({length:5},()=>f.issue());
 assert.throws(()=>f.consume(offers[0]),{code:'takeover_context_stale'});
 for(const offer of offers.slice(1))assert.doesNotThrow(()=>f.consume(offer));
});

test('global bound is strict and expired consent frees capacity without a timer',()=>{
 const f=fixture();for(let i=0;i<1024;i++)f.issue([f.session],'user-'+i);
 assert.throws(()=>f.issue([f.session],'user-overflow'),{code:'session_context_unavailable'});
 f.advance(120000);assert.doesNotThrow(()=>f.issue([f.session],'user-new'));
});

test('random collision or wrong entropy never replaces an existing authorization',()=>{
 const fixed=Buffer.alloc(32,4),f=fixture({randomBytes:()=>fixed}),offer=f.issue();
 assert.throws(()=>f.issue(),{code:'session_context_unavailable'});assert.doesNotThrow(()=>f.consume(offer));
 assert.throws(()=>fixture({randomBytes:()=>randomBytes(16)}).issue(),{code:'session_context_unavailable'});
});

test('login summaries expose only actual safe keys and at most five total desktop identities',()=>{
 const rows=Array.from({length:5},(_,i)=>({issuer:i===0?'browser-console':'windows-pc-v1',createdAt:1700000000000+i,tokenHash:String(i).repeat(64)}));
 const view=existingLoginView({username:'actual-user'},rows,()=>Array.from({length:7},(_,i)=>({id:'pve-'+i,name:'Actual '+i,secret:'not exported'})));
 assert.deepEqual(Object.keys(view).sort(),['sessionCount','sessions','username']);assert.equal(view.sessionCount,5);
 assert.equal(view.sessions.flatMap(s=>s.desktops).length,5);assert.equal(view.sessions[0].issuer,'browser-console');
 for(const row of view.sessions)assert.deepEqual(Object.keys(row).sort(),['createdAt','desktops','issuer']);
 assert.equal(JSON.stringify(view).includes('secret'),false);assert.equal(JSON.stringify(view).includes('tokenHash'),false);
});

test('unsafe display labels are not echoed and Unicode bounds follow actual code points',()=>{
 assert.throws(()=>existingLoginView({username:'bad\u202ename'},[{issuer:'legacy',createdAt:1}],()=>[]),{code:'session_context_unavailable'});
 assert.throws(()=>existingLoginView({username:'real-user'},[{issuer:'legacy',createdAt:1}],()=>[{id:'pve-1',name:'bad\u0085name'}]),{code:'session_context_unavailable'});
 const name='\u{10400}'.repeat(64);assert.equal(existingLoginView({username:name},[{issuer:'legacy',createdAt:1}],()=>[]).username,name);
});

test('unknown, duplicated and array-valued protocol headers are rejected before new context use',()=>{
 const req=value=>({headers:{'x-multitap-takeover-context':value},rawHeaders:['X-Multitap-Takeover-Context',value]});
 assert.equal(requestedSessionContext({headers:{},rawHeaders:[]},'x-multitap-takeover-context'),false);
 assert.equal(requestedSessionContext(req('v1'),'x-multitap-takeover-context'),true);
 for(const value of ['v2',['v1'],'v1, v1'])assert.throws(()=>requestedSessionContext(req(value),'x-multitap-takeover-context'),{code:'invalid_context_protocol'});
 const duplicate=req('v1');duplicate.rawHeaders.push('x-multitap-takeover-context','v1');assert.throws(()=>requestedSessionContext(duplicate,'x-multitap-takeover-context'),{code:'invalid_context_protocol'});
});

test('Session context labels reject the exact directional, line-separator and UTF16 forms refused by both clients',()=>{
 const unsafe=['\u061c','\u200e','\u200f','\u2028','\u2029','\ud800','\udc00','\ud800\ud800','\udc00\ud800'];
 for(const middle of unsafe){
  const name='actual'+middle+'name';
  assert.throws(()=>existingLoginView({username:'actual-user'},[{issuer:'legacy',createdAt:1}],()=>[{id:target.id,name}]),{code:'session_context_unavailable'});
  const f=fixture();assert.throws(()=>f.api.issue({userId:actor,tokenHash:auth,desktop:{id:target.id,name},sessions:[f.session],usernameForSession:()=> 'actual-user'}),{code:'session_context_unavailable'});
 }
 const valid='Actual\u{10400}Name';assert.equal(existingLoginView({username:'actual-user'},[{issuer:'legacy',createdAt:1}],()=>[{id:target.id,name:valid}]).sessions[0].desktops[0].name,valid);
});
