import { test } from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { activitySnapshot, localActivityRequest } from '../src/activity.mjs';
import * as activityModule from '../src/activity.mjs';

// GET /internal/activity (src/activity.mjs) is what the update tooling on the
// server reads before replacing the broker. It must count exactly what the
// broker holds, never leak identities, and answer only a local caller.
const origin = 'https://activity.invalid';
const caps = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true, diagnosticsSupported: true, monitors: [{ id: '0', width: 1920, height: 1080, primary: true }] };

async function fixture(t, { twoDesktops = false } = {}) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const tokens = {};
  store.addUser({ name: 'admin', passwordHash: 'fixture', role: 'admin' });
  store.addUser({ name: 'alice', passwordHash: 'fixture', role: 'user' });
  for (const [name, purpose] of [['admin', 'admin'], ['alice', 'client']]) { tokens[name] = newToken(); store.createSession(store.findUser(name), tokenHash(tokens[name]), Date.now(), 3600000, purpose); }
  const vms = [{ vmid: 149, node: 'fixture', name: 'TESTER', status: 'running' }];
  if (twoDesktops) vms.push({ vmid: 247, node: 'fixture', name: 'Other fixture', status: 'running' });
  store.syncProxmox(vms, 'fixture'); store.assign('alice', 'pve-149');
  if (twoDesktops) store.assign('alice', 'pve-247');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve.invalid', tokenId: 'fixture@pve!fixture', secret: 'fixture' }), 'fixture');
  const code = newToken(), uuid = '00000000-1234-1234-1234-000000000141', hostToken = newToken();
  store.issueEnrollment('pve-149', uuid, tokenHash(code), 'fixture');
  store.consumeEnrollment(tokenHash(code), uuid, { hostname: 'fixture-1', version: '0.2.51', tokenHash: tokenHash(hostToken) });
  const server = await buildServer({ store, vault, publicOrigin: origin, proxmox: { inventory: async () => vms }, relayOptions: { authorizationMs: 20, heartbeatMs: 1000 }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  const port = server.address().port, base = `http://127.0.0.1:${port}`;
  const raw = (headers = {}, path = '/internal/activity') => new Promise((resolve, reject) => {
    const req = http.request({ host: '127.0.0.1', port, path, method: 'GET', headers: { Host: `127.0.0.1:${port}`, ...headers } }, res => {
      let text = ''; res.setEncoding('utf8'); res.on('data', chunk => text += chunk); res.on('end', () => resolve({ status: res.statusCode, text }));
    });
    req.on('error', reject); req.end();
  });
  const activity = async () => { const r = await raw(); assert.equal(r.status, 200, r.text); return JSON.parse(r.text); };
  const client = async (method, path, body) => {
    const response = await fetch(base + '/api/v1' + path, { method, headers: { Authorization: 'Bearer ' + tokens.alice, ...(body !== undefined ? { 'Content-Type': 'application/json' } : {}) }, body: body === undefined ? undefined : JSON.stringify(body) });
    return { status: response.status, body: await response.json() };
  };
  async function host() {
    const ws = new WebSocket(base.replace('http:', 'ws:') + '/api/v1/hosts/channel', { headers: { Authorization: 'Host ' + hostToken, 'X-Multitap-Host-Protocol': 'capabilities-v1' }, perMessageDeflate: false });
    await once(ws, 'open');
    const replies = [];
    ws.on('message', data => replies.push(JSON.parse(data.toString())));
    ws.reply = async type => { for (let i = 0; i < 200; i++) { const index = replies.findIndex(value => value.type === type); if (index >= 0) return replies.splice(index, 1)[0]; await new Promise(resolve => setTimeout(resolve, 10)); } assert.fail('No ' + type); };
    ws.send(JSON.stringify({ type: 'host.capabilities', capabilities: caps })); await ws.reply('host.capabilities.ack');
    t.after(() => ws.close());
    return ws;
  }
  const live = async () => {
    const ws = new WebSocket(base.replace('http:', 'ws:') + '/api/v1/events', { headers: { Authorization: 'Bearer ' + tokens.alice, Origin: origin }, perMessageDeflate: false });
    await once(ws, 'open'); t.after(() => ws.close()); return ws;
  };
  return { store, server, base, raw, activity, client, host, live, tokens, hostToken, port, wsUrl: url => url.replace(origin.replace('https:', 'wss:'), base.replace('http:', 'ws:')) };
}
const settle = async predicate => { for (let i = 0; i < 200; i++) { if (await predicate()) return; await new Promise(resolve => setTimeout(resolve, 10)); } assert.fail('condition not reached'); };

test('only a local caller without forwarding headers gets the activity report', async t => {
  const f = await fixture(t);
  assert.equal((await f.raw()).status, 200);
  for (const headers of [{ 'X-Forwarded-For': '203.0.113.9' }, { 'X-Real-IP': '203.0.113.9' }, { Forwarded: 'for=203.0.113.9' }, { Via: '1.1 proxy' }, { 'X-Forwarded-Host': 'vdi.example' }, { Host: 'vdi.example' }, { Host: '127.0.0.1.evil.example' }]) {
    const r = await f.raw(headers); assert.equal(r.status, 404, JSON.stringify(headers)); assert.equal(r.text.includes('"sessions"'), false);
  }
  assert.equal((await f.raw({}, '/internal/activity?x=1')).status, 404);
  const post = await fetch(f.base + '/internal/activity', { method: 'POST' }); assert.notEqual(post.status, 200);
  assert.equal(localActivityRequest({ socket: { remoteAddress: '192.0.2.101' }, headers: { host: '127.0.0.1:18480' } }), false, 'a remote peer is refused even with a loopback Host');
  assert.equal(localActivityRequest({ socket: { remoteAddress: '::ffff:127.0.0.1' }, headers: { host: 'localhost:18480' } }), true);
});

test('the report counts sessions, Hosts, live channels and recent activity exactly and leaks no identity', async t => {
  const f = await fixture(t);
  let a = await f.activity();
  assert.equal(a.protocol, 'activity-v1');
  assert.deepEqual(a.sessions, { total: 0, stream: 0, attached: 0, tickets: 0 });
  assert.deepEqual(a.operations, { display: 0, sessionCleanup: 0, hostUpdates: 0, power: 0 });
  assert.equal(a.hosts.connected, 0); assert.equal(a.live.channels, 0);
  assert.ok(a.sinceActivityMs >= 0 && a.sinceActivityMs <= a.uptimeMs, 'a fresh broker reports its own uptime as the quiet time');

  const host = await f.host(); assert.equal((await f.activity()).hosts.connected, 1);
  // A client's live channel is reported but is not a session.
  const live = await f.live(); await settle(async () => (await f.activity()).live.channels === 1);
  a = await f.activity(); assert.equal(a.sessions.total, 0);

  // A ticket counts as a session before the client attaches.
  const connect = await f.client('POST', '/desktops/pve-149/connect', {}); assert.equal(connect.status, 200);
  a = await f.activity(); assert.deepEqual(a.sessions, { total: 1, stream: 1, attached: 0, tickets: 1 });
  assert.ok(a.sinceActivityMs < 5000, 'creating a session is activity');
  const viewer = new WebSocket(f.wsUrl(connect.body.webSocketUrl), { headers: { Authorization: 'Bearer ' + f.tokens.alice } });
  await once(viewer, 'open'); await host.reply('session.start');
  a = await f.activity(); assert.deepEqual(a.sessions, { total: 1, stream: 1, attached: 1, tickets: 0 });
  assert.equal(a.live.channels, 1);

  for (const secret of [f.tokens.alice, f.tokens.admin, f.hostToken, tokenHash(f.hostToken), 'alice', 'pve-149', '00000000-1234', 'fixture-1']) assert.equal(JSON.stringify(a).includes(secret), false, 'no identity in the report');

  // Ending the session leaves only "recent activity" for the quiet window.
  viewer.close(); await host.reply('session.stop');
  await settle(async () => (await f.activity()).sessions.total === 0);
  a = await f.activity();
  assert.equal(a.sessions.total, 0); assert.ok(a.sinceActivityMs < 5000);
  live.close();
});

test('sub-channels and operations are reported from the broker state', async t => {
  const f = await fixture(t);
  const relay = { sessions: new Map(), hosts: new Map(), clipboard: { pairs: new Set([1, 2]), activeTransfers: 1 },
    drive: { pairs: new Set([{ shares: new Map([[1, 1]]), outstanding: new Map([[1, 1], [2, 2]]) }]) }, usb: { pairs: new Set([{ devices: new Map([[1, 1]]) }]) },
    print: { pairs: new Set([{ transfer: {} }, { transfer: null }]) }, displayBusy: new Map([['x', 1]]), sessionStop: { pending: new Map([['y', 1]]) },
    hostUpdate: { operations: new Map([['a', { status: 'installing' }], ['b', { status: 'waiting' }], ['c', { status: 'completed' }]]) }, activityAt: 0 };
  const a = activitySnapshot({ relay, desktopEvents: { clients: new Map([['h', { ws: { readyState: WebSocket.OPEN } }], ['i', { ws: { readyState: WebSocket.CLOSING } }]]) }, store: f.store, now: 100000, startedAt: 40000 });
  assert.deepEqual(a.channels, { clipboard: 2, clipboardTransfers: 1, drive: 1, driveShares: 1, driveRequests: 2, usb: 1, usbDevices: 1, print: 2, printTransfers: 1 });
  assert.deepEqual(a.operations, { display: 1, sessionCleanup: 1, hostUpdates: 1, power: 0 });
  assert.equal(a.live.channels, 1); assert.equal(a.sinceActivityMs, 60000); assert.equal(a.uptimeMs, 60000);
});

test('desktop activity is CLI-local only with one existing exact target and preserves the global route', async t => {
  const f = await fixture(t, { twoDesktops: true });
  const path = '/internal/activity/desktop?desktopId=pve-149';
  const normal = await f.raw({}, path);
  assert.equal(normal.status, 200, normal.text);
  assert.equal(JSON.parse(normal.text).protocol, 'activity-desktop-v1');
  for (const headers of [{ Origin: 'https://other.invalid' }, { Origin: 'null' }, { Origin: '' }, { 'X-Forwarded-For': '127.0.0.1' }, { Forwarded: 'for=127.0.0.1' }, { Via: 'proxy' }, { Host: 'activity.invalid' }, { Host: '127.0.0.1.evil.invalid' }]) {
    const denied = await f.raw(headers, path); assert.equal(denied.status, 404, JSON.stringify(headers));
    assert.equal(denied.text.includes('activity-desktop-v1'), false);
  }
  for (const bad of ['/internal/activity/desktop', '/internal/activity/desktop?desktopId=', path + '&x=1', path + '&desktopId=pve-247', '/internal/activity/desktop?desktopId=pve-404', '/internal/activity/desktop?desktopId=../pve-149']) assert.equal((await f.raw({}, bad)).status, 404, bad);
  assert.notEqual((await fetch(f.base + path, { method: 'POST' })).status, 200);
  assert.equal((await f.raw({ Origin: 'https://other.invalid' })).status, 200, 'existing activity-v1 Origin behavior is unchanged');
  assert.equal(activityModule.localDesktopActivityTarget({ method: 'GET', socket: { remoteAddress: '192.0.2.1' }, headers: { host: '127.0.0.1' } }, new URL('http://localhost' + path)), null);
});

test('actual desktop activity separates another desktop and counts pending and attached tickets without leaking identities', async t => {
  const f = await fixture(t, { twoDesktops: true }), host = await f.host();
  const target = async id => { const response = await f.raw({}, '/internal/activity/desktop?desktopId=' + id); assert.equal(response.status, 200, response.text); return JSON.parse(response.text); };
  const initial = await target('pve-149'); assert.equal(initial.complete, true); assert.equal(initial.host.connected, true); assert.deepEqual(initial.sessions, { total: 0, stream: 0, attached: 0, tickets: 0 });
  const connect = await f.client('POST', '/desktops/pve-149/connect', {}); assert.equal(connect.status, 200);
  const pending = await target('pve-149'); assert.deepEqual(pending.sessions, { total: 1, stream: 1, attached: 0, tickets: 1 });
  const other = await target('pve-247'); assert.equal(other.complete, true); assert.equal(other.host.connected, false); assert.equal(other.sessions.total, 0);
  const viewer = new WebSocket(f.wsUrl(connect.body.webSocketUrl), { headers: { Authorization: 'Bearer ' + f.tokens.alice } });
  await once(viewer, 'open'); await host.reply('session.start');
  assert.deepEqual((await target('pve-149')).sessions, { total: 1, stream: 1, attached: 1, tickets: 0 });
  const global = await f.activity(); assert.deepEqual(global.sessions, { total: 1, stream: 1, attached: 1, tickets: 0 });
  for (const secret of [f.tokens.alice, f.tokens.admin, f.hostToken, tokenHash(f.hostToken), 'alice', 'pve-149', 'fixture-1', '00000000-1234']) assert.equal(JSON.stringify(pending).includes(secret), false);
  viewer.close(); await host.reply('session.stop'); await settle(async () => (await target('pve-149')).sessions.total === 0);
  f.store.db.prepare('UPDATE hosts SET revoked=1').run(); assert.equal((await target('pve-149')).host.connected, false, 'open stale Host socket is not authenticated connectivity');
});

function desktopState(t) {
  const store = new Store(':memory:'); t.after(() => store.close());
  store.syncProxmox([{ vmid: 149, node: 'fixture', name: 'Target', status: 'running' }, { vmid: 247, node: 'fixture', name: 'Other', status: 'running' }], 'fixture');
  const relay = { sessions: new Map(), hosts: new Map(), hostAuthenticated: () => false,
    clipboard: { pairs: new Set(), activeTransfers: 0 }, drive: { pairs: new Set() }, usb: { pairs: new Set() }, print: { pairs: new Set() },
    displayBusy: new Map(), console: { grants: new Map() }, sessionStop: { pending: new Map() }, hostUpdate: { operations: new Map() },
    pcPower: { busy: () => false }, accessPending: () => false };
  const vmAudio = { operations: new Map(), uncertain: new Set() };
  return { store, relay, vmAudio, now: 123456789 };
}

test('desktop activity retains every scoped owner, retired console, uncertain and waiting operation', t => {
  const f = desktopState(t), a = { desktopId: 'pve-149' }, b = { desktopId: 'pve-247' };
  f.relay.clipboard.pairs.add({ session: a, transfer: {}, pulls: [{}] }); f.relay.clipboard.pairs.add({ session: b, transfer: {}, pulls: [] }); f.relay.clipboard.activeTransfers = 3;
  f.relay.drive.pairs.add({ session: a, shares: new Map([['PRIVATE_SHARE', {}]]), outstanding: new Map([['PRIVATE_REQUEST', {}], ['OTHER_REQUEST', {}]]) });
  f.relay.usb.pairs.add({ session: a, devices: new Map([['PRIVATE_DEVICE', {}]]) }); f.relay.print.pairs.add({ session: a, transfer: {} });
  f.relay.displayBusy.set('pve-149', {}); f.relay.console.grants.set('PRIVATE_GRANT', { desktopId: 'pve-149', closed: true, pending: 1 }); f.vmAudio.operations.set('pve-149', {}); f.vmAudio.uncertain.add('pve-149');
  f.relay.sessionStop.pending.set('pve-149', { workerQuiescent: true }); f.relay.hostUpdate.operations.set('pve-149', { status: 'waiting' }); f.relay.accessPending = () => true;
  f.store.db.prepare("INSERT INTO power_tasks(id,desktop_id,user_id,action,status,active,submitted_at,guard_until,config_stamp) VALUES('fixture-power','pve-149','PRIVATE_ACTOR','reboot','running',1,1,1000,'fixture')").run();
  const report = activityModule.desktopActivitySnapshot(f, 'pve-149'); assert.equal(report.complete, true);
  assert.deepEqual(report.channels, { clipboard: 1, clipboardTransfers: 2, drive: 1, driveShares: 1, driveRequests: 2, usb: 1, usbDevices: 1, print: 1, printTransfers: 1 });
  assert.deepEqual(report.operations, { display: 1, console: 1, vmConfig: 1, vmConfigUncertain: 1, sessionCleanup: 1, hostUpdates: 1, power: 1, serverAccess: 1 });
  assert.equal(activityModule.desktopActivitySnapshot(f, 'pve-247').operations.power, 0);
  for (const secret of ['PRIVATE_', 'pve-149', 'PRIVATE_ACTOR', 'fixture-power']) assert.equal(JSON.stringify(report).includes(secret), false);
});

test('desktop activity never turns missing or unattributed state into idle counters', t => {
  for (const key of ['sessions', 'hosts', 'clipboard', 'drive', 'usb', 'print', 'displayBusy', 'console', 'sessionStop', 'hostUpdate', 'pcPower', 'accessPending']) {
    const f = desktopState(t); delete f.relay[key]; assert.equal(activityModule.desktopActivitySnapshot(f, 'pve-149').complete, false, key);
  }
  const malformed = desktopState(t); malformed.relay.clipboard.pairs.add({ session: {}, transfer: null, pulls: [] }); assert.equal(activityModule.desktopActivitySnapshot(malformed, 'pve-149').complete, false);
  const unattributed = desktopState(t); unattributed.relay.clipboard.activeTransfers = 1; const result = activityModule.desktopActivitySnapshot(unattributed, 'pve-149'); assert.equal(result.complete, false); assert.equal(result.channels.clipboardTransfers, null);
  const missingAudio = desktopState(t); delete missingAudio.vmAudio; assert.equal(activityModule.desktopActivitySnapshot(missingAudio, 'pve-149').complete, false);
});
