import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { PowerControl } from '../src/power.mjs';
import { VmStates, deriveVmState, classifyHostPreparation } from '../src/vm-state.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { enrollFixtureHosts } from './fixtures/admin-assignment-input.mjs';

const config = { endpoint: 'https://pve.example:8006', tokenId: 'fixture@pve!power', secret: 'vm-state-secret', ca: '' };
const ID = 'pve-710';
const machineUuid='12345678-1234-4234-8234-123456789abc';

// A fake Proxmox + Host world the state machine reads; the test script moves it.
function world(over = {}) {
  const w = { clock: 1_800_000_000_000, status: 'stopped', uptime: null, task: { status: 'running', exitStatus: null }, agent: 'down', agentEnabled: true,
    host: false, ready: false, session: false, inventoryReads: 0, statusReads: 0, agentCalls: 0, agentEnabledCalls: 0, ...over };
  w.proxmox = {
    async inventory(_config,{targets}) {assert.deepEqual(targets,[{node:'pve',vmid:710,expectedUuid:machineUuid}]);w.inventoryReads++; return [{ vmid: 710, node: 'pve', name: 'VM 710', status: w.status, uptimeSeconds: w.uptime,machineUuid }]; },
    async power(_c, vmid, action, { beforeSubmit,node,expectedUuid } = {}) {assert.equal(node,'pve');assert.equal(expectedUuid,machineUuid);beforeSubmit?.(); return { node: 'pve', task: `UPID:pve:1:2:3:qm${action}:${vmid}:fixture@pve:` }; },
    async task() { return w.task; },
    async vmStatus() { w.statusReads++; return { status: w.status, paused: false, uptimeSeconds: w.uptime }; },
    async agentEnabled() { w.agentEnabledCalls++; return w.agentEnabled; },
    async agentPing() { w.agentCalls++; return w.agent; },
  };
  w.relay = { hostConnected: () => w.host, hostReady: () => w.host && w.ready, hasSession: () => w.session };
  return w;
}
function machine(w, { script = [], ...extra } = {}) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const uid = store.addUser({ name: 'alice', passwordHash: 'x', role: 'user' });
  store.syncProxmox([{ vmid: 710, status: w.status, name: 'VM 710', node: 'pve',machineUuid }], 'fixture');
  enrollFixtureHosts(store,[ID]);
  store.assign('alice', ID, 'fixture');
  store.saveSetting('proxmox', vault.seal(config), 'fixture');
  const power = new PowerControl({ store, vault, proxmox: w.proxmox, now: () => w.clock });
  const changes = [], seen = [];
  const sleeps = [];
  const states = new VmStates({ store, power, relay: w.relay, now: () => w.clock, onChange: id => changes.push(id),
    sleep: async () => { seen.push(view().vmState); w.clock += 1000; const step = script.shift(); await step?.(); }, ...extra });
  function view() {
    const rows = store.listDesktops(uid).map(d => ({ ...d, host: { connected: w.host }, canConnect: w.host && w.ready }));
    return states.decorate(power.decorate(rows, uid))[0];
  }
  const authorize = () => store.db.prepare('SELECT * FROM desktops WHERE id=?').get(ID);
  return { store, power, states, view, changes, seen, sleeps, uid, authorize,
    async submit(action) { const result = await power.submit(authorize, uid, action); return { result, done: states.powerEvent(ID) }; } };
}
const settled = states => states.filter((s, i) => s !== states[i - 1]);
const base = { now: 1_800_000_000_000, power: 'running', powerStale: false, powerError: null, task: { active: null, last: null }, uptimeSeconds: 99999, host: false, ready: false, session: false, agent: null, droppedAt: null };

test('derive: every state and reason is decided by the four signals', () => {
  const d = patch => deriveVmState({ ...base, ...patch });
  const ok = (action, ago = 1000) => ({ action, status: 'stopped', exit_status: 'OK', checked_at: base.now - ago });
  assert.deepEqual(d({ power: 'stopped' }), { state: 'off', reason: 'proxmox_stopped' });
  assert.equal(d({ power: 'unknown' }).state, 'unknown');
  assert.equal(d({ powerStale: true }).state, 'unknown');
  assert.equal(d({ powerError: 'proxmox_unreachable' }).state, 'unknown');
  assert.equal(d({ power: 'stopped', task: { active: { action: 'start' }, last: null } }).state, 'starting');
  assert.equal(d({ task: { active: { action: 'shutdown' }, last: null } }).state, 'stopping');
  assert.equal(d({ task: { active: { action: 'stop' }, last: null } }).state, 'stopping');
  assert.equal(d({ task: { active: { action: 'reboot' }, last: null } }).state, 'rebooting');
  assert.deepEqual(d({ power: 'stopped', task: { active: null, last: ok('start') } }), { state: 'starting', reason: 'awaiting_proxmox' });
  assert.equal(d({ power: 'running', task: { active: null, last: ok('shutdown') } }).state, 'stopping');
  assert.equal(d({ power: 'stopped', task: { active: null, last: ok('shutdown') } }).state, 'off');
  assert.deepEqual(d({ uptimeSeconds: 5 }), { state: 'windows_starting', reason: 'booting' });
  assert.deepEqual(d({ uptimeSeconds: 5, agent: 'up' }), { state: 'host_wait', reason: 'agent_up' });
  assert.deepEqual(d({ host: true, ready: false }), { state: 'host_wait', reason: 'host_preparing' });
  assert.deepEqual(d({ host: true, ready: true }), { state: 'ready', reason: 'host_ready' });
  assert.deepEqual(d({ host: true, ready: true, session: true }), { state: 'in_use', reason: 'session_active' });
  assert.deepEqual(d({ task: { active: null, last: ok('reboot', 5000) } }), { state: 'windows_starting', reason: 'rebooting' });
  assert.deepEqual(d({ droppedAt: base.now - 2000, agent: 'up' }), { state: 'host_lost', reason: 'host_disconnected_vm_healthy' });
  assert.deepEqual(d({ droppedAt: base.now - 2000, agent: 'down' }), { state: 'windows_starting', reason: 'restarting' });
  assert.deepEqual(d({ droppedAt: base.now - 2000 }), { state: 'host_lost', reason: 'host_disconnected' });
  assert.deepEqual(d({}), { state: 'host_offline', reason: 'host_not_connected' });
  // A failed task never invents a state: the real status and the reason.
  const failed = { action: 'start', status: 'stopped', exit_status: 'ERROR: boom', checked_at: base.now - 1000 };
  assert.deepEqual(d({ power: 'stopped', task: { active: null, last: failed } }), { state: 'off', reason: 'power_task_failed' });
});

test('start from the client: 켜는 중 -> Windows 시작 중 -> 연결 준비 중 -> 연결 가능, one VM probe per step and no inventory reads', async () => {
  const w = world();
  const m = machine(w, { script: [
    () => { w.task = { status: 'stopped', exitStatus: 'OK' }; },
    () => { w.status = 'running'; w.uptime = 3; },
    () => { w.agent = 'up'; },
    () => { w.host = true; m.states.hostEvent(ID, 'connected'); },
    () => { w.ready = true; m.states.hostEvent(ID, 'ready'); },
  ] });
  await m.power.refresh();
  assert.equal(m.view().vmState, 'off');
  const reads = w.inventoryReads;
  const { done } = await m.submit('start');
  assert.equal(m.view().vmState, 'starting');
  await done;
  assert.deepEqual(settled([...m.seen, m.view().vmState]), ['starting', 'windows_starting', 'host_wait', 'ready']);
  assert.equal(w.inventoryReads, reads, 'the fast phase asks one VM, not the inventory');
  assert.ok(w.statusReads >= 4);
  assert.ok(m.changes.length >= 5, 'every transition is pushed');
});

test('shutdown and stop from the client: 종료 중 -> 꺼짐 once Proxmox says stopped and the Host is gone', async () => {
  for (const action of ['shutdown', 'stop']) {
    const w = world({ status: 'running', uptime: 99999, host: true, ready: true });
    const m = machine(w, { script: [
      () => { w.task = { status: 'stopped', exitStatus: 'OK' }; },
      () => { w.status = 'stopped'; w.uptime = null; w.host = false; w.ready = false; m.states.hostEvent(ID, 'disconnected', { unexpected: true }); },
    ] });
    await m.power.refresh();
    assert.equal(m.view().vmState, 'ready');
    const { done } = await m.submit(action);
    assert.equal(m.view().vmState, 'stopping');
    await done;
    assert.deepEqual(settled([...m.seen, m.view().vmState]), ['stopping', 'off'], action);
  }
});

test('reboot from the client: 다시 시작 중 -> Windows 시작 중 -> 연결 준비 중 -> 연결 가능', async () => {
  const w = world({ status: 'running', uptime: 99999, host: true, ready: true });
  const m = machine(w, { script: [
    () => { w.task = { status: 'stopped', exitStatus: 'OK' }; },
    () => { w.host = false; w.ready = false; w.uptime = 2; m.states.hostEvent(ID, 'disconnected', { unexpected: true }); },
    () => { w.agent = 'up'; },
    () => { w.host = true; m.states.hostEvent(ID, 'connected'); },
    () => { w.ready = true; m.states.hostEvent(ID, 'ready'); },
  ] });
  await m.power.refresh();
  const { done } = await m.submit('reboot');
  assert.equal(m.view().vmState, 'rebooting');
  await done;
  assert.deepEqual(settled([...m.seen, m.view().vmState]), ['rebooting', 'windows_starting', 'host_wait', 'ready']);
});

test('shutdown from inside Windows: the unexpected Host drop starts the watch and 꺼짐 is pushed without a refresh', async () => {
  const w = world({ status: 'running', uptime: 99999, host: true, ready: true, agent: 'down' });
  const m = machine(w, { script: [
    () => { /* still shutting down */ },
    () => { w.status = 'stopped'; w.uptime = null; },
  ] });
  await m.power.refresh();
  assert.equal(m.view().vmState, 'ready');
  w.host = false; w.ready = false;
  const done = m.states.hostEvent(ID, 'disconnected', { unexpected: true });
  assert.ok(done, 'an unexplained Host drop opens a bounded watch');
  assert.equal(m.view().vmState, 'host_lost', 'before the agent was asked');
  await done;
  assert.deepEqual(settled([...m.seen, m.view().vmState]), ['host_lost', 'windows_starting', 'off']);
});

test('restart from inside Windows: the Host returns on a running VM and the tile comes back by itself', async () => {
  const w = world({ status: 'running', uptime: 99999, host: true, ready: true, agent: 'up' });
  const m = machine(w, { script: [
    () => { w.agent = 'down'; },
    () => { w.uptime = 4; w.agent = 'up'; },
    () => { w.host = true; w.ready = true; m.states.hostEvent(ID, 'connected'); },
  ] });
  await m.power.refresh();
  w.host = false; w.ready = false;
  const done = m.states.hostEvent(ID, 'disconnected', { unexpected: true });
  await done;
  assert.deepEqual(settled([...m.seen, m.view().vmState]), ['host_lost', 'windows_starting', 'host_wait', 'ready']);
});

test('Host dropped but the VM is healthy (agent answers): Host 연결 끊김, never 꺼짐, until the Host returns', async () => {
  const w = world({ status: 'running', uptime: 99999, host: true, ready: true, agent: 'up' });
  const m = machine(w, { script: [() => {}, () => {}, () => { w.host = true; w.ready = true; m.states.hostEvent(ID, 'connected'); }] });
  await m.power.refresh();
  w.host = false; w.ready = false;
  const done = m.states.hostEvent(ID, 'disconnected', { unexpected: true });
  await done;
  assert.deepEqual(settled([...m.seen, m.view().vmState]), ['host_lost', 'ready']);
  assert.equal(m.view().vmStateReason, 'host_ready');
});

test('a Host revoked or a server stopping is not an unexplained drop', () => {
  const w = world({ status: 'running', uptime: 99999 });
  const m = machine(w);
  assert.equal(m.states.hostEvent(ID, 'disconnected', { reason: 'host_revoked', unexpected: false }), undefined);
  assert.equal(m.states.watches.size, 0);
});

test('a failed power task pushes the failure reason with the real status', async () => {
  const w = world({ task: { status: 'running', exitStatus: null } });
  const m = machine(w, { script: [() => { w.task = { status: 'stopped', exitStatus: 'ERROR: start failed' }; }, () => {}] });
  await m.power.refresh();
  const { done } = await m.submit('start');
  await done;
  const after = m.view();
  assert.equal(after.vmState, 'off');
  assert.equal(after.vmStateReason, 'power_task_failed');
});

test('guest agent: refused identity is not asked again, a VM without the agent is never pinged, neither raises an error', async () => {
  const denied = world({ status: 'running', uptime: 3, agent: 'denied' });
  const a = machine(denied, { script: [() => {}, () => {}, () => {}] });
  await a.power.refresh();
  a.states.powerEvent(ID);
  await a.states.watches.get(ID).done;
  assert.equal(denied.agentCalls, 1, 'one refused ping, then silence');
  assert.equal(a.view().vmState, 'windows_starting');
  assert.equal(a.states.agentRefused(), true, 'the refusal is visible to the admin console');
  a.states.resetAgentDenial();
  assert.equal(a.states.agentRefused(), false, 'saving the Proxmox connection clears it');
  const off = world({ status: 'running', uptime: 3, agentEnabled: false });
  const b = machine(off, { script: [() => {}, () => {}] });
  await b.power.refresh();
  b.states.powerEvent(ID);
  await b.states.watches.get(ID).done;
  assert.equal(off.agentCalls, 0);
  assert.equal(b.view().vmState, 'windows_starting');
});

test('the watch is bounded in time', async () => {
  const w = world({ status: 'running', uptime: 3 });
  const m = machine(w, { watchMs: 5000, script: [] });
  await m.power.refresh();
  m.states.powerEvent(ID);
  await m.states.watches.get(ID).done;
  assert.ok(m.seen.length <= 6, `watched ${m.seen.length} steps`);
  assert.equal(m.states.watches.size, 0);
});

test('changes made outside Multitap arrive through the slow sync as a new state with a larger revision', async () => {
  const w = world();
  const m = machine(w);
  await m.power.refresh();
  const off = m.view();
  assert.equal(off.vmState, 'off');
  w.status = 'running'; w.uptime = 2; w.clock += 60000;
  await m.power.refresh({ maxAgeMs: 45000 });
  const on = m.view();
  assert.equal(on.vmState, 'windows_starting');
  assert.ok(on.vmStateRevision > off.vmStateRevision);
  assert.equal(m.view().vmStateRevision, on.vmStateRevision, 'unchanged state keeps its revision');
});

test('broker restart rebuilds the state from one status read, with a revision above the old one', async () => {
  const w = world({ status: 'running', uptime: 99999, host: true, ready: true });
  const first = machine(w);
  await first.power.refresh();
  const before = first.view();
  w.clock += 5000;
  const second = machine(w);
  await second.power.refresh();
  const after = second.view();
  assert.equal(after.vmState, 'ready');
  assert.ok(after.vmStateRevision > before.vmStateRevision);
});

test('GET /desktops carries vmState, its reason and the revision; the live channel stays within the background sync', async t => {
  const w = world({ status: 'running', uptime: 99999 });
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  store.addUser({ name: 'alice', passwordHash: 'x', role: 'user' });
  const token = newToken();
  store.createSession(store.findUser('alice'), tokenHash(token), Date.now(), 3600000, 'client');
  store.syncProxmox([{ vmid: 710, status: 'running', name: 'VM 710', node: 'pve',machineUuid }], 'fixture');
  enrollFixtureHosts(store,[ID]);
  store.assign('alice', ID, 'fixture');
  store.saveSetting('proxmox', vault.seal(config), 'fixture');
  const server = await buildServer({ store, vault, proxmox: w.proxmox, publicOrigin: 'https://vm-state.example', logger: { error: v => assert.fail(v) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  const response = await fetch(`http://127.0.0.1:${server.address().port}/api/v1/desktops`, { headers: { Authorization: 'Bearer ' + token } });
  const desktop = (await response.json()).desktops[0];
  assert.equal(desktop.vmState, 'host_offline');
  assert.equal(desktop.vmStateReason, 'host_not_connected');
  assert.ok(Number.isSafeInteger(desktop.vmStateRevision) && desktop.vmStateRevision > 0);
});

const connectedDesktop = (caps, extra = {}) => ({ host: { connected: true, capabilities: caps }, ...extra });

test('classifyHostPreparation: boot steps are transient, definite failures and unknown codes are not', () => {
  const c = (caps, extra) => classifyHostPreparation(connectedDesktop({ interactiveSession: false, virtualDisplayReady: false, ...caps }, extra));
  assert.equal(classifyHostPreparation({ host: { connected: false } }), null);
  assert.equal(classifyHostPreparation(connectedDesktop(undefined)), null, 'a legacy Host reports nothing to classify');
  assert.deepEqual(classifyHostPreparation({ host: { connected: true, capabilitiesPending: true } }), { transient: 'session_preparing' });
  assert.deepEqual(c({}), { transient: 'windows_login' }, 'no interactive session yet: auto-logon is running');
  assert.deepEqual(c({ interactiveSession: true }), { transient: 'display_preparing' });
  assert.deepEqual(c({ interactiveSession: true, virtualDisplayReady: true }), { transient: 'session_preparing' });
  assert.deepEqual(c({ viewportFailureReason: 'windows_login_required' }), { transient: 'windows_login' });
  for (const reason of ['owned_virtual_output_inactive', 'native_display_change_pending', 'viewport_sources_unavailable', 'host_maintenance', 'viewport_busy']) assert.deepEqual(c({ viewportFailureReason: reason }), { transient: 'display_preparing' }, reason);
  for (const reason of ['windows_session_unknown', 'no_interactive_session', 'viewport_probe_pending', 'mode_catalog_timeout']) assert.deepEqual(c({ viewportFailureReason: reason }), { transient: 'session_preparing' }, reason);
  for (const reason of ['reboot_required', 'administrator_required', 'viewport_recovery_required', 'native_topology_recovery_required', 'driver_signature_untrusted', 'viewport_installed_driver_unavailable', 'existing_virtual_display_requires_review', 'windows_session_locked', 'rdp_session_active', 'interactive_console_required', 'a_code_from_the_future']) assert.deepEqual(c({ viewportFailureReason: reason }), { transient: null }, reason);
  // The console preparation reason wins while there is no interactive session, as in the clients.
  assert.deepEqual(c({ viewportFailureReason: 'reboot_required' }, { consolePreparation: { supported: true, reason: 'windows_login_required' } }), { transient: 'windows_login' });
  assert.deepEqual(c({ interactiveSession: true, viewportFailureReason: 'reboot_required' }, { consolePreparation: { supported: true, reason: 'windows_login_required' } }), { transient: null });
});

test('derive: preparing only inside the grace window and only for transient reasons', () => {
  const d = patch => deriveVmState({ ...base, host: true, ready: false, hostConnectedAt: base.now - 10000, preparation: { transient: 'windows_login' }, ...patch });
  assert.deepEqual(d({}), { state: 'preparing', reason: 'windows_login' });
  assert.deepEqual(d({ preparation: { transient: 'display_preparing' } }), { state: 'preparing', reason: 'display_preparing' });
  assert.deepEqual(d({ preparation: { transient: 'session_preparing' } }), { state: 'preparing', reason: 'session_preparing' });
  // Just before / at the end of the 3 minute default window.
  assert.equal(d({ hostConnectedAt: base.now - 179999 }).state, 'preparing');
  assert.deepEqual(d({ hostConnectedAt: base.now - 180000 }), { state: 'host_wait', reason: 'host_preparing' });
  assert.equal(d({ hostConnectedAt: base.now - 60000, preparingMs: 30000 }).state, 'host_wait', 'configurable');
  // A definite failure falls back at once, even a second after connecting.
  assert.deepEqual(d({ hostConnectedAt: base.now - 1000, preparation: { transient: null } }), { state: 'host_wait', reason: 'host_preparing' });
  // Unknown connect time (broker restarted under a connected Host) and no report: today's behaviour.
  assert.equal(d({ hostConnectedAt: null }).state, 'host_wait');
  assert.equal(d({ preparation: null }).state, 'host_wait');
  // Ready and in use are never overridden; a Host-less VM never shows it.
  assert.equal(d({ ready: true }).state, 'ready');
  assert.equal(d({ ready: true, session: true }).state, 'in_use');
  assert.equal(d({ host: false }).state, 'host_offline');
});

test('boot: the tile shows progress through login and display steps, warns only after the grace window or for a definite failure', async () => {
  const w = world({ status: 'running', uptime: 99999, host: false, ready: false, agent: 'up' });
  const caps = { interactiveSession: false, virtualDisplayReady: false };
  const m = machine(w);
  await m.power.refresh();
  const view = () => {
    const rows = m.store.listDesktops(m.uid).map(d => ({ ...d, host: { connected: w.host, capabilities: w.host ? { ...caps } : undefined }, canConnect: w.host && w.ready }));
    return m.states.decorate(m.power.decorate(rows, m.uid))[0];
  };
  w.host = true; m.states.hostEvent(ID, 'connected');
  assert.deepEqual([view().vmState, view().vmStateReason], ['preparing', 'windows_login']);
  const first = view().vmStateRevision;
  caps.interactiveSession = true;
  assert.deepEqual([view().vmState, view().vmStateReason], ['preparing', 'display_preparing']);
  assert.ok(view().vmStateRevision > first, 'a changed step is a new revision');
  caps.viewportFailureReason = 'reboot_required';
  assert.deepEqual([view().vmState, view().vmStateReason], ['host_wait', 'host_preparing'], 'reboot required is definite');
  delete caps.viewportFailureReason;
  assert.equal(view().vmState, 'preparing');
  w.clock += 180000; await m.power.refresh();
  assert.equal(view().vmState, 'host_wait', 'grace expired');
  w.host = false; m.states.hostEvent(ID, 'disconnected', { unexpected: true });
  w.host = true; m.states.hostEvent(ID, 'connected');
  assert.equal(view().vmState, 'preparing', 'a reconnect after an unexplained drop gets a new window');
  caps.virtualDisplayReady = true; w.ready = true; m.states.hostEvent(ID, 'ready');
  assert.equal(view().vmState, 'ready');
});
