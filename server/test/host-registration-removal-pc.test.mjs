import test from 'node:test';
import assert from 'node:assert/strict';
import { once } from 'node:events';
import { setImmediate as nextTurn } from 'node:timers/promises';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';

const origin = 'https://pc-removal.fixture.invalid';
const pcUuid = 'a1234567-89ab-4123-8123-0123456789ab';
const vm = { vmid: 884, node: 'fixture', name: 'Unrelated synthetic VM', status: 'running', machineUuid: 'b1234567-89ab-4123-8123-0123456789ab' };

async function fixture(t) {
  const store = new Store(':memory:'), vault = new Vault(Buffer.alloc(32, 41));
  const admin = store.addUser({ name: 'pc-removal-admin', role: 'admin', passwordHash: 'synthetic-unused' });
  const assigned = store.addUser({ name: 'pc-removal-user', passwordHash: 'synthetic-unused' });
  store.syncProxmox([vm], admin);
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve.fixture.invalid', tokenId: 'fixture@pve!pc-removal', secret: 'synthetic-unused' }), admin);
  const token = newToken();
  store.createSession(store.findUser('pc-removal-admin'), tokenHash(token), Date.now(), 60000, 'admin');
  let inventoryCalls = 0;
  const errors = [], sockets = [];
  const server = await buildServer({ store, vault, publicOrigin: origin,
    proxmox: { inventory: async () => { ++inventoryCalls; throw Error('unexpected_full_inventory'); } },
    relayOptions: { authorizationMs: 60000, heartbeatMs: 60000 }, logger: { error: value => errors.push(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const base = 'http://127.0.0.1:' + server.address().port;
  t.after(async () => { for (const ws of sockets) ws.terminate(); await nextTurn(); await new Promise(resolve => server.close(resolve)); store.close(); assert.deepEqual(errors, []); });
  async function call(path, data, { method = 'POST', adminCall = false, shown = true } = {}) {
    const response = await fetch(base + '/api/v1/' + path, { method,
      headers: { Origin: origin, 'Content-Type': 'application/json', ...(shown ? { 'X-Multitap-Host-Request-Code': 'v1' } : {}),
        ...(adminCall ? { Cookie: '__Secure-multitap-admin=' + token, 'X-CSRF-Token': vault.csrf(token) } : {}) },
      body: data === undefined ? undefined : JSON.stringify(data) });
    return { status: response.status, body: await response.json() };
  }
  async function request({ shown = true } = {}) {
    const input = { requestKey: newToken(), hostname: 'Synthetic physical Host', machineUuid: pcUuid, version: '1.0.0' };
    const result = await call('hosts/requests', input, { shown });
    assert.equal(result.status, 201, result.body.error?.code);
    return { input, id: result.body.requestId };
  }
  const status = request => call('hosts/requests/status', { requestId: request.id, requestKey: request.input.requestKey });
  const approve = (request, extra = {}) => call('admin/hosts/requests/' + request.id + '/approve-pc', { name: 'Synthetic physical PC', acknowledgeCode: true, acknowledgeUnlinked: true, ...extra }, { adminCall: true });
  const remove = (desktopId, hostId) => call('admin/desktops/' + desktopId + '/host', { expectedHostId: hostId, confirm: true }, { method: 'DELETE', adminCall: true });
  const stableData = () => JSON.stringify(['desktops', 'assignments', 'local_pc_approvals'].map(table => store.db.prepare('SELECT * FROM ' + table + ' ORDER BY rowid').all()));
  // What a removal must leave alone: every other desktop, its approval and assignments, and the accounts.
  const keptData = id => JSON.stringify([store.db.prepare('SELECT * FROM desktops WHERE id<>? ORDER BY rowid').all(id), store.db.prepare('SELECT * FROM local_pc_approvals WHERE desktop_id<>? ORDER BY rowid').all(id), store.db.prepare('SELECT * FROM assignments WHERE desktop_id<>? ORDER BY rowid').all(id), store.db.prepare('SELECT id,disabled FROM users ORDER BY id').all()]);
  // What a removal must take away: everything bound to the PC (so adding it again starts clean).
  const leftOver = id => ['desktops WHERE id', 'hosts WHERE desktop_id', 'local_pc_approvals WHERE desktop_id', 'pc_wake WHERE desktop_id', 'assignments WHERE desktop_id', 'host_enrollments WHERE desktop_id'].map(from => store.db.prepare('SELECT count(*) AS n FROM ' + from + '=?').get(id).n);
  async function initial() {
    const req = await request(), approved = await approve(req);
    assert.equal(approved.status, 200, approved.body.error?.code);
    const delivered = await status(req), credentials = delivered.body.credentials;
    assert.equal(delivered.body.status, 'approved'); assert.ok(credentials);
    const id = approved.body.desktopId;
    store.assign('pc-removal-user', id, admin); store.assign('pc-removal-user', 'pve-884', admin);
    store.updateAssignmentPolicy('pc-removal-user', id, {
      clipboardPolicy: { textToRemote: true, textToLocal: false, filesToRemote: true, filesToLocal: false },
      devicePolicy: { drives: true, printers: false, usbDevices: true, smartCards: false, serialPorts: false },
    }, admin);
    return { request: req, id, credentials };
  }
  async function openHost(credentials) {
    const ws = new WebSocket(base.replace('http:', 'ws:') + '/api/v1/hosts/channel', { headers: { Authorization: 'Host ' + credentials.hostToken, 'X-Multitap-Host-Protocol': 'capabilities-v1' } });
    sockets.push(ws); await once(ws, 'open'); await nextTurn(); return ws;
  }
  async function deniedHost(credentials) {
    return new Promise(resolve => {
      const ws = new WebSocket(base.replace('http:', 'ws:') + '/api/v1/hosts/channel', { headers: { Authorization: 'Host ' + credentials.hostToken } });
      sockets.push(ws); ws.on('error', () => {});
      ws.on('unexpected-response', (req, res) => { const code = res.statusCode; res.resume(); req.destroy(); resolve(code); });
      ws.on('open', () => { ws.terminate(); resolve(101); });
    });
  }
  return { store, admin, assigned, request, status, approve, remove, initial, openHost, deniedHost, stableData, keptData, leftOver, inventoryCalls: () => inventoryCalls };
}

// A removed Host takes its assignments and their permissions along, for a local PC as for a VM.
test('HTTP local PC deletion closes its idle Host, revokes old delivery and removes the PC with everything bound to it', async t => {
  const f = await fixture(t), old = await f.initial(), ws = await f.openHost(old.credentials);
  f.store.db.prepare('INSERT INTO pc_wake(desktop_id,host_id,adapters,reported_at) VALUES(?,?,?,?)').run(old.id, old.credentials.hostId, '[]', Date.now());
  const before = f.keptData(old.id), closed = once(ws, 'close');
  const result = await f.remove(old.id, old.credentials.hostId);
  assert.equal(result.status, 200); assert.deepEqual(result.body, { ok: true, desktopId: old.id, hostId: old.credentials.hostId, removedAssignments: 1 }); await closed;
  assert.equal(f.keptData(old.id), before);
  assert.deepEqual(f.leftOver(old.id), [0, 0, 0, 0, 0, 0], 'nothing of the PC is left to get in the way of adding it again');
  assert.deepEqual(f.store.userAssignments(f.assigned).map(value => value.desktopId), ['pve-884'], 'only the removed PC lost its assignment');
  assert.equal(f.store.authenticateHost(tokenHash(old.credentials.hostToken)), undefined);
  assert.equal(f.store.registeredHostIds().has(old.id), false);
  const delivered = await f.status(old.request);
  assert.equal(delivered.body.status, 'rejected'); assert.equal(delivered.body.credentials, undefined);
  const stored = f.store.db.prepare('SELECT credentials_sealed FROM host_requests WHERE id=?').get(old.request.id);
  assert.equal(stored.credentials_sealed, null); assert.equal(await f.deniedHost(old.credentials), 401);
  assert.equal(f.inventoryCalls(), 0);
});

test('a deleted PC comes back as a new PC: fresh shown code and unlinked confirmation are still needed, the old target cannot be named', async t => {
  const f = await fixture(t), old = await f.initial(); assert.equal((await f.remove(old.id, old.credentials.hostId)).status, 200);
  const remainingGrants = JSON.stringify(f.store.userAssignments(f.assigned)); assert.deepEqual(JSON.parse(remainingGrants).map(value => value.desktopId), ['pve-884']);
  const before = f.stableData(), fresh = await f.request();
  assert.equal((await f.status(fresh)).body.credentials, undefined);
  for (const acknowledgeUnlinked of [undefined, false, 1, 'true']) {
    const result = await f.approve(fresh, { acknowledgeUnlinked });
    assert.equal(result.status, 400); assert.equal(result.body.error.code, 'local_pc_unlinked_confirmation_required'); assert.equal(f.stableData(), before);
  }
  // Nothing is left to replace: naming the deleted PC or asking for a replacement is refused.
  for (const extra of [{ replaceExisting: true }, { desktopId: old.id }, { desktopId: old.id, replaceExisting: true }, { desktopId: 'pc-c1234567-89ab-4123-8123-0123456789ab', replaceExisting: true }]) {
    const result = await f.approve(fresh, extra); assert.equal(result.status, 409); assert.equal(f.stableData(), before);
  }
  const approved = await f.approve(fresh); assert.equal(approved.status, 200, approved.body.error?.code); assert.equal(approved.body.desktopId, 'pc-' + fresh.id);
  const now = (await f.status(fresh)).body.credentials; assert.ok(now); assert.notEqual(now.hostId, old.credentials.hostId); assert.notEqual(now.hostToken, old.credentials.hostToken);
  assert.equal(f.store.authenticateHost(tokenHash(now.hostToken)).desktop_id, 'pc-' + fresh.id); assert.equal(f.store.authenticateHost(tokenHash(old.credentials.hostToken)), undefined);
  assert.equal((await f.status(old.request)).body.credentials, undefined);
  assert.equal(f.store.db.prepare('SELECT host_id FROM local_pc_approvals WHERE machine_uuid=?').get(pcUuid).host_id, now.hostId);
  // The PC registered again starts with nobody assigned; a new assignment starts denied.
  assert.equal(JSON.stringify(f.store.userAssignments(f.assigned)), remainingGrants);
  f.store.assign('pc-removal-user', 'pc-' + fresh.id, f.admin);
  assert.deepEqual(f.store.userAssignments(f.assigned).find(value => value.desktopId === 'pc-' + fresh.id).clipboardPolicy, { textToRemote: false, textToLocal: false, filesToRemote: false, filesToLocal: false });
  const preserved = JSON.stringify(f.store.userAssignments(f.assigned));
  const staleDelete = await f.remove(old.id, old.credentials.hostId); assert.equal(staleDelete.status, 409); assert.equal(staleDelete.body.error.code, 'host_registration_changed');
  assert.equal(JSON.stringify(f.store.userAssignments(f.assigned)), preserved); assert.equal(f.store.authenticateHost(tokenHash(now.hostToken)).id, now.hostId);
  assert.equal(f.inventoryCalls(), 0);
});

test('replacing the Host of a registered PC (a reinstall, not removed first) keeps its assignments and permissions', async t => {
  const f = await fixture(t), old = await f.initial(), before = JSON.stringify(f.store.db.prepare('SELECT * FROM assignments ORDER BY rowid').all());
  const fresh = await f.request(), approved = await f.approve(fresh, { desktopId: old.id, replaceExisting: true });
  assert.equal(approved.status, 200, approved.body.error?.code); assert.equal(approved.body.desktopId, old.id);
  assert.equal(JSON.stringify(f.store.db.prepare('SELECT * FROM assignments ORDER BY rowid').all()), before);
  assert.deepEqual(f.store.userAssignments(f.assigned).find(value => value.desktopId === old.id).clipboardPolicy, { textToRemote: true, textToLocal: false, filesToRemote: true, filesToLocal: false });
  assert.equal(f.store.db.prepare("SELECT count(*) AS n FROM audit_events WHERE event='desktop_assignments_removed'").get().n, 0);
});

test('unlinked acknowledgement cannot replace the deleted PC through a request that never showed its verification code', async t => {
  const f = await fixture(t), old = await f.initial(); assert.equal((await f.remove(old.id, old.credentials.hostId)).status, 200);
  const fresh = await f.request({ shown: false }), before = f.stableData();
  const result = await f.approve(fresh, { desktopId: old.id, replaceExisting: true });
  assert.equal(result.status, 409); assert.equal(result.body.error.code, 'host_request_code_required'); assert.equal(f.stableData(), before);
  assert.equal(f.store.authenticateHost(tokenHash(old.credentials.hostToken)), undefined); assert.equal(f.inventoryCalls(), 0);
});

test('a fresh known VM UUID conflict blocks local PC tombstone reapproval without changing grants or delivering credentials', async t => {
  const f = await fixture(t), old = await f.initial(); assert.equal((await f.remove(old.id, old.credentials.hostId)).status, 200);
  const fresh = await f.request();
  f.store.syncProxmox([vm, { vmid: 885, node: 'fixture', name: 'Now-known synthetic VM', status: 'stopped', machineUuid: pcUuid }], 'synthetic');
  const before = f.stableData(), result = await f.approve(fresh, { desktopId: old.id, replaceExisting: true });
  assert.equal(result.status, 409); assert.equal(result.body.error.code, 'host_request_matches_vm'); assert.equal(f.stableData(), before);
  assert.equal((await f.status(fresh)).body.credentials, undefined); assert.equal(f.store.authenticateHost(tokenHash(old.credentials.hostToken)), undefined);
  assert.equal(f.inventoryCalls(), 0);
});
