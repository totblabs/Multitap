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
  const path=url.replace('/api/v1/admin','');requests.push({path,method:options.method??'GET',hasAbortSignal:!!options.signal,body:options.body===undefined?undefined:JSON.parse(options.body)});
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
 const instrumented=source+`\nexport const qa={ctx,state,api,load,refresh,showLogin,enter,modal,syncProxmox,readProxmoxStatus,change,checkServerUpdate,getLost:()=>serverLost};\n//# sourceURL=admin-session-fixture-${++serial}.js`;
 const {qa}=await import('data:text/javascript;base64,'+Buffer.from(instrumented).toString('base64'));
 t.after(()=>{qa.showLogin();for(const [key,value]of Object.entries(previous))globalThis[key]=value;});
 const settle=async()=>{for(let n=0;n<5;n++)await new Promise(r=>setImmediate(r));};await settle();
 return {qa,user,window,dialog,queue,requests,settle,node:s=>document.querySelector(s),setInventory:value=>{currentInventory=value;},login:async who=>{currentInventory=inventory(who.id);await qa.enter({user:who,csrfToken:'fixture-csrf'});await settle();}};
}


const moduleUrl=new URL('../admin/js/system.js',import.meta.url),moduleSource=(await readFile(moduleUrl,'utf8')).replace(/from '(\.[^']+)'/g,(_,path)=>`from '${new URL(path,moduleUrl).href}'`);const system=await import('data:text/javascript;base64,'+Buffer.from(moduleSource+'\nexport {renewProxmox};').toString('base64'));
const desktops=await import('../admin/js/desktops.js');
test('Token renewal continuation cannot show another login integration result',{timeout:10000},async t=>{
 const h=await app(t),hold=deferred();h.qa.state.data.proxmox={configured:true,tokenId:'fixture@pve!fixture',endpoint:'https://fixture.invalid',setupScriptSha256:'ab'.repeat(32)};let action;system.renewProxmox({...h.qa.ctx,modal:(_t,_f,callback)=>{action=callback;}});
 h.queue('/proxmox/sync',hold.promise);let rejected;const pending=action({renewal:''}).catch(e=>{rejected=e.code??e.message;});await h.settle();assert.equal(h.qa.state.pveBusy,true);h.qa.showLogin();await h.login(administrator('admin-b'));h.qa.state.data.proxmox={configured:true,expiresAt:'2030-01-01T00:00:00Z',expirySource:'proxmox'};const before=h.node('#notice').textContent;hold.resolve(response({proxmox:{configured:true}}));await pending;await h.settle();console.log(JSON.stringify({flow:'renew',notice:h.node('#notice').textContent,rejected,realNetwork:false}));assert.equal(h.node('#notice').textContent,before);
});
test('Old power observation timer cannot send follow-up through the next login',{timeout:10000},async t=>{
 t.mock.timers.enable({apis:['setTimeout']});const h=await app(t);let action;desktops.powerForm({...h.qa.ctx,modal:(_a,_b,callback)=>{action=callback;}},{id:'pve-700',kind:'vm',name:'Fixture VM',pve_vmid:700,pve_power_status:'running'},'reboot');assert.equal(h.requests.filter(r=>r.path==='/desktops/pve-700/power').length,0,'nothing is sent before the dialog is confirmed');h.queue('/desktops/pve-700/power',response({task:'fixture-task',node:'fixture-node'}));let rejected;const pending=action().catch(e=>{rejected=e.code??e.message;});await h.settle();assert.equal(h.requests.filter(r=>r.path==='/desktops/pve-700/power').length,1);h.qa.showLogin();await h.login(administrator('admin-b'));h.queue('/proxmox/task',response({status:'stopped',exitStatus:'OK'}));const before=h.node('#notice').textContent;t.mock.timers.tick(1500);await h.settle();await pending;const pollCount=h.requests.filter(r=>r.path==='/proxmox/task').length;console.log(JSON.stringify({flow:'power-observe',pollCount,notice:h.node('#notice').textContent,rejected,realPower:false,realNetwork:false}));assert.equal(pollCount,0,'old timer must stop before dispatching under the new session');assert.equal(h.node('#notice').textContent,before);
});

function renewalAction(h) {
 let action; system.renewProxmox({ ...h.qa.ctx, modal: (_title, _fields, callback) => { action = callback; } }); return action;
}
for (const stage of ['save', 'sync', 'success']) test('Renewal ' + stage + ' reports confirmed expiry save separately from verification and never replays', async t => {
 const h = await app(t), config = { configured: true, tokenId: 'fixture@pve!fixture', endpoint: 'https://fixture.invalid', expiresAt: '2099-01-01T00:00:00Z', expirySource: 'proxmox', setupScriptSha256: 'ab'.repeat(32) };
 h.setInventory({ ...inventory('current'), proxmox: config }); h.qa.state.data.proxmox = config;
 const block = 'MULTITAP-RENEWAL-V1:\n' + Buffer.from(JSON.stringify({ tokenId: config.tokenId, expiresAt: config.expiresAt })).toString('base64') + '\nEND-MULTITAP-RENEWAL';
 h.queue('/proxmox', stage === 'save' ? response({ error: { code: 'fixture_current_error' } }, 500) : response({ ok: true }));
 if (stage === 'sync') h.queue('/proxmox/sync', response({ error: { code: 'fixture_current_error' } }, 500));
 let error; try { await renewalAction(h)({ renewal: block }); } catch (caught) { error = caught; }
 assert.equal(h.requests.filter(value => value.path === '/proxmox' && value.method === 'PUT').length, 1);
 if (stage === 'success') { assert.equal(error, undefined); assert.match(h.node('#notice').textContent, /만료일을 확인/); }
 else { assert.ok(error); if (stage === 'sync') assert.match(error.message, /저장했지만.*확인.*실패/); else assert.doesNotMatch(error.message, /저장했지만/); }
 assert.equal(h.qa.state.pveBusy, false);
});

const vm = (id = 'pve-700') => ({ id, kind: 'vm', name: 'Fixture VM', pve_vmid: 700, pve_power_status: 'running' });
function powerAction(h, target = vm(), kind = 'reboot') {
 let action, opened;
 desktops.powerForm({ ...h.qa.ctx, modal: (title, _fields, callback, options) => { opened = { title, save: options.save }; action = callback; } }, target, kind);
 assert.deepEqual(opened, { title: target.name + (kind === 'reboot' ? ' 다시 시작' : ' 끄기'), save: kind === 'reboot' ? '다시 시작' : '끄기' });
 return action;
}
for (const outcome of ['success', 'failure']) test('Current power observation ' + outcome + ' keeps one accepted request and a current result', async t => {
 t.mock.timers.enable({ apis: ['setTimeout'] }); const h = await app(t);
 h.queue('/desktops/pve-700/power', response({ task: 'fixture-task', node: 'fixture-node' }));
 h.queue('/proxmox/task', response({ status: 'stopped', exitStatus: outcome === 'success' ? 'OK' : 'fixture-failure' }));
 // The dialog's action ends once Proxmox has taken the request (the dialog closes); the task is
 // followed after it, and how it ended is a toast, not an error left in a dialog that stands frozen until then.
 let error, ended = false; const pending = powerAction(h)().catch(caught => { error = caught; }).then(() => { ended = true; }); await h.settle();
 assert.equal(h.requests.filter(value => value.path === '/proxmox/task').length, 0);
 assert.equal(ended, true, 'the dialog does not wait for the Proxmox task'); assert.equal(error, undefined);
 assert.equal(desktops.startPending(h.qa.ctx, vm()), true, 'the request in flight is still followed');
 t.mock.timers.tick(1500); await h.settle(); await pending;
 assert.equal(desktops.startPending(h.qa.ctx, vm()), false);
 assert.equal(h.requests.filter(value => value.path === '/desktops/pve-700/power').length, 1);
 assert.deepEqual(h.requests.find(value => value.path === '/desktops/pve-700/power').body, { action: 'reboot' });
 assert.equal(h.requests.filter(value => value.path === '/proxmox/task').length, 1);
 if (outcome === 'success') { assert.equal(error, undefined); assert.match(h.node('#notice').textContent, /전원 작업이 완료/); }
 else { assert.equal(error, undefined); assert.match(h.node('#notice').textContent, /Proxmox 작업 실패: fixture-failure/); assert.ok(h.node('#notice').classList.contains('is-error')); }
});

test('A late accepted power response cannot start an observer under a later login', async t => {
 t.mock.timers.enable({ apis: ['setTimeout'] }); const h = await app(t), hold = deferred();
 h.queue('/desktops/pve-700/power', hold.promise); const pending = powerAction(h)().catch(error => error.code); await h.settle();
 h.qa.showLogin(); await h.login(administrator('admin-b')); const before = h.node('#notice').textContent;
 hold.resolve(response({ task: 'fixture-task', node: 'fixture-node' })); await h.settle(); assert.equal(await pending, 'admin_session_changed');
 t.mock.timers.tick(1500); await h.settle();
 assert.equal(h.requests.filter(value => value.path === '/proxmox/task').length, 0); assert.equal(h.node('#notice').textContent, before);
 assert.equal(h.requests.filter(value => value.path === '/desktops/pve-700/power').length, 1);
});

test('Independent VM power observers do not supersede each other or replay the power requests', async t => {
 t.mock.timers.enable({ apis: ['setTimeout'] }); const h = await app(t), targets = [vm(), vm('pve-701')];
 for (const target of targets) h.queue('/desktops/' + target.id + '/power', response({ task: target.id + '-task', node: 'fixture-node' }));
 h.queue('/proxmox/task', response({ status: 'stopped', exitStatus: 'OK' })); h.queue('/proxmox/task', response({ status: 'stopped', exitStatus: 'OK' }));
 const pending = targets.map(target => powerAction(h, target)()); await h.settle(); t.mock.timers.tick(1500); await h.settle(); await Promise.all(pending);
 assert.equal(h.requests.filter(value => value.path === '/proxmox/task').length, 2);
 for (const target of targets) assert.equal(h.requests.filter(value => value.path === '/desktops/' + target.id + '/power').length, 1);
});

test('Old start observation cannot notify or clear a new login start flag for the same VM', async t => {
 t.mock.timers.enable({ apis: ['setTimeout'] }); const h = await app(t), old = deferred(), fresh = deferred(), target = { ...vm(), pve_power_status: 'stopped' };
 h.queue('/desktops/pve-700/power', old.promise); desktops.powerForm(h.qa.ctx, target, 'start'); await h.settle(); assert.equal(desktops.startPending(h.qa.ctx, target), true);
 h.qa.showLogin(); await h.login(administrator('admin-b'));
 h.queue('/desktops/pve-700/power', fresh.promise); desktops.powerForm(h.qa.ctx, target, 'start'); await h.settle(); assert.equal(desktops.startPending(h.qa.ctx, target), true);
 const before = h.node('#notice').textContent;
 old.resolve(response({ task: 'old-task', node: 'fixture-node' })); await h.settle(); assert.equal(desktops.startPending(h.qa.ctx, target), true); assert.equal(h.node('#notice').textContent, before);
 h.queue('/proxmox/task', response({ status: 'stopped', exitStatus: 'OK' })); fresh.resolve(response({ task: 'new-task', node: 'fixture-node' })); await h.settle(); t.mock.timers.tick(1500); await h.settle();
 assert.equal(desktops.startPending(h.qa.ctx, target), false);
 assert.equal(h.requests.filter(value => value.path === '/desktops/pve-700/power').length, 2);
 assert.equal(h.requests.filter(value => value.path === '/proxmox/task').length, 1);
});

test('Current start failure releases its pending set and retains the real error without retrying', async t => {
 const h = await app(t), target = { ...vm(), pve_power_status: 'stopped' };
 h.queue('/desktops/pve-700/power', response({ error: { code: 'fixture_current_error' } }, 500)); desktops.powerForm(h.qa.ctx, target, 'start'); await h.settle();
 assert.equal(desktops.startPending(h.qa.ctx, target), false); assert.ok(h.node('#notice').textContent);
 assert.equal(h.requests.filter(value => value.path === '/desktops/pve-700/power').length, 1);
});
