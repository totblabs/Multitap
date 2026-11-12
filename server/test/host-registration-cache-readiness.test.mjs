import test from 'node:test';
import assert from 'node:assert/strict';
import {Store} from '../src/store.mjs';
import {Vault} from '../src/vault.mjs';
import {HostRequests} from '../src/host-requests.mjs';
import {Proxmox} from '../src/proxmox.mjs';
import {newToken} from '../src/security.mjs';

const uuid='12345678-1234-4234-8234-123456789abc';
function fixture(t,{unchecked=false,shown=true,observed=uuid}={}){
  const store=new Store(':memory:'),vault=new Vault(Buffer.alloc(32,19)),proxmox=new Proxmox(),calls=[];
  t.after(()=>store.close());
  if(unchecked)store.syncProxmox([{vmid:710,node:'node-a',name:'Existing unchecked VM',status:'running'}],'fixture');
  store.saveSetting('proxmox_initialized','1','fixture');
  store.saveSetting('proxmox',vault.seal({endpoint:'https://fixture.invalid',tokenId:'synthetic@pve!fixture',secret:'synthetic'}),'fixture');
  proxmox.call=async(_config,path,method='GET')=>{
    calls.push({path,method});assert.equal(method,'GET');assert.equal(path,'/nodes/node-a/qemu/710/config?current=1');
    return {smbios1:'uuid='+observed,ostype:'win11'};
  };
  const requests=new HostRequests({store,vault,proxmox}),request=requests.create({requestKey:newToken(),hostname:'Synthetic fixture Host',machineUuid:uuid,version:'1.0.12'},'fixture',shown);
  return {store,requests,request,calls};
}
for(const unchecked of [false,true])test('Host-first explicit mapping approves without cached candidates or global inventory: '+(unchecked?'unchecked existing row':'no existing row'),async t=>{
  const f=fixture(t,{unchecked}),pending=f.requests.pending().requests.find(value=>value.requestId===f.request.requestId);
  assert.equal(pending.matchState,'unknown');assert.deepEqual(pending.candidates,[]);
  if(unchecked)assert.equal(f.store.db.prepare("SELECT pve_machine_checked_at FROM desktops WHERE id='pve-710'").get().pve_machine_checked_at,null);
  const syncsBefore=f.store.db.prepare("SELECT count(*) n FROM audit_events WHERE event='proxmox_synchronized'").get().n;
  const result=await f.requests.approve(f.request.requestId,{node:'node-a',vmid:710,acknowledgeCode:true},()=> 'fixture');
  assert.equal(result.ok,true);assert.equal(result.status,'approved');assert.equal(result.desktopId,'pve-710');
  assert.deepEqual(f.calls,[{path:'/nodes/node-a/qemu/710/config?current=1',method:'GET'}]);
  assert.equal(f.store.db.prepare("SELECT count(*) n FROM audit_events WHERE event='proxmox_synchronized'").get().n,syncsBefore);
  assert.equal(f.store.db.prepare('SELECT count(*) n FROM desktops').get().n,1);
  assert.equal(f.store.db.prepare('SELECT machine_uuid FROM hosts').get().machine_uuid,uuid);
});
test('Host-first unchecked-cache approval still refuses authoritative UUID mismatch with no Host or discovery',async t=>{
  const f=fixture(t,{unchecked:true,observed:'22345678-1234-4234-8234-123456789abc'});
  await assert.rejects(f.requests.approve(f.request.requestId,{node:'node-a',vmid:710,acknowledgeCode:true},()=> 'fixture'),e=>e.code==='proxmox_machine_uuid_mismatch');
  assert.equal(f.store.db.prepare('SELECT count(*) n FROM hosts').get().n,0);
  assert.equal(f.calls.length,1);assert.equal(f.calls[0].path,'/nodes/node-a/qemu/710/config?current=1');
});
test('Host-first direct mapping requires shown code and explicit acknowledgment before any exact target query',async t=>{
  for(const [shown,input]of [[false,{acknowledgeCode:true}],[true,{}]]){
    const f=fixture(t,{shown});
    await assert.rejects(f.requests.approve(f.request.requestId,{node:'node-a',vmid:710,...input},()=> 'fixture'),e=>e.code==='host_request_code_required');
    assert.deepEqual(f.calls,[]);assert.equal(f.store.db.prepare('SELECT count(*) n FROM hosts').get().n,0);
  }
});
