// A Host request from a VM that never had a Host is placed by the server:
// the request's SMBIOS UUID is looked up in
// Proxmox instead of asking the administrator for node and VM ID. Synthetic
// Store, vault and Proxmox doubles only; no network.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { setTimeout as delay } from 'node:timers/promises';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { HostRequests } from '../src/host-requests.mjs';
import { HostVmLookup } from '../src/host-vm-lookup.mjs';
import { Proxmox } from '../src/proxmox.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { HttpError } from '../src/errors.mjs';

const uuidNew = 'aaaaaaaa-1111-4111-8111-000000000174', uuidKnown = '12345678-1234-1234-1234-123456789abc', uuidOther = 'bbbbbbbb-2222-4222-8222-000000000300';
const metadata = (machineUuid = uuidNew, requestKey = newToken()) => ({ requestKey, machineUuid, hostname: 'NEW-WINDOWS', version: '1.0.44' });
const error = code => value => value instanceof HttpError && value.code === code;
// machines: uuid -> [{ node, vmid, name }], as Proxmox.machineIndex returns it.
function fixture(t, { machines = new Map([[uuidNew, [{ node: 'pve-node-a', vmid: 174, name: 'CRAFT-VM' }]]]), unread = 0, options = {} } = {}) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32)); let clock = 1_000_000, scans = 0, identityReads = [], failure = null, gate = null, identity = null;
  const admin = store.addUser({ name: 'fixture-admin', role: 'admin', passwordHash: 'isolated-unused-password-hash' });
  // 710 is known to the table (bound UUID); 174 is a row without a bound UUID,
  // as an older inventory import left it; 175 has no row at all.
  store.syncProxmox([{ vmid: 710, node: 'fixture-node', name: 'Known VM', status: 'running', machineUuid: uuidKnown }, { vmid: 174, node: 'old-node', name: 'VM 174 row', status: 'running' }], 'fixture');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve-fixture.invalid', tokenId: 'fixture@pve!lookup', secret: 'isolated-not-real-secret' }), 'fixture');
  const proxmox = {
    machineIndex: async (config, limits) => { scans++; proxmox.lastLimits = limits; if (gate) await gate; if (failure) throw failure; return { machines: f.machines, total: [...f.machines.values()].flat().length, unread: f.unread }; },
    machineIdentity: async (config, vmid, target) => { identityReads.push({ vmid, ...target }); if (identity instanceof Error) throw identity; return { machineUuid: identity ?? target.expectedUuid }; },
  };
  const requests = new HostRequests({ store, vault, proxmox, now: () => clock, lookupWaitMs: 200, ...options });
  t.after(() => store.close());
  const f = { store, vault, proxmox, requests, admin, machines, unread, authorize: () => admin, advance: ms => { clock += ms; }, identityReads,
    fail: value => { failure = value; }, hold: value => { gate = value; }, setIdentity: value => { identity = value; }, get scans() { return scans; },
    create(input = metadata(), shows = true) { const created = requests.create(input, 'address', shows); clock += 1; return { input, id: created.requestId }; },
    async listed(id) { return (await requests.pendingResolved()).requests.find(value => value.requestId === id); } };
  return f;
}

test('a request from a VM without an earlier Host is placed by its UUID, with the existing fields unchanged', async t => {
  const f = fixture(t), request = f.create(), listed = await f.listed(request.id);
  assert.deepEqual(listed.vmLookup, { state: 'matched', vm: { node: 'pve-node-a', vmid: 174, name: 'CRAFT-VM', desktopId: 'pve-174', hasExistingHost: false } });
  // Backward compatible: the table-based fields say what they always said.
  assert.equal(listed.matchState, 'unknown'); assert.equal(listed.suggestedDesktopId, null); assert.deepEqual(listed.candidates, []);
  assert.equal(listed.localPcRegistration.allowed, false); assert.equal(listed.localPcRegistration.reason, 'host_request_matches_vm');
  assert.equal(f.scans, 1); assert.equal(f.proxmox.lastLimits.maxVms, 1000); assert.ok(f.proxmox.lastLimits.signal instanceof AbortSignal);
  assert.equal(f.identityReads.length, 0, 'listing never binds or verifies an identity');
  assert.equal(f.store.db.prepare("SELECT pve_machine_uuid FROM desktops WHERE id='pve-174'").get().pve_machine_uuid, null, 'a lookup result is not stored as an identity');
});

test('the found VM is approved with one confirmation and the code tick, with the fresh identity check and the Host binding as before', async t => {
  const f = fixture(t), request = f.create(); await f.listed(request.id);
  // The found VM needs no typing, but the code on the Host screen is still compared and ticked.
  for (const input of [{ node: 'pve-node-a', vmid: 174, serverMatch: true }, { node: 'pve-node-a', vmid: 174, serverMatch: true, acknowledgeCode: false }, { node: 'pve-node-a', vmid: 174, serverMatch: true, replaceExisting: true }])
    await assert.rejects(f.requests.approve(request.id, input, f.authorize), error('host_request_code_required'), JSON.stringify(input));
  // A Host that shows no code cannot be approved on this path either.
  const g = fixture(t), silent = g.create(metadata(), false); await g.listed(silent.id);
  await assert.rejects(g.requests.approve(silent.id, { node: 'pve-node-a', vmid: 174, serverMatch: true, acknowledgeCode: true }, g.authorize), error('host_request_code_required'));
  assert.equal(f.identityReads.length + g.identityReads.length, 0);
  const result = await f.requests.approve(request.id, { node: 'pve-node-a', vmid: 174, serverMatch: true, acknowledgeCode: true }, f.authorize);
  assert.deepEqual(result, { ok: true, requestId: request.id, status: 'approved', desktopId: 'pve-174' });
  assert.deepEqual(f.identityReads, [{ vmid: 174, node: 'pve-node-a', expectedUuid: uuidNew }]);
  const desktop = f.store.db.prepare("SELECT pve_node,pve_machine_uuid,name FROM desktops WHERE id='pve-174'").get();
  assert.equal(desktop.pve_node, 'pve-node-a', 'the node Proxmox reported replaces the stale one'); assert.equal(desktop.pve_machine_uuid, uuidNew); assert.equal(desktop.name, 'VM 174 row');
  const credentials = f.requests.status({ requestId: request.id, requestKey: request.input.requestKey }).credentials;
  assert.equal(f.store.authenticateHost(tokenHash(credentials.hostToken)).desktop_id, 'pve-174');
});

test('a VM without any desktop row gets one with Proxmox\'s name', async t => {
  const f = fixture(t, { machines: new Map([[uuidNew, [{ node: 'pve-node-a', vmid: 175, name: 'BRAND-NEW' }]]]) }), request = f.create();
  assert.deepEqual((await f.listed(request.id)).vmLookup.vm, { node: 'pve-node-a', vmid: 175, name: 'BRAND-NEW', desktopId: null, hasExistingHost: false });
  assert.equal((await f.requests.approve(request.id, { node: 'pve-node-a', vmid: 175, serverMatch: true, acknowledgeCode: true }, f.authorize)).desktopId, 'pve-175');
  assert.deepEqual({ ...f.store.db.prepare("SELECT name,pve_node,pve_vmid,pve_machine_uuid FROM desktops WHERE id='pve-175'").get() }, { name: 'BRAND-NEW', pve_node: 'pve-node-a', pve_vmid: 175, pve_machine_uuid: uuidNew });
  assert.equal(f.store.db.prepare("SELECT count(*) AS n FROM assignments WHERE desktop_id='pve-175'").get().n, 0);
});

test('approval through the found VM still fails when the fresh identity check disagrees', async t => {
  const f = fixture(t), request = f.create(); await f.listed(request.id);
  const hosts = () => f.store.db.prepare('SELECT count(*) AS n FROM hosts').get().n, input = { node: 'pve-node-a', vmid: 174, serverMatch: true, acknowledgeCode: true };
  // Proxmox now says another UUID (the VM was replaced after the lookup).
  f.setIdentity(new HttpError(409, 'proxmox_machine_uuid_mismatch'));
  await assert.rejects(f.requests.approve(request.id, input, f.authorize), error('proxmox_machine_uuid_mismatch'));
  f.setIdentity(uuidOther);
  await assert.rejects(f.requests.approve(request.id, input, f.authorize), error('host_machine_mismatch'));
  f.setIdentity(new HttpError(502, 'proxmox_unreachable'));
  await assert.rejects(f.requests.approve(request.id, input, f.authorize), error('proxmox_unreachable'));
  assert.equal(hosts(), 0); assert.equal(f.identityReads.length, 3);
  assert.equal(f.requests.status({ requestId: request.id, requestKey: request.input.requestKey }).status, 'pending');
  assert.equal(f.store.db.prepare("SELECT pve_machine_uuid FROM desktops WHERE id='pve-174'").get().pve_machine_uuid, null);
  f.setIdentity(null);
  assert.equal((await f.requests.approve(request.id, input, f.authorize)).status, 'approved');
});

test('serverMatch cannot be used to choose a VM: only the server\'s own single match is accepted', async t => {
  const f = fixture(t), request = f.create(); await f.listed(request.id);
  for (const input of [{ node: 'pve-node-a', vmid: 710 }, { node: 'other-node', vmid: 174 }, { node: 'fixture-node', vmid: 710 }])
    await assert.rejects(f.requests.approve(request.id, { ...input, serverMatch: true, acknowledgeCode: true }, f.authorize), error('host_request_match_changed'));
  // Combined with a table desktop, or malformed, it is refused outright.
  for (const input of [{ desktopId: 'pve-710', serverMatch: true, acknowledgeCode: true }, { node: 'pve-node-a', vmid: 174, serverMatch: 'yes' }, { node: 'pve-node-a', vmid: 174, serverMatch: 1 }])
    await assert.rejects(f.requests.approve(request.id, input, f.authorize), value => value instanceof HttpError && value.status === 400 && value.code === 'invalid_host_approval');
  // serverMatch:false is the typed path with its code requirement.
  await assert.rejects(f.requests.approve(request.id, { node: 'pve-node-a', vmid: 174, serverMatch: false }, f.authorize), error('host_request_code_required'));
  // A request the server never looked up has no match to lean on.
  const g = fixture(t), unseen = g.create();
  await assert.rejects(g.requests.approve(unseen.id, { node: 'pve-node-a', vmid: 174, serverMatch: true, acknowledgeCode: true }, g.authorize), error('host_request_match_changed'));
  assert.equal(f.identityReads.length + g.identityReads.length, 0); assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM hosts').get().n, 0);
});

test('the same UUID on two VMs is ambiguous: nothing is suggested and typing still needs the code', async t => {
  const f = fixture(t, { machines: new Map([[uuidNew, [{ node: 'pve-node-a', vmid: 174, name: 'ORIGINAL' }, { node: 'pve-node-a', vmid: 9001, name: 'RESTORED-COPY' }]]]) }), request = f.create();
  const listed = await f.listed(request.id);
  assert.deepEqual(listed.vmLookup, { state: 'ambiguous', count: 2 }); assert.equal(listed.localPcRegistration.allowed, false);
  for (const vmid of [174, 9001]) await assert.rejects(f.requests.approve(request.id, { node: 'pve-node-a', vmid, serverMatch: true, acknowledgeCode: true }, f.authorize), error('host_request_match_changed'));
  await assert.rejects(f.requests.approve(request.id, { node: 'pve-node-a', vmid: 174 }, f.authorize), error('host_request_code_required'));
  assert.equal(f.identityReads.length, 0);
});

test('a UUID no VM carries is not found, and the typed fallback keeps today\'s rules', async t => {
  const f = fixture(t, { machines: new Map([[uuidOther, [{ node: 'pve-node-a', vmid: 300, name: 'SOMETHING-ELSE' }]]]) }), request = f.create();
  const listed = await f.listed(request.id);
  assert.deepEqual(listed.vmLookup, { state: 'not_found' }); assert.equal(listed.localPcRegistration.allowed, true, 'not a VM: still registrable as a local PC');
  await assert.rejects(f.requests.approve(request.id, { node: 'pve-node-a', vmid: 174, serverMatch: true, acknowledgeCode: true }, f.authorize), error('host_request_match_changed'));
  await assert.rejects(f.requests.approve(request.id, { node: 'pve-node-a', vmid: 174 }, f.authorize), error('host_request_code_required'));
  assert.equal((await f.requests.approve(request.id, { node: 'pve-node-a', vmid: 174, acknowledgeCode: true }, f.authorize)).status, 'approved');
});

test('an incomplete read never claims "not found"', async t => {
  const f = fixture(t, { machines: new Map(), unread: 2 }), request = f.create();
  assert.deepEqual((await f.listed(request.id)).vmLookup, { state: 'unavailable', errorCode: 'proxmox_lookup_incomplete' });
});

test('a Proxmox failure is reported as unavailable, cached, and retried no sooner than the minimum interval', async t => {
  const f = fixture(t), request = f.create(); f.fail(new HttpError(502, 'proxmox_unreachable'));
  assert.deepEqual((await f.listed(request.id)).vmLookup, { state: 'unavailable', errorCode: 'proxmox_unreachable' });
  for (let i = 0; i < 5; i++) { f.advance(2999); assert.equal((await f.listed(request.id)).vmLookup.state, 'unavailable'); }
  assert.equal(f.scans, 1, 'five console polls within 15 s do not ask Proxmox again');
  await assert.rejects(f.requests.approve(request.id, { node: 'pve-node-a', vmid: 174, serverMatch: true, acknowledgeCode: true }, f.authorize), error('host_request_match_changed'));
  f.advance(10); f.fail(null);
  assert.equal((await f.listed(request.id)).vmLookup.state, 'matched'); assert.equal(f.scans, 2);
  // An unexpected error and a Proxmox double without the lookup read as unavailable too.
  const g = fixture(t), other = g.create(); g.fail(new Error('synthetic'));
  assert.deepEqual((await g.listed(other.id)).vmLookup, { state: 'unavailable', errorCode: 'proxmox_unreachable' });
  const h = fixture(t); delete h.proxmox.machineIndex; const old = h.create();
  assert.deepEqual((await h.listed(old.id)).vmLookup, { state: 'unavailable', errorCode: 'proxmox_lookup_unavailable' });
});

test('many unverified requests cost one cluster read; polls are served from it', async t => {
  const f = fixture(t, { options: { createLimit: 1000 } }), ids = [];
  for (let i = 0; i < 60; i++) ids.push(f.create(metadata('cccccccc-3333-4333-8333-' + String(i).padStart(12, '0'))).id);
  const real = f.create();
  const [first, second, third] = await Promise.all([f.requests.pendingResolved(), f.requests.pendingResolved(), f.requests.pendingResolved()]);
  assert.equal(f.scans, 1, 'concurrent polls share the one running read');
  for (const listing of [first, second, third]) {
    assert.equal(listing.requests.find(value => value.requestId === real.id).vmLookup.state, 'matched');
    assert.ok(ids.every(id => listing.requests.find(value => value.requestId === id).vmLookup.state === 'not_found'));
  }
  // Twenty polls over a minute of console time: nothing new to place, so no read until the snapshot ages out.
  for (let i = 0; i < 19; i++) { f.advance(3000); await f.requests.pendingResolved(); }
  assert.equal(f.scans, 1);
  f.advance(3001); await f.requests.pendingResolved(); assert.equal(f.scans, 2, 'refreshed once after maxAgeMs while requests still wait');
});

test('a flood of new UUIDs cannot make Proxmox reads more frequent than the minimum interval', async t => {
  const f = fixture(t, { options: { createLimit: 1000 } }); let n = 0;
  const flood = () => f.create(metadata('dddddddd-4444-4444-8444-' + String(n++).padStart(12, '0')));
  flood(); await f.requests.pendingResolved(); assert.equal(f.scans, 1);
  // A new claim every second for 60 s, with the console polling after each one.
  for (let second = 0; second < 60; second++) { f.advance(999); const late = flood(); const state = (await f.listed(late.id)).vmLookup.state; assert.ok(['searching', 'not_found'].includes(state), state); }
  assert.ok(f.scans <= 1 + 4, `at most one read per 15 s, saw ${f.scans}`); assert.ok(f.scans >= 4);
});

test('nothing is read from Proxmox without a request that needs placing, or from the Host-facing calls', async t => {
  const f = fixture(t);
  await f.requests.pendingResolved(); assert.equal(f.scans, 0, 'no pending request');
  const known = f.create(metadata(uuidKnown)); const listed = await f.listed(known.id);
  assert.equal(listed.matchState, 'matched'); assert.deepEqual(listed.vmLookup, { state: 'known' }); assert.equal(f.scans, 0, 'the table places it');
  const fresh = f.create();
  for (let i = 0; i < 10; i++) { f.requests.status({ requestId: fresh.id, requestKey: fresh.input.requestKey }, 'address', true); f.requests.pending(); }
  assert.equal(f.scans, 0, 'creating and polling a request (unauthenticated) and the plain listing never read Proxmox');
  assert.equal(f.requests.pending().requests.find(value => value.requestId === fresh.id).vmLookup.state, 'searching');
});

test('a slow read does not hold the console: the request shows as searching and the next poll has the answer', async t => {
  const f = fixture(t, { options: { lookupWaitMs: 20 } }), request = f.create(); let release; f.hold(new Promise(resolve => { release = resolve; }));
  const started = Date.now(), listed = await f.listed(request.id);
  assert.deepEqual(listed.vmLookup, { state: 'searching' }); assert.ok(Date.now() - started < 2000);
  assert.equal((await f.listed(request.id)).vmLookup.state, 'searching'); assert.equal(f.scans, 1, 'no second read while one runs');
  release(); await delay(20);
  assert.equal((await f.listed(request.id)).vmLookup.state, 'matched'); assert.equal(f.scans, 1);
});

test('a snapshot belongs to the Proxmox settings it was read with', async t => {
  const f = fixture(t), request = f.create(); assert.equal((await f.listed(request.id)).vmLookup.state, 'matched');
  f.store.saveSetting('proxmox', f.vault.seal({ endpoint: 'https://other-pve.invalid', tokenId: 'fixture@pve!other', secret: 'isolated-not-real-secret' }), 'fixture');
  assert.equal(f.requests.pending().requests[0].vmLookup.state, 'searching');
  await assert.rejects(f.requests.approve(request.id, { node: 'pve-node-a', vmid: 174, serverMatch: true, acknowledgeCode: true }, f.authorize), error('host_request_match_changed'));
  assert.equal((await f.listed(request.id)).vmLookup.state, 'searching', 'still inside the minimum interval'); assert.equal(f.scans, 1);
  f.advance(15000); assert.equal((await f.listed(request.id)).vmLookup.state, 'matched'); assert.equal(f.scans, 2);
});

test('duplicate claims are guarded on the found-VM path exactly as before', async t => {
  const f = fixture(t), squat = f.create(metadata(), true), real = f.create(metadata(), true), input = { node: 'pve-node-a', vmid: 174, serverMatch: true, acknowledgeCode: true };
  const listed = await f.listed(real.id); assert.deepEqual(listed.duplicateRequestIds, [squat.id]); assert.equal(listed.vmLookup.state, 'matched');
  await assert.rejects(f.requests.approve(real.id, input, f.authorize), error('host_request_duplicate'));
  assert.equal(f.identityReads.length, 0);
  assert.equal((await f.requests.approve(real.id, { ...input, acknowledgeDuplicates: true }, f.authorize)).status, 'approved');
  // A claimant that shows no code cannot be told apart from its rival.
  const g = fixture(t), silent = g.create(metadata(), false); g.create(metadata(), true); await g.listed(silent.id);
  await assert.rejects(g.requests.approve(silent.id, { ...input, acknowledgeDuplicates: true }, g.authorize), error('host_request_code_required'));
  // Once bound by an approval, the table places the VM itself and guards a second Host as before.
  const h = fixture(t), one = h.create(metadata(), true); await h.listed(one.id); await h.requests.approve(one.id, input, h.authorize);
  const next = h.create(metadata(), true), relisted = await h.listed(next.id);
  assert.equal(relisted.matchState, 'matched'); assert.deepEqual(relisted.vmLookup, { state: 'known' }); assert.equal(relisted.candidates[0].hasExistingHost, true);
  await assert.rejects(h.requests.approve(next.id, { desktopId: 'pve-174', acknowledgeCode: true }, h.authorize), error('host_already_registered'));
});

test('another machine found under a VM number that had a Host needs the explicit replacement and inherits no assignment', async t => {
  const f = fixture(t), code = tokenHash(newToken()), user = f.store.addUser({ name: 'earlier-user', passwordHash: 'isolated-unused-password-hash' });
  // VM 174 was machine uuidOther with a registered Host and an assigned user.
  f.store.issueEnrollment('pve-174', uuidOther, code, f.admin);
  const old = f.store.consumeEnrollment(code, uuidOther, { hostname: 'OLD-MACHINE', version: '1.0.40', tokenHash: tokenHash(newToken()) });
  f.store.assign('earlier-user', 'pve-174', f.admin); f.store.assign('earlier-user', 'pve-710', f.admin);
  const request = f.create(), listed = await f.listed(request.id), input = { node: 'pve-node-a', vmid: 174, serverMatch: true, acknowledgeCode: true };
  assert.deepEqual(listed.candidates, []);
  assert.deepEqual(listed.vmLookup, { state: 'matched', vm: { node: 'pve-node-a', vmid: 174, name: 'CRAFT-VM', desktopId: 'pve-174', hasExistingHost: true } });
  await assert.rejects(f.requests.approve(request.id, input, f.authorize), error('host_already_registered'));
  assert.equal(f.store.userAssignments(user).length, 2);
  assert.equal((await f.requests.approve(request.id, { ...input, replaceExisting: true }, f.authorize)).desktopId, 'pve-174');
  assert.deepEqual(f.store.userAssignments(user).map(value => value.desktopId), ['pve-710'], 'the new machine starts with nobody assigned');
  assert.deepEqual({ ...f.store.db.prepare("SELECT actor,subject FROM audit_events WHERE event='desktop_assignments_removed'").get() }, { actor: f.admin, subject: 'pve-174:1' });
  assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM hosts WHERE id=?').get(old.hostId).n, 0);
});

test('a machine Proxmox lists as a VM cannot be registered as a local PC', async t => {
  const f = fixture(t), request = f.create(); await f.listed(request.id);
  await assert.rejects(f.requests.approveLocalPc(request.id, { name: 'Not a PC', acknowledgeCode: true, acknowledgeUnlinked: true }, f.authorize), error('host_request_matches_vm'));
  assert.equal(f.store.db.prepare("SELECT count(*) AS n FROM desktops WHERE kind='pc'").get().n, 0);
  const g = fixture(t, { machines: new Map() }), pc = g.create(); await g.listed(pc.id);
  assert.equal((await g.requests.approveLocalPc(pc.id, { name: 'Real PC', acknowledgeCode: true, acknowledgeUnlinked: true }, g.authorize)).status, 'approved');
});

test('HostVmLookup: one read at a time, bounded limits passed on, old snapshots dropped', async t => {
  let clock = 0, calls = 0; const limits = [];
  const lookup = new HostVmLookup({ now: () => clock, scanMs: 5000, maxVms: 50, keepMs: 100000,
    proxmox: { machineIndex: async (config, value) => { calls++; limits.push(value); return { machines: new Map([[uuidNew, [{ node: 'n', vmid: 100, name: null }]]]), total: 1, unread: 0 }; } } });
  assert.equal(lookup.refresh('r1', {}, []), null, 'nothing wanted, nothing read');
  const wanted = [{ uuid: uuidNew, createdAt: 0 }], running = lookup.refresh('r1', {}, wanted);
  assert.equal(lookup.refresh('r1', {}, wanted), running); assert.equal(lookup.refresh('r2', {}, wanted), null);
  await running; assert.equal(calls, 1); assert.equal(limits[0].maxVms, 50); assert.ok(limits[0].signal instanceof AbortSignal);
  assert.deepEqual(lookup.view('r1', uuidNew, 0), { state: 'matched', vm: { node: 'n', vmid: 100, name: null } });
  assert.deepEqual(lookup.view('r2', uuidNew, 0), { state: 'searching' });
  assert.deepEqual(lookup.view('r1', uuidOther, 0), { state: 'not_found' }); assert.deepEqual(lookup.view('r1', uuidOther, 1), { state: 'searching' });
  clock = 100001; assert.deepEqual(lookup.view('r1', uuidNew, 0), { state: 'searching' }, 'a snapshot older than keepMs is not shown or used');
});

// Proxmox.machineIndex against a stubbed transport: what is asked and how much.
function cluster(vms, { fail = new Set() } = {}) {
  const proxmox = new Proxmox(), paths = []; let active = 0, peak = 0;
  proxmox.call = async (config, path, method, body, options) => {
    paths.push(path); assert.equal(method ?? 'GET', 'GET'); assert.ok(options?.signal instanceof AbortSignal, 'every read carries the deadline');
    if (path === '/cluster/resources?type=vm') return vms.map(({ smbios1, ...row }) => row);
    const match = /^\/nodes\/([^/]+)\/qemu\/(\d+)\/config\?current=1$/.exec(path); assert.ok(match, path);
    active++; peak = Math.max(peak, active); await delay(2); active--;
    if (fail.has(Number(match[2]))) throw new HttpError(502, 'proxmox_unreachable');
    const vm = vms.find(value => value.vmid === Number(match[2]) && value.node === match[1]);
    return { name: vm.name, smbios1: vm.smbios1, ...(vm.template ? { template: 1 } : {}) };
  };
  return { proxmox, paths, peak: () => peak };
}
const smbios = value => `uuid=${value},manufacturer=U1lOVEg=,base64=1`;
test('Proxmox.machineIndex reads one listing and each QEMU VM once, four at a time, and reports what it could not read', async () => {
  const vms = [
    { type: 'qemu', node: 'pve1', vmid: 100, name: 'one', smbios1: smbios(uuidNew) },
    { type: 'qemu', node: 'pve2', vmid: 101, name: 'copy', smbios1: smbios(uuidNew.toUpperCase()) },
    { type: 'qemu', node: 'pve1', vmid: 102, name: 'other', smbios1: smbios(uuidOther) },
    { type: 'qemu', node: 'pve1', vmid: 103, name: 'unreadable', smbios1: smbios(uuidKnown) },
    { type: 'qemu', node: 'pve1', vmid: 104, name: 'no-uuid', smbios1: 'manufacturer=U1lOVEg=' },
    { type: 'qemu', node: 'pve1', vmid: 105, name: 'template', template: 1, smbios1: smbios(uuidKnown) },
    { type: 'lxc', node: 'pve1', vmid: 106, name: 'container' },
    { type: 'qemu', node: 'bad/node', vmid: 107, name: 'invalid-node', smbios1: smbios(uuidKnown) },
    { type: 'qemu', node: 'pve1', vmid: 100, name: 'duplicate-row', smbios1: smbios(uuidKnown) },
    ...Array.from({ length: 12 }, (_, i) => ({ type: 'qemu', node: 'pve1', vmid: 200 + i, name: 'filler' + i, smbios1: smbios('eeeeeeee-5555-4555-8555-' + String(i).padStart(12, '0')) })),
  ];
  const f = cluster(vms, { fail: new Set([103]) }), result = await f.proxmox.machineIndex({}, { signal: AbortSignal.timeout(5000) });
  assert.deepEqual(result.machines.get(uuidNew), [{ node: 'pve1', vmid: 100, name: 'one' }, { node: 'pve2', vmid: 101, name: 'copy' }], 'the same UUID on two VMs is reported as two');
  assert.deepEqual(result.machines.get(uuidOther), [{ node: 'pve1', vmid: 102, name: 'other' }]);
  assert.equal(result.machines.has(uuidKnown), false); assert.equal(result.total, 17); assert.equal(result.unread, 1);
  assert.equal(f.paths.filter(path => path.startsWith('/cluster/')).length, 1); assert.equal(f.paths.length, 1 + 17);
  assert.equal(new Set(f.paths).size, f.paths.length, 'no VM is read twice'); assert.ok(f.peak() <= 4 && f.peak() >= 2, 'peak ' + f.peak());
  assert.ok(f.paths.every(path => path === '/cluster/resources?type=vm' || /\/config\?current=1$/.test(path)), 'read-only config reads, nothing else');
});
test('Proxmox.machineIndex caps the work and stops at the deadline', async () => {
  const vms = Array.from({ length: 30 }, (_, i) => ({ type: 'qemu', node: 'pve1', vmid: 100 + i, name: 'vm' + i, smbios1: smbios('ffffffff-6666-4666-8666-' + String(i).padStart(12, '0')) }));
  const capped = cluster(vms), result = await capped.proxmox.machineIndex({}, { signal: AbortSignal.timeout(5000), maxVms: 10 });
  assert.equal(capped.paths.length, 1 + 10); assert.equal(result.machines.size, 10); assert.equal(result.unread, 20); assert.equal(result.total, 30);
  const aborted = cluster(vms), controller = new AbortController(); controller.abort();
  const late = await aborted.proxmox.machineIndex({}, { signal: controller.signal });
  assert.equal(late.machines.size, 0); assert.equal(late.unread, 30); assert.equal(aborted.paths.length, 1, 'no VM is read after the deadline');
  const broken = new Proxmox(); broken.call = async () => ({ not: 'a list' });
  await assert.rejects(broken.machineIndex({}, { signal: AbortSignal.timeout(1000) }), error('proxmox_invalid_response'));
});

test('the administrator request list carries the lookup over HTTP and the found VM approves through the route', async t => {
  const origin = 'https://lookup.fixture.invalid', store = new Store(':memory:'), vault = new Vault(Buffer.alloc(32, 7)), token = newToken();
  store.addUser({ name: 'operator', role: 'admin', passwordHash: 'synthetic' });
  store.createSession(store.findUser('operator'), tokenHash(token), Date.now(), 60000, 'admin');
  store.syncProxmox([{ vmid: 710, node: 'fixture-node', name: 'Known VM', status: 'running', machineUuid: uuidKnown }], 'fixture');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve-fixture.invalid', tokenId: 'fixture@pve!http', secret: 'isolated-not-real-secret' }), 'fixture');
  let scans = 0; const errors = [];
  const proxmox = { inventory: async () => [], machineIndex: async () => { scans++; return { machines: new Map([[uuidNew, [{ node: 'pve-node-a', vmid: 174, name: 'CRAFT-VM' }]]]), total: 1, unread: 0 }; },
    machineIdentity: async (config, vmid, target) => ({ machineUuid: target.expectedUuid }) };
  const server = await buildServer({ store, vault, publicOrigin: origin, proxmox, relayOptions: { authorizationMs: 60000, heartbeatMs: 60000 }, logger: { error: value => errors.push(value) } });
  await new Promise(done => server.listen(0, '127.0.0.1', done)); const base = 'http://127.0.0.1:' + server.address().port;
  t.after(async () => { await new Promise(done => server.close(done)); store.close(); assert.deepEqual(errors, []); });
  const call = async (path, { method = 'GET', data, admin = true } = {}) => {
    const response = await fetch(base + '/api/v1/' + path, { method, headers: { Origin: origin, 'X-Multitap-Host-Request-Code': 'v1', ...(admin ? { Cookie: '__Secure-multitap-admin=' + token, 'X-CSRF-Token': vault.csrf(token) } : {}), ...(data === undefined ? {} : { 'Content-Type': 'application/json' }) }, body: data === undefined ? undefined : JSON.stringify(data) });
    return { status: response.status, body: await response.json() };
  };
  const input = metadata(), created = await call('hosts/requests', { method: 'POST', data: input, admin: false }); assert.equal(created.status, 201, created.body.error?.code);
  assert.equal(scans, 0, 'the unauthenticated request alone reads nothing from Proxmox');
  await delay(5);
  const listing = await call('admin/hosts/requests'); assert.equal(listing.status, 200);
  assert.deepEqual(listing.body.requests[0].vmLookup, { state: 'matched', vm: { node: 'pve-node-a', vmid: 174, name: 'CRAFT-VM', desktopId: null, hasExistingHost: false } });
  for (let i = 0; i < 5; i++) await call('admin/hosts/requests'); assert.equal(scans, 1);
  const refused = await call('admin/hosts/requests/' + created.body.requestId + '/approve', { method: 'POST', data: { node: 'pve-node-a', vmid: 174, serverMatch: true, replaceExisting: false } });
  assert.equal(refused.status, 409); assert.equal(refused.body.error.code, 'host_request_code_required', 'the found VM still needs the code tick');
  const approved = await call('admin/hosts/requests/' + created.body.requestId + '/approve', { method: 'POST', data: { node: 'pve-node-a', vmid: 174, serverMatch: true, acknowledgeCode: true, replaceExisting: false } });
  assert.equal(approved.status, 200, approved.body.error?.code); assert.equal(approved.body.desktopId, 'pve-174');
  assert.equal((await call('admin/hosts/requests')).body.pendingCount, 0);
});
