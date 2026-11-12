import { captureAssignmentInput, enrollFixtureHosts } from './fixtures/admin-assignment-input.mjs';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { buildServer } from '../src/http.mjs';
import { HttpError } from '../src/errors.mjs';

const machineUuid = 'b1234567-89ab-cdef-8123-0123456789ab';
async function fixture(t) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const admin = store.addUser({ name: 'audio-admin', role: 'admin', passwordHash: 'fixture-no-login' });
  const user = store.addUser({ name: 'audio-user', role: 'user', passwordHash: 'fixture-no-login' });
  const other = store.addUser({ name: 'audio-other', role: 'user', passwordHash: 'fixture-no-login' });
  const vm = { vmid: 149, node: 'fixture', name: 'VM', status: 'running', machineUuid };
  store.syncProxmox([vm], admin);
  enrollFixtureHosts(store, ['pve-149'], admin);
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://fixture.invalid', tokenId: 'fixture@pve!audio', secret: 'fixture-not-real' }), admin);
  store.replaceVmAssignments('audio-user', ['pve-149'], admin);
  const tokens = Object.fromEntries(['audio-admin', 'audio-user', 'audio-other'].map(name => {
    const token = newToken(); store.createSession(store.findUser(name), tokenHash(token), Date.now(), 3600000, name === 'audio-admin' ? 'admin' : 'client'); return [name, token];
  }));
  tokens['admin-client'] = newToken();
  store.createSession(store.findUser('audio-admin'), tokenHash(tokens['admin-client']), Date.now(), 3600000, 'client');
  // Every Proxmox request the broker makes is counted; none may be a write.
  const asked = [];
  const proxmox = { inventory: async () => { asked.push('inventory'); return [vm]; }, vmConfig: async () => { asked.push('vmConfig'); return { digest: 'a'.repeat(40) }; }, call: async (_config, path, method = 'GET') => { asked.push(method + ' ' + path); return { '/vms': { 'VM.Audit': 1, 'VM.Console': 0 } }; } };
  const origin = 'https://audio-fixture.invalid';
  const server = await buildServer({ store, vault, proxmox, publicOrigin: origin, logger: { error() {} } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  const call = async (path, { method = 'GET', name = 'audio-user', data, adminCookie = false, csrf = true, foreign = false } = {}) => {
    data = captureAssignmentInput(store, path, data);
    const token = tokens[name];
    const response = await fetch('http://127.0.0.1:' + server.address().port + '/api/v1' + path, { method, headers: {
      Origin: foreign ? 'https://foreign.invalid' : origin,
      ...(adminCookie ? { Cookie: '__Secure-multitap-admin=' + token, ...(csrf ? { 'X-CSRF-Token': vault.csrf(token) } : {}) } : { Authorization: 'Bearer ' + token }),
      ...(data ? { 'Content-Type': 'application/json' } : {}),
    }, body: data ? JSON.stringify(data) : undefined });
    return { status: response.status, body: await response.json() };
  };
  return { store, admin, user, other, call, asked, proxmox };
}

test('a released client is told the sound-card dialog is not offered, and its status or add request is refused cleanly without touching Proxmox', async t => {
  const f = await fixture(t), path = '/desktops/pve-149/audio';
  // The list: vm-audio-v1 is still spoken, and nothing may be added (clients that have a sound-card dialog then hide it).
  const list = await f.call('/desktops');
  assert.equal(list.body.desktops[0].vmAudioProtocol, 'vm-audio-v1'); assert.equal(list.body.desktops[0].soundCardAddAllowed, false);
  f.asked.length = 0;
  // A request that comes anyway: the refusal those clients already name ("이 계정은 … 추가할 수 없어요. 관리자에게 요청하세요.").
  const status = await f.call(path);
  assert.deepEqual([status.status, status.body.error.code], [403, 'vm_audio_permission_denied']);
  const input = { protocol: 'vm-audio-v1', action: 'add', requestId: '12345678-1234-4234-8234-123456789abc', revision: 'a'.repeat(64), confirm: true };
  const added = await f.call(path, { method: 'POST', data: input });
  assert.deepEqual([added.status, added.body.error.code], [403, 'vm_audio_permission_denied']);
  // The same strict input and origin rules as before, in front of the refusal.
  assert.equal((await f.call(path, { method: 'POST', data: { ...input, audio0: 'device=AC97' } })).status, 400);
  assert.equal((await f.call(path, { method: 'POST', data: input, foreign: true })).status, 403);
  assert.equal((await f.call(path, { method: 'PUT', data: input })).status, 405);
  // Who may not see the desktop learns nothing more than before.
  assert.equal((await f.call(path, { name: 'audio-other' })).status, 404);
  assert.equal((await f.call(path, { name: 'audio-admin' })).status, 401, 'an administrator-purpose token is not a client session');
  assert.equal((await f.call(path, { name: 'admin-client' })).status, 404);
  f.store.assign('audio-admin', 'pve-149', f.admin);
  assert.deepEqual([(await f.call(path, { name: 'admin-client' })).body.error.code, (await f.call(path, { name: 'admin-client', method: 'POST', data: input })).body.error.code], ['vm_audio_permission_denied', 'vm_audio_permission_denied'], 'an administrator account is refused the same way');
  assert.deepEqual(f.asked.filter(entry => !entry.startsWith('vmConfig')), [], 'no audio request asked Proxmox for anything but the list state');
  assert.equal(f.asked.some(entry => /^(PUT|POST|DELETE) /.test(entry)), false);
  for (const text of [JSON.stringify(status.body), JSON.stringify(added.body)]) { assert.equal(text.includes('fixture-not-real'), false); assert.equal(text.includes(machineUuid), false); }
  assert.equal(f.store.db.prepare("SELECT count(*) n FROM audit_events WHERE event LIKE 'vm_audio%'").get().n, 0);
});

test('the administrator API has no sound-card permission check any more and reports the console permission instead', async t => {
  const f = await fixture(t);
  const gone = await f.call('/admin/proxmox/sound-card-permission', { method: 'POST', name: 'audio-admin', adminCookie: true, data: {} });
  assert.equal(gone.status, 404);
  const metadata = (await f.call('/admin/inventory', { name: 'audio-admin', adminCookie: true })).body.proxmox;
  assert.equal('soundCardPrivilege' in metadata, false); assert.equal(metadata.consolePrivilege, 'unknown');
  assert.equal(f.asked.some(entry => /^(PUT|POST|DELETE) /.test(entry)), false);
});

test('The console permission shows 있음/없음 from each sync and the administrator\'s check, is forgotten when the connection changes, and grants nothing', async t => {
  const f = await fixture(t);
  const privilege = async () => (await f.call('/admin/inventory', { name: 'audio-admin', adminCookie: true })).body.proxmox.consolePrivilege;
  const check = (extra = {}) => f.call('/admin/proxmox/console-permission', { method: 'POST', name: 'audio-admin', adminCookie: true, data: {}, ...extra });
  const sync = () => f.call('/admin/proxmox/sync', { method: 'POST', name: 'audio-admin', adminCookie: true, data: {} });
  assert.equal(await privilege(), 'unknown', 'not asked since this start');
  // Every sync reads it; a failed read keeps the last answer.
  f.proxmox.call = async () => ({ '/vms': { 'VM.Audit': 1, 'VM.PowerMgmt': 1 } });
  const first = await sync(); assert.equal(first.status, 200); assert.equal(first.body.proxmox.consolePrivilege, 'missing'); assert.equal(await privilege(), 'missing');
  f.proxmox.call = async () => { throw new HttpError(502, 'proxmox_unreachable'); };
  assert.equal((await sync()).status, 200, 'the sync itself still succeeds'); assert.equal(await privilege(), 'missing');
  // Zero is a granted, non-propagating permission, not a denial.
  f.proxmox.call = async () => ({ '/vms': { 'VM.Audit': 1, 'VM.Console': 0 } });
  assert.equal((await sync()).status, 200); assert.equal(await privilege(), 'granted');
  // The administrator's read-only check (after running the command) updates it both ways and answers with the new state.
  f.proxmox.call = async () => ({ '/vms': { 'VM.Audit': 1 } });
  const missing = await check(); assert.deepEqual([missing.status, missing.body.granted, missing.body.scope, missing.body.proxmox.consolePrivilege], [200, false, '/vms', 'missing']);
  f.proxmox.call = async () => ({ '/vms': { 'VM.Audit': 1, 'VM.Console': 1 } });
  const granted = await check(); assert.deepEqual([granted.body.granted, granted.body.proxmox.consolePrivilege], [true, 'granted']); assert.equal(await privilege(), 'granted');
  // A privilege on another path, or a malformed answer, is not this one.
  for (const answer of [{ '/': { 'VM.Console': 1 } }, { '/vms': ['VM.Console'] }, { '/vms': { 'VM.Console': 'yes' } }, null]) { f.proxmox.call = async () => answer; assert.equal((await check()).body.granted, false, JSON.stringify(answer)); }
  assert.equal((await check({ csrf: false })).status, 403); assert.equal((await f.call('/admin/proxmox/console-permission', { method: 'POST', data: {} })).status, 401, 'a client session cannot ask');
  // New connection settings: the old answer no longer describes them.
  f.proxmox.call = async () => ({ '/vms': { 'VM.Audit': 1, 'VM.Console': 1 } });
  await check(); assert.equal(await privilege(), 'granted');
  const changed = await f.call('/admin/proxmox', { method: 'PUT', name: 'audio-admin', adminCookie: true, data: { endpoint: 'https://fixture.invalid', tokenId: 'fixture@pve!audio2', secret: 'fixture-not-real-2', ca: '' } });
  assert.equal(changed.status, 200, JSON.stringify(changed.body)); assert.equal(changed.body.proxmox.consolePrivilege, 'unknown'); assert.equal(await privilege(), 'unknown');
  assert.equal((await f.call('/admin/inventory')).status === 200, false, 'a client session cannot read it');
});

test('One VM page request changes several accounts atomically, only where each already has the VM', async t => {
  const f = await fixture(t), path = '/admin/desktops/pve-149/permissions';
  const put = (permissions, extra = {}) => f.call(path, { method: 'PUT', name: 'audio-admin', adminCookie: true, data: { permissions }, ...extra });
  const clip = { textToRemote: true, textToLocal: false, filesToRemote: false, filesToLocal: false };
  const devices = { drives: true, printers: false, usbDevices: false, smartCards: false, serialPorts: false };
  // An account without this VM refuses the whole request.
  const partial = await put([{ username: 'audio-user', clipboardPolicy: clip }, { username: 'audio-other', clipboardPolicy: clip }]);
  assert.equal(partial.status, 409); assert.equal(partial.body.error.code, 'assignment_changed');
  assert.equal(f.store.clipboardPolicy(f.user, 'pve-149').textToRemote, false, 'nothing changed');
  f.store.assign('audio-other', 'pve-149', f.admin);
  const result = await put([{ username: 'audio-user', clipboardPolicy: clip, devicePolicy: devices, soundCardAdd: true }, { username: 'audio-other', clipboardPolicy: clip }]);
  assert.equal(result.status, 200); assert.equal(result.body.policiesChanged, 2);
  assert.equal(f.store.clipboardPolicy(f.other, 'pve-149').textToRemote, true);
  assert.equal(f.store.devicePolicy(f.user, 'pve-149').drives, true);
  assert.equal(f.store.soundCardAddAllowed(f.user, 'pve-149'), true);
  assert.equal(f.store.soundCardAddAllowed(f.other, 'pve-149'), false, 'an omitted grant keeps its saved value');
  assert.equal((await put([{ username: 'audio-other', clipboardPolicy: clip }])).body.policiesChanged, 0, 'no change, no revocation sweep');
  // Validation and authorization stay as strict as the per-account path.
  assert.equal((await put([{ username: 'audio-user', soundCardAdd: 1 }])).status, 400);
  assert.equal((await put([{ username: 'audio-user' }, { username: 'audio-user' }])).status, 400);
  assert.equal((await put([{ username: 'audio-user', desktopId: 'pve-149' }])).status, 400);
  assert.equal((await put([])).status, 400);
  assert.equal((await put([{ username: 'audio-user', clipboardPolicy: clip }], { csrf: false })).status, 403);
  assert.equal((await f.call(path, { method: 'PUT', data: { permissions: [{ username: 'audio-user' }] } })).status, 401, 'a client session cannot use it');
  assert.equal((await f.call('/admin/desktops/pve-999/permissions', { method: 'PUT', name: 'audio-admin', adminCookie: true, data: { permissions: [{ username: 'audio-user' }] } })).status, 404);
});

test('Atomic assignment HTTP accepts explicit boolean and rejects other flag types without changing prior assignment', async t => {
  const f = await fixture(t), path = '/admin/assignments/batch';
  const put = assignments => f.call(path, { method: 'PUT', name: 'audio-admin', adminCookie: true, data: { username: 'audio-user', assignments } });
  assert.equal((await put([{ desktopId: 'pve-149', soundCardAdd: true }])).status, 200);
  assert.equal(f.store.soundCardAddAllowed(f.user, 'pve-149'), true);
  assert.equal((await put([{ desktopId: 'pve-149', soundCardAdd: 1 }])).status, 400);
  assert.equal(f.store.soundCardAddAllowed(f.user, 'pve-149'), true);
  assert.equal((await put([{ desktopId: 'pve-149' }])).status, 200);
  assert.equal(f.store.soundCardAddAllowed(f.user, 'pve-149'), true);
});
