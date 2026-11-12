import {test} from 'node:test';
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {parseHTML} from 'linkedom';

const appUrl=new URL('../admin/app.js',import.meta.url),response=(body,status=200)=>({ok:status>=200&&status<300,status,json:async()=>body});
const deferred=()=>{let resolve,reject;const promise=new Promise((a,b)=>{resolve=a;reject=b;});return{promise,resolve,reject};};
const csrf='c'.repeat(43),lateCsrf='l'.repeat(43),challenge='t'.repeat(43),secret='A'.repeat(32);
const codes=Array.from({length:10},(_,i)=>i.toString(16).padStart(32,'0'));
const user={id:'admin-fixture',username:'fixture-admin',role:'admin',mustChangeCredentials:false};
const inventory={users:[],desktops:[],assignments:[],audit:[],proxmox:{configured:false},onboarding:{complete:true,proxmoxRequired:false}};
const candidate=()=>({protocol:'mfa-v1',setupToken:challenge,secret,expiresAt:new Date(Date.now()+60000).toISOString(),uri:`otpauth://totp/Multitap%3Afixture?secret=${secret}&issuer=Multitap&algorithm=SHA1&digits=6&period=30`});
let serial=0;
async function fixture(t,{authenticated=true}={}){
  const window=parseHTML(await readFile(new URL('../admin/index.html',import.meta.url),'utf8'));
  const keys=['document','Node','location','fetch','FormData','sessionStorage','matchMedia','addEventListener'];
  const previous=Object.fromEntries(keys.map(key=>[key,globalThis[key]]));
  globalThis.document=window.document;globalThis.Node=window.Node;globalThis.location=new URL('https://admin.fixture.invalid/admin/');
  const listeners=new Map();globalThis.addEventListener=(name,fn)=>{const entries=listeners.get(name)??[];entries.push(fn);listeners.set(name,entries);};
  globalThis.sessionStorage={getItem:()=>null,setItem(){throw Error('MFA test must not persist secret data');}};
  globalThis.matchMedia=()=>({matches:false,addEventListener(){}});
  const formProto=Object.getPrototypeOf(document.querySelector('form'));
  formProto.reset=function(){for(const input of this.querySelectorAll('input,textarea'))input.value='';};
  Object.defineProperty(formProto,'elements',{configurable:true,get(){return Object.fromEntries([...this.querySelectorAll('[name]')].map(input=>[input.name,input]));}});
  globalThis.FormData=class{constructor(form){this.values=[...form.querySelectorAll('[name]')].map(input=>[input.name,input.value]);}*[Symbol.iterator](){yield* this.values;}};
  const modal=document.querySelector('#modal');modal.showModal=()=>{modal.open=true;};modal.close=()=>{modal.open=false;};
  const requests=[],queues=new Map();let enabled=false;
  const queue=(path,value)=>{const entries=queues.get(path)??[];entries.push(value);queues.set(path,entries);};
  globalThis.fetch=async(url,options={})=>{
    const path=url.replace('/api/v1/admin','');requests.push({path,method:options.method??'GET',headers:{...options.headers},data:options.body?JSON.parse(options.body):undefined});
    if(queues.get(path)?.length){const next=queues.get(path).shift();return typeof next==='function'?next(options):await next;}
    if(url==='/healthz')return response({service:'multitap-broker',version:'1.0.20'});
    if(url==='/api/v1/downloads')return response({releases:[]});
    if(path==='/session')return authenticated?response({user,csrfToken:csrf}):response({error:{code:'authentication_required'}},401);
    if(path==='/inventory')return response(inventory);
    if(path==='/server-info')return response({service:'multitap-broker',version:'1.0.20',deployment:{kind:'unknown',configured:false},update:{state:'not_configured'}});
    if(path==='/proxmox/status')return response({proxmox:inventory.proxmox});
    if(path==='/mfa')return response({protocol:'mfa-v1',enabled,recoveryCodesRemaining:enabled?10:0});
    if(path==='/mfa/setup')return response(candidate(),201);
    if(path==='/mfa/reauth')return response({ok:true});
    if(path==='/mfa/enable'||path==='/mfa/recovery'){enabled=true;return response({enabled:true,recoveryCodes:codes,requiresLogin:true});}
    if(path==='/mfa/disable'){enabled=false;return response({enabled:false,requiresLogin:true});}
    if(path==='/logout')return response({ok:true});
    return response({sessions:[],hosts:[],events:[],requests:[],pendingCount:0});
  };
  const source=(await readFile(appUrl,'utf8')).replace(/from '(\.[^']+)'/g,(_,name)=>`from '${new URL(name,appUrl).href}'`);
  const {qa}=await import('data:text/javascript;base64,'+Buffer.from(source+`\nexport const qa={state,api,showLogin,enter,security:accountSecurityController};\n//# sourceURL=admin-mfa-app-fixture-${++serial}.js`).toString('base64'));
  const settle=async()=>{for(let i=0;i<8;i++)await new Promise(r=>setImmediate(r));};
  t.after(()=>{qa.showLogin();qa.security.destroy();for(const [key,value]of Object.entries(previous))globalThis[key]=value;});
  await settle();
  return{window,qa,requests,queue,settle,node:selector=>document.querySelector(selector),emit:name=>{for(const fn of listeners.get(name)??[])fn(new window.Event(name));}};
}
async function candidateFlow(f){f.node('#account-security').dispatchEvent(new f.window.Event('click'));await f.settle();await f.qa.security.act({kind:'begin',intent:'setup'});await f.qa.security.act({kind:'submitProof',currentPassword:'Synthetic-browser-password!'});}

test('actual anonymous login replacement keeps its own submit lock after an older request finishes',async t=>{
  const f=await fixture(t,{authenticated:false}),first=deferred(),second=deferred();f.queue('/login',first.promise);f.queue('/login',second.promise);
  const form=f.node('#login-form'),submit=form.querySelector('button');
  for(let i=0;i<2;i++){form.elements.username.value='fixture-admin';form.elements.password.value='Synthetic-browser-password!';form.dispatchEvent(new f.window.Event('submit',{cancelable:true}));}
  first.resolve(response({error:{code:'invalid_credentials'}},401));await f.settle();assert.equal(submit.disabled,true);assert.equal(f.node('#login-error').textContent,'');
  second.resolve(response({error:{code:'invalid_credentials'}},401));await f.settle();assert.equal(submit.disabled,false);
});

test('actual challenge rejects wall rollback locally and wipes the pending authentication form',async t=>{
  const f=await fixture(t,{authenticated:false}),realNow=Date.now;let wall=realNow();
  try{
    Date.now=()=>wall;
    f.queue('/login',response({mfa:{protocol:'mfa-v1',challengeToken:challenge,expiresAt:new Date(wall+60000).toISOString(),methods:['totp','recovery']}},202));
    const form=f.node('#login-form');form.elements.username.value='fixture-admin';form.elements.password.value='Synthetic-browser-password!';form.dispatchEvent(new f.window.Event('submit',{cancelable:true}));await f.settle();
    wall-=600000;const mfa=f.node('#admin-mfa-form');mfa.elements.code.value='123456';mfa.dispatchEvent(new f.window.Event('submit',{cancelable:true}));
  }finally{Date.now=realNow;}
  await f.settle();assert.equal(f.requests.some(row=>row.path==='/mfa/verify'),false);assert.equal(f.node('#admin-mfa').hidden,true);assert.equal(f.qa.state.user,null);assert.equal(f.node('#admin-mfa-form').elements.code.value,'');
});

test('actual admin form negotiates MFA202 without authenticated state then submits normalized grouped recovery',async t=>{
  const f=await fixture(t,{authenticated:false});
  f.queue('/login',response({mfa:{protocol:'mfa-v1',challengeToken:challenge,expiresAt:new Date(Date.now()+60000).toISOString(),methods:['totp','recovery']}},202));
  const form=f.node('#login-form');form.elements.username.value='fixture-admin';form.elements.password.value='Synthetic-browser-password!';form.dispatchEvent(new f.window.Event('submit',{cancelable:true}));await f.settle();
  assert.equal(f.qa.state.user,null);assert.equal(f.qa.state.csrf,'');assert.equal(f.node('#admin-mfa').hidden,false);assert.equal(form.elements.password.value,'');
  assert.equal(f.requests.find(row=>row.path==='/login').headers['X-Multitap-Auth-Protocol'],'mfa-v1');
  f.node('#admin-mfa-mode').dispatchEvent(new f.window.Event('click'));const mfa=f.node('#admin-mfa-form');const raw='ABCDEF0123456789ABCDEF0123456789';mfa.elements.recoveryCode.value=raw.match(/.{4}/g).join('-');
  f.queue('/mfa/verify',response({user,csrfToken:csrf}));mfa.dispatchEvent(new f.window.Event('submit',{cancelable:true}));await f.settle();
  const verify=f.requests.find(row=>row.path==='/mfa/verify');assert.deepEqual(verify.data,{challengeToken:challenge,recoveryCode:raw.toLowerCase()});assert.equal(f.qa.state.user.id,user.id);assert.equal(f.node('#console').hidden,false);
});
test('cancel during actual MFA verification cannot restore UI and cleanup uses only the late response CSRF',async t=>{
  const f=await fixture(t,{authenticated:false});f.queue('/login',response({mfa:{protocol:'mfa-v1',challengeToken:challenge,expiresAt:new Date(Date.now()+60000).toISOString(),methods:['totp','recovery']}},202));
  const login=f.node('#login-form');login.elements.username.value='fixture-admin';login.elements.password.value='Synthetic-browser-password!';login.dispatchEvent(new f.window.Event('submit',{cancelable:true}));await f.settle();
  const held=deferred();f.queue('/mfa/verify',held.promise);const mfa=f.node('#admin-mfa-form');mfa.elements.code.value='123456';mfa.dispatchEvent(new f.window.Event('submit',{cancelable:true}));await f.settle();assert.equal(f.node('#admin-mfa-cancel').disabled,false);
  f.node('#admin-mfa-cancel').dispatchEvent(new f.window.Event('click'));held.resolve(response({user,csrfToken:lateCsrf}));await f.settle();
  assert.equal(f.qa.state.user,null);assert.equal(f.node('#login').hidden,false);assert.equal(f.node('#admin-mfa').hidden,true);assert.equal(mfa.elements.code.value,'');
  const cleanups=f.requests.filter(row=>row.path==='/logout');assert.equal(cleanups.length,1);assert.equal(cleanups[0].headers['X-CSRF-Token'],lateCsrf);
});
test('actual management ACK resets user/CSRF while keeping only its once-only result through an old401',async t=>{
  const f=await fixture(t),late=deferred();await candidateFlow(f);f.queue('/late-old-request',late.promise);const old=f.qa.api('/late-old-request').catch(error=>error);
  await f.qa.security.act({kind:'submitSetup',code:'123456'});assert.equal(f.qa.state.user,null);assert.equal(f.qa.state.csrf,'');assert.equal(f.node('#login').hidden,false);assert.equal(f.qa.security.hasCompletedResult(),true);assert.equal(document.querySelectorAll('[data-recovery-code]').length,10);
  late.resolve(response({error:{code:'authentication_required'}},401));const retired=await old;assert.equal(retired.code,'admin_session_changed');assert.equal(f.qa.security.hasCompletedResult(),true);assert.equal(document.querySelectorAll('[data-recovery-code]').length,10);
  assert.equal(f.requests.filter(row=>row.path==='/logout').length,0,'valid ACK already revoked; no invented extra logout');
});
test('current401 before ACK cancels real view and never resurrects a late code list',async t=>{
  const f=await fixture(t),held=deferred();await candidateFlow(f);f.queue('/mfa/enable',held.promise);const pending=f.qa.security.act({kind:'submitSetup',code:'123456'});f.queue('/external401',response({error:{code:'authentication_required'}},401));await assert.rejects(f.qa.api('/external401'),e=>e.status===401);
  held.resolve(response({enabled:true,recoveryCodes:codes,requiresLogin:true}));await pending;assert.equal(f.qa.state.user,null);assert.equal(f.qa.security.hasCompletedResult(),false);assert.equal(document.querySelectorAll('[data-recovery-code]').length,0);
});
test('manual signout intent before ACK cancels result even while server logout is held',async t=>{
  const f=await fixture(t),enable=deferred(),logout=deferred();await candidateFlow(f);f.queue('/mfa/enable',enable.promise);const pending=f.qa.security.act({kind:'submitSetup',code:'123456'});f.queue('/logout',logout.promise);f.node('#logout').dispatchEvent(new f.window.Event('click'));enable.resolve(response({enabled:true,recoveryCodes:codes,requiresLogin:true}));await pending;assert.equal(f.qa.security.hasCompletedResult(),false);assert.equal(document.querySelectorAll('[data-recovery-code]').length,0);logout.resolve(response({ok:true}));await f.settle();assert.equal(f.qa.state.user,null);
});
test('uncertain mutation resets real auth and attempts old-CSRF cleanup without touching current-state credentials',async t=>{
  const f=await fixture(t);await candidateFlow(f);f.queue('/mfa/enable',()=>{throw Error('synthetic-network-failure');});await f.qa.security.act({kind:'submitSetup',code:'123456'});await f.settle();
  assert.equal(f.qa.state.user,null);assert.equal(f.qa.state.csrf,'');assert.equal(document.querySelectorAll('[data-recovery-code]').length,0);assert.equal(f.qa.security.hasCompletedResult(),true);assert.match(document.querySelector('.account-security-dialog').textContent,/다시 로그인/);
  const cleanup=f.requests.find(row=>row.path==='/logout');assert.equal(cleanup.headers['X-CSRF-Token'],csrf);
});
test('new login and page privacy disposal erase completed result rather than carry it to another owner',async t=>{
  const f=await fixture(t);await candidateFlow(f);await f.qa.security.act({kind:'submitSetup',code:'123456'});assert.equal(document.querySelectorAll('[data-recovery-code]').length,10);
  f.emit('pagehide');assert.equal(f.qa.security.hasCompletedResult(),false);assert.equal(document.querySelectorAll('[data-recovery-code]').length,0);
  await f.qa.enter({user:{...user,id:'new-owner',username:'new-owner'},csrfToken:'n'.repeat(43)});assert.equal(f.qa.state.user.id,'new-owner');assert.equal(document.querySelectorAll('[data-recovery-code]').length,0);
});
test('saved result closes to actual login username focus without claiming a retained session',async t=>{
  const f=await fixture(t);await candidateFlow(f);await f.qa.security.act({kind:'submitSetup',code:'123456'});
  const username=f.node('#login-form').elements.username;let focused=false;username.focus=()=>{focused=true;};
  const saved=document.querySelector('[data-saved-codes]');saved.checked=true;saved.dispatchEvent(new f.window.Event('change'));
  document.querySelector('[data-finish-codes]').dispatchEvent(new f.window.Event('click'));await f.settle();
  assert.equal(focused,true);assert.equal(f.qa.state.user,null);assert.equal(f.qa.state.csrf,'');assert.equal(f.qa.security.hasCompletedResult(),false);assert.equal(document.querySelectorAll('[data-recovery-code]').length,0);
});
