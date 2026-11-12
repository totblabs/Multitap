import { test } from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword, newToken, tokenHash } from '../src/security.mjs';
import { serverAccessQuiescence } from '../update/server-access-engine.mjs';

// Saving the server access settings ends the
// existing connections. A real broker, Host channel and client session on
// loopback; the privileged engine is a memory stand-in that only records what
// the broker asks of it. No deployment, listener change or OS setting.
const origin = 'https://access-connections.invalid', password = 'Access-connections-fixture-only-58!';
const caps = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true, diagnosticsSupported: true, monitors: [{ id: '0', width: 1920, height: 1080, primary: true }] };
const settle = async predicate => { for (let i = 0; i < 300; i++) { if (await predicate()) return; await new Promise(resolve => setTimeout(resolve, 10)); } assert.fail('condition not reached'); };

async function fixture(t) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32)), tokens = {}, engine = { state: 'idle', operationId: null, fence: null, cancels: [] };
  store.addUser({ name: 'admin', passwordHash: await hashPassword(password), role: 'admin' });
  store.addUser({ name: 'alice', passwordHash: 'fixture', role: 'user' });
  store.saveSetting('proxmox_initialized', '1', 'fixture');
  tokens.alice = newToken(); store.createSession(store.findUser('alice'), tokenHash(tokens.alice), Date.now(), 3600000, 'client');
  const vms = [{ vmid: 149, node: 'fixture', name: 'TESTER', status: 'running' }];
  store.syncProxmox(vms, 'fixture'); store.assign('alice', 'pve-149');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve.invalid', tokenId: 'fixture@pve!fixture', secret: 'fixture' }), 'fixture');
  const code = newToken(), uuid = '00000000-1234-1234-1234-000000000149', hostToken = newToken();
  store.issueEnrollment('pve-149', uuid, tokenHash(code), 'fixture');
  store.consumeEnrollment(tokenHash(code), uuid, { hostname: 'fixture-1', version: '0.2.51', tokenHash: tokenHash(hostToken) });
  // The engine as the broker sees it: status, an accepted save, a cancel.
  const accessClient = {
    status: async () => ({ protocol: 'server-access-v1', managed: true, applySupported: true, revision: '0'.repeat(64), config: null, pendingConfig: null, state: engine.state, operationId: engine.operationId,
      rollbackVerified: false, unchangedVerified: engine.state === 'failed', ...(engine.state === 'failed' ? { errorCode: 'server_access_cancelled' } : {}) }),
    test: async () => ({ testId: 'b'.repeat(64) }),
    save: async (_input, context) => { engine.fence = context.fence; engine.operationId = context.fence.operationId; engine.state = 'waiting_idle'; return { accepted: true, operationId: context.fence.operationId, state: 'waiting_idle' }; },
    cancel: async input => { engine.cancels.push(input); engine.state = 'failed'; return { accepted: true, operationId: input.operationId, state: 'failed', cancelled: true }; },
  };
  const server = await buildServer({ store, vault, publicOrigin: origin, accessClient, proxmox: { inventory: async () => vms }, relayOptions: { authorizationMs: 60000, heartbeatMs: 60000 }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  const port = server.address().port, base = `http://127.0.0.1:${port}`;
  // The updater's own loopback request (server-access-health.mjs localAccessRequest).
  const internal = (path, { method = 'GET', fence } = {}) => new Promise((resolve, reject) => {
    const req = http.request({ host: '127.0.0.1', port, path, method, headers: { Host: `127.0.0.1:${port}`, Accept: 'application/json', ...(fence ? { 'x-multitap-access-fence': fence } : {}), ...(method === 'POST' ? { 'Content-Length': 0 } : {}) } }, res => {
      let text = ''; res.setEncoding('utf8'); res.on('data', chunk => text += chunk); res.on('end', () => { let body = null; try { body = JSON.parse(text); } catch { /* asserted by the caller */ } resolve({ status: res.statusCode, body }); });
    });
    req.on('error', reject); req.end();
  });
  const client = async (method, path, body) => {
    const response = await fetch(base + '/api/v1' + path, { method, headers: { Authorization: 'Bearer ' + tokens.alice, ...(body !== undefined ? { 'Content-Type': 'application/json' } : {}) }, body: body === undefined ? undefined : JSON.stringify(body) });
    return { status: response.status, body: await response.json() };
  };
  let cookie = '', csrf = '';
  const admin = async (path, method = 'GET', data) => {
    const response = await fetch(base + '/api/v1/admin' + path, { method, headers: { Cookie: cookie, ...(data !== undefined ? { 'Content-Type': 'application/json' } : {}), ...(method !== 'GET' ? { Origin: origin, 'X-CSRF-Token': csrf } : {}) }, body: data === undefined ? undefined : JSON.stringify(data) });
    return { status: response.status, headers: response.headers, body: await response.json() };
  };
  const login = await admin('/login', 'POST', { username: 'admin', password });
  assert.equal(login.status, 200); cookie = login.headers.get('set-cookie').split(';')[0]; csrf = login.body.csrfToken;
  async function host() {
    const ws = new WebSocket(base.replace('http:', 'ws:') + '/api/v1/hosts/channel', { headers: { Authorization: 'Host ' + hostToken, 'X-Multitap-Host-Protocol': 'capabilities-v1' }, perMessageDeflate: false });
    await once(ws, 'open');
    const replies = [];
    ws.on('message', data => replies.push(JSON.parse(data.toString())));
    ws.reply = async type => { for (let i = 0; i < 300; i++) { const index = replies.findIndex(value => value.type === type); if (index >= 0) return replies.splice(index, 1)[0]; await new Promise(resolve => setTimeout(resolve, 10)); } assert.fail('No ' + type); };
    ws.send(JSON.stringify({ type: 'host.capabilities', capabilities: caps })); await ws.reply('host.capabilities.ack');
    t.after(() => ws.close());
    return ws;
  }
  return { store, server, engine, internal, client, admin, host, tokens, wsUrl: url => url.replace(origin.replace('https:', 'wss:'), base.replace('http:', 'ws:')) };
}

test('a save ends a connected session the way a stopping server does, records how many, and reconnects wait instead of failing', { timeout: 30000 }, async t => {
  const f = await fixture(t), host = await f.host();
  const connect = await f.client('POST', '/desktops/pve-149/connect', {}); assert.equal(connect.status, 200);
  const viewer = new WebSocket(f.wsUrl(connect.body.webSocketUrl), { headers: { Authorization: 'Bearer ' + f.tokens.alice } });
  const messages = []; viewer.on('message', (data, binary) => { if (!binary) messages.push(JSON.parse(data.toString())); });
  const closed = new Promise(resolve => viewer.once('close', (code, reason) => resolve({ code, reason: reason.toString() })));
  await once(viewer, 'open'); await host.reply('session.start');

  // The console's confirmation reads how many people a save would cut off.
  const before = await f.admin('/server-access');
  assert.equal(before.status, 200); assert.deepEqual(before.body.connections, { people: 1, sessions: 1, consoles: 0 });

  const saved = await f.admin('/server-access/save', 'POST', { config: {}, testId: 'b'.repeat(64), baseRevision: '0'.repeat(64), confirm: true });
  assert.equal(saved.status, 202); assert.equal(saved.body.operationId, f.engine.operationId);
  const fence = f.engine.fence.token;
  // Saving alone ends nothing: the engine first waits for the broker's own work.
  const report = await f.internal('/internal/server-access/activity', { fence });
  assert.equal(report.status, 200); assert.deepEqual(report.body.connections, { people: 1, sessions: 1, consoles: 0 });
  assert.deepEqual(serverAccessQuiescence(report.body).blockers, [], 'a connection (and its recent start) is not a reason to wait');
  assert.equal(serverAccessQuiescence(report.body).open, true); assert.equal(f.server.relay.sessions.size, 1);
  // While it waits a new connection is told the settings are being applied.
  const refused = await f.client('POST', '/desktops/pve-149/connect', {}); assert.equal(refused.status, 409); assert.equal(refused.body.error.code, 'server_access_pending');

  const ended = await f.internal('/internal/server-access/end-connections', { method: 'POST', fence });
  assert.equal(ended.status, 200); assert.deepEqual(ended.body, { protocol: 'server-access-connections-v1', fence: true, ended: { people: 1, sessions: 1, consoles: 0 } });
  // Host and client see exactly what a server update makes them see.
  const stop = await host.reply('session.stop'); assert.equal(stop.sessionId, connect.body.sessionId); assert.equal(stop.reason, 'server_stopping');
  assert.deepEqual(await closed, { code: 1000, reason: 'server_stopping' });
  assert.deepEqual(messages.filter(value => value.type === 'session.ended').map(value => value.reason), ['server_stopping']);
  assert.equal(f.server.relay.sessions.size, 0);
  await settle(async () => { const quiet = serverAccessQuiescence((await f.internal('/internal/server-access/activity', { fence })).body); return !quiet.open && !quiet.blockers.length; });
  // The audit row carries who saved and the count: operation:people:connections.
  const rows = f.store.auditLog().filter(row => row.event === 'server_access_connections_ended');
  assert.equal(rows.length, 1); assert.equal(rows[0].subject, f.engine.operationId + ':1:1'); assert.equal(rows[0].actor, f.store.findUser('admin').id);
  // Until the restart a released client's reconnect is answered as a restart
  // (it waits and tries again), not as something to check.
  const retry = await f.client('POST', '/desktops/pve-149/connect', {}); assert.equal(retry.status, 503); assert.equal(retry.body.error.code, 'server_restarting');
  // Asking again ends nothing more and records nothing more.
  const again = await f.internal('/internal/server-access/end-connections', { method: 'POST', fence });
  assert.deepEqual(again.body.ended, { people: 0, sessions: 0, consoles: 0 });
  assert.equal(f.store.auditLog().filter(row => row.event === 'server_access_connections_ended').length, 1);
});

test('the console cancels a waiting save: only its own operation, audited, and the server takes connections again', { timeout: 30000 }, async t => {
  const f = await fixture(t), host = await f.host();
  const saved = await f.admin('/server-access/save', 'POST', { config: {}, testId: 'b'.repeat(64), baseRevision: '0'.repeat(64), confirm: true });
  assert.equal(saved.status, 202);
  assert.equal((await f.client('POST', '/desktops/pve-149/connect', {})).status, 409);
  for (const body of [{}, { operationId: 'not-an-operation' }, { operationId: saved.body.operationId, extra: true }]) assert.equal((await f.admin('/server-access/cancel', 'POST', body)).status, 400);
  const other = await f.admin('/server-access/cancel', 'POST', { operationId: '12345678-1234-4234-8234-123456789abc' });
  assert.equal(other.status, 409); assert.equal(other.body.error.code, 'server_access_cancel_unavailable'); assert.deepEqual(f.engine.cancels, []);
  assert.equal((await f.admin('/server-access/cancel')).status, 405);
  const cancelled = await f.admin('/server-access/cancel', 'POST', { operationId: saved.body.operationId });
  assert.equal(cancelled.status, 200); assert.equal(cancelled.body.cancelled, true); assert.deepEqual(f.engine.cancels, [{ operationId: saved.body.operationId }]);
  const rows = f.store.auditLog().filter(row => row.event === 'server_access_cancelled');
  assert.equal(rows.length, 1); assert.equal(rows[0].subject, saved.body.operationId); assert.equal(rows[0].actor, f.store.findUser('admin').id);
  assert.equal(f.server.accessRuntime.blocked, false, 'the fence opened on the engine\'s verified unchanged result');
  const connect = await f.client('POST', '/desktops/pve-149/connect', {}); assert.equal(connect.status, 200);
  assert.equal(f.store.auditLog().some(row => row.event === 'server_access_connections_ended'), false);
  host.close();
});
