import { captureAssignmentInput, enrollFixtureHosts } from './fixtures/admin-assignment-input.mjs';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, dirname, basename, resolve } from 'node:path';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { HttpError } from '../src/errors.mjs';

const origin = 'https://onboarding-tests.example';
const password = 'Onboarding-tests-only-password!';
const config = { endpoint: 'https://pve.example:8006', tokenId: 'vdi@pve!broker', secret: 'Onboarding-tests-only-token', ca: '' };
const vm = vmid => ({ vmid, name: `Reverse name ${10000 - vmid}`, node: 'pve-a', status: 'stopped' });
async function fixture(t, { credentialsRequired = false } = {}) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const hash = await hashPassword(password);
  store.addUser({ name: 'admin', passwordHash: hash, role: 'admin' });
  store.addUser({ name: 'alice', passwordHash: hash });
  store.addUser({ name: 'bobby', passwordHash: hash });
  if (credentialsRequired) store.db.prepare("UPDATE users SET must_change_credentials=1 WHERE username='admin'").run();
  let inventory = [vm(1000), vm(110), vm(101)];
  const adapter = { call: async () => { if (inventory instanceof Error) throw inventory; return {}; }, inventory: async (_config, { targets }) => {
    if (inventory instanceof Error) throw inventory;
    return targets.map(target => { const row = inventory.find(vm => vm.vmid === target.vmid); return row ? { ...row, node: target.node, machineUuid: target.expectedUuid } : { ...target, status: 'unknown', powerError: 'proxmox_vm_not_found' }; });
  }, tokenStatus: async () => { throw new HttpError(502, 'proxmox_permission_denied'); } };
  let setupCode = null;
  const server = await buildServer({ store, vault, proxmox: adapter, publicOrigin: origin, logger: { error: value => assert.fail(value) }, initialSetupOptions: { announce: code => { setupCode = code; } } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  let cookie = '', csrf = '';
  async function call(path, method = 'GET', data) {
    data = captureAssignmentInput(store, path, data);
    const response = await fetch(`http://127.0.0.1:${server.address().port}/api/v1/admin` + path, { method,
      headers: { Cookie: cookie, ...(method !== 'GET' ? { Origin: origin, 'X-CSRF-Token': csrf, 'Content-Type': 'application/json' } : {}) },
      body: data === undefined ? undefined : JSON.stringify(data) });
    return { status: response.status, headers: response.headers, body: await response.json() };
  }
  async function login(username = 'admin', secret = password) {
    const response = await call('/login', 'POST', { username, password: secret });
    assert.equal(response.status, 200);
    cookie = response.headers.get('set-cookie').split(';')[0]; csrf = response.body.csrfToken;
    return response;
  }
  return { store, call, login, inventory: value => { inventory = value; }, get setupCode() { return setupCode; } };
}

test('Fresh administration requires changed credentials then an authenticated API check, with no VM discovery or later outage relock', async t => {
  const f = await fixture(t, { credentialsRequired: true });
  assert.equal((await f.login()).body.onboarding.stage, 'credentials');
  assert.equal((await f.call('/proxmox', 'PUT', config)).body.error.code, 'credentials_change_required');
  assert.match(f.setupCode, /^[0-9A-HJKMNP-TV-Z]{4}-[0-9A-HJKMNP-TV-Z]{4}-[0-9A-HJKMNP-TV-Z]{4}$/);
  assert.equal((await f.call('/setup', 'POST', { username: 'owner', password: 'Changed-onboarding-password!', passwordConfirmation: 'Changed-onboarding-password!', setupCode: f.setupCode })).status, 200);
  assert.equal((await f.login('owner', 'Changed-onboarding-password!')).body.onboarding.stage, 'proxmox');
  const before = f.store.inventory().users.length;
  assert.equal((await f.call('/users', 'POST', { username: 'new-user', password, passwordConfirmation: password })).body.error.code, 'proxmox_setup_required');
  assert.equal(f.store.inventory().users.length, before);
  assert.equal((await f.call('/assignments/batch', 'PUT', { username: 'alice', desktopIds: [] })).status, 428);
  assert.equal((await f.call('/proxmox', 'PUT', config)).body.onboarding.proxmoxRequired, true);
  assert.equal((await f.call('/proxmox/status')).body.onboarding.proxmoxRequired, true);
  f.inventory(new HttpError(502, 'proxmox_unreachable'));
  assert.equal((await f.call('/proxmox/sync', 'POST', {})).status, 502);
  assert.equal((await f.call('/session')).body.onboarding.complete, false);
  f.inventory([]);
  const success = await f.call('/proxmox/sync', 'POST', {});
  assert.equal(success.body.onboarding.complete, true); assert.equal(success.body.count, 0);
  assert.equal(f.store.setting('proxmox_initialized'), '1');
  assert.equal((await f.call('/users', 'POST', { username: 'new-user', password, passwordConfirmation: password })).status, 201);
  f.inventory(new HttpError(502, 'proxmox_unreachable'));
  assert.equal((await f.call('/proxmox/sync', 'POST', {})).status, 502);
  const expectedAccountId = f.store.findUser('alice').id;
  assert.equal((await f.call('/users/alice', 'PUT', { expectedAccountId, notes: 'Outage does not lock account administration' })).status, 200);
  assert.equal((await f.call('/session')).body.onboarding.complete, true);
});

test('Batch assignment replaces one user atomically, keeps unavailable/legacy assignments and ignores retired VM enabled flags', async t => {
  const f = await fixture(t); await f.login();
  f.store.syncProxmox([vm(1000),vm(110),vm(101)], 'fixture-seed-only');
  enrollFixtureHosts(f.store, ['pve-101', 'pve-110', 'pve-1000']);
  await f.call('/proxmox', 'PUT', config); await f.call('/proxmox/sync', 'POST', {});
  assert.deepEqual((await f.call('/inventory')).body.desktops.map(d => d.pve_vmid), [101, 110, 1000]);
  let result = await f.call('/assignments/batch', 'PUT', { username: 'alice', desktopIds: ['pve-1000', 'pve-110', 'pve-101'] });
  assert.equal(result.status, 200); assert.equal(result.body.added, 3); assert.equal(result.body.removed, 0);
  const alice = f.store.findUser('alice'), bobby = f.store.findUser('bobby');
  f.store.assign('bobby', 'pve-101');
  f.store.addDesktop({ id: 'legacy-pc', name: 'Legacy fixture', kind: 'pc' });
  assert.throws(() => f.store.assign('alice', 'legacy-pc'), /user_or_desktop_unavailable/);
  // Represent an assignment inherited from an older database, not permission
  // to create a new unapproved physical-PC assignment through Store.assign.
  f.store.db.prepare('INSERT INTO assignments(user_id,desktop_id) VALUES(?,?)').run(alice.id, 'legacy-pc');
  f.store.setDesktopEnabled('pve-101', false);
  assert.deepEqual(f.store.listDesktops(alice.id).slice(0, 3).map(d => d.id), ['pve-101', 'pve-110', 'pve-1000']);
  assert.equal((await f.call('/desktops/pve-101/enabled', 'PUT', { enabled: true })).status, 405);
  // An inherited unavailable mapping stays preserved; a missing scoped status
  // observation alone must never invent cluster-wide deletion evidence.
  f.store.db.prepare("UPDATE desktops SET pve_available=0 WHERE id='pve-110'").run();
  f.inventory([vm(1000), vm(101)]); await f.call('/proxmox/sync', 'POST', {});
  const before = JSON.stringify(f.store.inventory().assignments);
  for (const desktopIds of [['pve-1000', 'not-authorized-id'], ['pve-110'], ['legacy-pc'], ['pve-101', 'pve-101']]) {
    assert.notEqual((await f.call('/assignments/batch', 'PUT', { username: 'alice', desktopIds })).status, 200);
    assert.equal(JSON.stringify(f.store.inventory().assignments), before);
  }
  result = await f.call('/assignments/batch', 'PUT', { username: 'alice', desktopIds: ['pve-1000'] });
  assert.equal(result.status, 200); assert.equal(result.body.removed, 1); assert.equal(result.body.preservedUnavailable, 2);
  assert.deepEqual(f.store.listDesktops(bobby.id).map(d => d.id), ['pve-101']);
  assert.deepEqual(f.store.inventory().assignments.filter(a => a.username === 'alice').map(a => a.desktop_id), ['pve-110', 'pve-1000', 'legacy-pc']);
  const metadata = (await f.call('/inventory')).body;
  assert.equal(metadata.users.find(u => u.username === 'alice').unavailableAssignmentCount, 2);
  assert.equal(metadata.preservedUnavailableAssignments, 2);
  f.inventory([vm(101), vm(110), vm(1000)]); await f.call('/proxmox/sync', 'POST', {});
  assert.deepEqual(f.store.listDesktops(alice.id).map(d => d.id), ['pve-110', 'pve-1000']);
  assert.equal(f.store.assignedDesktop(alice.id, 'legacy-pc'), undefined, 'retaining a historical row does not approve or expose that PC');
  assert.ok(f.store.inventory().assignments.some(a => a.username === 'alice' && a.desktop_id === 'legacy-pc'), 'batch replacement preserves the historical assignment row');
  f.store.setUserEnabled('alice', false);
  const disabledBefore = JSON.stringify(f.store.inventory().assignments);
  assert.equal((await f.call('/assignments/batch', 'PUT', { username: 'alice', desktopIds: [] })).status, 409);
  assert.equal(JSON.stringify(f.store.inventory().assignments), disabledBefore);
  assert.equal((await f.call('/assignments/batch', 'PUT', { username: 'bobby', desktopIds: Array.from({ length: 501 }, (_, i) => `vm-${i}`) })).status, 400);
});

test('Upgrading a previously synchronized installation preserves commissioning after an outage and restart', async t => {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-onboarding-upgrade-'));
  t.after(async () => {
    assert.equal(dirname(resolve(directory)), resolve(tmpdir()));
    assert.ok(basename(directory).startsWith('multitap-onboarding-upgrade-'));
    await rm(directory, { recursive: true });
  });
  const path = join(directory, 'broker.sqlite');
  let store = new Store(path);
  store.saveSetting('proxmox', 'encrypted-fixture-config', 'test');
  store.saveSetting('proxmox_sync', JSON.stringify({ state: 'error', checkedAt: '2026-10-22T00:00:00.000Z', count: 0 }), 'test');
  store.close(); store = new Store(path);
  assert.equal(store.onboarding({ must_change_credentials: 0 }).complete, true);
  store.saveSetting('proxmox_sync', JSON.stringify({ state: 'unchecked' }), 'test');
  store.close(); store = new Store(path);
  try { assert.equal(store.onboarding({ must_change_credentials: 0 }).complete, true); }
  finally { store.close(); }
});
