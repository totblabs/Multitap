import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { safeClientDiagnosticReport, receiveClientDiagnostics, clientDiagnostics, clientDiagnosticRetentionMs, clientDiagnosticBatchesPerUser as PER, clientDiagnosticBatchesTotal as TOTAL, clientDiagnosticNewestKept as KEPT, viewportStageNumeric, connectTimingNumeric } from '../src/client-diagnostics.mjs';

const operationId = '614c03e4-5595-4d77-a075-f6a61cfc4e8b';
const report = () => ({ protocol: 'client-diagnostics-v1', runId: 'a'.repeat(32), sequence: 1, version: '0.4.38+52', events: [{ time: new Date().toISOString(), event: 'displayConfigure', desktopId: 'pve-149', operationId, requestId: operationId, mode: 'stream', code: 'display_configuration_failed', values: { ok: false, action: 1, monitors: 2, elapsedMs: 7354 } }] });

test('Negotiated takeover diagnostics keep only five fixed refusal codes and discard consent secrets',()=>{
  for(const code of ['takeover_context_stale','takeover_context_expired','invalid_takeover_context','invalid_context_protocol','session_context_unavailable']){
    const raw=report();raw.events[0]={time:new Date().toISOString(),event:'brokerRequest',desktopId:'pve-149',code,
      context:'PRIVATE_SENTINEL',takeoverOffer:{context:'PRIVATE_SENTINEL'},existingLogin:{username:'PRIVATE_SENTINEL'},
      values:{ok:false,stage:3,context:'PRIVATE_SENTINEL',challengeToken:'PRIVATE_SENTINEL',username:'PRIVATE_SENTINEL',message:'PRIVATE_SENTINEL'}};
    const projected=safeClientDiagnosticReport(raw);assert.equal(projected.events[0].code,code);
    assert.deepEqual(projected.events[0].values,{ok:false,stage:3});assert.equal(JSON.stringify(projected).includes('PRIVATE_SENTINEL'),false);
  }
  const arbitrary=report();arbitrary.events[0].code='takeover_context_PRIVATE_SENTINEL';assert.equal(safeClientDiagnosticReport(arbitrary).events[0].code,'unknown_failure');
});

test('A viewport resize result keeps its stage times (where the resize spent its time)', () => {
  const raw = report();
  const stages = Object.fromEntries(viewportStageNumeric.map((key, index) => [key, 10 + index]));
  raw.events = [{ time: new Date().toISOString(), event: 'viewportResult', desktopId: 'pve-170', mode: 'stream', values: { ok: true, width: 1920, height: 1080, elapsedMs: 780, ...stages, hostEncoderMs: 5, hostFutureMs: 7, probeMsText: 'PRIVATE_SENTINEL' } }];
  const values = safeClientDiagnosticReport(raw).events[0].values;
  assert.deepEqual(values, { ok: true, width: 1920, height: 1080, elapsedMs: 780, ...stages });
  assert.equal(viewportStageNumeric.length, 14);
  for (const bad of [-1, 1.5, 0x100000000, '12']) {
    const invalid = report();invalid.events[0].values = { ackMs: bad };
    assert.deepEqual(safeClientDiagnosticReport(invalid).events[0].values, {}, String(bad));
  }
});

test('Client diagnostic projection excludes all arbitrary text, rejects malformed bounds and keeps exact correlations', () => {
  const raw = report();raw.password = 'PRIVATE_SENTINEL';raw.events[0].message = 'PRIVATE_SENTINEL';raw.events[0].values.token = 'PRIVATE_SENTINEL';raw.events[0].values.width = Infinity;
  const safe = safeClientDiagnosticReport(raw);
  assert.equal(JSON.stringify(safe).includes('PRIVATE_SENTINEL'), false);assert.equal(safe.events[0].operationId, operationId);assert.equal(safe.events[0].values.elapsedMs, 7354);assert.equal(safe.events[0].values.width, undefined);
  raw.events[0].code = 'PRIVATE_SENTINEL';assert.equal(safeClientDiagnosticReport(raw).events[0].code, 'unknown_failure');
  for (const change of [r=>r.version='PRIVATE_SENTINEL',r=>r.runId='invalid',r=>r.sequence=-1,r=>r.sequence=0x100000000,r=>r.events=Array(17).fill(r.events[0]),r=>r.events[0].event='PRIVATE_SENTINEL',r=>r.events[0].operationId='PRIVATE_SENTINEL',r=>r.events[0].desktopId='../149',r=>r.events[0].time='2026-02-30T00:00:00.000Z',r=>r.events[0].values=[]]) { const invalid=report();change(invalid);assert.equal(safeClientDiagnosticReport(invalid),null); }
  const unknown = report();unknown.version=null;assert.equal(safeClientDiagnosticReport(unknown).version,null);
});

test('Every connect refusal the relay answers (409) is kept as the client reported it', async () => {
  // During a Host update, connects refused with host_not_registered /
  // stream_unavailable must not arrive as unknown_failure: the restart window could not be read from them.
  const relay = await readFile(new URL('../src/relay.mjs', import.meta.url), 'utf8');
  const refusals = [...new Set([...relay.matchAll(/HttpError\(409,\s*'([a-z_]+)'/g)].map(match => match[1]))];
  assert.ok(refusals.includes('host_not_registered') && refusals.includes('stream_unavailable'));
  for (const code of refusals) {
    const raw = report();raw.events[0].event = 'brokerRequest';raw.events[0].code = code;
    assert.equal(safeClientDiagnosticReport(raw).events[0].code, code, code);
  }
});

test('fixed negotiated sign-in failures survive projection without admitting peer text or new prefixes', () => {
  for (const code of ['signin_stream_unavailable','invalid_signin_request','signin_authority_lost','invalid_signin_info','invalid_signin_ack','signin_phase_changed']) {
    const raw = report(); raw.events[0].event = 'brokerRequest'; raw.events[0].code = code;
    raw.events[0].message = 'PRIVATE_SENTINEL'; raw.events[0].values.token = 'PRIVATE_SENTINEL';
    const safe = safeClientDiagnosticReport(raw); assert.equal(safe.events[0].code, code);
    assert.equal(JSON.stringify(safe).includes('PRIVATE_SENTINEL'), false);
  }
  for (const code of ['signin_future_untrusted','signin_authority_lost PRIVATE_SENTINEL','invalid_signin_info\u0000PRIVATE_SENTINEL','PRIVATE_SENTINEL']) {
    const raw = report(); raw.events[0].code = code;
    assert.equal(safeClientDiagnosticReport(raw).events[0].code, 'unknown_failure');
  }
});

test('Every session end reason the relay sends is kept as the client reported it', async () => {
  // The client records the relay's own reason; one missing
  // here would read as unknown_failure in the administrator's log.
  // Other broker modules end sessions through the relay (`r.stop`) as well.
  const source = (await Promise.all(['relay.mjs', 'console-preparation.mjs', 'host-update.mjs'].map(name => readFile(new URL('../src/' + name, import.meta.url), 'utf8')))).join('\n');
  const reasons = [...new Set([...source.matchAll(/\b(?:this|r)\.(?:stop|unavailable|dropHost|resizeFailed|cancelDisplay)\([^;]*?'([a-z_]+)'/g)].map(match => match[1]))];
  assert.ok(reasons.length > 30);
  assert.ok(reasons.includes('host_update'));
  for (const reason of reasons) {
    const raw = report();raw.events[0].code = reason;
    assert.equal(safeClientDiagnosticReport(raw).events[0].code, reason, reason);
  }
});

test('Clipboard channel and transfer outcomes are kept with fixed codes and no content', () => {
  const raw = report();
  raw.events = [
    { time: new Date().toISOString(), event: 'clipboard', desktopId: 'pve-149', mode: 'stream', values: { action: 1, ok: true, auto: true, available: true } },
    { time: new Date().toISOString(), event: 'clipboard', desktopId: 'pve-149', mode: 'stream', code: 'clipboard_watch_unavailable', values: { action: 4, ok: false, text: 'PRIVATE_SENTINEL' } },
  ];
  const safe = safeClientDiagnosticReport(raw);
  assert.ok(safe);
  assert.deepEqual(safe.events[0].values, { action: 1, ok: true, auto: true, available: true });
  assert.equal(safe.events[1].code, 'clipboard_watch_unavailable');
  assert.equal(JSON.stringify(safe).includes('PRIVATE_SENTINEL'), false);
});

test('USB recovery outcomes retain their fixed reason and counts without hardware identity', () => {
  for (const code of ['usb_prepare_timeout', 'usb_return_unknown', 'usb_capture_timeout', 'usb_request_stuck', 'usb_device_not_returned']) {
    const raw = report();
    raw.events[0] = { time: new Date().toISOString(), event: 'usb', desktopId: 'pve-149', sessionId: operationId, mode: 'stream', code,
      values: { ok: false, stage: code === 'usb_prepare_timeout' ? 12 : 10, usbBack: 2, usbReq: 0xA1FE, serial: 'PRIVATE_SENTINEL', name: 'PRIVATE_SENTINEL' } };
    const safe = safeClientDiagnosticReport(raw);
    assert.equal(safe.events[0].code, code);
    assert.equal(safe.events[0].sessionId, operationId);
    assert.equal(safe.events[0].values.usbBack, 2);
    assert.equal(safe.events[0].values.usbReq, 0xA1FE);
    assert.equal(JSON.stringify(safe).includes('PRIVATE_SENTINEL'), false);
  }
});

test('Update download failures keep their class, versions, requests and bytes', () => {
  // Without its class a failed download arrives as a bare { ok: false }.
  for (const code of ['download_network', 'download_timeout', 'download_server_error', 'download_not_found', 'download_http_status', 'download_disk_full', 'download_file_error', 'download_size_mismatch', 'download_failed', 'hash_mismatch', 'package_version_mismatch', 'installer_start_failed', 'signature_invalid']) {
    const raw = report();
    raw.events[0] = { time: new Date().toISOString(), event: 'updateDownload', code, values: { ok: false, current: 4162, latest: 4170, attempts: 4, receivedKib: 51200, elapsedMs: 61000, path: 'PRIVATE_SENTINEL' } };
    const safe = safeClientDiagnosticReport(raw);
    assert.equal(safe.events[0].code, code);
    assert.deepEqual(safe.events[0].values, { ok: false, current: 4162, latest: 4170, attempts: 4, receivedKib: 51200, elapsedMs: 61000 });
  }
});

function accounts(store) {
  store.addUser({name:'owner',passwordHash:'fixture',role:'admin'});store.addUser({name:'alice',passwordHash:'fixture'});store.addUser({name:'bob',passwordHash:'fixture'});
  store.syncProxmox([{vmid:149,node:'fixture',name:'TESTER',status:'running'},{vmid:170,node:'fixture',name:'OTHER',status:'running'}],'fixture');store.assign('alice','pve-149');store.assign('bob','pve-170');
}
test('Diagnostic storage is idempotent, assignment gated, bounded and expires without granting host readiness', () => {
  const store=new Store(':memory:');accounts(store);const alice=store.findUser('alice'), bob=store.findUser('bob'), now=Date.now();
  try {
    const raw=report();receiveClientDiagnostics(store,alice,raw,now);receiveClientDiagnostics(store,alice,raw,now+1);
    assert.equal(store.db.prepare('SELECT count(*) n FROM client_diagnostics').get().n,1);
    assert.throws(()=>receiveClientDiagnostics(store,bob,raw),e=>e.status===404);
    assert.equal(clientDiagnostics(store,'pve-149').events[0].account,'alice');assert.equal(clientDiagnostics(store,'pve-170').events.length,0);
    const generic=report();generic.sequence=2;delete generic.events[0].desktopId;receiveClientDiagnostics(store,alice,generic,now);
    assert.equal(clientDiagnostics(store).events.length,2);assert.equal(clientDiagnostics(store,'pve-149').events.length,1);
    for(let i=3;i<=PER+22;i++){const r=report();r.sequence=i;receiveClientDiagnostics(store,alice,r,now);}
    assert.equal(store.db.prepare('SELECT count(*) n FROM client_diagnostics').get().n,PER);
    for(let u=0;u<5;u++) {store.addUser({name:'bounded'+u,passwordHash:'fixture'});const user=store.findUser('bounded'+u);for(let i=1;i<=PER;i++){const r=report();r.sequence=i;delete r.events[0].desktopId;receiveClientDiagnostics(store,user,r,now);}}
    assert.equal(store.db.prepare('SELECT count(*) n FROM client_diagnostics').get().n,TOTAL);
    assert.equal(clientDiagnostics(store,null,now+clientDiagnosticRetentionMs+1).events.length,0);
    const fresh=report();fresh.sequence=PER+23;receiveClientDiagnostics(store,alice,fresh,now+clientDiagnosticRetentionMs+1);
    assert.equal(store.db.prepare('SELECT count(*) n FROM client_diagnostics').get().n,1);
    assert.equal(store.db.prepare('SELECT count(*) n FROM hosts').get().n,0);
    store.revoke('alice','pve-149');fresh.sequence=PER+24;assert.throws(()=>receiveClientDiagnostics(store,alice,fresh),e=>e.status===404);
  } finally {store.close();}
});

async function fixture(t) {
  const store=new Store(':memory:'), vault=new Vault(randomBytes(32));accounts(store);
  store.saveSetting('proxmox',vault.seal({endpoint:'https://pve.invalid',tokenId:'fixture@pve!fixture',secret:'fixture'}),'fixture');store.saveSetting('proxmox_initialized','1','fixture');
  const client=newToken(), admin=newToken(), bob=newToken();store.createSession(store.findUser('alice'),tokenHash(client),Date.now(),60000);store.createSession(store.findUser('bob'),tokenHash(bob),Date.now(),60000);store.createSession(store.findUser('owner'),tokenHash(admin),Date.now(),60000,'admin');
  const server=await buildServer({store,vault,publicOrigin:'https://diagnostics.invalid',proxmox:{inventory:async()=>[]},logger:{error:()=>assert.fail('Unexpected server error')}});await new Promise(r=>server.listen(0,'127.0.0.1',r));
  t.after(async()=>{await new Promise(r=>server.close(r));store.close();});
  const call=async(path,{method='GET',who='client',data,origin}={})=>{
    const response=await fetch(`http://127.0.0.1:${server.address().port}${path}`,{method,headers:{...(who==='admin'?{Cookie:'__Secure-multitap-admin='+admin}:who==='client'||who==='bob'?{Authorization:'Bearer '+(who==='bob'?bob:client)}:{}),...(origin?{Origin:origin}:{}),...(data?{'Content-Type':'application/json'}:{})},body:data?JSON.stringify(data):undefined});return{status:response.status,body:await response.json()};
  };
  return{store,call,client};
}

function inventoryReport(sequence,{failed=false,mixed=false}={}) {
  const value=report();value.sequence=sequence;
  value.events=[{time:new Date().toISOString(),event:'inventory',values:{ok:!failed}}];
  if(failed)value.events[0].code='network_error';
  if(mixed)value.events.push({...report().events[0],code:'capture_failed'});
  return value;
}

test('Routine inventory reports cannot evict VM failures from per-account diagnostic retention',()=>{
  const store=new Store(':memory:');accounts(store);const alice=store.findUser('alice'),now=Date.now();
  try {
    receiveClientDiagnostics(store,alice,report(),now);
    receiveClientDiagnostics(store,alice,inventoryReport(2,{failed:true}),now+1);
    receiveClientDiagnostics(store,alice,inventoryReport(3,{mixed:true}),now+2);
    for(let sequence=4;sequence<=PER+52;sequence++)receiveClientDiagnostics(store,alice,inventoryReport(sequence),now+sequence);
    assert.equal(store.db.prepare('SELECT count(*) n FROM client_diagnostics').get().n,PER);
    const rows=clientDiagnostics(store).events;
    assert.equal(rows.length,128);
    assert.ok(rows.every((row,index)=>index===0||Date.parse(rows[index-1].receivedAt)>=Date.parse(row.receivedAt)));
    assert.ok(rows.some(e=>e.operationId===operationId&&e.code==='display_configuration_failed'));
    assert.ok(rows.some(e=>e.code==='capture_failed'));
    assert.ok(rows.some(e=>e.event==='inventory'&&e.code==='network_error'));
    assert.ok(store.db.prepare('SELECT 1 FROM client_diagnostics WHERE sequence=?').get(PER+52));
    assert.equal(clientDiagnostics(store,'pve-170').events.length,0);
  } finally {store.close();}
});

test('Global inventory churn preserves activity within the same strict diagnostic row and expiry bounds',()=>{
  const store=new Store(':memory:');accounts(store);const now=Date.now();
  try {
    receiveClientDiagnostics(store,store.findUser('alice'),report(),now);
    for(let i=0;i<5;i++){
      const name='polling'+i;store.addUser({name,passwordHash:'fixture'});const user=store.findUser(name);
      for(let sequence=1;sequence<=PER;sequence++)receiveClientDiagnostics(store,user,inventoryReport(sequence),now+1);
    }
    assert.equal(store.db.prepare('SELECT count(*) n FROM client_diagnostics').get().n,TOTAL);
    assert.ok(clientDiagnostics(store,'pve-149').events.some(e=>e.operationId===operationId));
    assert.equal(clientDiagnostics(store,null,now+clientDiagnosticRetentionMs+2).events.length,0);
    const fresh=report();fresh.sequence=2;receiveClientDiagnostics(store,store.findUser('alice'),fresh,now+clientDiagnosticRetentionMs+2);
    assert.equal(store.db.prepare('SELECT count(*) n FROM client_diagnostics').get().n,1);
  } finally {store.close();}
});
function statsReport(sequence) {
  const value=report();value.sequence=sequence;
  value.events=[{time:new Date().toISOString(),event:'streamStats',desktopId:'pve-149',mode:'stream',sessionId:operationId,values:{samples:30,windowMs:30012,width:1264,height:760,decodeFps10:598,encodeFps10:600,decodeFpsLow10:291,captureFpsLow10:300,encodeFpsLow10:299,decodeTier:2,decodeVendor:32902,receiveKbps:18450,bitrateKbps:42000,auto:true,rttMs:21,hostWorkUs:9200,encodeUs:4400,decodeUs:2100,inputHostP50Us:6100,inputFrameP95Us:48200,
    captureMaxUs:41000,textureWaitUs:13000,textureUploadUs:190,queuedFrames:310,skippedFrames:0,texturePath:0,displayHz:32,gpuHardware:false,remoteSession:true,mediaWaits:7,audioDropMs:60,
    hostInputCaptureP50Us:31500,hostInputCaptureP95Us:70250,hostCaptureEncodedP50Us:2100,hostCaptureEncodedP95Us:6400,inputPushP50Us:800,inputDecodeP50Us:1600,
    hostSendQueueP50Us:1200,hostSendQueueP95Us:4100,hostSendWriteP50Us:300,hostSendWriteP95Us:2600,relayForwardP50Us:400,relayForwardP95Us:1900,inputTransferP50Us:9800,inputTransferP95Us:31000,inputFrameBytes:48211,inputDownlinkP50Us:11400,inputDownlinkP95Us:52000,
    secretText:'PRIVATE_SENTINEL',fps:59.8,adapterName:'PRIVATE_ADAPTER',gpuHardwareText:'yes',texturePathName:'pixel'}}];
  return value;
}

test('a full account still keeps its newest statistics windows, without giving up older evidence to them',()=>{
  const store=new Store(':memory:');accounts(store);const alice=store.findUser('alice'),now=Date.now();
  try {
    for(let sequence=1;sequence<=PER;sequence++){const r=report();r.sequence=sequence;receiveClientDiagnostics(store,alice,r,now);}
    receiveClientDiagnostics(store,alice,statsReport(PER+1),now);
    const kinds=()=>store.db.prepare('SELECT report FROM client_diagnostics').all().map(r=>JSON.parse(r.report).events[0].event);
    assert.equal(kinds().filter(k=>k==='streamStats').length,1,'the window that just arrived is there');
    for(let sequence=PER+2;sequence<=PER+1+KEPT*2;sequence++)receiveClientDiagnostics(store,alice,statsReport(sequence),now);
    assert.equal(kinds().length,PER);
    assert.equal(kinds().filter(k=>k==='streamStats').length,KEPT,'statistics take only the newest rows, the rest stays evidence');
  } finally {store.close();}
});

test('audio-stats-v1 fields are kept as fixed non-negative integers; anything else is dropped',()=>{
  const audio={audioDepthMs:44,audioTargetMs:40,audioLatencyMs:56,audioUnderruns:2,audioOverruns:1,audioInserted:480,audioTrimmed:960,audioAvLagMs:31,audioAvLeadMs:0,
    hostAudioCaptureUs:11800,hostAudioCaptureMaxUs:23400,hostAudioSent:2998,hostAudioDropped:2,hostAudioQueueUs:700,hostAudioQueueMaxUs:9100};
  const value=statsReport(1);Object.assign(value.events[0].values,audio);
  const safe=safeClientDiagnosticReport(value);
  for(const [key,expected] of Object.entries(audio))assert.equal(safe.events[0].values[key],expected,key);
  const bad=statsReport(2);Object.assign(bad.events[0].values,{audioDepthMs:-1,audioAvLagMs:3.5,audioUnderruns:'2',hostAudioSent:0x100000000,audioPcm:'PRIVATE_SENTINEL'});
  const dropped=safeClientDiagnosticReport(bad).events[0].values;
  for(const key of ['audioDepthMs','audioAvLagMs','audioUnderruns','hostAudioSent','audioPcm'])assert.equal(dropped[key],undefined,key);
});

test('stream-stats-v1 rows keep only integer/boolean fields, are acknowledged, and stay routine in retention',()=>{
  const store=new Store(':memory:');accounts(store);const alice=store.findUser('alice'),now=Date.now();
  try {
    const ack=receiveClientDiagnostics(store,alice,statsReport(1),now);
    assert.equal(ack.streamStatsProtocol,'stream-stats-v1');
    const row=clientDiagnostics(store,'pve-149').events[0];
    assert.equal(row.event,'streamStats');assert.equal(row.values.decodeFps10,598);assert.equal(row.values.auto,true);assert.equal(row.values.bitrateKbps,42000);assert.equal(row.values.inputHostP50Us,6100);assert.equal(row.values.inputFrameP95Us,48200);assert.equal(row.values.captureMaxUs,41000);
    assert.equal(row.values.fps,undefined);assert.equal(JSON.stringify(row).includes('PRIVATE_SENTINEL'),false);
    // tracks-v1: the slowest output's rates.
    assert.equal(row.values.decodeFpsLow10,291);assert.equal(row.values.captureFpsLow10,300);assert.equal(row.values.encodeFpsLow10,299);
    assert.equal(row.values.decodeTier,2);assert.equal(row.values.decodeVendor,32902);
    // texture-pacing-v1 and render facts: integers and booleans only, never adapter strings.
    assert.equal(row.values.textureWaitUs,13000);assert.equal(row.values.textureUploadUs,190);assert.equal(row.values.queuedFrames,310);assert.equal(row.values.skippedFrames,0);
    assert.equal(row.values.texturePath,0);assert.equal(row.values.displayHz,32);assert.equal(row.values.gpuHardware,false);assert.equal(row.values.remoteSession,true);
    assert.equal(JSON.stringify(row).includes('PRIVATE_ADAPTER'),false);assert.equal(row.values.gpuHardwareText,undefined);assert.equal(row.values.texturePathName,undefined);
    // Backpressure that waits or drops audio instead of ending the stream.
    assert.equal(row.values.mediaWaits,7);assert.equal(row.values.audioDropMs,60);
    // Where input -> frame goes: host and client shares.
    assert.equal(row.values.hostInputCaptureP50Us,31500);assert.equal(row.values.hostInputCaptureP95Us,70250);assert.equal(row.values.hostCaptureEncodedP50Us,2100);
    assert.equal(row.values.hostCaptureEncodedP95Us,6400);assert.equal(row.values.inputPushP50Us,800);assert.equal(row.values.inputDecodeP50Us,1600);
    // The tagged packet after the encoder: host send path, relay share, transfer and size.
    for(const [key,value] of Object.entries({hostSendQueueP50Us:1200,hostSendQueueP95Us:4100,hostSendWriteP50Us:300,hostSendWriteP95Us:2600,relayForwardP50Us:400,relayForwardP95Us:1900,inputTransferP50Us:9800,inputTransferP95Us:31000,inputFrameBytes:48211,inputDownlinkP50Us:11400,inputDownlinkP95Us:52000}))assert.equal(row.values[key],value,key);
    // Wrong types are dropped, not coerced.
    const typed=statsReport(2);Object.assign(typed.events[0].values,{gpuHardware:1,remoteSession:'true',texturePath:true,displayHz:59.94,textureWaitUs:-5});
    receiveClientDiagnostics(store,alice,typed,now);
    const coerced=clientDiagnostics(store,'pve-149').events.find(e=>e.sequence===2);
    for(const key of ['gpuHardware','remoteSession','texturePath','displayHz','textureWaitUs'])assert.equal(coerced.values[key],undefined,key);
    // A long session's windows are evicted before a failure, like inventory polling.
    const failure=report();failure.sequence=1000;receiveClientDiagnostics(store,alice,failure,now);
    for(let sequence=2;sequence<=PER+72;sequence++)receiveClientDiagnostics(store,alice,statsReport(sequence),now+sequence);
    assert.equal(store.db.prepare('SELECT count(*) n FROM client_diagnostics').get().n,PER);
    const rows=clientDiagnostics(store,'pve-149').events;
    assert.ok(rows.some(e=>e.code==='display_configuration_failed'));
    assert.ok(rows.some(e=>e.event==='streamStats'&&e.sequence===PER+72));
  } finally {store.close();}
});

test('Production HTTP accepts authenticated uploads, restricts administrator reads and rejects logout/revocation/oversize',async t=>{
  const f=await fixture(t),path='/api/v1/client/diagnostics';
  for(const who of [null,'admin'])assert.equal((await f.call(path,{method:'POST',who,data:report()})).status,401);
  assert.equal((await f.call(path,{method:'POST',who:'bob',data:report()})).status,404);
  assert.equal((await f.call(path,{method:'POST',origin:'https://evil.invalid',data:report()})).status,403);
  const accepted=await f.call(path,{method:'POST',data:report()});assert.equal(accepted.status,202);assert.equal(accepted.body.sequence,1);
  for(const read of ['/api/v1/admin/client-diagnostics','/api/v1/admin/desktops/pve-149/client-diagnostics']) {
    assert.equal((await f.call(read)).status,401);const value=await f.call(read,{who:'admin'});assert.equal(value.status,200);assert.equal(value.body.events[0].operationId,operationId);assert.equal(value.body.events[0].version,'0.4.38+52');
  }
  assert.equal((await f.call(path)).status,405);
  assert.equal((await f.call(path,{method:'POST',data:{...report(),secret:'x'.repeat(16385)}})).status,413);
  f.store.logout(tokenHash(f.client));assert.equal((await f.call(path,{method:'POST',data:report()})).status,401);
  assert.equal(f.store.db.prepare('SELECT count(*) n FROM client_diagnostics').get().n,1);
});
test('Production HTTP bounds uploads per authenticated account',async t=>{
  const f=await fixture(t);for(let i=1;i<=60;i++){const r=report();r.sequence=i;assert.equal((await f.call('/api/v1/client/diagnostics',{method:'POST',data:r})).status,202);}
  assert.equal((await f.call('/api/v1/client/diagnostics',{method:'POST',data:report()})).status,429);
});
test('Actual Dart reporter bytes cross the production HTTP route and remain readable by admin', {skip:!process.env.MULTITAP_CLIENT_DIAGNOSTIC_REPORT},async t=>{
  const f=await fixture(t),raw=JSON.parse(await readFile(process.env.MULTITAP_CLIENT_DIAGNOSTIC_REPORT,'utf8'));
  assert.equal((await f.call('/api/v1/client/diagnostics',{method:'POST',data:raw})).status,202);
  const value=await f.call('/api/v1/admin/desktops/pve-149/client-diagnostics',{who:'admin'});assert.equal(value.status,200);
  assert.ok(value.body.events.some(e=>e.code==='capture_failed'));assert.equal(JSON.stringify(value.body).includes('PRIVATE_SENTINEL'),false);
});

test('udp-media-v1 probe fields are kept inside their bounds; a value past a bound is dropped, the row survives',()=>{
  const row=values=>safeClientDiagnosticReport({protocol:'client-diagnostics-v1',runId:'a'.repeat(32),sequence:1,version:'0.4.140',events:[{time:new Date().toISOString(),event:'streamStats',mode:'stream',values:{samples:1,...values}}]}).events[0].values;
  const inside={udpProbe:5,udpRttP50Ms:18,udpRttP95Ms:60000,udpLossPct:100,udpRelayIndex:3,udpKeepSent:0xffffffff,udpKeepLost:0};
  assert.deepEqual(row(inside),{samples:1,...inside});
  for(const value of [{udpProbe:6},{udpRttP50Ms:60001},{udpRttP95Ms:60001},{udpLossPct:101},{udpRelayIndex:4},{udpKeepSent:0x100000000},{udpKeepLost:-1},{udpProbe:1.5},{udpProbe:'1'},{udpLossPct:true}])assert.deepEqual(row(value),{samples:1},JSON.stringify(value));
  // Android stall watch: kept within bounds, a cause past 6 is dropped.
  const stall={udpKeyRequests:7,udpStalls:2,udpStallCause:5};
  assert.deepEqual(row(stall),{samples:1,...stall});
  for(const value of [{udpStallCause:7},{udpStalls:-1},{udpKeyRequests:0x100000000}])assert.deepEqual(row(value),{samples:1},JSON.stringify(value));
});

test('client-load fields (tenths of a percent) are kept; unknown integer fields are dropped without rejecting the report',()=>{
  const store=new Store(':memory:');accounts(store);const alice=store.findUser('alice'),now=Date.now();
  try {
    const sent=statsReport(1);
    Object.assign(sent.events[0].values,{appCpuPct10:123,systemCpuPct10:341,gpuDecodePct10:80,gpu3dPct10:0,videoGapMaxMs:208,appCpuPctFuture:7,appCpuPct10Text:'12'});
    const ack=receiveClientDiagnostics(store,alice,sent,now);
    assert.equal(ack.accepted,true);
    const row=clientDiagnostics(store,'pve-149').events[0];
    assert.equal(row.values.appCpuPct10,123);assert.equal(row.values.systemCpuPct10,341);assert.equal(row.values.gpuDecodePct10,80);assert.equal(row.values.gpu3dPct10,0);assert.equal(row.values.videoGapMaxMs,208);
    assert.equal(row.values.appCpuPctFuture,undefined);assert.equal(row.values.appCpuPct10Text,undefined);
    assert.equal(row.values.decodeFps10,598,'the rest of the row survives');
  } finally {store.close();}
});

test('stream-stall-v1 rows survive upload with their fixed fields, are acknowledged, and stay routine in retention',()=>{
  const store=new Store(':memory:');accounts(store);const alice=store.findUser('alice'),now=Date.now();
  const stall={stallKind:1,stallMs:1029,stallKey:1,stallQueue:8,stallQueueMax:8,stallOffers:3,stallFull:5,stallDecodes:2,stallDecodeMaxUs:18000,stallPending:1,stallSkipped:0,stallWaitMs:0,
    stallPtsGapMs:16,hostChangeLateMs:21,stallArrivalMs:612,stallUdpLost:0,stallUdpKeys:2,stallUdpWaits:1,stallUdpSkips:1,stallUdpNacks:4,stallUdpFallbacks:0,udpActivePath:1,
    hostWindows:2,hostEncodeGapMs:40,hostCaptureGapMs:35,hostWaitMs:12,hostCaptureMs:3,hostConvertMs:2,hostEncodeMs:9,hostKeyFrames:2,hostKeyRequests:2,hostBitrateSteps:1,hostTargetMinKbps:18000,
    hostUdpBacklogKeys:0,hostUdpClientKeys:2,hostUdpRefused:0,rttMs:9,decoderBusy:274,stallDropped:0};
  const sent=sequence=>({protocol:'client-diagnostics-v1',runId:'d'.repeat(32),sequence,version:'0.4.162',events:[{time:new Date(now).toISOString(),event:'streamStall',desktopId:'pve-149',mode:'stream',sessionId:operationId,values:{...stall,stallText:'PRIVATE_SENTINEL',stallFuture:3,stallKind2:1}}]});
  try {
    const ack=receiveClientDiagnostics(store,alice,sent(1),now);
    assert.equal(ack.streamStallProtocol,'stream-stall-v1');assert.equal(ack.streamStatsProtocol,'stream-stats-v1');
    const row=clientDiagnostics(store,'pve-149').events[0];
    assert.equal(row.event,'streamStall');assert.deepEqual(row.values,stall);assert.equal(JSON.stringify(row).includes('PRIVATE_SENTINEL'),false);
    // Past a bound: the field is dropped, the row survives.
    const bounded=sent(2);Object.assign(bounded.events[0].values,{stallKind:3,stallKey:2});
    const kept=safeClientDiagnosticReport(bounded).events[0].values;assert.equal(kept.stallKind,undefined);assert.equal(kept.stallKey,undefined);assert.equal(kept.stallMs,1029);
    // A session's closing row: only the count of stalls no row carried.
    const closing=sent(4);closing.events[0].values={stallDropped:12};assert.deepEqual(safeClientDiagnosticReport(closing).events[0].values,{stallDropped:12});
    // New statistics-window fields.
    const window=statsReport(3);Object.assign(window.events[0].values,{stalls:4,stallMaxMs:1029,fpsTarget:45,udpQueueWaits:12,udpQueueSkips:1,udpNacks:30});
    receiveClientDiagnostics(store,alice,window,now);
    const stats=clientDiagnostics(store,'pve-149').events.find(e=>e.sequence===3);
    for(const [key,value] of Object.entries({stalls:4,stallMaxMs:1029,fpsTarget:45,udpQueueWaits:12,udpQueueSkips:1,udpNacks:30}))assert.equal(stats.values[key],value,key);
    // Many stall rows do not evict a failure.
    const failure=report();failure.sequence=1000;receiveClientDiagnostics(store,alice,failure,now);
    for(let sequence=4;sequence<=PER+72;sequence++)receiveClientDiagnostics(store,alice,sent(sequence),now+sequence);
    assert.ok(clientDiagnostics(store,'pve-149').events.some(e=>e.code==='display_configuration_failed'));
  } finally {store.close();}
});
test('connect-timing-v1: a first streamReady keeps its connection stages; anything else is dropped without rejecting the row',()=>{
  const store=new Store(':memory:');accounts(store);const alice=store.findUser('alice'),now=Date.now();
  try {
    const sent=report();
    const stages={windowMs:312,loadMs:48,retireMs:0,refreshMs:61,presentMs:35,outputsMs:2,viewportMs:17,connectMs:84,dialMs:96,infoMs:640,decoderMs:71,keyFrameMs:38,firstFrameMs:22,hostAdmitMs:3,hostWorkerMs:180,hostDisplayMs:240,hostMediaMs:120};
    sent.events=[{time:new Date().toISOString(),event:'streamReady',desktopId:'pve-149',mode:'stream',sessionId:operationId,values:{ok:true,width:1264,height:760,elapsedMs:867,...stages,startStagesText:'PRIVATE_SENTINEL',hostStartMs:7}}];
    assert.equal(receiveClientDiagnostics(store,alice,sent,now).accepted,true);
    const row=clientDiagnostics(store,'pve-149').events[0];
    assert.equal(row.event,'streamReady');
    assert.deepEqual(row.values,{ok:true,width:1264,height:760,elapsedMs:867,...stages});
    assert.deepEqual([...connectTimingNumeric].sort(),Object.keys(stages).sort(),'the list names exactly the client stages');
  } finally {store.close();}
});

test('audio format, Opus concealment and the player error are kept on streamStats; streamReady keeps the format and decoder prestart',()=>{
  // Without these the rows cannot say which audio format a session had (a slow Opus connect, for one).
  const window=statsReport(1);Object.assign(window.events[0].values,{audioFormat:2,audioConcealed:37,audioError:0x88890004});
  const values=safeClientDiagnosticReport(window).events[0].values;
  assert.equal(values.audioFormat,2);assert.equal(values.audioConcealed,37);assert.equal(values.audioError,0x88890004);
  const bad=statsReport(2);Object.assign(bad.events[0].values,{audioFormat:'opus',audioConcealed:-1,audioError:-1});
  const dropped=safeClientDiagnosticReport(bad).events[0].values;
  for(const key of ['audioFormat','audioConcealed','audioError'])assert.equal(dropped[key],undefined,key);
  const ready=report();
  ready.events=[{time:new Date().toISOString(),event:'streamReady',desktopId:'pve-149',mode:'stream',values:{ok:true,width:1264,height:760,audioFormat:1,decoderPrestarted:true}}];
  assert.deepEqual(safeClientDiagnosticReport(ready).events[0].values,{ok:true,width:1264,height:760,audioFormat:1,decoderPrestarted:true});
  ready.events[0].values.decoderPrestarted=1;
  assert.equal(safeClientDiagnosticReport(ready).events[0].values.decoderPrestarted,undefined,'a boolean only');
});

test('Android decoder low-latency configuration fields are kept',()=>{
  const sent=statsReport(1);Object.assign(sent.events[0].values,{decoderLowLatency:31,decoderVendorKeys:5,decoderLowLatencyName:'x'});
  const values=safeClientDiagnosticReport(sent).events[0].values;
  assert.equal(values.decoderLowLatency,31);assert.equal(values.decoderVendorKeys,5);
  assert.equal(values.decoderLowLatencyName,undefined);assert.equal(values.decodeFps10,598,'the rest of the row survives');
});
