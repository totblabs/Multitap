// Actual Proxmox adapter, only its HTTPS-call/socket-construction boundaries
// are replaced. No request, listener, TLS connection or credential is opened.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { Proxmox } from '../src/proxmox.mjs';
import { HttpError } from '../src/errors.mjs';
const config={endpoint:'https://pve.fixture.invalid:8006',tokenId:'fixture@pve!fixture',secret:'SERVER_ONLY_SECRET',ca:'configured-ca-only'};
const uuid='12345678-1234-1234-1234-123456789abc';
const snapshot={node:'node-one',vmid:149,machineUuid:uuid};
const target={node:'node-one',expectedUuid:uuid};
class Socket extends EventEmitter{readyState=0;terminate(){this.readyState=3;this.emit('close');}}
test('Permission value0 is a valid non-propagating grant and identity comes from the exact registered current config',async()=>{
  const p=new Proxmox(),calls=[];
  p.call=async(_c,path)=>{calls.push(path);if(path.startsWith('/access/permissions'))return {'/vms/149':{'VM.Console':0}};if(path==='/nodes/node-one/qemu/149/status/current')return {status:'running'};if(path==='/nodes/node-one/qemu/149/config?current=1')return {smbios1:'uuid='+uuid};throw Error('unexpected-boundary');};
  assert.deepEqual(await p.consoleSnapshot(config,149,target),snapshot);
  assert.deepEqual(calls,['/access/permissions?path=%2Fvms%2F149','/nodes/node-one/qemu/149/config?current=1','/nodes/node-one/qemu/149/status/current']);
});
test('Missing VM.Console refuses before inventory/config/proxy and does not change privileges',async()=>{
  const p=new Proxmox(),calls=[];p.call=async(_c,path,method)=>{calls.push([path,method]);return {'/vms/149':{'VM.Audit':1}};};
  await assert.rejects(p.consoleSnapshot(config,149,target),e=>e.code==='console_permission_denied'&&e.status===403);
  assert.deepEqual(calls,[['/access/permissions?path=%2Fvms%2F149','GET']]);
});
test('QEMU preparation model proof comes only from supported fresh VM configuration',async()=>{
  for(const [configuration,expected] of [[{vga:'std'},'std'],[{ostype:'win11'},'std'],[{vga:'qxl'},undefined],[{ostype:'win11',hostpci0:'0000:00:01.0'},'std'],[{vga:'virtio,memory=32'},undefined]]){
    const p=new Proxmox();p.call=async(_c,path)=>path.startsWith('/access/')?{'/vms/149':{'VM.Console':1}}:path.endsWith('/status/current')?{status:'running'}:{smbios1:'uuid='+uuid,...configuration};
    const value=await p.consoleSnapshot(config,149,target);assert.equal(value.model,expected);assert.equal(value.machineUuid,uuid);
  }
});
// A VM whose Proxmox display is none has no picture: the
// Console says so (console_no_display) before any proxy ticket is asked for.
test('A VM without a picture (display none or serial, or a GPU as primary VGA) refuses the Console with console_no_display',async()=>{
  for(const configuration of [{vga:'none'},{vga:'type=none'},{vga:'serial0'},{vga:'type=serial1,memory=16'},{ostype:'win11',hostpci0:'0000:00:01.0,x-vga=1'},{hostpci1:'mapping=gpu,pcie=1,x-vga=on'}]){
    const p=new Proxmox(),calls=[];p.call=async(_c,path)=>{calls.push(path);return path.startsWith('/access/')?{'/vms/149':{'VM.Console':1}}:path.endsWith('/status/current')?{status:'running'}:{smbios1:'uuid='+uuid,...configuration};};
    await assert.rejects(p.consoleSnapshot(config,149,target),e=>e.code==='console_no_display'&&e.status===409,JSON.stringify(configuration));
    assert.equal(calls.some(path=>path.includes('vncproxy')),false);
  }
});
test('Absent/nonrunning VM and missing SMBIOS identity do not manufacture console readiness',async()=>{
  for(const mode of ['missing','stopped','uuid']){
    const p=new Proxmox();p.call=async(_c,path)=>{assert.equal(path.startsWith('/cluster/'),false);if(path.startsWith('/access/'))return {'/vms/149':{'VM.Console':1}};if(mode==='missing')throw new HttpError(404,'proxmox_vm_not_found');if(path.endsWith('/status/current'))return {status:mode==='stopped'?'stopped':'running'};return mode==='uuid'?{}:{smbios1:'uuid='+uuid};};
    await assert.rejects(p.consoleSnapshot(config,149,target),e=>e.code===(mode==='uuid'?'proxmox_machine_uuid_missing':mode==='missing'?'proxmox_vm_not_found':'console_vm_unavailable'));
  }
});
for(const password of [undefined,'explicit-new-password'])test(`Console adapter uses ${password?'explicit':'legacy-ticket'} password only inside server and preserves verified TLS`,async()=>{
  let captured;const socket=new Socket();const p=new Proxmox({consoleSocket:(url,protocols,options)=>{captured={url,protocols,options};return socket;}});
  const calls=[];p.call=async(_c,path,method,body)=>{calls.push([path,method,body]);return {port:5901,ticket:'SERVER_ONLY_VNC_TICKET',...(password?{password}:{})};};
  const abort=new AbortController(),result=await p.openConsole(config,snapshot,{signal:abort.signal});
  assert.equal(result.socket,socket);assert.equal(result.password,password??'SERVER_ONLY_VNC_TICKET');
  assert.deepEqual(calls,[['/nodes/node-one/qemu/149/vncproxy','POST','websocket=1']]);
  assert.equal(captured.url.origin,'wss://pve.fixture.invalid:8006');assert.equal(captured.url.pathname,'/api2/json/nodes/node-one/qemu/149/vncwebsocket');
  assert.equal(captured.url.searchParams.get('vncticket'),'SERVER_ONLY_VNC_TICKET');
  assert.deepEqual(captured.protocols,['binary']);assert.equal(captured.options.rejectUnauthorized,true);assert.equal(captured.options.ca,config.ca);
  assert.equal(captured.options.followRedirects,false);assert.equal(captured.options.headers.Authorization,'PVEAPIToken=fixture@pve!fixture=SERVER_ONLY_SECRET');
  abort.abort();assert.equal(socket.readyState,3);
});
test('Cancelled or malformed proxy responses cannot dial an arbitrary target',async()=>{
  for(const value of [{port:22,ticket:'server-only'},{port:5901,ticket:'bad\nheader'},{port:5901,ticket:'ok',password:''}]){
    let dials=0;const p=new Proxmox({consoleSocket:()=>{++dials;throw Error('must-not-dial');}});p.call=async()=>value;
    await assert.rejects(p.openConsole(config,snapshot),e=>e.code==='console_invalid_upstream');assert.equal(dials,0);
  }
  const abort=new AbortController();abort.abort();const p=new Proxmox();p.call=async()=>{throw Error('must-not-request');};await assert.rejects(p.openConsole(config,snapshot,{signal:abort.signal}),e=>e.code==='console_cancelled');
});
test('Official PVE VNC port range is inclusive5900..5999 and does not allow adjacent ports',async()=>{
  for(const port of [5899,5900,5999,6000]){
    let dials=0;const p=new Proxmox({consoleSocket:url=>{++dials;assert.equal(url.searchParams.get('port'),String(port));return new Socket();}});
    p.call=async()=>({port,ticket:'synthetic-only'});
    if(port===5900||port===5999){const connection=await p.openConsole(config,snapshot);assert.equal(dials,1);connection.socket.terminate();}
    else{await assert.rejects(p.openConsole(config,snapshot),e=>e.code==='console_invalid_upstream');assert.equal(dials,0);}
  }
});

test('Real PVE numeric-string VNC ports use the same bounded authorized websocket endpoint',async()=>{
  for(const port of ['5900','5999']){
    let captured,dials=0;const socket=new Socket();
    const p=new Proxmox({consoleSocket:(url,protocols,options)=>{++dials;captured={url,protocols,options};return socket;}});
    p.call=async()=>({port,ticket:'SYNTHETIC_VNC_TICKET',password:'synthetic'});
    const connection=await p.openConsole(config,snapshot);
    assert.equal(dials,1);assert.equal(captured.url.origin,'wss://pve.fixture.invalid:8006');
    assert.equal(captured.url.pathname,'/api2/json/nodes/node-one/qemu/149/vncwebsocket');
    assert.equal(captured.url.searchParams.get('port'),port);
    assert.equal(captured.options.rejectUnauthorized,true);assert.equal(captured.options.followRedirects,false);
    assert.equal(connection.password,'synthetic');connection.socket.terminate();
  }
});

test('Port normalization rejects noncanonical strings and nonnumeric values before any websocket dial',async()=>{
  for(const port of ['5899','6000','05900','5900\n',' 5900','5900 ','+5900','5.9e3','5900.0','0x170c','５９００','',null,true,false,[],[5900],{},5900.5,NaN,Infinity]){
    let dials=0;const p=new Proxmox({consoleSocket:()=>{++dials;throw Error('must-not-dial');}});
    p.call=async()=>({port,ticket:'SYNTHETIC_VNC_TICKET'});
    await assert.rejects(p.openConsole(config,snapshot),e=>e.code==='console_invalid_upstream');assert.equal(dials,0);
  }
});
