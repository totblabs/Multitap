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



const hosts = await import('../admin/js/hosts.js');
const desktop = { id: 'pve-fixture', name: 'Fixture VM', pve_vmid: 700, kind: 'vm' };
function seed(h, values = [desktop]) {
 h.qa.state.data.hostUpdates = { latest: { version: '9.0.0', changelog: 'Fixture' }, desktops: Object.fromEntries(values.map(value => [value.id, { available: true, sessionActive: true, version: '8.0.0', latestVersion: '9.0.0' }])) };
}
const endpoint = value => '/desktops/' + value.id + '/host-update';
function begin(h, value = desktop) {
 hosts.hostUpdateForm(h.qa.ctx, value);
 const button = [...h.node('#modal-fields').querySelectorAll('button')].find(node => node.textContent.includes('지금 업데이트')); assert.ok(button); return button;
}
const accepted = () => response({ hostUpdate: { operation: { status: 'requested' } } });
const refused = () => response({ error: 'host_offline' }, 409);

for (const outcome of ['success', 'failure']) for (const replacement of ['modal', 'new-login', 'same-object-login'])
test('Late immediate Host update ' + outcome + ' cannot change a pending ' + replacement + ' modal or unlock its controls', async t => {
 const h = await app(t), old = deferred(), fresh = deferred(); seed(h);
 const now = begin(h); h.queue(endpoint(desktop), old.promise); now.dispatchEvent(new h.window.Event('click')); await h.settle();
 assert.equal(h.requests.filter(value => value.path === endpoint(desktop)).length, 1);
 let prevented = false; h.dialog.oncancel({ preventDefault() { prevented = true; } }); assert.equal(prevented, true, 'special action shares normal submitting Escape guard');
 // Programmatic replacement and logout must remain safe even though ordinary
 // Escape is blocked while a submitted operation is in flight.
 if (replacement !== 'modal') { h.qa.showLogin(); await h.login(replacement === 'same-object-login' ? h.user : administrator('admin-b')); }
 h.qa.modal('Unrelated draft', [{ name: 'draft', label: 'Draft', value: 'new draft' }], () => fresh.promise, { reload: false });
 const freshPending = h.node('#modal-form').onsubmit({ preventDefault() {} });
 assert.equal(h.node('#modal-save').disabled, true);
 const before = h.node('#notice').textContent;
 old.resolve(outcome === 'success' ? accepted() : refused()); await h.settle();
 assert.equal(h.dialog.open, true); assert.equal(h.node('#modal-fields').querySelector('[name=draft]').value, 'new draft');
 assert.equal(h.node('#modal-error').textContent, ''); assert.equal(h.node('#notice').textContent, before);
 for (const selector of ['#modal-save', '#modal-close', '#modal-cancel']) assert.equal(h.node(selector).disabled, true, 'old finally cannot release the new submission');
 assert.equal(h.requests.filter(value => value.path === endpoint(desktop)).length, 1, 'modal replacement never cancels or replays accepted work');
 fresh.resolve(); await freshPending; assert.equal(h.dialog.open, false, 'current new owner completes normally');
});

for (const mode of ['immediate', 'wait']) test('Current ' + mode + ' Host submission is single-flight and supports one explicit retry after failure', async t => {
 const h = await app(t), hold = deferred(); seed(h); const now = begin(h); h.queue(endpoint(desktop), hold.promise);
 const submit = () => h.node('#modal-form').onsubmit({ preventDefault() {} });
 if (mode === 'immediate') now.dispatchEvent(new h.window.Event('click')); else void submit();
 await h.settle(); now.dispatchEvent(new h.window.Event('click')); void submit(); await h.settle();
 assert.equal(h.requests.filter(value => value.path === endpoint(desktop)).length, 1);
 for (const node of [now, h.node('#modal-save'), h.node('#modal-close'), h.node('#modal-cancel')]) assert.equal(node.disabled, true);
 hold.resolve(refused()); await h.settle(); assert.equal(h.dialog.open, true); assert.ok(h.node('#modal-error').textContent);
 for (const node of [now, h.node('#modal-save'), h.node('#modal-close'), h.node('#modal-cancel')]) assert.equal(node.disabled, false);
 assert.equal(h.requests.filter(value => value.path === endpoint(desktop)).length, 1, 'failed operation never retries itself');
 h.queue(endpoint(desktop), accepted());
 if (mode === 'immediate') now.dispatchEvent(new h.window.Event('click')); else await submit();
 await h.settle(); assert.equal(h.dialog.open, false); assert.equal(h.requests.filter(value => value.path === endpoint(desktop)).length, 2);
 assert.match(h.node('#notice').textContent, /업데이트를 요청/);
});

test('A detached immediate button cannot submit against a later modal revision', async t => {
 const h = await app(t); seed(h); const old = begin(h);
 const revision = h.qa.modal('Later draft', [{ name: 'draft', label: 'Draft', value: 'keep' }], async () => {}, { reload: false });
 assert.equal(typeof revision, 'number'); assert.equal(h.qa.ctx.modalCurrent(revision), true);
 old.dispatchEvent(new h.window.Event('click')); await h.settle();
 assert.equal(h.requests.filter(value => value.path === endpoint(desktop)).length, 0);
 assert.equal(h.node('#modal-fields').querySelector('[name=draft]').value, 'keep'); assert.equal(h.dialog.open, true);
});

test('Independent Hosts retain independent accepted results while the current modal alone owns controls', async t => {
 const h = await app(t), a = deferred(), b = deferred(), other = { ...desktop, id: 'pve-other', pve_vmid: 701 }; seed(h, [desktop, other]);
 const first = begin(h); h.queue(endpoint(desktop), a.promise); first.dispatchEvent(new h.window.Event('click')); await h.settle();
 const second = begin(h, other); h.queue(endpoint(other), b.promise); second.dispatchEvent(new h.window.Event('click')); await h.settle();
 a.resolve(accepted()); await h.settle(); assert.equal(h.dialog.open, true); assert.equal(second.disabled, true);
 assert.equal(h.qa.state.data.hostUpdates.desktops[desktop.id].operation.status, 'requested', 'same-session accepted work still reaches its own Host status');
 b.resolve(accepted()); await h.settle(); assert.equal(h.dialog.open, false);
 assert.equal(h.qa.state.data.hostUpdates.desktops[other.id].operation.status, 'requested');
 assert.equal(h.requests.filter(value => value.path === endpoint(desktop)).length, 1); assert.equal(h.requests.filter(value => value.path === endpoint(other)).length, 1);
});

// 일괄 업데이트 (데스크톱 > 업데이트, 설정): ‘연결을 끊고 N대 업데이트’ is one request for all,
// ‘접속 안 한 N대만’ is the single-Host request of each idle desktop, one after another.
const fleet = [{ ...desktop, id: 'pve-busy', name: 'Busy VM', pve_vmid: 710 }, { ...desktop, id: 'pve-idle-a', name: 'Idle A', pve_vmid: 711 }, { ...desktop, id: 'pve-idle-b', name: 'Idle B', pve_vmid: 712 }];
async function seedFleet(h) {
 h.setInventory({ ...inventory('fleet'), desktops: fleet.map(value => ({ ...value })),
  hostUpdates: { latest: { version: '9.0.0', changelog: 'Fixture' }, desktops: Object.fromEntries(fleet.map(value => [value.id, { available: true, connected: true, supported: true, newer: true, sessionActive: value.id === 'pve-busy', version: '8.0.0', latestVersion: '9.0.0' }])) } });
 await h.qa.refresh();
 h.qa.state.resources.set('hosts', { value: { hosts: fleet.map(value => ({ desktopId: value.id })) }, error: '', loading: false, at: Date.now() });
 h.qa.state.resources.set('sessions', { value: { sessions: [{ sessionId: 'fixture-session', desktopId: 'pve-busy', account: 'fixture-user' }] }, error: '', loading: false, at: Date.now() });
}
function beginBulk(h) {
 hosts.hostUpdateAll(h.qa.ctx);
 assert.equal(h.node('#modal-title').textContent, '데스크톱 3대를 업데이트할까요?');
 assert.equal(h.node('#modal-fields .confirm-who').textContent, '접속 중인 1명의 연결을 먼저 끊어요');
 assert.equal(h.node('#modal-save').textContent, '연결을 끊고 3대 업데이트');
 const idle = [...h.node('#modal-fields').querySelectorAll('button')].find(node => node.textContent === '접속 안 한 2대만'); assert.ok(idle); return idle;
}
const hostPosts = h => h.requests.filter(value => value.method === 'POST' && /\/host-updates?(\/all)?$/.test(value.path));
const click = (h, node) => node.dispatchEvent(new h.window.Event('click'));

test('Idle-only bulk update asks each idle Host once, in turn, and never the connected one', async t => {
 const h = await app(t), first = deferred(); await seedFleet(h); const idle = beginBulk(h);
 assert.deepEqual(hostPosts(h), [], 'opening the question sends nothing');
 h.queue(endpoint(fleet[1]), first.promise); h.queue(endpoint(fleet[2]), accepted());
 click(h, idle); await h.settle();
 assert.deepEqual(hostPosts(h).map(value => value.path), [endpoint(fleet[1])], 'one Host at a time');
 click(h, idle); void h.node('#modal-form').onsubmit({ preventDefault() {} }); await h.settle();
 assert.deepEqual(hostPosts(h).map(value => value.path), [endpoint(fleet[1])], 'neither button starts a second run while one is in flight');
 for (const node of [idle, h.node('#modal-save'), h.node('#modal-close'), h.node('#modal-cancel')]) assert.equal(node.disabled, true);
 first.resolve(accepted()); await h.settle();
 assert.deepEqual(hostPosts(h), [fleet[1], fleet[2]].map(value => ({ path: endpoint(value), method: 'POST', hasAbortSignal: false, body: { version: '9.0.0', disconnectSessions: false } })),
  'the connected desktop is not asked, nothing is disconnected, and the all-Hosts request is not sent');
 assert.equal(h.dialog.open, false);
 assert.deepEqual(h.qa.state.ui.hostBatch, { version: '9.0.0', ids: ['pve-idle-a', 'pve-idle-b'], skipped: 0 });
 assert.match(h.node('#notice').textContent, /^Host 9\.0\.0 업데이트를 2대에 요청했어요\./);
});

for (const mode of ['idle-only', 'all']) for (const sameUser of [false, true])
test('Bulk ' + mode + ' update answered after a ' + (sameUser ? 'same-object' : 'new') + ' login asks no further Host and shows no result', async t => {
 const h = await app(t), held = deferred(); await seedFleet(h); const idle = beginBulk(h);
 const path = mode === 'all' ? '/host-updates/all' : endpoint(fleet[1]);
 h.queue(path, held.promise);
 if (mode === 'all') void h.node('#modal-form').onsubmit({ preventDefault() {} }); else click(h, idle);
 await h.settle();
 assert.deepEqual(hostPosts(h).map(value => value.path), [path]);
 h.qa.showLogin(); await h.login(sameUser ? h.user : administrator('admin-b')); await seedFleet(h);
 const before = h.node('#notice').textContent;
 held.resolve(mode === 'all' ? response({ results: fleet.map(value => ({ desktopId: value.id, started: true })) }) : accepted()); await h.settle();
 assert.deepEqual(hostPosts(h).map(value => value.path), [path], 'the remaining Hosts are not asked under the later login, and the accepted request is not replayed');
 assert.equal(h.qa.state.ui.hostBatch, undefined, 'no result panel in the later login');
 assert.equal(h.node('#notice').textContent, before);
 assert.equal(h.qa.state.data.hostUpdates.desktops['pve-idle-a'].operation, undefined, 'the old answer is not written into the later login inventory');
 assert.equal(h.dialog.open, false);
});
