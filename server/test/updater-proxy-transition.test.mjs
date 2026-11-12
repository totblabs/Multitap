// Prepared, never executed by this agent. Pure request admission, fake HTTP/TLS
// request objects, and one owned temporary env file. No listener/daemon/SSH.
import test from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { mkdtemp, writeFile, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve, dirname, basename } from 'node:path';
import * as health from '../update/deployment-health.mjs';
import { legacyServerAccess } from '../update/server-access-policy.mjs';
import { resolveRequestAccess } from '../src/request-origin.mjs';

const origin = 'https://public-upgrade.invalid';
const legacy = 'MULTITAP_PUBLIC_ORIGIN=' + origin + '\nMULTITAP_HOST=0.0.0.0\nMULTITAP_PORT=18480\n';
const proxyPeer = '192.0.2.25'; // Synthetic test identity, never an installed address.
const explicit = legacy + 'MULTITAP_TRUSTED_PROXIES=' + proxyPeer + '\n';
// Let the same test reproduce baseline behavior, not just a missing-export
// import failure. These fallbacks are exactly the previous CLI/read behavior.
const preflight = text => (health.deploymentEnvironmentForUpdate ?? health.deploymentEnvironment)(text);
const readyHealth = (config, options) => (health.deploymentBrokerHealth ?? health.localBrokerHealth)(config, options);
const baseBody = () => ({ service: 'multitap-broker', status: 'ok', version: '0.5.84',
  transport: { protocol: 'broker-transport-v1', publicOrigin: origin, scheme: 'https', allowInsecureHttp: false, directTls: false },
  access: { protocol: 'server-access-v1', origin, revision: 'a'.repeat(64), installationId: 'b'.repeat(64), bootstrapOnly: false } });

function request(config, peer = proxyPeer) {
  const host = new URL(origin).host;
  return { method: 'GET', url: '/admin', headers: { host, 'x-forwarded-proto': 'https' }, rawHeaders: ['Host', host],
    socket: { encrypted: false, localPort: config.https.listenPort, localAddress: '192.0.2.40', remoteAddress: peer } };
}
function wire({ local = baseBody(), remote = baseBody(), status = 200, tlsError = false } = {}) {
  const calls = [];
  const factory = tls => (options, callback) => {
    calls.push({ tls, options });
    const req = new EventEmitter(); req.destroy = () => {};
    req.end = () => queueMicrotask(() => {
      if (tls && tlsError) { req.emit('error', Object.assign(Error('synthetic TLS refusal'), { code: 'CERT_HAS_EXPIRED' })); return; }
      const res = new EventEmitter(); res.statusCode = tls ? status : 200; res.destroy = () => {};
      res.headers = { location: 'https://must-not-follow.invalid/healthz' };
      callback(res);
      res.emit('data', Buffer.from(JSON.stringify(tls ? remote : local))); res.emit('end');
    });
    return req;
  };
  return { calls, http: factory(false), https: factory(true) };
}

test('83 to 84: unconfigured proxy transition is blocked rather than trusting forwarded HTTPS or enabling HTTP', () => {
  const runtime = legacyServerAccess(health.deploymentEnvironmentValues(legacy));
  assert.deepEqual(runtime.trustedProxies, ['127.0.0.1', '::1']);
  assert.equal(runtime.http.enabled, false);
  assert.throws(() => resolveRequestAccess(request(runtime), runtime), e => e.code === 'http_disabled');
  assert.throws(() => preflight(legacy), /update_proxy_configuration_required/);
  for (const value of ['', 'none', '192.0.2.0/24', '*', '192.0.2.25,not-an-IP']) {
    assert.throws(() => preflight(legacy + 'MULTITAP_TRUSTED_PROXIES=' + value + '\n'), /update_proxy_configuration_required/);
  }
});

test('the protected environment reader used by released 83 provisioner fences before its first apply step', async t => {
  const dir = await mkdtemp(join(tmpdir(), 'multitap-upgrade-proxy-'));
  t.after(() => {
    const absolute = resolve(dir);
    assert.equal(dirname(absolute), resolve(tmpdir()));
    assert.ok(basename(absolute).startsWith('multitap-upgrade-proxy-'));
    return rm(absolute, { recursive: true, force: true });
  });
  const file = join(dir, 'broker.env'); await writeFile(file, legacy, { mode: 0o600 });
  let applyCalls = 0;
  const releasedPreflight = async () => {
    const config = await health.readDeploymentEnvironment(file);
    applyCalls++; // Substituted first stop/link/bootstrap action; never an OS call.
    return config;
  };
  await assert.rejects(releasedPreflight(), /update_proxy_configuration_required/);
  assert.equal(applyCalls, 0); assert.equal(await readFile(file, 'utf8'), legacy);
});

test('only explicit approved installation proxy IPs are preserved, without broad trust or an HTTP change', () => {
  assert.equal(preflight(explicit).publicOrigin, origin);
  const runtime = legacyServerAccess(health.deploymentEnvironmentValues(explicit));
  assert.deepEqual(runtime.trustedProxies, [proxyPeer]);
  assert.equal(runtime.http.enabled, false);
  assert.equal(resolveRequestAccess(request(runtime), runtime).origin, origin);
  assert.throws(() => resolveRequestAccess(request(runtime, '192.0.2.26'), runtime), e => e.code === 'http_disabled');
});

test('local healthProbeOnly remains a liveness result but cannot hide public HTTPS 403', async () => {
  const local = baseBody(); local.access.healthProbeOnly = true;
  const fake = wire({ local, status: 403 }), config = preflight(explicit);
  assert.equal((await health.localBrokerHealth(config, fake)).version, '0.5.84');
  fake.calls.length = 0;
  assert.equal(await readyHealth(config, fake), null);
  assert.equal(fake.calls.length, 2);
  assert.equal(fake.calls[1].tls, true);
  assert.equal(fake.calls[1].options.hostname, 'public-upgrade.invalid');
});

test('successful deployment health needs actual configured TLS origin and local revision/installation identity', async () => {
  const local = baseBody(); local.access.healthProbeOnly = true;
  const fake = wire({ local });
  assert.deepEqual(await readyHealth(preflight(explicit), fake), { service: 'multitap-broker', status: 'ok', version: '0.5.84' });
  assert.equal(fake.calls.length, 2);
  const options = fake.calls[1].options;
  assert.equal(options.hostname, 'public-upgrade.invalid'); assert.equal(options.port, 443);
  assert.equal(options.path, '/healthz'); assert.equal(options.rejectUnauthorized, true);
  assert.equal(options.servername, 'public-upgrade.invalid'); assert.equal(options.agent, false);
  assert.equal(typeof options.checkServerIdentity, 'function');
  assert.deepEqual(options.headers, { Host: 'public-upgrade.invalid', Accept: 'application/json' });
  for (const change of [body => { body.access.healthProbeOnly = true; }, body => { body.access.revision = 'c'.repeat(64); },
    body => { body.access.installationId = 'd'.repeat(64); }, body => { body.access.origin = 'https://other.invalid'; },
    body => { body.transport.scheme = 'http'; }, body => { body.version = '0.5.83'; }]) {
    const remote = baseBody(); change(remote);
    assert.equal(await readyHealth(preflight(explicit), wire({ local, remote })), null);
  }
});

test('redirect and untrusted TLS never fall back to a raw local ready result', async () => {
  for (const options of [{ status: 302 }, { tlsError: true }]) {
    const fake = wire(options);
    assert.equal(await readyHealth(preflight(explicit), fake), null);
    assert.equal(fake.calls.filter(c => c.tls).length, 1);
    assert.equal(fake.calls.filter(c => !c.tls).length, 1);
    assert.ok(fake.calls.every(c => c.options.path === '/healthz'));
    assert.equal(fake.calls.some(c => c.options.hostname === 'must-not-follow.invalid'), false);
  }
});

test('explicit direct TLS and explicit HTTP keep their existing paths and never gain proxy trust', () => {
  const direct = legacy + 'MULTITAP_TLS_MODE=direct\nMULTITAP_TLS_CERT_FILE=/fixture/cert.pem\nMULTITAP_TLS_KEY_FILE=/fixture/key.pem\n';
  assert.equal(preflight(direct).tlsMode, 'direct');
  const plain = 'MULTITAP_PUBLIC_ORIGIN=http://192.0.2.40:18480\nMULTITAP_HOST=0.0.0.0\nMULTITAP_PORT=18480\nMULTITAP_ALLOW_INSECURE_HTTP=1\n';
  assert.equal(preflight(plain).allowInsecureHttp, true);
  assert.throws(() => preflight(plain.replace('MULTITAP_ALLOW_INSECURE_HTTP=1\n', '')), /installation_invalid/);
  assert.equal(preflight('MULTITAP_HOST=127.0.0.1\nMULTITAP_PORT=18480\n').bootstrapOnly, true);
});
