import test from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { createServer, request as httpRequest } from 'node:http';
import { randomBytes } from 'node:crypto';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { ServerAccessDeployment } from '../update/server-access-deployment.mjs';
import * as deploymentHealth from '../update/deployment-health.mjs';
import * as dockerHealth from '../update/docker.mjs';
import { normalizeServerAccess, serverAccessHash, serverAccessUnconfiguredRevision } from '../update/server-access-policy.mjs';
import { publicAccessRequest, validAccessHealth, localAccessRequest } from '../update/server-access-health.mjs';
const config = normalizeServerAccess({ protocol: 'server-access-v1', primaryOrigin: 'https://alpha.example:8443', https: { enabled: true, origin: 'https://alpha.example:8443', mode: 'proxy', listenHost: '127.0.0.1', listenPort: 18480 }, http: { enabled: false, origin: null, listenHost: '127.0.0.1', listenPort: 18481 }, trustedProxies: ['127.0.0.1'] });
const identity = 'a'.repeat(64), expected = { config, revision: serverAccessHash(config), installationId: identity, origin: config.primaryOrigin };
function response(value, status = 200) {
  const calls = [];
  const request = (options, callback) => {
    calls.push(options); const req = new EventEmitter(); req.destroy = () => {}; req.end = () => queueMicrotask(() => {
      const res = new EventEmitter(); res.statusCode = status; res.destroy = () => {}; callback(res);
      if (status === 200) { res.emit('data', Buffer.from(JSON.stringify(value))); res.emit('end'); }
    }); return req;
  };
  return { calls, request };
}
test('public route proof requires actual revision/installation/origin and rejects legacy health-only metadata', () => {
  const good = { service: 'multitap-broker', status: 'ok', authentication: 'id-password', version: '0.5.84', access: { protocol: 'server-access-v1', revision: expected.revision, installationId: identity, origin: config.primaryOrigin, primaryOrigin: config.primaryOrigin, allowedOrigins: [config.primaryOrigin], bootstrapOnly: false } };
  assert.equal(validAccessHealth(good, expected), true);
  for (const patch of [{ revision: '0'.repeat(64) }, { installationId: 'b'.repeat(64) }, { origin: 'http://alpha.example:8443' }, { healthProbeOnly: true }, { bootstrapOnly: true }]) assert.equal(validAccessHealth({ ...good, access: { ...good.access, ...patch } }, expected), false);
  assert.equal(validAccessHealth({ ...good, access: { ...good.access, healthProbeOnly: true } }, { ...expected, allowHealthProbeOnly: true }), true, 'only private fixed local recovery may accept the compatibility proof');
});
test('external checks never follow redirects, probe unconfigured origins or relax TLS; old probe lacks the new native user agent', async () => {
  const fake = response({ ok: true });
  assert.equal(await publicAccessRequest(config, 'https://other.example', { https: fake.request }), null); assert.equal(fake.calls.length, 0);
  await publicAccessRequest(config, config.primaryOrigin, { https: fake.request });
  assert.equal(fake.calls[0].rejectUnauthorized, true); assert.equal(fake.calls[0].path, '/healthz'); assert.equal(fake.calls[0].headers['User-Agent'], 'Multitap-Access-Verification/1'); assert.equal(fake.calls[0].headers.Authorization, undefined);
  const redirect = response({}, 302); assert.equal(await publicAccessRequest(config, config.primaryOrigin, { https: redirect.request }), null); assert.equal(redirect.calls.length, 1);
  const local = response({}); await localAccessRequest({ publicOrigin: config.primaryOrigin, host: '0.0.0.0', port: 18480, tlsMode: 'proxy' }, { http: local.request, activityToken: 'e'.repeat(64) });
  assert.equal(local.calls[0].path, '/internal/server-access/activity'); assert.equal(local.calls[0].headers.Host, '127.0.0.1:18480'); assert.equal(local.calls[0].headers['x-multitap-access-fence'], 'e'.repeat(64)); assert.equal(local.calls[0].headers['User-Agent'], undefined);
  assert.equal(local.calls[0].method, 'GET');
});

test('ending the broker\'s connections is one fenced loopback POST, never sent without the fence, and Docker passes it through its helper', async () => {
  const transport = { publicOrigin: config.primaryOrigin, host: '0.0.0.0', port: 18480, tlsMode: 'proxy' }, fence = 'e'.repeat(64);
  const ending = response({ protocol: 'server-access-connections-v1', fence: true, ended: { people: 1, sessions: 1, consoles: 0 } });
  assert.deepEqual((await localAccessRequest(transport, { http: ending.request, activityToken: fence, endConnections: true })).ended, { people: 1, sessions: 1, consoles: 0 });
  assert.equal(ending.calls.length, 1); assert.equal(ending.calls[0].path, '/internal/server-access/end-connections'); assert.equal(ending.calls[0].method, 'POST');
  assert.equal(ending.calls[0].headers.Host, '127.0.0.1:18480'); assert.equal(ending.calls[0].headers['x-multitap-access-fence'], fence); assert.equal(ending.calls[0].headers['Content-Length'], 0);
  for (const options of [{ endConnections: true }, { endConnections: true, activityToken: 'not-a-fence' }, { endConnections: 'yes', activityToken: fence }]) {
    const never = response({}); assert.equal(await localAccessRequest(transport, { http: never.request, ...options }), null); assert.equal(never.calls.length, 0);
  }
  const requests = [];
  const systemd = new ServerAccessDeployment({ adapter: { platform: 'linux-x64-systemd' }, directory: '/unused-memory-fixture', fixture: { id: '0123456789abcdef' },
    run: () => assert.fail('no service command'), localRequest: async (target, options) => { requests.push({ target, options }); return { protocol: 'server-access-connections-v1', fence: true }; } });
  await systemd.activity({ transport }, fence); await systemd.endConnections({ transport }, fence);
  assert.deepEqual(requests.map(row => row.options), [{ activityToken: fence }, { activityToken: fence, endConnections: true }]);
  assert.equal(await systemd.endConnections({ transport }, undefined), null); assert.equal(requests.length, 2, 'no fence, no request');
  const inputs = [];
  const docker = new ServerAccessDeployment({ adapter: { platform: 'linux-x64-docker', runInput: async (args, bytes) => {
    assert.deepEqual(args, ['exec', '--interactive', 'synthetic-owned-container', '/usr/local/bin/node', '/app/update/server-access-health.mjs', '--local-probe']);
    inputs.push(JSON.parse(bytes.toString('utf8'))); return JSON.stringify({ protocol: 'server-access-connections-v1', fence: true });
  } }, directory: '/unused-memory-fixture', run: () => assert.fail('no real Docker command') });
  const current = { config: null, internalPort: 18480, containerId: 'synthetic-owned-container' };
  await docker.activity(current, fence); assert.equal((await docker.endConnections(current, fence)).fence, true);
  assert.deepEqual(inputs, [{ config: null, port: 18480, activityToken: fence }, { config: null, port: 18480, activityToken: fence, endConnections: true }]);
});

test('minimal public health binds normalized policy hash without exposing either raw URL field', () => {
  const good = { service: 'multitap-broker', status: 'ok', authentication: 'id-password', version: '0.5.84', access: {
    protocol: 'server-access-v1', revision: expected.revision, installationId: identity, origin: config.primaryOrigin, bootstrapOnly: false,
  } };
  assert.equal(validAccessHealth(good, expected), true);
  const other = normalizeServerAccess({ ...config, http: { ...config.http, enabled: true, origin: 'http://192.0.2.10:18481' } });
  assert.equal(validAccessHealth(good, { ...expected, config: other }), false, 'expected revision must hash the actual expected policy');
  assert.equal(validAccessHealth(good, { ...expected, origin: 'https://other.example' }), false, 'expected origin must be in Root-owned policy');
  assert.equal(validAccessHealth(good, { ...expected, config: {} }), false);
  for (const patch of [{ revision: '0'.repeat(64) }, { installationId: 'b'.repeat(64) }, { origin: 'http://alpha.example:8443' },
    { bootstrapOnly: true }, { healthProbeOnly: true }, { healthProbeOnly: 'true' },
    { primaryOrigin: 'http://192.0.2.10:18481' }, { allowedOrigins: [] }, { allowedOrigins: [config.primaryOrigin, config.primaryOrigin] }]) {
    assert.equal(validAccessHealth({ ...good, access: { ...good.access, ...patch } }, expected), false);
  }
  assert.equal(validAccessHealth({ ...good, access: { ...good.access, healthProbeOnly: true } }, { ...expected, allowHealthProbeOnly: true }), true);
  const localTransport = { publicOrigin: null, bootstrapOnly: true, tlsMode: 'proxy', tlsFiles: null, host: '127.0.0.1', port: 18500 }, origin = 'http://127.0.0.1:18500';
  const bootstrap = { ...good, transport: { protocol: 'broker-transport-v1', publicOrigin: origin, scheme: 'http', allowInsecureHttp: false, directTls: false },
    access: { ...good.access, revision: serverAccessUnconfiguredRevision, origin, bootstrapOnly: true } };
  const localExpected = { config: null, revision: serverAccessUnconfiguredRevision, installationId: identity, origin, localTransport, allowHealthProbeOnly: true };
  assert.equal(validAccessHealth(bootstrap, localExpected), true);
  assert.equal(validAccessHealth(bootstrap, { ...localExpected, revision: expected.revision }), false);
  assert.equal(validAccessHealth(bootstrap, { ...localExpected, origin: null }), false, 'the receiver never reports synthetic null origin');
  assert.equal(validAccessHealth(bootstrap, { ...localExpected, localTransport: undefined }), false, 'bootstrap has no public expected origin without Root receiver metadata');
});

// Actual loopback HTTP with the configured trusted-proxy contract. HTTPS here
// means the admitted public proxy origin, not a TLS handshake; existing TLS
// matrix cases remain the TLS/CA/redirect witnesses. No external address opens.
async function listen(server, port) {
  await new Promise((done, fail) => { const failed = error => { server.off('listening', ready); fail(error); };
    const ready = () => { server.off('error', failed); done(); };
    server.once('error', failed); server.once('listening', ready); server.listen(port, '127.0.0.1'); });
}
test('actual public health and root JSON disclose only their admitted origin and actual policy revision', { timeout: 15000 }, async t => {
  const reservation = createServer(); await listen(reservation, 0); const port = reservation.address().port;
  await new Promise(done => reservation.close(done)); // bind race is a setup failure, never retried
  const current = normalizeServerAccess({ protocol: 'server-access-v1', primaryOrigin: 'https://public-fixture.invalid:8443',
    https: { enabled: true, origin: 'https://public-fixture.invalid:8443', mode: 'proxy', listenHost: '127.0.0.1', listenPort: port },
    http: { enabled: true, origin: 'http://192.0.2.10:18481', listenHost: '127.0.0.1', listenPort: port }, trustedProxies: ['127.0.0.1'] });
  const revision = serverAccessHash(current), store = new Store(':memory:'), sockets = new Set(); let server;
  t.after(async () => { try { for (const socket of sockets) socket.destroy(); if (server) await new Promise(done => server.close(done)); } finally { store.close(); } });
  server = await buildServer({ store, vault: new Vault(randomBytes(32)), publicOrigin: current.primaryOrigin,
    allowInsecureHttp: true, accessConfig: current, accessRevision: '0'.repeat(64), accessInstallationId: identity,
    trustedProxies: ['127.0.0.1'], logger: { error: () => assert.fail('unexpected fixture server error') } });
  server.on('connection', socket => { sockets.add(socket); socket.once('close', () => sockets.delete(socket)); }); await listen(server, port);
  async function request(path, origin) {
    return new Promise((done, fail) => {
      const target = new URL(origin); const req = httpRequest({ hostname: '127.0.0.1', port, path, method: 'GET', agent: false, timeout: 3000,
        headers: { Host: target.host, Accept: 'application/json', 'User-Agent': 'Synthetic-public-health-test', 'X-Forwarded-Proto': target.protocol.slice(0, -1) } }, res => {
          const chunks = []; let size = 0; res.on('data', chunk => { size += chunk.length; if (size > 8192) req.destroy(Error('fixture_health_bound')); else chunks.push(chunk); });
          res.once('error', fail); res.once('aborted', () => fail(Error('fixture_health_aborted')));
          res.once('end', () => { try { const raw = Buffer.concat(chunks).toString('utf8'); done({ status: res.statusCode, raw, body: JSON.parse(raw) }); } catch (error) { fail(error); } });
        });
      req.once('error', fail); req.once('timeout', () => req.destroy(Error('fixture_health_timeout'))); req.end();
    });
  }
  for (const origin of [current.https.origin, current.http.origin]) for (const path of ['/healthz', '/']) {
    const reply = await request(path, origin), scheme = new URL(origin).protocol.slice(0, -1);
    assert.equal(reply.status, 200); assert.equal(reply.body.access.origin, origin); assert.equal(reply.body.access.revision, revision, 'hash is derived from actual policy, not the injected label');
    assert.equal(reply.body.access.installationId, identity); assert.notEqual(reply.body.access.healthProbeOnly, true);
    assert.equal(Object.hasOwn(reply.body.access, 'primaryOrigin'), false); assert.equal(Object.hasOwn(reply.body.access, 'allowedOrigins'), false);
    const other = origin === current.https.origin ? current.http.origin : current.https.origin; assert.equal(reply.raw.includes(other), false);
    assert.deepEqual(reply.body.transport, { protocol: 'broker-transport-v1', publicOrigin: origin, scheme, allowInsecureHttp: scheme === 'http', directTls: false });
    assert.equal(validAccessHealth(reply.body, { config: current, revision, installationId: identity, origin }), true);
  }
});

test('actual TCP bootstrap local health binds restore to the owned receiver and internal Docker port', { timeout: 15000 }, async t => {
  const reservation = createServer(); await listen(reservation, 0); const port = reservation.address().port;
  await new Promise(done => reservation.close(done));
  const store = new Store(':memory:'), sockets = new Set(); let server;
  t.after(async () => { try { for (const socket of sockets) socket.destroy(); if (server) await new Promise(done => server.close(done)); } finally { store.close(); } });
  server = await buildServer({ store, vault: new Vault(randomBytes(32)), publicOrigin: null, bootstrap: true,
    accessConfig: null, accessRevision: serverAccessUnconfiguredRevision, accessInstallationId: identity,
    trustedProxies: [], logger: { error: () => assert.fail('unexpected bootstrap fixture server error') } });
  server.on('connection', socket => { sockets.add(socket); socket.once('close', () => sockets.delete(socket)); }); await listen(server, port);
  const localTransport = { publicOrigin: null, tlsMode: 'proxy', tlsFiles: null, bootstrapOnly: true, host: '127.0.0.1', port };
  const origin = 'http://127.0.0.1:' + port, value = await localAccessRequest(localTransport);
  assert.equal(value?.access.origin, origin); assert.equal(value.access.bootstrapOnly, true); assert.notEqual(value.access.healthProbeOnly, true);
  const localExpected = { config: null, revision: serverAccessUnconfiguredRevision, installationId: identity, origin, localTransport, allowHealthProbeOnly: true };
  const previous = { revision: serverAccessUnconfiguredRevision };
  const systemd = new ServerAccessDeployment({ adapter: { platform: 'linux-x64-systemd' }, directory: '/unused-bootstrap-memory-fixture', fixture: { id: '0123456789abcdef' },
    run: () => assert.fail('fixture must never execute service or filesystem commands') });
  systemd.installationId = identity;
  systemd.current = async () => ({ config: null, revision: serverAccessUnconfiguredRevision, transport: localTransport });
  const mappedPort = port === 65535 ? 65534 : 65535; let containerCalls = 0;
  const docker = new ServerAccessDeployment({ adapter: { platform: 'linux-x64-docker', runInput: async (args, bytes) => {
    containerCalls++; assert.deepEqual(args, ['exec', '--interactive', 'synthetic-owned-container', '/usr/local/bin/node', '/app/update/server-access-health.mjs', '--local-probe']);
    const input = JSON.parse(bytes.toString('utf8')); assert.deepEqual(input, { config: null, port });
    // Inject only the container-exec boundary. The health request itself uses
    // the actual production helper/TCP receiver, never a fabricated response.
    return JSON.stringify(await localAccessRequest({ ...localTransport, port: input.port }));
  } }, directory: '/unused-bootstrap-memory-fixture', run: () => assert.fail('no real Docker/service command') });
  docker.installationId = identity;
  docker.current = async () => ({ config: null, revision: serverAccessUnconfiguredRevision,
    transport: { ...localTransport, port: mappedPort }, internalPort: port, containerId: 'synthetic-owned-container' });
  const outcomes = { direct: validAccessHealth(value, localExpected), basic: (await deploymentHealth.localBrokerHealth(localTransport))?.version === value.version,
    systemdRestore: await systemd.unchangedAfterRestart(previous), dockerRestore: await docker.unchangedAfterRestart(previous) };
  console.info('bootstrap-local-health', JSON.stringify(outcomes));
  assert.deepEqual(outcomes, { direct: true, basic: true, systemdRestore: true, dockerRestore: true });
  assert.equal(containerCalls, 1);
  for (const patch of [{ origin: null }, { localTransport: undefined }, { allowHealthProbeOnly: false }, { installationId: 'b'.repeat(64) },
    { localTransport: { ...localTransport, port: mappedPort } }, { origin: 'http://untrusted-fixture.invalid:' + port }]) {
    assert.equal(validAccessHealth(value, { ...localExpected, ...patch }), false);
  }
  assert.equal(validAccessHealth({ ...value, access: { ...value.access, origin: 'http://untrusted-fixture.invalid:' + port } }, localExpected), false);
  assert.equal(validAccessHealth({ ...value, access: { ...value.access, bootstrapOnly: false } }, localExpected), false);
  const options = deploymentHealth.localHealthOptions(localTransport, { internalAuthority: true });
  assert.equal(options.options.headers.Host, '127.0.0.1:' + port); assert.equal(options.options.path, '/healthz');
});

test('Docker fallback minimal metadata uses actual origin and preserves revision and bootstrap receiver checks', () => {
  const value = { service: 'multitap-broker', status: 'ok', authentication: 'id-password', version: '0.5.84', access: {
    protocol: 'server-access-v1', revision: expected.revision, origin: config.primaryOrigin, bootstrapOnly: false } };
  assert.equal(dockerHealth.dockerAccessHealthVersion(value, config), value.version);
  for (const patch of [{ origin: 'http://untrusted-fixture.invalid' }, { revision: '0'.repeat(64) }, { protocol: 'unknown' }, { bootstrapOnly: true },
    { primaryOrigin: 'http://untrusted-fixture.invalid' }]) assert.equal(dockerHealth.dockerAccessHealthVersion({ ...value, access: { ...value.access, ...patch } }, config), null);
  const transport = { publicOrigin: null, tlsMode: 'proxy', tlsFiles: null, bootstrapOnly: true, host: '127.0.0.1', port: 18500 }, origin = 'http://127.0.0.1:18500';
  const bootstrap = { ...value, transport: { protocol: 'broker-transport-v1', publicOrigin: origin, scheme: 'http', allowInsecureHttp: false, directTls: false },
    access: { protocol: 'server-access-v1', revision: serverAccessUnconfiguredRevision, origin, bootstrapOnly: true } };
  assert.equal(dockerHealth.dockerAccessHealthVersion(bootstrap, null, transport), value.version);
  assert.equal(dockerHealth.dockerAccessHealthVersion(bootstrap, null), null);
  assert.equal(dockerHealth.dockerAccessHealthVersion(bootstrap, null, { ...transport, port: 18501 }), null);
});
