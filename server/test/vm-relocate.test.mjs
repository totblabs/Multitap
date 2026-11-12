// A registered desktop follows its VM when Proxmox moves it: migration or HA
// recovery to another node, a backup restored to a new VM ID. Real Store,
// Proxmox adapter, PowerControl, Relay/ConsoleRelay and stream proof; the
// cluster is an in-memory table behind Proxmox.call. No network.
import test from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { Proxmox } from '../src/proxmox.mjs';
import { PowerControl } from '../src/power.mjs';
import { Relay } from '../src/relay.mjs';
import { HttpError } from '../src/errors.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { VmRelocation, readRegisteredInventory } from '../src/vm-relocate.mjs';
import { streamConsoleProof, streamConsoleProfileProtocol } from '../src/stream-console-profile.mjs';
import { auditEntry } from '../admin/js/timeline.js';
import { eventLabel } from '../admin/js/labels.js';

const UUID = '12345678-1234-4234-8234-123456789abc';
const OTHER = '87654321-4321-4321-8321-cba987654321';
const SECOND = '11111111-2222-4333-8444-555555555555';

// Proxmox as seen through its API: a config read of a VM that is not on the
// asked node is an HTTP error (500 "does not exist", or 595 from the proxy of
// a node that is down), both proxmox_request_failed in proxmox.mjs.
function cluster() {
  const vms = new Map(), calls = [], down = new Set();
  let listing = null, held = null;
  const call = async (_config, path, method = 'GET') => {
    calls.push(path);
    if (path === '/cluster/resources?type=vm') {
      if (held) await held;
      if (listing) throw new HttpError(502, listing);
      return [...vms].map(([vmid, vm]) => ({ type: 'qemu', vmid, node: vm.node, name: vm.name, template: vm.template ? 1 : 0 }));
    }
    if (path.startsWith('/access/permissions')) { const scope = decodeURIComponent(path.split('path=')[1]); return { [scope]: { 'VM.Console': 1 } }; }
    const match = /^\/nodes\/([^/]+)\/qemu\/(\d+)\/(config\?current=1|status\/current)$/.exec(path);
    if (!match || method !== 'GET') assert.fail('unexpected Proxmox call ' + method + ' ' + path);
    const [, node, id, what] = match, vm = vms.get(Number(id));
    if (down.has(node) || !vm || vm.node !== node) throw new HttpError(502, 'proxmox_request_failed');
    if (what === 'status/current') return { status: 'running', uptime: 5, cpus: 2, maxmem: 1024, mem: 512, cpu: 0.1 };
    return { name: vm.name, smbios1: 'uuid=' + vm.uuid, ostype: 'win11', vga: 'std', ...(vm.template ? { template: 1 } : {}) };
  };
  return { vms, calls, down, call, failListing: code => { listing = code; }, holdListing: value => { held = value; },
    reads: (vmid, node) => calls.filter(path => path === `/nodes/${node}/qemu/${vmid}/config?current=1`).length,
    listings: () => calls.filter(path => path.startsWith('/cluster/resources')).length };
}

function fixture(t, { relocationOptions = {} } = {}) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  t.after(() => store.close());
  const actor = store.addUser({ name: 'operator', role: 'admin', passwordHash: 'fixture' });
  const user = store.addUser({ name: 'assigned', passwordHash: 'fixture' });
  store.syncProxmox([710, 711].map(vmid => ({ vmid, node: 'node-a', name: 'VM ' + vmid, status: 'running' })), actor);
  store.assign('assigned', 'pve-710');
  const config = { endpoint: 'https://fixture.invalid', tokenId: 'fixture@pve!test', secret: 'synthetic' };
  store.saveSetting('proxmox', vault.seal(config), actor);
  const hostToken = tokenHash(newToken()), code = tokenHash(newToken());
  store.issueEnrollment('pve-710', UUID, code, actor);
  const host = store.consumeEnrollment(code, UUID, { hostname: 'fixture', version: '1.0.60', tokenHash: hostToken });
  const pve = cluster();
  pve.vms.set(710, { node: 'node-a', uuid: UUID, name: 'desk' });
  pve.vms.set(711, { node: 'node-a', uuid: SECOND, name: 'other' });
  const api = new Proxmox(); api.call = pve.call;
  let now = Date.now();
  const relocation = new VmRelocation({ store, proxmox: api, now: () => now, ...relocationOptions });
  const power = new PowerControl({ store, vault, proxmox: api, now: () => now, relocation });
  const row = (id = 'pve-710') => ({ ...store.db.prepare('SELECT * FROM desktops WHERE id=?').get(id) });
  const snapshot = () => JSON.stringify({ assignments: store.db.prepare('SELECT * FROM assignments ORDER BY user_id,desktop_id').all(), hosts: store.db.prepare('SELECT * FROM hosts ORDER BY id').all() });
  const relocations = () => store.db.prepare("SELECT actor,subject FROM audit_events WHERE event='desktop_vm_relocated' ORDER BY id").all().map(value => ({ ...value }));
  // The administrator's Proxmox refresh (admin-http.mjs /proxmox/sync).
  const adminSync = async () => {
    const read = await readRegisteredInventory({ proxmox: api, relocation, store, config, revision: store.setting('proxmox'), targets: store.registeredVmTargets(), force: true });
    store.refreshRegisteredVms(read.targets, read.vms, actor, () => {}, now);
    return read;
  };
  return { store, vault, config, actor, user, host, hostToken, pve, api, relocation, power, row, snapshot, relocations, adminSync,
    advance: ms => { now += ms; }, refresh: () => power.refresh({ maxAgeMs: 0 }) };
}

function assertKept(f, before) {
  assert.equal(f.snapshot(), before, 'assignments, permissions and Host registration are kept');
  assert.equal(f.store.authenticateHost(f.hostToken)?.id, f.host.hostId);
  assert.equal(f.store.assignedDesktop(f.user, 'pve-710')?.id, 'pve-710');
}

test('migration to another node: the desktop follows with one listing and one config read', async t => {
  const f = fixture(t), before = f.snapshot();
  await f.refresh(); assert.equal(f.row().pve_power_status, 'running');
  f.pve.vms.get(710).node = 'node-b'; f.advance(5000);
  await f.refresh();
  assert.equal(f.row().pve_node, 'node-b'); assert.equal(f.row().pve_vmid, 710);
  assert.equal(f.row().pve_power_status, 'running'); assert.equal(f.row().pve_power_error, null); assert.equal(f.row().pve_available, 1);
  assert.equal(f.row().name, 'VM 710'); assert.equal(f.row().description, 'Proxmox VM 710');
  assertKept(f, before);
  assert.deepEqual(f.relocations(), [{ actor: 'server', subject: 'pve-710:node-a:710:node-b:710' }]);
  assert.equal(f.pve.listings(), 1); assert.equal(f.pve.reads(711, 'node-a'), 0, 'no cluster-wide config scan for the same VM ID');
  assert.deepEqual(f.store.registeredVmTargets().map(({ node, vmid }) => ({ node, vmid })), [{ node: 'node-b', vmid: 710 }]);
  // Power works at the new place.
  const status = await f.power.probe('pve-710'); assert.equal(status.node, 'node-b');
});

test('HA-like move from a node that is down, through the administrator refresh', async t => {
  const f = fixture(t), before = f.snapshot();
  f.pve.down.add('node-a'); f.pve.vms.get(710).node = 'node-c';
  const read = await f.adminSync();
  assert.deepEqual(read.relocations, [{ desktopId: 'pve-710', state: 'moved', from: { node: 'node-a', vmid: 710 }, to: { node: 'node-c', vmid: 710 } }]);
  assert.equal(f.row().pve_node, 'node-c'); assert.equal(f.row().pve_power_status, 'running'); assert.equal(f.row().pve_power_error, null);
  assertKept(f, before);
  assert.deepEqual(f.relocations(), [{ actor: 'server', subject: 'pve-710:node-a:710:node-c:710' }]);
});

test('backup restored to a new VM ID: same row, assignments and Host; texts the server wrote follow the number', async t => {
  const f = fixture(t), before = f.snapshot();
  f.store.db.prepare("UPDATE desktops SET name='Proxmox name' WHERE id='pve-710'").run();
  f.pve.vms.delete(710); f.pve.vms.set(725, { node: 'node-b', uuid: UUID, name: 'restored' });
  await f.refresh();
  assert.equal(f.row().pve_vmid, 725); assert.equal(f.row().pve_node, 'node-b'); assert.equal(f.row().id, 'pve-710');
  assert.equal(f.row().name, 'Proxmox name', 'a name from Proxmox or the administrator is kept');
  assert.equal(f.row().description, 'Proxmox VM 725');
  assert.equal(f.row().pve_power_status, 'running'); assert.equal(f.row().pve_available, 1);
  assertKept(f, before);
  assert.deepEqual(f.relocations(), [{ actor: 'server', subject: 'pve-710:node-a:710:node-b:725' }]);
  assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM desktops WHERE pve_vmid=725').get().n, 1);
});

test('duplicate UUID or a template carrying it is ambiguous: nothing changes', async t => {
  for (const second of [{ node: 'node-a', uuid: UUID, name: 'copy' }, { node: 'node-a', uuid: UUID, name: 'template', template: true }]) {
    const f = fixture(t), before = f.snapshot(), row = f.row();
    f.pve.vms.delete(710); f.pve.vms.set(725, { node: 'node-b', uuid: UUID, name: 'restored' }); f.pve.vms.set(726, second);
    if (second.template) f.pve.vms.delete(725);
    const read = await f.adminSync();
    assert.equal(read.relocations[0].state, 'ambiguous');
    const after = f.row();
    assert.equal(after.pve_power_error, 'proxmox_request_failed');
    for (const key of ['id', 'name', 'description', 'pve_node', 'pve_vmid', 'pve_machine_uuid', 'pve_available']) assert.equal(after[key], row[key], key);
    assert.equal(f.snapshot(), before); assert.deepEqual(f.relocations(), []);
  }
});

test('VM ID reused by another VM: quarantine as before, no lookup', async t => {
  const f = fixture(t);
  f.pve.vms.get(710).uuid = OTHER;
  await f.refresh();
  assert.equal(f.row().pve_available, 0); assert.equal(f.row().pve_power_error, 'proxmox_machine_uuid_mismatch');
  assert.equal(f.store.authenticateHost(f.hostToken), undefined);
  assert.equal(f.pve.listings(), 0); assert.deepEqual(f.relocations(), []);
  // The original also restored elsewhere does not lift it: a mismatch is no missing VM.
  f.pve.vms.set(725, { node: 'node-a', uuid: UUID, name: 'restored' }); f.advance(120000);
  await f.refresh();
  assert.equal(f.row().pve_vmid, 710); assert.equal(f.row().pve_available, 0); assert.equal(f.pve.listings(), 0);
});

test('VM ID now on another node as another VM: replaced, nothing changes', async t => {
  const f = fixture(t), before = f.snapshot();
  f.pve.vms.set(710, { node: 'node-b', uuid: OTHER, name: 'someone else' });
  const read = await f.adminSync();
  assert.equal(read.relocations[0].state, 'replaced');
  assert.equal(f.row().pve_node, 'node-a'); assert.equal(f.row().pve_available, 1);
  assert.equal(f.snapshot(), before); assert.deepEqual(f.relocations(), []);
});

test('the new VM ID already belongs to another desktop row: refused as a conflict', async t => {
  const f = fixture(t);
  f.store.syncProxmox([{ vmid: 725, node: 'node-b', name: 'VM 725', status: 'running' }], f.actor);
  f.pve.vms.delete(710); f.pve.vms.set(725, { node: 'node-b', uuid: UUID, name: 'restored' });
  const read = await f.adminSync();
  assert.equal(read.relocations[0].state, 'conflict');
  assert.equal(f.row().pve_vmid, 710); assert.deepEqual(f.relocations(), []);
});

test('rate limit: once per desktop a minute, and a lookup that found nothing waits longer', async t => {
  const f = fixture(t);
  f.pve.failListing('proxmox_unreachable'); f.pve.vms.get(710).node = 'node-b';
  await f.refresh(); assert.equal(f.pve.listings(), 1);
  for (let n = 0; n < 5; n++) { f.advance(5000); await f.refresh(); }
  assert.equal(f.pve.listings(), 1, 'not again within the minute');
  assert.equal(f.row().pve_node, 'node-a');
  f.advance(60000); f.pve.failListing(null); await f.refresh();
  assert.equal(f.pve.listings(), 2); assert.equal(f.row().pve_node, 'node-b');
  // Gone everywhere: a full lookup finds nothing and is not repeated every minute.
  f.pve.vms.delete(710); f.advance(5000); await f.refresh();
  assert.equal(f.pve.listings(), 2, 'the move itself was the lookup of this minute');
  f.advance(60000); await f.refresh();
  assert.equal(f.pve.listings(), 3);
  f.advance(65000); await f.refresh(); assert.equal(f.pve.listings(), 3, 'not_found waits retryMs');
  await f.adminSync(); assert.equal(f.pve.listings(), 4, 'the administrator refresh waits the minute only');
  await f.adminSync(); assert.equal(f.pve.listings(), 4);
});

test('no recorded Proxmox UUID: no lookup', async t => {
  const f = fixture(t);
  f.store.db.prepare("UPDATE desktops SET pve_machine_uuid=NULL WHERE id='pve-710'").run();
  f.pve.vms.get(710).node = 'node-b';
  await f.refresh(); await f.adminSync();
  assert.equal(f.pve.listings(), 0); assert.equal(f.row().pve_node, 'node-a'); assert.deepEqual(f.relocations(), []);
});

test('a slow lookup does not hold the refresh past its wait and is picked up by the next refresh', async t => {
  const f = fixture(t);
  f.power.relocationWaitMs = 50; let release;
  f.pve.holdListing(new Promise(resolve => { release = resolve; }));
  f.pve.vms.delete(710); f.pve.vms.set(725, { node: 'node-a', uuid: UUID, name: 'restored' });
  await f.refresh();
  assert.equal(f.row().pve_power_error, 'proxmox_request_failed'); assert.equal(f.row().pve_vmid, 710);
  const running = f.relocation.running.promise; release(); await running;
  assert.equal(f.row().pve_vmid, 725);
  await f.refresh();
  assert.equal(f.row().pve_power_status, 'running'); assert.equal(f.row().pve_power_error, null);
});

test('Console follows a moved VM before its first snapshot', async t => {
  const f = fixture(t), token = newToken();
  f.store.createSession(f.store.findUser('assigned'), tokenHash(token), Date.now(), 3600000, 'client');
  f.pve.vms.get(710).node = 'node-b';
  const relay = new Relay({ store: f.store, vault: f.vault, proxmox: f.api, publicOrigin: 'https://broker.invalid', authorizationMs: 3600000, heartbeatMs: 3600000 });
  t.after(() => relay.close());
  relay.vmRelocation = f.relocation;
  const grant = await relay.console.create(f.store.findUser('assigned'), tokenHash(token), 'pve-710');
  assert.ok(grant.id);
  const value = relay.console.grants.get(grant.id);
  assert.equal(value.node, 'node-b'); assert.equal(value.snapshot.node, 'node-b');
  assert.equal(f.row().pve_node, 'node-b');
  assert.deepEqual(f.relocations(), [{ actor: 'server', subject: 'pve-710:node-a:710:node-b:710' }]);
  relay.console.cancel(f.store.findUser('assigned'), tokenHash(token), 'pve-710', grant.id);
});

test('stream console proof follows a moved VM within its wait', async t => {
  for (const move of [pve => { pve.vms.get(710).node = 'node-b'; }, pve => { pve.vms.delete(710); pve.vms.set(725, { node: 'node-a', uuid: UUID, name: 'restored' }); }]) {
    const f = fixture(t);
    move(f.pve);
    const host = f.store.hostConsoleBinding(f.hostToken);
    const session = { id: 'session-1', userId: f.user, desktopId: 'pve-710', mode: 'stream', ws: { readyState: 1 },
      host: { tokenHash: f.hostToken, identity: { id: host.id }, capabilities: { streamProfileProtocol: streamConsoleProfileProtocol } } };
    const relay = { store: f.store, proxmox: f.api, vmRelocation: f.relocation, config: () => f.config, displayBusy: new Set(),
      current: () => true, authorized: () => true, powerBusy: () => false, consoleProofReadMs: 5000 };
    const result = await streamConsoleProof(relay, session);
    assert.deepEqual(result.proof, { protocol: streamConsoleProfileProtocol, sessionId: 'session-1', machineUuid: UUID, model: 'std' });
    assert.equal(f.relocations().length, 1);
  }
});

test('the administrator activity log names the relocation with both places', () => {
  assert.equal(eventLabel('desktop_vm_relocated'), '데스크톱 VM 위치 변경');
  const entry = auditEntry({ id: 1, created_at: 0, event: 'desktop_vm_relocated', actor: 'server', subject: 'pve-710:node-a:710:node-b:725' }, new Map(), new Map([['pve-710', {}]]));
  assert.equal(entry.desktopId, 'pve-710');
  assert.deepEqual(entry.detail, ['node-a 노드 VM 710 → node-b 노드 VM 725']);
});
