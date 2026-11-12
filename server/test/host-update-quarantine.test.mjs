import test from 'node:test';import assert from 'node:assert/strict';import{createHash}from'node:crypto';
import{Store}from'../src/store.mjs';import{HostUpdates,hostUpdateProtocol}from'../src/host-update.mjs';
const hash=v=>createHash('sha256').update(v).digest('hex'),uuid='12345678-1234-4234-8234-123456789abc';
const release={product:'host',format:'installer',version:'9.0.2',filename:'Multitap-Host-9.0.2-windows-x64.exe',url:'/downloads/Multitap-Host-9.0.2-windows-x64.exe',sha256:'a'.repeat(64),size:1000};
function fixture(t){
 const store=new Store(':memory:');store.syncProxmox([{node:'fixture',vmid:710,name:'Synthetic VM',status:'running',machineUuid:uuid}],'fixture');
 const code=hash('synthetic-enrollment'),tokenHash=hash('synthetic-host');store.issueEnrollment('pve-710',uuid,code,'fixture');store.consumeEnrollment(code,uuid,{hostname:'Synthetic Host',version:'1.0.12',tokenHash});
 const host={identity:store.authenticateHost(tokenHash),tokenHash,updateProtocol:hostUpdateProtocol,ws:{readyState:1}},sent=[];
 const relay={store,hosts:new Map([['pve-710',host]]),sessions:new Map(),displayBusy:new Map([['pve-710',{}]]),powerBusy:()=>false,sessionStop:{blocked:()=>false},assertAccessAdmission(){},send(_ws,value){sent.push(value);return true;},dropHost(){assert.fail('No transport mutation expected');}};
 const updates=new HostUpdates(relay,{catalog:async()=>[release],tickMs:60000,retryMs:0});t.after(()=>{updates.close();store.close();});
 const wait=()=>updates.request({actor:'synthetic-admin',desktopId:'pve-710',version:release.version,authorize:()=>({})});
 const observation=powerError=>{const targets=store.registeredVmTargets(),target=targets[0];store.refreshRegisteredVms(targets,[{node:target.node,vmid:target.vmid,status:'unknown',expectedUuid:uuid,powerError}],'fixture',()=>{});};
 return{store,host,relay,sent,updates,wait,observation};
}
for(const mode of ['healthy','unknown-transport','uuid-mismatch','revoked','deleted','wrong-map','wrong-identity','auth-throws'])test('queued update fresh Host authority: '+mode,async t=>{
 const f=fixture(t);await f.wait();assert.equal(f.updates.operations.get('pve-710').status,'waiting');
 if(mode==='unknown-transport')f.observation('proxmox_unreachable');
 else if(mode==='uuid-mismatch')f.observation('proxmox_machine_uuid_mismatch');
 else if(mode==='revoked')f.store.db.prepare('UPDATE hosts SET revoked=1 WHERE id=?').run(f.host.identity.id);
 else if(mode==='deleted')f.store.db.prepare('DELETE FROM hosts WHERE id=?').run(f.host.identity.id);
 else if(mode==='wrong-map')f.relay.hosts.set('pve-710',{...f.host,identity:{...f.host.identity,id:'foreign-current-host',version:release.version}});
 else if(mode==='wrong-identity')f.host.identity={...f.host.identity,desktop_id:'pve-711'};
 else if(mode==='auth-throws')f.store.authenticateHost=()=>{throw Error('synthetic-private-auth-failure');};
 const allowed=['healthy','unknown-transport'].includes(mode),view=f.updates.view('pve-710',release);assert.equal(view.connected,allowed);assert.equal(view.supported,allowed);
 f.relay.displayBusy.clear();f.updates.tick();assert.equal(f.sent.filter(x=>x.type==='host.update.start').length,allowed?1:0);
 const op=f.updates.operations.get('pve-710');assert.equal(op.status,allowed?'dispatched':'unknown');if(!allowed)assert.equal(op.code,'host_disconnected');
});
test('direct stale Host dispatch does not resolve target-version completion before authentication',async t=>{const f=fixture(t);await f.wait();f.host.identity.version=release.version;f.observation('proxmox_machine_uuid_mismatch');f.updates.dispatch(f.updates.operations.get('pve-710'));assert.equal(f.sent.length,0);assert.equal(f.updates.operations.get('pve-710').status,'unknown');});
