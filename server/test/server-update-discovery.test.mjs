import { test } from 'node:test';
import assert from 'node:assert/strict';
import { generateKeyPairSync, sign, randomUUID } from 'node:crypto';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { ServerUpdateClient } from '../src/server-update.mjs';
import { UpdateEngine } from '../update/engine.mjs';
import { adminRoutes } from '../src/admin-http.mjs';
import { HttpError } from '../src/errors.mjs';

const tenMinutes = 10 * 60 * 1000;
function fixture() {
  let now = Date.parse('2026-10-28T01:00:00.000Z');
  const client = new ServerUpdateClient('/run/fixture-update/control.sock', { now: () => now });
  const calls = [], result = { state: 'idle', feedConfigured: true, applySupported: true, currentVersion: '0.5.70' };
  client.request = async (path, input, options) => {
    assert.ok(options.timeoutMs > 0 && options.timeoutMs <= 4000, 'one discovery has a bounded deadline');
    calls.push({ path, input });
    if (path === '/status') return { ...result };
    assert.equal(path, '/check', 'discovery cannot call apply or fetch an archive');
    assert.deepEqual(input, {}); result.state = 'checking'; return { accepted: true };
  };
  return { client, calls, result, time: () => now, advance: ms => { now += ms; } };
}

test('automatic discovery deduplicates administrators, caches metadata ten minutes and never installs', async () => {
  const f = fixture(), a = f.client.discover(), b = f.client.discover();
  assert.equal(a, b); assert.equal((await a).state, 'checking');
  assert.deepEqual(f.calls.map(c => c.path), ['/status', '/check', '/status']);
  await f.client.discover(); assert.equal(f.calls.filter(c => c.path === '/check').length, 1, 'checking cannot enqueue another check');
  Object.assign(f.result, { state: 'available', checkedAt: new Date(f.time()).toISOString() });
  f.advance(tenMinutes - 1); await f.client.discover(); assert.equal(f.calls.filter(c => c.path === '/check').length, 1);
  f.advance(1); await f.client.discover(); assert.equal(f.calls.filter(c => c.path === '/check').length, 2);
});

test('metadata discovery rechecks the initiating administrator after status and before enqueueing a check', async () => {
  for (const status of [401, 403]) {
    const f = fixture();
    await assert.rejects(f.client.discover({ authorize: () => { throw new HttpError(status, 'authentication_required'); } }), error => error.status === status);
    assert.deepEqual(f.calls.map(c => c.path), ['/status'], 'revoked authority must not enqueue /check');
    await f.client.discover({ authorize: () => {} });
    assert.equal(f.calls.filter(c => c.path === '/check').length, 1, 'a cancelled principal cannot delay the next valid administrator');
  }
});

test('discovery keeps operation failures and recovery visible and gives successful completion time to reach the console', async () => {
  for (const state of ['downloading', 'verifying', 'backing_up', 'applying', 'restarting', 'health_check', 'interrupted', 'rolled_back', 'recovery_required', 'failed']) {
    const f = fixture(); Object.assign(f.result, { state, operationId: 'operation', errorCode: 'update_health_failed' });
    assert.deepEqual(await f.client.discover(), f.result); assert.deepEqual(f.calls.map(c => c.path), ['/status'], state);
  }
  const earlyFailure = fixture(); Object.assign(earlyFailure.result, { state: 'failed', latest: { version: '0.5.71' }, releaseId: 'a'.repeat(64), errorCode: 'update_disk_space' });
  assert.deepEqual(await earlyFailure.client.discover(), earlyFailure.result); assert.deepEqual(earlyFailure.calls.map(c => c.path), ['/status'], 'an apply failure before operationId is also preserved');
  const interrupted = fixture(); Object.assign(interrupted.result, { state: 'failed', errorCode: 'update_interrupted' });
  await interrupted.client.discover(); assert.deepEqual(interrupted.calls.map(c => c.path), ['/status'], 'an interrupted check is left for explicit review');
  const f = fixture(); Object.assign(f.result, { state: 'succeeded', operationId: 'finished', checkedAt: new Date(f.time() - tenMinutes).toISOString() });
  assert.equal((await f.client.discover()).state, 'succeeded');
  f.advance(tenMinutes - 1); assert.equal((await f.client.discover()).state, 'succeeded');
  f.advance(1); assert.equal((await f.client.discover()).state, 'checking');
  assert.equal(f.calls.filter(c => c.path === '/check').length, 1);
});

test('failed metadata checks back off for a minute, concurrent manual checking is reread and unconfigured feeds stay passive', async () => {
  const f = fixture(); Object.assign(f.result, { state: 'failed', checkedAt: new Date(f.time()).toISOString(), errorCode: 'update_signature_invalid' });
  f.advance(59999); assert.equal((await f.client.discover()).state, 'failed');
  f.advance(1); assert.equal((await f.client.discover()).state, 'checking');
  const unconfigured = fixture(); unconfigured.result.feedConfigured = false; await unconfigured.client.discover();
  assert.deepEqual(unconfigured.calls.map(c => c.path), ['/status']);
  const race = fixture(), original = race.client.request;
  race.client.request = async (path, ...args) => { if (path === '/check') { race.result.state = 'downloading'; throw new HttpError(409, 'update_busy'); } return original(path, ...args); };
  assert.equal((await race.client.discover()).state, 'downloading');
  const unavailable = fixture(); let attempts = 0;
  unavailable.client.check = async () => { attempts++; throw new HttpError(503, 'server_update_unavailable'); };
  await assert.rejects(unavailable.client.discover(), error => error.code === 'server_update_unavailable');
  await unavailable.client.discover(); assert.equal(attempts, 1);
  unavailable.advance(60000); await assert.rejects(unavailable.client.discover()); assert.equal(attempts, 2);
});

test('the local status transport has an absolute deadline even when the peer keeps trickling data', async t => {
  const socket = process.platform === 'win32' ? '\\\\.\\pipe\\multitap-update-fixture-' + randomUUID() : join(tmpdir(), 'multitap-update-' + randomUUID() + '.sock');
  const server = createServer((_req, res) => {
    res.writeHead(200, { 'Content-Type': 'application/json' }); res.write('{');
    const timer = setInterval(() => res.write(' '), 10);
    res.on('close', () => clearInterval(timer));
  });
  await new Promise((resolve, reject) => { server.once('error', reject); server.listen(socket, resolve); });
  t.after(async () => { server.closeAllConnections(); await new Promise(resolve => server.close(resolve)); });
  const client = new ServerUpdateClient('/run/fixture-update/control.sock');
  // The production path validator stays Unix-only. A test-only named pipe lets
  // the exact HTTP transport run on Windows as well as the Linux socket fixture.
  client.socketPath = socket;
  const start = performance.now();
  await assert.rejects(client.request('/status', undefined, { timeoutMs: 80 }), error => error.code === 'server_update_unavailable');
  assert.ok(performance.now() - start < 2000, 'continual response data must not defeat the deadline');
});

test('discovery metadata passes the actual Ed25519 verifier and cannot offer a changed signature or another deployment', async () => {
  const keys = generateKeyPairSync('ed25519'), publicKey = keys.publicKey.export({ type: 'spki', format: 'pem' });
  const release = { schema: 1, product: 'multitap-broker', version: '0.5.71', platform: 'linux-x64-systemd', releasedAt: '2026-10-28T00:00:00.000Z', changelog: 'Verified metadata fixture', artifact: { path: '/downloads/Multitap-Server-0.5.71-linux-x64.tar.gz', size: 1234, sha256: 'a'.repeat(64) } };
  for (const kind of ['valid', 'changed', 'other-platform']) {
    const f = fixture(), requests = [], offered = { ...release, platform: kind === 'other-platform' ? 'linux-x64-docker' : release.platform, artifact: { ...release.artifact, path: kind === 'other-platform' ? '/downloads/Multitap-Server-0.5.71-linux-x64-docker.tar.gz' : release.artifact.path } };
    const envelope = { release: offered, signature: sign(null, Buffer.from(JSON.stringify(offered)), keys.privateKey).toString('base64') };
    if (kind === 'changed') envelope.release.changelog += 'changed';
    const engine = new UpdateEngine({ directory: '/fixture-unused', feed: 'https://updates.fixture.invalid/feed.json', publicKey, adapter: { platform: 'linux-x64-systemd', version: async () => '0.5.70', prepare: () => assert.fail('metadata must not prepare an install'), apply: () => assert.fail('metadata must not install') }, fetcher: async url => { requests.push(String(url)); return new Response(JSON.stringify(envelope)); } });
    // Storage durability is covered by the update engine's integration tests.
    // Here the real verification/check pipeline runs without touching a server.
    engine.set = async values => { engine.state = { ...engine.state, ...values }; };
    f.client.request = async path => { if (path === '/status') return engine.status(); assert.equal(path, '/check'); return engine.check(); };
    await f.client.discover(); await engine.task;
    const result = await f.client.discover();
    assert.equal(result.state, kind === 'valid' ? 'available' : 'failed');
    if (kind === 'valid') { assert.equal(result.latest.version, '0.5.71'); assert.match(result.releaseId, /^[a-f0-9]{64}$/); }
    else { assert.equal(result.latest, undefined); assert.equal(result.errorCode, kind === 'changed' ? 'update_signature_invalid' : 'update_platform_unsupported'); }
    assert.deepEqual(requests, ['https://updates.fixture.invalid/feed.json']);
  }
});

test('GET server-info invokes metadata discovery only for the authenticated current administrator', async () => {
  const user = { id: 'fixture-owner', role: 'admin', must_change_credentials: 0 };
  let authenticated = true, discoveries = 0, sent;
  const routes = adminRoutes({ store: { authenticate: () => authenticated ? user : null }, vault: {}, publicOrigin: 'https://fixture.invalid', serverInfo: { service: 'multitap-broker', version: '0.5.70' }, serverUpdate: { discover: async () => { discoveries++; return { state: 'checking', feedConfigured: true, applySupported: true }; }, status: () => assert.fail('discover is preferred'), apply: () => assert.fail('GET must not install') } });
  const req = { method: 'GET', headers: { cookie: '__Secure-multitap-admin=' + 'a'.repeat(43) } }, res = {};
  const call = () => routes(req, res, '/api/v1/admin/server-info', (status, body) => { sent = { status, body }; });
  await call(); assert.equal(discoveries, 1); assert.equal(sent.status, 200); assert.equal(sent.body.update.state, 'checking');
  authenticated = false; await assert.rejects(call(), error => error.status === 401); assert.equal(discoveries, 1);
  authenticated = true; sent = null;
  const revoked = adminRoutes({ store: { authenticate: () => authenticated ? user : null }, vault: {}, serverInfo: {}, serverUpdate: { discover: async () => { authenticated = false; return { state: 'available' }; } } });
  await assert.rejects(revoked(req, res, '/api/v1/admin/server-info', () => assert.fail('revoked administrator must not receive delayed metadata')), error => error.status === 401);
});

test('a server-info read revoked during the updater status await cannot start the signed feed check', async () => {
  const f = fixture(), user = { id: 'fixture-owner', role: 'admin', must_change_credentials: 0 };
  let authenticated = true;
  const request = f.client.request;
  f.client.request = async (...args) => { const value = await request(...args); authenticated = false; return value; };
  const routes = adminRoutes({ store: { authenticate: () => authenticated ? user : null }, vault: {}, serverInfo: {}, serverUpdate: f.client });
  const req = { method: 'GET', headers: { cookie: '__Secure-multitap-admin=' + 'a'.repeat(43) } };
  await assert.rejects(routes(req, {}, '/api/v1/admin/server-info', () => assert.fail('revoked read cannot return data')), error => error.status === 401);
  assert.deepEqual(f.calls.map(call => call.path), ['/status']);
});
