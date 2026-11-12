import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes, X509Certificate } from 'node:crypto';
import { request as httpsRequest } from 'node:https';
import { readFile, lstat, realpath } from 'node:fs/promises';
import { basename, dirname, join, resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { loadTlsListener } from '../src/tls-listener.mjs';
import { hashPassword } from '../src/security.mjs';

// Explicit Root-owned test material only. Importing/running without that
// material skips; this file never generates keys or changes global trust.
const directory = process.env.MULTITAP_TEST_TLS_SECURITY_DIRECTORY;
const options = { skip: !directory ? 'Root-owned ephemeral TLS security fixture required' : false };
const files = ['cert.pem', 'key.pem', 'wrong-key.pem', 'expired-cert.pem'];
async function material() {
  const dir = resolve(directory);
  assert.equal(dirname(dir), resolve(tmpdir()));
  assert.match(basename(dir), /^multitap-tls-security-fixture-[a-f0-9]{32}$/);
  assert.equal(await realpath(dir), dir);
  const folder = await lstat(dir);
  assert.ok(folder.isDirectory() && !folder.isSymbolicLink());
  if (process.platform !== 'win32') {
    assert.equal(folder.mode & 0o077, 0);
    assert.equal(folder.uid, process.getuid());
  }
  for (const file of files) {
    const entry = await lstat(join(dir, file));
    assert.ok(entry.isFile() && !entry.isSymbolicLink() && entry.size > 0 && entry.size <= 65536);
    if (process.platform !== 'win32') {
      assert.equal(entry.mode & 0o077, 0, 'test key/certificate remains private');
      assert.equal(entry.uid, process.getuid());
    }
  }
  const good = new X509Certificate(await readFile(join(dir, 'cert.pem')));
  const expired = new X509Certificate(await readFile(join(dir, 'expired-cert.pem')));
  assert.equal(good.checkHost('localhost'), 'localhost');
  assert.equal(expired.checkHost('localhost'), 'localhost');
  assert.ok(Date.parse(good.validFrom) < Date.now() && Date.parse(good.validTo) > Date.now());
  assert.ok(Date.parse(expired.validTo) < Date.now());
  return dir;
}

async function listener(t, dir, certName = 'cert.pem') {
  const tls = await loadTlsListener({ certFile: join(dir, certName), keyFile: join(dir, 'key.pem') });
  const store = new Store(':memory:');
  // Avoid the unrelated first-administrator setup-code output in this fixture.
  store.addUser({ name: 'tls-fixture-admin', role: 'admin', passwordHash: await hashPassword('TLS-memory-fixture-only-42!') });
  let server;
  try { server = await buildServer({ store, vault: new Vault(randomBytes(32)), publicOrigin: 'https://localhost', tls, logger: { error() {} } }); }
  catch (error) { store.close(); throw error; }
  finally { tls.key.fill(0); }
  let requests = 0;
  server.on('request', () => ++requests);
  t.after(async () => { await new Promise(done => server.close(done)); store.close(); });
  // Own ephemeral loopback listener only; never a product/public port.
  await new Promise((done, reject) => { server.once('error', reject); server.listen(0, '127.0.0.1', done); });
  return { port: server.address().port, requests: () => requests };
}

function get(port, { ca, servername = 'localhost' } = {}) {
  return new Promise((done, reject) => {
    const request = httpsRequest({ hostname: '127.0.0.1', port, servername, path: '/healthz',
      ca, rejectUnauthorized: true, minVersion: 'TLSv1.2', timeout: 2500 }, response => {
      const chunks = []; let size = 0;
      response.on('data', chunk => { size += chunk.length; if (size > 65536) request.destroy(Error('test_body_limit')); else chunks.push(chunk); });
      response.once('error', reject);
      response.once('end', () => {
        try { done({ status: response.statusCode, body: JSON.parse(Buffer.concat(chunks).toString('utf8')) }); }
        catch { reject(Error('test_health_json')); }
      });
    });
    request.once('error', reject);
    request.once('timeout', () => request.destroy(Error('test_tls_timeout')));
    request.end();
  });
}

test('supplied native TLS serves verified broker health without a global trust change', options, async t => {
  const dir = await material(), f = await listener(t, dir);
  const reply = await get(f.port, { ca: await readFile(join(dir, 'cert.pem')) });
  assert.equal(reply.status, 200);
  assert.equal(reply.body.service, 'multitap-broker');
  assert.equal(reply.body.transport.directTls, true);
  assert.equal(reply.body.transport.publicOrigin, 'https://localhost');
  assert.equal(reply.body.transport.allowInsecureHttp, false);
  assert.equal(f.requests(), 1);
});

test('supplied TLS refuses a mismatched private key before any listener bind', options, async () => {
  const dir = await material();
  await assert.rejects(loadTlsListener({ certFile: join(dir, 'cert.pem'), keyFile: join(dir, 'wrong-key.pem') }),
    { message: 'invalid_tls_credentials' });
});

test('verified native TLS refuses a different hostname before any HTTP request', options, async t => {
  const dir = await material(), f = await listener(t, dir);
  await assert.rejects(get(f.port, { ca: await readFile(join(dir, 'cert.pem')), servername: 'other.invalid' }),
    error => error.code === 'ERR_TLS_CERT_ALTNAME_INVALID');
  assert.equal(f.requests(), 0);
});

test('verified native TLS refuses an untrusted chain without a trust-all retry', options, async t => {
  const dir = await material(), f = await listener(t, dir);
  await assert.rejects(get(f.port), error => ['DEPTH_ZERO_SELF_SIGNED_CERT', 'SELF_SIGNED_CERT_IN_CHAIN',
    'UNABLE_TO_VERIFY_LEAF_SIGNATURE', 'UNABLE_TO_GET_ISSUER_CERT_LOCALLY'].includes(error.code));
  assert.equal(f.requests(), 0);
});

test('verified native TLS refuses an expired supplied certificate before HTTP', options, async t => {
  const dir = await material(), f = await listener(t, dir, 'expired-cert.pem');
  await assert.rejects(get(f.port, { ca: await readFile(join(dir, 'expired-cert.pem')) }),
    error => error.code === 'CERT_HAS_EXPIRED');
  assert.equal(f.requests(), 0);
});
