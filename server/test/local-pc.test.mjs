import { captureAssignmentInput } from './fixtures/admin-assignment-input.mjs';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { HostRequests } from '../src/host-requests.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { buildServer } from '../src/http.mjs';
import { Relay } from '../src/relay.mjs';
import { initialViewportSessionSupported } from '../src/initial-viewport.mjs';

const pcUuid = 'a1234567-89ab-cdef-8123-0123456789ab';
const vmUuid = 'b1234567-89ab-cdef-8123-0123456789ab';
const vm = { vmid: 710, node: 'fixture', name: 'VM', status: 'running', machineUuid: vmUuid };
const fails = code => error => error.code === code;
function fixture(t) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const admin = store.addUser({ name: 'pc-admin', role: 'admin', passwordHash: 'fake-no-login' });
  const user = store.addUser({ name: 'pc-user', role: 'user', passwordHash: 'fake-no-login' });
  store.addUser({ name: 'pc-other', role: 'user', passwordHash: 'fake-no-login' });
  store.syncProxmox([vm], admin); store.saveSetting('proxmox', vault.seal({ endpoint: 'https://fixture.invalid', tokenId: 'fixture@pve!pc', secret: 'fake-no-real-secret' }), admin);
  const proxmox = { inventory: async () => [vm] }, requests = new HostRequests({ store, vault, proxmox });
  const create = (shown = true, machineUuid = pcUuid) => {
    const input = { requestKey: newToken(), hostname: 'Physical PC', machineUuid, version: '0.2.87' };
    return { input, created: requests.create(input, 'fixture', shown) };
  };
  t.after(() => store.close()); return { store, requests, proxmox, create, user, admin, authorize: () => admin };
}
const approve = (f, request, extra = {}) => f.requests.approveLocalPc(request.created.requestId, { name: '내 PC', acknowledgeCode: true, acknowledgeUnlinked: true, ...extra }, f.authorize);
const credentials = (f, request) => f.requests.status({ requestId: request.created.requestId, requestKey: request.input.requestKey }, 'fixture', true).credentials;

test('Local PC approval requires a displayed code and explicit comparison; pending claims receive no credentials', async t => {
  const f = fixture(t), request = f.create();
  assert.equal(credentials(f, request), undefined);
  await assert.rejects(approve(f, request, { acknowledgeCode: false }), fails('invalid_local_pc_approval'));
  const legacy = f.create(false, 'c1234567-89ab-cdef-8123-0123456789ab');
  await assert.rejects(approve(f, legacy), fails('host_request_code_required'));
  assert.equal(f.store.db.prepare('SELECT count(*) n FROM local_pc_approvals').get().n, 0);
});

test('A code-approved physical Host authenticates, can be assigned, starts with denied redirections and never leaks to another account', async t => {
  const f = fixture(t), request = f.create(), result = await approve(f, request), secret = credentials(f, request);
  assert.match(result.desktopId, /^pc-[0-9a-f-]{36}$/);
  assert.equal(f.store.authenticateHost(tokenHash(secret.hostToken)).desktop_id, result.desktopId);
  f.store.replaceVmAssignments('pc-user', [result.desktopId], f.admin);
  assert.equal(f.store.assignedDesktop(f.user, result.desktopId).kind, 'pc');
  assert.equal(f.store.listDesktops(f.user)[0].id, result.desktopId);
  assert.deepEqual(f.store.listDesktops(f.store.findUser('pc-other').id), []);
  assert.ok(Object.values(f.store.clipboardPolicy(f.user, result.desktopId)).every(v => v === false));
  assert.ok(Object.values(f.store.devicePolicy(f.user, result.desktopId)).every(v => v === false));
  assert.equal(f.store.setFavorite(f.user, result.desktopId, true), true);
  assert.equal(f.store.inventory().desktops.find(d => d.id === result.desktopId).localPcApproved, true);
});

test('Bare legacy/manual PC rows and mismatched or revoked Host bindings cannot authenticate or become assigned', async t => {
  const f = fixture(t), id = 'legacy-pc', token = newToken();
  f.store.addDesktop({ id, name: 'Legacy', kind: 'pc' }, f.admin);
  f.store.db.prepare('INSERT INTO hosts(id,desktop_id,token_hash,hostname,machine_uuid,version,created_at) VALUES(?,?,?,?,?,?,?)').run('fake-host', id, tokenHash(token), 'Legacy', pcUuid, '0.2.87', Date.now());
  assert.equal(f.store.authenticateHost(tokenHash(token)), undefined);
  assert.throws(() => f.store.replaceVmAssignments('pc-user', [id]), /unavailable/);
  const request = f.create(), result = await approve(f, request), secret = credentials(f, request);
  f.store.db.prepare('UPDATE hosts SET machine_uuid=? WHERE id=?').run(vmUuid, secret.hostId);
  assert.equal(f.store.authenticateHost(tokenHash(secret.hostToken)), undefined);
  assert.equal(f.store.inventory().desktops.find(d => d.id === result.desktopId).localPcApproved, false);
});

test('Known VM UUID conflicts prevent PC approval; an explicit unlinked choice never asserts whole-cluster absence', async t => {
  const f = fixture(t), matched = f.create(true, vmUuid);
  await assert.rejects(approve(f, matched), fails('host_request_matches_vm'));
  const fresh = f.create(); let fullQueries = 0; f.proxmox.inventory = async () => { fullQueries++; throw Error('no full discovery'); };
  assert.equal((await approve(f, fresh)).status, 'approved');
  assert.equal(fullQueries, 0); assert.equal(f.store.db.prepare('SELECT count(*) n FROM local_pc_approvals').get().n, 1);
});

test('Final transaction reauthorization commits neither a PC nor a Host after authority is withdrawn', async t => {
  const f = fixture(t), request = f.create(); let checks = 0;
  const authorize = () => { if (++checks >= 3) throw Object.assign(Error(), { code: 'administrator_required' }); return f.admin; };
  await assert.rejects(f.requests.approveLocalPc(request.created.requestId, { name: 'PC', acknowledgeCode: true, acknowledgeUnlinked: true }, authorize), fails('administrator_required'));
  assert.equal(f.store.db.prepare('SELECT count(*) n FROM local_pc_approvals').get().n, 0);
});

test('Replacing a PC requires selecting the exact previous PC and explicit replacement; assignments survive while the old Host loses access', async t => {
  const f = fixture(t), first = f.create(), initial = await approve(f, first), old = credentials(f, first);
  f.store.replaceVmAssignments('pc-user', [initial.desktopId], f.admin);
  const next = f.create();
  await assert.rejects(approve(f, next), fails('local_pc_already_registered'));
  const replacement = await approve(f, next, { desktopId: initial.desktopId, replaceExisting: true });
  assert.equal(replacement.desktopId, initial.desktopId);
  assert.equal(f.store.authenticateHost(tokenHash(old.hostToken)), undefined);
  assert.equal(f.store.authenticateHost(tokenHash(credentials(f, next).hostToken)).desktop_id, initial.desktopId);
  assert.equal(f.store.assignedDesktop(f.user, initial.desktopId).kind, 'pc');
});

test('Proxmox inventory refresh and an absent VM do not delete or reclassify an approved physical PC', async t => {
  const f = fixture(t), request = f.create(), result = await approve(f, request);
  f.store.replaceVmAssignments('pc-user', [result.desktopId], f.admin);
  f.store.syncProxmox([], f.admin);
  assert.equal(f.store.assignedDesktop(f.user, result.desktopId).kind, 'pc');
  assert.equal(f.store.inventory().desktops.find(d => d.id === result.desktopId).localPcApproved, true);
});

test('Physical PC display dispatch never obtains a Proxmox console proof or sends profile-track fields', async () => {
  const sent = [], session = { localPc: true, mode: 'stream', host: { ws: {}, capabilities: { streamProfileProtocol: 'console-profile-v1' } }, trackState: {} };
  const relay = { admitSessionStart: () => true, current: () => true, authorized: () => true, send: (_wire, command) => { sent.push(command); return true; } };
  const command = { type: 'session.start', sessionId: 'fixture' };
  await Relay.prototype.dispatchSessionStartGuarded.call(relay, session, command);
  assert.deepEqual(sent, [command]); assert.equal(session.hostDispatched, true);
  assert.equal(Object.hasOwn(sent[0], 'consoleProfile'), false);
  assert.equal(Object.hasOwn(sent[0], 'initialProfileTracks'), false);
});

test('Physical PC capabilities hide Proxmox profile negotiation without mutating the real Host state', () => {
  const caps = { streamProfileProtocol: 'console-profile-v1', streamProfileTracksProtocol: 'tracks-v1', streamProfileViewportProtocol: 'epoch-v1', dynamicResolution: true };
  const relay = { store: { devicePolicy: () => ({}) } };
  const view = Relay.prototype.visibleCapabilities.call(relay, caps, undefined, 'fixture', true);
  assert.deepEqual(view, { dynamicResolution: true }); assert.equal(caps.streamProfileProtocol, 'console-profile-v1');
  const host = { capabilitiesPending: false, capabilitiesAt: 100, capabilities: { ...caps, service: true, initialViewportProtocol: 'epoch-v1', resizeProtocol: 'epoch-v1', viewportProvisioning: true, virtualMonitorControl: true, interactiveSession: true, streaming: true, virtualDisplayReady: true, monitors: [{ id: '0', primary: true }] } };
  assert.equal(initialViewportSessionSupported({ localPc: true, host }, 100, 1000), true);
  host.capabilities.monitors[0].primary = false;
  assert.equal(initialViewportSessionSupported({ localPc: true, host }, 100, 1000), false);
});

test('Physical PC approval is cookie-admin, CSRF and same-origin protected; assignment list reaches only its authorized account', async t => {
  const f = fixture(t), origin = 'https://local-pc-fixture.invalid', adminToken = newToken(), clientToken = newToken();
  f.store.createSession(f.store.findUser('pc-admin'), tokenHash(adminToken), Date.now(), 3600000, 'admin');
  f.store.createSession(f.store.findUser('pc-user'), tokenHash(clientToken), Date.now(), 3600000, 'client');
  const server = await buildServer({ store: f.store, vault: f.requests.vault, proxmox: f.proxmox, publicOrigin: origin, logger: { error() {} } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve)); t.after(() => new Promise(resolve => server.close(resolve)));
  const call = async (path, { method = 'POST', data, admin = false, csrf = true, headers = {} } = {}) => {
    data = captureAssignmentInput(f.store, path, data);
    const response = await fetch('http://127.0.0.1:' + server.address().port + '/api/v1' + path, { method, headers: { Origin: origin, ...(data ? { 'Content-Type': 'application/json' } : {}), ...(admin ? { Cookie: '__Secure-multitap-admin=' + adminToken, ...(csrf ? { 'X-CSRF-Token': f.requests.vault.csrf(adminToken) } : {}) } : {}), ...headers }, body: data ? JSON.stringify(data) : undefined });
    return { status: response.status, body: await response.json() };
  };
  const input = { requestKey: newToken(), hostname: 'Physical PC', machineUuid: pcUuid, version: '0.2.87' };
  const created = await call('/hosts/requests', { data: input, headers: { 'X-Multitap-Host-Request-Code': 'v1' } }); assert.equal(created.status, 201);
  const path = '/admin/hosts/requests/' + created.body.requestId + '/approve-pc', data = { name: '내 PC', acknowledgeCode: true, acknowledgeUnlinked: true };
  assert.equal((await call(path, { data })).status, 401);
  assert.equal((await call(path, { data, headers: { Authorization: 'Bearer ' + clientToken } })).status, 401);
  assert.equal((await call(path, { data, admin: true, csrf: false })).status, 403);
  assert.equal((await call(path, { data, admin: true, headers: { Origin: 'https://wrong.invalid' } })).status, 403);
  const approved = await call(path, { data, admin: true }); assert.equal(approved.status, 200); assert.equal(Object.hasOwn(approved.body, 'credentials'), false);
  const id = approved.body.desktopId;
  assert.equal((await call('/admin/assignments/batch', { method: 'PUT', admin: true, data: { username: 'pc-user', desktopIds: [id] } })).status, 200);
  const listing = await call('/desktops', { method: 'GET', headers: { Authorization: 'Bearer ' + clientToken } });
  assert.equal(listing.status, 200); assert.equal(listing.body.desktops.length, 1); assert.equal(listing.body.desktops[0].kind, 'pc'); assert.equal(listing.body.desktops[0].localPcApproved, true);
  assert.equal(Object.hasOwn(listing.body.desktops[0], 'powerState'), false);
  assert.equal((await call('/admin/desktops/' + id, { method: 'PUT', admin: true, data: { name: '이름 변경' } })).status, 200);
  assert.equal(f.store.assignedDesktop(f.user, id).name, '이름 변경');
  assert.equal((await call('/admin/desktops/' + id + '/power', { admin: true, data: { action: 'reboot' } })).status, 404);
  let permissionReads = 0;
  f.proxmox.call = async (_config, path) => { permissionReads++; assert.equal(path, '/access/permissions?path=%2Fvms'); return { '/vms': { 'VM.Console': 0 } }; };
  const permissionPath = '/admin/proxmox/console-permission';
  assert.equal((await call(permissionPath, { admin: true, csrf: false, data: {} })).status, 403);
  assert.equal((await call(permissionPath, { admin: true, data: { vmid: 149 } })).status, 400);
  assert.equal(permissionReads, 0);
  const checked = await call(permissionPath, { admin: true, data: {} }); assert.equal(checked.status, 200); assert.equal(checked.body.granted, true);
  f.proxmox.call = async () => { f.store.logout(tokenHash(adminToken)); return { '/vms': { 'VM.Console': 1 } }; };
  assert.equal((await call(permissionPath, { admin: true, data: {} })).status, 401);
});
