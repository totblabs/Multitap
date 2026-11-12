import test from 'node:test';
import assert from 'node:assert/strict';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { PowerControl } from '../src/power.mjs';
import { newToken, tokenHash } from '../src/security.mjs';

const deferred = () => { let resolve; const promise = new Promise(done => { resolve = done; }); return { promise, resolve }; };
function fixture(t, { empty = false } = {}) {
  const store = new Store(':memory:'), vault = new Vault(Buffer.alloc(32, 17));
  t.after(() => store.close());
  const actor = store.addUser({ name: 'operator', role: 'admin', passwordHash: 'fixture' });
  store.syncProxmox([810,811,812].map(vmid => ({ vmid, node: 'node-a', name: `VM ${vmid}`, status: 'stopped' })), actor);
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://fixture.invalid', tokenId: 'fixture@pve!test', secret: 'fixture-only' }), actor);
  const enroll = id => {
    const code = newToken(), uuid = `12345678-1234-4234-8234-${id.toString().padStart(12, '0')}`;
    store.issueEnrollment(`pve-${id}`, uuid, tokenHash(code), actor);
    return store.consumeEnrollment(tokenHash(code), uuid, { hostname: 'fixture-host', version: '1.0.14', tokenHash: tokenHash(newToken()) }).hostId;
  };
  if (!empty) { enroll(810); enroll(811); }
  let now = Date.now(), reader, calls = [];
  const row = (vmid, extra = {}) => ({ node: 'node-a', vmid, name: `VM ${vmid}`, status: 'running', cpuUsage: 0.25, vcpus: 4, memoryBytes: 8192, memoryUsedBytes: 4096, uptimeSeconds: 30, ...extra });
  reader = async () => [row(810), row(811)];
  const proxmox = { inventory: async (config, options) => { calls.push(structuredClone(options)); return reader(config, options); }, vmStatus: async () => ({ status: 'running', paused: false, uptimeSeconds: 30 }) };
  const power = new PowerControl({ store, vault, proxmox, now: () => now });
  return { store, power, proxmox, actor, row, calls, enroll, setReader: value => { reader = value; }, advance: () => { now += 5000; } };
}

test('shared status refresh queries only exact registered bindings and never modifies an unregistered VM', async t => {
  const f = fixture(t), unregistered = JSON.stringify(f.store.db.prepare("SELECT * FROM desktops WHERE id='pve-812'").get());
  await Promise.all(Array.from({ length: 8 }, () => f.power.refresh()));
  assert.equal(f.calls.length, 1);
  assert.deepEqual(f.calls[0].targets, f.store.registeredVmTargets().map(({ node, vmid, expectedUuid }) => ({ node, vmid, expectedUuid })));
  assert.deepEqual(f.calls[0].targets.map(target => target.vmid), [810,811]);
  assert.equal(JSON.stringify(f.store.db.prepare("SELECT * FROM desktops WHERE id='pve-812'").get()), unregistered);
  assert.equal(f.power.view('pve-810', f.actor).resources.cpuUsagePercent, 25);
});

test('an installation with no registered VM makes no upstream observation request', async t => {
  const f = fixture(t, { empty: true });
  f.setReader(() => assert.fail('no VM request without a registration'));
  await f.power.refresh();
  assert.equal(f.calls.length, 0);
  assert.equal(f.power.snapshotError, null);
});

test('late status read cannot write after that Host registration is removed', async t => {
  const f = fixture(t), hold = deferred(), target = f.store.registeredVmTargets()[0];
  f.setReader(() => hold.promise);
  const before = JSON.stringify(f.store.db.prepare('SELECT * FROM desktops ORDER BY id').all());
  const refresh = f.power.refresh();
  f.store.removeHostRegistration(target.desktopId, target.hostId, f.actor, () => {});
  hold.resolve([f.row(810), f.row(811)]); await refresh;
  assert.equal(f.power.snapshotError, 'proxmox_targets_changed');
  assert.equal(JSON.stringify(f.store.db.prepare('SELECT * FROM desktops ORDER BY id').all()), before);
  assert.equal(f.power.resources.size, 0);
});

test('replacing a registration invalidates a fresh status cache', async t => {
  const f = fixture(t); await f.power.refresh();
  f.enroll(810); await f.power.refresh();
  assert.equal(f.calls.length, 2);
  assert.equal(f.power.snapshotError, null);
});

test('one failed registered target retains an explicitly stale sample while another target updates', async t => {
  const f = fixture(t); await f.power.refresh();
  const previous = f.power.view('pve-810', f.actor).resources;
  f.advance(); f.setReader(async () => [f.row(810, { status: 'unknown', powerError: 'proxmox_unreachable' }), f.row(811, { cpuUsage: 0.5 })]);
  await f.power.refresh();
  const failed = f.power.view('pve-810', f.actor);
  assert.equal(failed.power.error, 'proxmox_unreachable');
  assert.deepEqual(failed.resources, { ...previous, stale: true });
  assert.equal(f.power.view('pve-811', f.actor).resources.cpuUsagePercent, 50);
});

test('an upstream response outside the captured registered target set never writes any VM state', async t => {
  const f = fixture(t), before = JSON.stringify(f.store.db.prepare('SELECT * FROM desktops ORDER BY id').all());
  f.setReader(async () => [f.row(810), f.row(811), f.row(812)]);
  await f.power.refresh();
  assert.equal(f.power.snapshotError, 'proxmox_invalid_response');
  assert.equal(JSON.stringify(f.store.db.prepare('SELECT * FROM desktops ORDER BY id').all()), before);
});

test('a late per-VM status probe cannot write across registration replacement', async t => {
  const f = fixture(t), hold = deferred(); f.proxmox.vmStatus = () => hold.promise;
  const probe = f.power.probe('pve-810'); f.enroll(810);
  // Re-enrollment legitimately updates its identity-check timestamp. Only the
  // subsequent late observation must leave that committed record unchanged.
  const before = JSON.stringify(f.store.db.prepare("SELECT * FROM desktops WHERE id='pve-810'").get());
  hold.resolve({ status: 'running', paused: false, uptimeSeconds: 30 });
  await assert.rejects(probe, error => error.code === 'proxmox_targets_changed');
  assert.equal(JSON.stringify(f.store.db.prepare("SELECT * FROM desktops WHERE id='pve-810'").get()), before);
});
