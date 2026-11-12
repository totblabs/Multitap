import { captureAssignmentInput, enrollFixtureHosts } from './fixtures/admin-assignment-input.mjs';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { HttpError } from '../src/errors.mjs';
import { Proxmox, validateProxmox } from '../src/proxmox.mjs';

const origin = 'https://sync-tests.example';
const secret = 'Sync-tests-only-token-secret';
const config = { endpoint: 'https://pve.example:8006', tokenId: 'vdi@pve!broker', secret, ca: '' };
const vm = (vmid, extra = {}) => ({ vmid, node: 'pve-a', name: `VM ${vmid}`, status: 'stopped', ...extra });
async function fixture(t, adapter) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const password = 'Sync-tests-only-password!';
  const hashed = await hashPassword(password);
  store.addUser({ name: 'admin', passwordHash: hashed, role: 'admin' });
  store.addUser({ name: 'alice', passwordHash: hashed });
  const server = await buildServer({ store, vault, publicOrigin: origin, proxmox: { call: async () => ({}), ...adapter }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  let cookie = '', csrf = '';
  const call = async (path, method = 'GET', data) => {
    data = captureAssignmentInput(store, path, data);
    const response = await fetch(`http://127.0.0.1:${server.address().port}/api/v1/admin` + path, { method,
      headers: { Cookie: cookie, ...(method !== 'GET' ? { Origin: origin, 'X-CSRF-Token': csrf, 'Content-Type': 'application/json' } : {}) },
      body: data === undefined ? undefined : JSON.stringify(data) });
    return { status: response.status, headers: response.headers, body: await response.json() };
  };
  const login = await call('/login', 'POST', { username: 'admin', password });
  assert.equal(login.status, 200); cookie = login.headers.get('set-cookie').split(';')[0]; csrf = login.body.csrfToken;
  assert.equal((await call('/proxmox', 'PUT', config)).status, 200);
  return { store, vault, call };
}

test('Scoped refresh preserves registered VM names/assignments, never discovers rows and retains unavailable registration', async t => {
  let failed = false;
  const f = await fixture(t, { inventory: async (_config, { targets }) => targets.map(target => failed
    ? { ...target, status: 'unknown', powerError: 'proxmox_vm_not_found' }
    : { node: target.node, vmid: target.vmid, machineUuid: target.expectedUuid, name: 'PVE renamed', status: 'running' }) });
  f.store.addDesktop({ id: 'custom-existing', name: '내 작업 VM', kind: 'vm', vmid: 710, os: '사용자 설정 OS' });
  f.store.syncProxmox([vm(710), vm(711)], 'fixture-seed-only');
  enrollFixtureHosts(f.store, ['custom-existing']);
  f.store.assign('alice', 'custom-existing');
  const alice = f.store.findUser('alice');
  const foreign = JSON.stringify(f.store.inventory().desktops.find(d => d.id === 'pve-711'));
  const first = await f.call('/proxmox/sync', 'POST', {});
  assert.equal(first.status, 200); assert.equal(first.body.count, 1); assert.equal(first.body.added, 0);
  assert.equal(JSON.stringify(f.store.inventory().desktops.find(d => d.id === 'pve-711')), foreign);
  assert.equal(f.store.inventory().desktops.find(d => d.id === 'custom-existing').name, '내 작업 VM');
  assert.equal((await f.call('/proxmox/sync', 'POST', {})).body.added, 0);
  const updated = f.store.inventory().desktops;
  assert.equal(updated.find(d => d.pve_vmid === 710).pve_node, 'pve-a');
  assert.equal(updated.find(d => d.pve_vmid === 710).pve_power_status, 'running');
  assert.equal(updated.find(d => d.pve_vmid === 710).os, '사용자 설정 OS');
  failed = true;
  assert.equal((await f.call('/proxmox/sync', 'POST', {})).body.unavailable, 1);
  assert.equal(f.store.inventory().assignments.length, 1);
  assert.deepEqual((await f.call('/inventory')).body.desktops.map(d => d.id), ['custom-existing']);
  assert.equal(f.store.inventory().desktops.find(d => d.id === 'custom-existing').pve_power_status, 'unknown');
  assert.equal((await f.call('/desktops/custom-existing', 'DELETE')).status, 405);
  failed = false;
  assert.equal((await f.call('/proxmox/sync', 'POST', {})).status, 200);
  assert.deepEqual(f.store.listDesktops(alice.id).map(d => d.id), ['custom-existing']);
  assert.equal(f.store.inventory().assignments.length, 1);
});

test('Failed/malformed scoped observations preserve data and never infer VM deletion from an empty response', async t => {
  let response = [vm(710)];
  const f = await fixture(t, { inventory: async () => { if (response instanceof Error) throw response; return response; } });
  f.store.syncProxmox([vm(710)], 'fixture-seed-only'); enrollFixtureHosts(f.store, ['pve-710']);
  const expectedUuid = f.store.registeredVmTargets()[0].expectedUuid;
  response = [{ ...vm(710), machineUuid: expectedUuid }];
  assert.equal((await f.call('/proxmox/sync', 'POST', {})).status, 200);
  const before = JSON.stringify(f.store.inventory().desktops);
  response = new HttpError(502, 'proxmox_unreachable');
  assert.equal((await f.call('/proxmox/sync', 'POST', {})).status, 502);
  assert.equal(JSON.stringify(f.store.inventory().desktops), before);
  assert.equal((await f.call('/inventory')).body.proxmox.state, 'error');
  response = [vm(710), vm(710)];
  assert.equal((await f.call('/proxmox/sync', 'POST', {})).status, 502);
  assert.equal(JSON.stringify(f.store.inventory().desktops), before);
  response = [];
  assert.equal((await f.call('/proxmox/sync', 'POST', {})).status, 502);
  assert.deepEqual((await f.call('/inventory')).body.desktops.map(d => d.id), ['pve-710']);
});

test('A changed connection configuration cannot commit a pending inventory result', async t => {
  let begin, finish;
  const started = new Promise(resolve => { begin = resolve; });
  const pending = new Promise(resolve => { finish = resolve; });
  const f = await fixture(t, { inventory: async () => { begin(); return pending; } });
  f.store.syncProxmox([vm(710)], 'fixture-seed-only'); enrollFixtureHosts(f.store, ['pve-710']);
  const before = JSON.stringify(f.store.inventory().desktops);
  const sync = f.call('/proxmox/sync', 'POST', {});
  await started;
  assert.equal((await f.call('/proxmox', 'PUT', { ...config, tokenId: 'vdi@pve!replacement' })).status, 200);
  finish([vm(710)]);
  const result = await sync;
  assert.equal(result.status, 409); assert.equal(result.body.error.code, 'proxmox_settings_changed');
  assert.equal(JSON.stringify(f.store.inventory().desktops), before);
});

test('Expiry metadata distinguishes imported expiry from live verification and preserves the token secret on renewal', async t => {
  const imported = '2030-01-01T00:00:00.000Z', renewed = '2031-01-01T00:00:00.000Z';
  let expiry = new HttpError(502, 'proxmox_permission_denied');
  const f = await fixture(t, { inventory: async () => [vm(710)], tokenStatus: async () => { if (expiry instanceof Error) throw expiry; return expiry; } });
  assert.equal((await f.call('/proxmox', 'PUT', { ...config, expiresAt: imported })).status, 200);
  const fallback = (await f.call('/proxmox/status')).body.proxmox;
  assert.equal(fallback.expiresAt, imported); assert.equal(fallback.expirySource, 'configured');
  assert.equal(fallback.expiryError, 'proxmox_permission_denied');
  assert.equal((await f.call('/proxmox', 'PUT', { endpoint: config.endpoint, tokenId: config.tokenId, ca: '', expiresAt: renewed })).status, 200);
  assert.equal(f.vault.open(f.store.setting('proxmox')).secret, secret);
  expiry = { expiresAt: renewed, tokenExpiresAt: renewed, accountExpiresAt: null, accountExpiryKnown: true };
  const live = (await f.call('/proxmox/status')).body.proxmox;
  assert.equal(live.expiresAt, renewed); assert.equal(live.expirySource, 'proxmox');
  assert.equal(live.userId, 'vdi@pve'); assert.equal(live.tokenName, 'broker');
  assert.equal(JSON.stringify(live).includes(secret), false);
  assert.throws(() => validateProxmox({ ...config, expiresAt: 'not-a-date' }), /invalid_proxmox_expiry/);
});

test('Token metadata adapter only reads its configured owner/token and uses the earlier effective expiry', async () => {
  const adapter = new Proxmox(), paths = [];
  adapter.call = async (_config, path, method = 'GET') => {
    paths.push({ path, method });
    if (path.includes('/token/')) return { expire: 2000000000 };
    return [{ userid: 'unrelated@pve', expire: 1 }, { userid: 'vdi@pve', expire: 1900000000 }];
  };
  const status = await adapter.tokenStatus(config);
  assert.equal(status.expiresAt, new Date(1900000000 * 1000).toISOString());
  assert.deepEqual(paths, [{ path: '/access/users/vdi%40pve/token/broker', method: 'GET' }, { path: '/access/users', method: 'GET' }]);
});
