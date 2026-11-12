import { captureAssignmentInput } from './fixtures/admin-assignment-input.mjs';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { DatabaseSync } from 'node:sqlite';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Store as Store18 } from './fixtures/mfa-store18.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { devicePolicyKeys } from '../src/device-policy.mjs';

// Fixture-only broker/host/client sockets. Permissions are granted per
// assignment (user x VM); no real Windows clipboard is involved.
const origin = 'https://assignment-permissions.example';
const deny = Object.freeze(Object.fromEntries(devicePolicyKeys.map(key => [key, false])));
const clipDeny = Object.freeze({ textToRemote: false, textToLocal: false, filesToRemote: false, filesToLocal: false });
const clipAll = Object.freeze({ textToRemote: true, textToLocal: true, filesToRemote: true, filesToLocal: true });
const caps = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true, clipboard: true, clipboardSync: true };
const vms = [750, 751, 752];

test('schema 13→14 copies every account mask into each of its existing assignments and starts new assignments denied', async t => {
  const root = await mkdtemp(join(tmpdir(), 'multitap-assignment-permissions-')); const path = join(root, 'fixture.sqlite');
  let store = new Store18(path); t.after(async () => { store.close(); await rm(root, { recursive: true, force: true }); });
  const admin = store.addUser({ name: '관리자', role: 'admin', passwordHash: 'retained-admin-hash' });
  const alice = store.addUser({ name: 'alice', passwordHash: 'retained-alice-hash', notes: 'retained' });
  const bobby = store.addUser({ name: 'bobby', passwordHash: 'retained-bobby-hash' });
  const carol = store.addUser({ name: 'carol', passwordHash: 'retained-carol-hash' });
  store.syncProxmox(vms.map(vmid => ({ vmid, node: 'fixture', name: 'VM ' + vmid, status: 'running' })), 'fixture');
  for (const [name, id] of [['alice', 'pve-750'], ['alice', 'pve-751'], ['bobby', 'pve-750'], ['carol', 'pve-752']]) store.assign(name, id, 'fixture');
  // Each VM has a registered Host: assignments of desktops without one are
  // removed once at the first start of this release (hostless-assignments-cleanup.test.mjs).
  for (const vmid of vms) store.db.prepare('INSERT INTO hosts(id,desktop_id,token_hash,hostname,machine_uuid,version,created_at) VALUES(?,?,?,?,?,?,?)').run('retained-host-' + vmid, 'pve-' + vmid, 'retained-token-' + vmid, 'retained', '12345678-1234-4234-8234-' + String(vmid).padStart(12, '0'), '1.0.0', 1000);
  store.createSession(store.findUser('alice'), 'retained-session', 1000, 9999999999999);
  store.close();
  const previous = new DatabaseSync(path);
  // Schema 13: the account rows carried one mask applied to all assigned VMs.
  previous.exec(`ALTER TABLE assignments DROP COLUMN sound_card_add;ALTER TABLE assignments DROP COLUMN clipboard_permissions;ALTER TABLE assignments DROP COLUMN rdp_redirections;ALTER TABLE assignments DROP COLUMN favorite;ALTER TABLE assignments DROP COLUMN last_used_at;ALTER TABLE desktops DROP COLUMN pve_tags;ALTER TABLE desktops DROP COLUMN pve_pool;
    UPDATE users SET clipboard_permissions=5,rdp_redirections=65 WHERE username='alice';
    UPDATE users SET clipboard_permissions=15,rdp_redirections=127,disabled=1 WHERE username='carol';
    DROP TABLE session_end_reasons;DROP TABLE session_activity;DROP INDEX sessions_windows_owner;DROP INDEX sessions_replaced;ALTER TABLE sessions DROP COLUMN replaced_at;ALTER TABLE sessions DROP COLUMN issuer;PRAGMA user_version=13;`);
  const before = Object.fromEntries(['users', 'sessions', 'assignments', 'desktops', 'settings', 'audit_events'].map(table => [table, previous.prepare('SELECT rowid,* FROM ' + table + ' ORDER BY rowid').all().map(row => ({ ...row }))]));
  previous.close();
  store = new Store(path);
  assert.equal(store.db.prepare('PRAGMA user_version').get().user_version, 19);
  assert.equal(store.setting('hostless_assignments_cleanup'), '0', 'the one-time cleanup found nothing to remove');
  for (const table of ['users', 'sessions', 'desktops', 'settings', 'audit_events']) assert.deepEqual(store.db.prepare('SELECT rowid,* FROM ' + table + (table === 'settings' ? " WHERE key NOT IN ('hostless_assignments_cleanup','usage_backfill')" : '') + ' ORDER BY rowid').all().map(({ pve_tags, pve_pool, issuer, replaced_at, authorization_revision, ...row }) => { if (table === 'users') assert.equal(authorization_revision, 1); return { ...row }; }), before[table], table + ' is unchanged');
  const masks = Object.fromEntries(before.users.map(user => [user.id, [user.clipboard_permissions, user.rdp_redirections]]));
  assert.deepEqual(store.db.prepare('SELECT rowid,* FROM assignments ORDER BY rowid').all().map(({ clipboard_permissions, rdp_redirections, favorite, last_used_at, sound_card_add, ...row }) => {
    assert.deepEqual([clipboard_permissions, rdp_redirections, favorite, last_used_at], [...masks[row.user_id], 0, null], 'each assignment keeps its account mask'); return { ...row };
  }), before.assignments);
  const aliceClipboard = { ...clipDeny, textToRemote: true, filesToRemote: true }, aliceDevices = { ...deny, drives: true, serialPorts: true };
  for (const id of ['pve-750', 'pve-751']) { assert.deepEqual(store.clipboardPolicy(alice, id), aliceClipboard); assert.deepEqual(store.devicePolicy(alice, id), aliceDevices); }
  assert.deepEqual([store.clipboardPolicy(alice, 'pve-752'), store.devicePolicy(alice, 'pve-752')], [clipDeny, deny], 'an unassigned VM grants nothing');
  assert.deepEqual([store.clipboardPolicy(bobby, 'pve-750'), store.devicePolicy(bobby, 'pve-750')], [clipDeny, deny]);
  assert.deepEqual([store.clipboardPolicy(carol, 'pve-752'), store.devicePolicy(carol, 'pve-752')], [clipDeny, deny], 'a disabled account never carries its copied grant');
  assert.equal(store.db.prepare("SELECT clipboard_permissions FROM assignments WHERE user_id=?").get(carol).clipboard_permissions, 15, 'the disabled account keeps its copied mask for re-enablement');
  for (const [column, value] of [['clipboard_permissions', 16], ['rdp_redirections', 128], ['clipboard_permissions', -1]]) assert.throws(() => store.db.prepare(`UPDATE assignments SET ${column}=? WHERE user_id=?`).run(value, alice), /CHECK/);
  store.assign('alice', 'pve-752', admin);
  assert.deepEqual([store.clipboardPolicy(alice, 'pve-752'), store.devicePolicy(alice, 'pve-752')], [clipDeny, deny], 'a new assignment starts denied');
  assert.equal(store.replaceVmAssignments('bobby', ['pve-750', 'pve-751'], admin).added, 1);
  assert.deepEqual(store.devicePolicy(bobby, 'pve-751'), deny, 'a new batch assignment without a policy starts denied');
  store.close(); store = new Store(path);
  assert.deepEqual(store.clipboardPolicy(alice, 'pve-751'), aliceClipboard, 'reopening does not repeat the copy');
});

async function socket(url, authorization) {
  const ws = new WebSocket(url, { headers: { Authorization: authorization }, perMessageDeflate: false }), queue = [], waiters = []; ws.on('error', () => {});
  ws.closed = new Promise(resolve => ws.once('close', code => { for (const waiter of waiters.splice(0)) { clearTimeout(waiter.timer); waiter.reject(new Error('closed')); } resolve(code); }));
  ws.on('message', (data, binary) => { const value = binary ? data : JSON.parse(data.toString('utf8')); const index = waiters.findIndex(waiter => waiter.predicate(value)); if (index >= 0) { const waiter = waiters.splice(index, 1)[0]; clearTimeout(waiter.timer); waiter.resolve(value); } else queue.push(value); });
  ws.queue = queue; ws.json = value => ws.send(JSON.stringify(value));
  ws.take = (predicate = () => true) => { const index = queue.findIndex(predicate); if (index >= 0) return Promise.resolve(queue.splice(index, 1)[0]); return new Promise((resolve, reject) => { const waiter = { predicate, resolve, reject, timer: setTimeout(() => { const at = waiters.indexOf(waiter); if (at >= 0) waiters.splice(at, 1); reject(new Error('message_timeout')); }, 2500) }; waiters.push(waiter); }); };
  await new Promise((resolve, reject) => { ws.once('open', resolve); ws.once('error', reject); ws.once('unexpected-response', (_req, response) => { response.resume(); reject(Object.assign(new Error('upgrade_rejected'), { status: response.statusCode })); }); }); return ws;
}
async function fixture(t) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32)), sockets = [], logs = [];
  store.addUser({ name: 'fixture-admin', role: 'admin', passwordHash: 'fixture-unused-password-hash' });
  for (const name of ['alice', 'bobby']) store.addUser({ name, role: 'user', passwordHash: 'fixture-unused-password-hash' });
  store.saveSetting('proxmox_initialized', '1', 'fixture');
  const tokens = { admin: newToken(), alice: newToken(), bobby: newToken() }, hostTokens = {};
  for (const name of ['admin', 'alice', 'bobby']) store.createSession(store.findUser(name === 'admin' ? 'fixture-admin' : name), tokenHash(tokens[name]), Date.now(), 3600000, name === 'admin' ? 'admin' : 'client');
  store.syncProxmox(vms.map(vmid => ({ vmid, node: 'fixture-node', name: 'Fake VM ' + vmid, status: 'running' })), 'fixture');
  for (const vmid of vms) {
    const code = tokenHash(newToken()), uuid = '12345678-1234-1234-1234-' + String(vmid).padStart(12, '0'); hostTokens[vmid] = newToken();
    store.issueEnrollment('pve-' + vmid, uuid, code, 'fixture'); store.consumeEnrollment(code, uuid, { hostname: 'Fixture Host ' + vmid, version: '0.2.49', tokenHash: tokenHash(hostTokens[vmid]) });
  }
  const server = await buildServer({ store, vault, publicOrigin: origin, proxmox: { inventory: async () => [] }, relayOptions: { heartbeatMs: 1000, authorizationMs: 20 }, logger: { error: value => logs.push(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve)); const base = 'http://127.0.0.1:' + server.address().port;
  t.after(async () => { for (const ws of sockets) ws.terminate(); await new Promise(resolve => server.close(resolve)); store.close(); assert.deepEqual(logs, []); });
  const open = async (path, authorization) => { const ws = await socket(base.replace(/^http/, 'ws') + path, authorization); sockets.push(ws); return ws; };
  const call = async (path, { method = 'GET', data, as = 'alice' } = {}) => { data = captureAssignmentInput(store, path, data); const response = await fetch(base + '/api/v1' + path, { method, headers: { Origin: origin, ...(data !== undefined ? { 'Content-Type': 'application/json' } : {}), ...(as === 'admin' ? { Cookie: '__Secure-multitap-admin=' + tokens.admin, 'X-CSRF-Token': vault.csrf(tokens.admin) } : { Authorization: 'Bearer ' + tokens[as] }) }, body: data === undefined ? undefined : JSON.stringify(data) }); return { status: response.status, body: await response.json() }; };
  const hosts = {};
  for (const vmid of [750, 751]) { const host = hosts['pve-' + vmid] = await open('/api/v1/hosts/channel', 'Host ' + hostTokens[vmid]); host.json({ type: 'host.capabilities', capabilities: caps }); assert.equal((await host.take(v => v.type === 'host.capabilities.ack')).accepted, true); }
  const batch = (assignments, username = 'alice') => call('/admin/assignments/batch', { method: 'PUT', as: 'admin', data: { username, assignments } });
  // One live controller on a VM: its broker grant, media socket and host start.
  const controller = async (id, mode = 'stream') => {
    const response = await call('/desktops/' + id + '/connect', { method: 'POST', data: mode === 'stream' ? { clipboardProtocol: 'clipboard-sync-v1' } : { mode } }); assert.equal(response.status, 200);
    const media = await open(new URL(response.body.webSocketUrl).pathname, 'Bearer ' + tokens.alice), start = await hosts[id].take(v => v.type === 'session.start');
    return { connection: response.body, media, start };
  };
  return { store, tokens, hostTokens, hosts, call, open, batch, controller, alice: store.findUser('alice').id };
}

test('two VMs of one user carry independent grants in the desktop list, connect response, host start and clipboard channel', async t => {
  const f = await fixture(t);
  const textToVm = { ...clipDeny, textToRemote: true };
  let result = await f.batch([{ desktopId: 'pve-750', clipboardPolicy: textToVm, devicePolicy: { ...deny, drives: true } }, { desktopId: 'pve-751' }]);
  assert.equal(result.status, 200); assert.equal(result.body.added, 2);
  assert.deepEqual([f.store.clipboardPolicy(f.alice, 'pve-751'), f.store.devicePolicy(f.alice, 'pve-751')], [clipDeny, deny], 'a new assignment without a policy is denied');
  const list = (await f.call('/desktops')).body.desktops;
  assert.deepEqual(list.map(d => [d.id, d.clipboardPolicy, d.host.capabilities.clipboard]), [['pve-750', textToVm, true], ['pve-751', clipDeny, false]]);
  assert.deepEqual((await f.call('/desktops/pve-751')).body.desktop.clipboardPolicy, clipDeny);
  const granted = await f.controller('pve-750'), denied = await f.controller('pve-751');
  assert.deepEqual([granted.connection.clipboardPolicy, granted.start.clipboardPolicy], [textToVm, textToVm]);
  assert.deepEqual([denied.connection.clipboardPolicy, denied.start.clipboardPolicy], [clipDeny, clipDeny]);
  assert.equal(denied.connection.clipboardWebSocketUrl, undefined); assert.equal(denied.start.clipboardUrl, undefined);
  await assert.rejects(f.open('/api/v1/sessions/' + denied.connection.sessionId + '/clipboard', 'Bearer ' + f.tokens.alice), error => error.status === 401);
  const local = await f.open(new URL(granted.connection.clipboardWebSocketUrl).pathname, 'Bearer ' + f.tokens.alice), remote = await f.open(new URL(granted.start.clipboardUrl).pathname, 'Host ' + f.hostTokens[750]);
  assert.deepEqual((await local.take(v => v.type === 'clipboard.ready')).clipboardPolicy, textToVm); assert.deepEqual((await remote.take(v => v.type === 'clipboard.ready')).clipboardPolicy, textToVm);
  for (const { media } of [granted, denied]) { media.json({ type: 'session.stop' }); await media.take(v => v.type === 'session.ended'); }
  for (const id of ['pve-750', 'pve-751']) await f.hosts[id].take(v => v.type === 'session.stop');
  // Another user's assignment to the same VM is a separate, denied grant.
  assert.equal((await f.batch([{ desktopId: 'pve-750' }], 'bobby')).status, 200);
  assert.deepEqual((await f.call('/desktops', { as: 'bobby' })).body.desktops.map(d => [d.id, d.clipboardPolicy]), [['pve-750', clipDeny]]);
  result = await f.call('/admin/inventory', { as: 'admin' });
  assert.deepEqual(result.body.assignments.map(a => [a.username, a.desktop_id, a.clipboardPolicy.textToRemote, a.devicePolicy.drives]), [['alice', 'pve-750', true, true], ['alice', 'pve-751', false, false], ['bobby', 'pve-750', false, false]]);
});

test('restricting one assignment ends only that VM session\'s clipboard; grants, unrelated VMs and omitted policies keep sessions', async t => {
  const f = await fixture(t);
  assert.equal((await f.batch([{ desktopId: 'pve-750', clipboardPolicy: clipAll }, { desktopId: 'pve-751', clipboardPolicy: clipAll }])).status, 200);
  const first = await f.controller('pve-750'), second = await f.controller('pve-751');
  const quiet = async (...sessions) => { await new Promise(resolve => setTimeout(resolve, 100)); for (const s of sessions) assert.equal(s.media.queue.some(v => v.type === 'session.ended'), false); };
  // An unchanged batch (omitted policies keep existing values) and a widened grant keep both sessions.
  let result = await f.batch([{ desktopId: 'pve-750' }, { desktopId: 'pve-751', devicePolicy: { ...deny, printers: true } }]);
  assert.equal(result.status, 200); assert.equal(result.body.policiesChanged, 1);
  assert.deepEqual(f.store.clipboardPolicy(f.alice, 'pve-750'), clipAll, 'an omitted policy keeps the existing value');
  await quiet(first, second);
  result = await f.batch([{ desktopId: 'pve-750', clipboardPolicy: { ...clipAll, filesToLocal: false } }, { desktopId: 'pve-751' }]);
  assert.equal(result.status, 200); assert.equal(result.body.policiesChanged, 1);
  // Only the clipboard ends; the stream stays.
  await quiet(first, second);
  assert.equal(first.media.readyState, WebSocket.OPEN);
  assert.equal(f.hosts['pve-750'].queue?.some?.(v => v.type === 'session.stop') ?? false, false);
  assert.equal(second.media.readyState, WebSocket.OPEN);
  assert.ok(f.store.auditLog().some(event => event.event === 'clipboard_policy_changed' && event.subject === f.alice + ':pve-750'));
  // Removing an assignment still ends its session through authorization.
  assert.equal((await f.batch([{ desktopId: 'pve-750' }])).status, 200);
  assert.equal((await second.media.take(v => v.type === 'session.ended')).reason, 'authorization_revoked');
});

test('the batch update is atomic: any invalid entry changes no assignment or permission', async t => {
  const f = await fixture(t);
  assert.equal((await f.batch([{ desktopId: 'pve-750', clipboardPolicy: { ...clipDeny, textToLocal: true }, devicePolicy: { ...deny, serialPorts: true } }])).status, 200);
  const snapshot = () => JSON.stringify({ rows: f.store.db.prepare('SELECT * FROM assignments ORDER BY user_id,desktop_id').all(), audit: f.store.db.prepare('SELECT count(*) AS n FROM audit_events').get().n });
  const before = snapshot();
  const cases = [
    [[{ desktopId: 'pve-750', clipboardPolicy: clipAll }, { desktopId: 'pve-751', clipboardPolicy: { textToRemote: true } }], 400, 'invalid_clipboard_policy'],
    [[{ desktopId: 'pve-751', devicePolicy: { ...deny, drives: true } }, { desktopId: 'pve-750', devicePolicy: { ...deny, clipboard: true } }], 400, 'invalid_device_policy'],
    [[{ desktopId: 'pve-750', clipboardPolicy: null }], 400, 'invalid_clipboard_policy'],
    [[{ desktopId: 'pve-750', clipboardPolicy: clipAll, extra: true }], 400, 'invalid_assignment_entry'],
    [[{ desktopId: 'pve-751', clipboardPolicy: clipAll }, 'pve-750'], 400, 'invalid_assignment_entry'],
    [[{ desktopId: 'pve-750' }, { desktopId: 'pve-750', clipboardPolicy: clipAll }], 400, 'invalid_desktop_ids'],
    [[{ desktopId: 'pve-751', clipboardPolicy: clipAll }, { desktopId: 'pve-missing', clipboardPolicy: clipAll }], 409, 'user_or_desktop_unavailable'],
    [[{ clipboardPolicy: clipAll }], 400, 'invalid_desktop_ids'],
  ];
  for (const [assignments, status, code] of cases) {
    const result = await f.batch(assignments); assert.equal(result.status, status, JSON.stringify(assignments)); assert.equal(result.body.error.code, code);
    assert.equal(snapshot(), before, 'nothing changed for ' + JSON.stringify(assignments));
  }
  for (const data of [{ username: 'alice' }, { username: 'alice', desktopIds: [], assignments: [] }, { username: 'alice', desktopIds: [{ desktopId: 'pve-750' }] }, { username: 'alice', assignments: 'pve-750' }]) {
    assert.equal((await f.call('/admin/assignments/batch', { method: 'PUT', as: 'admin', data })).status, 400); assert.equal(snapshot(), before);
  }
  // The legacy id list remains accepted and keeps existing permissions.
  const legacy = await f.call('/admin/assignments/batch', { method: 'PUT', as: 'admin', data: { username: 'alice', desktopIds: ['pve-750', 'pve-751'] } });
  assert.equal(legacy.status, 200); assert.equal(legacy.body.added, 1);
  assert.deepEqual([f.store.clipboardPolicy(f.alice, 'pve-750'), f.store.devicePolicy(f.alice, 'pve-750')], [{ ...clipDeny, textToLocal: true }, { ...deny, serialPorts: true }]);
  assert.deepEqual([f.store.clipboardPolicy(f.alice, 'pve-751'), f.store.devicePolicy(f.alice, 'pve-751')], [clipDeny, deny]);
  // A complete 500-VM per-assignment body fits the request limit.
  const large = Array.from({ length: 500 }, (_, i) => ({ desktopId: 'vm-' + i, clipboardPolicy: clipAll, devicePolicy: deny }));
  assert.equal((await f.batch(large)).body.error.code, 'user_or_desktop_unavailable');
});
