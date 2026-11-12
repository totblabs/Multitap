import test from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes, randomUUID } from 'node:crypto';
import { createServer, request as httpRequest } from 'node:http';
import { mkdtemp, writeFile, chmod, stat, rename, unlink, rmdir, realpath } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve, dirname, basename } from 'node:path';
import { AccessRuntime } from '../src/access-runtime.mjs';
import { legacyProxyHealthProbe } from '../src/request-origin.mjs';
import { HttpError } from '../src/errors.mjs';
import { buildServer } from '../src/http.mjs';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { normalizeServerAccess, serverAccessHash } from '../update/server-access-policy.mjs';

// Memory operation clients plus Root-owned loopback TCP only. No deployment,
// real accounts, TLS keys, Windows/Android capture, credentials or OS settings.
const primary = 'https://managed-fixture.invalid';
const authority = new URL(primary).host;
const statusContext = { managementOrigin: primary };
const deferred = () => {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
};
const state = (operationId, values = {}) => ({ protocol: 'server-access-v1', managed: true,
  applySupported: true, revision: '0'.repeat(64), config: null, pendingConfig: null,
  state: 'waiting_idle', operationId, rollbackVerified: false, unchangedVerified: false, ...values });
const fenced = runtime => assert.throws(() => runtime.assertAdmission(), error => error.status === 409 && error.code === 'server_access_pending');
// A fence this process started with (it was just restarted by an apply), or
// one whose connections are being ended, refuses the way a restarting server
// does, so released clients wait and connect again by themselves.
const restarting = runtime => assert.throws(() => runtime.assertAdmission(), error => error.status === 503 && error.code === 'server_restarting');

function config(port, httpOnly = false) {
  return normalizeServerAccess({ protocol: 'server-access-v1',
    primaryOrigin: httpOnly ? 'http://127.0.0.1:' + port : primary,
    https: { enabled: !httpOnly, origin: httpOnly ? null : primary, mode: 'proxy', listenHost: '127.0.0.1', listenPort: port },
    http: { enabled: httpOnly, origin: httpOnly ? 'http://127.0.0.1:' + port : null, listenHost: '127.0.0.1', listenPort: port },
    trustedProxies: [] });
}

test('save installs the current admission fence before async dispatch; dropped ACK keeps that same operation without replay', { timeout: 10000 }, async t => {
  const acknowledgement = deferred(); let calls = 0, sent, observed = state(null);
  const runtime = new AccessRuntime({ client: {
    save: (_input, context) => {
      calls++; sent = context.fence;
      assert.equal(runtime.blocked, true); fenced(runtime);
      assert.match(sent.operationId, /^[a-f0-9-]{36}$/);
      assert.equal(runtime.accepts(sent.token), true);
      return acknowledgement.promise;
    }, status: async () => observed,
  } });
  t.after(() => runtime.close());
  const pending = runtime.save({ synthetic: true }, statusContext);
  assert.equal(calls, 1); fenced(runtime);
  await assert.rejects(runtime.save({}, statusContext), error => error.code === 'server_access_pending');
  const dropped = assert.rejects(pending, /synthetic_ack_dropped/);
  acknowledgement.reject(Error('synthetic_ack_dropped')); await dropped;
  assert.equal(calls, 1); assert.equal(runtime.lease.operationId, sent.operationId);
  assert.equal(runtime.accepts(sent.token), true); fenced(runtime);
  observed = state(randomUUID(), { state: 'succeeded' });
  await runtime.status(statusContext); fenced(runtime);
  observed = state(sent.operationId, { state: 'failed', unchangedVerified: false });
  await runtime.status(statusContext); fenced(runtime);
  observed = state(sent.operationId, { state: 'rolled_back', rollbackVerified: true });
  const reconciled = await runtime.status(statusContext);
  assert.equal(reconciled.state, 'failed_rolled_back');
  assert.equal(runtime.blocked, false); assert.equal(runtime.accepts(sent.token), false);
  assert.equal(calls, 1, 'status reconciliation never retries save');
});

test('only matching authoritative succeeded or explicitly verified rollback/unchanged terminal observations clear a startup fence', async () => {
  // The production engine emits succeeded only after its apply/health verify;
  // it has no separate invented succeededVerified DTO field.
  for (const terminal of [{ state: 'succeeded' }, { state: 'rolled_back', rollbackVerified: true }, { state: 'failed', unchangedVerified: true }]) {
    const operationId = randomUUID(), observation = deferred(); let saves = 0;
    const runtime = new AccessRuntime({ initialOperationId: operationId, client: {
      status: () => observation.promise, save: async () => { saves++; assert.fail('startup pending must not dispatch save'); },
    } });
    try {
      restarting(runtime); await assert.rejects(runtime.save({}, statusContext), error => error.code === 'server_restarting');
      const pending = runtime.status(statusContext); restarting(runtime);
      observation.resolve(state(operationId, terminal)); await pending;
      assert.equal(runtime.blocked, false); assert.doesNotThrow(() => runtime.assertAdmission()); assert.equal(saves, 0);
    } finally { runtime.close(); }
  }
});

test('foreign/null terminal IDs, unverified failure/rollback and recovery-required cannot retire a pending startup operation', async () => {
  const operationId = randomUUID(); let observation;
  const runtime = new AccessRuntime({ initialOperationId: operationId, client: { status: async () => observation } });
  try {
    for (const unsafe of [state(null, { state: 'succeeded' }), state(randomUUID(), { state: 'succeeded' }),
      state(randomUUID(), { state: 'rolled_back', rollbackVerified: true }), state(randomUUID(), { state: 'failed', unchangedVerified: true }),
      state(operationId, { state: 'rolled_back', rollbackVerified: false }), state(operationId, { state: 'failed', unchangedVerified: false }),
      state(operationId, { state: 'recovery_required', rollbackVerified: true, unchangedVerified: true }), state(operationId)]) {
      observation = unsafe; await runtime.status(statusContext); restarting(runtime);
      assert.equal(runtime.lease.operationId, operationId);
    }
    // The restart answer is bounded: a fence that never clears is "pending" again.
    runtime.lease.restartUntil = Date.now() - 1; fenced(runtime);
  } finally { runtime.close(); }
  assert.throws(() => new AccessRuntime({ initialOperationId: 'not-an-operation' }), /operation_invalid/);
});

test('a mismatched save acknowledgement cannot change or release the preissued operation', async t => {
  let requested;
  const runtime = new AccessRuntime({ client: { save: async (_input, context) => {
    requested = context.fence.operationId; return { accepted: true, operationId: randomUUID() };
  }, status: async () => state(requested) } });
  t.after(() => runtime.close());
  await assert.rejects(runtime.save({}, statusContext), error => error.status === 503 && error.code === 'server_access_result_unknown');
  assert.equal(runtime.lease.operationId, requested); fenced(runtime);
});

function probeRequest(values = {}) {
  return { method: 'GET', url: '/healthz', headers: { host: authority, ...values.headers },
    socket: { localPort: 18480, encrypted: false, ...values.socket },
    ...(values.method ? { method: values.method } : {}), ...(values.url ? { url: values.url } : {}) };
}

test('legacy proxy health compatibility is exact unauthenticated GET health only, never an API or transport grant', () => {
  const current = config(18480);
  assert.equal(legacyProxyHealthProbe(probeRequest(), current), true);
  assert.equal(legacyProxyHealthProbe(probeRequest({ headers: { host: authority.toUpperCase() + ':443' } }), current), true);
  for (const header of ['origin', 'authorization', 'cookie', 'user-agent', 'x-forwarded-for', 'x-forwarded-host', 'x-forwarded-proto', 'x-real-ip', 'forwarded', 'via']) {
    assert.equal(legacyProxyHealthProbe(probeRequest({ headers: { [header]: '' } }), current), false, header + ' presence is forbidden even empty');
  }
  for (const bad of [probeRequest({ method: 'POST' }), probeRequest({ method: 'HEAD' }), probeRequest({ url: '/healthz?x=1' }),
    probeRequest({ url: '/healthz/' }), probeRequest({ url: '/' }), probeRequest({ url: '/api/v1/auth/login' }),
    probeRequest({ url: '/api/v1/hosts/channel' }), probeRequest({ socket: { encrypted: true } }), probeRequest({ socket: { localPort: 18481 } })]) {
    assert.equal(legacyProxyHealthProbe(bad, current), false);
  }
  for (const host of ['other-fixture.invalid', authority + ':444', 'user@' + authority, authority + '/healthz', authority + '?x=1',
    authority + '#x', 'http://' + authority, authority + '\\evil', '', '127.0.0.1', '[::1]']) {
    assert.equal(legacyProxyHealthProbe(probeRequest({ headers: { host } }), current), false, 'bad authority');
  }
  assert.equal(legacyProxyHealthProbe(probeRequest(), { ...current, primaryOrigin: 'https://other-fixture.invalid' }), false);
  assert.equal(legacyProxyHealthProbe(probeRequest(), { ...current, https: { ...current.https, enabled: false } }), false);
  assert.equal(legacyProxyHealthProbe(probeRequest(), { ...current, https: { ...current.https, mode: 'direct' } }), false);
});

async function listen(server, port) {
  await new Promise((done, fail) => {
    const failed = error => { server.off('listening', ready); fail(error); };
    const ready = () => { server.off('error', failed); done(); };
    server.once('error', failed); server.once('listening', ready); server.listen(port, '127.0.0.1');
  });
}
async function fixture(t, { httpOnly = false, identityFile = null, client = null } = {}) {
  const reservation = createServer(); await listen(reservation, 0);
  const port = reservation.address().port;
  await new Promise(done => reservation.close(done)); // bind race is a setup failure, not retried
  const current = config(port, httpOnly), store = new Store(':memory:'), sockets = new Set();
  let server;
  t.after(async () => {
    try {
      for (const socket of sockets) socket.destroy();
      if (server) await new Promise(done => server.close(done));
    } finally { store.close(); }
  });
  server = await buildServer({ store, vault: new Vault(randomBytes(32)), publicOrigin: current.primaryOrigin,
    allowInsecureHttp: httpOnly, accessConfig: current, accessRevision: serverAccessHash(current),
    accessInstallationId: null, accessIdentityFile: identityFile, accessClient: client,
    trustedProxies: [], logger: { error: () => assert.fail('unexpected fixture server error') } });
  server.on('connection', socket => { sockets.add(socket); socket.once('close', () => sockets.delete(socket)); });
  await listen(server, port);
  async function request(path, { method = 'GET', headers = {}, data } = {}) {
    return new Promise((done, fail) => {
      const request = httpRequest({ hostname: '127.0.0.1', port, path, method, agent: false, timeout: 3000,
        headers: { Host: new URL(current.primaryOrigin).host, ...(data === undefined ? {} : { 'Content-Type': 'application/json' }), ...headers } }, response => {
        let size = 0; const bytes = [];
        response.on('data', chunk => { size += chunk.length; if (size > 65536) request.destroy(Error('fixture_body_limit')); else bytes.push(chunk); });
        response.once('error', fail); response.once('aborted', () => fail(Error('fixture_response_aborted')));
        response.once('end', () => { let body; try { body = JSON.parse(Buffer.concat(bytes).toString('utf8')); } catch { body = null; }
          done({ status: response.statusCode, body }); });
      });
      request.once('upgrade', (_response, socket) => { socket.destroy(); fail(Error('forbidden_fixture_upgrade_accepted')); });
      request.once('error', fail); request.once('timeout', () => request.destroy(Error('fixture_request_timeout')));
      request.end(data === undefined ? undefined : JSON.stringify(data));
    });
  }
  return { server, store, request, current };
}
const code = response => typeof response.body?.error === 'string' ? response.body.error : response.body?.error?.code;

// A running Host update does not refuse the save. The engine
// waits for it (bounded, shown on the page, cancellable) and the fenced report
// is what tells it so; a request that only waits for its connection is not
// in flight and holds nothing.
test('actual server accepts a save during a Host update; the fenced report counts only the ones in flight', { timeout: 15000 }, async t => {
  let saves = 0, fence;
  const f = await fixture(t, { httpOnly: true, client: { status: async () => state(fence?.operationId ?? null),
    save: async (_input, context) => { saves++; fence = context.fence; return { accepted: true, operationId: context.fence.operationId }; } } });
  const updates = f.server.relay.hostUpdate.operations;
  for (const [index, status] of ['waiting', 'dispatched', 'accepted', 'downloading', 'verifying', 'installing'].entries()) updates.set('synthetic-desktop-' + index, { status, checkedAt: Date.now(), submittedAt: Date.now(), retryAt: Infinity });
  await f.server.accessRuntime.save({}, statusContext, { actor: 'synthetic-admin' });
  assert.equal(saves, 1); fenced(f.server.accessRuntime); assert.equal(f.server.accessRuntime.lease.actor, 'synthetic-admin');
  const local = { Host: '127.0.0.1:' + new URL(f.current.primaryOrigin).port };
  const report = await f.request('/internal/server-access/activity', { headers: { ...local, 'x-multitap-access-fence': fence.token } });
  assert.equal(report.status, 200); assert.equal(report.body.fence, true); assert.equal(report.body.activity.operations.hostUpdates, 5);
  assert.deepEqual(report.body.connections, { people: 0, sessions: 0, consoles: 0 });
  // Without the current fence neither the report nor the ending exists.
  for (const headers of [local, { ...local, 'x-multitap-access-fence': 'f'.repeat(64) }]) {
    assert.notEqual((await f.request('/internal/server-access/activity', { headers })).status, 200);
    assert.notEqual((await f.request('/internal/server-access/end-connections', { method: 'POST', headers })).status, 200);
  }
  assert.notEqual((await f.request('/internal/server-access/end-connections', { headers: { ...local, 'x-multitap-access-fence': fence.token } })).status, 200, 'a read never ends connections');
  fenced(f.server.accessRuntime);
  // Nobody is connected: ending is a no-op that leaves no audit row, and from
  // then on a refusal reads as the restart it is about to be.
  const ended = await f.request('/internal/server-access/end-connections', { method: 'POST', headers: { ...local, 'x-multitap-access-fence': fence.token } });
  assert.equal(ended.status, 200); assert.deepEqual(ended.body, { protocol: 'server-access-connections-v1', fence: true, ended: { people: 0, sessions: 0, consoles: 0 } });
  assert.equal(f.store.auditLog().some(row => row.event === 'server_access_connections_ended'), false);
  restarting(f.server.accessRuntime);
});

test('cancel withdraws exactly the fenced operation and the fence opens only on the verified unchanged result', async t => {
  let observed = state(null), cancels = [], sent;
  const runtime = new AccessRuntime({ client: {
    save: async (_input, context) => { sent = context.fence; observed = state(sent.operationId); return { accepted: true, operationId: sent.operationId }; },
    cancel: async input => { cancels.push(input); observed = state(sent.operationId, { state: 'failed', errorCode: 'server_access_cancelled', unchangedVerified: true }); return { accepted: true, operationId: input.operationId, state: 'failed', cancelled: true }; },
    status: async () => observed,
  } });
  t.after(() => runtime.close());
  await assert.rejects(runtime.cancel(randomUUID()), error => error.status === 409 && error.code === 'server_access_cancel_unavailable');
  await runtime.save({}, statusContext); fenced(runtime);
  await assert.rejects(runtime.cancel(randomUUID()), error => error.status === 409 && error.code === 'server_access_cancel_unavailable');
  assert.deepEqual(cancels, []); fenced(runtime);
  const result = await runtime.cancel(sent.operationId);
  assert.deepEqual(cancels, [{ operationId: sent.operationId }]); assert.equal(result.cancelled, true);
  assert.equal(runtime.blocked, false); assert.doesNotThrow(() => runtime.assertAdmission());
  // A refused cancel (already applying) keeps the fence.
  const late = new AccessRuntime({ client: { save: async (_input, context) => ({ accepted: true, operationId: context.fence.operationId }), status: async () => state(late.lease?.operationId),
    cancel: async () => { throw new HttpError(409, 'server_access_cancel_too_late'); } } });
  t.after(() => late.close());
  await late.save({}, statusContext);
  await assert.rejects(late.cancel(late.lease.operationId), error => error.code === 'server_access_cancel_too_late'); fenced(late);
});

test('actual proxy backend legacy health is read-only; unknown authority, browser/auth headers and HTTP API/WS remain denied', { timeout: 15000 }, async t => {
  const f = await fixture(t);
  const health = await f.request('/healthz');
  assert.equal(health.status, 200); assert.equal(health.body.access.healthProbeOnly, true);
  assert.equal(health.body.access.origin, primary); assert.equal(Object.hasOwn(health.body.access, 'allowedOrigins'), false); assert.equal(Object.hasOwn(health.body.access, 'primaryOrigin'), false);
  assert.equal(health.body.transport.scheme, 'https'); assert.equal(health.body.transport.allowInsecureHttp, false);
  assert.equal(health.body.access.installationId, null);
  const denied = [
    [{ Host: 'unknown-fixture.invalid' }, 'origin_not_allowed'], [{ Origin: primary }, 'origin_not_allowed'],
    [{ 'User-Agent': 'synthetic-browser' }, 'http_disabled'], [{ Authorization: 'Bearer synthetic-not-a-real-session' }, 'http_disabled'],
    [{ Cookie: 'synthetic=no-real-session' }, 'http_disabled'], [{ 'X-Forwarded-Proto': 'https' }, 'http_disabled'],
  ];
  for (const [headers, expected] of denied) { const reply = await f.request('/healthz', { headers }); assert.equal(reply.status, 403); assert.equal(code(reply), expected); }
  for (const path of ['/', '/healthz?x=1', '/healthz/', '/api/v1/desktops', '/api/v1/admin/server-access']) {
    const reply = await f.request(path); assert.equal(reply.status, 403); assert.equal(code(reply), 'http_disabled');
  }
  const login = await f.request('/api/v1/auth/login', { method: 'POST', data: { username: 'synthetic-user', password: 'synthetic-not-a-credential' } });
  assert.equal(login.status, 403); assert.equal(code(login), 'http_disabled'); assert.equal(Object.hasOwn(login.body, 'token'), false);
  const upgrade = await f.request('/api/v1/hosts/channel', { headers: { Connection: 'Upgrade', Upgrade: 'websocket',
    'Sec-WebSocket-Version': '13', 'Sec-WebSocket-Key': Buffer.alloc(16, 7).toString('base64') } });
  assert.equal(upgrade.status, 403);
  assert.equal(f.server.relay.hosts.size, 0); assert.equal(f.server.relay.sessions.size, 0);
  assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM sessions').get().n, 0);
});

test('same-process actual HTTP health observes a later protected installation ID exactly once and never adopts its replacement', {
  timeout: 15000,
  skip: process.platform !== 'win32' && (typeof process.getuid !== 'function' || process.getuid() !== 0)
    ? 'production protected identity requires uid0 on Linux; no permission bypass' : false,
}, async t => {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-server-access-runtime-'));
  const identity = join(directory, 'server-access.identity'), replacement = join(directory, 'replacement.identity');
  t.after(async () => {
    for (const path of [replacement, identity]) {
      try { await chmod(path, 0o600); await unlink(path); } catch (error) { if (error.code !== 'ENOENT') throw error; }
    }
    await rmdir(directory); // exact owned empty directory, never recursive delete
  });
  assert.equal(dirname(directory), resolve(tmpdir())); assert.match(basename(directory), /^multitap-server-access-runtime-/);
  assert.equal(await realpath(directory), directory);
  const f = await fixture(t, { httpOnly: true, identityFile: identity });
  const missing = await f.request('/healthz');
  assert.equal(missing.status, 200); assert.equal(missing.body.access.installationId, null);
  async function protectedIdentity(path, value) {
    await writeFile(path, value + '\n', { flag: 'wx', mode: 0o600 });
    await chmod(path, process.platform === 'win32' ? 0o444 : 0o600);
    const info = await stat(path); assert.equal(info.mode & 0o022, 0);
    if (process.platform !== 'win32') assert.equal(info.uid, 0);
  }
  const first = randomBytes(32).toString('hex'), second = randomBytes(32).toString('hex');
  await protectedIdentity(identity, first);
  const observed = await f.request('/healthz');
  assert.equal(observed.status, 200); assert.match(observed.body.access.installationId, /^[a-f0-9]{64}$/);
  assert.equal(observed.body.access.installationId, first);
  await protectedIdentity(replacement, second);
  await chmod(identity, 0o600); await rename(replacement, identity);
  const replaced = await f.request('/healthz');
  assert.equal(replaced.status, 200); assert.equal(replaced.body.access.installationId, first);
  assert.equal(f.server.accessRuntime.installationId, first);
});
