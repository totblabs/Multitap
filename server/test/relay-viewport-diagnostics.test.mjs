import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';

const reasons = ['', 'no_interactive_session', 'interactive_console_required', 'administrator_required', 'viewport_probe_pending', 'viewport_probe_failed', 'viewport_probe_timeout', 'viewport_support_unavailable', 'viewport_recovery_required', 'viewport_settings_unavailable', 'viewport_settings_untrusted', 'viewport_settings_read_failed', 'viewport_original_settings_invalid', 'viewport_original_settings_modified', 'viewport_settings_modified_externally', 'viewport_package_path_invalid', 'viewport_package_path_untrusted', 'viewport_package_not_administrator_controlled', 'viewport_installed_driver_unavailable', 'viewport_installed_driver_untrusted', 'viewport_installed_driver_mismatch', 'viewport_sources_unavailable', 'viewport_connector_identity_unavailable', 'viewport_connector_identity_ambiguous', 'viewport_ccd_query_failed', 'viewport_ccd_source_invalid', 'viewport_ccd_snapshot_limit', 'viewport_topology_changed', 'viewport_ccd_validate_failed', 'monitor_not_available', 'driver_hash_mismatch', 'driver_signature_untrusted', 'driver_catalog_membership_invalid'];
reasons.push('host_maintenance', 'mode_catalog_update_required', 'mode_catalog_failed', 'mode_catalog_timeout', 'mode_catalog_cancelled', 'mode_catalog_registration_timeout', 'mode_catalog_not_verified', 'viewport_rollback_incomplete', 'reboot_required', 'existing_virtual_display_requires_review', 'shared_vendor_path_conflict', 'owned_virtual_output_inactive', 'owned_virtual_display_not_ready', 'viewport_busy', 'owned_device_restart_failed', 'owned_device_restart_status_unavailable');
const nativeReasons = ['native_display_not_ready', 'native_display_provider_unavailable', 'native_display_snapshot_invalid', 'native_display_configuration_untrusted', 'native_display_topology_mismatch', 'native_display_change_pending', 'native_topology_not_ready', 'native_topology_recovery_required'];
reasons.push(...nativeReasons);
const capabilities = () => ({ service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true, dynamicResolution: false, viewportProvisioning: false,
  monitors: [{ id: '0', width: 1920, height: 1080, primary: true, modes: [{ width: 1920, height: 1080, refreshRate: 60 }] }] });
async function fixture(t) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32)), tokens = {};
  for (const name of ['admin', 'alice', 'bobby']) {
    store.addUser({ name, passwordHash: 'isolated-diagnostic-fixture', role: name === 'admin' ? 'admin' : 'user' });
    tokens[name] = newToken(); store.createSession(store.findUser(name), tokenHash(tokens[name]), Date.now(), 3600000, name === 'admin' ? 'admin' : 'client');
  }
  const vms = [610, 611].map(vmid => ({ vmid, node: 'fixture', name: 'VM ' + vmid, status: 'running' }));
  store.syncProxmox(vms, 'fixture'); store.assign('alice', 'pve-610'); store.assign('bobby', 'pve-611');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve-fixture.example', tokenId: 'test@pve!fixture', secret: 'isolated-fixture-only' }), 'fixture');
  const machineUuid = '00000000-1234-1234-1234-000000000001', code = newToken(), hostToken = newToken();
  store.issueEnrollment('pve-610', machineUuid, tokenHash(code), 'fixture');
  store.consumeEnrollment(tokenHash(code), machineUuid, { hostname: 'Viewport diagnostic fixture', version: '0.2.12', tokenHash: tokenHash(hostToken) });
  const server = await buildServer({ store, vault, publicOrigin: 'https://viewport-fixture.example', proxmox: { inventory: async () => vms }, relayOptions: { authorizationMs: 20, heartbeatMs: 10000 }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  const base = `http://127.0.0.1:${server.address().port}`;
  async function get(as = 'alice') {
    const admin = as === 'admin';
    const response = await fetch(base + '/api/v1/' + (admin ? 'admin/inventory' : 'desktops'), { headers: admin ? { Cookie: '__Secure-multitap-admin=' + tokens.admin } : as ? { Authorization: 'Bearer ' + tokens[as] } : {} });
    return { status: response.status, body: await response.json() };
  }
  const ws = new WebSocket(base.replace('http:', 'ws:') + '/api/v1/hosts/channel', { headers: { Authorization: 'Host ' + hostToken, 'X-Multitap-Host-Protocol': 'capabilities-v1' }, perMessageDeflate: false });
  await once(ws, 'open');
  const queue = [], waiters = [];
  ws.on('message', (bytes, binary) => {
    assert.equal(binary, false); const value = JSON.parse(bytes.toString()); const at = waiters.findIndex(waiter => waiter.predicate(value));
    if (at < 0) queue.push(value); else { const waiter = waiters.splice(at, 1)[0]; clearTimeout(waiter.timer); waiter.resolve(value); }
  });
  function take(predicate) {
    const at = queue.findIndex(predicate); if (at >= 0) return Promise.resolve(queue.splice(at, 1)[0]);
    return new Promise((resolve, reject) => { const waiter = { predicate, resolve, timer: setTimeout(() => reject(new Error('fixture_message_timeout')), 2000) }; waiters.push(waiter); });
  }
  async function send(value) {
    const type = value.type === 'host.capabilities' ? 'host.capabilities.ack' : 'host.heartbeat.ack';
    const reply = take(packet => packet.type === type); ws.send(JSON.stringify(value)); return reply;
  }
  async function advertise(caps) { const reply = await send({ type: 'host.capabilities', capabilities: caps }); assert.equal(reply.type, 'host.capabilities.ack'); return reply.accepted; }
  async function desktop(as = 'alice') { const response = await get(as); assert.equal(response.status, 200); return response.body.desktops.find(value => value.id === 'pve-610'); }
  async function connect(mode) {
    const response = await fetch(base + '/api/v1/desktops/pve-610/connect', { method: 'POST', headers: { Authorization: 'Bearer ' + tokens.alice, 'Content-Type': 'application/json' }, body: JSON.stringify({ mode }) });
    return { status: response.status, body: await response.json() };
  }
  return { store, get, desktop, advertise, send, ws, take, connect, base, tokens };
}

test('Optional viewport diagnostics retain all fixed codes through WS/HTTP and reserve mode blocking for explicit maintenance', async t => {
  const f = await fixture(t), original = capabilities();
  assert.equal((await f.desktop()).canConnect, false); // No capability ACK yet.
  for (const viewportFailureReason of reasons) {
    assert.equal(await f.advertise({ ...original, viewportFailureReason }), true);
    const current = await f.desktop();
    assert.equal(current.host.capabilities.viewportFailureReason, viewportFailureReason);
    const available = viewportFailureReason !== 'host_maintenance';
    assert.equal(current.canConnect, available); assert.deepEqual(current.connectionModes, { stream: available });
    assert.equal(current.host.capabilities.dynamicResolution, false); assert.equal(current.host.capabilities.viewportProvisioning, false);
    assert.deepEqual(current.host.capabilities.monitors, original.monitors);
  }
  assert.equal((await f.desktop('admin')).host.capabilities.viewportFailureReason, reasons.at(-1));
  assert.equal(await f.desktop('bobby'), undefined); assert.equal((await f.get(null)).status, 401);
});

test('Unknown or malformed viewport diagnostics are omitted while the rest of a valid advertisement remains usable', async t => {
  const f = await fixture(t), original = capabilities();
  const hostile = ['unknown_future_code', 'PRIVATE_HOST_TOKEN=https://private.invalid/password', 'viewport_ccd_validate_failed\n', 'x'.repeat(5000), null, 3, true, {}, [], { toString: 'viewport_recovery_required' }];
  for (const viewportFailureReason of hostile) {
    assert.equal(await f.advertise({ ...original, viewportFailureReason: 'viewport_recovery_required' }), true);
    assert.equal(await f.advertise({ ...original, viewportFailureReason, hostToken: 'PRIVATE_HOST_TOKEN' }), true);
    const current = await f.desktop(), caps = current.host.capabilities;
    assert.equal(Object.hasOwn(caps, 'viewportFailureReason'), false); assert.equal(Object.hasOwn(caps, 'hostToken'), false);
    assert.equal(current.canConnect, true); assert.deepEqual(current.connectionModes, { stream: true });
    assert.deepEqual(caps.monitors, original.monitors); assert.equal(JSON.stringify(current).includes('PRIVATE_HOST_TOKEN'), false);
    assert.equal(JSON.stringify((await f.get('admin')).body).includes('private.invalid'), false);
  }
});

test('Unready native outputs retain their cause for the assigned client and admin without granting streaming', async t => {
  const f = await fixture(t);
  for (const viewportFailureReason of ['reboot_required', ...nativeReasons]) {
    assert.equal(await f.advertise({ ...capabilities(), streaming: false, virtualDisplayReady: false, monitors: [], viewportFailureReason }), true);
    for (const as of ['alice', 'admin']) {
      const current = await f.desktop(as);
      assert.equal(current.host.capabilities.viewportFailureReason, viewportFailureReason);
      // Nothing remains connectable.
      assert.deepEqual(current.connectionModes, { stream: false });
      assert.deepEqual(current.host.capabilities.monitors, []);
    }
    assert.equal(await f.desktop('bobby'), undefined);
    const rejected = await f.connect('stream');
    assert.equal(rejected.status, 409);
    assert.equal(rejected.body.error.details.reason, 'virtual_display_required');
  }
  assert.deepEqual(await f.send({ type: 'host.heartbeat', capabilities: capabilities() }), { type: 'host.heartbeat.ack' });
  const ready = await f.desktop();
  assert.equal(Object.hasOwn(ready.host.capabilities, 'viewportFailureReason'), false);
  assert.deepEqual(ready.connectionModes, { stream: true });
});

test('Heartbeat replaces or clears viewport diagnostics and legacy omission does not preserve a stale error', async t => {
  const f = await fixture(t), original = capabilities();
  assert.equal(await f.advertise({ ...original, viewportFailureReason: 'viewport_probe_timeout' }), true);
  assert.deepEqual(await f.send({ type: 'host.heartbeat', capabilities: { ...original, viewportFailureReason: 'viewport_ccd_validate_failed' } }), { type: 'host.heartbeat.ack' });
  assert.equal((await f.desktop()).host.capabilities.viewportFailureReason, 'viewport_ccd_validate_failed');
  assert.deepEqual(await f.send({ type: 'host.heartbeat', capabilities: original }), { type: 'host.heartbeat.ack' });
  assert.equal(Object.hasOwn((await f.desktop()).host.capabilities, 'viewportFailureReason'), false);
  assert.equal(await f.advertise({ ...original, viewportFailureReason: '' }), true);
  assert.equal((await f.desktop()).host.capabilities.viewportFailureReason, '');
});

test('A diagnostic never grants dynamic resolution or bypasses existing capability validation', async t => {
  const f = await fixture(t), original = capabilities();
  assert.equal(await f.advertise({ ...original, viewportFailureReason: 'viewport_support_unavailable' }), true);
  for (const invalid of [{ ...original, streaming: 'true' }, { ...original, dynamicResolution: true }, { ...original, viewportProvisioning: true }]) {
    assert.equal(await f.advertise({ ...invalid, viewportFailureReason: '' }), false);
    assert.equal((await f.desktop()).host.capabilities.viewportFailureReason, 'viewport_support_unavailable');
  }
  const dynamic = { ...original, dynamicResolution: true, viewportProvisioning: true, resizeProtocol: 'epoch-v1', viewportFailureReason: 'viewport_probe_failed' };
  assert.equal(await f.advertise(dynamic), true);
  const current = await f.desktop(); assert.equal(current.canConnect, true); assert.equal(current.host.capabilities.dynamicResolution, true);
  assert.equal(current.host.capabilities.viewportProvisioning, true); assert.deepEqual(current.host.capabilities.monitors, original.monitors);
});

test('Catalog maintenance rejects a queued stream session explicitly while keeping the authenticated host channel reusable', async t => {
  const f = await fixture(t); assert.equal(await f.advertise(capabilities()), true);
  const response = await f.connect('stream'); assert.equal(response.status, 200);
  const start = f.take(packet => packet.type === 'session.start');
  const client = new WebSocket(f.base.replace('http:', 'ws:') + '/api/v1/sessions/' + response.body.sessionId, { headers: { Authorization: 'Bearer ' + f.tokens.alice } });
  await once(client, 'open'); assert.equal((await start).sessionId, response.body.sessionId);
  const received = once(client, 'message'), closed = once(client, 'close');
  f.ws.send(JSON.stringify({ type: 'stream.unavailable', sessionId: response.body.sessionId, reason: 'host_maintenance' }));
  const [bytes] = await received; assert.equal(JSON.parse(bytes.toString()).reason, 'host_maintenance'); await closed;
  assert.equal((await f.desktop()).host.connected, true); assert.equal((await f.desktop()).canConnect, true);
  assert.equal(await f.advertise({ ...capabilities(), viewportFailureReason: 'host_maintenance' }), true);
  const maintenance = await f.desktop();
  assert.equal(maintenance.canConnect, false); assert.deepEqual(maintenance.connectionModes, { stream: false });
  const refused = await f.connect('stream'); assert.equal(refused.status, 409); assert.equal(refused.body.error.details.reason, 'host_maintenance');
});
