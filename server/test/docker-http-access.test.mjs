import test from 'node:test';
import assert from 'node:assert/strict';
import { createServer, request } from 'node:http';
import { randomBytes } from 'node:crypto';
import { configuration } from '../src/config.mjs';
import { buildServer } from '../src/http.mjs';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { hashPassword, initialPasswordHash } from '../src/security.mjs';
import { resolveRequestAccess } from '../src/request-origin.mjs';
import { setupGuide } from '../src/initial-setup.mjs';
import { startupSecurityWarnings } from '../src/startup-security.mjs';
import { serverAccessListeners, serverAccessHash, normalizeServerAccess } from '../update/server-access-policy.mjs';

// http-origin-v1 and the Docker first setup by IP: a Compose
// install serves plain HTTP on the LAN beside or instead of HTTPS from .env,
// and a fresh one opens its first setup at http://<server address>:<port>/.
const noFile = { readAccessFile: () => null };
const env = extra => ({ MULTITAP_HOST: '0.0.0.0', MULTITAP_PORT: '18480', MULTITAP_DEPLOYMENT_KIND: 'docker', ...extra });

test('MULTITAP_HTTP_ORIGIN adds the LAN HTTP address beside HTTPS on the same proxy listener', () => {
  const c = configuration(env({ MULTITAP_PUBLIC_ORIGIN: 'https://vdi.example.com', MULTITAP_HTTP_ORIGIN: 'http://192.0.2.10:18480/', MULTITAP_ALLOW_INSECURE_HTTP: '1', MULTITAP_TRUSTED_PROXIES: '172.18.0.1' }), noFile);
  assert.equal(c.bootstrapOnly, false);
  assert.equal(c.publicOrigin, 'https://vdi.example.com');
  assert.deepEqual(c.allowedOrigins, ['https://vdi.example.com', 'http://192.0.2.10:18480']);
  assert.deepEqual({ ...c.accessConfig.http }, { enabled: true, origin: 'http://192.0.2.10:18480', listenHost: '0.0.0.0', listenPort: 18480 });
  assert.equal(c.accessConfig.https.mode, 'proxy');
  assert.deepEqual(c.trustedProxies, ['172.18.0.1']);
  // One plain listener answers both: the proxy hop for HTTPS, LAN browsers for HTTP.
  assert.deepEqual(serverAccessListeners(c.accessConfig).map(row => ({ protocol: row.protocol, port: row.port, origins: row.origins })),
    [{ protocol: 'http', port: 18480, origins: ['https://vdi.example.com', 'http://192.0.2.10:18480'] }]);
  assert.equal(c.accessRevision, serverAccessHash(c.accessConfig));
  const [warning] = startupSecurityWarnings(c);
  assert.equal(warning.code, 'cleartext_http_enabled'); assert.equal(warning.originRole, 'additional');
});

test('MULTITAP_HTTP_ORIGIN alone is an HTTP-only install, not a bootstrap', () => {
  const c = configuration(env({ NODE_ENV: 'production', MULTITAP_DATA_DIR: '/tmp/multitap-http-origin', MULTITAP_HTTP_ORIGIN: 'http://192.0.2.10:18480', MULTITAP_ALLOW_INSECURE_HTTP: '1' }), noFile);
  assert.equal(c.bootstrapOnly, false);
  assert.equal(c.publicOrigin, 'http://192.0.2.10:18480');
  assert.equal(c.allowInsecureHttp, true);
  assert.equal(c.accessConfig.https.enabled, false);
  assert.deepEqual(c.allowedOrigins, ['http://192.0.2.10:18480']);
  assert.equal(c.legacyPublicOrigin, 'http://192.0.2.10:18480');
  assert.equal(startupSecurityWarnings(c)[0].originRole, 'primary');
  // The same address through the older http:// public origin is the same policy.
  const legacy = configuration(env({ NODE_ENV: 'production', MULTITAP_DATA_DIR: '/tmp/multitap-http-origin', MULTITAP_PUBLIC_ORIGIN: 'http://192.0.2.10:18480', MULTITAP_ALLOW_INSECURE_HTTP: '1' }), noFile);
  assert.equal(legacy.accessRevision, c.accessRevision);
});

test('An HTTP address needs the explicit opt-in, an exact http origin, a proxy TLS listener and no updater', () => {
  const https = { MULTITAP_PUBLIC_ORIGIN: 'https://vdi.example.com' };
  for (const [extra, code] of [
    [{ ...https, MULTITAP_HTTP_ORIGIN: 'http://192.0.2.10:18480' }, /http_origin_requires_allow_insecure_http/],
    [{ ...https, MULTITAP_HTTP_ORIGIN: 'http://192.0.2.10:18480', MULTITAP_ALLOW_INSECURE_HTTP: '0' }, /http_origin_requires_allow_insecure_http/],
    [{ ...https, MULTITAP_HTTP_ORIGIN: 'https://192.0.2.10:18480', MULTITAP_ALLOW_INSECURE_HTTP: '1' }, /invalid_http_origin/],
    [{ ...https, MULTITAP_HTTP_ORIGIN: 'http://192.0.2.10:18480/admin', MULTITAP_ALLOW_INSECURE_HTTP: '1' }, /invalid_http_origin/],
    [{ ...https, MULTITAP_HTTP_ORIGIN: 'http://user@192.0.2.10:18480', MULTITAP_ALLOW_INSECURE_HTTP: '1' }, /invalid_http_origin/],
    [{ ...https, MULTITAP_HTTP_ORIGIN: 'http://192.0.2.10:18480', MULTITAP_ALLOW_INSECURE_HTTP: '1', MULTITAP_UPDATE_SOCKET: '/run/multitap-update/control.sock' }, /http_origin_use_server_access_settings/],
    [{ ...https, MULTITAP_HTTP_ORIGIN: 'http://192.0.2.10:18480', MULTITAP_ALLOW_INSECURE_HTTP: '1', MULTITAP_TLS_MODE: 'direct', MULTITAP_TLS_CERT_FILE: '/tls/cert.pem', MULTITAP_TLS_KEY_FILE: '/tls/key.pem' }, /http_origin_requires_proxy_tls/],
    [{ MULTITAP_PUBLIC_ORIGIN: 'http://192.0.2.10:18480', MULTITAP_HTTP_ORIGIN: 'http://192.0.2.11:18480', MULTITAP_ALLOW_INSECURE_HTTP: '1' }, /invalid_http_origin/],
  ]) assert.throws(() => configuration(env(extra), noFile), code, JSON.stringify(extra));
  // The same address twice is one HTTP endpoint.
  const same = configuration(env({ MULTITAP_PUBLIC_ORIGIN: 'http://192.0.2.10:18480', MULTITAP_HTTP_ORIGIN: 'http://192.0.2.10:18480', MULTITAP_ALLOW_INSECURE_HTTP: '1' }), noFile);
  assert.deepEqual(same.allowedOrigins, ['http://192.0.2.10:18480']);
});

test('A saved server access policy supersedes the environment HTTP address', () => {
  const config = normalizeServerAccess({ protocol: 'server-access-v1', primaryOrigin: 'https://saved.example',
    https: { enabled: true, origin: 'https://saved.example', mode: 'proxy', listenHost: '0.0.0.0', listenPort: 18480 },
    http: { enabled: false, origin: null, listenHost: '0.0.0.0', listenPort: 18481 }, trustedProxies: ['172.18.0.1'] });
  const state = JSON.stringify({ schema: 1, protocol: 'server-access-v1', revision: serverAccessHash(config), config, updatedAt: '2026-11-03T00:00:00.000Z' });
  const readAccessFile = path => path.endsWith('server-access.json') ? state : 'c'.repeat(64);
  const c = configuration(env({ MULTITAP_PUBLIC_ORIGIN: 'https://vdi.example.com', MULTITAP_HTTP_ORIGIN: 'http://192.0.2.10:18480', MULTITAP_ALLOW_INSECURE_HTTP: '1' }), { readAccessFile });
  assert.deepEqual(c.allowedOrigins, ['https://saved.example']);
});

test('MULTITAP_SERVER_ADDRESS names the Docker first-setup origin from the published port; other installs refuse it', () => {
  const fresh = extra => configuration(env({ NODE_ENV: 'production', MULTITAP_DATA_DIR: '/tmp/multitap-setup-address', ...extra }), noFile);
  assert.equal(fresh({}).setupOrigin, null);
  assert.equal(fresh({ MULTITAP_SERVER_ADDRESS: '192.0.2.10' }).setupOrigin, 'http://192.0.2.10:18480');
  for (const [published, origin] of [['28490', 'http://192.0.2.10:28490'], ['0.0.0.0:18480', 'http://192.0.2.10:18480'], ['192.0.2.10:8080', 'http://192.0.2.10:8080'], ['[::]:18480', 'http://192.0.2.10:18480'], ['80', 'http://192.0.2.10']]) {
    const c = fresh({ MULTITAP_SERVER_ADDRESS: '192.0.2.10', MULTITAP_PUBLISHED_PORT: published });
    assert.equal(c.bootstrapOnly, true); assert.equal(c.setupOrigin, origin, published);
  }
  assert.equal(fresh({ MULTITAP_SERVER_ADDRESS: '2001:db8::10' }).setupOrigin, 'http://[2001:db8::10]:18480');
  for (const address of ['vdi.example.com', '192.0.2.010', '192.0.2.10:18480', 'http://192.0.2.10', 'fe80::1%eth0', ' 192.0.2.10']) {
    assert.throws(() => fresh({ MULTITAP_SERVER_ADDRESS: address }), /invalid_server_address/, address);
  }
  for (const published of ['0', '65536', 'abc', '18480:18480', '192.0.2.10:']) {
    assert.throws(() => fresh({ MULTITAP_SERVER_ADDRESS: '192.0.2.10', MULTITAP_PUBLISHED_PORT: published }), /invalid_published_port|invalid_port|invalid_server_address/, published);
  }
  for (const kind of ['systemd', 'external', 'unknown']) {
    assert.throws(() => fresh({ MULTITAP_DEPLOYMENT_KIND: kind, MULTITAP_SERVER_ADDRESS: '192.0.2.10' }), /server_address_docker_only/, kind);
  }
});

const socketRequest = ({ host, local = '172.18.0.2', remote = '192.0.2.50', port = 18480, origin, encrypted = false, forwarded }) => ({
  headers: { host, ...(origin ? { origin } : {}), ...(forwarded ? { 'x-forwarded-proto': forwarded } : {}) }, rawHeaders: ['Host', host],
  socket: { localAddress: local, remoteAddress: remote, localPort: port, encrypted },
});
test('Bootstrap admits exactly the configured Docker setup origin, never another address, port, scheme or Origin', () => {
  const setupOrigin = 'http://192.0.2.10:18480';
  const admitted = resolveRequestAccess(socketRequest({ host: '192.0.2.10:18480' }), null, { bootstrap: true, setupOrigin });
  assert.deepEqual(admitted, { origin: setupOrigin, bootstrapOnly: true, proxied: false });
  assert.equal(resolveRequestAccess(socketRequest({ host: '192.0.2.10:18480', origin: setupOrigin }), null, { bootstrap: true, setupOrigin }).bootstrapOnly, true);
  for (const input of [{ host: '192.0.2.11:18480' }, { host: '192.0.2.10:18481' }, { host: '192.0.2.10' }, { host: 'vdi.example.com:18480' }, { host: 'localhost:18480' },
    { host: '192.0.2.10:18480', origin: 'http://evil.example' }, { host: '192.0.2.10:18480', encrypted: true }]) {
    assert.throws(() => resolveRequestAccess(socketRequest(input), null, { bootstrap: true, setupOrigin }), error => error.code === 'origin_not_allowed', JSON.stringify(input));
  }
  // Without the setting the native rule stands: the container address is not the typed one.
  assert.throws(() => resolveRequestAccess(socketRequest({ host: '192.0.2.10:18480' }), null, { bootstrap: true }), error => error.code === 'origin_not_allowed');
  // Not a bootstrap server: the setup address grants nothing.
  assert.throws(() => resolveRequestAccess(socketRequest({ host: '192.0.2.10:18480' }), null, { bootstrap: false, setupOrigin }), error => error.code === 'origin_not_allowed');
});

test('MULTITAP_HTTP_ORIGIN takes a comma list: the first is the main address, the others exact extras', () => {
  const both = configuration(env({ MULTITAP_PUBLIC_ORIGIN: 'https://vdi.example.com', MULTITAP_HTTP_ORIGIN: 'http://192.0.2.10:18480, http://203.0.113.7:18480', MULTITAP_ALLOW_INSECURE_HTTP: '1', MULTITAP_TRUSTED_PROXIES: '172.18.0.1' }), noFile);
  assert.deepEqual(both.allowedOrigins, ['https://vdi.example.com', 'http://192.0.2.10:18480', 'http://203.0.113.7:18480']);
  assert.deepEqual(both.accessConfig.http.extraOrigins, ['http://203.0.113.7:18480']);
  const alone = configuration(env({ NODE_ENV: 'production', MULTITAP_DATA_DIR: '/tmp/multitap-http-origin', MULTITAP_HTTP_ORIGIN: 'http://192.0.2.10:18480,http://203.0.113.7:18480', MULTITAP_ALLOW_INSECURE_HTTP: '1' }), noFile);
  assert.equal(alone.publicOrigin, 'http://192.0.2.10:18480');
  assert.deepEqual(alone.allowedOrigins, ['http://192.0.2.10:18480', 'http://203.0.113.7:18480']);
  const legacy = configuration(env({ MULTITAP_PUBLIC_ORIGIN: 'http://192.0.2.10:18480', MULTITAP_HTTP_ORIGIN: 'http://203.0.113.7:18480,http://192.0.2.10:18480', MULTITAP_ALLOW_INSECURE_HTTP: '1' }), noFile);
  assert.deepEqual(legacy.allowedOrigins, ['http://192.0.2.10:18480', 'http://203.0.113.7:18480']);
  for (const list of ['http://192.0.2.10:18480,http://192.0.2.10:18480', 'http://192.0.2.10:18480,https://203.0.113.7', 'http://192.0.2.10:18480,http://0.0.0.0:18480',
    'http://192.0.2.10:1,http://192.0.2.10:2,http://192.0.2.10:3,http://192.0.2.10:4,http://192.0.2.10:5', ',']) {
    assert.throws(() => configuration(env({ MULTITAP_PUBLIC_ORIGIN: 'https://vdi.example.com', MULTITAP_HTTP_ORIGIN: list, MULTITAP_ALLOW_INSECURE_HTTP: '1' }), noFile), /invalid_http_origin|origin_invalid/, list);
  }
  // Every listed address is admitted on the one listener; nothing else is.
  for (const host of ['192.0.2.10:18480', '203.0.113.7:18480']) assert.equal(resolveRequestAccess(socketRequest({ host }), both.accessConfig).origin, 'http://' + host);
  assert.throws(() => resolveRequestAccess(socketRequest({ host: '203.0.113.8:18480' }), both.accessConfig), error => error.code === 'origin_not_allowed');
});

test('The setup line names the address to open, or says how to set it', () => {
  assert.deepEqual(setupGuide({ bootstrapOnly: true, setupOrigin: 'http://192.0.2.10:18480', deploymentKind: 'docker' }),
    { urls: ['http://192.0.2.10:18480'], message: '처음 설정: http://192.0.2.10:18480 를 브라우저로 여세요 / First setup: open http://192.0.2.10:18480 in a browser' });
  assert.match(setupGuide({ bootstrapOnly: true, deploymentKind: 'docker' }).message, /MULTITAP_SERVER_ADDRESS=<서버 IP>/);
  assert.equal(setupGuide({ bootstrapOnly: true, deploymentKind: 'systemd', bootstrapPort: 18480 }).message, '처음 설정: http://<서버 IP>:18480 를 브라우저로 여세요 / First setup: open http://<server IP>:18480 in a browser');
  assert.deepEqual(setupGuide({ bootstrapOnly: false, allowedOrigins: ['https://vdi.example.com', 'http://192.0.2.10:18480'] }).urls, ['https://vdi.example.com', 'http://192.0.2.10:18480']);
  assert.match(setupGuide({ bootstrapOnly: false, allowedOrigins: ['https://vdi.example.com', 'http://192.0.2.10:18480'] }).message, /https:\/\/vdi\.example\.com 또는 http:\/\/192\.0\.2\.10:18480 를/);
});

async function freePort() {
  const reservation = createServer();
  await new Promise(resolve => reservation.listen(0, '127.0.0.1', resolve));
  const { port } = reservation.address();
  await new Promise(resolve => reservation.close(resolve));
  return port;
}
function call(port, { host, path = '/healthz', method = 'GET', headers = {}, body }) {
  return new Promise((resolve, reject) => {
    const req = request({ hostname: '127.0.0.1', port, path, method, headers: { Host: host, ...headers } }, res => {
      const chunks = []; res.on('data', chunk => chunks.push(chunk));
      res.once('end', () => { const text = Buffer.concat(chunks).toString('utf8'); let json = null; try { json = JSON.parse(text); } catch {} resolve({ status: res.statusCode, headers: res.headers, text, json }); });
    });
    req.on('error', reject); if (body !== undefined) req.write(body); req.end();
  });
}

test('One Docker listener serves the HTTPS proxy hop and LAN HTTP with their own health profile and admin cookie', async t => {
  const port = await freePort();
  const c = configuration(env({ MULTITAP_HOST: '127.0.0.1', MULTITAP_PORT: String(port), MULTITAP_PUBLIC_ORIGIN: 'https://vdi.example.com', MULTITAP_HTTP_ORIGIN: 'http://192.0.2.10:18480', MULTITAP_ALLOW_INSECURE_HTTP: '1', MULTITAP_TRUSTED_PROXIES: '127.0.0.1' }), noFile);
  const store = new Store(':memory:'), password = 'Docker-http-tests-password-42!';
  store.addUser({ name: 'owner', role: 'admin', passwordHash: await hashPassword(password) });
  const server = await buildServer({ store, vault: new Vault(randomBytes(32)), publicOrigin: c.publicOrigin, allowInsecureHttp: c.allowInsecureHttp,
    accessConfig: c.accessConfig, accessRevision: c.accessRevision, legacyAdminOrigin: c.legacyPublicOrigin, trustedProxies: c.trustedProxies,
    deploymentKind: 'docker', initialSetupOptions: { announce: () => {} }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(port, '127.0.0.1', resolve));
  t.after(async () => { server.closeAllConnections(); await new Promise(resolve => server.close(resolve)); store.close(); });

  // What the Windows client's 연결 테스트 reads for an http:// address.
  const plain = await call(port, { host: '192.0.2.10:18480' });
  assert.equal(plain.status, 200);
  assert.deepEqual(plain.json.transport, { protocol: 'broker-transport-v1', publicOrigin: 'http://192.0.2.10:18480', scheme: 'http', allowInsecureHttp: true, directTls: false });
  assert.equal(plain.json.access.origin, 'http://192.0.2.10:18480'); assert.equal(plain.json.access.bootstrapOnly, false);
  assert.equal(plain.headers['strict-transport-security'], undefined);
  const secure = await call(port, { host: 'vdi.example.com', headers: { 'X-Forwarded-Proto': 'https' } });
  assert.equal(secure.status, 200); assert.equal(secure.json.transport.publicOrigin, 'https://vdi.example.com'); assert.equal(secure.json.transport.scheme, 'https');
  assert.equal(secure.headers['strict-transport-security'], 'max-age=31536000', 'another hostname keeps HTTPS-only HSTS');
  // (/healthz from this loopback peer would be the local liveness probe.)
  assert.equal((await call(port, { host: 'vdi.example.com', path: '/api/v1/downloads' })).json.error.code, 'origin_not_allowed', 'the HTTPS name over plain HTTP stays refused');
  assert.equal((await call(port, { host: '192.0.2.11:18480', path: '/api/v1/downloads' })).status, 403);
  assert.equal((await call(port, { host: '192.0.2.10:18480', path: '/api/v1/downloads' })).status, 200);

  const login = async (origin, headers = {}) => call(port, { host: new URL(origin).host, path: '/api/v1/admin/login', method: 'POST', headers: { Origin: origin, 'Content-Type': 'application/json', ...headers }, body: JSON.stringify({ username: 'owner', password }) });
  const overHttp = await login('http://192.0.2.10:18480');
  assert.equal(overHttp.status, 200);
  assert.match(overHttp.headers['set-cookie'][0], /^multitap-admin-http-[0-9a-f]{16}=/);
  assert.doesNotMatch(overHttp.headers['set-cookie'][0], /Secure/);
  const overHttps = await login('https://vdi.example.com', { 'X-Forwarded-Proto': 'https' });
  assert.equal(overHttps.status, 200);
  assert.match(overHttps.headers['set-cookie'][0], /^__Secure-multitap-admin-[0-9a-f]{16}=.*; Secure;/);
  // A browser on the LAN address reaches the public pages too.
  for (const path of ['/download', '/admin/']) assert.equal((await call(port, { host: '192.0.2.10:18480', path, headers: { Accept: 'text/html' } })).status, 200, path);
});

test('A fresh install opens its first setup from the root address; afterwards the root is the public page', async t => {
  const port = await freePort();
  const store = new Store(':memory:');
  const id = store.addUser({ name: 'admin', role: 'admin', passwordHash: await initialPasswordHash() });
  store.db.prepare('UPDATE users SET must_change_credentials=1 WHERE id=?').run(id);
  const announced = [];
  const bootstrap = await buildServer({ store, vault: new Vault(randomBytes(32)), publicOrigin: null, bootstrap: true, setupOrigin: 'http://192.0.2.10:18480',
    deploymentKind: 'docker', initialSetupOptions: { announce: (code, guide) => announced.push(guide) }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => bootstrap.listen(port, '127.0.0.1', resolve));
  t.after(async () => { bootstrap.closeAllConnections(); if (bootstrap.listening) await new Promise(resolve => bootstrap.close(resolve)); });
  assert.equal(announced.length, 1);
  for (const accept of ['text/html', 'application/json']) {
    const root = await call(port, { host: '192.0.2.10:18480', path: '/', headers: { Accept: accept } });
    assert.equal(root.status, 302, accept); assert.equal(root.headers.location, '/admin/');
  }
  assert.equal((await call(port, { host: '192.0.2.10:18480', path: '/', method: 'HEAD' })).status, 302);
  assert.equal((await call(port, { host: '192.0.2.10:18480', path: '/admin/', headers: { Accept: 'text/html' } })).status, 200);
  const health = await call(port, { host: '192.0.2.10:18480' });
  assert.equal(health.status, 200); assert.equal(health.json.access.bootstrapOnly, true);
  // Only the bootstrap paths: no public page, catalog, client API or other address.
  for (const path of ['/download', '/api/v1/downloads', '/get/client', '/api/v1/desktops']) assert.equal((await call(port, { host: '192.0.2.10:18480', path })).status, 403, path);
  // The first-setup page's Proxmox step: its script answers, naming the setup address; only GET.
  const script = await call(port, { host: '192.0.2.10:18480', path: '/setup/proxmox.py' });
  assert.equal(script.status, 200); assert.match(script.text, /^BROKER_ORIGIN = "http:\/\/192\.0\.2\.10:18480"\r?$/m);
  assert.equal((await call(port, { host: '192.0.2.11:18480', path: '/setup/proxmox.py' })).status, 403, 'another address still refused');
  assert.notEqual((await call(port, { host: '192.0.2.10:18480', path: '/setup/proxmox.py', method: 'POST' })).status, 200, 'a read only');
  assert.equal((await call(port, { host: '192.0.2.10:18480', path: '/setup/other.py' })).status, 403);
  assert.equal((await call(port, { host: '192.0.2.10:18480', path: '/?x=1' })).status, 403);
  assert.equal((await call(port, { host: '192.0.2.11:18480', path: '/' })).status, 403);
  bootstrap.closeAllConnections(); await new Promise(resolve => bootstrap.close(resolve));

  // An installation with its address but no administrator yet: a browser's root goes to setup.
  const origin = 'https://setup-root.example';
  const configured = await buildServer({ store, vault: new Vault(randomBytes(32)), publicOrigin: origin, initialSetupOptions: { announce: () => {} }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => configured.listen(0, '127.0.0.1', resolve));
  t.after(async () => { configured.closeAllConnections(); await new Promise(resolve => configured.close(resolve)); store.close(); });
  const at = configured.address().port;
  const pending = await call(at, { host: 'setup-root.example', path: '/', headers: { Accept: 'text/html' } });
  assert.equal(pending.status, 302); assert.equal(pending.headers.location, '/admin/');
  const api = await call(at, { host: 'setup-root.example', path: '/', headers: { Accept: 'application/json' } });
  assert.equal(api.status, 200); assert.equal(api.json.service, 'multitap-broker');
  store.db.prepare('UPDATE users SET must_change_credentials=0 WHERE id=?').run(id);
  const page = await call(at, { host: 'setup-root.example', path: '/', headers: { Accept: 'text/html', 'Accept-Language': 'ko-KR' } });
  assert.equal(page.status, 200);
  assert.match(page.text, /<a class="button secondary site-admin" href="\/admin\/">[\s\S]*?<span>관리자 페이지<\/span><\/a>/);
});
