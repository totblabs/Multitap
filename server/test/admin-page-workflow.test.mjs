import {userForm} from '../admin/js/users.js';
import {el} from '../admin/js/dom.js';
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
  if(path==='/hosts')return response({hosts:currentInventory.desktops.map(d=>({desktopId:d.id}))});
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




async function privateActions(path, names) {
 const url = new URL(path, appUrl), source = (await readFile(url, 'utf8')).replace(/from '(\.[^']+)'/g, (_, relative) => `from '${new URL(relative, url).href}'`);
 return import('data:text/javascript;base64,' + Buffer.from(source + '\nexport {' + names.join(',') + '};').toString('base64'));
}
const users = await privateActions('./js/users.js', ['assignDesktops', 'enableForm']);
const system = await privateActions('./js/system.js', ['saveProxmox', 'checkConsolePermission']);
const desktops = await import('../admin/js/desktops.js');
const targetUser = { id: 'target-id', username: 'target', disabled: false, role: 'user' };
const targetDesktop = { id: 'pve-149', name: 'Fixture VM', kind: 'vm', pve_vmid: 149, pve_available: 1, enabled: true };
async function seed(h) { h.setInventory({ ...inventory('seed'), users: [targetUser], desktops: [targetDesktop] }); await h.qa.refresh(); await h.qa.load('hosts',{force:true}); }
// 사용자 > 데스크톱 배정: the real dialog with the one registered desktop ticked,
// submitted through the real form. There is no draft: the choice is saved at once.
function submitAssignment(h) {
 users.assignDesktops(h.qa.ctx, h.qa.ctx.userByName('target'));
 assert.equal(h.node('#modal-title').textContent, '데스크톱 배정');
 const box = h.node('#modal-fields input[data-vm-id="pve-149"]'); assert.ok(box, 'the registered desktop is offered');
 box.checked = true;
 return h.node('#modal-form').onsubmit({ preventDefault() {} });
}
const configured = owner => ({ ...inventory(owner), proxmox: { configured: true, unavailableCount: 0 } });
const accountInput = { username: 'created-by-previous-admin', role: 'user', notes: '', password: 'Only-fixture-password', passwordConfirmation: 'Only-fixture-password' };

for (const sameUser of [false, true]) test('ST14 Proxmox continuation cannot announce success in a newer ' + (sameUser ? 'same-object login' : 'account'), async t => {
 const h = await app(t), hold = deferred();
 h.queue('/proxmox', response({ ok: true })); h.queue('/inventory', response(configured('saved'))); h.queue('/proxmox/sync', hold.promise);
 const pending = system.saveProxmox(h.qa.ctx, {}).catch(error => error.code); await h.settle();
 assert.equal(h.qa.state.pveBusy, true);
 h.qa.showLogin(); await h.login(sameUser ? h.user : administrator('admin-b'));
 const before = { data: JSON.stringify(h.qa.state.data), notice: h.node('#notice').textContent };
 hold.resolve(response({ proxmox: { configured: true } })); await pending; await h.settle();
 assert.equal(JSON.stringify(h.qa.state.data), before.data); assert.equal(h.node('#notice').textContent, before.notice);
 assert.equal(h.requests.filter(value => value.path === '/proxmox' && value.method === 'PUT').length, 1, 'a committed write is never replayed');
});

for (const sameUser of [false, true]) test('ST14 created-account refresh cannot navigate a newer ' + (sameUser ? 'same-object login' : 'account'), async t => {
 const h = await app(t), hold = deferred(); let action;
 userForm({ ...h.qa.ctx, modal: (_title, _fields, callback) => { action = callback; } });
 h.queue('/users', response({ ok: true }, 201)); h.queue('/inventory', hold.promise);
 const pending = action(accountInput).catch(error => error.code); await h.settle(); assert.equal(h.qa.state.loading, true);
 h.qa.showLogin(); await h.login(sameUser ? h.user : administrator('admin-b'));
 const before = { route: JSON.stringify(h.qa.state.route), data: JSON.stringify(h.qa.state.data), notice: h.node('#notice').textContent };
 hold.resolve(response(inventory('old'))); await pending; await h.settle();
 assert.equal(JSON.stringify(h.qa.state.data), before.data); assert.equal(JSON.stringify(h.qa.state.route), before.route); assert.equal(h.node('#notice').textContent, before.notice);
});

for (const kind of ['assignment', 'enable', 'console']) test('ST14 old ' + kind + ' catch/finally cannot unlock or notify a new pending action', async t => {
 const h = await app(t), old = deferred(), fresh = deferred(), target = { id: 'target-id', username: 'target', disabled: kind === 'enable' };
 const prepare = async () => { if (kind === 'assignment') await seed(h); else h.qa.state.data.users.push(target); };
 await prepare();
 const path = kind === 'assignment' ? '/assignments/batch' : kind === 'enable' ? '/users/target/enabled' : '/proxmox/console-permission';
 const run = () => kind === 'assignment' ? submitAssignment(h) : kind === 'enable' ? users.enableForm(h.qa.ctx, target) : system.checkConsolePermission(h.qa.ctx);
 const busy = () => kind === 'assignment' ? h.qa.state.assignmentSaving : kind === 'enable' ? h.qa.state.ui.accountEnabling : h.qa.state.ui.consoleChecking;
 h.queue(path, old.promise); const oldPending = run(); await h.settle(); assert.equal(busy(), true);
 h.qa.showLogin(); await h.login(administrator('admin-b')); await prepare(); h.queue(path, fresh.promise); const freshPending = run(); await h.settle(); assert.equal(busy(), true);
 old.resolve(response({ ok: true, added: 1, removed: 0, granted: true })); await oldPending; await h.settle();
 const afterOld = { busy: busy(), notice: h.node('#notice').textContent, dialog: h.dialog.open, error: h.node('#modal-error').textContent };
 fresh.resolve(response({ ok: true, added: 1, removed: 0, granted: true })); await freshPending;
 assert.equal(afterOld.busy, true); assert.equal(afterOld.notice, ''); assert.equal(busy(), false);
 if (kind === 'assignment') {
  assert.deepEqual([afterOld.dialog, afterOld.error], [true, ''], 'the old answer neither closes nor marks the newer dialog');
  const writes = h.requests.filter(value => value.path === path && value.method === 'PUT');
  assert.equal(writes.length, 2, 'one write per explicit submit; the old one is never replayed');
  assert.deepEqual(writes[1].body, { username: 'target', expectedAccountId: 'target-id', desktopIds: ['pve-149'] });
  assert.match(h.node('#notice').textContent, /target 님에게 데스크톱 1대 배정을 저장했어요\./);
 }
});

function assignmentModal(h) { let action; desktops.assignDesktopUser({ ...h.qa.ctx, modal: (_title, _fields, callback) => { action = callback; } }, targetDesktop); return action; }
test('ST14 desktop assignment finally cannot unlock a newer login assignment', async t => {
 const h = await app(t), old = deferred(), fresh = deferred(); await seed(h);
 h.queue('/assignments/batch', old.promise); const oldPending = assignmentModal(h)({ username: 'target' }).catch(error => error.code); await h.settle();
 assert.equal(h.qa.state.assignmentSaving, true);
 h.qa.showLogin(); await h.login(administrator('admin-b')); await seed(h);
 h.queue('/assignments/batch', fresh.promise); const newPending = assignmentModal(h)({ username: 'target' }); await h.settle();
 old.resolve(response({ added: 1 })); await oldPending; const afterOld = h.qa.state.assignmentSaving;
 fresh.resolve(response({ added: 1 })); await newPending;
 assert.equal(afterOld, true); assert.equal(h.qa.state.assignmentSaving, false);
});

for (const stage of ['save', 'inventory', 'sync', 'success']) test('ST07 confirmed-save boundary and current Proxmox ' + stage + ' outcome remain visible without replay', async t => {
 const h = await app(t); h.setInventory(configured('current'));
 h.queue('/proxmox', stage === 'save' ? Promise.reject(Error('fixture unknown save outcome')) : response({ ok: true }));
 if (stage === 'inventory') h.queue('/inventory', Promise.reject(Error('fixture verification unavailable')));
 if (stage === 'sync') h.queue('/proxmox/sync', Promise.reject(Error('fixture verification unavailable')));
 let error; try { await system.saveProxmox(h.qa.ctx, {}); } catch (caught) { error = caught; }
 assert.equal(h.requests.filter(value => value.path === '/proxmox' && value.method === 'PUT').length, 1);
 assert.equal(h.qa.state.pveBusy, false);
 if (stage === 'success') { assert.equal(error, undefined); assert.match(h.node('#notice').textContent, /등록된 VM의 상태/); }
 else {
   assert.ok(error); assert.notEqual(error.code, 'admin_session_changed');
   if (stage === 'save') assert.doesNotMatch(error.message, /저장했지만|저장됐|저장됨/);
   else assert.match(error.message, /저장했지만.*확인.*실패/);
   assert.equal(h.node('#notice').textContent, '', 'caller receives the current failure; no success notice');
 }
});

for (const kind of ['assignment', 'enable', 'console']) test('Current ' + kind + ' failure stays visible and releases only its own lock', async t => {
 const h = await app(t), target = { id: 'target-id', username: 'target', disabled: kind === 'enable' };
 if (kind === 'assignment') await seed(h); else h.qa.state.data.users.push(target);
 const path = kind === 'assignment' ? '/assignments/batch' : kind === 'enable' ? '/users/target/enabled' : '/proxmox/console-permission';
 h.queue(path, response({ error: { code: 'fixture_current_error' } }, 500));
 await (kind === 'assignment' ? submitAssignment(h) : kind === 'enable' ? users.enableForm(h.qa.ctx, target) : system.checkConsolePermission(h.qa.ctx));
 const busy = kind === 'assignment' ? h.qa.state.assignmentSaving : kind === 'enable' ? h.qa.state.ui.accountEnabling : h.qa.state.ui.consoleChecking;
 // 데스크톱 배정 fails inside its dialog: the error is shown there and the ticked desktop stays ticked.
 const shown = kind === 'assignment' ? h.node('#modal-error').textContent : h.node('#notice').textContent;
 assert.equal(busy, false); assert.ok(shown); assert.doesNotMatch(shown, /이전 로그인/);
 if (kind === 'assignment') {
  assert.equal(h.dialog.open, true, 'a failed write keeps the dialog');
  assert.equal(h.node('#modal-fields input[data-vm-id="pve-149"]').checked, true, 'a failed write keeps the exact choice');
  assert.equal(h.node('#modal-save').disabled, false, 'the dialog can be submitted again by hand');
  assert.deepEqual(h.qa.state.data.assignments, [], 'nothing is shown as assigned');
  assert.doesNotMatch(h.node('#notice').textContent, /저장했어요/);
 }
 assert.equal(h.requests.filter(value => value.path === path).length, 1, 'failure never automatically resubmits a write');
});

test('Current account creation still refreshes once and navigates to the committed account', async t => {
 const h = await app(t); let action;
 userForm({ ...h.qa.ctx, modal: (_title, _fields, callback) => { action = callback; } });
 h.queue('/users', response({ ok: true }, 201)); await action(accountInput);
 assert.equal(h.qa.state.route.page, 'user'); assert.equal(h.qa.state.route.id, accountInput.username);
 assert.match(h.node('#notice').textContent, /계정을 만들었어요/);
 assert.equal(h.requests.filter(value => value.path === '/users' && value.method === 'POST').length, 1);
});

test('Two same-session saves keep operation ownership when an older PUT finishes while the newer PUT is pending', async t => {
 const h = await app(t), old = deferred(), fresh = deferred(); h.setInventory(configured('current'));
 h.queue('/proxmox', old.promise); h.queue('/proxmox', fresh.promise);
 const first = system.saveProxmox(h.qa.ctx, {}).catch(error => error.code);
 const second = system.saveProxmox(h.qa.ctx, {}); await h.settle();
 const reads = h.requests.filter(value => value.path === '/inventory').length;
 old.resolve(response({ ok: true })); assert.equal(await first, 'admin_session_changed'); await h.settle();
 assert.equal(h.requests.filter(value => value.path === '/inventory').length, reads, 'superseded owner cannot start verification for the newer operation');
 assert.equal(h.node('#notice').textContent, '');
 fresh.resolve(response({ ok: true })); await second;
 assert.match(h.node('#notice').textContent, /등록된 VM의 상태/);
 assert.equal(h.requests.filter(value => value.path === '/proxmox' && value.method === 'PUT').length, 2, 'two explicit writes only; no replay');
});

test('Page action keys isolate sibling work and reject superseded/same-object-login ownership', async t => {
 const h = await app(t), first = h.qa.ctx.pageAction('assignment-save'), second = h.qa.ctx.pageAction('assignment-save'), sibling = h.qa.ctx.pageAction('console-permission');
 assert.equal(first.current(), false); assert.throws(() => first.check(), error => error.code === 'admin_session_changed');
 assert.equal(second.current(), true); assert.equal(sibling.current(), true);
 h.qa.showLogin(); await h.login(h.user);
 assert.equal(second.current(), false); assert.equal(sibling.current(), false);
 const current = h.qa.ctx.pageAction('assignment-save'); assert.equal(current.current(), true);
});

// 데스크톱 > 쓰는 사람 with target's sharing opened: the real ‘글·그림 복사’ choice,
// which saves at once (nothing is turned off, so it does not ask first).
function chooseDesktopSharing(h) {
 h.qa.state.data.assignments = [{ desktop_id: targetDesktop.id, username: 'target' }];
 h.qa.state.ui.sharingOpen = targetUser.id + ':' + targetDesktop.id;
 const page = el('main', {}, desktops.detail(h.qa.ctx, { page: 'desktop', id: targetDesktop.id }));
 const choice = page.querySelector('#desktop-people button[data-preset="clipboard"][data-account="target"]'); assert.ok(choice);
 assert.equal(choice.getAttribute('aria-pressed'), 'false');
 choice.dispatchEvent(new h.window.Event('click'));
}
test('Desktop sharing saved under an old login cannot mark, notify or clear the shared assignment lock of a newer login', async t => {
 const h = await app(t), old = deferred(), fresh = deferred(), path = '/desktops/pve-149/permissions'; await seed(h);
 h.queue(path, old.promise); chooseDesktopSharing(h); await h.settle(); assert.equal(h.qa.state.assignmentSaving, true);
 h.qa.showLogin(); await h.login(administrator('admin-b')); await seed(h);
 h.queue(path, fresh.promise); chooseDesktopSharing(h); await h.settle();
 old.resolve(response({ policiesChanged: 1 })); await h.settle();
 assert.equal(h.qa.state.assignmentSaving, true); assert.equal(h.qa.state.ui.sharingSaved, undefined, 'the old answer cannot mark the newer login row as saved');
 assert.equal(h.node('#notice').textContent, '');
 fresh.resolve(response({ policiesChanged: 1 })); await h.settle();
 assert.equal(h.qa.state.assignmentSaving, false); assert.match(h.node('#notice').textContent, /^저장했어요\.\ntarget · Fixture VM: 글·그림 복사/);
 assert.deepEqual(h.qa.state.ui.sharingSaved.keys, ['target-id:pve-149']);
 const writes = h.requests.filter(value => value.path === path && value.method === 'PUT');
 assert.equal(writes.length, 2, 'one write per explicit choice; the old one is never replayed');
 for (const write of writes) assert.deepEqual(write.body, { permissions: [{ username: 'target', expectedAccountId: 'target-id',
  clipboardPolicy: { textToRemote: true, textToLocal: true, filesToRemote: false, filesToLocal: false },
  devicePolicy: { drives: false, printers: false, usbDevices: false, smartCards: false, serialPorts: false } }] });
});
