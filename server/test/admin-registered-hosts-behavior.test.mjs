import {test} from 'node:test';
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {parseHTML} from 'linkedom';
import {el} from '../admin/js/dom.js';
import * as desktops from '../admin/js/desktops.js';
import * as overview from '../admin/js/overview.js';
const appUrl=new URL('../admin/app.js',import.meta.url);
let serial=0;
const reply=(body,status=200)=>({ok:status<400,status,json:async()=>body});
const vm=(id,number,name,power='running')=>({id,kind:'vm',name,pve_vmid:number,pve_node:'fixture-node',pve_os_family:'windows',pve_os_type:'win11',pve_available:1,pve_power_status:power,host:{connected:id==='registered-on',version:'1.0.14'}});
const fixtureDesktops=()=>[vm('registered-on',245,'REGISTERED-ONLINE'),vm('registered-off',247,'REGISTERED-OFFLINE','stopped'),vm('not-enrolled',249,'UNREGISTERED-VM'),{id:'registered-pc',kind:'pc',name:'REGISTERED-PC',localPcApproved:true,host:{connected:false}},{id:'no-host-pc',kind:'pc',name:'APPROVED-PC-WITHOUT-HOST',localPcApproved:true,host:{connected:false}},{id:'unapproved-pc',kind:'pc',name:'UNAPPROVED-PC',localPcApproved:false}];
const fixtureHosts=()=>({hosts:[['registered-on',true],['registered-off',false],['registered-pc',false]].map(([desktopId,connected],i)=>({desktopId,hostId:`00000000-0000-4000-8000-00000000000${i+1}`,hostname:'fixture-host',version:'1.0.14',connected,enrolledAt:'2026-11-01T00:00:00Z',lastSeenAt:'2026-11-01T00:00:00Z'}))});
async function app(t,{filteredInventory=false}={}){
 const window=parseHTML(await readFile(new URL('../admin/index.html',import.meta.url),'utf8'));
 const keys=['document','Node','fetch','FormData','location','sessionStorage','matchMedia','addEventListener'];const previous=Object.fromEntries(keys.map(k=>[k,globalThis[k]]));
 globalThis.document=window.document;globalThis.Node=window.Node;globalThis.location=new URL('https://admin.fixture.invalid/admin/');globalThis.sessionStorage={getItem:()=>null,setItem(){}};globalThis.matchMedia=()=>({matches:false,addEventListener(){}});globalThis.addEventListener=()=>{};
 const formProto=Object.getPrototypeOf(document.querySelector('form'));formProto.reset=function(){for(const n of this.querySelectorAll('input,textarea'))n.value='';};Object.defineProperty(formProto,'elements',{configurable:true,get(){return Object.fromEntries([...this.querySelectorAll('[name]')].map(n=>[n.name,n]));}});
 globalThis.FormData=class{constructor(form){this.rows=[...form.querySelectorAll('[name]')].map(n=>[n.name,n.value]);}*[Symbol.iterator](){yield* this.rows;}};
 const dialog=document.querySelector('#modal');dialog.showModal=()=>{dialog.open=true;};dialog.close=()=>{dialog.open=false;};
 const user={id:'fixture-admin',username:'fixture-admin',role:'admin',mustChangeCredentials:false};
 const data={users:[user],desktops:fixtureDesktops(),assignments:[],audit:[],proxmox:{configured:true,state:'ok',checkedAt:'2026-11-05T00:00:00Z',endpoint:'https://pve.fixture.invalid:8006',hasSecret:true},onboarding:{complete:true,proxmoxRequired:false},hostUpdates:{latest:null,desktops:{}}};
 if(filteredInventory){const ids=new Set(fixtureHosts().hosts.map(h=>h.desktopId));data.desktops=data.desktops.filter(d=>ids.has(d.id));}
 const writes=[];
 globalThis.fetch=async(url,options={})=>{const p=url.replace('/api/v1/admin','');if(options.method&&options.method!=='GET')writes.push({path:p,method:options.method});if(p==='/session')return reply({user,csrfToken:'fixture-csrf'});if(p==='/inventory')return reply(data);if(p==='/hosts')return reply(fixtureHosts());if(p==='/proxmox/status'||p==='/proxmox/sync')return reply({proxmox:data.proxmox});if(p==='/server-info')return reply({service:'multitap-broker',version:'1.0.15',deployment:{kind:'unknown',configured:false},update:{state:'not_configured',feedConfigured:false,applySupported:false}});if(url==='/api/v1/downloads')return reply({releases:[]});return reply({sessions:[],events:[],requests:[],pendingCount:0,hosts:[]});};
 const source=(await readFile(appUrl,'utf8')).replace(/from '(\.[^']+)'/g,(_,p)=>`from '${new URL(p,appUrl).href}'`);
 const {qa}=await import('data:text/javascript;base64,'+Buffer.from(source+`\nexport const qa={ctx,state,showLogin,searchMatches};\n//# sourceURL=registered-hosts-fixture-${++serial}.js`).toString('base64'));
 const settle=async()=>{for(let i=0;i<6;++i)await new Promise(r=>setImmediate(r));};await settle();
 t.after(()=>{qa.showLogin();for(const [k,v]of Object.entries(previous))globalThis[k]=v;});
 qa.state.data=data;qa.state.pveBusy=false;qa.state.pveError='';qa.state.pveLoaded=true;
 qa.state.resources.set('hosts',{value:fixtureHosts(),error:'',loading:false,at:Date.now()});
 qa.state.resources.set('hostRequests',{value:{requests:[]},error:'',loading:false,at:Date.now()});
 const render=(fn,route={})=>el('main',{},fn(qa.ctx,route));
 return {qa,data,writes,render,dialog,settle,hostsEntry:()=>qa.state.resources.get('hosts')};
}
const registeredIds=['registered-off','registered-on','registered-pc'];
const listIds=root=>[...root.querySelectorAll('.vm-list-item')].map(n=>n.getAttribute('data-focus')?.replace('vm-pick-','')).sort();
const tile=(root,label)=>[...root.querySelectorAll('nav.stat-tiles a.stat-tile')].find(n=>n.querySelector('span').textContent===label)?.querySelector('strong').textContent;
const noAllEscape=root=>assert.equal([...root.querySelectorAll('button,a')].some(n=>/전체 VM 보기/.test(n.textContent)||n.getAttribute('data-desktop-scope')==='all'),false,'there is no full-inventory escape');

// Actual UI modules with intentionally full synthetic inventory test the
// registration boundary. These tests do NOT prove backend projection/auth.
test('registered desktop list retains offline/stopped VM and approved registered PC only',async t=>{const h=await app(t);const root=h.render(desktops.list);assert.deepEqual(listIds(root),registeredIds);assert.match(root.textContent,/REGISTERED-OFFLINE/);assert.doesNotMatch(root.textContent,/UNREGISTERED-VM|APPROVED-PC-WITHOUT-HOST|UNAPPROVED-PC/);assert.deepEqual(h.writes,[]);});
test('remembered all scope cannot reopen unregistered inventory',async t=>{const h=await app(t);h.qa.state.ui.desktops={scope:'all',query:'',filter:'none',byNode:false,selected:'not-enrolled'};const root=h.render(desktops.list);assert.deepEqual(listIds(root),registeredIds);noAllEscape(root);});
test('unregistered deep link cannot select desktop or request its diagnostic resources',async t=>{const h=await app(t);const needs=desktops.workspaceNeeds(h.qa.ctx,{id:'not-enrolled'});assert.equal(needs.some(n=>n.endsWith(':not-enrolled')),false,'unregistered target diagnostics are not requested');const root=h.render(desktops.detail,{id:'not-enrolled'});assert.doesNotMatch(root.textContent,/UNREGISTERED-VM/);noAllEscape(root);assert.deepEqual(h.writes,[]);});
test('registered offline deep link remains addressable',async t=>{const h=await app(t);const needs=desktops.workspaceNeeds(h.qa.ctx,{id:'registered-off'});assert.ok(needs.includes('diagnostics:registered-off'));const root=h.render(desktops.detail,{id:'registered-off'});assert.match(root.textContent,/REGISTERED-OFFLINE/);noAllEscape(root);});
for(const kind of ['loading','error-with-stale-value','malformed','unknown'])test(`registration ${kind} fails closed even with remembered all scope`,async t=>{const h=await app(t);h.qa.state.ui.desktops={scope:'all',query:'',filter:'none',byNode:false,selected:'not-enrolled'};const entry=h.hostsEntry();if(kind==='loading'){entry.loading=true;entry.value=null;}if(kind==='error-with-stale-value')entry.error='fixture unavailable';if(kind==='malformed')entry.value={hosts:[{desktopId:42}]};if(kind==='unknown')entry.value=null;const root=h.render(desktops.list);assert.deepEqual(listIds(root),[]);assert.doesNotMatch(root.textContent,/REGISTERED-ONLINE|REGISTERED-OFFLINE|UNREGISTERED-VM/);noAllEscape(root);assert.deepEqual(h.writes,[]);});
test('empty registration has registration guidance and no all-VM escape',async t=>{const h=await app(t);h.hostsEntry().value={hosts:[]};const root=h.render(desktops.list);assert.deepEqual(listIds(root),[]);assert.match(root.textContent,/Host|등록/);noAllEscape(root);assert.deepEqual(h.writes,[]);});
test('pending enrollment keeps its matching unregistered VM available for approval only',async t=>{const h=await app(t);h.hostsEntry().value={hosts:[]};const request={requestId:'00000000-0000-4000-8000-000000000099',hostname:'PENDING-FIXTURE',machineUuid:'fixture-machine',version:'1.0.14',verificationCode:'ABCD',codeDisplay:'shown',status:'pending',expiresAt:'2099-01-01T00:00:00Z',candidates:[{desktopId:'not-enrolled',vmid:249,name:'UNREGISTERED-VM',hasExistingHost:false}]};h.qa.state.resources.get('hostRequests').value={requests:[request]};const root=h.render(desktops.list);const waiting=root.querySelector('section.requests-panel');assert.equal(waiting.querySelector('h2').textContent,'승인 대기1');assert.equal(waiting.querySelectorAll('.request-card').length,1);assert.match(waiting.querySelector('.request-match').textContent,/등록 정보가 있는 VM: VM 249 · UNREGISTERED-VM/);assert.deepEqual(listIds(root),[],'a VM waiting for approval is not a list row');waiting.remove();assert.doesNotMatch(root.textContent,/UNREGISTERED-VM/,'outside its request card the unregistered VM is not named');noAllEscape(root);const approve=[...waiting.querySelectorAll('button')].find(b=>/확인하고 승인/.test(b.textContent));assert.ok(approve&&!approve.disabled,'pending matching request remains actionable');approve.click();assert.equal(document.querySelector('#modal-title').textContent,'Host 연결 승인');assert.ok(h.dialog.open);assert.ok([...h.dialog.querySelectorAll('option')].some(n=>n.value==='not-enrolled'),'only matching request supplies the unregistered approval target');assert.deepEqual(h.writes,[]);});
// The console applies the registration boundary itself to the global search and to 홈,
// here with the intentionally full synthetic inventory.
test('global search lists registered desktops only',async t=>{const h=await app(t);assert.deepEqual(h.qa.searchMatches('registered').filter(n=>n.page==='desktop').map(n=>n.id).sort(),registeredIds);for(const query of['UNREGISTERED-VM','APPROVED-PC-WITHOUT-HOST','UNAPPROVED-PC','249'])assert.deepEqual(h.qa.searchMatches(query),[],query);});
test('home counts registered desktops only and names no other',async t=>{const h=await app(t);const root=h.render(overview.render);assert.equal(tile(root,'접속할 수 있는 데스크톱'),'1 / 3','one of the three registered desktops answers; the other two stay counted');assert.equal(tile(root,'접속 불가'),'0','a stopped VM and a silent local PC are off, not problems');assert.doesNotMatch(root.textContent,/UNREGISTERED-VM|APPROVED-PC-WITHOUT-HOST|UNAPPROVED-PC/);assert.equal(root.querySelector('[data-todo=unreachable]'),null);assert.deepEqual(h.writes,[]);});
for(const kind of ['loading','error-with-stale-value','malformed','unknown'])test(`home registration ${kind} leaves the desktop counts unknown`,async t=>{const h=await app(t);const entry=h.hostsEntry();if(kind==='loading'){entry.loading=true;entry.value=null;}if(kind==='error-with-stale-value')entry.error='fixture unavailable';if(kind==='malformed')entry.value={hosts:[{desktopId:42}]};if(kind==='unknown')entry.value=null;const root=h.render(overview.render);assert.equal(tile(root,'접속할 수 있는 데스크톱'),'—');assert.equal(tile(root,'접속 불가'),'—');assert.equal(root.querySelector('[data-todo=unreachable]'),null);assert.doesNotMatch(root.textContent,/REGISTERED-ONLINE|REGISTERED-OFFLINE|UNREGISTERED-VM/);assert.deepEqual(h.writes,[]);});
// These controls consume a synthetic REGISTERED-ONLY backend response. They
// qualify actual app search/overview consumption, NOT server-side filtering.
test('filtered-inventory CONTROL: actual app search keeps offline registered Host',async t=>{const h=await app(t,{filteredInventory:true});const results=h.qa.searchMatches('registered');assert.deepEqual(results.filter(n=>n.page==='desktop').map(n=>n.id).sort(),registeredIds);assert.deepEqual(h.qa.searchMatches('UNREGISTERED-VM'),[]);});
test('filtered-inventory CONTROL: actual overview uses registered desktop count and keeps the offline ones in it',async t=>{const h=await app(t,{filteredInventory:true});const root=h.render(overview.render);assert.doesNotMatch(root.textContent,/UNREGISTERED-VM|APPROVED-PC-WITHOUT-HOST/);assert.equal(tile(root,'접속할 수 있는 데스크톱'),'1 / 3');assert.equal(tile(root,'접속 불가'),'0');assert.equal(root.querySelectorAll('nav.stat-tiles a.stat-tile').length,4);});

// Proxmox transport freshness is NOT Host enrollment availability. Keep only
// previously known desktops backed by a valid registry; never invent a row
// for a registry orphan. Registry loading/error cases above stay fail closed.
for(const freshness of ['sync-busy','temporary-error'])test(`valid registered offline VM survives Proxmox ${freshness}`,async t=>{
 const h=await app(t);
 h.hostsEntry().value.hosts.push({...fixtureHosts().hosts[1],desktopId:'registry-orphan',hostId:'00000000-0000-4000-8000-000000000004'});
 if(freshness==='sync-busy')h.qa.state.pveBusy=true;
 else{h.qa.state.pveError='fixture temporary Proxmox error';h.qa.state.data.proxmox.state='error';}
 const root=h.render(desktops.list);
 assert.deepEqual(listIds(root),registeredIds,'last-known registered VM/PC rows survive PVE freshness changes');
 assert.match(root.textContent,/REGISTERED-OFFLINE/);
 assert.doesNotMatch(root.textContent,/UNREGISTERED-VM|APPROVED-PC-WITHOUT-HOST|UNAPPROVED-PC|registry-orphan/);
 assert.deepEqual(h.writes,[],'read-only stale-PVE rendering sends no power/enrollment request');
});
