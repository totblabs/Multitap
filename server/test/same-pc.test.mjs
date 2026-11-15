// same-pc-v1 (src/same-pc.mjs): every listed desktop carries hostMachineTag,
// derived from its current Host's SMBIOS UUID, so a Windows app can refuse to
// connect to the desktop whose Host runs on the PC the app runs on. The UUID
// itself never reaches a client. Real loopback HTTP with synthetic identities.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash, randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { HostRequests } from '../src/host-requests.mjs';
import { buildServer } from '../src/http.mjs';
import { hostMachineTag } from '../src/same-pc.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { enrollFixtureHosts } from './fixtures/admin-assignment-input.mjs';

const origin = 'https://same-pc-fixture.example';
const machineUuid = vmid => '12345678-1234-4234-8234-' + String(vmid).padStart(12, '0');
const vm = vmid => ({ vmid, node: 'fixture-node', name: `VM${vmid}`, status: 'running', machineUuid: machineUuid(vmid) });
const pcUuid = 'a1234567-89ab-cdef-8123-0123456789ab';
const tagForm = /^[0-9a-f]{64}$/;
// What a client computes on its own from the desktop id and its PC's UUID.
const clientTag = (id, uuid) => createHash('sha256').update('multitap-same-pc-v1\n' + id + '\n' + uuid.toLowerCase(), 'utf8').digest('hex');

test('The tag is the fixed SHA-256 of the version line, the desktop id and the lowercase UUID', () => {
  // Fixed vectors, also checked by the Windows app's own test (same_pc_test.dart).
  assert.equal(hostMachineTag('pve-710', '12345678-1234-4234-8234-000000000710'), 'ffd750749f61c8f55352ae92c1d64bcf32e10de9b13a8103ca148fcd7868ce9e');
  assert.equal(hostMachineTag('pc-0f8fad5b-d9cb-469f-a165-70867728950e', pcUuid), '432f17dfe8914d7b9aea1a77dac64b677262ead32b58d7e2bf5e5717e6d19c03');
  assert.equal(hostMachineTag('pve-710', '12345678-1234-4234-8234-000000000710'.toUpperCase()), hostMachineTag('pve-710', '12345678-1234-4234-8234-000000000710'), 'letter case of the stored UUID does not matter');
  assert.equal(hostMachineTag('pve-710', 'A1234567-89AB-CDEF-8123-0123456789AB'), clientTag('pve-710', pcUuid));
  assert.notEqual(hostMachineTag('pve-710', pcUuid), hostMachineTag('pve-711', pcUuid), 'bound to the desktop: two desktops never share a tag');
  assert.notEqual(hostMachineTag('pve-710', pcUuid), hostMachineTag('pve-710', machineUuid(710)));
  for (const value of [undefined, null, '', 'not-a-uuid', '{' + pcUuid + '}', pcUuid.replaceAll('-', ''), pcUuid + ' ', 5, {}]) assert.equal(hostMachineTag('pve-710', value), null, 'no tag from ' + JSON.stringify(value));
  for (const value of [undefined, null, '', 5]) assert.equal(hostMachineTag(value, pcUuid), null);
});

async function fixture(t) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32)), sockets = [], errors = [];
  for (const name of ['alice', 'bobby', 'admin']) store.addUser({ name, role: name === 'admin' ? 'admin' : 'user', passwordHash: 'fixture-unused' });
  const inventory = [vm(710), vm(711), vm(712)];
  store.syncProxmox(inventory, 'fixture');
  enrollFixtureHosts(store, ['pve-710', 'pve-711', 'pve-712']);
  store.assign('alice', 'pve-710'); store.assign('alice', 'pve-711'); store.assign('bobby', 'pve-711'); store.assign('bobby', 'pve-712');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://isolated-pve.example', tokenId: 'fixture@pve!samepc', secret: 'isolated-same-pc-token' }), 'fixture');
  const tokens = { alice: newToken(), bobby: newToken() };
  for (const name of Object.keys(tokens)) store.createSession(store.findUser(name), tokenHash(tokens[name]), Date.now(), 3600000, 'client');
  const proxmox = { inventory: async (_config, { targets }) => targets.map(target => ({ ...inventory.find(row => row.vmid === target.vmid) })) };
  const server = await buildServer({ store, vault, proxmox, publicOrigin: origin, vmStateOptions: { syncMs: 2000 }, relayOptions: { heartbeatMs: 10000, authorizationMs: 10000 }, desktopEventOptions: { tickMs: 20, authorizationMs: 20, heartbeatMs: 1000 }, logger: { error: value => errors.push(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { for (const ws of sockets) ws.terminate(); await new Promise(resolve => server.close(resolve)); store.close(); assert.deepEqual(errors, [], 'no request failed unexpectedly'); });
  const base = 'http://127.0.0.1:' + server.address().port;
  const read = async (path, as = 'alice') => {
    const response = await fetch(base + '/api/v1' + path, { headers: { Authorization: 'Bearer ' + tokens[as] } });
    const text = await response.text();
    return { status: response.status, text, body: JSON.parse(text) };
  };
  const open = async as => {
    const ws = new WebSocket(base.replace('http:', 'ws:') + '/api/v1/events', { headers: { Authorization: 'Bearer ' + tokens[as], Origin: origin }, perMessageDeflate: false });
    ws.messages = []; ws.on('message', value => ws.messages.push(JSON.parse(value.toString('utf8')))); ws.on('error', () => {});
    await new Promise((resolve, reject) => { ws.once('open', resolve); ws.once('unexpected-response', (_req, response) => { response.resume(); reject(new Error('upgrade_rejected')); }); });
    sockets.push(ws); return ws;
  };
  const until = async (predicate, message = 'fixture_timeout') => { const end = Date.now() + 3000; while (!predicate()) { assert.ok(Date.now() < end, message); await new Promise(resolve => setTimeout(resolve, 5)); } };
  return { store, read, open, until, user: name => store.findUser(name).id };
}

test('Every listed desktop carries the tag of its own Host; the UUID itself is not sent', async t => {
  const f = await fixture(t);
  const listed = await f.read('/desktops');
  assert.equal(listed.status, 200);
  assert.deepEqual(listed.body.desktops.map(desktop => [desktop.id, desktop.hostMachineTag]), [['pve-710', clientTag('pve-710', machineUuid(710))], ['pve-711', clientTag('pve-711', machineUuid(711))]]);
  for (const desktop of listed.body.desktops) assert.match(desktop.hostMachineTag, tagForm);
  // A client on the PC of pve-710 matches that desktop and no other.
  assert.equal(listed.body.desktops.filter(desktop => desktop.hostMachineTag === clientTag(desktop.id, machineUuid(710))).map(desktop => desktop.id).join(), 'pve-710');
  for (const text of [listed.text, (await f.read('/desktops/pve-710')).text]) {
    for (const vmid of [710, 711, 712]) assert.equal(text.toLowerCase().includes(machineUuid(vmid)), false, 'no Host UUID in the answer');
    assert.equal(/machine_?uuid/i.test(text), false, 'no UUID field in the answer');
  }
  const single = await f.read('/desktops/pve-711');
  assert.equal(single.body.desktop.hostMachineTag, clientTag('pve-711', machineUuid(711)), 'the single desktop read carries it too');
  // Another account sees the same tag for the shared desktop and only its own desktops.
  const other = await f.read('/desktops', 'bobby');
  assert.deepEqual(other.body.desktops.map(desktop => [desktop.id, desktop.hostMachineTag]), [['pve-711', clientTag('pve-711', machineUuid(711))], ['pve-712', clientTag('pve-712', machineUuid(712))]]);
  assert.equal(f.store.listDesktops(f.user('alice')).some(row => 'host_machine_uuid' in row || 'machine_uuid' in row), false, 'the store row carries no UUID either');
});

test('The tag follows the current Host: a UUID stored in capitals gives the same tag and a removed Host leaves the list', async t => {
  const f = await fixture(t);
  const before = (await f.read('/desktops')).body.desktops.find(desktop => desktop.id === 'pve-710').hostMachineTag;
  f.store.db.prepare('UPDATE hosts SET machine_uuid=upper(machine_uuid) WHERE desktop_id=?').run('pve-710');
  f.store.db.prepare('UPDATE desktops SET pve_machine_uuid=upper(pve_machine_uuid) WHERE id=?').run('pve-710');
  assert.equal((await f.read('/desktops')).body.desktops.find(desktop => desktop.id === 'pve-710').hostMachineTag, before);
  // A damaged UUID never becomes a tag (the app then has nothing to match).
  f.store.db.prepare('UPDATE desktops SET pve_machine_uuid=NULL WHERE id=?').run('pve-711');
  f.store.db.prepare("UPDATE hosts SET machine_uuid='damaged' WHERE desktop_id=?").run('pve-711');
  const damaged = f.store.listDesktops(f.user('alice')).find(row => row.id === 'pve-711');
  assert.ok(damaged, 'still listed');
  assert.equal(Object.hasOwn(damaged, 'hostMachineTag'), false);
  // Only a current Host is listed at all, so a revoked one publishes no tag.
  f.store.db.prepare('UPDATE hosts SET revoked=1 WHERE desktop_id=?').run('pve-710');
  assert.equal(f.store.listDesktops(f.user('alice')).some(row => row.id === 'pve-710'), false);
});

test('The live desktop channel starts and stays quiet with the tag in every snapshot', async t => {
  const f = await fixture(t);
  const ws = await f.open('alice');
  await f.until(() => ws.messages.some(value => value.type === 'desktops.ready'), 'the watch became ready');
  await new Promise(resolve => setTimeout(resolve, 150));
  assert.deepEqual(ws.messages.filter(value => value.type === 'desktops.changed'), [], 'an unchanged tag is not a change');
});

test('An approved PC carries the tag of its own Host', async t => {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  t.after(() => store.close());
  const admin = store.addUser({ name: 'pc-admin', role: 'admin', passwordHash: 'fake-no-login' });
  const user = store.addUser({ name: 'pc-user', role: 'user', passwordHash: 'fake-no-login' });
  store.syncProxmox([vm(710)], admin); store.saveSetting('proxmox', vault.seal({ endpoint: 'https://fixture.invalid', tokenId: 'fixture@pve!pc', secret: 'fake-no-real-secret' }), admin);
  const requests = new HostRequests({ store, vault, proxmox: { inventory: async () => [vm(710)] } });
  const input = { requestKey: newToken(), hostname: 'Physical PC', machineUuid: pcUuid, version: '0.2.87' };
  const created = requests.create(input, 'fixture', true);
  const result = await requests.approveLocalPc(created.requestId, { name: 'PC', acknowledgeCode: true, acknowledgeUnlinked: true }, () => admin);
  store.replaceVmAssignments('pc-user', [result.desktopId], admin);
  const [row] = store.listDesktops(user);
  assert.equal(row.id, result.desktopId);
  assert.equal(row.kind, 'pc');
  assert.equal(row.hostMachineTag, clientTag(result.desktopId, pcUuid));
  assert.equal(JSON.stringify(row).includes(pcUuid), false);
});
