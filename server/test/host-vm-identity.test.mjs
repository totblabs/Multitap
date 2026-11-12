import { test } from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { randomBytes } from 'node:crypto';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname, resolve } from 'node:path';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { Relay } from '../src/relay.mjs';
import { HostRequests } from '../src/host-requests.mjs';
import { newToken, tokenHash } from '../src/security.mjs';

const uuidA = '12345678-1234-1234-1234-123456789abc';
const uuidB = '87654321-4321-4321-4321-cba987654321';
const uuidC = '12345678-1234-1234-1234-000000000711';
const vm = (machineUuid = uuidA, vmid = 710) => ({ vmid, node: 'fixture', name: `VM ${vmid}`, status: 'running', machineUuid, tags: ['office'], pool: 'staff' });
const sync = (store, machineUuid, now) => store.syncProxmox([vm(machineUuid), vm(uuidC, 711)], 'fixture', now);
const enroll = (store, uuid = uuidA, id = 'pve-710') => {
  const codeHash = tokenHash(newToken()), hash = tokenHash(newToken());
  store.issueEnrollment(id, uuid, codeHash, 'fixture');
  const result = store.consumeEnrollment(codeHash, uuid, { hostname: 'Fixture Host', version: '0.2.79', tokenHash: hash });
  return { ...result, hash };
};
function fixture(t) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const admin = store.addUser({ name: 'admin', role: 'admin', passwordHash: 'unused-fixture-hash' });
  store.addUser({ name: 'alice', passwordHash: 'unused-fixture-hash' });
  sync(store, uuidA); store.assign('alice', 'pve-710');
  const user = store.findUser('alice'), userHash = tokenHash(newToken());
  store.createSession(user, userHash, Date.now(), 3600000);
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve-fixture.invalid', tokenId: 'fixture@pve!identity', secret: 'unused-fixture-secret' }), 'fixture');
  let identity = uuidA;
  const proxmox = { machineIdentity: async () => ({ machineUuid: identity }) };
  const f = { store, vault, proxmox, admin, user, userHash, setIdentity: value => { identity = value; } };
  t.after(() => { try { f.beforeClose?.(); } finally { store.close(); } });
  return f;
}
const desktop = store => store.db.prepare('SELECT * FROM desktops WHERE id=?').get('pve-710');
const hostRow = (store, id) => store.db.prepare('SELECT * FROM hosts WHERE id=?').get(id);
const errorCode = code => error => error.code === code;

// Another machine
// under the same VM number must not inherit the users of the one before it.
test('A valid same-VMID UUID change revokes only the old Host, drops its assignments and preserves presentation', t => {
  const f = fixture(t), old = enroll(f.store), unrelated = enroll(f.store, uuidC, 'pve-711');
  f.store.assign('alice', 'pve-711'); f.store.assign('admin', 'pve-710');
  f.store.updateDesktop('pve-710', { name: 'My desktop', description: 'Keep this note', os: 'Custom Windows' });
  f.store.db.prepare('UPDATE assignments SET favorite=1,last_used_at=123,clipboard_permissions=1,rdp_redirections=1 WHERE desktop_id=?').run('pve-710');
  const others = f.store.db.prepare("SELECT * FROM assignments WHERE desktop_id<>'pve-710'").all(), users = f.store.db.prepare('SELECT id,disabled FROM users ORDER BY id').all();
  assert.ok(f.store.authenticateHost(old.hash)); assert.ok(f.store.hostConsoleBinding(old.hash));
  sync(f.store, uuidB);
  assert.equal(f.store.authenticateHost(old.hash), undefined);
  assert.equal(f.store.hostConsoleBinding(old.hash), undefined);
  assert.equal(hostRow(f.store, old.hostId).revoked, 1);
  assert.equal(f.store.authenticateHost(unrelated.hash).id, unrelated.hostId);
  assert.deepEqual(f.store.db.prepare("SELECT * FROM assignments WHERE desktop_id='pve-710'").all(), [], 'nobody stays assigned to the replaced machine');
  assert.deepEqual(f.store.db.prepare("SELECT * FROM assignments WHERE desktop_id<>'pve-710'").all(), others, 'other desktops keep their assignments');
  assert.deepEqual(f.store.db.prepare('SELECT id,disabled FROM users ORDER BY id').all(), users, 'accounts are untouched');
  assert.deepEqual(f.store.db.prepare("SELECT event,subject FROM audit_events WHERE event IN ('host_identity_changed','desktop_assignments_removed') ORDER BY id").all().map(row => ({ ...row })),
    [{ event: 'host_identity_changed', subject: 'pve-710' }, { event: 'desktop_assignments_removed', subject: 'pve-710:2' }]);
  // The same identity read again removes nothing more and records nothing more.
  f.store.assign('alice', 'pve-710'); sync(f.store, uuidB);
  assert.equal(f.store.db.prepare("SELECT count(*) AS n FROM assignments WHERE desktop_id='pve-710'").get().n, 1);
  assert.equal(f.store.db.prepare("SELECT count(*) AS n FROM audit_events WHERE event='desktop_assignments_removed'").get().n, 1);
  const row = desktop(f.store);
  assert.equal(row.name, 'My desktop'); assert.equal(row.description, 'Keep this note'); assert.equal(row.os, 'Custom Windows');
  assert.equal(row.pve_machine_uuid, uuidB); assert.equal(row.pve_available, 1);
  assert.deepEqual(f.store.listDesktops(f.user.id).map(value => value.id), ['pve-711'], 'revoked Host leaves the client projection');
  assert.deepEqual(JSON.parse(row.pve_tags), ['office']);
  assert.equal(row.pve_pool, 'staff');
});

test('Unknown/failed UUID enrichment preserves the last binding without claiming a fresh identity check', t => {
  const f = fixture(t), old = enroll(f.store);
  for (const value of [null, undefined, '00000000-0000-0000-0000-000000000000', 'FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF']) {
    // Omitted enrichment is distinct from a valid authoritative replacement.
    const item = vm(); if (value === undefined) delete item.machineUuid; else item.machineUuid = value;
    f.store.syncProxmox([item, vm(uuidC, 711)], 'fixture');
    assert.equal(f.store.authenticateHost(old.hash).id, old.hostId);
    assert.equal(desktop(f.store).pve_machine_uuid, uuidA);
    assert.equal(desktop(f.store).pve_machine_checked_at, null);
  }
  sync(f.store, uuidA.toUpperCase(), 123456);
  assert.equal(f.store.authenticateHost(old.hash).id, old.hostId);
  assert.equal(desktop(f.store).pve_machine_checked_at, 123456);
  const before = desktop(f.store);
  assert.throws(() => f.store.syncProxmox([vm(uuidB), vm('invalid', 711)], 'fixture'), /invalid_proxmox_inventory/);
  assert.deepEqual(desktop(f.store), before);
  assert.equal(hostRow(f.store, old.hostId).revoked, 0);
});

test('Returning to an old UUID or losing enrichment never revives its revoked token or old enrollment code', t => {
  const f = fixture(t), old = enroll(f.store), oldCode = tokenHash(newToken());
  f.store.issueEnrollment('pve-710', uuidA, oldCode, 'fixture');
  sync(f.store, uuidB); sync(f.store, null); sync(f.store, uuidA);
  assert.equal(f.store.authenticateHost(old.hash), undefined);
  assert.equal(f.store.pendingEnrollment(oldCode), undefined);
  assert.throws(() => f.store.consumeEnrollment(oldCode, uuidA, { hostname: 'Old', version: '0.2.79', tokenHash: tokenHash(newToken()) }), /invalid_enrollment/);
  const replacement = enroll(f.store);
  assert.equal(f.store.authenticateHost(replacement.hash).id, replacement.hostId);
  assert.equal(f.store.authenticateHost(old.hash), undefined);
});

test('A previously unknown UUID can be bound without trusting the existing Host claim', t => {
  const f = fixture(t), old = enroll(f.store);
  f.store.db.prepare('UPDATE desktops SET pve_machine_uuid=NULL,pve_machine_checked_at=NULL WHERE id=?').run('pve-710');
  assert.ok(f.store.authenticateHost(old.hash));
  sync(f.store, uuidB);
  assert.equal(f.store.authenticateHost(old.hash), undefined);
  assert.equal(hostRow(f.store, old.hostId).revoked, 1);
});

test('Startup repairs an already stored mismatch without a schema bump or fabricated fresh check', t => {
  const directory = mkdtempSync(join(tmpdir(), 'multitap-host-identity-'));
  assert.equal(dirname(resolve(directory)), resolve(tmpdir()));
  const path = join(directory, 'fixture.db'); let store = new Store(path);
  t.after(() => { try { store.close(); } finally { rmSync(directory, { recursive: true, force: true }); } });
  sync(store, uuidA); const old = enroll(store), unrelated = enroll(store, uuidC, 'pve-711');
  // Represents the released broker's A Host + cached B inventory state.
  store.db.prepare('UPDATE desktops SET pve_machine_uuid=?,pve_machine_checked_at=123 WHERE id=?').run(uuidB, 'pve-710');
  store.db.prepare('UPDATE desktops SET pve_machine_uuid=?,pve_machine_checked_at=123 WHERE id=?').run('00000000-0000-0000-0000-000000000000', 'pve-711');
  store.close(); store = new Store(path);
  assert.equal(store.authenticateHost(old.hash), undefined); assert.equal(hostRow(store, old.hostId).revoked, 1);
  assert.equal(desktop(store).pve_machine_checked_at, 123);
  assert.equal(store.authenticateHost(unrelated.hash).id, unrelated.hostId);
  assert.equal(store.db.prepare('SELECT pve_machine_uuid FROM desktops WHERE id=?').get('pve-711').pve_machine_uuid, null);
  assert.equal(store.db.prepare('PRAGMA user_version').get().user_version, 19);
  sync(store, uuidA); assert.equal(store.authenticateHost(old.hash), undefined);
});

test('Approved old-request credentials are withdrawn; a deliberate fresh B replacement remains possible', async t => {
  const f = fixture(t), requests = new HostRequests(f);
  const inputA = { requestKey: newToken(), machineUuid: uuidA, hostname: 'Host A', version: '0.2.79' };
  const requestA = requests.create(inputA, 'address', true), claimA = { requestId: requestA.requestId, requestKey: inputA.requestKey };
  await requests.approve(requestA.requestId, { desktopId: 'pve-710', acknowledgeCode: true }, () => f.admin);
  const old = requests.status(claimA).credentials;
  sync(f.store, uuidB); f.setIdentity(uuidB);
  assert.equal(requests.status(claimA).status, 'rejected');
  assert.equal(f.store.db.prepare('SELECT credentials_sealed FROM host_requests WHERE id=?').get(requestA.requestId).credentials_sealed, null);
  const inputB = { ...inputA, requestKey: newToken(), machineUuid: uuidB }, requestB = requests.create(inputB, 'address', true);
  await assert.rejects(requests.approve(requestB.requestId, { desktopId: 'pve-710', acknowledgeCode: true }, () => f.admin), errorCode('host_already_registered'));
  await requests.approve(requestB.requestId, { desktopId: 'pve-710', replaceExisting: true, acknowledgeCode: true }, () => f.admin);
  const replacement = requests.status({ requestId: requestB.requestId, requestKey: inputB.requestKey }).credentials;
  assert.equal(f.store.authenticateHost(tokenHash(replacement.hostToken)).id, replacement.hostId);
  assert.equal(f.store.authenticateHost(tokenHash(old.hostToken)), undefined);
  // Machine B does not inherit machine A's user.
  assert.equal(f.store.assignedDesktop(f.user.id, 'pve-710'), undefined);
  assert.equal(f.store.db.prepare("SELECT count(*) AS n FROM assignments WHERE desktop_id='pve-710'").get().n, 0);
});

// A deleted desktop added again starts clean, with no
// "replace the existing Host" question left over from the deleted one.
test('A deleted VM is approved again as a plain new Host, without a replacement and without its old users', async t => {
  const f = fixture(t), requests = new HostRequests(f);
  const first = { requestKey: newToken(), machineUuid: uuidA, hostname: 'Host A', version: '0.2.79' }, created = requests.create(first, 'address', true);
  await requests.approve(created.requestId, { desktopId: 'pve-710', acknowledgeCode: true }, () => f.admin);
  const old = requests.status({ requestId: created.requestId, requestKey: first.requestKey }).credentials;
  f.store.removeHostRegistration('pve-710', old.hostId, f.admin, () => {});
  assert.equal(f.store.db.prepare("SELECT count(*) AS n FROM hosts WHERE desktop_id='pve-710'").get().n, 0);
  const again = { ...first, requestKey: newToken() }, request = requests.create(again, 'address', true);
  const listed = requests.pending().requests.find(row => row.requestId === request.requestId);
  assert.equal(listed.candidates[0].hasExistingHost, false, 'the console does not ask to replace a deleted Host');
  await requests.approve(request.requestId, { desktopId: 'pve-710', acknowledgeCode: true }, () => f.admin);
  const now = requests.status({ requestId: request.requestId, requestKey: again.requestKey }).credentials;
  assert.equal(f.store.authenticateHost(tokenHash(now.hostToken)).id, now.hostId);
  assert.equal(f.store.authenticateHost(tokenHash(old.hostToken)), undefined);
  assert.equal(f.store.db.prepare("SELECT count(*) AS n FROM assignments WHERE desktop_id='pve-710'").get().n, 0);
});

test('Replacing the Host of the same machine (a reinstall) keeps its assignments and their permissions', async t => {
  const f = fixture(t), requests = new HostRequests(f);
  const first = { requestKey: newToken(), machineUuid: uuidA, hostname: 'Host A', version: '0.2.79' }, created = requests.create(first, 'address', true);
  await requests.approve(created.requestId, { desktopId: 'pve-710', acknowledgeCode: true }, () => f.admin);
  f.store.db.prepare('UPDATE assignments SET favorite=1,last_used_at=123,clipboard_permissions=5,rdp_redirections=3,sound_card_add=1 WHERE desktop_id=?').run('pve-710');
  const before = f.store.db.prepare('SELECT * FROM assignments ORDER BY user_id,desktop_id').all();
  const again = { ...first, requestKey: newToken(), hostname: 'Host A reinstalled', version: '1.0.44' }, reinstall = requests.create(again, 'address', true);
  await requests.approve(reinstall.requestId, { desktopId: 'pve-710', replaceExisting: true, acknowledgeCode: true }, () => f.admin);
  const credentials = requests.status({ requestId: reinstall.requestId, requestKey: again.requestKey }).credentials;
  assert.equal(f.store.authenticateHost(tokenHash(credentials.hostToken)).id, credentials.hostId);
  assert.deepEqual(f.store.db.prepare('SELECT * FROM assignments ORDER BY user_id,desktop_id').all(), before);
  assert.equal(f.store.assignedDesktop(f.user.id, 'pve-710').id, 'pve-710');
  assert.equal(f.store.db.prepare("SELECT count(*) AS n FROM audit_events WHERE event='desktop_assignments_removed'").get().n, 0);
});

test('A replacement approved for another machine drops the assignments even when the old Host was never revoked', async t => {
  // The stored UUID is unknown (a row from an older release), so only the old
  // Host row says which machine the assignments belonged to.
  const f = fixture(t), old = enroll(f.store), requests = new HostRequests(f);
  f.store.db.prepare('UPDATE desktops SET pve_machine_uuid=NULL,pve_machine_checked_at=NULL WHERE id=?').run('pve-710');
  assert.ok(f.store.authenticateHost(old.hash)); f.setIdentity(uuidB);
  const input = { requestKey: newToken(), machineUuid: uuidB, hostname: 'Host B', version: '1.0.44' }, created = requests.create(input, 'address', true);
  await requests.approve(created.requestId, { node: 'fixture', vmid: 710, acknowledgeCode: true, replaceExisting: true }, () => f.admin);
  assert.equal(f.store.db.prepare("SELECT count(*) AS n FROM assignments WHERE desktop_id='pve-710'").get().n, 0);
  assert.deepEqual({ ...f.store.db.prepare("SELECT actor,subject FROM audit_events WHERE event='desktop_assignments_removed'").get() }, { actor: f.admin, subject: 'pve-710:1' });
  assert.equal(f.store.authenticateHost(old.hash), undefined);
});

test('An approval paused on UUID A cannot overwrite a newer authoritative UUID B', async t => {
  const f = fixture(t); enroll(f.store);
  let finish, begin;
  const started = new Promise(resolve => { begin = resolve; });
  f.proxmox.machineIdentity = () => { begin(); return new Promise(resolve => { finish = resolve; }); };
  const requests = new HostRequests(f), input = { requestKey: newToken(), machineUuid: uuidA, hostname: 'Host A', version: '0.2.79' }, request = requests.create(input, 'address', true);
  const pending = requests.approve(request.requestId, { desktopId: 'pve-710', replaceExisting: true, acknowledgeCode: true }, () => f.admin);
  await started; sync(f.store, uuidB); finish({ machineUuid: uuidA });
  await assert.rejects(pending, errorCode('desktop_changed'));
  assert.equal(desktop(f.store).pve_machine_uuid, uuidB);
  assert.equal(requests.status({ requestId: request.requestId, requestKey: input.requestKey }).status, 'pending');
});

class Socket extends EventEmitter {
  readyState = 1; bufferedAmount = 0; sent = [];
  send(payload, { binary }, done) { this.sent.push(binary ? Buffer.from(payload) : JSON.parse(payload)); done?.(); }
  close(code, reason) { if (this.readyState === 3) return; this.readyState = 3; this.closeCode = code; this.closeReason = reason; this.emit('close'); }
  terminate() { this.close(1006, 'terminated'); }
  json(value) { this.emit('message', Buffer.from(JSON.stringify(value)), false); }
  ping() {}
}
function relayFixture(t) {
  const f = fixture(t), old = enroll(f.store);
  const relay = new Relay({ ...f, publicOrigin: 'https://relay-fixture.invalid', authorizationMs: 3600000, heartbeatMs: 3600000 });
  f.beforeClose = () => relay.close();
  const hostSocket = new Socket(); relay.attachHost(hostSocket, f.store.authenticateHost(old.hash), old.hash);
  const ticket = relay.connect(f.user, f.userHash, 'pve-710'), client = new Socket();
  relay.attachClient(client, relay.sessions.get(ticket.sessionId));
  hostSocket.json({ type: 'stream.info', codec: 'h264', width: 1280, height: 720, sourceWidth: 1920, sourceHeight: 1080, fpsTarget: 60, audio: { sampleRate: 48000, channels: 2, format: 's16le', available: false } });
  return { ...f, old, relay, hostSocket, client, session: relay.sessions.get(ticket.sessionId) };
}

test('An already open Host cannot send media, heartbeat or input after the UUID change, before any sweep', t => {
  const f = relayFixture(t), video = Buffer.alloc(18); video[0] = 1; video[1] = 1;
  f.hostSocket.emit('message', video, true);
  assert.equal(f.client.sent.filter(Buffer.isBuffer).length, 1);
  sync(f.store, uuidB);
  assert.equal(f.relay.hostConnected('pve-710'), false); assert.equal(f.relay.hostReady('pve-710'), false);
  assert.deepEqual(f.relay.decorate(f.store.listDesktops(f.user.id), f.user.id), [], 'revoked Host is not an offline client card');
  // The user is no longer assigned to the VM number's new machine at all.
  assert.throws(() => f.relay.connect(f.user, f.userHash, 'pve-710'), errorCode('desktop_not_found'));
  const inputs = f.hostSocket.sent.filter(value => value.type === 'input').length;
  f.client.json({ type: 'input', event: 'move', x: 0.2, y: 0.3 });
  assert.equal(f.hostSocket.sent.filter(value => value.type === 'input').length, inputs);
  f.hostSocket.emit('message', video, true);
  assert.equal(f.client.sent.filter(Buffer.isBuffer).length, 1);
  assert.equal(f.client.sent.find(value => value.type === 'session.ended').reason, 'host_revoked');
  assert.equal(f.relay.sessions.size, 0); assert.equal(f.relay.hosts.size, 0);
  f.hostSocket.json({ type: 'host.heartbeat', version: '9.9.9' });
  assert.equal(hostRow(f.store, f.old.hostId).version, '0.2.79');
});

test('A silent old Host is retired by revalidation; fresh enrollment and late old callbacks cannot affect B', t => {
  const f = relayFixture(t), oldHost = f.session.host;
  sync(f.store, uuidB); f.relay.revalidate();
  assert.equal(f.client.sent.find(value => value.type === 'session.ended').reason, 'host_revoked');
  const replacement = enroll(f.store, uuidB), socket = new Socket();
  f.relay.attachHost(socket, f.store.authenticateHost(replacement.hash), replacement.hash);
  const fresh = f.relay.hosts.get('pve-710');
  const stale = new Socket(); f.relay.attachHost(stale, oldHost.identity, f.old.hash);
  assert.equal(stale.closeCode, 1008); assert.equal(f.relay.hosts.get('pve-710'), fresh);
  f.hostSocket.emit('close'); assert.equal(f.relay.hosts.get('pve-710'), fresh);
  // B starts with nobody assigned; the administrator assigns the user again.
  assert.throws(() => f.relay.connect(f.user, f.userHash, 'pve-710'), errorCode('desktop_not_found'));
  f.store.assign('alice', 'pve-710');
  const ticket = f.relay.connect(f.user, f.userHash, 'pve-710');
  assert.equal(f.relay.sessions.get(ticket.sessionId).host, fresh);
});

test('A stale client upgrade cannot dispatch session.start to the old Host after an identity change', t => {
  const f = fixture(t), old = enroll(f.store), relay = new Relay({ ...f, publicOrigin: 'https://relay-fixture.invalid', authorizationMs: 3600000, heartbeatMs: 3600000 });
  f.beforeClose = () => relay.close();
  const host = new Socket(); relay.attachHost(host, f.store.authenticateHost(old.hash), old.hash);
  const ticket = relay.connect(f.user, f.userHash, 'pve-710'), pending = relay.sessions.get(ticket.sessionId);
  sync(f.store, uuidB);
  const client = new Socket(); relay.attachClient(client, pending);
  assert.equal(client.closeCode, 1008); assert.equal(host.sent.some(value => value.type === 'session.start'), false);
});

test('Legacy enrollment issuance also rejects an identity result overtaken by a newer inventory', async t => {
  const f = fixture(t), relay = new Relay({ ...f, publicOrigin: 'https://relay-fixture.invalid', authorizationMs: 3600000, heartbeatMs: 3600000 });
  f.beforeClose = () => relay.close();
  let finish, begin; const started = new Promise(resolve => { begin = resolve; });
  f.proxmox.machineIdentity = () => { begin(); return new Promise(resolve => { finish = resolve; }); };
  const pending = relay.createEnrollment(desktop(f.store), f.admin);
  await started; sync(f.store, uuidB); finish({ machineUuid: uuidA });
  await assert.rejects(pending, errorCode('desktop_changed'));
  assert.equal(desktop(f.store).pve_machine_uuid, uuidB);
  assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM host_enrollments').get().n, 0);
});
