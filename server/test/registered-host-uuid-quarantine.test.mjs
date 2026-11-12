import test from 'node:test';
import assert from 'node:assert/strict';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { Proxmox } from '../src/proxmox.mjs';
import { PowerControl } from '../src/power.mjs';
import { HttpError } from '../src/errors.mjs';
import { newToken, tokenHash } from '../src/security.mjs';

const UUID = '12345678-1234-4234-8234-123456789abc';
const OTHER = '87654321-4321-4321-8321-cba987654321';
function fixture(t) {
  const store = new Store(':memory:'), vault = new Vault(Buffer.alloc(32, 28));
  t.after(() => store.close());
  const actor = store.addUser({ name: 'operator', role: 'admin', passwordHash: 'fixture' });
  const user = store.addUser({ name: 'assigned', passwordHash: 'fixture' });
  store.syncProxmox([710, 711].map(vmid => ({ vmid, node: 'node-a', name: 'fixture', status: 'running' })), actor);
  store.assign('assigned', 'pve-710');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://fixture.invalid', tokenId: 'fixture@pve!test', secret: 'synthetic' }), actor);
  const enroll = () => {
    const code = tokenHash(newToken()), hash = tokenHash(newToken());
    store.issueEnrollment('pve-710', UUID, code, actor);
    const row = store.consumeEnrollment(code, UUID, { hostname: 'fixture', version: '1.0.14', tokenHash: hash });
    return { ...row, hash };
  };
  const host = enroll(), api = new Proxmox(), calls = [];
  let observed = UUID, error = null, held = null, now = Date.now();
  api.call = async (_config, path, method = 'GET') => {
    calls.push({ path, method }); assert.equal(method, 'GET'); assert.ok(path.startsWith('/nodes/node-a/qemu/710/'));
    if (held) await held;
    if (error) throw new HttpError(502, error);
    if (path.includes('/config')) return { smbios1: 'uuid=' + observed, name: 'fixture', ostype: 'win11' };
    if (path.endsWith('/status/current')) return { status: 'running', uptime: 1, cpus: 2, maxmem: 1024, mem: 512, cpu: 0.1 };
    assert.fail('unexpected API path');
  };
  const power = new PowerControl({ store, vault, proxmox: api, now: () => now });
  const row = () => ({ ...store.db.prepare("SELECT * FROM desktops WHERE id='pve-710'").get() });
  const refresh = async kind => {
    if (kind === 'power') { await power.refresh({ maxAgeMs: 0 }); return; }
    const targets = store.registeredVmTargets();
    const observations = await api.inventory({}, { targets: targets.map(({ node, vmid, expectedUuid }) => ({ node, vmid, expectedUuid })) });
    store.refreshRegisteredVms(targets, observations, actor, () => {}, now);
  };
  return { store, actor, user, host, enroll, api, power, calls, row, refresh,
    observe: value => { observed = value; now += 5000; }, fail: value => { error = value; now += 5000; }, hold: value => { held = value; } };
}

for (const kind of ['store', 'power']) {
  test(kind + ' definitive current UUID mismatch quarantines availability only; exact matching config restores it', async t => {
    const f = fixture(t), assignments = JSON.stringify(f.store.db.prepare('SELECT * FROM assignments').all());
    const untouched = JSON.stringify(f.store.db.prepare("SELECT * FROM desktops WHERE id='pve-711'").get());
    f.observe(OTHER); await f.refresh(kind);
    assert.equal(f.row().pve_available, 0, 'definitive mismatch must close stale Host authority');
    assert.equal(f.row().pve_power_error, 'proxmox_machine_uuid_mismatch');
    assert.equal(f.store.authenticateHost(f.host.hash), undefined);
    assert.equal(f.store.assignedDesktop(f.user, 'pve-710'), undefined);
    assert.equal(f.store.hostConsoleBinding(f.host.hash), undefined);
    assert.equal(f.store.db.prepare('SELECT revoked FROM hosts WHERE id=?').get(f.host.hostId).revoked, 0);
    assert.equal(f.row().pve_machine_uuid, UUID); assert.equal(f.store.registeredVmTargets()[0].hostId, f.host.hostId);
    assert.equal(f.store.registeredHostIds().has('pve-710'), true, 'admin registration remains visible');
    assert.equal(JSON.stringify(f.store.db.prepare('SELECT * FROM assignments').all()), assignments);
    assert.equal(JSON.stringify(f.store.db.prepare("SELECT * FROM desktops WHERE id='pve-711'").get()), untouched);
    f.fail('proxmox_unreachable'); await f.refresh(kind); assert.equal(f.row().pve_available, 0, 'unknown cannot revive quarantine');
    f.fail(null); f.observe(UUID); await f.refresh(kind);
    assert.equal(f.row().pve_available, 1); assert.equal(f.row().pve_power_error, null);
    assert.equal(f.store.authenticateHost(f.host.hash).id, f.host.hostId);
    assert.equal(f.store.assignedDesktop(f.user, 'pve-710').id, 'pve-710');
  });
  test(kind + ' network/permission/missing UUID uncertainty preserves prior availability and registration', async t => {
    const f = fixture(t);
    for (const error of ['proxmox_unreachable', 'proxmox_permission_denied', 'proxmox_machine_uuid_missing']) {
      f.fail(error); await f.refresh(kind);
      assert.equal(f.row().pve_available, 1); assert.equal(f.store.authenticateHost(f.host.hash).id, f.host.hostId);
      assert.equal(f.store.db.prepare('SELECT revoked FROM hosts WHERE id=?').get(f.host.hostId).revoked, 0);
    }
  });
  test(kind + ' late mismatch cannot quarantine a replacement Host registration', async t => {
    const f = fixture(t); let release;
    f.hold(new Promise(resolve => { release = resolve; })); f.observe(OTHER);
    const pending = f.refresh(kind); const next = f.enroll(); release();
    if (kind === 'store') await assert.rejects(pending, /host_registration_changed/); else { await pending; assert.equal(f.power.snapshotError, 'proxmox_targets_changed'); }
    assert.equal(f.row().pve_available, 1); assert.equal(f.store.authenticateHost(next.hash).id, next.hostId);
    assert.equal(f.row().pve_power_error, null);
  });
}

test('exact mismatch/restore target CAS rejects stale node, VMID, expected UUID and Host identity', t => {
  const f = fixture(t), targets = f.store.registeredVmTargets();
  for (const [column, value] of [['pve_node', 'node-b'], ['pve_vmid', 712], ['pve_machine_uuid', OTHER]]) {
    const before = f.row()[column]; f.store.db.prepare('UPDATE desktops SET ' + column + '=? WHERE id=?').run(value, 'pve-710');
    assert.throws(() => f.store.refreshRegisteredVms(targets, [{ node: 'node-a', vmid: 710, expectedUuid: UUID, status: 'unknown', powerError: 'proxmox_machine_uuid_mismatch' }], f.actor), /host_registration_changed/);
    assert.equal(f.row().pve_available, 1); f.store.db.prepare('UPDATE desktops SET ' + column + '=? WHERE id=?').run(before, 'pve-710');
  }
});

test('a power sample without an actual matching observed UUID cannot revive quarantined availability', async t => {
  const f = fixture(t); f.store.db.prepare("UPDATE desktops SET pve_available=0 WHERE id='pve-710'").run();
  f.api.inventory = async () => [{ node: 'node-a', vmid: 710, status: 'running' }];
  await f.refresh('power'); assert.equal(f.row().pve_available, 0);
  assert.equal(f.store.authenticateHost(f.host.hash), undefined);
});

test('the single-statement availability CAS itself refuses stale Host/node/VMID/UUID without relying on the batch stamp', t => {
  const f = fixture(t), target = f.store.registeredVmTargets()[0];
  const mismatch = { node: target.node, vmid: target.vmid, expectedUuid: UUID, status: 'unknown', powerError: 'proxmox_machine_uuid_mismatch' };
  for (const [key, value] of [['hostId', 'different-host'], ['node', 'node-b'], ['vmid', 712], ['expectedUuid', OTHER]]) {
    const changed = { ...target, [key]: value }, observed = { ...mismatch, node: changed.node, vmid: changed.vmid, expectedUuid: changed.expectedUuid };
    assert.equal(f.store.observeRegisteredVmAvailability(changed, observed), false);
    assert.equal(f.row().pve_available, 1);
  }
  const next = f.enroll(); assert.equal(f.store.observeRegisteredVmAvailability(target, mismatch), false);
  assert.equal(f.store.authenticateHost(next.hash).id, next.hostId);
});

test('a malformed mismatch receipt cannot quarantine or adopt a reported replacement identity', t => {
  const f = fixture(t), target = f.store.registeredVmTargets()[0];
  for (const extra of [{ expectedUuid: OTHER }, { status: 'running' }, { machineUuid: OTHER }]) {
    assert.throws(() => f.store.observeRegisteredVmAvailability(target, { node: target.node, vmid: target.vmid, expectedUuid: UUID,
      status: 'unknown', powerError: 'proxmox_machine_uuid_mismatch', ...extra }), /invalid_proxmox_inventory/);
    assert.equal(f.row().pve_available, 1); assert.equal(f.row().pve_machine_uuid, UUID);
  }
});
