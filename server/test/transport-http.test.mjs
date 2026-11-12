import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { request as httpsRequest } from 'node:https';
import { readFile } from 'node:fs/promises';
import { basename, dirname, resolve, join } from 'node:path';
import { tmpdir } from 'node:os';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { loadTlsListener } from '../src/tls-listener.mjs';

const password = 'Transport-memory-fixture-only-42!';
async function fixture(t, options) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  store.addUser({ name: 'transport-admin', role: 'admin', passwordHash: await hashPassword(password) });
  store.saveSetting('proxmox_initialized', '1', 'transport-fixture');
  let server;
  try { server = await buildServer({ store, vault, logger: { error: value => assert.fail(value) }, ...options }); }
  finally { options.tls?.key.fill(0); }
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  return { server, store, base: 'http://127.0.0.1:' + server.address().port };
}

for (const [publicOrigin, allowInsecureHttp, cookieName, secure] of [
  ['https://proxy.example:8443', false, '__Secure-multitap-admin', true],
  ['http://192.0.2.10:18400', true, 'multitap-admin-http', false],
]) {
  test('public scheme owns health, cookie, HSTS and CSRF behind an HTTP backend: ' + publicOrigin, async t => {
    const f = await fixture(t, { publicOrigin, allowInsecureHttp });
    const health = await fetch(f.base + '/healthz', { headers: { 'X-Forwarded-Proto': secure ? 'http' : 'https' } });
    const snapshot = await health.json();
    assert.deepEqual(snapshot.transport, { protocol: 'broker-transport-v1', publicOrigin, scheme: secure ? 'https' : 'http', allowInsecureHttp: !secure, directTls: false });
    assert.equal(health.headers.get('strict-transport-security'), secure ? 'max-age=31536000' : null);
    const page = await fetch(f.base + '/admin');
    assert.equal(page.headers.get('strict-transport-security'), secure ? 'max-age=31536000' : null);
    assert.ok(!page.headers.get('content-security-policy').includes('*'));
    const send = async (path, { origin = publicOrigin, cookie, csrf, data, method = 'GET' } = {}) => fetch(f.base + path, { method, headers: { Origin: origin, ...(cookie ? { Cookie: cookie } : {}), ...(csrf ? { 'X-CSRF-Token': csrf } : {}), ...(data === undefined ? {} : { 'Content-Type': 'application/json' }) }, body: data === undefined ? undefined : JSON.stringify(data), redirect: 'error' });
    const login = await send('/api/v1/admin/login', { method: 'POST', data: { username: 'transport-admin', password } });
    assert.equal(login.status, 200);
    const auth = await login.json(), rawCookie = login.headers.get('set-cookie'), cookie = rawCookie.split(';')[0];
    assert.ok(rawCookie.startsWith(cookieName + '='));
    assert.equal(rawCookie.includes('; Secure;'), secure);
    assert.match(rawCookie, /Path=\/api\/v1\/admin; HttpOnly;/);
    assert.match(rawCookie, /SameSite=Strict/);
    const otherName = secure ? 'multitap-admin-http' : '__Secure-multitap-admin';
    assert.equal((await send('/api/v1/admin/server-info', { cookie: otherName + cookie.slice(cookie.indexOf('=')) })).status, 401);
    assert.equal((await send('/api/v1/admin/server-info', { cookie })).status, 200);
    assert.equal((await send('/api/v1/admin/logout', { method: 'POST', cookie, data: {} })).status, 403, 'HTTP does not waive CSRF');
    assert.equal((await send('/api/v1/admin/logout', { method: 'POST', cookie, csrf: auth.csrfToken, origin: publicOrigin + ':9999', data: {} })).status, 403);
    const logout = await send('/api/v1/admin/logout', { method: 'POST', cookie, csrf: auth.csrfToken, data: {} });
    assert.equal(logout.status, 200);
    assert.ok(logout.headers.get('set-cookie').startsWith(cookieName + '='));
    assert.match(logout.headers.get('set-cookie'), /Max-Age=0/);
    assert.equal((await send('/api/v1/admin/server-info', { cookie })).status, 401);
  });
}

test('HTTP without installation opt-in and HTTP with TLS options fail before account or socket work', async () => {
  await assert.rejects(buildServer({ store: {}, publicOrigin: 'http://192.0.2.10:18400' }), /invalid_public_origin/);
  await assert.rejects(buildServer({ store: {}, publicOrigin: 'http://192.0.2.10:18400', allowInsecureHttp: true, tls: {} }), /invalid_tls_configuration/);
  assert.equal(await loadTlsListener(null), null);
});

// Root may opt in after creating its own ephemeral localhost certificate/key
// with OpenSSL. No key is committed, no certificate is generated/imported here,
// and this test never changes a global trust store or uses rejectUnauthorized=false.
const tlsDirectory = process.env.MULTITAP_TEST_TLS_DIRECTORY;
test('native TLS listener serves the same authenticated broker over verified TLS', { skip: !tlsDirectory ? 'Root-owned ephemeral TLS fixture required' : false }, async t => {
  const dir = resolve(tlsDirectory);
  assert.equal(dirname(dir), resolve(tmpdir()));
  assert.match(basename(dir), /^multitap-tls-fixture-[a-f0-9]{32}$/);
  const tls = await loadTlsListener({ certFile: join(dir, 'cert.pem'), keyFile: join(dir, 'key.pem') });
  const f = await fixture(t, { publicOrigin: 'https://localhost', tls });
  const ca = await readFile(join(dir, 'cert.pem'));
  const reply = await new Promise((resolveReply, reject) => {
    const req = httpsRequest({ hostname: '127.0.0.1', servername: 'localhost', port: f.server.address().port, path: '/healthz', ca, rejectUnauthorized: true, timeout: 2500 }, res => {
      const chunks = []; let bytes = 0;
      res.on('data', chunk => { bytes += chunk.length; if (bytes > 65536) req.destroy(); else chunks.push(chunk); });
      res.once('end', () => resolveReply({ status: res.statusCode, body: JSON.parse(Buffer.concat(chunks).toString('utf8')) }));
      res.once('error', reject);
    });
    req.once('error', reject); req.once('timeout', () => req.destroy()); req.end();
  });
  assert.equal(reply.status, 200);
  assert.equal(reply.body.transport.directTls, true);
  assert.equal(reply.body.transport.publicOrigin, 'https://localhost');
  assert.equal(reply.body.transport.allowInsecureHttp, false);
});
