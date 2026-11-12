import test from 'node:test';
import assert from 'node:assert/strict';
import { safeEncoderInfo, safeHostEncoderDiagnostic } from '../src/encoder-diagnostics.mjs';
import { safeDiagnosticReport } from '../src/host-diagnostics.mjs';
const info = () => ({ protocol:'h264-encoder-v1',backend:'nvenc',initialized:true,hardwareVerified:false,
 adapterLuid:'fedcba98:76543210',vendorId:0x10de,deviceId:0x2684,adapterName:'Fixture NVIDIA adapter',gpuFallbackCode:0,softwareFallbackCode:0 });
const observation = () => ({ protocol:'host-encoder-diagnostics-v1',observed:true,observationAgeMs:0,tracks:[{id:'0',epoch:1,info:info()}] });
const report = () => ({protocol:'host-diagnostics-v1',source:'host-service',version:'0.2.100',collectedAt:'2026-10-29T00:00:00.000Z',interactiveSession:true,consoleSession:true,captureAvailable:false,
 display:{observed:true,observationAgeMs:0,installed:true,deviceReady:true,owned:true,activeDisplay:false,configurationPending:true,recoveryRequired:true,rebootRequired:false,remoteSession:false,monitorCount:0,width:0,height:0,reason:'native_topology_recovery_required'},native:{available:false},events:[]});
test('encoder observation keeps initialization separate from verified hardware output',()=>{
 assert.equal(safeEncoderInfo(info()).hardwareVerified,false);
 assert.equal(safeEncoderInfo({...info(),hardwareVerified:true}).hardwareVerified,true);
 for(const invalid of [{...info(),hardwareVerified:1},{...info(),initialized:false},{...info(),hardwareVerified:true,adapterLuid:null},{...info(),hardwareVerified:true,vendorId:0x1002},{...info(),backend:'x264'}])assert.equal(safeEncoderInfo(invalid),null);
});
test('LUID full64bit text, vendor and fixed fallback vocabulary are bounded',()=>{
 assert.equal(safeEncoderInfo({...info(),adapterLuid:'ffffffff:ffffffff'}).adapterLuid,'ffffffff:ffffffff');
 for(const change of [{adapterLuid:2**64},{adapterLuid:'FEDCBA98:76543210'},{adapterLuid:'00000000:00000000'},{vendorId:0x100000000},{deviceId:-1},{gpuFallbackCode:31},{gpuFallbackCode:45},{softwareFallbackCode:8},{adapterName:'bad\nname'},{adapterName:'x'.repeat(129)}])assert.equal(safeEncoderInfo({...info(),...change}),null);
 for(const [backend,vendorId] of [['amf',0x1002],['qsv-mft',0x8086]])assert.equal(safeEncoderInfo({...info(),backend,vendorId,hardwareVerified:true}).vendorId,vendorId);
});
test('CPU fallback cannot retain selected hardware identity and keeps fixed cause',()=>{
 const cpu={...info(),backend:'x264',adapterLuid:null,vendorId:null,deviceId:null,adapterName:null,gpuFallbackCode:43};
 assert.equal(safeEncoderInfo(cpu).gpuFallbackCode,43);assert.equal(safeEncoderInfo({...cpu,hardwareVerified:true}),null);
});
test('current per-track snapshots preserve mixed GPU/CPU and stale becomes unknown',()=>{
 const observed=observation(),cpu={...info(),backend:'mft',adapterLuid:null,vendorId:null,deviceId:null,adapterName:null};
 observed.tracks.push({id:'1',epoch:9,info:cpu});const safe=safeHostEncoderDiagnostic(observed);
 assert.equal(safe.tracks[0].info.backend,'nvenc');assert.equal(safe.tracks[1].info.backend,'mft');
 assert.equal(safeHostEncoderDiagnostic({...observed,observationAgeMs:5001}),null);
 assert.equal(safeHostEncoderDiagnostic({...observed,tracks:[observed.tracks[0],observed.tracks[0]]}),null);
 assert.deepEqual(safeHostEncoderDiagnostic({protocol:'host-encoder-diagnostics-v1',observed:false,observationAgeMs:null,tracks:[]}),{protocol:'host-encoder-diagnostics-v1',observed:false,observationAgeMs:null,tracks:[]});
});
test('optional encoder data never changes base Host readiness or poisons old reports',()=>{
 const old=safeDiagnosticReport(report());assert.equal(Object.hasOwn(old,'encoder'),false);
 const valid=safeDiagnosticReport({...report(),encoder:observation()});assert.ok(valid.encoder);assert.equal(valid.captureAvailable,false);assert.equal(valid.display.activeDisplay,false);
 const bad=safeDiagnosticReport({...report(),encoder:{...observation(),observationAgeMs:-1}});assert.ok(bad);assert.equal(Object.hasOwn(bad,'encoder'),false);
});
