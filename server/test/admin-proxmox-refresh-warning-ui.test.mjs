import {test} from 'node:test';
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
// app.js text comes from the catalog (docs/i18n.md); the callers below receive the same lookup.
import {t} from '../admin/js/i18n.js';

// Exact shipped callers, memory-only API/session collaborators; no server/VM.
const app=await readFile(new URL('../admin/app.js',import.meta.url),'utf8');
const start=app.indexOf('async function readProxmoxStatus()');
const end=app.indexOf('// Host program updates',start);
assert.ok(start>=0&&end>start);
const callers=app.slice(start,end);
const url=new URL('../admin/js/system.js',import.meta.url);
const source=(await readFile(url,'utf8')).replace(/from '(\.[^']+)'/g,(_,path)=>`from '${new URL(path,url).href}'`);
const system=await import('data:text/javascript;base64,'+Buffer.from(source+'\nexport {saveProxmox};').toString('base64'));

function fixture({count=0,direct=count,vmCount=3,changeSession=false}={}){
  const notices=[],calls=[],rows=Array.from({length:vmCount},(_,i)=>({id:'pve-'+(100+i),registered:true}));
  const state={user:{id:'admin-a'},csrf:'csrf-a',pveBusy:false,pveError:'',pveLoaded:true,route:{page:'proxmox'},data:{desktops:rows,proxmox:{configured:true}}};
  const config=()=>({configured:true,state:'ok',vmCount,unavailableCount:count});
  const api=async(path,method='GET')=>{
    calls.push({path,method});
    if(path==='/proxmox')return{ok:true};
    if(path==='/inventory')return{desktops:rows,proxmox:config()};
    if(path==='/proxmox/sync')return{unavailable:direct,proxmox:config()};
    if(path==='/proxmox/status'){
      if(changeSession)state.user={id:'admin-b'};
      return{proxmox:config()};
    }
    throw Error('unexpected fixture path '+path);
  };
  const notice=(text,error=false)=>notices.push({text,error});
  const methods=Function('state','api','notice','t',`let inventoryRevision=0;
    const sessionOwner=()=>state.user,sessionCurrent=owner=>owner===state.user;
    const sessionChanged=()=>Object.assign(Error('changed'),{code:'admin_session_changed'});
    const setupRequired=()=>false,render=()=>{},loadPage=()=>{},pushHash=()=>{};
    ${callers}\nreturn {readProxmoxStatus,syncProxmox};`)(state,api,notice,t);
  const ctx={state,api,notice,syncProxmox:methods.syncProxmox,pageAction:()=>{
    const owner=state.user,csrf=state.csrf;
    return{check(){if(state.user!==owner||state.csrf!==csrf)throw Error('changed');}};
  }};
  return{state,notices,calls,methods,save:()=>system.saveProxmox(ctx,{})};
}

test('healthy zero refresh preserves the existing success notice',async()=>{
  const f=fixture();await f.save();
  assert.deepEqual(f.notices,[{text:'Proxmox 연결과 등록된 VM의 상태를 확인했어요.',error:false}]);
  assert.equal(f.state.pveError,'');assert.equal(f.state.pveLoaded,true);
});
for(const [label,count] of [['partial',1],['all',3]])test(label+' failure warns once and preserves registered rows',async()=>{
  const f=fixture({count});await f.save();
  assert.equal(f.notices.length,1);assert.equal(f.notices[0].error,true);
  assert.match(f.notices[0].text,new RegExp(count+'대.*확인하지 못'));
  assert.match(f.notices[0].text,/기존 목록.*유지/);
  assert.doesNotMatch(f.notices[0].text,/상태를 확인했어요/);
  assert.equal(f.state.data.desktops.length,3);assert.equal(f.state.pveError,'');assert.equal(f.state.pveLoaded,true);
});
test('unknown status count does not invent a successful VM check',async()=>{
  const f=fixture({count:null,direct:null});await f.save();
  assert.equal(f.notices.length,1);assert.match(f.notices[0].text,/연결 설정을 저장/);
  assert.doesNotMatch(f.notices[0].text,/상태를 확인했어요|0대|null|undefined/);
});
test('direct sync unavailable count survives unknown follow-up metadata',async()=>{
  const f=fixture({count:null,direct:2});await f.save();
  assert.equal(f.state.data.proxmox.unavailableCount,2);
  assert.equal(f.notices.length,1);assert.equal(f.notices[0].error,true);
  assert.match(f.notices[0].text,/2대/);
});
test('explicit refresh warns without converting partial failure into a fatal load error',async()=>{
  const f=fixture({count:1});await f.methods.syncProxmox();
  assert.equal(f.notices.length,1);assert.equal(f.notices[0].error,true);
  assert.equal(f.state.pveError,'');assert.equal(f.state.data.desktops.length,3);
});
test('new principal never receives the old refresh notice or metadata',async()=>{
  const f=fixture({count:1,changeSession:true});
  await assert.rejects(f.save(),/changed/);
  assert.deepEqual(f.notices,[]);
});
test('zero registered VMs is a healthy refresh with no invented failure',async()=>{
  const f=fixture({count:0,vmCount:0});await f.save();
  assert.equal(f.notices.length,1);assert.equal(f.notices[0].error,false);
  assert.equal(f.state.data.desktops.length,0);
});
test('malformed unavailable counts remain unknown rather than coercing a warning',async()=>{
  for(const count of [-1,1.5,'2',Number.MAX_SAFE_INTEGER+1]){
    const f=fixture({count,direct:count});await f.save();
    assert.equal(f.notices.length,1);assert.equal(f.notices[0].error,false);
    assert.doesNotMatch(f.notices[0].text,/상태를 확인했어요|\d대/);
  }
});
