// Actual isolated SQLite + HTTP/TLS. Synthetic users/keys; no production IO.
import test from 'node:test';
import assert from 'node:assert/strict';
import { createServer, request as httpRequest } from 'node:http';
import { request as httpsRequest } from 'node:https';
import { generateKeyPairSync, sign } from 'node:crypto';
import { mkdtempSync, rmSync, copyFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { Store } from '../../src/store.mjs';
import { Store as Store18 } from '../fixtures/mfa-store18.mjs';
import { Vault } from '../../src/vault.mjs';
import { MfaProvider } from '../../src/mfa.mjs';
import { buildServer } from '../../src/http.mjs';
import { hashPassword, newToken, tokenHash } from '../../src/security.mjs';
import { totpAt } from '../../src/mfa-primitives.mjs';
import { normalizeServerAccess } from '../../update/server-access-policy.mjs';

const origin = 'https://mfa.fixture.invalid', password = 'Synthetic-MFA-password-42!';
const hashed = hashPassword(password);
const headers = { 'X-Multitap-Auth-Protocol': 'mfa-v1', 'X-Multitap-Session-Policy': 'windows-pc-v1', 'X-Multitap-Login-Intent': 'confirmable-v1' };
function decode32(text) {
  let bits = 0, value = 0; const out = [];
  for (const char of text) { value = (value << 5) | 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567'.indexOf(char); bits += 5; if (bits >= 8) out.push((value >>> (bits -= 8)) & 255); }
  return Buffer.from(out);
}
function certificate() {
  const der = (tag, ...parts) => { const b = Buffer.concat(parts); return Buffer.concat([Buffer.from([tag, ...(b.length < 128 ? [b.length] : b.length < 256 ? [0x81, b.length] : [0x82, b.length >> 8, b.length & 255])]), b]); };
  const seq = (...parts) => der(0x30, ...parts), oid = text => der(6, Buffer.from(text, 'hex'));
  const time = ms => der(0x17, Buffer.from(new Date(ms).toISOString().replace(/[-:T]/g, '').slice(2, 14) + 'Z'));
  const { privateKey, publicKey } = generateKeyPairSync('ec', { namedCurve: 'P-256' });
  const algorithm = seq(oid('2a8648ce3d040302')), name = seq(der(0x31, seq(oid('550403'), der(0x0c, Buffer.from('localhost')))));
  const tbs = seq(der(0xa0, der(2, Buffer.from([2]))), der(2, Buffer.from([1])), algorithm, name,
    seq(time(Date.now() - 86400000), time(Date.now() + 86400000)), name, publicKey.export({ type: 'spki', format: 'der' }));
  const body = seq(tbs, algorithm, der(3, Buffer.from([0]), sign('sha256', tbs, privateKey)));
  return { key: privateKey.export({ type: 'pkcs8', format: 'pem' }), cert: '-----BEGIN CERTIFICATE-----\n' + body.toString('base64').match(/.{1,64}/g).join('\n') + '\n-----END CERTIFICATE-----\n' };
}
async function fixture(t, { transport = 'proxy', file = false } = {}) {
  const reservation = createServer(); await new Promise(resolve => reservation.listen(0, '127.0.0.1', resolve));
  const port = reservation.address().port; await new Promise(resolve => reservation.close(resolve));
  let now = Date.now(), mono = 0;
  // A file-backed store lives in the temp directory and goes with the test.
  const dir = file ? mkdtempSync(join(tmpdir(), 'multitap-mfa-fixture-')) : null;
  const dbPath = file ? resolve(dir, 'synthetic.sqlite') : ':memory:';
  let store = new Store(dbPath), vault = new Vault(Buffer.alloc(32, 23)), server;
  const userId = store.addUser({ name: 'alice', passwordHash: await hashed });
  const adminId = store.addUser({ name: 'administrator', passwordHash: await hashed, role: 'admin' });
  const errors = [];
  const tls = transport === 'direct' ? certificate() : null;
  const access = transport === 'proxy' ? normalizeServerAccess({ protocol: 'server-access-v1', primaryOrigin: origin,
    https: { enabled: true, origin, mode: 'proxy', listenHost: '127.0.0.1', listenPort: port },
    http: { enabled: false, origin: null, listenHost: '127.0.0.1', listenPort: port }, trustedProxies: ['127.0.0.1'] }) : undefined;
  async function start() {
    server = await buildServer({ store, vault, publicOrigin: origin, tls, accessConfig: access, trustedProxies: access?.trustedProxies,
      loginLimit: 100, addressLimit: 200, sessionAccessOptions: { now: () => now }, mfaOptions: { now: () => now, monotonic: () => mono }, logger: { error: value => errors.push(value) } });
    await new Promise(resolve => server.listen(port, '127.0.0.1', resolve));
  }
  await start();
  async function stop() { server.closeAllConnections(); await new Promise(resolve => server.close(resolve)); }
  t.after(async () => { await stop(); store.close(); if (dir) rmSync(dir, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); assert.deepEqual(errors, []); });
  function call(path, { data, token, method = data === undefined ? 'GET' : 'POST', extra = {}, cookie } = {}) {
    const payload = data === undefined ? undefined : JSON.stringify(data);
    return new Promise((resolve, reject) => {
      const send = tls ? httpsRequest : httpRequest;
      const req = send({ host: '127.0.0.1', port, path, method, ...(tls ? { ca: tls.cert, servername: 'localhost' } : {}),
        headers: { Host: new URL(origin).host, Origin: origin, ...(transport === 'proxy' ? { 'X-Forwarded-Proto': 'https' } : {}),
          ...(payload === undefined ? {} : { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(payload) }),
          ...(token ? { Authorization: 'Bearer ' + token } : {}), ...(cookie ? { Cookie: cookie } : {}), ...extra } }, res => {
        const chunks = []; res.on('data', value => chunks.push(value)); res.on('end', () => {
          try { resolve({ status: res.statusCode, body: JSON.parse(Buffer.concat(chunks)), headers: res.headers, cookie: res.headers['set-cookie']?.[0]?.split(';')[0] }); }
          catch (error) { reject(error); }
        });
      }); req.on('error', reject); req.setTimeout(5000, () => req.destroy(Error('synthetic request timeout'))); req.end(payload);
    });
  }
  const login = (admin = false, support = true) => call('/api/v1/' + (admin ? 'admin' : 'auth') + '/login',
    { data: { username: admin ? 'administrator' : 'alice', password }, extra: support ? (admin ? { 'X-Multitap-Auth-Protocol': 'mfa-v1' } : headers) : {} });
  const verify = (challenge, code, { admin = false, recovery = false } = {}) => call('/api/v1/' + (admin ? 'admin' : 'auth') + '/mfa/verify',
    { data: { challengeToken: challenge, [recovery ? 'recoveryCode' : 'code']: code }, extra: headers });
  function next(ms = 30000) { now += ms; mono += ms; }
  async function enroll(admin = false) {
    const first = await login(admin); assert.equal(first.status, 200);
    const principal = admin ? { cookie: first.cookie, extra: { 'X-CSRF-Token': first.body.csrfToken } } : { token: first.body.sessionToken };
    const prefix = '/api/v1/' + (admin ? 'admin' : 'auth') + '/mfa';
    const setup = await call(prefix + '/setup', { ...principal, data: { currentPassword: password } }); assert.equal(setup.status, 201);
    const secret = decode32(setup.body.secret);
    const enabled = await call(prefix + '/enable', { ...principal, data: { setupToken: setup.body.setupToken, code: totpAt(secret, Math.floor(now / 30000)) } });
    assert.equal(enabled.status, 200); assert.equal(enabled.body.recoveryCodes.length, 10); return { secret, codes: enabled.body.recoveryCodes, first };
  }
  return { get server() { return server; }, call, login, verify, enroll, next, now: () => now, userId, adminId, errors, get store() { return store; },
    async restart() { assert.notEqual(dbPath, ':memory:'); await stop(); store.close(); store = new Store(dbPath); await start(); },
    provider: () => new MfaProvider({ store, vault, now: () => now, monotonic: () => mono }), rollback: ms => { now -= ms; } };
}

export { fixture, password };
