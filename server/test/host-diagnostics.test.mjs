import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { safeDiagnosticReport } from '../src/host-diagnostics.mjs';
import { Relay } from '../src/relay.mjs';
import { readFileSync } from 'node:fs';

const report = () => ({ protocol: 'host-diagnostics-v1', source: 'host-service', version: '0.2.24', collectedAt: new Date().toISOString(), interactiveSession: true, consoleSession: true, captureAvailable: false,
  display: { observed: true, observationAgeMs: 0, installed: true, deviceReady: true, owned: true, activeDisplay: false, configurationPending: true, recoveryRequired: true, rebootRequired: false, remoteSession: false, monitorCount: 0, width: 0, height: 0, reason: 'native_topology_recovery_required' },
  native: { available: true, installed: true, started: true, leaseActive: false, activationVerified: false, configurationPending: false, recoveryRequired: true, countControlAvailable: false, phase: 'recovery', failureReason: 'controller_start_failed' },
  events: [{ time: new Date().toISOString(), event: 'displayObserved', values: { installed: true, active: false, monitors: 0 } }] });

test('hostAction 270, the udp-media-v1 bind result, keeps its stage and relay index', () => {
  const raw = report();raw.events = [{ time: new Date().toISOString(), event: 'hostAction', values: { action: 270, stage: 4, error: 1, ticket: 'DO_NOT_SEND' } }];
  assert.deepEqual(safeDiagnosticReport(raw).events[0].values, { action: 270, stage: 4, error: 1 });
});

test('Automatic diagnostics project only bounded non-secret status and numeric/boolean records', () => {
  const raw = report();raw.password = 'DO_NOT_SEND';raw.native.owner = 'DO_NOT_SEND';raw.native.failureReason = 'DO_NOT_SEND';raw.display.reason = 'DO_NOT_SEND';raw.events[0].values.token = 'DO_NOT_SEND';raw.events[0].values.error = 'DO_NOT_SEND';raw.events[0].values.registered = 'DO_NOT_SEND';
  const safe = safeDiagnosticReport(raw);
  assert.ok(safe);assert.equal(JSON.stringify(safe).includes('DO_NOT_SEND'), false);
  assert.equal(safe.display.reason, 'viewport_probe_failed');assert.equal(safe.native.failureReason, 'internal_failure');assert.deepEqual(safe.events[0].values, { installed: true, active: false, monitors: 0 });
  for (const mutate of [r => r.display.monitorCount = 3, r => r.display.rebootRequired = 'true', r => r.native.leaseActive = 1, r => r.version = 'SECRET', r => r.collectedAt = '2026-02-30T00:00:00.000Z', r => r.events = Array(9).fill(raw.events[0])]) {
    const invalid = report();mutate(invalid);assert.equal(safeDiagnosticReport(invalid), null);
  }
  const absent = report();absent.native = { available: false, secret: 'DO_NOT_SEND' };
  assert.deepEqual(safeDiagnosticReport(absent).native, { available: false });
  const unknown=report();unknown.display.observed=false;unknown.display.observationAgeMs=null;
  assert.equal(safeDiagnosticReport(unknown).display.observationAgeMs,null);
  unknown.display.observationAgeMs=0;assert.equal(safeDiagnosticReport(unknown),null);
});

test('Activation diagnostics preserve exact Windows failure and distinguish retired attempts without granting readiness', () => {
  const raw=report();raw.native.activation={reason:'native_topology_validate_failed',systemError:1610,attempted:false,windowsVerified:false,password:'DO_NOT_SEND'};raw.native.activationCurrent=false;
  const safe=safeDiagnosticReport(raw);
  assert.deepEqual(safe.native.activation,{reason:'native_topology_validate_failed',systemError:1610,attempted:false,windowsVerified:false});
  assert.equal(safe.native.activationCurrent,false);assert.equal(safe.native.activationVerified,false);assert.equal(safe.display.activeDisplay,false);
  for(const mutate of [a=>a.reason='DO_NOT_SEND',a=>a.systemError=-1,a=>a.systemError=0x100000000,a=>a.systemError=1.5,a=>a.systemError='5',a=>a.attempted=1,a=>a.windowsVerified='true',a=>a.windowsVerified=true]){
    const invalid=structuredClone(raw);mutate(invalid.native.activation);const filtered=safeDiagnosticReport(invalid);
    assert.ok(filtered);assert.equal(filtered.native.activation,undefined);assert.equal(JSON.stringify(filtered).includes('DO_NOT_SEND'),false);
  }
  const verified=report();verified.native.activation={reason:'',systemError:0,attempted:false,windowsVerified:true};verified.native.activationCurrent=true;
  assert.equal(safeDiagnosticReport(verified).native.activation.windowsVerified,true);
  delete verified.native.activationCurrent;assert.equal(safeDiagnosticReport(verified).native.activation,undefined);
});

test('optional console recovery diagnostics keep bounded reasons and session counts without exporting identities or authorizing transfer',()=>{
  const raw=report();raw.consoleRecovery={reason:'windows_identity_mismatch',ready:false,eligible:false,complete:true,configuredIdentity:true,consoleIdentityKnown:true,consoleOccupied:false,activeRdpSessions:0,disconnectedRdpSessions:0,disconnectedConsoleSessions:1,unknownSessions:0,lockedSessions:1,sid:'DO_NOT_SEND',username:'DO_NOT_SEND',sessionId:'DO_NOT_SEND'};
  const safe=safeDiagnosticReport(raw);assert.equal(safe.consoleRecovery.disconnectedConsoleSessions,1);assert.equal(safe.consoleRecovery.reason,'windows_identity_mismatch');assert.equal(safe.display.activeDisplay,false);assert.equal(JSON.stringify(safe).includes('DO_NOT_SEND'),false);
  for(const [field,value] of [['activeRdpSessions',65],['lockedSessions',-1],['unknownSessions',1.5],['eligible','true']]){const invalid=structuredClone(raw);invalid.consoleRecovery[field]=value;assert.equal(safeDiagnosticReport(invalid).consoleRecovery,undefined);}
  raw.consoleRecovery.reason='DO_NOT_SEND';assert.equal(safeDiagnosticReport(raw).consoleRecovery.reason,'windows_session_unknown');delete raw.consoleRecovery;assert.equal(safeDiagnosticReport(raw).consoleRecovery,undefined);
});

const recovery=()=>({path:'retirement',blocker:'started',quiescent:'none',ledger:'settled_shape',retirement:'console_not_primary',worker:'retire_rejected',hold:'profile_not_restored',
  records:{profile:'present',launch:'present',runtime:'present',identity:'present',baseline:'absent',viewport:'absent',reclaim:'absent',retirement:'absent',expiredConsole:'absent'},
  profilePhase:'recovery',profileResize:true,profileStopRequested:true,profileLaunchBound:true,runtimePhase:'recovery',runtimeCheckpoint:false,runtimeMatchesProfile:true,launchMatchesProfile:true,
  launchService:'gone',launchWorker:'gone',logon:'new',identityMatchesLaunch:true,retirementPhase:'none',expiredConsolePhase:'none',attempts:3,budgetExhausted:true,lastAttemptAgeMs:4200});
test('profile recovery explanation keeps only fixed codes and never displaces the base report',()=>{
  const raw=report();raw.native.failureReason='native_stream_profile_recovery_required';raw.native.recovery={...recovery(),operation:'DO_NOT_SEND',sid:'DO_NOT_SEND'};
  const safe=safeDiagnosticReport(raw);assert.deepEqual(safe.native.recovery,recovery());assert.equal(JSON.stringify(safe).includes('DO_NOT_SEND'),false);assert.equal(safe.display.activeDisplay,false);
  for(const mutate of [r=>r.blocker='C:\\ProgramData\\DO_NOT_SEND',r=>r.ledger='DO_NOT_SEND',r=>r.records.profile='DO_NOT_SEND',r=>delete r.records.launch,r=>r.records=null,r=>r.attempts=256,r=>r.attempts=1.5,r=>r.lastAttemptAgeMs=-1,r=>r.profileResize='true',r=>r.budgetExhausted=null,r=>r.logon='DO_NOT_SEND',r=>delete r.hold]){
    const invalid=structuredClone(raw);mutate(invalid.native.recovery);const result=safeDiagnosticReport(invalid);
    assert.ok(result);assert.equal(result.native.recovery,undefined);assert.equal(result.native.failureReason,'native_stream_profile_recovery_required');assert.equal(JSON.stringify(result).includes('DO_NOT_SEND'),false);
  }
  const nullable=structuredClone(raw);Object.assign(nullable.native.recovery,{runtimeMatchesProfile:null,launchMatchesProfile:null,identityMatchesLaunch:null,lastAttemptAgeMs:null});assert.equal(safeDiagnosticReport(nullable).native.recovery.lastAttemptAgeMs,null);
  const absent=report();absent.native={available:false,recovery:recovery()};assert.deepEqual(safeDiagnosticReport(absent).native,{available:false});
  // Bounded maximum: eight maximal events plus geometry and recovery stay within the slot.
  const bounded=structuredClone(raw);const values=Object.fromEntries(['action','stage','monitors','width','height','error','expected','missing','missingOwned','fallback','stale'].map(k=>[k,0xffffffff]));
  for(const key of ['ok','installed','started','active','owned','pending','recovery','reboot','remote','running','registered','online','changed','restartAttempted','consoleExpected','consoleActive','remapped'])values[key]=false;
  bounded.events=Array.from({length:8},()=>({time:bounded.collectedAt,event:'displayObserved',values}));bounded.native.activation={reason:'native_topology_scanline_mismatch',systemError:0xffffffff,attempted:false,windowsVerified:false};bounded.native.activationCurrent=false;
  const projected=safeDiagnosticReport(bounded);assert.ok(projected);assert.ok(Buffer.byteLength(JSON.stringify(projected))<=6144);
  // Display v2 restore detail survives the projection.
  assert.equal(projected.events[0].values.missingOwned,0xffffffff);assert.equal(projected.events[0].values.consoleActive,false);
});

// The exact bytes Host device_status.hpp deviceStatusJson() writes (schema 1).
const devices = () => ({ schema: 1, drives: { winfsp: false, package: true, webClient: 'usable' }, usb: { supported: true, package: true, controller: 'none' }, printers: { reason: '', spooler: 'stopped' }, audio: { output: 'absent' } });
// The Host's inventory group (display adapters and the USB controller).
const inventory = () => ({ displayAdapters: { read: true, current: 1, legacy: 2, duplicate: 0, unproven: 1, foreign: 1, currentStarted: true, blocker: '', cleanup: 'pending_restart' }, usbController: 'foreign' });
test('device status keeps only fixed codes and drops the whole object for anything else, never the base report', () => {
  const raw = report(); raw.devices = { ...devices(), path: 'C:\\DO_NOT_SEND' }; raw.devices.usb.name = 'DO_NOT_SEND';
  const safe = safeDiagnosticReport(raw);
  assert.deepEqual(safe.devices, devices()); assert.equal(JSON.stringify(safe).includes('DO_NOT_SEND'), false);
  const both = report(); both.devices = { ...devices(), ...inventory() }; both.devices.displayAdapters.serial = 'DO_NOT_SEND';
  assert.deepEqual(safeDiagnosticReport(both).devices, { ...devices(), ...inventory() });
  const only = report(); only.devices = { schema: 1, ...inventory() };
  assert.deepEqual(safeDiagnosticReport(only).devices, { schema: 1, ...inventory() }, 'either group may come alone');
  for (const mutate of [d => d.drives.webClient = 'starting', d => d.printers.reason = 'Spooler error 1722', d => d.usbController = 'vendor', d => d.displayAdapters.legacy = 65, d => d.displayAdapters.foreign = -1,
    d => d.displayAdapters.blocker = 'later', d => d.displayAdapters.cleanup = 'C:\\', d => d.displayAdapters.currentStarted = 1, d => delete d.displayAdapters.read, d => d.displayAdapters = []]) {
    const invalid = report(); invalid.devices = { ...devices(), ...inventory() }; mutate(invalid.devices);
    const result = safeDiagnosticReport(invalid);
    assert.ok(result, 'the report itself stays'); assert.equal(result.devices, undefined);
  }
  assert.equal(safeDiagnosticReport({ ...report(), devices: { schema: 1 } }).devices, undefined, 'an empty object carries nothing');
  for (const mutate of [d => d.schema = 2, d => delete d.usb, d => d.drives.winfsp = 'true', d => d.usb.package = 1, d => d.audio = null]) {
    const invalid = report(); invalid.devices = devices(); mutate(invalid.devices);
    const result = safeDiagnosticReport(invalid);
    assert.ok(result, 'the report itself stays'); assert.equal(result.devices, undefined);
  }
  assert.equal(safeDiagnosticReport(report()).devices, undefined, 'a Host without device status reports none');
  // At the size bound the base report wins.
  const full = report(); full.devices = devices(); const values = Object.fromEntries(['action', 'stage', 'monitors', 'width', 'height', 'error', 'expected', 'missing', 'missingOwned', 'fallback', 'stale'].map(key => [key, 0xffffffff]));
  for (const key of ['ok', 'installed', 'started', 'active', 'owned', 'pending', 'recovery', 'reboot', 'remote', 'running', 'registered', 'online', 'changed', 'restartAttempted', 'consoleExpected', 'consoleActive', 'remapped']) values[key] = false;
  full.events = Array.from({ length: 8 }, () => ({ time: full.collectedAt, event: 'displayObserved', values }));
  const bounded = safeDiagnosticReport(full); assert.ok(bounded); assert.ok(Buffer.byteLength(JSON.stringify(bounded)) <= 6144);
  if (bounded.devices) assert.deepEqual(bounded.devices, devices());
  assert.equal(bounded.events.length, 8);
});

const ownedMonitors=()=>[
  {id:'0',width:1920,height:1080,left:1920,top:0,primary:false},
  {id:'1',width:1280,height:720,left:3840,top:-120,primary:false}
];
test('Optional owned-output observations preserve geometry and absence of a primary without leaking identity',()=>{
  const raw=report();raw.display.monitorCount=2;raw.display.monitors=ownedMonitors();
  raw.display.monitors[0].displayDeviceName='DO_NOT_SEND';raw.display.monitors[1].serial='DO_NOT_SEND';
  const safe=safeDiagnosticReport(raw);assert.deepEqual(safe.display.monitors,ownedMonitors());
  assert.equal(JSON.stringify(safe).includes('DO_NOT_SEND'),false);assert.equal(safe.display.activeDisplay,false);assert.equal(safe.captureAvailable,false);
  const legacy=report();assert.equal(safeDiagnosticReport(legacy).display.monitors,undefined);
  const empty=report();empty.display.monitors=[];assert.deepEqual(safeDiagnosticReport(empty).display.monitors,[]);
  const one=report();one.display.monitorCount=1;one.display.monitors=[{id:'0',width:1920,height:1080,left:0,top:0,primary:true}];
  assert.deepEqual(safeDiagnosticReport(one).display.monitors,one.display.monitors);
  for(const mutate of [
    r=>r.display.monitors='DO_NOT_SEND',r=>r.display.monitors=[null,null],r=>r.display.monitors[0].id='DO_NOT_SEND',r=>r.display.monitors[0].id='1',
    r=>r.display.monitors[0].width=0,r=>r.display.monitors[0].height=16385,r=>r.display.monitors[0].width=1920.5,r=>r.display.monitors[0].height='720',
    r=>r.display.monitors[0].left=-32769,r=>r.display.monitors[0].top=32769,r=>r.display.monitors[0].left=NaN,r=>r.display.monitors[0].top=Infinity,
    r=>r.display.monitors[0].primary='false',r=>r.display.monitors[0].primary=true,r=>r.display.monitors[0].left=0,
    r=>r.display.monitors.forEach(m=>Object.assign(m,{left:0,top:0,primary:true})),r=>r.display.monitors.pop(),r=>r.display.monitors.push({...r.display.monitors[0]}),
    r=>r.display.owned=false,r=>Object.assign(r.display,{observed:false,observationAgeMs:null})
  ]){
    const changed=structuredClone(raw);mutate(changed);const result=safeDiagnosticReport(changed);
    assert.ok(result);assert.equal(result.display.monitors,undefined);assert.equal(result.display.recoveryRequired,true);assert.equal(JSON.stringify(result).includes('DO_NOT_SEND'),false);
  }
  const bounded=structuredClone(raw);bounded.display.monitors=ownedMonitors().map((m,i)=>({...m,width:16384,height:16384,left:i?32768:-32768,top:i?32768:-32768}));
  const values=Object.fromEntries(['action','stage','monitors','width','height','error','expected','missing','missingOwned','fallback','stale'].map(k=>[k,0xffffffff]));
  for(const key of ['ok','installed','started','active','owned','pending','recovery','reboot','remote','running','registered','online','changed','restartAttempted','consoleExpected','consoleActive','remapped'])values[key]=false;
  bounded.events=Array.from({length:8},()=>({time:bounded.collectedAt,event:'displayObserved',values}));
  const maximum=safeDiagnosticReport(bounded);assert.ok(maximum);assert.equal(maximum.display.monitors.length,2);assert.ok(Buffer.byteLength(JSON.stringify(maximum))<=6144);
});

async function fixture(t) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  store.addUser({ name: 'admin', passwordHash: 'fixture', role: 'admin' });store.addUser({ name: 'alice', passwordHash: 'fixture', role: 'user' });
  const adminToken = newToken(), userToken = newToken();store.createSession(store.findUser('admin'), tokenHash(adminToken), Date.now(), 60000, 'admin');store.createSession(store.findUser('alice'), tokenHash(userToken), Date.now(), 60000);
  const vms = [{ vmid: 149, node: 'fixture', name: 'TESTER', status: 'running' }];store.syncProxmox(vms, 'fixture');store.assign('alice', 'pve-149');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve.invalid', tokenId: 'fixture@pve!fixture', secret: 'fixture' }), 'fixture');
  const code = newToken(), hostToken = newToken(), machine = '00000000-1234-1234-1234-000000000001';store.issueEnrollment('pve-149', machine, tokenHash(code), 'fixture');store.consumeEnrollment(tokenHash(code), machine, { hostname: 'fixture', version: '0.2.24', tokenHash: tokenHash(hostToken) });
  const server = await buildServer({ store, vault, publicOrigin: 'https://diagnostics.invalid', proxmox: { inventory: async () => vms }, logger: { error: () => assert.fail('Unexpected server error') } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));t.after(async () => { await new Promise(resolve => server.close(resolve));store.close(); });
  const base = `http://127.0.0.1:${server.address().port}`;
  const connect = async () => { const ws = new WebSocket(base.replace('http:', 'ws:') + '/api/v1/hosts/channel', { headers: { Authorization: 'Host ' + hostToken, 'X-Multitap-Host-Protocol': 'capabilities-v1' } });await once(ws, 'open');return ws; };
  const ws = await connect();
  const send = async value => { const receive = once(ws, 'message');ws.send(JSON.stringify(value));const [bytes] = await receive;return JSON.parse(bytes.toString()); };
  const caps = { service: true, interactiveSession: true, streaming: false, virtualDisplayReady: false, monitors: [], diagnosticsSupported: true };
  const api = async (method = 'GET', who = 'admin', path = '/api/v1/admin/desktops/pve-149/diagnostics', body) => {
    const response = await fetch(base + path, { method, headers: { ...(who === 'admin' ? { Cookie: '__Secure-multitap-admin=' + adminToken } : who === 'alice' ? { Authorization: 'Bearer ' + userToken } : {}), ...(method === 'POST' ? { Origin: 'https://diagnostics.invalid', 'X-CSRF-Token': vault.csrf(adminToken), 'Content-Type': 'application/json' } : {}) }, body: body ? JSON.stringify(body) : undefined });return { status: response.status, body: await response.json() };
  };
  return { store, ws, send, caps, api, connect, hostToken, server };
}

test('Authenticated automatic upload persists latest report for admin only and cannot grant streaming', async t => {
  const f = await fixture(t);
  assert.deepEqual(await f.send({ type: 'host.capabilities', capabilities: f.caps }), { type: 'host.capabilities.ack', accepted: true, diagnosticsProtocol: 'host-diagnostics-v1', closingProtocol: 'host-closing-v1', hostResumeProtocol: 'host-resume-v1', resourcesProtocol: 'host-resources-v2', desktopName: 'TESTER' });
  assert.equal((await f.api()).body.report, null);
  const observed=report();observed.display.monitorCount=2;observed.display.monitors=ownedMonitors();
  assert.deepEqual(await f.send({ type: 'host.diagnostics', report: observed }), { type: 'host.diagnostics.ack', accepted: true });
  const stored = await f.api();assert.equal(stored.status, 200);assert.equal(stored.body.stale, false);assert.equal(stored.body.report.native.failureReason, 'controller_start_failed');
  assert.deepEqual(stored.body.report.display.monitors,ownedMonitors());
  for (const who of ['alice', null]) assert.equal((await f.api('GET', who)).status, 401);
  assert.equal((await f.api('GET', 'alice', '/api/v1/desktops/pve-149/diagnostics')).status, 404);
  const client = await f.api('GET', 'alice', '/api/v1/desktops');assert.deepEqual(client.body.desktops[0].connectionModes, { stream: false });assert.equal(JSON.stringify(client.body).includes('controller_start_failed'), false);
  assert.deepEqual(await f.send({ type: 'host.diagnostics', report: report() }), { type: 'host.diagnostics.ack', accepted: false });
  assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM host_diagnostics').get().n, 1);
  const request = once(f.ws, 'message');assert.equal((await f.api('POST', 'admin', undefined, {})).status, 202);assert.deepEqual(JSON.parse((await request)[0].toString()), { type: 'host.diagnostics.request' });
  assert.equal((await f.api('POST', 'admin', undefined, {})).status, 429);
  f.store.db.prepare('UPDATE host_diagnostics SET received_at=?').run(Date.now() - 100000);
  assert.equal((await f.api()).body.stale, true);
  const next = report();next.display.rebootRequired = true;
  assert.equal((await f.send({ type: 'host.diagnostics', report: next })).accepted, true);
  assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM host_diagnostics').get().n, 1);
  const closed = once(f.ws, 'close');f.ws.close();await closed;assert.equal((await f.api()).body.stale, true);
});

test('Missing negotiation, malformed reports and revoked hosts cannot populate diagnostic storage', async t => {
  const f = await fixture(t);
  const old = { ...f.caps };delete old.diagnosticsSupported;
  assert.deepEqual(await f.send({ type: 'host.capabilities', capabilities: old }), { type: 'host.capabilities.ack', accepted: true, closingProtocol: 'host-closing-v1', hostResumeProtocol: 'host-resume-v1', resourcesProtocol: 'host-resources-v2', desktopName: 'TESTER' });
  assert.equal((await f.send({ type: 'host.diagnostics', report: report() })).accepted, false);
  assert.equal((await f.api('POST', 'admin', undefined, {})).body.error.code, 'host_diagnostics_update_required');
  await f.send({ type: 'host.capabilities', capabilities: f.caps });
  const invalid = report();invalid.display.width = -1;
  assert.equal((await f.send({ type: 'host.diagnostics', report: invalid })).accepted, false);
  assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM host_diagnostics').get().n, 0);
  assert.equal((await f.send({ type: 'host.heartbeat', capabilities: f.caps })).type, 'host.heartbeat.ack');
  const identity = f.store.authenticateHost(tokenHash(f.hostToken));
  const host = { identity, tokenHash: tokenHash(f.hostToken), capabilities: f.caps };
  const boundary = { hosts: new Map([['pve-149', host]]), store: f.store };
  boundary.hosts.set('pve-149', { ...host });
  assert.equal(Relay.prototype.receiveDiagnostics.call(boundary, host, { report: report() }), false);
  boundary.hosts.set('pve-149', host);
  f.store.db.prepare('UPDATE hosts SET revoked=1').run();
  assert.equal(Relay.prototype.receiveDiagnostics.call(boundary, host, { report: report() }), false);
  assert.equal((await f.api()).body.report, null);
  const failedDisk={hosts:new Map([['pve-149',host]]),store:{authenticateHost:()=>identity,db:{prepare(){throw Error('isolated disk failure');}}}};
  assert.equal(Relay.prototype.receiveDiagnostics.call(failedDisk,host,{report:report()}),false);
});

test('Actual production C++ diagnostic bytes cross the authenticated control socket and retain safe fields', { skip: !process.env.MULTITAP_HOST_DIAGNOSTIC_VECTOR }, async t => {
  const bytes = readFileSync(process.env.MULTITAP_HOST_DIAGNOSTIC_VECTOR, 'utf8');
  assert.ok(Buffer.byteLength(bytes) <= 6144);
  const native = JSON.parse(bytes), safe = safeDiagnosticReport(native);assert.ok(safe);
  const f = await fixture(t);await f.send({ type: 'host.capabilities', capabilities: f.caps });
  const ack = once(f.ws, 'message');f.ws.send('{"type":"host.diagnostics","report":' + bytes + '}');
  assert.deepEqual(JSON.parse((await ack)[0].toString()), { type: 'host.diagnostics.ack', accepted: true });
  const value = (await f.api()).body;assert.deepEqual(value.report, safe);assert.equal(value.report.events.length, 8);
  // Old retained fixtures remain compatible; new C++ vectors must keep their
  // projected outputs unchanged across transport, storage and the admin API.
  if(native.display.monitors!==undefined){assert.deepEqual(value.report.display.monitors,native.display.monitors);assert.equal(value.report.display.monitors.length,2);}
});

test('connect-timing-v1: hostAction 200 stage 10 keeps the start stages as numbers', () => {
  const raw = report();raw.events = [{ time: new Date().toISOString(), event: 'hostAction', values: { action: 200, ok: true, stage: 10, error: 0, admitMs: 3, workerMs: 180, displayMs: 240, mediaMs: 120, frameMs: 41, startMs: 9, displayMsText: 'DO_NOT_SEND', mediaMsFlag: true } }];
  assert.deepEqual(safeDiagnosticReport(raw).events[0].values, { action: 200, ok: true, stage: 10, error: 0, admitMs: 3, workerMs: 180, displayMs: 240, mediaMs: 120, frameMs: 41 });
});
