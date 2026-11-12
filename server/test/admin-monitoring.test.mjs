import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { streamStatsSessions } from '../src/admin-monitoring.mjs';

// Read-only administrator monitoring views: GET /api/v1/admin/sessions,
// /hosts and /stream-stats (src/admin-monitoring.mjs).
const origin = 'https://monitoring.invalid';
const caps = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true, diagnosticsSupported: true, monitors: [{ id: '0', width: 1920, height: 1080, primary: true }] };
const report = () => ({ protocol: 'host-diagnostics-v1', source: 'host-service', version: '0.2.61', collectedAt: new Date().toISOString(), interactiveSession: true, consoleSession: true, captureAvailable: true,
  display: { observed: true, observationAgeMs: 0, installed: true, deviceReady: true, owned: true, activeDisplay: false, configurationPending: false, recoveryRequired: true, rebootRequired: false, remoteSession: false, monitorCount: 0, width: 0, height: 0, reason: 'native_topology_recovery_required' },
  native: { available: true, installed: true, started: true, leaseActive: false, activationVerified: false, configurationPending: false, recoveryRequired: true, countControlAvailable: false, phase: 'recovery', failureReason: 'controller_start_failed' },
  events: [{ time: new Date().toISOString(), event: 'displayConfigure', values: { ok: false, monitors: 2, error: 1610 } }] });

async function fixture(t, { commissioned = true } = {}) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const tokens = {};
  for (const [name, role, purpose] of [['admin', 'admin', 'admin'], ['alice', 'user', 'client'], ['bob', 'user', 'client']]) {
    store.addUser({ name, passwordHash: 'fixture', role });
    tokens[name] = newToken(); store.createSession(store.findUser(name), tokenHash(tokens[name]), Date.now(), 3600000, purpose);
  }
  const vms = [{ vmid: 149, node: 'fixture', name: 'TESTER', status: 'running' }, { vmid: 150, node: 'fixture', name: 'SECOND', status: 'running' }, { vmid: 151, node: 'fixture', name: 'THIRD', status: 'stopped' }];
  store.syncProxmox(vms, 'fixture'); store.assign('alice', 'pve-149'); store.assign('bob', 'pve-150');
  if (commissioned) store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve.invalid', tokenId: 'fixture@pve!fixture', secret: 'fixture' }), 'fixture');
  else store.db.prepare("DELETE FROM settings WHERE key='proxmox_initialized'").run();
  const hostTokens = {};
  for (const [desktop, index] of [['pve-149', 1], ['pve-150', 2], ['pve-151', 3]]) {
    const code = newToken(), uuid = `00000000-1234-1234-1234-00000000014${index}`; hostTokens[desktop] = newToken();
    store.issueEnrollment(desktop, uuid, tokenHash(code), 'fixture');
    store.consumeEnrollment(tokenHash(code), uuid, { hostname: 'fixture-' + index, version: '0.2.5' + index, tokenHash: tokenHash(hostTokens[desktop]) });
  }
  store.db.prepare("UPDATE hosts SET revoked=1 WHERE desktop_id='pve-151'").run();
  const server = await buildServer({ store, vault, publicOrigin: origin, proxmox: { inventory: async () => vms }, relayOptions: { authorizationMs: 20, heartbeatMs: 1000 }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  const base = `http://127.0.0.1:${server.address().port}`;
  const admin = async (method, path, { cookie = tokens.admin, write = method !== 'GET' } = {}) => {
    const response = await fetch(base + '/api/v1/admin' + path, { method, headers: { ...(cookie ? { Cookie: '__Secure-multitap-admin=' + cookie } : {}), ...(write ? { Origin: origin, 'X-CSRF-Token': vault.csrf(cookie), 'Content-Type': 'application/json' } : {}) }, body: write ? '{}' : undefined });
    return { status: response.status, text: await response.text(), get body() { return JSON.parse(this.text); } };
  };
  const client = async (as, method, path, body) => {
    const response = await fetch(base + '/api/v1' + path, { method, headers: { Authorization: 'Bearer ' + tokens[as], ...(body !== undefined ? { 'Content-Type': 'application/json' } : {}) }, body: body === undefined ? undefined : JSON.stringify(body) });
    return { status: response.status, body: await response.json() };
  };
  async function host(desktop) {
    const ws = new WebSocket(base.replace('http:', 'ws:') + '/api/v1/hosts/channel', { headers: { Authorization: 'Host ' + hostTokens[desktop], 'X-Multitap-Host-Protocol': 'capabilities-v1' }, perMessageDeflate: false });
    await once(ws, 'open');
    const replies = [];
    ws.on('message', data => replies.push(JSON.parse(data.toString())));
    const reply = async type => { for (let i = 0; i < 200; i++) { const index = replies.findIndex(value => value.type === type); if (index >= 0) return replies.splice(index, 1)[0]; await new Promise(resolve => setTimeout(resolve, 10)); } assert.fail('No ' + type); };
    ws.json = value => ws.send(JSON.stringify(value)); ws.reply = reply;
    ws.json({ type: 'host.capabilities', capabilities: caps }); await reply('host.capabilities.ack');
    t.after(() => ws.close());
    return ws;
  }
  return { store, base, tokens, hostTokens, admin, client, host };
}

test('monitoring views are administrator-only, read-only and wait for Proxmox commissioning', async t => {
  const f = await fixture(t);
  for (const path of ['/sessions', '/hosts', '/stream-stats']) {
    assert.equal((await f.admin('GET', path, { cookie: null })).status, 401, path);
    assert.equal((await fetch(f.base + '/api/v1/admin' + path, { headers: { Authorization: 'Bearer ' + f.tokens.alice } })).status, 401, 'a client bearer token is not an administrator session');
    assert.equal((await f.admin('GET', path, { cookie: f.tokens.alice })).status, 401, 'a client session token is not an administrator cookie');
    assert.equal((await f.admin('POST', path, { write: false })).status, 403, 'writes still require the same-origin check');
    assert.equal((await f.admin('POST', path)).status, 405, path);
    assert.equal((await f.admin('GET', path + '/x')).status, 404);
    const ok = await f.admin('GET', path);
    assert.equal(ok.status, 200, path);
    for (const secret of [f.tokens.admin, f.tokens.alice, ...Object.values(f.hostTokens), tokenHash(f.hostTokens['pve-149']), '00000000-1234-1234-1234-000000000141']) assert.equal(ok.text.includes(secret), false, `${path} leaks no token, hash or machine identity`);
  }
  const fresh = await fixture(t, { commissioned: false });
  assert.equal((await fresh.admin('GET', '/hosts')).body.error.code, 'proxmox_setup_required');
});

test('GET /hosts reports enrolled Hosts with live connection state and a diagnostics digest', async t => {
  const f = await fixture(t);
  let hosts = (await f.admin('GET', '/hosts')).body.hosts;
  assert.deepEqual(hosts.map(h => h.desktopId), ['pve-149', 'pve-150'], 'revoked Hosts are not listed');
  assert.deepEqual(hosts.map(h => [h.connected, h.version, h.hostname, h.diagnostics]), [[false, '0.2.51', 'fixture-1', null], [false, '0.2.52', 'fixture-2', null]]);
  assert.ok(hosts.every(h => Number.isFinite(Date.parse(h.enrolledAt))));
  const ws = await f.host('pve-149');
  ws.json({ type: 'host.diagnostics', report: report() }); assert.equal((await ws.reply('host.diagnostics.ack')).accepted, true);
  hosts = (await f.admin('GET', '/hosts')).body.hosts;
  const live = hosts.find(h => h.desktopId === 'pve-149');
  assert.equal(live.connected, true); assert.ok(Number.isFinite(Date.parse(live.lastSeenAt)));
  assert.equal(live.diagnostics.stale, false); assert.equal(live.diagnostics.display.reason, 'native_topology_recovery_required');
  assert.equal(live.diagnostics.display.recoveryRequired, true); assert.equal(live.diagnostics.native.failureReason, 'controller_start_failed');
  assert.deepEqual(live.diagnostics.events.map(e => [e.event, e.values]), [['displayConfigure', { ok: false, monitors: 2, error: 1610 }]]);
  assert.deepEqual(Object.keys(live).sort(), ['connected', 'desktopId', 'diagnostics', 'enrolledAt', 'hostId', 'hostname', 'lastSeenAt', 'version']);
  assert.equal(live.hostId, f.store.db.prepare('SELECT id FROM hosts WHERE desktop_id=?').get('pve-149').id);
  ws.close(); await once(ws, 'close');
  for (let i = 0; i < 100 && (await f.admin('GET', '/hosts')).body.hosts[0].connected; i++) await new Promise(resolve => setTimeout(resolve, 10));
  const offline = (await f.admin('GET', '/hosts')).body.hosts[0];
  assert.equal(offline.connected, false); assert.equal(offline.diagnostics.stale, true, 'an offline Host keeps its last report, marked stale');
});

test('GET /sessions lists current relay sessions by account and mode without session credentials', async t => {
  const f = await fixture(t), ws = await f.host('pve-149');
  assert.deepEqual((await f.admin('GET', '/sessions')).body.sessions, []);
  const connect = await f.client('alice', 'POST', '/desktops/pve-149/connect', {});
  assert.equal(connect.status, 200);
  let sessions = (await f.admin('GET', '/sessions')).body.sessions;
  assert.deepEqual(sessions, [{ sessionId: connect.body.sessionId, desktopId: 'pve-149', account: 'alice', mode: 'stream', attached: false, ready: false }]);
  const viewer = new WebSocket(connect.body.webSocketUrl.replace(origin.replace('https:', 'wss:'), f.base.replace('http:', 'ws:')), { headers: { Authorization: 'Bearer ' + f.tokens.alice } });
  await once(viewer, 'open'); await ws.reply('session.start');
  sessions = (await f.admin('GET', '/sessions')).body.sessions;
  assert.equal(sessions[0].attached, true);
  viewer.close(); await ws.reply('session.stop');
  for (let i = 0; i < 100 && (await f.admin('GET', '/sessions')).body.sessions.length; i++) await new Promise(resolve => setTimeout(resolve, 10));
  assert.deepEqual((await f.admin('GET', '/sessions')).body.sessions, []);
});

test('GET /stream-stats groups streaming windows by session in time order and ignores other client records', async t => {
  const f = await fixture(t);
  const sessionA = '11111111-2222-4333-8444-555555555555', sessionB = '66666666-7777-4888-9999-aaaaaaaaaaaa';
  const now = Date.now();
  const window = (time, sessionId, desktopId, fps) => ({ time, event: 'streamStats', desktopId, sessionId, mode: 'stream', values: { samples: 30, windowMs: 30000, decodeFps10: fps, captureFps10: 600, rttMs: 12, inputFrameP50Us: 31000, secret: 'DO_NOT_STORE' } });
  const send = async (as, sequence, events) => (await f.client(as, 'POST', '/client/diagnostics', { protocol: 'client-diagnostics-v1', runId: (as === 'alice' ? 'a' : 'b').repeat(32), sequence, version: '0.4.52', events })).status;
  // The fixture clock is the request time, so windows are stamped relative to now.
  const stamp = offset => new Date(now - 600000 + offset * 1000).toISOString();
  assert.equal(await send('alice', 1, [window(stamp(30), sessionA, 'pve-149', 590), { time: stamp(31), event: 'streamStart', desktopId: 'pve-149', sessionId: sessionA, mode: 'stream', values: {} }]), 202);
  assert.equal(await send('alice', 2, [window(stamp(60), sessionA, 'pve-149', 575), window(stamp(90), sessionA, 'pve-149', 580)]), 202);
  assert.equal(await send('bob', 1, [window(stamp(120), sessionB, 'pve-150', 300)]), 202);
  const value = (await f.admin('GET', '/stream-stats')).body;
  assert.equal(value.protocol, 'stream-stats-v1'); assert.equal(value.truncated, false);
  assert.deepEqual(value.sessions.map(s => [s.sessionId, s.desktopId, s.account, s.windows.length]), [[sessionB, 'pve-150', 'bob', 1], [sessionA, 'pve-149', 'alice', 3]], 'newest session first');
  assert.deepEqual(value.sessions[1].windows.map(w => w.values.decodeFps10), [590, 575, 580], 'windows in time order');
  assert.equal(JSON.stringify(value).includes('DO_NOT_STORE'), false, 'only the fixed numeric fields survive');
  assert.equal(value.sessions[1].version, '0.4.52');
  // The view is bounded to the newest windows.
  const bounded = streamStatsSessions(f.store, Date.now(), 2);
  assert.equal(bounded.truncated, true); assert.equal(bounded.sessions.reduce((n, s) => n + s.windows.length, 0), 2);
  assert.deepEqual(bounded.sessions.map(s => s.sessionId), [sessionB, sessionA]);
});
