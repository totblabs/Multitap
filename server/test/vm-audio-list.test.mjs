import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { VmAudioControl } from '../src/vm-audio.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { enrollFixtureHosts } from './fixtures/admin-assignment-input.mjs';

const machineUuid = 'b1234567-89ab-cdef-8123-0123456789ab';
const settle = async () => { for (let i = 0; i < 20; i++) await new Promise(resolve => setImmediate(resolve)); };
// Two VMs: 149 has no sound card, 150 has audio0 in its config.
function proxmoxFixture() {
  const configs = { 149: { effective: { digest: 'a'.repeat(40) }, current: {} }, 150: { effective: { digest: 'c'.repeat(40), audio0: 'device=ich9-intel-hda,driver=none' }, current: { audio0: 'device=ich9-intel-hda,driver=none' } } };
  const reads = [];
  return { configs, reads,
    vmConfig: async (_config, node, vmid, { current = false } = {}) => { reads.push([vmid, current ? 'current' : 'effective']); return structuredClone(current ? { ...configs[vmid].current, digest: configs[vmid].effective.digest } : configs[vmid].effective); } };
}
function unitFixture() {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32)), admin = store.addUser({ name: 'list-admin', role: 'admin', passwordHash: 'fixture-no-login' });
  const user = store.addUser({ name: 'list-user', role: 'user', passwordHash: 'fixture-no-login' });
  store.syncProxmox([149, 150].map(vmid => ({ vmid, node: 'fixture', name: 'VM ' + vmid, status: 'running' })), admin);
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://fixture.invalid', tokenId: 'fixture@pve!audio', secret: 'fixture-not-real' }), admin);
  store.replaceVmAssignments('list-user', ['pve-149', 'pve-150'], admin);
  let clock = 1000000, changes = 0;
  const proxmox = proxmoxFixture();
  const control = new VmAudioControl({ store, vault, proxmox, relay: {}, now: () => clock, onChange: () => ++changes });
  return { store, control, proxmox, viewer: { id: user, role: 'user' }, admin, tick: ms => { clock += ms; }, changes: () => changes };
}

test('the list state is read from each VM config, only a VM with audio0 needs a second read, and an unchanged config is not read again', async () => {
  const f = unitFixture();
  assert.equal(f.control.listView('pve-149', f.viewer), undefined, 'unknown state: no field, no badge');
  await f.control.refreshStale();
  assert.deepEqual(f.proxmox.reads, [[149, 'effective'], [150, 'effective'], [150, 'current']]);
  assert.deepEqual(f.control.listView('pve-149', f.viewer), { state: 'absent', canAdd: false }, 'observed, and never addable from a client');
  assert.deepEqual(f.control.listView('pve-150', f.viewer), { state: 'present', canAdd: false });
  assert.equal(f.changes(), 2);
  // Within the cache time nothing is read.
  f.proxmox.reads.length = 0; f.tick(60000); await f.control.refreshStale();
  assert.deepEqual(f.proxmox.reads, []);
  // After it, an unchanged digest skips the second read.
  f.tick(600000); await f.control.refreshStale();
  assert.deepEqual(f.proxmox.reads, [[149, 'effective'], [150, 'effective']]);
  assert.equal(f.changes(), 2, 'no push without a change');
});

test('the state follows what Proxmox shows (an audio device added there is pending, then present); nothing is ever addable and no reason is given', async () => {
  const f = unitFixture();
  await f.control.refreshStale();
  for (const viewer of [f.viewer, { id: f.admin, role: 'admin' }]) assert.deepEqual(f.control.listView('pve-149', viewer), { state: 'absent', canAdd: false });
  assert.equal(f.control.listView('pve-149', null), undefined, 'no viewer, no field');
  // The administrator adds an audio device in Proxmox: pending until the VM restarts; a power action makes the VM due for a re-read.
  Object.assign(f.proxmox.configs[149].effective, { audio0: 'device=ich9-intel-hda,driver=none', digest: 'b'.repeat(40) });
  f.control.markDue('pve-149'); f.tick(5000); await f.control.refreshStale();
  assert.deepEqual(f.control.listView('pve-149', f.viewer), { state: 'pending', canAdd: false });
  f.proxmox.configs[149].current.audio0 = 'device=ich9-intel-hda,driver=none'; f.proxmox.configs[149].effective.digest = 'd'.repeat(40);
  f.tick(5000); await f.control.refreshStale();
  assert.deepEqual(f.control.listView('pve-149', f.viewer), { state: 'present', canAdd: false });
  // A locked config is still only observed.
  f.proxmox.configs[150] = { effective: { digest: 'e'.repeat(40), lock: 'backup' }, current: {} };
  f.control.markDue('pve-150'); f.tick(5000); await f.control.refreshStale();
  assert.deepEqual(f.control.listView('pve-150', f.viewer), { state: 'absent', canAdd: false });
  assert.equal(f.proxmox.reads.every(([, kind]) => ['effective', 'current'].includes(kind)), true, 'config reads only');
});

test('the admin inventory sync seeds the states from the configs it already read', async () => {
  const f = unitFixture();
  await f.control.observeInventory({}, [{ vmid: 149, node: 'fixture', audioConfig: { digest: 'a'.repeat(40) } }, { vmid: 150, node: 'fixture', audioConfig: f.proxmox.configs[150].effective }, { vmid: 151, node: 'fixture', audioConfig: {} }]);
  assert.deepEqual(f.proxmox.reads, [[150, 'current']], 'one extra read, only for the VM whose config has audio0');
  assert.equal(f.control.listView('pve-149', f.viewer).state, 'absent'); assert.equal(f.control.listView('pve-150', f.viewer).state, 'present');
  await f.control.observeInventory({}, [{ vmid: 150, node: 'fixture', audioConfig: f.proxmox.configs[150].effective }]);
  assert.deepEqual(f.proxmox.reads, [[150, 'current']], 'the same digest is not read again');
});

test('the client desktop list carries the observed state once known, never offers the dialog, and refuses an add', async t => {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const admin = store.addUser({ name: 'list-admin', role: 'admin', passwordHash: 'fixture-no-login' });
  store.addUser({ name: 'list-user', role: 'user', passwordHash: 'fixture-no-login' });
  const vm = { vmid: 149, node: 'fixture', name: 'VM', status: 'running', machineUuid };
  store.syncProxmox([vm], admin);
  enrollFixtureHosts(store, ['pve-149']);
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://fixture.invalid', tokenId: 'fixture@pve!audio', secret: 'fixture-not-real' }), admin);
  store.replaceVmAssignments('list-user', ['pve-149'], admin);
  const token = newToken(); store.createSession(store.findUser('list-user'), tokenHash(token), Date.now(), 3600000, 'client');
  // The first config read waits, so the list is seen while the state is unknown.
  const base = proxmoxFixture(); let open; const gate = new Promise(resolve => { open = resolve; });
  const proxmox = { ...base, vmConfig: async (...args) => { await gate; return base.vmConfig(...args); }, inventory: async () => [vm] };
  const origin = 'https://audio-list.invalid';
  const server = await buildServer({ store, vault, proxmox, publicOrigin: origin, logger: { error() {} } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  const call = async (path, method = 'GET', data) => { const response = await fetch('http://127.0.0.1:' + server.address().port + '/api/v1' + path, { method, headers: { Origin: origin, Authorization: 'Bearer ' + token, ...(data ? { 'Content-Type': 'application/json' } : {}) }, body: data ? JSON.stringify(data) : undefined }); return { status: response.status, body: await response.json() }; };
  const listed = async () => (await call('/desktops')).body.desktops.find(d => d.id === 'pve-149');
  const before = await listed();
  assert.equal('audio' in before, false, 'unknown until Proxmox was read'); assert.equal(before.soundCardAddAllowed, false); assert.equal(before.vmAudioProtocol, 'vm-audio-v1');
  open(); await settle();
  assert.deepEqual((await listed()).audio, { state: 'absent', canAdd: false });
  const status = await call('/desktops/pve-149/audio');
  assert.deepEqual([status.status, status.body.error.code], [403, 'vm_audio_permission_denied']);
  const added = await call('/desktops/pve-149/audio', 'POST', { protocol: 'vm-audio-v1', action: 'add', requestId: '12345678-1234-4234-8234-123456789abc', revision: 'a'.repeat(64), confirm: true });
  assert.deepEqual([added.status, added.body.error.code], [403, 'vm_audio_permission_denied']);
  assert.deepEqual((await listed()).audio, { state: 'absent', canAdd: false }, 'nothing changed');
});
