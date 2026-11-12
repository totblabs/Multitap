import test from 'node:test';
import assert from 'node:assert/strict';
import { normalizeServerAccess, allowedServerOrigins, probedServerOrigins, legacyServerAccess, serverAccessListeners, serverAccessPortMismatches, serverAccessHash, decodeServerAccessState } from '../update/server-access-policy.mjs';
import { configuration } from '../src/config.mjs';
import { resolveRequestAccess } from '../src/request-origin.mjs';

export const accessFixture = () => ({ protocol: 'server-access-v1', primaryOrigin: 'https://alpha.example:8443', https: { enabled: true, origin: 'https://alpha.example:8443', mode: 'proxy', listenHost: '127.0.0.1', listenPort: 18480 }, http: { enabled: false, origin: null, listenHost: '0.0.0.0', listenPort: 18481 }, trustedProxies: ['127.0.0.1'] });
test('access origins, independent ports, primary selection and explicit HTTP consent are exact', () => {
  const input = accessFixture(), first = normalizeServerAccess(input);
  assert.equal(first.https.listenPort, 18480); assert.equal(first.primaryOrigin, 'https://alpha.example:8443'); assert.deepEqual(allowedServerOrigins(first), [first.primaryOrigin]);
  const dual = normalizeServerAccess({ ...input, http: { ...input.http, enabled: true, origin: 'http://alpha.example:8080' }, primaryOrigin: 'http://alpha.example:8080' });
  assert.deepEqual(allowedServerOrigins(dual), ['http://alpha.example:8080', 'https://alpha.example:8443']); assert.equal(dual.http.listenPort, 18481);
  for (const value of [{ ...input, primaryOrigin: 'http://alpha.example:8080' }, { ...input, https: { ...input.https, enabled: false } }, { ...input, trustedProxies: ['192.0.2.0/24'] }, { ...input, trustedProxies: ['private'] }, { ...input, redirect: 'http://other.example' }]) assert.throws(() => normalizeServerAccess(value), /server_access_/);
  for (const origin of ['https://user:secret@alpha.example', 'https://alpha.example/path', 'https://alpha.example?x', 'https://127.1']) assert.throws(() => normalizeServerAccess({ ...input, primaryOrigin: origin, https: { ...input.https, origin } }));
});
test('proxy/direct distinction groups only the same plaintext backend and never mixes TLS on one socket', () => {
  const input = accessFixture(); input.http = { enabled: true, origin: 'http://alpha.example:8080', listenHost: '127.0.0.1', listenPort: 18480 };
  assert.equal(serverAccessListeners(input).length, 1); assert.equal(serverAccessListeners(input)[0].origins.length, 2);
  assert.throws(() => normalizeServerAccess({ ...input, https: { ...input.https, mode: 'direct', certFile: '/supplied/cert.pem', keyFile: '/supplied/key.pem' } }), /listener_conflict/);
  assert.throws(() => normalizeServerAccess({ ...input, https: { ...input.https, certFile: '/supplied/cert.pem' } }), /tls_configuration_invalid/);
});
test('legacy HTTPS keeps its origin/listener without silently trusting private networks', () => {
  const value = legacyServerAccess({ MULTITAP_PUBLIC_ORIGIN: 'https://alpha.example:8443', MULTITAP_HOST: '0.0.0.0', MULTITAP_PORT: '19000' });
  assert.equal(value.primaryOrigin, 'https://alpha.example:8443'); assert.equal(value.https.listenPort, 19000); assert.deepEqual(value.trustedProxies, ['127.0.0.1', '::1']); assert.equal(value.http.enabled, false);
  assert.deepEqual(legacyServerAccess({ MULTITAP_PUBLIC_ORIGIN: 'https://alpha.example', MULTITAP_TRUSTED_PROXIES: '192.0.2.10' }).trustedProxies, ['192.0.2.10']);
});
test('protected saved canonical revision wins over legacy environment; malformed saved state never falls back', () => {
  const config = normalizeServerAccess(accessFixture()), state = { schema: 1, protocol: 'server-access-v1', config, revision: serverAccessHash(config), updatedAt: '2026-10-29T00:00:00.000Z' };
  const read = path => path.endsWith('server-access.identity') ? 'a'.repeat(64) : JSON.stringify(state);
  const result = configuration({ NODE_ENV: 'production', MULTITAP_DATA_DIR: '/fixture', MULTITAP_PUBLIC_ORIGIN: 'https://old.example', MULTITAP_ACCESS_OPERATION_ID: '' }, { readAccessFile: read });
  assert.equal(result.publicOrigin, config.primaryOrigin); assert.equal(result.legacyPublicOrigin, null, 'saved policy never moves an old unscoped cookie to a new primary origin'); assert.equal(result.accessOperationId, null); assert.equal(result.accessInstallationId, 'a'.repeat(64));
  assert.throws(() => decodeServerAccessState({ ...state, revision: '0'.repeat(64) }), /state_invalid/);
  assert.throws(() => configuration({ MULTITAP_PUBLIC_ORIGIN: 'https://old.example' }, { readAccessFile: () => '{bad' }), /state_invalid/);
});

test('a managed root environment cannot reopen legacy endpoints or cookies after policy/identity loss', () => {
  const env = { NODE_ENV: 'production', MULTITAP_DATA_DIR: '/fixture', MULTITAP_PUBLIC_ORIGIN: 'https://old.example', MULTITAP_SERVER_ACCESS_FILE: '/fixture/server-access.json' };
  assert.throws(() => configuration(env, { readAccessFile: path => path.endsWith('.identity') ? 'a'.repeat(64) : null }), /state_missing/);
  assert.throws(() => configuration(env, { readAccessFile: () => null }), /state_missing/);
  const config = normalizeServerAccess(accessFixture()), state = JSON.stringify({ schema: 1, protocol: 'server-access-v1', config, revision: serverAccessHash(config), updatedAt: '2026-10-29T00:00:00.000Z' });
  assert.throws(() => configuration(env, { readAccessFile: path => path.endsWith('.identity') ? null : state }), /identity_missing/);
  const legacy = configuration({ ...env, MULTITAP_SERVER_ACCESS_FILE: undefined }, { readAccessFile: () => null });
  assert.equal(legacy.legacyPublicOrigin, 'https://old.example'); assert.equal(legacy.publicOrigin, 'https://old.example');
  const fresh = configuration({ NODE_ENV: 'production', MULTITAP_DATA_DIR: '/fixture' }, { readAccessFile: () => null });
  assert.equal(fresh.bootstrapOnly, true); assert.equal(fresh.accessInstallationId, null);
});

// access-origins-v1: one card, several exact addresses on its listener.
const lanHttp = () => ({ protocol: 'server-access-v1', primaryOrigin: 'http://192.0.2.10:18480', https: { enabled: false, origin: null, mode: 'proxy', listenHost: '127.0.0.1', listenPort: 18400 }, http: { enabled: true, origin: 'http://192.0.2.10:18480', listenHost: '0.0.0.0', listenPort: 18480 }, trustedProxies: [] });
const withPublic = (...extra) => { const value = lanHttp(); value.http.extraOrigins = extra; return value; };
test('a saved policy without extra addresses keeps its released revision and loads', () => {
  // Revision computed by a released broker's policy code for exactly this file.
  const released = '4cfb69d6374ec278dac060dcc40971cae936d9d765374d1af7fc852e0736d110';
  assert.equal(serverAccessHash(lanHttp()), released);
  assert.equal(serverAccessHash({ ...lanHttp(), http: { ...lanHttp().http, extraOrigins: [] } }), released, 'an empty list is the same policy');
  assert.equal(Object.hasOwn(normalizeServerAccess(lanHttp()).http, 'extraOrigins'), false);
  const state = { schema: 1, protocol: 'server-access-v1', revision: released, config: lanHttp(), updatedAt: '2026-11-03T00:00:00.000Z' };
  assert.deepEqual(decodeServerAccessState(state).config.http, lanHttp().http);
});
test('extra addresses are exact, same-scheme, distinct and bounded; each one is admitted', () => {
  const config = normalizeServerAccess(withPublic('http://203.0.113.7:18480/'));
  assert.deepEqual(config.http.extraOrigins, ['http://203.0.113.7:18480']);
  assert.deepEqual(allowedServerOrigins(config), ['http://192.0.2.10:18480', 'http://203.0.113.7:18480']);
  assert.deepEqual(probedServerOrigins(config), ['http://192.0.2.10:18480'], 'a router-forwarded address is not probed from the server');
  assert.deepEqual(serverAccessListeners(config)[0].origins, ['http://192.0.2.10:18480', 'http://203.0.113.7:18480']);
  const state = { schema: 1, protocol: 'server-access-v1', revision: serverAccessHash(config), config, updatedAt: '2026-11-03T00:00:00.000Z' };
  assert.deepEqual(decodeServerAccessState(JSON.parse(JSON.stringify(state))).config, config);
  for (const extra of [['https://203.0.113.7:18480'], ['http://0.0.0.0:18480'], ['http://[::]:18480'], ['*'], ['http://*.example'], ['http://203.0.113.7:18480/admin'], ['http://user@203.0.113.7'],
    ['http://192.0.2.10:18480'], ['http://203.0.113.7:18480', 'http://203.0.113.7:18480'], ['http://203.0.113.7:1', 'http://203.0.113.7:2', 'http://203.0.113.7:3', 'http://203.0.113.7:4'], [null]]) {
    assert.throws(() => normalizeServerAccess(withPublic(...extra)), /server_access_origin_invalid/, JSON.stringify(extra));
  }
  assert.throws(() => normalizeServerAccess({ ...lanHttp(), http: { ...lanHttp().http, extraOrigins: 'http://203.0.113.7:18480' } }), /server_access_origin_invalid/);
  const off = lanHttp(); off.https = { enabled: true, origin: 'https://vdi.example.com', mode: 'proxy', listenHost: '127.0.0.1', listenPort: 18400 }; off.primaryOrigin = 'https://vdi.example.com';
  off.http = { enabled: false, origin: null, extraOrigins: ['http://203.0.113.7:18480'], listenHost: '0.0.0.0', listenPort: 18480 };
  assert.throws(() => normalizeServerAccess(off), /server_access_origin_invalid/, 'no extra address without a main one');
  // The primary stays a card's main address (the console puts the chosen one first).
  assert.throws(() => normalizeServerAccess({ ...withPublic('http://203.0.113.7:18480'), primaryOrigin: 'http://203.0.113.7:18480' }), /primary_origin_invalid/);
  // HTTPS takes a second domain the same way.
  const secure = normalizeServerAccess({ ...off, http: lanHttp().http, https: { ...off.https, extraOrigins: ['https://vdi2.example.com'] } });
  assert.deepEqual(allowedServerOrigins(secure), ['https://vdi.example.com', 'https://vdi2.example.com', 'http://192.0.2.10:18480']);

  const request = (host, port = 18480) => ({ headers: { host }, rawHeaders: ['Host', host], socket: { localAddress: '192.0.2.10', remoteAddress: '198.51.100.20', localPort: port, encrypted: false } });
  for (const host of ['192.0.2.10:18480', '203.0.113.7:18480']) assert.equal(resolveRequestAccess(request(host), config).origin, 'http://' + host);
  for (const [host, port] of [['203.0.113.8:18480', 18480], ['203.0.113.7:18481', 18480], ['203.0.113.7:18480', 18481]]) {
    assert.throws(() => resolveRequestAccess(request(host, port), config), error => error.code === 'origin_not_allowed', host + ' @' + port);
  }
  assert.throws(() => resolveRequestAccess(request('203.0.113.7:18480'), normalizeServerAccess(lanHttp())), error => error.code === 'origin_not_allowed', 'without the extra address the public IP stays refused');
});
test('each extra address is checked against the listener port; a router forward may differ', () => {
  assert.deepEqual(serverAccessPortMismatches(withPublic('http://203.0.113.7:18480')), []);
  assert.deepEqual(serverAccessPortMismatches(withPublic('http://203.0.113.7')), [{ scheme: 'http', hostname: '203.0.113.7', external: 80, listenPort: 18480 }]);
});