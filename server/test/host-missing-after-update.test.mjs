import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes, randomUUID } from 'node:crypto';
import { once } from 'node:events';
import { mkdtemp, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';

// hostMissingAfterUpdate on the desktop list: a Host that went away with the
// desktop's last update attempt and stayed away (an update that ended
// install_failed, after which the Host never came back; without this
// the card only says offline). The "Hosts" are this test's WebSockets.
const origin = 'https://host-missing.invalid';
const machine = '00000000-1234-1234-1234-000000000149';
const pcUuid = 'b1234567-89ab-cdef-8123-0123456789ab', pcId = 'pc-' + pcUuid;
const sha = version => (version.replaceAll('.', '') + 'a'.repeat(64)).slice(0, 64);
const release = version => ({ product: 'host', version, filename: `Multitap-Host-${version}-windows-x64.exe`, size: 17000000 + Number(version.split('.')[2]), sha256: sha(version), changelog: 'Host ' + version, releasedAt: '2026-11-01T00:00:00.000Z' });
const caps = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true, monitors: [{ id: '0', width: 1920, height: 1080, primary: true }] };
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const missingMs = 600;

async function until(predicate, label) {
  for (let i = 0; i < 250; i++) { if (await predicate()) return; await delay(20); }
  assert.fail('timeout: ' + label);
}

async function fixture(t, { hostUpdateOptions = {}, seed } = {}) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const directory = await mkdtemp(join(tmpdir(), 'multitap-host-missing-'));
  await writeFile(join(directory, 'releases.json'), JSON.stringify([release('0.2.145'), release('0.2.146')]));
  store.addUser({ name: 'alice', passwordHash: 'fixture', role: 'user' });
  const token = newToken(); store.createSession(store.findUser('alice'), tokenHash(token), Date.now(), 3600000, 'client');
  const vms = [{ vmid: 149, node: 'fixture', name: 'TESTER', status: 'running' }];
  store.syncProxmox(vms, 'fixture');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve.invalid', tokenId: 'fixture@pve!fixture', secret: 'fixture' }), 'fixture');
  const hostTokens = { 'pve-149': newToken(), [pcId]: newToken() };
  const code = newToken();
  store.issueEnrollment('pve-149', machine, tokenHash(code), 'fixture');
  store.consumeEnrollment(tokenHash(code), machine, { hostname: 'TESTER', version: '0.2.145', tokenHash: tokenHash(hostTokens['pve-149']) });
  // An approved physical PC exactly as local-pc.mjs records one.
  const pcHost = randomUUID(), now = Date.now();
  store.db.prepare("INSERT INTO desktops(id,name,kind,description,os,enabled,created_at) VALUES(?,?,'pc','fixture','Windows',1,?)").run(pcId, 'TOWER-PC', now);
  store.db.prepare('INSERT INTO hosts(id,desktop_id,token_hash,hostname,machine_uuid,version,created_at) VALUES(?,?,?,?,?,?,?)').run(pcHost, pcId, tokenHash(hostTokens[pcId]), 'TOWER-PC', pcUuid, '0.2.145', now);
  store.db.prepare('INSERT INTO local_pc_approvals(desktop_id,host_id,machine_uuid,approved_at,actor) VALUES(?,?,?,?,?)').run(pcId, pcHost, pcUuid, now, 'fixture');
  store.replaceVmAssignments('alice', ['pve-149', pcId], 'fixture');
  seed?.(store);
  const server = await buildServer({ store, vault, publicOrigin: origin, downloadDir: directory, proxmox: { inventory: async () => vms.map(vm => ({ ...vm })) },
    relayOptions: { authorizationMs: 20, heartbeatMs: 1000 }, powerOptions: { cacheMs: 50 }, desktopEventOptions: { tickMs: 20, authorizationMs: 20, heartbeatMs: 1000 },
    hostUpdateOptions: { tickMs: 20, retryMs: 0, catalogMs: 0, missingMs, ...hostUpdateOptions }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const sockets = [];
  t.after(async () => { for (const ws of sockets) ws.terminate(); await new Promise(resolve => server.close(resolve)); store.close(); await rm(directory, { recursive: true, force: true }); });
  const base = `http://127.0.0.1:${server.address().port}`;
  const client = async (method, path, body) => {
    const response = await fetch(base + '/api/v1' + path, { method, headers: { Authorization: 'Bearer ' + token, ...(body !== undefined ? { 'Content-Type': 'application/json' } : {}) }, body: body === undefined ? undefined : JSON.stringify(body) });
    return { status: response.status, body: await response.json() };
  };
  const desktop = async id => (await client('GET', '/desktops')).body.desktops.find(d => d.id === id);
  async function host(id, { version = '0.2.145' } = {}) {
    const ws = new WebSocket(base.replace('http:', 'ws:') + '/api/v1/hosts/channel', { headers: { Authorization: 'Host ' + hostTokens[id], 'X-Multitap-Host-Protocol': 'capabilities-v1' }, perMessageDeflate: false });
    sockets.push(ws);
    await once(ws, 'open');
    const queue = [], waiting = [];
    ws.on('message', data => { const value = JSON.parse(data.toString()); const index = waiting.findIndex(w => w.predicate(value)); if (index < 0) queue.push(value); else { const w = waiting.splice(index, 1)[0]; clearTimeout(w.timer); w.resolve(value); } });
    ws.json = value => ws.send(JSON.stringify(value));
    ws.take = (predicate, ms = 3000) => { const index = queue.findIndex(predicate); if (index >= 0) return Promise.resolve(queue.splice(index, 1)[0]); return new Promise((resolve, reject) => { waiting.push({ predicate, resolve, timer: setTimeout(() => reject(new Error('message_timeout')), ms) }); }); };
    ws.json({ type: 'host.update.hello', protocol: 'host-update-v1', version }); await ws.take(v => v.type === 'host.update.hello.ack');
    if (id === pcId) ws.json({ type: 'host.power.hello', protocol: 'pc-power-v1', actions: ['shutdown', 'reboot'] });
    ws.json({ type: 'host.capabilities', capabilities: caps }); await ws.take(v => v.type === 'host.capabilities.ack');
    return ws;
  }
  // The client starts the update; the Host reports how it ended.
  async function update(ws, id, state = 'failed', code = 'install_failed') {
    const response = await client('POST', `/desktops/${id}/host-update`, { version: '0.2.146' });
    assert.equal(response.status, 202);
    const start = await ws.take(v => v.type === 'host.update.start');
    ws.json({ type: 'host.update.status', requestId: start.requestId, state, code, ...(state === 'completed' ? { version: '0.2.146' } : {}) });
    await ws.take(v => v.type === 'host.update.status.ack');
  }
  // A real Host's heartbeat (about every 10 s) is its last sign of life.
  async function leave(ws) {
    ws.json({ type: 'host.heartbeat', version: '0.2.145' }); await ws.take(v => v.type === 'host.heartbeat.ack');
    ws.close(); await once(ws, 'close');
  }
  // A released client's watch (no X-ToTB-Live): list invalidations only.
  async function watch() {
    const ws = new WebSocket(base.replace('http:', 'ws:') + '/api/v1/events', { headers: { Authorization: 'Bearer ' + token, Origin: origin }, perMessageDeflate: false });
    sockets.push(ws); ws.queue = [];
    ws.on('message', bytes => ws.queue.push(JSON.parse(bytes.toString())));
    await once(ws, 'open');
    await until(() => ws.queue.some(m => m.type === 'desktops.ready'), 'ready');
    return ws;
  }
  return { store, vms, client, desktop, host, update, leave, watch };
}
const changes = ws => ws.queue.filter(m => m.type === 'desktops.changed').length;

test('a Host that leaves after a failed update and stays away is named on the list and pushed; it clears when the Host reconnects', async t => {
  const f = await fixture(t), alice = await f.watch();
  const h = await f.host('pve-149');
  await f.update(h, 'pve-149', 'failed', 'install_failed');
  await f.leave(h);
  const early = await f.desktop('pve-149');
  assert.equal(early.host.connected, false);
  assert.equal(early.vmState, 'host_lost');
  assert.equal(Object.hasOwn(early, 'hostMissingAfterUpdate'), false, 'not before the wait: an ordinary restart may still bring the Host back');
  // The drop itself was pushed; the state appearing is one more push.
  await until(() => changes(alice) > 0, 'drop pushed');
  await delay(100);
  const before = changes(alice);
  await until(() => changes(alice) > before, 'state pushed');
  const later = await f.desktop('pve-149');
  assert.deepEqual(later.hostMissingAfterUpdate, { result: 'failed', code: 'install_failed', fromVersion: '0.2.145', targetVersion: '0.2.146' });
  // Additive for released clients: every other field keeps its value and meaning.
  assert.deepEqual(Object.keys(later).filter(key => key !== 'hostMissingAfterUpdate').sort(), Object.keys(early).sort());
  for (const key of ['status', 'canConnect', 'vmState', 'vmStateReason', 'vmStateRevision', 'host', 'hostUpdate', 'connectionModes']) assert.deepEqual(later[key], early[key], key);
  assert.deepEqual([...new Set(alice.queue.map(m => m.type))].sort(), ['desktops.changed', 'desktops.ready'], 'a released watch gets only what it already knows');
  // The Host returns (restored and reinstalled): the state is gone at once.
  const pushed = changes(alice);
  const back = await f.host('pve-149');
  assert.equal(Object.hasOwn(await f.desktop('pve-149'), 'hostMissingAfterUpdate'), false);
  await until(() => changes(alice) > pushed, 'return pushed');
  back.close();
});

test('a stopped VM is not reported; the same VM running again without its Host is', async t => {
  const f = await fixture(t), h = await f.host('pve-149');
  await f.update(h, 'pve-149', 'failed', 'install_failed');
  await f.leave(h);
  f.vms[0].status = 'stopped';
  await delay(missingMs + 200);
  const off = await f.desktop('pve-149');
  assert.equal(off.vmState, 'off');
  assert.equal(Object.hasOwn(off, 'hostMissingAfterUpdate'), false, 'a VM that is off explains a missing Host');
  f.vms[0].status = 'running';
  await delay(100);
  const on = await f.desktop('pve-149');
  assert.equal(on.vmState, 'host_offline');
  assert.equal(on.hostMissingAfterUpdate?.result, 'failed', 'running again and still no Host');
});

test('a completed update whose Host then disappears is reported for a PC as well', async t => {
  const f = await fixture(t), h = await f.host(pcId);
  await f.update(h, pcId, 'completed', '');
  await f.leave(h);
  assert.equal(Object.hasOwn(await f.desktop(pcId), 'hostMissingAfterUpdate'), false);
  await delay(missingMs + 150);
  assert.deepEqual((await f.desktop(pcId)).hostMissingAfterUpdate, { result: 'completed', code: '', fromVersion: '0.2.145', targetVersion: '0.2.146' });
});

test('a PC shut down through Multitap after the update is off, not missing', async t => {
  const f = await fixture(t), h = await f.host(pcId);
  await f.update(h, pcId, 'failed', 'install_failed');
  const submitted = await f.client('POST', `/desktops/${pcId}/power`, { action: 'shutdown' });
  assert.equal(submitted.status, 202);
  const request = await h.take(v => v.type === 'host.power.request');
  h.json({ type: 'host.power.result', requestId: request.requestId, state: 'accepted', code: '' });
  await until(async () => (await f.desktop(pcId)).pcPower.busy === false, 'shutdown accepted');
  await f.leave(h);
  await delay(missingMs + 200);
  assert.equal(Object.hasOwn(await f.desktop(pcId), 'hostMissingAfterUpdate'), false);
});

test('a refused attempt, or a Host that left long after the attempt, is not the update\'s to explain', async t => {
  const f = await fixture(t, { hostUpdateOptions: { linkMs: 300 } });
  const vm = await f.host('pve-149');
  await f.update(vm, 'pve-149', 'rejected', 'busy');
  await f.leave(vm);
  const pc = await f.host(pcId);
  await f.update(pc, pcId, 'failed', 'install_failed');
  await delay(500); // past linkMs while the Host stays connected
  await f.leave(pc);
  await delay(missingMs + 200);
  assert.equal(Object.hasOwn(await f.desktop('pve-149'), 'hostMissingAfterUpdate'), false, 'rejected: the Host was never asked to install');
  assert.equal(Object.hasOwn(await f.desktop(pcId), 'hostMissingAfterUpdate'), false, 'the Host was heard from well after the attempt');
});

test('the answer survives a broker restart through the audit log', async t => {
  const tenMinutes = 10 * 60000;
  const f = await fixture(t, { seed: store => {
    const at = Date.now() - tenMinutes;
    store.audit('host_update_requested', 'fixture', 'pve-149:0.2.145>0.2.146:client', at - 60000);
    store.audit('host_update_failed', 'fixture', 'pve-149:0.2.145>0.2.146:install_failed', at);
    store.hostSeen(store.db.prepare('SELECT id FROM hosts WHERE desktop_id=?').get('pve-149').id, at + 5000);
    // The PC's last attempt was still under way when the broker stopped: unknown.
    store.audit('host_update_failed', 'fixture', `${pcId}:0.2.144>0.2.145:download_failed`, at - 60000);
    store.audit('host_update_requested', 'fixture', `${pcId}:0.2.145>0.2.146:client`, at);
    store.hostSeen(store.db.prepare('SELECT id FROM hosts WHERE desktop_id=?').get(pcId).id, at + 5000);
  } });
  assert.deepEqual((await f.desktop('pve-149')).hostMissingAfterUpdate, { result: 'failed', code: 'install_failed', fromVersion: '0.2.145', targetVersion: '0.2.146' });
  assert.equal(Object.hasOwn(await f.desktop(pcId), 'hostMissingAfterUpdate'), false);
  // GET /desktops/:id carries the same field.
  assert.equal((await f.client('GET', '/desktops/pve-149')).body.desktop.hostMissingAfterUpdate.result, 'failed');
});
