// NEW portable tests of actual shared controller + real view. No OS trust proof.
// In linkedom ONLY, Event.isTrusted is writable; setting it here exercises the
// production branch without adding any product trust predicate or touch hook.
import test from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import {createRequire} from 'node:module';
import {pathToFileURL} from 'node:url';
const require=createRequire(process.env.MFA_PRIVACY_NODE_PACKAGE?path.resolve(process.env.MFA_PRIVACY_NODE_PACKAGE):new URL('../package.json',import.meta.url));
const {parseHTML}=require('linkedom');
const source=process.env.MFA_PRIVACY_SOURCE_ROOT?pathToFileURL(path.resolve(process.env.MFA_PRIVACY_SOURCE_ROOT)+path.sep):new URL('../admin/js/',import.meta.url);
const {createAccountSecurityController}=await import(new URL('account-security-controller.js',source));
const {createAccountSecurityView}=await import(new URL('account-security-view.js',source));
const LIMIT=30*60*1000;
const codes=Array.from({length:10},(_,i)=>i.toString(16).padStart(32,'0'));
const deferred=()=>{let resolve;const promise=new Promise(r=>resolve=r);return{promise,resolve};};

class Clock {
 wall=1800000000000; mono=1000; serial=0; tasks=new Map();
 now=()=>this.wall; monotonic=()=>this.mono;
 schedule=(fn,ms)=>{const id=++this.serial;this.tasks.set(id,{fn,at:this.mono+Math.max(0,ms)});return id;};
 cancel=id=>this.tasks.delete(id);
 advance(ms,{fire=true}={}){this.wall+=ms;this.mono+=ms;if(fire)this.flush();}
 flush(){for(let turn=0;turn<100;turn++){const due=[...this.tasks].filter(([,task])=>task.at<=this.mono).sort((a,b)=>a[1].at-b[1].at);if(!due.length)return;const[id,task]=due[0];this.tasks.delete(id);task.fn();}throw Error('Fake timer did not converge');}
}
function fixture(t){
 const {document,window}=parseHTML('<html><body><button id="outside">Outside</button></body></html>'),oldDocument=globalThis.document;
 globalThis.document=document;Object.defineProperty(document,'hidden',{configurable:true,value:false});
 const clock=new Clock(),requests=[],retirements=[],views=[];let auth=true,revision=1,scope='https://privacy.fixture.invalid',held=null;
 let controller;
 const createView=options=>{const actual=createAccountSecurityView(options);actual.element.getBoundingClientRect=()=>({x:20,y:20,left:20,top:20,right:220,bottom:220,width:200,height:200});const record={...actual,models:[]};record.render=model=>{record.models.push(structuredClone(model));actual.render(model);};views.push(record);return record;};
 controller=createAccountSecurityController({document,createView,owner:()=>({revision}),isCurrent:owner=>auth&&owner?.revision===revision,origin:()=>scope,accountName:()=> 'Synthetic privacy account',now:clock.now,monotonic:clock.monotonic,schedule:clock.schedule,cancel:clock.cancel,
  request:async(route,method,data,{accept})=>{requests.push({route,method});let reply;if(route==='/mfa')reply={status:200,body:{protocol:'mfa-v1',enabled:true,recoveryCodesRemaining:10}};else if(route==='/mfa/reauth')reply={status:200,body:{ok:true}};else if(route==='/mfa/recovery')reply=held?await held.promise:{status:200,body:{enabled:true,recoveryCodes:[...codes],requiresLogin:true}};else throw Error('Unexpected synthetic request '+route);return accept(reply);},
  retireAuthentication:ticket=>{retirements.push(ticket);auth=false;revision++;controller.retire({preserveResult:ticket});},
 });
 const view=()=>views.at(-1),rows=()=>view()?.element.querySelectorAll('[data-recovery-code]').length??0;
 const emit=(type,{trusted=false,target=view().element,coords=[50,50]}={})=>{const event=new window.Event(type,{bubbles:true,cancelable:true});Object.defineProperty(event,'isTrusted',{value:trusted});if(['pointerdown','wheel'].includes(type)&&coords!==null){Object.defineProperties(event,{clientX:{value:coords[0]},clientY:{value:coords[1]}});}target.dispatchEvent(event);return event;};
 const lifecycle=type=>{if(type==='visibilitychange')document.dispatchEvent(new window.Event(type));else window.dispatchEvent(new window.Event(type));};
 const assertVisible=()=>{assert.equal(rows(),10);assert.equal(controller.hasCompletedResult(),true);assert.equal(auth,false);};
 const assertDisposed=()=>{assert.equal(rows(),0,'Actual real-view code nodes must be wiped/removed before querying the controller.');assert.equal(controller.hasCompletedResult(),false);assert.equal(auth,false);assert.equal(clock.tasks.size,0,'Owned privacy timer is canceled.');};
 async function result(){await controller.open();await controller.act({kind:'begin',intent:'recovery'});const pending=controller.act({kind:'submitProof',currentPassword:'Synthetic-password!',code:'123456'});if(!held)await pending;return pending;}
 t.after(()=>{controller.destroy();clock.tasks.clear();globalThis.document=oldDocument;});
 return {controller,clock,document,window,requests,retirements,view,emit,lifecycle,assertVisible,assertDisposed,result,
  hold(){held=deferred();return held;},release(){held.resolve({status:200,body:{enabled:true,recoveryCodes:[...codes],requiresLogin:true}});held=null;},
  retire(){auth=false;revision++;controller.retire();},successor(){auth=true;revision++;controller.retire();},changeOrigin(){scope='https://replacement.fixture.invalid';},replaceDocument(){globalThis.document=parseHTML('<html><body></body></html>').document;},
 };
}

test('privacy deadline begins only after the strict result ACK, not pending mutation',async t=>{
 const f=fixture(t);f.hold();const pending=f.result();await new Promise(resolve=>setImmediate(resolve));assert.equal(f.clock.tasks.size,0);assert.equal(f.retirements.length,0);
 f.clock.advance(2*LIMIT);f.release();await pending;f.assertVisible();assert.equal(f.clock.tasks.size,1);
 f.clock.advance(LIMIT-1);f.assertVisible();f.clock.advance(1);f.assertDisposed();assert.equal(f.requests.length,3);
});
test('unattended result drops all ten codes at exactly thirty minutes without any request',async t=>{
 const f=fixture(t);await f.result();f.assertVisible();f.clock.advance(LIMIT-1);f.assertVisible();f.clock.advance(1);f.assertDisposed();assert.equal(f.requests.length,3);
});
test('network actions, rerender, focus and lifecycle return inspect but never extend the result',async t=>{
 const f=fixture(t);await f.result();f.clock.advance(LIMIT-1000);const count=f.requests.length;
 await f.controller.act({kind:'refresh'});f.view().render(f.view().models.at(-1));f.emit('focus',{trusted:true});f.lifecycle('visibilitychange');f.lifecycle('pageshow');
 assert.equal(f.requests.length,count);f.clock.advance(1000);f.assertDisposed();
});
for(const type of ['pointerdown','keydown','wheel'])test(`only trusted inside ${type} renews an unexpired result`,async t=>{
 const f=fixture(t);await f.result();f.clock.advance(LIMIT-1000);f.emit(type,{trusted:true,target:f.view().element.querySelector('[data-recovery-code]')});
 f.clock.advance(1000);f.assertVisible();f.clock.advance(LIMIT-1000);f.assertDisposed();assert.equal(f.requests.length,3);
});
test('synthetic inside pointer/key/wheel and trusted pointermove do not renew privacy',async t=>{
 const f=fixture(t);await f.result();f.clock.advance(LIMIT-1);for(const type of ['pointerdown','keydown','wheel'])f.emit(type);f.emit('pointermove',{trusted:true});f.clock.advance(1);f.assertDisposed();
});
test('trusted input outside the exact result view never renews privacy',async t=>{
 const f=fixture(t);await f.result();f.clock.advance(LIMIT-1);for(const type of ['pointerdown','keydown','wheel'])f.emit(type,{trusted:true,target:f.document.getElementById('outside')});f.clock.advance(1);f.assertDisposed();
});
for(const type of ['pointerdown','keydown','wheel'])test(`expiry is checked BEFORE trusted ${type} can reset a throttled deadline`,async t=>{
 const f=fixture(t);await f.result();f.clock.advance(LIMIT,{fire:false});f.emit(type,{trusted:true});f.assertDisposed();assert.equal(f.requests.length,3);
});
test('wall-clock rollback before admission time immediately discards on lifecycle check',async t=>{
 const f=fixture(t);await f.result();f.clock.wall--;f.lifecycle('pageshow');f.assertDisposed();
});
test('partial wall rollback after a later observed time discards even while above original admission',async t=>{
 const f=fixture(t);await f.result();f.clock.advance(60000);f.lifecycle('visibilitychange');f.clock.wall--;f.lifecycle('pageshow');f.assertDisposed();
});
test('partial monotonic rollback after a later observed time discards even while above admission',async t=>{
 const f=fixture(t);await f.result();f.clock.advance(60000);f.lifecycle('visibilitychange');f.clock.mono--;f.lifecycle('pageshow');f.assertDisposed();
});
test('wall thirty-minute expiry wins even when monotonic time has barely advanced',async t=>{
 const f=fixture(t);await f.result();f.clock.wall+=LIMIT;f.clock.mono++;f.lifecycle('pageshow');f.assertDisposed();
});
test('monotonic thirty-minute expiry wins even when wall time barely advances',async t=>{
 const f=fixture(t);await f.result();f.clock.mono+=LIMIT;f.clock.wall++;f.lifecycle('pageshow');f.assertDisposed();
});
for(const how of ['account','close','destroy'])test(`${how} retirement immediately wipes result, cancels timer and grants no authority`,async t=>{
 const f=fixture(t);await f.result();const count=f.requests.length;
 if(how==='account')f.retire();else if(how==='close')f.emit('close');else f.controller.destroy();f.assertDisposed();assert.equal(f.requests.length,count);
});
for(const how of ['origin','document'])test(`changed ${how} cannot retain old result through lifecycle return`,async t=>{
 const f=fixture(t);await f.result();if(how==='origin')f.changeOrigin();else f.replaceDocument();f.lifecycle('pageshow');f.assertDisposed();
});
test('opaque result ticket never starts auth requests and a different ticket clears immediately',async t=>{
 const f=fixture(t);await f.result();for(const action of [{kind:'refresh'},{kind:'begin',intent:'setup'},{kind:'submitSetup',code:'123456'},{kind:'submitProof',currentPassword:'Synthetic-password!',code:'123456'}])await f.controller.act(action);
 assert.equal(f.requests.length,3);f.assertVisible();f.controller.retire({preserveResult:Object.freeze(Object.create(null))});f.assertDisposed();
});
test('repeated exact display-ticket preservation never renews the privacy deadline',async t=>{
 const f=fixture(t);await f.result();f.clock.advance(LIMIT-1);f.controller.retire({preserveResult:f.retirements[0]});f.clock.advance(1);f.assertDisposed();
});
test('queued callback and input from the retired view cannot clear or renew successor result',async t=>{
 const f=fixture(t);await f.result();assert.equal(f.clock.tasks.size,1);const oldCallback=[...f.clock.tasks.values()][0].fn,oldElement=f.view().element;
 f.clock.advance(1000);f.successor();await f.result();f.clock.advance(LIMIT-1000);oldCallback();f.emit('pointerdown',{trusted:true,target:oldElement});f.assertVisible();f.clock.advance(1000);f.assertDisposed();assert.equal(f.requests.length,6);
});

// Three new regressions for native modal-backdrop retargeting. Linkedom does
// not implement browser hit testing; target=dialog with outside coordinates is
// deliberately modeled from separate actual Edge/CDP evidence. Trust is still
// fabricated only in this test. Keyboard events intentionally have no coords.
for(const type of ['pointerdown','wheel'])test('retargeted dialog '+type+' with point outside its actual rectangle must not renew privacy',async t=>{
 const f=fixture(t);await f.result();f.clock.advance(LIMIT-1);
 f.emit(type,{trusted:true,target:f.view().element,coords:[4,4]});
 f.clock.advance(1);f.assertDisposed();assert.equal(f.requests.length,3);
});
test('missing or nonfinite pointer/wheel coordinates never renew result privacy',async t=>{
 const f=fixture(t);await f.result();f.clock.advance(LIMIT-1);
 for(const type of ['pointerdown','wheel'])for(const coords of [null,[undefined,50],[50,undefined],[NaN,50],[50,NaN],[Infinity,50],[50,-Infinity]])f.emit(type,{trusted:true,target:f.view().element,coords});
 f.clock.advance(1);f.assertDisposed();assert.equal(f.requests.length,3);
});
