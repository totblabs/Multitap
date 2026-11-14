import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes, randomUUID } from 'node:crypto';
import { once } from 'node:events';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';

// Local PC power (pc-power-v1) through the real broker routes and a fake Host
// channel. No Windows call runs here and nothing is ever restarted: the
// "Host" is this test's WebSocket, and Wake-on-LAN goes to a recording stub.
const origin = 'https://pc-power.invalid';
const pcUuid = 'a1234567-89ab-cdef-8123-0123456789ab';
const pcId = 'pc-' + pcUuid;
const mac = 'D8:BB:C1:12:34:56';
const caps = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true, monitors: [{ id: '0', width: 1920, height: 1080, primary: true }] };

async function fixture(t, { wakeMs = 240000, replyMs = 15000, sendWake } = {}) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const tokens = {};
  for (const [name, role] of [['alice', 'user'], ['bob', 'user'], ['carol', 'user']]) {
    store.addUser({ name, passwordHash: 'fixture', role });
    tokens[name] = newToken(); store.createSession(store.findUser(name), tokenHash(tokens[name]), Date.now(), 3600000, 'client');
  }
  store.syncProxmox([{ vmid: 149, node: 'fixture', name: 'VM', status: 'running' }], 'fixture');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve.invalid', tokenId: 'fixture@pve!fixture', secret: 'fixture' }), 'fixture');
  // An approved physical PC exactly as local-pc.mjs records one.
  const hostToken = newToken(), hostId = randomUUID(), now = Date.now();
  store.db.prepare("INSERT INTO desktops(id,name,kind,description,os,enabled,created_at) VALUES(?,?,'pc','fixture','Windows',1,?)").run(pcId, 'TOWER-PC', now);
  store.db.prepare('INSERT INTO hosts(id,desktop_id,token_hash,hostname,machine_uuid,version,created_at) VALUES(?,?,?,?,?,?,?)').run(hostId, pcId, tokenHash(hostToken), 'TOWER-PC', pcUuid, '0.2.146', now);
  store.db.prepare('INSERT INTO local_pc_approvals(desktop_id,host_id,machine_uuid,approved_at,actor) VALUES(?,?,?,?,?)').run(pcId, hostId, pcUuid, now, 'fixture');
  store.replaceVmAssignments('alice', [pcId], 'fixture'); store.replaceVmAssignments('bob', [pcId], 'fixture');
  const woken = [];
  const server = await buildServer({ store, vault, publicOrigin: origin, proxmox: { inventory: async () => [{ vmid: 149, node: 'fixture', name: 'VM', status: 'running' }] },
    relayOptions: { authorizationMs: 20, heartbeatMs: 1000 }, pcPowerOptions: { tickMs: 10, wakeMs, replyMs, sendWake: sendWake ?? (async macs => { woken.push(macs); return macs.length; }) }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  const base = `http://127.0.0.1:${server.address().port}`;
  const client = async (as, method, path, body) => {
    const response = await fetch(base + '/api/v1' + path, { method, headers: { Authorization: 'Bearer ' + tokens[as], ...(body !== undefined ? { 'Content-Type': 'application/json' } : {}) }, body: body === undefined ? undefined : JSON.stringify(body) });
    const text = await response.text();
    return { status: response.status, body: JSON.parse(text), text };
  };
  async function host({ hello = { type: 'host.power.hello', protocol: 'pc-power-v1', actions: ['shutdown', 'reboot'], wake: [{ mac: mac.replaceAll(':', '-').toLowerCase(), wakeEnabled: true, magicPacket: null }] } } = {}) {
    const ws = new WebSocket(base.replace('http:', 'ws:') + '/api/v1/hosts/channel', { headers: { Authorization: 'Host ' + hostToken, 'X-Multitap-Host-Protocol': 'capabilities-v1' }, perMessageDeflate: false });
    await once(ws, 'open');
    const queue = [], waiting = [];
    ws.on('message', data => { const value = JSON.parse(data.toString()); const index = waiting.findIndex(w => w.predicate(value)); if (index < 0) queue.push(value); else { const w = waiting.splice(index, 1)[0]; clearTimeout(w.timer); w.resolve(value); } });
    ws.json = value => ws.send(JSON.stringify(value));
    ws.take = (predicate, ms = 2000) => { const index = queue.findIndex(predicate); if (index >= 0) return Promise.resolve(queue.splice(index, 1)[0]); return new Promise((resolve, reject) => { waiting.push({ predicate, resolve, timer: setTimeout(() => reject(new Error('message_timeout')), ms) }); }); };
    ws.none = async (predicate, ms = 150) => { await new Promise(resolve => setTimeout(resolve, ms)); assert.equal(queue.some(predicate), false); };
    if (hello) ws.json(hello);
    ws.json({ type: 'host.capabilities', capabilities: caps }); await ws.take(v => v.type === 'host.capabilities.ack');
    t.after(() => ws.terminate());
    return ws;
  }
  const pc = async (as = 'alice') => (await client(as, 'GET', '/desktops')).body.desktops.find(d => d.id === pcId);
  const until = async (read, predicate, label) => { for (let i = 0; i < 300; i++) { const value = await read(); if (predicate(value)) return value; await new Promise(resolve => setTimeout(resolve, 10)); } assert.fail('Timed out waiting for ' + label); };
  const close = async ws => { ws.close(); await until(pc, d => d?.host?.connected === false, 'Host offline'); };
  return { store, client, host, pc, until, close, woken };
}

test('A Host before pc-power-v1 is offered no PC power and receives no request', async t => {
  const f = await fixture(t), legacy = await f.host({ hello: null });
  const desktop = await f.pc();
  assert.deepEqual(desktop.pcPower, { protocol: 'pc-power-v1', actions: [], wake: false, busy: false });
  assert.equal(Object.hasOwn(desktop, 'powerState'), false);
  const refused = await f.client('alice', 'POST', `/desktops/${pcId}/power`, { action: 'reboot' });
  assert.equal(refused.status, 409); assert.equal(refused.body.error.code, 'pc_power_unsupported');
  await legacy.none(v => v.type === 'host.power.request');
  assert.ok(f.store.auditLog().some(row => row.event === 'power_refused' && row.subject === `${pcId}:reboot:pc_power_unsupported`));
});

test('다시 시작 goes to the assigned PC Host once, its acceptance ends the task, and no client sees the MAC', async t => {
  const f = await fixture(t), host = await f.host();
  const listed = await f.until(f.pc, d => d.pcPower.actions.length === 2, 'announced actions');
  assert.deepEqual(listed.pcPower, { protocol: 'pc-power-v1', actions: ['shutdown', 'reboot'], wake: true, busy: false });
  const listing = await f.client('alice', 'GET', '/desktops');
  assert.doesNotMatch(listing.text, /D8[:-]BB|d8[:-]bb/i);
  // Not assigned: the ordinary 404, nothing reaches the Host.
  assert.equal((await f.client('carol', 'POST', `/desktops/${pcId}/power`, { action: 'reboot' })).status, 404);
  assert.equal((await f.client('alice', 'POST', `/desktops/${pcId}/power`, { action: 'stop' })).body.error.code, 'invalid_power_action');
  const submitted = await f.client('alice', 'POST', `/desktops/${pcId}/power`, { action: 'reboot' });
  assert.equal(submitted.status, 202);
  assert.equal(submitted.body.powerTask.status, 'submitted'); assert.equal(submitted.body.powerBusy, true); assert.equal(submitted.body.canForceStop, false);
  assert.doesNotMatch(submitted.text, /D8[:-]BB/i);
  const request = await host.take(v => v.type === 'host.power.request');
  assert.deepEqual(Object.keys(request).sort(), ['action', 'requestId', 'type']);
  assert.deepEqual({ action: request.action, requestId: request.requestId }, { action: 'reboot', requestId: submitted.body.powerTask.id });
  // A second action while one is open is refused before the Host.
  const again = await f.client('bob', 'POST', `/desktops/${pcId}/power`, { action: 'shutdown' });
  assert.equal(again.status, 409); assert.equal(again.body.error.code, 'power_in_progress');
  await host.none(v => v.type === 'host.power.request');
  // Another account cannot read this account's task.
  assert.equal((await f.client('bob', 'GET', `/desktops/${pcId}/power/tasks/${request.requestId}`)).status, 404);
  host.json({ type: 'host.power.result', requestId: request.requestId, state: 'accepted', code: '' });
  const done = await f.until(async () => (await f.client('alice', 'GET', `/desktops/${pcId}/power/tasks/${request.requestId}`)).body, v => v.powerTask.status === 'stopped', 'accepted');
  assert.equal(done.powerTask.exitStatus, 'OK'); assert.equal(done.powerTask.action, 'reboot'); assert.equal(done.powerBusy, false);
  const audit = f.store.auditLog().map(row => `${row.event}:${row.subject}`);
  assert.ok(audit.includes(`power_submitted:${pcId}:reboot`)); assert.ok(audit.includes(`pc_power_accepted:${pcId}:reboot`));
});

test('A Host refusal reaches the client as its fixed code; a silent Host ends as unknown', async t => {
  const f = await fixture(t, { replyMs: 100 }), host = await f.host();
  await f.until(f.pc, d => d.pcPower.actions.length === 2, 'announced actions');
  const first = await f.client('alice', 'POST', `/desktops/${pcId}/power`, { action: 'shutdown' });
  const request = await host.take(v => v.type === 'host.power.request');
  // Codes outside the fixed list are ignored (the reply timeout then decides).
  host.json({ type: 'host.power.result', requestId: request.requestId, state: 'refused', code: 'Access is denied.' });
  host.json({ type: 'host.power.result', requestId: request.requestId, state: 'refused', code: 'machine_locked' });
  const refused = await f.until(async () => (await f.client('alice', 'GET', `/desktops/${pcId}/power/tasks/${first.body.powerTask.id}`)).body, v => v.powerTask.status !== 'submitted', 'refusal');
  assert.deepEqual([refused.powerTask.status, refused.powerTask.exitStatus], ['stopped', 'machine_locked']);
  assert.ok(f.store.auditLog().some(row => row.event === 'pc_power_refused' && row.subject === `${pcId}:shutdown:machine_locked`));
  const second = await f.client('alice', 'POST', `/desktops/${pcId}/power`, { action: 'reboot' });
  await host.take(v => v.type === 'host.power.request');
  const silent = await f.until(async () => (await f.client('alice', 'GET', `/desktops/${pcId}/power/tasks/${second.body.powerTask.id}`)).body, v => v.powerTask.status !== 'submitted', 'reply timeout');
  assert.deepEqual([silent.powerTask.status, silent.powerTask.exitStatus], ['unknown', 'host_no_response']);
});

test('켜기 sends Wake-on-LAN to the stored MAC of an offline PC and ends when its Host connects', async t => {
  const f = await fixture(t), host = await f.host();
  await f.until(f.pc, d => d.pcPower.wake === true, 'wake report');
  // Online: no 켜기.
  assert.equal((await f.client('alice', 'POST', `/desktops/${pcId}/power`, { action: 'start' })).body.error.code, 'invalid_power_state');
  await f.close(host);
  const offline = await f.pc();
  assert.deepEqual(offline.pcPower.actions, ['start']);
  assert.equal((await f.client('alice', 'POST', `/desktops/${pcId}/power`, { action: 'reboot' })).body.error.code, 'invalid_power_state');
  const started = await f.client('alice', 'POST', `/desktops/${pcId}/power`, { action: 'start' });
  assert.equal(started.status, 202); assert.equal(started.body.powerTask.status, 'running');
  assert.deepEqual(f.woken, [[mac]]);
  assert.ok(f.store.auditLog().some(row => row.event === 'pc_wake_sent' && row.subject === pcId));
  await f.host();
  const online = await f.until(async () => (await f.client('alice', 'GET', `/desktops/${pcId}/power/tasks/${started.body.powerTask.id}`)).body, v => v.powerTask.status === 'stopped', 'online');
  assert.equal(online.powerTask.exitStatus, 'OK'); assert.equal(online.power.state, 'running');
  assert.ok(f.store.auditLog().some(row => row.event === 'pc_wake_online' && row.subject === pcId));
});

test('A PC that does not come back ends as wake_timeout; a PC without a wired adapter cannot be woken', async t => {
  const f = await fixture(t, { wakeMs: 100 });
  await f.close(await f.host());
  const started = await f.client('alice', 'POST', `/desktops/${pcId}/power`, { action: 'start' });
  const expired = await f.until(async () => (await f.client('alice', 'GET', `/desktops/${pcId}/power/tasks/${started.body.powerTask.id}`)).body, v => v.powerTask.status === 'stopped', 'timeout');
  assert.equal(expired.powerTask.exitStatus, 'wake_timeout');
  assert.ok(f.store.auditLog().some(row => row.event === 'pc_wake_timeout'));
  // The administrator sees the stored report; the latest report replaces it.
  const inventory = f.store.inventory().desktops.find(d => d.id === pcId);
  assert.deepEqual(inventory.wake.adapters, [{ mac, wakeEnabled: true, magicPacket: null }]);
  await f.close(await f.host({ hello: { type: 'host.power.hello', protocol: 'pc-power-v1', actions: ['shutdown', 'reboot'], wake: [] } }));
  assert.deepEqual((await f.pc()).pcPower, { protocol: 'pc-power-v1', actions: [], wake: false, busy: false, task: { ...expired.powerTask } });
  const none = await f.client('alice', 'POST', `/desktops/${pcId}/power`, { action: 'start' });
  assert.equal(none.status, 409); assert.equal(none.body.error.code, 'pc_wake_unavailable');
  assert.equal(f.woken.length, 1);
});

test('켜기 that could not send a packet ends as wake_send_failed and records why (a PC wake failure, not a VM power failure)', async t => {
  const f = await fixture(t, { sendWake: async () => { throw Object.assign(new Error('wake_send_failed'), { reason: 'EPERM' }); } });
  await f.close(await f.host());
  const started = await f.client('alice', 'POST', `/desktops/${pcId}/power`, { action: 'start' });
  assert.equal(started.body.powerTask.status, 'stopped');
  assert.equal(started.body.powerTask.exitStatus, 'wake_send_failed');
  const rows = f.store.auditLog().map(row => [row.event, row.subject]);
  assert.ok(rows.some(([event, subject]) => event === 'pc_wake_failed' && subject === `${pcId}:EPERM`), JSON.stringify(rows));
  assert.ok(!rows.some(([event]) => event === 'power_failed' || event === 'pc_wake_sent'));
});

test('A Host that could not read its adapters keeps the last wake report', async t => {
  const f = await fixture(t);
  await f.close(await f.host());
  await f.close(await f.host({ hello: { type: 'host.power.hello', protocol: 'pc-power-v1', actions: ['shutdown', 'reboot'] } }));
  assert.deepEqual(f.store.inventory().desktops.find(d => d.id === pcId).wake.adapters, [{ mac, wakeEnabled: true, magicPacket: null }]);
  assert.deepEqual((await f.pc()).pcPower.actions, ['start']);
});

test('Malformed wake reports store nothing', async t => {
  const f = await fixture(t);
  for (const wake of [[{ mac: '01:00:5E:00:00:01' }], [{ mac: '00:00:00:00:00:00' }], [{ mac: mac, wakeEnabled: 'yes' }], [{ mac, extra: 1 }], Array(9).fill({ mac })]) {
    await f.close(await f.host({ hello: { type: 'host.power.hello', protocol: 'pc-power-v1', actions: ['reboot'], wake } }));
    assert.equal(f.store.db.prepare('SELECT count(*) n FROM pc_wake').get().n, 0, JSON.stringify(wake));
  }
});
