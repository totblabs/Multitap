import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { mkdtemp, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';

const origin = 'https://host-update.invalid';
const machine = '00000000-1234-1234-1234-000000000149';
const sha = version => (version.replaceAll('.', '') + 'a'.repeat(64)).slice(0, 64);
const release = (version, extra = {}) => ({ product: 'host', version, filename: `Multitap-Host-${version}-windows-x64.exe`, size: 17000000 + Number(version.split('.')[2]), sha256: sha(version), changelog: `Host ${version}\n원격 업데이트`, releasedAt: '2026-10-25T00:00:00.000Z', ...extra });
const caps = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true, monitors: [{ id: '0', width: 1920, height: 1080, primary: true }] };

async function fixture(t, { releases = [release('0.2.58'), release('0.2.60'), release('0.2.59')], hostUpdateOptions = {} } = {}) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const directory = await mkdtemp(join(tmpdir(), 'multitap-host-update-'));
  await writeFile(join(directory, 'releases.json'), JSON.stringify([{ product: 'client', version: '0.4.58', filename: 'Multitap-0.4.58-windows-x64.exe', size: 10, sha256: 'b'.repeat(64) }, ...releases]));
  const tokens = {};
  for (const [name, role, purpose] of [['admin', 'admin', 'admin'], ['owner', 'admin', 'client'], ['alice', 'user', 'client']]) {
    store.addUser({ name, passwordHash: 'fixture', role });
    tokens[name] = newToken(); store.createSession(store.findUser(name), tokenHash(tokens[name]), Date.now(), 3600000, purpose);
  }
  const vms = [{ vmid: 149, node: 'fixture', name: 'TESTER', status: 'running' }, { vmid: 150, node: 'fixture', name: 'SECOND', status: 'running' }];
  store.syncProxmox(vms, 'fixture'); store.assign('alice', 'pve-149'); store.assign('owner', 'pve-149');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve.invalid', tokenId: 'fixture@pve!fixture', secret: 'fixture' }), 'fixture');
  const hostTokens = {};
  for (const [desktop, index] of [['pve-149', 1], ['pve-150', 2]]) {
    const code = newToken(), uuid = machine.slice(0, -1) + index; hostTokens[desktop] = newToken();
    store.issueEnrollment(desktop, uuid, tokenHash(code), 'fixture');
    store.consumeEnrollment(tokenHash(code), uuid, { hostname: 'fixture-' + index, version: '0.2.58', tokenHash: tokenHash(hostTokens[desktop]) });
  }
  const server = await buildServer({ store, vault, publicOrigin: origin, downloadDir: directory, proxmox: { inventory: async () => vms },
    relayOptions: { authorizationMs: 20, heartbeatMs: 1000 }, hostUpdateOptions: { tickMs: 20, retryMs: 0, catalogMs: 0, ...hostUpdateOptions }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); await rm(directory, { recursive: true, force: true }); });
  const base = `http://127.0.0.1:${server.address().port}`;
  const admin = async (method, path, body) => {
    const response = await fetch(base + '/api/v1/admin' + path, { method, headers: { Cookie: '__Secure-multitap-admin=' + tokens.admin, ...(method !== 'GET' ? { Origin: origin, 'X-CSRF-Token': vault.csrf(tokens.admin), 'Content-Type': 'application/json' } : {}) }, body: body === undefined ? undefined : JSON.stringify(body) });
    return { status: response.status, body: await response.json() };
  };
  const client = async (as, method, path, body) => {
    const response = await fetch(base + '/api/v1' + path, { method, headers: { Authorization: 'Bearer ' + tokens[as], ...(body !== undefined ? { 'Content-Type': 'application/json' } : {}) }, body: body === undefined ? undefined : JSON.stringify(body) });
    return { status: response.status, body: await response.json() };
  };
  async function host(desktop = 'pve-149', { hello = true, version = '0.2.58', result, releaseSignature } = {}) {
    const ws = new WebSocket(base.replace('http:', 'ws:') + '/api/v1/hosts/channel', { headers: { Authorization: 'Host ' + hostTokens[desktop], 'X-Multitap-Host-Protocol': 'capabilities-v1' }, perMessageDeflate: false });
    await once(ws, 'open');
    const queue = [], waiting = [];
    ws.on('message', data => { const value = JSON.parse(data.toString()); const index = waiting.findIndex(w => w.predicate(value)); if (index < 0) queue.push(value); else { const w = waiting.splice(index, 1)[0]; clearTimeout(w.timer); w.resolve(value); } });
    ws.json = value => ws.send(JSON.stringify(value));
    ws.take = (predicate = () => true, ms = 2000) => { const index = queue.findIndex(predicate); if (index >= 0) return Promise.resolve(queue.splice(index, 1)[0]); return new Promise((resolve, reject) => { const w = { predicate, resolve, timer: setTimeout(() => reject(new Error('message_timeout')), ms) }; waiting.push(w); }); };
    ws.none = async (predicate, ms = 150) => { await new Promise(resolve => setTimeout(resolve, ms)); assert.equal(queue.some(predicate), false); };
    if (hello) { ws.json({ type: 'host.update.hello', protocol: 'host-update-v1', version, ...(result ? { result } : {}), ...(releaseSignature ? { releaseSignature } : {}) }); const ack = await ws.take(v => v.type === 'host.update.hello.ack'); ws.ack = ack; }
    ws.json({ type: 'host.capabilities', capabilities: caps }); await ws.take(v => v.type === 'host.capabilities.ack');
    return ws;
  }
  const view = async (id = 'pve-149') => (await admin('GET', '/host-updates')).body.desktops[id];
  const until = async (predicate, label) => { for (let i = 0; i < 150; i++) { const value = await view(); if (predicate(value)) return value; await new Promise(resolve => setTimeout(resolve, 10)); } assert.fail('Timed out waiting for ' + label); };
  return { store, server, base, tokens, admin, client, host, view, until };
}

test('Released hosts are left alone and the admin sees the minimum version instead of a command', async t => {
  const f = await fixture(t), legacy = await f.host('pve-149', { hello: false });
  const summary = (await f.admin('GET', '/host-updates')).body;
  assert.equal(summary.latest.version, '0.2.60'); assert.equal(summary.latest.changelog, 'Host 0.2.60\n원격 업데이트');
  assert.deepEqual({ ...summary.desktops['pve-149'], operation: undefined }, { protocol: 'host-update-v1', minimumVersion: '0.2.59', connected: true, supported: false, version: '0.2.58', latestVersion: '0.2.60', newer: true, available: false, sessionActive: false, operation: undefined });
  const refused = await f.admin('POST', '/desktops/pve-149/host-update', { version: '0.2.60' });
  assert.equal(refused.status, 409); assert.equal(refused.body.error.code, 'host_update_unsupported'); assert.deepEqual(refused.body.error.details, { minimumVersion: '0.2.59' });
  await legacy.none(v => v.type === 'host.update.start');
  assert.equal(f.store.auditLog().some(row => row.event === 'host_update_requested'), false);
  const inventory = (await f.admin('GET', '/inventory')).body;
  assert.equal(inventory.hostUpdates.desktops['pve-149'].supported, false); assert.equal(inventory.hostUpdates.desktops['pve-150'].connected, false);
});

test('Admin update sends only catalog metadata over the authenticated channel and reports progress to completion', async t => {
  const f = await fixture(t), host = await f.host();
  assert.equal(host.ack.protocol, 'host-update-v1'); assert.equal(host.ack.result, undefined);
  assert.equal((await f.view()).available, true);
  const response = await f.admin('POST', '/desktops/pve-149/host-update', { version: '0.2.60' });
  assert.equal(response.status, 202); assert.equal(response.body.hostUpdate.operation.status, 'dispatched'); assert.equal(response.body.hostUpdate.operation.fromVersion, '0.2.58');
  const start = await host.take(v => v.type === 'host.update.start');
  assert.deepEqual(Object.keys(start).sort(), ['allowDowngrade', 'disconnectSessions', 'filename', 'path', 'requestId', 'sha256', 'size', 'type', 'version']);
  assert.deepEqual({ version: start.version, filename: start.filename, size: start.size, sha256: start.sha256, path: start.path, allowDowngrade: start.allowDowngrade, disconnectSessions: start.disconnectSessions },
    { version: '0.2.60', filename: 'Multitap-Host-0.2.60-windows-x64.exe', size: 17000060, sha256: sha('0.2.60'), path: '/downloads/Multitap-Host-0.2.60-windows-x64.exe', allowDowngrade: false, disconnectSessions: false });
  assert.equal(start.requestId, response.body.hostUpdate.operation.id);
  assert.match(f.store.auditLog().find(row => row.event === 'host_update_requested').subject, /^pve-149:0\.2\.58>0\.2\.60:admin$/);
  // In flight: sessions, display changes and a second request wait.
  assert.equal((await f.client('alice', 'POST', '/desktops/pve-149/connect', {})).body.error.code, 'host_update_in_progress');
  assert.equal((await f.admin('POST', '/desktops/pve-149/host-update', { version: '0.2.60' })).body.error.code, 'host_update_in_progress');
  host.json({ type: 'host.update.status', requestId: start.requestId, state: 'accepted' });
  host.json({ type: 'host.update.status', requestId: start.requestId, state: 'downloading', received: 8500030, total: 17000060 });
  assert.equal((await f.until(v => v.operation.status === 'downloading', 'download')).operation.progress, 50);
  // Unknown codes, foreign fields and other request ids are ignored, never echoed.
  host.json({ type: 'host.update.status', requestId: start.requestId, state: 'failed', code: 'C:\\Users\\secret' });
  host.json({ type: 'host.update.status', requestId: '00000000-0000-4000-8000-000000000000', state: 'failed', code: 'hash_mismatch' });
  host.json({ type: 'host.update.status', requestId: start.requestId, state: 'downloading', received: 99999999, total: 17000060 });
  host.json({ type: 'host.heartbeat', version: '0.2.58' }); await host.take(v => v.type === 'host.heartbeat.ack');
  const steady = await f.view(); assert.equal(steady.operation.status, 'downloading'); assert.equal(steady.operation.progress, 50); assert.equal(JSON.stringify(steady).includes('secret'), false);
  host.json({ type: 'host.update.status', requestId: start.requestId, state: 'verifying' });
  host.json({ type: 'host.update.status', requestId: start.requestId, state: 'installing' });
  await f.until(v => v.operation.status === 'installing', 'installing');
  host.close(); await once(host, 'close');
  assert.equal((await f.view()).operation.status, 'installing');
  // The new service reports the installer result after its restart.
  const returned = await f.host('pve-149', { version: '0.2.60', result: { requestId: start.requestId, state: 'completed', code: '', version: '0.2.60', rebootRequired: false } });
  assert.equal(returned.ack.result, start.requestId);
  const done = await f.until(v => v.operation.status === 'completed', 'completion');
  assert.equal(done.version, '0.2.60'); assert.equal(done.newer, false); assert.equal(done.available, false); assert.equal(done.operation.progress, 100);
  assert.match(f.store.auditLog().find(row => row.event === 'host_update_completed').subject, /^pve-149:0\.2\.58>0\.2\.60:-$/);
  assert.equal((await f.client('alice', 'POST', '/desktops/pve-149/connect', {})).status, 200);
});

test('Never downgrade or reinstall unless the administrator explicitly allows an older catalog release', async t => {
  const f = await fixture(t), host = await f.host('pve-149', { version: '0.2.59' });
  assert.equal((await f.admin('POST', '/desktops/pve-149/host-update', { version: '0.2.59' })).body.error.code, 'host_update_current');
  assert.equal((await f.admin('POST', '/desktops/pve-149/host-update', { version: '0.2.58' })).body.error.code, 'host_update_not_newer');
  assert.equal((await f.admin('POST', '/desktops/pve-149/host-update', { version: '0.2.61' })).body.error.code, 'host_release_unavailable');
  for (const version of ['latest', '0.2', 'https://evil.invalid/x.exe', 7]) assert.equal((await f.admin('POST', '/desktops/pve-149/host-update', { version })).status, 400);
  assert.equal((await f.admin('POST', '/desktops/pve-149/host-update', { version: '0.2.60', path: '/downloads/other.exe' })).status, 400);
  assert.equal((await f.client('owner', 'POST', '/desktops/pve-149/host-update', { version: '0.2.58', allowDowngrade: true })).status, 400);
  await host.none(v => v.type === 'host.update.start');
  const response = await f.admin('POST', '/desktops/pve-149/host-update', { version: '0.2.58', allowDowngrade: true });
  assert.equal(response.status, 202);
  const start = await host.take(v => v.type === 'host.update.start'); assert.equal(start.version, '0.2.58'); assert.equal(start.allowDowngrade, true);
});

test('Windows client: any assigned account, assignment-bound, latest release only and audited', async t => {
  const f = await fixture(t), host = await f.host();
  // An assigned ordinary account sees the same Host update view an administrator does.
  const plain = (await f.client('alice', 'GET', '/desktops')).body.desktops[0];
  assert.equal(plain.hostUpdate.available, true); assert.equal(plain.hostUpdate.version, '0.2.58'); assert.equal(plain.hostUpdate.latestVersion, '0.2.60');
  assert.equal((await f.client('alice', 'GET', '/desktops/pve-149')).body.desktop.hostUpdate.latestVersion, '0.2.60');
  assert.equal((await f.client('owner', 'GET', '/desktops')).body.desktops[0].hostUpdate.latestVersion, '0.2.60');
  assert.equal((await f.client('alice', 'POST', '/desktops/pve-150/host-update', { version: '0.2.60' })).status, 404);
  assert.equal((await f.client('alice', 'POST', '/desktops/pve-149/host-update', { version: '0.2.59' })).body.error.code, 'host_release_unavailable');
  assert.equal((await f.client('alice', 'POST', '/desktops/pve-149/host-update', { version: '0.2.60', allowDowngrade: true })).status, 400);
  assert.equal((await f.client('alice', 'GET', '/desktops/pve-149/host-update')).status, 405);
  await host.none(v => v.type === 'host.update.start');
  const accepted = await f.client('alice', 'POST', '/desktops/pve-149/host-update', { version: '0.2.60' });
  assert.equal(accepted.status, 202); assert.equal(accepted.body.hostUpdate.operation.status, 'dispatched');
  await host.take(v => v.type === 'host.update.start');
  const audit = f.store.auditLog().find(row => row.event === 'host_update_requested');
  assert.equal(audit.actor, f.store.findUser('alice').id); assert.match(audit.subject, /:client$/);
  // The admin cookie session cannot be replayed as a client bearer token.
  const response = await fetch(f.base + '/api/v1/desktops/pve-149/host-update', { method: 'POST', headers: { Authorization: 'Bearer ' + f.tokens.admin, 'Content-Type': 'application/json' }, body: '{"version":"0.2.60"}' });
  assert.equal(response.status, 401);
});

test('Open-source 1.0.0: a Host on 0.2.155 is offered 1.0.0 as the latest release and an assigned account installs it', async t => {
  const f = await fixture(t, { releases: [release('0.2.154'), release('1.0.0'), release('0.2.155')] }), host = await f.host('pve-149', { version: '0.2.155' });
  const plain = (await f.client('alice', 'GET', '/desktops')).body.desktops[0];
  assert.equal(plain.hostUpdate.version, '0.2.155'); assert.equal(plain.hostUpdate.latestVersion, '1.0.0'); assert.equal(plain.hostUpdate.available, true);
  assert.equal((await f.admin('GET', '/host-updates')).body.latest.version, '1.0.0');
  assert.equal((await f.client('alice', 'POST', '/desktops/pve-149/host-update', { version: '0.2.155' })).body.error.code, 'host_release_unavailable');
  const accepted = await f.client('alice', 'POST', '/desktops/pve-149/host-update', { version: '1.0.0' });
  assert.equal(accepted.status, 202); assert.equal(accepted.body.hostUpdate.operation.fromVersion, '0.2.155'); assert.equal(accepted.body.hostUpdate.operation.targetVersion, '1.0.0');
  const start = await host.take(v => v.type === 'host.update.start');
  assert.deepEqual({ version: start.version, filename: start.filename, path: start.path }, { version: '1.0.0', filename: 'Multitap-Host-1.0.0-windows-x64.exe', path: '/downloads/Multitap-Host-1.0.0-windows-x64.exe' });
  // Once the Host runs 1.0.0 nothing older is offered; a step back stays an administrator's explicit downgrade.
  host.json({ type: 'host.update.status', requestId: start.requestId, state: 'accepted' });
  host.json({ type: 'host.update.status', requestId: start.requestId, state: 'installing' });
  await f.until(v => v.operation?.status === 'installing', 'installing');
  host.close(); await once(host, 'close');
  const returned = await f.host('pve-149', { version: '1.0.0', result: { requestId: start.requestId, state: 'completed', code: '', version: '1.0.0', rebootRequired: false } });
  assert.equal(returned.ack.result, start.requestId);
  const done = await f.until(v => v.operation?.status === 'completed', 'completion');
  assert.equal(done.version, '1.0.0'); assert.equal(done.newer, false); assert.equal(done.available, false);
  assert.equal((await f.admin('POST', '/desktops/pve-149/host-update', { version: '0.2.155' })).body.error.code, 'host_update_not_newer');
});

test('An ordinary account cannot disconnect someone else\'s session for an update', async t => {
  const f = await fixture(t), host = await f.host();
  const connect = await f.client('owner', 'POST', '/desktops/pve-149/connect', {}); assert.equal(connect.status, 200);
  const viewer = new WebSocket(connect.body.webSocketUrl.replace(origin.replace('https:', 'wss:'), f.base.replace('http:', 'ws:')), { headers: { Authorization: 'Bearer ' + f.tokens.owner } });
  await once(viewer, 'open'); await host.take(v => v.type === 'session.start');
  const refused = await f.client('alice', 'POST', '/desktops/pve-149/host-update', { version: '0.2.60', disconnectSessions: true });
  assert.equal(refused.status, 409); assert.equal(refused.body.error.code, 'desktop_busy'); assert.equal(refused.body.error.details.reason, 'other_user');
  const waiting = await f.client('alice', 'POST', '/desktops/pve-149/host-update', { version: '0.2.60' });
  assert.equal(waiting.body.hostUpdate.operation.status, 'waiting'); assert.equal(waiting.body.hostUpdate.operation.code, 'session_active');
  await host.none(v => v.type === 'host.update.start', 120);
  viewer.close(); await host.take(v => v.type === 'session.stop');
  await host.take(v => v.type === 'host.update.start');
});

test('A session another account opens while the catalog is read is not disconnected by an ordinary account', async t => {
  // The other-user check used to run only before the catalog await; the
  // disconnect ran after it and ended whatever session existed by then.
  let gate = Promise.resolve(), reads = 0;
  const latest = { product: 'host', format: 'installer', version: '0.2.60', filename: 'Multitap-Host-0.2.60-windows-x64.exe', url: '/downloads/Multitap-Host-0.2.60-windows-x64.exe', size: 17000060, sha256: sha('0.2.60') };
  const f = await fixture(t, { hostUpdateOptions: { catalog: async () => { reads++; await gate; return [latest]; } } }), host = await f.host();
  let open; gate = new Promise(resolve => { open = resolve; });
  const before = reads;
  const pending = f.client('alice', 'POST', '/desktops/pve-149/host-update', { version: '0.2.60', disconnectSessions: true });
  while (reads === before) await new Promise(resolve => setTimeout(resolve, 5));
  const connect = await f.client('owner', 'POST', '/desktops/pve-149/connect', {}); assert.equal(connect.status, 200);
  const viewer = new WebSocket(connect.body.webSocketUrl.replace(origin.replace('https:', 'wss:'), f.base.replace('http:', 'ws:')), { headers: { Authorization: 'Bearer ' + f.tokens.owner } });
  await once(viewer, 'open'); await host.take(v => v.type === 'session.start');
  open();
  const refused = await pending;
  assert.equal(refused.status, 409); assert.equal(refused.body.error.code, 'desktop_busy'); assert.equal(refused.body.error.details.reason, 'other_user');
  await host.none(v => v.type === 'session.stop' || v.type === 'host.update.start', 120);
  assert.equal(viewer.readyState, WebSocket.OPEN);
  viewer.close();
});

for (const [change, expectedStatus] of [['disabled', 401], ['logout', 401], ['credentials', 401], ['unassigned', 404], ['admin-logout', 401]]) {
  test(`Host update rechecks ${change} after catalog I/O before accepting or dispatching`, { timeout: 10000 }, async t => {
    let gate = Promise.resolve(), reads = 0, open;
    t.after(() => open?.());
    const latest = { ...release('0.2.60'), format: 'installer', url: '/downloads/Multitap-Host-0.2.60-windows-x64.exe' };
    const f = await fixture(t, { hostUpdateOptions: { catalog: async () => { reads++; await gate; return [latest]; } } }), host = await f.host();
    gate = new Promise(resolve => { open = resolve; });
    const before = reads;
    const pending = change === 'admin-logout'
      ? f.admin('POST', '/desktops/pve-149/host-update', { version: '0.2.60' })
      : f.client('alice', 'POST', '/desktops/pve-149/host-update', { version: '0.2.60' });
    const deadline = Date.now() + 3000;
    while (reads === before && Date.now() < deadline) await new Promise(resolve => setTimeout(resolve, 5));
    assert.notEqual(reads, before, 'The request must reach catalog I/O');
    const id = f.store.findUser('alice').id;
    if (change === 'disabled') f.store.db.prepare('UPDATE users SET disabled=1 WHERE id=?').run(id);
    if (change === 'logout') f.store.logout(tokenHash(f.tokens.alice));
    if (change === 'credentials') f.store.db.prepare('UPDATE users SET must_change_credentials=1 WHERE id=?').run(id);
    if (change === 'unassigned') f.store.db.prepare('DELETE FROM assignments WHERE user_id=? AND desktop_id=?').run(id, 'pve-149');
    if (change === 'admin-logout') f.store.logout(tokenHash(f.tokens.admin));
    open();
    assert.equal((await pending).status, expectedStatus);
    await host.none(v => v.type === 'host.update.start' || v.type === 'session.stop');
    assert.equal(f.store.auditLog().some(row => row.event === 'host_update_requested'), false);
  });
}

test('A client administrator demoted during catalog I/O cannot disconnect another account', { timeout: 10000 }, async t => {
  let gate = Promise.resolve(), reads = 0, open;
  t.after(() => open?.());
  const latest = { ...release('0.2.60'), format: 'installer', url: '/downloads/Multitap-Host-0.2.60-windows-x64.exe' };
  const f = await fixture(t, { hostUpdateOptions: { catalog: async () => { reads++; await gate; return [latest]; } } }), host = await f.host();
  const connect = await f.client('alice', 'POST', '/desktops/pve-149/connect', {});
  assert.equal(connect.status, 200);
  const viewer = new WebSocket(connect.body.webSocketUrl.replace(origin.replace('https:', 'wss:'), f.base.replace('http:', 'ws:')), { headers: { Authorization: 'Bearer ' + f.tokens.alice } });
  await once(viewer, 'open'); await host.take(v => v.type === 'session.start');
  gate = new Promise(resolve => { open = resolve; });
  const before = reads;
  const pending = f.client('owner', 'POST', '/desktops/pve-149/host-update', { version: '0.2.60', disconnectSessions: true });
  const deadline = Date.now() + 3000;
  while (reads === before && Date.now() < deadline) await new Promise(resolve => setTimeout(resolve, 5));
  assert.notEqual(reads, before, 'The request must reach catalog I/O');
  f.store.db.prepare("UPDATE users SET role='user' WHERE id=?").run(f.store.findUser('owner').id);
  open();
  const refused = await pending;
  assert.equal(refused.status, 409); assert.equal(refused.body.error.code, 'desktop_busy');
  await host.none(v => v.type === 'host.update.start' || v.type === 'session.stop');
  assert.equal(viewer.readyState, WebSocket.OPEN);
  viewer.close();
});

test('An active session defers the update until it ends, or an explicit choice disconnects it now', async t => {
  const f = await fixture(t), host = await f.host();
  const connect = await f.client('alice', 'POST', '/desktops/pve-149/connect', {}); assert.equal(connect.status, 200);
  const viewer = new WebSocket(connect.body.webSocketUrl.replace(origin.replace('https:', 'wss:'), f.base.replace('http:', 'ws:')), { headers: { Authorization: 'Bearer ' + f.tokens.alice } });
  await once(viewer, 'open'); await host.take(v => v.type === 'session.start');
  assert.equal((await f.view()).sessionActive, true);
  const deferred = await f.admin('POST', '/desktops/pve-149/host-update', { version: '0.2.60' });
  assert.equal(deferred.body.hostUpdate.operation.status, 'waiting'); assert.equal(deferred.body.hostUpdate.operation.code, 'session_active');
  await host.none(v => v.type === 'host.update.start', 120);
  viewer.close(); await host.take(v => v.type === 'session.stop');
  const start = await host.take(v => v.type === 'host.update.start');
  assert.equal(start.disconnectSessions, false);
  // A host-side deferral returns the operation to waiting rather than failing.
  host.json({ type: 'host.update.status', requestId: start.requestId, state: 'deferred', code: 'session_active' });
  const again = await host.take(v => v.type === 'host.update.start' && v.requestId === start.requestId);
  host.json({ type: 'host.update.status', requestId: again.requestId, state: 'failed', code: 'hash_mismatch' });
  const failed = await f.until(v => v.operation.status === 'failed', 'failure');
  assert.equal(failed.operation.code, 'hash_mismatch'); assert.equal(failed.available, true);
  assert.match(f.store.auditLog().find(row => row.event === 'host_update_failed').subject, /:hash_mismatch$/);
  // Now with an active session and the explicit disconnect choice.
  const second = await f.client('alice', 'POST', '/desktops/pve-149/connect', {});
  const viewer2 = new WebSocket(second.body.webSocketUrl.replace(origin.replace('https:', 'wss:'), f.base.replace('http:', 'ws:')), { headers: { Authorization: 'Bearer ' + f.tokens.alice } });
  await once(viewer2, 'open'); await host.take(v => v.type === 'session.start');
  const ended = new Promise(resolve => viewer2.on('message', data => { const value = JSON.parse(data.toString()); if (value.type === 'session.ended') resolve(value); }));
  const now = await f.client('owner', 'POST', '/desktops/pve-149/host-update', { version: '0.2.60', disconnectSessions: true });
  assert.equal(now.status, 202);
  assert.equal((await ended).reason, 'host_update');
  const forced = await host.take(v => v.type === 'host.update.start' && v.requestId !== start.requestId);
  assert.equal(forced.disconnectSessions, true);
});

test('A waiting update that the Host received another way completes by its version instead of a same_version rejection', async t => {
  // Production: a client request waited for an open session, the Host
  // installer was run inside that session, and the dispatched request then
  // came back "rejected: same_version" although the Host had been updated.
  const f = await fixture(t), host = await f.host();
  const connect = await f.client('alice', 'POST', '/desktops/pve-149/connect', {}); assert.equal(connect.status, 200);
  const viewer = new WebSocket(connect.body.webSocketUrl.replace(origin.replace('https:', 'wss:'), f.base.replace('http:', 'ws:')), { headers: { Authorization: 'Bearer ' + f.tokens.alice } });
  await once(viewer, 'open'); await host.take(v => v.type === 'session.start');
  const waiting = await f.client('alice', 'POST', '/desktops/pve-149/host-update', { version: '0.2.60' });
  assert.equal(waiting.status, 202); assert.equal(waiting.body.hostUpdate.operation.status, 'waiting');
  host.close(); await once(host, 'close');
  const returned = await f.host('pve-149', { version: '0.2.60' });
  const done = await f.until(v => v.operation.status !== 'waiting', 'resolution');
  assert.equal(done.operation.status, 'completed'); assert.equal(done.operation.code, 'result_unreported'); assert.equal(done.version, '0.2.60');
  await returned.none(v => v.type === 'host.update.start', 150);
  assert.match(f.store.auditLog().find(row => row.event === 'host_update_completed').subject, /^pve-149:0\.2\.58>0\.2\.60:result_unreported$/);
  assert.equal(f.store.auditLog().some(row => row.event === 'host_update_rejected'), false);
  viewer.close();
});

test('A waiting update older than what the Host now runs is closed as not newer, never dispatched', async t => {
  const f = await fixture(t, { releases: [release('0.2.59'), release('0.2.60'), release('0.2.61')] }), host = await f.host('pve-149', { version: '0.2.59' });
  const connect = await f.client('alice', 'POST', '/desktops/pve-149/connect', {}); assert.equal(connect.status, 200);
  const viewer = new WebSocket(connect.body.webSocketUrl.replace(origin.replace('https:', 'wss:'), f.base.replace('http:', 'ws:')), { headers: { Authorization: 'Bearer ' + f.tokens.alice } });
  await once(viewer, 'open'); await host.take(v => v.type === 'session.start');
  assert.equal((await f.admin('POST', '/desktops/pve-149/host-update', { version: '0.2.60' })).body.hostUpdate.operation.status, 'waiting');
  host.close(); await once(host, 'close');
  const returned = await f.host('pve-149', { version: '0.2.61' });
  const done = await f.until(v => v.operation.status !== 'waiting', 'resolution');
  assert.equal(done.operation.status, 'rejected'); assert.equal(done.operation.code, 'not_newer');
  await returned.none(v => v.type === 'host.update.start', 150);
  viewer.close();
});

test('Silence and lost hosts become an honest unknown state, and a return without a result is judged by its version', async t => {
  const f = await fixture(t, { hostUpdateOptions: { dispatchMs: 80 } }), host = await f.host();
  await f.admin('POST', '/desktops/pve-149/host-update', { version: '0.2.60' }); await host.take(v => v.type === 'host.update.start');
  const silent = await f.until(v => v.operation.status === 'unknown', 'no response');
  assert.equal(silent.operation.code, 'host_no_response'); assert.equal(silent.available, true);
  const retry = await f.admin('POST', '/desktops/pve-149/host-update', { version: '0.2.60' }); assert.equal(retry.status, 202);
  const start = await host.take(v => v.type === 'host.update.start');
  host.json({ type: 'host.update.status', requestId: start.requestId, state: 'installing' });
  await f.until(v => v.operation.status === 'installing', 'installing');
  host.close(); await once(host, 'close');
  await f.host('pve-149', { version: '0.2.60' });
  const observed = await f.until(v => v.operation.status === 'completed', 'version evidence');
  assert.equal(observed.operation.code, 'result_unreported');
});

test('A returning host that still runs the old version without a result is not reported as updated', async t => {
  const f = await fixture(t), host = await f.host();
  await f.admin('POST', '/desktops/pve-149/host-update', { version: '0.2.60' });
  const start = await host.take(v => v.type === 'host.update.start');
  host.json({ type: 'host.update.status', requestId: start.requestId, state: 'installing' });
  await f.until(v => v.operation.status === 'installing', 'installing');
  host.close(); await once(host, 'close');
  const rolledBack = await f.host('pve-149', { version: '0.2.58', result: { requestId: start.requestId, state: 'failed', code: 'driver_install_required', version: '0.2.58' } });
  assert.equal(rolledBack.ack.result, start.requestId);
  const failed = await f.until(v => v.operation.status === 'failed', 'manual decision');
  assert.equal(failed.operation.code, 'driver_install_required'); assert.equal(failed.version, '0.2.58');
  // A claimed completion with the old version is not accepted as success.
  await f.admin('POST', '/desktops/pve-149/host-update', { version: '0.2.60' });
  const next = await rolledBack.take(v => v.type === 'host.update.start');
  rolledBack.json({ type: 'host.update.status', requestId: next.requestId, state: 'completed', version: '0.2.58' });
  // A final state is acknowledged so the Host can clear its pending record.
  assert.deepEqual(await rolledBack.take(v => v.type === 'host.update.status.ack'), { type: 'host.update.status.ack', requestId: next.requestId });
  assert.equal((await f.until(v => v.operation.status === 'failed', 'version mismatch')).operation.code, 'install_failed');
  rolledBack.json({ type: 'host.update.status', requestId: next.requestId, state: 'downloading', received: 1, total: 17000060 });
  await rolledBack.none(v => v.type === 'host.update.status.ack');
});

test('A release signature reaches only Hosts that announced release-signature support', async t => {
  const signature = Buffer.alloc(64, 7).toString('base64'), protocol = 'multitap-windows-release-signature-v1';
  const f = await fixture(t, { releases: [release('0.2.58'), release('0.2.60', { signature }), release('0.2.59')] });
  // A released Host (no announcement) keeps exactly the unsigned field set.
  const legacy = await f.host('pve-149');
  assert.equal(legacy.ack.releaseSignature, protocol, 'the ack tells new Hosts this broker knows signature_invalid');
  await f.admin('POST', '/desktops/pve-149/host-update', { version: '0.2.60' });
  const unsigned = await legacy.take(v => v.type === 'host.update.start');
  assert.deepEqual(Object.keys(unsigned).sort(), ['allowDowngrade', 'disconnectSessions', 'filename', 'path', 'requestId', 'sha256', 'size', 'type', 'version']);
  legacy.json({ type: 'host.update.status', requestId: unsigned.requestId, state: 'rejected', code: 'invalid_request' });
  await f.until(v => v.operation.status === 'rejected', 'legacy refusal');
  legacy.close(); await once(legacy, 'close');
  // A signature-aware Host receives the catalog signature and may refuse with its fixed code.
  const aware = await f.host('pve-149', { releaseSignature: protocol });
  await f.admin('POST', '/desktops/pve-149/host-update', { version: '0.2.60' });
  const signed = await aware.take(v => v.type === 'host.update.start');
  assert.equal(signed.signature, signature);
  aware.json({ type: 'host.update.status', requestId: signed.requestId, state: 'rejected', code: 'signature_invalid' });
  const refused = await f.until(v => v.operation.status === 'rejected', 'signature refusal');
  assert.equal(refused.operation.code, 'signature_invalid');
  assert.ok(f.store.auditLog().some(row => row.event === 'host_update_rejected' && /^pve-149:0\.2\.58>0\.2\.60:signature_invalid$/.test(row.subject)));
  // An unsigned catalog release reaches a signature-aware Host without the field (it refuses it itself).
  await f.admin('POST', '/desktops/pve-149/host-update', { version: '0.2.59' });
  const older = await aware.take(v => v.type === 'host.update.start');
  assert.equal(Object.hasOwn(older, 'signature'), false);
});

test('Update all requests every eligible host once and reports the rest', async t => {
  const f = await fixture(t), first = await f.host('pve-149'), second = await f.host('pve-150', { hello: false });
  const response = await f.admin('POST', '/host-updates/all', { version: '0.2.60' });
  assert.equal(response.status, 202);
  assert.deepEqual(response.body.results, [{ desktopId: 'pve-149', started: true, status: 'dispatched' }, { desktopId: 'pve-150', started: false, reason: 'host_update_unsupported' }]);
  await first.take(v => v.type === 'host.update.start'); await second.none(v => v.type === 'host.update.start');
  assert.deepEqual((await f.admin('POST', '/host-updates/all', { version: '0.2.60' })).body.results.map(row => row.reason), ['host_update_in_progress', 'host_update_unsupported']);
  assert.equal((await f.admin('POST', '/host-updates/all', { version: '0.2.60', force: true })).status, 400);
});

test('Host update endpoints stay administrator-only and CSRF protected', async t => {
  const f = await fixture(t); await f.host();
  const noCsrf = await fetch(f.base + '/api/v1/admin/desktops/pve-149/host-update', { method: 'POST', headers: { Cookie: '__Secure-multitap-admin=' + f.tokens.admin, Origin: origin, 'Content-Type': 'application/json' }, body: '{"version":"0.2.60"}' });
  assert.equal(noCsrf.status, 403);
  const userCookie = await fetch(f.base + '/api/v1/admin/host-updates', { headers: { Cookie: '__Secure-multitap-admin=' + f.tokens.alice } });
  assert.equal(userCookie.status, 401);
  assert.equal((await f.client('alice', 'GET', '/admin/host-updates')).status, 401);
});

test('confirmed administrator bulk update ends active relay sessions and leaves out a Host without remote updates', async t => {
  const f = await fixture(t), host = await f.host(), released = await f.host('pve-150', { hello: false });
  const connect = await f.client('alice', 'POST', '/desktops/pve-149/connect', {}); assert.equal(connect.status, 200);
  const viewer = new WebSocket(connect.body.webSocketUrl.replace(origin.replace('https:', 'wss:'), f.base.replace('http:', 'ws:')), { headers: { Authorization: 'Bearer ' + f.tokens.alice } });
  await once(viewer, 'open'); await host.take(v => v.type === 'session.start');
  const ended = new Promise(resolve => viewer.on('message', data => { const value = JSON.parse(data.toString()); if (value.type === 'session.ended') resolve(value); }));
  assert.equal((await f.view()).sessionActive, true);
  const noCsrf = await fetch(f.base + '/api/v1/admin/host-updates/all', { method: 'POST', headers: { Cookie: '__Secure-multitap-admin=' + f.tokens.admin, Origin: origin, 'Content-Type': 'application/json' }, body: '{"version":"0.2.60"}' });
  assert.equal(noCsrf.status, 403);
  const user = await fetch(f.base + '/api/v1/admin/host-updates/all', { method: 'POST', headers: { Cookie: '__Secure-multitap-admin=' + f.tokens.alice, Origin: origin, 'Content-Type': 'application/json' }, body: '{"version":"0.2.60"}' });
  assert.equal(user.status, 401);
  await host.none(value => value.type === 'session.stop' || value.type === 'host.update.start', 80);
  assert.equal((await f.view()).sessionActive, true, 'rejected/unconfirmed requests keep the active session');
  const response = await f.admin('POST', '/host-updates/all', { version: '0.2.60' }); assert.equal(response.status, 202);
  assert.equal((await ended).reason, 'host_update');
  const request = await host.take(value => value.type === 'host.update.start'); assert.equal(request.disconnectSessions, true);
  assert.deepEqual(response.body.results.find(row => row.desktopId === 'pve-150'), { desktopId: 'pve-150', started: false, reason: 'host_update_unsupported' });
  await released.none(value => value.type === 'host.update.start' || value.type === 'session.stop');
  viewer.close();
});
