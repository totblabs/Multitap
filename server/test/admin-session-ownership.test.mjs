import {test} from 'node:test';
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {parseHTML} from 'linkedom';

// Full unchanged app functions/modules and actual admin DOM, with only
// import resolution + readonly test exports appended in memory. No production
// exports, real fetch, server/account/database writes or browser are involved.
const appUrl = new URL('../admin/app.js', import.meta.url);
const deferred = () => {let resolve,reject;const promise=new Promise((a,b)=>{resolve=a;reject=b;});return {promise,resolve,reject};};
const response = (body,status=200) => ({ok:status<400,status,json:async()=>body});
const administrator = id => ({id,username:id,role:'admin',mustChangeCredentials:false});
const inventory = owner => ({owner,users:[],desktops:[],assignments:[],audit:[],proxmox:{configured:false},onboarding:{complete:true,proxmoxRequired:false}});
let serial=0;
async function app(t){
 const window=parseHTML(await readFile(new URL('../admin/index.html',import.meta.url),'utf8'));
 const previous=Object.fromEntries(['document','Node','fetch','FormData','location','sessionStorage','matchMedia','addEventListener'].map(k=>[k,globalThis[k]]));
 globalThis.document=window.document;globalThis.Node=window.Node;globalThis.location=new URL('https://admin.fixture.invalid/admin/');
 globalThis.sessionStorage={getItem:()=>null,setItem(){}};globalThis.matchMedia=()=>({matches:false,addEventListener(){}});globalThis.addEventListener=()=>{};
 const formProto=Object.getPrototypeOf(document.querySelector('form'));
 formProto.reset=function(){for(const n of this.querySelectorAll('input,textarea'))n.value='';};
 Object.defineProperty(formProto,'elements',{configurable:true,get(){return Object.fromEntries([...this.querySelectorAll('[name]')].map(n=>[n.name,n]));}});
 globalThis.FormData=class{constructor(form){this.rows=[...form.querySelectorAll('[name]')].map(n=>[n.name,n.value]);}*[Symbol.iterator](){yield* this.rows;}};
 const dialog=document.querySelector('#modal');dialog.showModal=()=>{dialog.open=true;};dialog.close=()=>{dialog.open=false;};
 const user=administrator('admin-a'),queues=new Map(),requests=[];let currentInventory=inventory('initial');
 const queue=(path,value)=>{const values=queues.get(path)??[];values.push(value);queues.set(path,values);};
 globalThis.fetch=async(url,options={})=>{
  const path=url.replace('/api/v1/admin','');requests.push({path,method:options.method??'GET'});
  if(queues.get(path)?.length){const next=queues.get(path).shift();return typeof next==='function'?next():await next;}
  if(url==='/healthz')return response({service:'multitap-broker',version:'1.0.12'});
  if(url==='/api/v1/downloads')return response({releases:[]});
  if(path==='/session')return response({user,csrfToken:'fixture-csrf'});
  if(path==='/inventory')return response(currentInventory);
  if(path==='/server-info')return response({service:'multitap-broker',version:'1.0.12',deployment:{kind:'unknown',configured:false},update:{state:'not_configured',feedConfigured:false,applySupported:false},checkedAt:new Date().toISOString()});
  if(path==='/proxmox/status')return response({proxmox:currentInventory.proxmox});
  if(path==='/proxmox/sync')return response({proxmox:currentInventory.proxmox});
  return response({sessions:[],hosts:[],events:[],requests:[],pendingCount:0});
 };
 const source=(await readFile(appUrl,'utf8')).replace(/from '(\.[^']+)'/g,(_,path)=>`from '${new URL(path,appUrl).href}'`);
 const instrumented=source+`\nexport const qa={state,api,refresh,showLogin,enter,modal,syncProxmox,readProxmoxStatus,change,checkServerUpdate,getLost:()=>serverLost};\n//# sourceURL=admin-session-fixture-${++serial}.js`;
 const {qa}=await import('data:text/javascript;base64,'+Buffer.from(instrumented).toString('base64'));
 t.after(()=>{qa.showLogin();for(const [key,value]of Object.entries(previous))globalThis[key]=value;});
 const settle=async()=>{for(let n=0;n<5;n++)await new Promise(r=>setImmediate(r));};await settle();
 return {qa,user,window,dialog,queue,requests,settle,node:s=>document.querySelector(s),setInventory:value=>{currentInventory=value;},login:async who=>{currentInventory=inventory(who.id);await qa.enter({user:who,csrfToken:'fixture-csrf'});await settle();}};
}

for(const sameReference of [false,true]) test(`ST02 old inventory success/finally cannot overwrite or unlock ${sameReference?'same-account re-login':'new account'} refresh`,async t=>{
 const h=await app(t),old=deferred(),fresh=deferred();h.queue('/inventory',old.promise);const pending=h.qa.refresh();
 h.qa.showLogin();h.queue('/inventory',fresh.promise);const who=sameReference?h.user:administrator('admin-b');const next=h.login(who);await h.settle();
 const newRequestAdmitted=h.requests.filter(r=>r.path==='/inventory').length===3;
 old.resolve(response(inventory('stale')));await pending;
 const busyAfterOld=h.qa.state.loading,disabledAfterOld=h.node('#refresh').disabled;
 fresh.resolve(response(inventory('fresh')));await next;
 assert.equal(newRequestAdmitted,true,'new owner starts its own inventory while old request remains pending');
 assert.equal(busyAfterOld,true,'old finally cannot clear current refresh loading');assert.equal(disabledAfterOld,true);
 assert.equal(h.qa.state.data.owner,'fresh');assert.equal(h.qa.state.user,who);assert.equal(h.qa.state.loading,false);assert.equal(h.node('#refresh').disabled,false);
});
test('ST02 old inventory rejection does not show an error in the new session',async t=>{
 const h=await app(t),old=deferred();h.queue('/inventory',old.promise);const pending=h.qa.refresh();h.qa.showLogin();await h.login(h.user);
 h.node('#notice').textContent='current-owner-notice';old.reject(new Error('old-network-failure'));await pending;
 assert.equal(h.node('#notice').textContent,'current-owner-notice');assert.equal(h.qa.state.data.owner,h.user.id);assert.equal(h.qa.getLost(),false);
});
for(const finish of ['401','body-401','setup-required','network','invalid-json','success'])test(`ST03 stale ${finish} cannot mutate same-account new generation`,async t=>{
 const h=await app(t),old=deferred(),json=deferred();
 // Defer body parsing independently when testing the second await boundary.
 if(finish==='body-401'){h.queue('/body-delay',{ok:false,status:401,json:()=>json.promise});}
 else h.queue('/delayed',old.promise);
 const pending=h.qa.api(finish==='body-401'?'/body-delay':'/delayed').then(()=>({resolved:true}),error=>({error}));await h.settle();
 h.qa.showLogin();await h.login(h.user);const route=h.qa.state.route.page;
 if(finish==='network')old.reject(new Error('network-failure'));
 else if(finish==='invalid-json')old.resolve({ok:true,status:200,json:async()=>{throw Error('bad-json');}});
 else if(finish==='body-401')json.resolve({error:{code:'unauthorized'}});
 else old.resolve(response(finish==='setup-required'?{error:{code:'proxmox_setup_required'}}:finish==='401'?{error:{code:'unauthorized'}}:{ok:true},finish==='success'?200:finish==='setup-required'?409:401));
 const result=await pending;
 assert.equal(h.qa.state.user,h.user);assert.equal(h.node('#console').hidden,false);assert.equal(h.qa.state.route.page,route);assert.equal(h.qa.getLost(),false);
 assert.equal(result.error?.code,'admin_session_changed','stale response is explicit local cancellation, not current request success/failure');
});
test('current unauthorized/network/setup failures retain their actual behavior',async t=>{
 const h=await app(t);h.queue('/current-network',()=>{throw Error('network');});await assert.rejects(h.qa.api('/current-network'),/네트워크/);assert.equal(h.qa.getLost(),true);
 h.queue('/current-setup',response({error:{code:'proxmox_setup_required'}},409));await assert.rejects(h.qa.api('/current-setup'),e=>e.code==='proxmox_setup_required');assert.equal(h.qa.state.route.page,'proxmox');
 h.queue('/current-unauthorized',response({error:{code:'unauthorized'}},401));await assert.rejects(h.qa.api('/current-unauthorized'),e=>e.code==='unauthorized');assert.equal(h.qa.state.user,null);assert.equal(h.node('#login').hidden,false);
});
test('current refresh failure retains prior valid inventory and clears its own busy controls',async t=>{
 const h=await app(t);h.queue('/inventory',()=>{throw Error('current-network-failure');});await h.qa.refresh();
 assert.equal(h.qa.state.data.owner,'initial');assert.equal(h.qa.state.loading,false);assert.equal(h.node('#refresh').disabled,false);assert.match(h.node('#notice').textContent,/네트워크/);
});
for(const fail of [false,true])test(`ST04 replaced modal ${fail?'error':'success'} leaves replacement fields/controls owned by new submit`,async t=>{
 const h=await app(t),old=deferred(),fresh=deferred();h.qa.modal('old',[],()=>old.promise,{reload:false});const pending=h.node('#modal-form').onsubmit({preventDefault(){}});
 h.dialog.close();h.qa.modal('new',[{name:'notes',label:'Current',value:'current-draft'}],()=>fresh.promise,{reload:false});const next=h.node('#modal-form').onsubmit({preventDefault(){}});
 if(fail)old.reject(Error('old-write-error'));else old.resolve();await pending;
 const open=h.dialog.open,title=h.node('#modal-title').textContent,draft=h.node('[name=notes]').value,busy=h.node('#modal-save').disabled,error=h.node('#modal-error').textContent;
 fresh.resolve();await next;
 assert.equal(open,true);assert.equal(title,'new');assert.equal(draft,'current-draft');assert.equal(busy,true);assert.equal(error,'');assert.equal(h.dialog.open,false);assert.equal(h.node('#modal-save').disabled,false);
});
test('ST04 same-account re-login invalidates pending modal action and old submit handler',async t=>{
 const h=await app(t),old=deferred();let writes=0;h.qa.modal('old',[],()=>{writes++;return old.promise;},{reload:false});const oldSubmit=h.node('#modal-form').onsubmit,pending=oldSubmit({preventDefault(){}});
 h.qa.showLogin();await h.login(h.user);h.qa.modal('new',[],async()=>{}, {reload:false});old.resolve();await pending;await oldSubmit({preventDefault(){}});
 assert.equal(writes,1,'a retained submit closure cannot issue a new write after owner retirement');assert.equal(h.dialog.open,true);assert.equal(h.node('#modal-title').textContent,'new');
});
test('current modal errors, confirmation mismatch and successful retry remain usable',async t=>{
 const h=await app(t);let attempts=0;h.qa.modal('current',[],async()=>{if(++attempts===1)throw Error('current-validation-error');},{reload:false});
 await h.node('#modal-form').onsubmit({preventDefault(){}});assert.equal(h.dialog.open,true);assert.equal(h.node('#modal-error').textContent,'current-validation-error');assert.equal(h.node('#modal-save').disabled,false);
 await h.node('#modal-form').onsubmit({preventDefault(){}});assert.equal(h.dialog.open,false);assert.equal(attempts,2);
 h.qa.modal('password',[{name:'password',label:'password',value:'one'},{name:'passwordConfirmation',label:'again',value:'two'}],async()=>{attempts++;},{reload:false});
 await h.node('#modal-form').onsubmit({preventDefault(){}});assert.equal(attempts,2);assert.equal(h.dialog.open,true);assert.equal(h.node('#modal-save').disabled,false);assert.notEqual(h.node('#modal-error').textContent,'');
});
test('old same-account Proxmox sync/status cannot replace new inventory or launch stale follow-up requests',async t=>{
 const h=await app(t),old=deferred();h.qa.state.data.proxmox.configured=true;h.queue('/proxmox/sync',old.promise);const pending=h.qa.syncProxmox();
 h.qa.showLogin();await h.login(h.user);const count=h.requests.filter(r=>r.path==='/inventory').length;
 old.resolve(response({proxmox:{configured:true,marker:'stale'}}));await pending;
 assert.equal(h.requests.filter(r=>r.path==='/inventory').length,count);assert.equal(h.qa.state.data.owner,h.user.id);assert.equal(h.qa.state.pveError,'');
 const status=deferred();h.queue('/proxmox/status',status.promise);const later=h.qa.readProxmoxStatus().catch(e=>e);h.qa.showLogin();await h.login(h.user);status.resolve(response({proxmox:{configured:true,marker:'stale'}}));await later;
 assert.equal(h.qa.state.data.proxmox.marker,undefined);
});
test('newer same-session sync inventory wins while old refresh still releases only its own busy state',async t=>{
 const h=await app(t),old=deferred();h.qa.state.data.proxmox.configured=true;
 h.queue('/inventory',old.promise);const pending=h.qa.refresh();
 const fresh=inventory('newer-sync');fresh.proxmox.configured=true;h.setInventory(fresh);
 await h.qa.syncProxmox();assert.equal(h.qa.state.data.owner,'newer-sync');
 old.resolve(response(inventory('older-refresh')));await pending;
 assert.equal(h.qa.state.data.owner,'newer-sync');assert.equal(h.qa.state.loading,false);assert.equal(h.node('#refresh').disabled,false);
});
test('retired login failure/finally cannot erase a newer pending login field or enable its button',async t=>{
 const h=await app(t),old=deferred(),fresh=deferred();h.qa.showLogin();
 const form=h.node('#login-form');form.elements.username.value='older';form.elements.password.value='older-fixture';h.queue('/login',old.promise);form.dispatchEvent(new h.window.Event('submit',{cancelable:true}));
 h.qa.showLogin();form.elements.username.value='newer';form.elements.password.value='newer-fixture';h.queue('/login',fresh.promise);form.dispatchEvent(new h.window.Event('submit',{cancelable:true}));
 old.reject(Error('retired-login-error'));await h.settle();assert.equal(form.elements.password.value,'newer-fixture');assert.equal(form.querySelector('button').disabled,true);assert.equal(h.node('#login-error').textContent,'');
 fresh.resolve(response({user:administrator('newer'),csrfToken:'fixture-csrf'}));await h.settle();assert.equal(h.qa.state.user.id,'newer');assert.equal(form.querySelector('button').disabled,false);
});
for(const fail of [false,true])test(`retired logout ${fail?'error':'success'} cannot log out or notify a replacement session`,async t=>{
 const h=await app(t),old=deferred();h.queue('/logout',old.promise);h.node('#logout').dispatchEvent(new h.window.Event('click'));
 h.qa.showLogin();await h.login(h.user);h.node('#notice').textContent='current-notice';if(fail)old.reject(Error('old-logout-error'));else old.resolve(response({ok:true}));await h.settle();
 assert.equal(h.qa.state.user,h.user);assert.equal(h.node('#console').hidden,false);assert.equal(h.node('#notice').textContent,'current-notice');
});
test('retired update check cannot clear current request submitting state in finally',async t=>{
 const h=await app(t),old=deferred(),fresh=deferred();h.queue('/server-update/check',old.promise);const pending=h.qa.checkServerUpdate();
 h.qa.showLogin();await h.login(h.user);h.queue('/server-update/check',fresh.promise);const next=h.qa.checkServerUpdate();
 old.reject(Error('old-update-error'));await pending;assert.equal(h.qa.state.serverUpdateSubmitting,true);
 fresh.resolve(response({ok:true}));await next;assert.equal(h.qa.state.serverUpdateSubmitting,false);
});
