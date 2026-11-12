import test from 'node:test';
import assert from 'node:assert/strict';
import {SigninEligibilityOffers,SigninStreamPhase,parseSigninBinding,signinStreamProtocol} from '../src/signin-stream.mjs';

const sessionId='12345678-1234-4234-8234-123456789abc';
const hex=n=>n.toString(16).padStart(32,'0');
const binding=(patch={})=>({protocol:signinStreamProtocol,phase:'signin',workerGeneration:1,transitionId:hex(1),mediaScope:{kind:'composite',epoch:1},...patch});
const facts=(patch={})=>({revision:1,qualifiedBackend:true,currentService:true,ownedDevice:true,windowsState:'locked',outputs:1,...patch});
function offerFixture(){let time=1000,serial=0;const host={},other={};let current=host;const policy=new SigninEligibilityOffers({isCurrentHost:value=>value===current,now:()=>time,newId:()=>hex(++serial)});return{policy,host,other,time:value=>time=value,current:value=>current=value};}
function phaseFixture(patch={}){
 let time=1000,authorized=true,current=true,nativeProof=true,previousExited=true;
 const host={},other={};const policy=new SigninStreamPhase({sessionId,hostBinding:host,binding:binding(),authorized:()=>authorized,isCurrentHost:h=>current&&h===host,isCurrentPhaseAuthority:()=>true,now:()=>time,
  validateInfo:(info,phase)=>info.codec==='h264'&&info.width===1280&&info.height===720&&(phase!=='signin'||info.audio?.available!==true),
  verifyHostFrame:(_host,_info,context)=>nativeProof&&(!context.previousWorkerExitRequired||previousExited),
  validateInput:input=>input.type==='input.reset'||input.type==='input.fence'||(input.type==='input'&&input.event==='key'&&typeof input.down==='boolean'),...patch});
 const message=(type,extra={},b=policy.binding)=>({type,protocol:signinStreamProtocol,sessionId,transitionId:b.transitionId,workerGeneration:b.workerGeneration,mediaScope:{...b.mediaScope},...extra});
 const info=(extra={})=>message('stream.phase.info',{phase:policy.binding.phase,firstFrameId:hex(10),info:{codec:'h264',width:1280,height:720,epoch:policy.binding.mediaScope.epoch,audio:{available:false}},...extra});
 const decoded=(extra={})=>message('stream.phase.decoded',{firstFrameId:hex(10),...extra});
 const input=(extra={})=>message('input.phase',{input:{type:'input',event:'key',down:true},...extra});
 const activate=()=>{assert.equal(policy.hostInfo(info(),host),true);assert.equal(policy.decoded(decoded()),true);};
 return{policy,host,other,message,info,decoded,input,activate,time:value=>time=value,authorized:value=>authorized=value,current:value=>current=value,native:value=>nativeProof=value,exited:value=>previousExited=value};
}

test('eligibility offer precedes assigned lease/baseline/pixels and is not input authority',()=>{
 const f=offerFixture(),offer=f.policy.observe(f.host,facts());assert.ok(offer);assert.equal(offer.protocol,signinStreamProtocol);assert.equal(f.policy.admit(f.host,offer.availabilityId),offer);
 assert.equal(Object.hasOwn(offer,'leaseActive'),false);assert.equal(Object.hasOwn(offer,'pixelsVerified'),false);
 const phase=phaseFixture();assert.equal(phase.policy.input(phase.input()),false);assert.equal(phase.policy.redirectsAllowed(),false);
});
for(const patch of[{qualifiedBackend:false},{currentService:false},{ownedDevice:false},{windowsState:'unlocked'},{windowsState:'uac'},{outputs:2},{revision:0},{revision:0x100000000},{extra:true}])test('eligibility rejects '+JSON.stringify(patch),()=>{const f=offerFixture();assert.equal(f.policy.observe(f.host,facts(patch)),null);});
test('offer receipt TTL is not renewed by duplicate proof, admission or delayed heartbeat',()=>{
 const f=offerFixture(),offer=f.policy.observe(f.host,facts());f.time(5999);assert.equal(f.policy.observe(f.host,facts()).expiresAtMs,6000);assert.ok(f.policy.admit(f.host,offer.availabilityId));f.time(6000);assert.equal(f.policy.admit(f.host,offer.availabilityId),null);assert.equal(f.policy.observe(f.host,facts()),null);assert.ok(f.policy.observe(f.host,facts({revision:2})));
});
test('withdrawal and invalid revision do not permit replay or counter wrap on the same Host',()=>{
 const f=offerFixture();f.policy.observe(f.host,facts({revision:0xffffffff}));f.policy.withdraw(f.host);assert.equal(f.policy.observe(f.host,facts()),null);assert.equal(f.policy.observe(f.host,facts({revision:0})),null);assert.equal(f.policy.observe(f.host,facts({revision:0xffffffff})),null);
});
test('offer cannot move across Host socket identity, revocation or a backward clock',()=>{
 const f=offerFixture(),offer=f.policy.observe(f.host,facts());assert.equal(f.policy.admit(f.other,offer.availabilityId),null);f.current(f.other);assert.equal(f.policy.admit(f.host,offer.availabilityId),null);assert.equal(f.policy.forgetDisconnected(f.other),false);assert.equal(f.policy.forgetDisconnected(f.host),true);const successor=f.policy.observe(f.other,facts());assert.ok(successor);assert.notEqual(successor.availabilityId,offer.availabilityId);assert.equal(f.policy.admit(f.other,offer.availabilityId),null);
 const clock=offerFixture(),original=clock.policy.observe(clock.host,facts());clock.time(999);assert.equal(clock.policy.admit(clock.host,original.availabilityId),null);clock.time(1001);assert.equal(clock.policy.observe(clock.host,facts()),null);assert.ok(clock.policy.observe(clock.host,facts({revision:2})));
});

test('Client grant binding is strict composite uint16 scope and separate uint32 worker generation',()=>{
 assert.deepEqual(parseSigninBinding(binding()),binding());assert.ok(parseSigninBinding(binding({workerGeneration:0xffffffff,mediaScope:{kind:'composite',epoch:65535}})));
 for(const value of[null,{},binding({workerGeneration:0}),binding({workerGeneration:1.5}),binding({workerGeneration:0x100000000}),binding({phase:'uac'}),binding({transitionId:'z'.repeat(32)}),binding({mediaScope:{kind:'composite',epoch:0}}),binding({mediaScope:{kind:'composite',epoch:65536}}),binding({mediaScope:{kind:'tracks',topologyEpoch:1,tracks:[{id:'0',epoch:1}]}}),{...binding(),verified:true}])assert.equal(parseSigninBinding(value),null);
});
test('assigned frame authority and scoped decoder ACK are both required before sign-in input',()=>{
 const f=phaseFixture();assert.equal(f.policy.decoded(f.decoded()),false);f.native(false);assert.equal(f.policy.hostInfo(f.info(),f.host),false);assert.equal(f.policy.input(f.input()),false);f.native(true);assert.equal(f.policy.hostInfo(f.info(),f.other),false);assert.equal(f.policy.hostInfo({...f.info(),verified:true},f.host),false);assert.equal(f.policy.hostInfo(f.info(),f.host),true);assert.equal(f.policy.input(f.input()),false);assert.equal(f.policy.decoded(f.decoded({firstFrameId:hex(11)})),false);assert.equal(f.policy.decoded(f.decoded()),true);assert.equal(f.policy.state,'signin_active');assert.equal(f.policy.input(f.input()),true);assert.equal(f.policy.redirectsAllowed(),false);
});
test('sign-in frame allows no audio or disabled audio but refuses audio capture',()=>{
 const noAudio=phaseFixture();const info=noAudio.info();delete info.info.audio;assert.equal(noAudio.policy.hostInfo(info,noAudio.host),true);
 const sound=phaseFixture();const loud=sound.info();loud.info.audio.available=true;assert.equal(sound.policy.hostInfo(loud,sound.host),false);
});
test('queued raw, wrong-session, old-generation and old-epoch input never gains a new binding',()=>{
 const f=phaseFixture();f.activate();assert.equal(f.policy.input({type:'input',event:'key',down:true}),false);
 for(const patch of[{sessionId:'22345678-1234-4234-8234-123456789abc'},{workerGeneration:2},{transitionId:hex(2)},{mediaScope:{kind:'composite',epoch:2}},{input:{type:'udp.input',bytes:[1]}},{input:{type:'input',event:'key',down:'yes'}},{untrusted:true}])assert.equal(f.policy.input(f.input(patch)),false);
 for(const type of['input.reset','input.fence'])assert.equal(f.policy.input(f.input({input:{type}})),true);
});
test('same logical session role handoff blocks input and redirects until exact previous exit and new decode',()=>{
 const f=phaseFixture();f.activate();const old=f.input();const prepare=f.message('stream.phase.prepare',{phase:'desktop',timeoutMs:90000,workerGeneration:2,transitionId:hex(2),mediaScope:{kind:'composite',epoch:2}});
 assert.equal(f.policy.prepare(prepare,f.host),true);assert.equal(f.policy.input(old),false);assert.equal(f.policy.redirectsAllowed(),false);f.exited(false);assert.equal(f.policy.hostInfo(f.info(),f.host),false);f.exited(true);assert.equal(f.policy.hostInfo(f.info(),f.host),true);assert.equal(f.policy.decoded(f.decoded()),true);assert.equal(f.policy.state,'desktop_active');assert.equal(f.policy.redirectsAllowed(),true);assert.equal(f.policy.input(old),false);assert.equal(f.policy.input(f.input()),true);
});
test('duplicate preparation/info does not extend caller-owned bounded deadline',()=>{
 const f=phaseFixture();f.activate();const next=f.message('stream.phase.prepare',{phase:'desktop',timeoutMs:90000,workerGeneration:2,transitionId:hex(2),mediaScope:{kind:'composite',epoch:2}});f.policy.prepare(next,f.host);const deadline=f.policy.deadlineMs;f.time(30000);assert.equal(f.policy.prepare({...next,timeoutMs:90000},f.host),true);assert.equal(f.policy.deadlineMs,deadline);assert.equal(f.policy.hostInfo(f.info(),f.host),true);f.time(60000);assert.equal(f.policy.hostInfo(f.info(),f.host),true);assert.equal(f.policy.deadlineMs,deadline);f.time(deadline);assert.equal(f.policy.decoded(f.decoded()),false);assert.equal(f.policy.state,'expired');
});
test('duplicate info cannot substitute decoder metadata or mutate the captured first-frame binding',()=>{
 const f=phaseFixture(),info=f.info();assert.equal(f.policy.hostInfo(info,f.host),true);info.info.audio.available=true;assert.equal(f.policy.hostInfo(info,f.host),false);assert.equal(f.policy.hostInfo(f.info(),f.host),true);const changed=f.info();changed.info.audio.sampleRate=48000;assert.equal(f.policy.hostInfo(changed,f.host),false);assert.equal(f.policy.decoded(f.decoded()),true);
});
test('initial decoder budget remains ninety seconds without a new short preparation timer',()=>{
 const f=phaseFixture();f.policy.hostInfo(f.info(),f.host);f.time(90999);assert.equal(f.policy.decoded(f.decoded()),true);
});
test('viewport epoch advances without worker change and fences input until fresh scoped decode',()=>{
 const f=phaseFixture({binding:binding({phase:'desktop'})});f.activate();const old=f.input(),worker=f.policy.binding.workerGeneration;assert.equal(f.policy.prepareResize({kind:'composite',epoch:2}),true);assert.equal(f.policy.binding.workerGeneration,worker);assert.equal(f.policy.input(old),false);assert.equal(f.policy.redirectsAllowed(),false);f.exited(false);assert.equal(f.policy.hostInfo(f.info(),f.host),true,'same worker resize requires no worker exit');assert.equal(f.policy.decoded(f.decoded()),true);assert.equal(f.policy.input(old),false);assert.equal(f.policy.input(f.input()),true);
});
test('wrong next role, skipped generation, non-next epoch, reused operation and wrap are refused',()=>{
 const f=phaseFixture();f.activate();const next=f.message('stream.phase.prepare',{phase:'desktop',timeoutMs:90000,workerGeneration:2,transitionId:hex(2),mediaScope:{kind:'composite',epoch:2}});
 for(const patch of[{phase:'signin'},{workerGeneration:3},{mediaScope:{kind:'composite',epoch:3}},{transitionId:hex(1)},{timeoutMs:0},{timeoutMs:90001}])assert.equal(f.policy.prepare({...next,...patch},f.host),false);
 const max=phaseFixture({binding:binding({workerGeneration:0xffffffff,mediaScope:{kind:'composite',epoch:65535}})});max.activate();assert.equal(max.policy.prepare(max.message('stream.phase.prepare',{phase:'desktop',timeoutMs:90000,workerGeneration:0x100000000,transitionId:hex(2),mediaScope:{kind:'composite',epoch:65536}}),max.host),false);assert.equal(max.policy.prepareResize({kind:'composite',epoch:65536}),false);
});
for(const kind of['authorization','Host socket','clock'])test('active phase is invalidated after '+kind+' loss',()=>{const f=phaseFixture({binding:binding({phase:'desktop'})});f.activate();if(kind==='authorization')f.authorized(false);if(kind==='Host socket')f.current(false);if(kind==='clock')f.time(999);assert.equal(f.policy.input(f.input()),false);assert.equal(f.policy.redirectsAllowed(),false);assert.equal(f.policy.state,'invalidated');});
test('closed phase cannot resurrect through late info/decoded/input',()=>{const f=phaseFixture();f.activate();f.policy.close();assert.equal(f.policy.hostInfo(f.info(),f.host),false);assert.equal(f.policy.decoded(f.decoded()),false);assert.equal(f.policy.input(f.input()),false);});

test('phase lifetime authority callback is mandatory',()=>{
 assert.throws(()=>phaseFixture({isCurrentPhaseAuthority:undefined}),/invalid_signin_phase_context/);
});
for(const outcome of[false,'throw'])test('initial info authority '+outcome+' permanently invalidates the phase',()=>{
 let current=outcome;const f=phaseFixture({isCurrentPhaseAuthority:()=>{if(current==='throw')throw Error('fixture');return current;}});
 assert.equal(f.policy.state,'await_host_frame','waiting has no native-lease prerequisite');assert.equal(f.policy.hostInfo(f.info(),f.host),false);assert.equal(f.policy.state,'invalidated');current=true;assert.equal(f.policy.hostInfo(f.info(),f.host),false);
});
test('native lease loss after info blocks queued decoded ACK despite a current Host socket',()=>{
 let current=true;const f=phaseFixture({isCurrentPhaseAuthority:()=>current});assert.equal(f.policy.hostInfo(f.info(),f.host),true);current=false;assert.equal(f.policy.decoded(f.decoded()),false);assert.equal(f.policy.state,'invalidated');current=true;assert.equal(f.policy.decoded(f.decoded()),false);assert.equal(f.policy.input(f.input()),false);
});
for(const action of['input','redirects','duplicate-info'])test('fresh native authority guards '+action+' with no cached positive proof',()=>{
 let current=true;const f=phaseFixture({binding:binding({phase:'desktop'}),isCurrentPhaseAuthority:()=>current});
 if(action==='duplicate-info')assert.equal(f.policy.hostInfo(f.info(),f.host),true);else f.activate();current=false;
 assert.equal(action==='input'?f.policy.input(f.input()):action==='redirects'?f.policy.redirectsAllowed():f.policy.hostInfo(f.info(),f.host),false);assert.equal(f.policy.state,'invalidated');current=true;assert.equal(f.policy.redirectsAllowed(),false);assert.equal(f.policy.input(f.input()),false);
});
test('a throwing native authority after activation also invalidates redirects and input',()=>{
 let throws=false;const f=phaseFixture({binding:binding({phase:'desktop'}),isCurrentPhaseAuthority:()=>{if(throws)throw Error('fixture');return true;}});f.activate();throws=true;assert.equal(f.policy.redirectsAllowed(),false);assert.equal(f.policy.input(f.input()),false);assert.equal(f.policy.state,'invalidated');
});
test('role prepare stays lease-free and later authority receives the exact assigned new binding',()=>{
 let current=false;const observed=[];const f=phaseFixture({isCurrentPhaseAuthority:(host,binding,id)=>{observed.push({host,binding,id});return current;}});
 assert.equal(observed.length,0);assert.equal(f.policy.state,'await_host_frame');current=true;f.activate();current=false;const before=observed.length;
 const next=f.message('stream.phase.prepare',{phase:'desktop',timeoutMs:90000,workerGeneration:2,transitionId:hex(2),mediaScope:{kind:'composite',epoch:2}});assert.equal(f.policy.prepare(next,f.host),true);assert.equal(observed.length,before);assert.equal(f.policy.state,'await_host_frame');
 current=true;assert.equal(f.policy.hostInfo(f.info(),f.host),true);assert.equal(f.policy.decoded(f.decoded()),true);assert.equal(f.policy.input(f.input()),true);assert.equal(f.policy.redirectsAllowed(),true);
 for(const proof of observed.slice(before)){assert.equal(proof.host,f.host);assert.equal(proof.id,sessionId);assert.deepEqual(proof.binding,f.policy.binding);assert.equal(proof.binding.workerGeneration,2);assert.equal(proof.binding.mediaScope.epoch,2);}
});
test('same-worker resize preparation never waits for a nonexistent future native lease',()=>{
 let current=true,calls=0;const f=phaseFixture({binding:binding({phase:'desktop'}),isCurrentPhaseAuthority:()=>{calls++;return current;}});f.activate();current=false;const before=calls;assert.equal(f.policy.prepareResize({kind:'composite',epoch:2}),true);assert.equal(calls,before);current=true;assert.equal(f.policy.hostInfo(f.info(),f.host),true);assert.equal(f.policy.decoded(f.decoded()),true);
});
