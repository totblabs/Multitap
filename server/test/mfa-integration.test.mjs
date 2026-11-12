// Actual isolated SQLite + HTTP/TLS. Synthetic users/keys; no production IO.
import test from 'node:test';
import assert from 'node:assert/strict';
import { createServer, request as httpRequest } from 'node:http';
import { request as httpsRequest } from 'node:https';
import { generateKeyPairSync, sign } from 'node:crypto';
import { mkdtempSync, rmSync, copyFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { Store } from '../src/store.mjs';
import { Store as Store18 } from './fixtures/mfa-store18.mjs';
import { Vault } from '../src/vault.mjs';
import { MfaProvider } from '../src/mfa.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword, newToken, tokenHash } from '../src/security.mjs';
import { totpAt } from '../src/mfa-primitives.mjs';
import { normalizeServerAccess } from '../update/server-access-policy.mjs';

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
  return { call, login, verify, enroll, next, now: () => now, userId, adminId, errors, get store() { return store; },
    async restart() { assert.notEqual(dbPath, ':memory:'); await stop(); store.close(); store = new Store(dbPath); await start(); },
    provider: () => new MfaProvider({ store, vault, now: () => now, monotonic: () => mono }), rollback: ms => { now -= ms; } };
}
const challenge = response => { assert.equal(response.status, 202); assert.equal(response.cookie, undefined); assert.equal(response.body.sessionToken, undefined); return response.body.mfa.challengeToken; };

test('MFA-OFF retains actual old HTTP login and explicit replacement semantics', async t => {
  const f = await fixture(t, { transport: 'label' }), first = await f.login(); assert.equal(first.status, 200);
  const second = await f.login(); assert.equal(second.status, 409); assert.equal(second.body.error.code, 'login_confirmation_required');
  assert.equal((await f.call('/api/v1/auth/me', { token: first.body.sessionToken })).status, 200);
  assert.equal(f.store.db.prepare('PRAGMA user_version').get().user_version, 19);
  assert.equal(f.store.db.prepare('SELECT count(*) n FROM account_mfa').get().n, 0);
});
test('real proxy enrollment is opt-in, secret is encrypted, activation revokes sessions and consumes enrollment step', async t => {
  const f = await fixture(t), e = await f.enroll();
  assert.equal((await f.call('/api/v1/auth/me', { token: e.first.body.sessionToken })).status, 401);
  assert.equal(f.store.db.prepare('SELECT count(*) n FROM sessions WHERE user_id=?').get(f.userId).n, 0);
  assert.equal(f.store.db.prepare('SELECT factor_ciphertext FROM account_mfa').get().factor_ciphertext.includes(e.secret.toString('hex')), false);
  assert.equal((await f.login(false, false)).status, 428);
  const c = challenge(await f.login());
  assert.equal((await f.verify(c, totpAt(e.secret, Math.floor(f.now() / 30000)))).status, 400);
  f.next(); const accepted = await f.verify(c, totpAt(e.secret, Math.floor(f.now() / 30000))); assert.equal(accepted.status, 200);
  assert.equal((await f.call('/api/v1/auth/mfa', { token: accepted.body.sessionToken })).body.enabled, true);
});
test('MFA success reaches the exact existing replacement question without early revocation or OTP replay', async t => {
  const f = await fixture(t), e = await f.enroll(); f.next();
  const first = await f.verify(challenge(await f.login()), totpAt(e.secret, Math.floor(f.now() / 30000))); assert.equal(first.status, 200);
  const c = challenge(await f.login()); f.next();
  const second = await f.verify(c, totpAt(e.secret, Math.floor(f.now() / 30000)));
  assert.equal(second.status, 409); assert.equal(second.body.error.code, 'login_confirmation_required'); assert.equal(second.cookie, undefined);
  assert.equal((await f.call('/api/v1/auth/me', { token: first.body.sessionToken })).status, 200);
  assert.equal((await f.verify(c, totpAt(e.secret, Math.floor(f.now() / 30000)))).status, 410);
  const admitted = await f.call('/api/v1/auth/login/confirm', { data: { challengeToken: second.body.error.details.challengeToken, confirm: true }, extra: headers });
  assert.equal(admitted.status, 200); assert.equal(admitted.body.sessionAccess.protocol, 'session-access-v1');
  assert.equal((await f.call('/api/v1/auth/me', { token: first.body.sessionToken })).body.error.code, 'pc_login_replaced');
});
test('one recovery code wins simultaneous different challenges exactly once', async t => {
  const f = await fixture(t), e = await f.enroll(), a = challenge(await f.login()), b = challenge(await f.login());
  const responses = await Promise.all([f.verify(a, e.codes[0], { recovery: true }), f.verify(b, e.codes[0], { recovery: true })]);
  assert.deepEqual(responses.map(x => x.status).sort(), [200, 400]);
  assert.equal(f.store.db.prepare('SELECT count(*) n FROM mfa_recovery_codes WHERE consumed_at IS NOT NULL').get().n, 1);
});
test('durable MFA limiter survives fresh challenges/restart and cannot expire through clock rollback', async t => {
  const f = await fixture(t, { file: true }), e = await f.enroll();
  const initial = challenge(await f.login());
  for (let i = 0; i < 5; i++) {
    const c = i === 4 ? challenge(await f.login()) : initial;
    const response = await f.verify(c, 'not-six-digits'); assert.equal(response.status, 400);
    const rejected = await f.verify(c, totpAt(e.secret, Math.floor(f.now() / 30000))); assert.equal(rejected.status, i === 4 ? 429 : 400);
  }
  assert.equal(f.store.db.prepare('SELECT failures FROM mfa_failure_budgets').get().failures, 5);
  await f.restart();
  const c = challenge(await f.login()); f.rollback(1000);
  assert.equal((await f.verify(c, e.codes[0], { recovery: true })).status, 410, 'wall rollback expires the live challenge');
  const next = challenge(await f.login()); assert.equal((await f.verify(next, e.codes[0], { recovery: true })).status, 429);
  f.next(62000); assert.equal((await f.verify(next, e.codes[0], { recovery: true })).status, 200);
});
test('admin has no cookie/CSRF before MFA, challenge audiences do not cross', async t => {
  const f = await fixture(t), e = await f.enroll(true); f.next();
  const pending = await f.login(true), c = challenge(pending); assert.equal(pending.body.csrfToken, undefined);
  assert.equal((await f.verify(c, e.codes[0], { recovery: true })).status, 403);
  const accepted = await f.verify(c, totpAt(e.secret, Math.floor(f.now() / 30000)), { admin: true });
  assert.equal(accepted.status, 200); assert.ok(accepted.cookie); assert.ok(accepted.body.csrfToken);
  assert.equal(f.store.db.prepare("SELECT count(*) n FROM sessions WHERE purpose='admin'").get().n, 1);
});
test('authorization disable/enable round trip invalidates MFA proof despite same current enabled state', async t => {
  const f = await fixture(t), e = await f.enroll(), c = challenge(await f.login());
  f.store.setUserEnabled('alice', false); f.store.setUserEnabled('alice', true);
  assert.equal((await f.verify(c, e.codes[0], { recovery: true })).status, 410);
  assert.equal(f.store.db.prepare('SELECT count(*) n FROM sessions WHERE user_id=?').get(f.userId).n, 0);
});
test('factor recovery regeneration requires recent factor proof and atomically revokes all own sessions', async t => {
  const f = await fixture(t), e = await f.enroll();
  const admitted = await f.verify(challenge(await f.login()), e.codes[0], { recovery: true }), token = admitted.body.sessionToken;
  const denied = await f.call('/api/v1/auth/mfa/recovery', { token, data: { currentPassword: password } }); assert.equal(denied.status, 403);
  const reauth = await f.call('/api/v1/auth/mfa/reauth', { token, data: { currentPassword: password, recoveryCode: e.codes[1] } }); assert.equal(reauth.status, 200);
  const regenerated = await f.call('/api/v1/auth/mfa/recovery', { token, data: { currentPassword: password } }); assert.equal(regenerated.status, 200);
  assert.equal(regenerated.body.requiresLogin, true); assert.equal((await f.call('/api/v1/auth/me', { token })).status, 401);
  assert.equal(f.store.db.prepare('SELECT generation FROM account_mfa WHERE user_id=?').get(f.userId).generation, 2);
  assert.equal((await f.verify(challenge(await f.login()), e.codes[2], { recovery: true })).status, 400);
});
test('HTTPS label and spoofed forwarding alone cannot authorize MFA', async t => {
  const f = await fixture(t, { transport: 'label' });
  // Explicit synthetic fixture enrollment avoids calling a live setup endpoint.
  const seed = Buffer.alloc(20, 2), user = f.store.findUser('alice');
  const { sealFactor } = await import('../src/mfa-primitives.mjs');
  f.store.db.prepare('INSERT INTO account_mfa(user_id,generation,enabled,factor_ciphertext,changed_at) VALUES(?,?,?,?,?)').run(user.id, 1, 1, sealFactor(new Vault(Buffer.alloc(32, 23)), user.id, 1, seed), f.now());
  const response = await f.call('/api/v1/auth/login', { data: { username: 'alice', password }, extra: { ...headers, 'X-Forwarded-Proto': 'https' } });
  assert.equal(response.status, 403); assert.equal(response.body.error.code, 'mfa_requires_https');
  assert.equal(f.store.db.prepare('SELECT count(*) n FROM sessions').get().n, 0);
});
test('actual loopback TLS permits MFA enrollment and verifies against the local fixture CA', async t => {
  const f = await fixture(t, { transport: 'direct' }), e = await f.enroll(); f.next();
  assert.equal((await f.verify(challenge(await f.login()), totpAt(e.secret, Math.floor(f.now() / 30000)))).status, 200);
});
test('local reset refuses mismatched UUID and revokes exactly the named account', async t => {
  const f = await fixture(t), e = await f.enroll();
  await f.verify(challenge(await f.login()), e.codes[0], { recovery: true });
  const other = await f.login(true); assert.equal(other.status, 200);
  assert.throws(() => f.provider().resetLocal(f.userId, f.adminId), /mfa_reset_confirmation_required/);
  assert.equal(f.provider().resetLocal(f.userId, f.userId).reset, true);
  assert.equal(f.store.db.prepare('SELECT count(*) n FROM sessions WHERE user_id=?').get(f.userId).n, 0);
  assert.equal(f.store.db.prepare('SELECT count(*) n FROM sessions WHERE user_id=?').get(f.adminId).n, 1);
});
test('one TOTP step wins simultaneous challenges exactly once', async t => {
  const f = await fixture(t), e = await f.enroll(), a = challenge(await f.login()), b = challenge(await f.login()); f.next();
  const code = totpAt(e.secret, Math.floor(f.now() / 30000));
  const responses = await Promise.all([f.verify(a, code), f.verify(b, code)]);
  assert.deepEqual(responses.map(value => value.status).sort(), [200, 400]);
  assert.equal(f.store.db.prepare('SELECT count(*) n FROM sessions WHERE user_id=?').get(f.userId).n, 1);
});
test('actual SQLite rollback cannot consume an OTP or issue a session', async t => {
  const f = await fixture(t), e = await f.enroll(), c = challenge(await f.login()); f.next();
  const before = f.store.db.prepare('SELECT last_accepted_step FROM account_mfa WHERE user_id=?').get(f.userId).last_accepted_step;
  f.store.db.exec(`CREATE TRIGGER synthetic_mfa_abort BEFORE INSERT ON sessions WHEN NEW.user_id='${f.userId}' BEGIN SELECT RAISE(ABORT,'synthetic_mfa_commit_failure'); END;`);
  const code = totpAt(e.secret, Math.floor(f.now() / 30000));
  assert.equal((await f.verify(c, code)).status, 500);
  assert.equal(f.errors.length, 1, 'one deliberate SQLite trigger failure was logged'); f.errors.length = 0;
  assert.equal(f.store.db.prepare('SELECT last_accepted_step FROM account_mfa WHERE user_id=?').get(f.userId).last_accepted_step, before);
  assert.equal(f.store.db.prepare('SELECT count(*) n FROM sessions WHERE user_id=?').get(f.userId).n, 0);
  f.store.db.exec('DROP TRIGGER synthetic_mfa_abort');
  assert.equal((await f.verify(c, code)).status, 200, 'the SAME original challenge/code may retry only because the transaction never committed');
});
test('authorization revision fences an old password-only confirmation even when account/session visible fields return unchanged', async t => {
  const f = await fixture(t), first = await f.login(), pending = await f.login();
  const before = f.store.db.prepare('SELECT authorization_revision FROM users WHERE id=?').get(f.userId).authorization_revision;
  // Deliberately retain rows to isolate the real revision trigger from the
  // normal Store disable path's independent session deletion protection.
  f.store.transaction(() => {
    f.store.db.prepare('UPDATE users SET disabled=1 WHERE id=?').run(f.userId);
    f.store.db.prepare('UPDATE users SET disabled=0 WHERE id=?').run(f.userId);
  });
  assert.equal(f.store.db.prepare('SELECT authorization_revision FROM users WHERE id=?').get(f.userId).authorization_revision, before + 2);
  assert.equal((await f.call('/api/v1/auth/me', { token: first.body.sessionToken })).status, 200);
  const confirmed = await f.call('/api/v1/auth/login/confirm', { data: { challengeToken: pending.body.error.details.challengeToken, confirm: true }, extra: headers });
  assert.equal(confirmed.status, 409); assert.equal(confirmed.body.error.code, 'login_confirmation_changed');
  assert.equal((await f.call('/api/v1/auth/me', { token: first.body.sessionToken })).status, 200);
});
test('console MFA keeps scoped CSRF/cookie-only grants and admin disable revokes both real audiences', async t => {
  const f = await fixture(t), e = await f.enroll(true);
  const admin = await f.verify(challenge(await f.login(true)), e.codes[0], { admin: true, recovery: true }); assert.equal(admin.status, 200);
  const pending = await f.call('/console/api/login', { data: { username: 'administrator', password }, extra: { 'X-Multitap-Auth-Protocol': 'mfa-v1' } });
  const c = challenge(pending); assert.ok(pending.body.csrfToken);
  const denied = await f.call('/console/api/mfa/verify', { data: { challengeToken: c, recoveryCode: e.codes[1] } }); assert.equal(denied.status, 403);
  const accepted = await f.call('/console/api/mfa/verify', { data: { challengeToken: c, recoveryCode: e.codes[1] }, extra: { 'X-Multitap-Console-CSRF': pending.body.csrfToken } });
  assert.equal(accepted.status, 200); assert.ok(accepted.cookie); assert.equal(accepted.body.sessionToken, undefined);
  const principal = { cookie: admin.cookie, extra: { 'X-CSRF-Token': admin.body.csrfToken } };
  assert.equal((await f.call('/api/v1/admin/mfa/reauth', { ...principal, data: { currentPassword: password, recoveryCode: e.codes[2] } })).status, 200);
  const disabled = await f.call('/api/v1/admin/mfa/disable', { ...principal, data: { currentPassword: password } }); assert.equal(disabled.status, 200);
  assert.equal(f.store.db.prepare('SELECT count(*) n FROM sessions WHERE user_id=?').get(f.adminId).n, 0);
  assert.equal((await f.call('/api/v1/admin/session', { cookie: admin.cookie })).status, 401);
  assert.equal((await f.call('/console/api/session', { cookie: accepted.cookie })).status, 401);
  assert.equal(f.store.db.prepare('SELECT enabled FROM account_mfa WHERE user_id=?').get(f.adminId).enabled, 0);
});
test('authenticated current-password failures use the same durable account budget before new password work', async t => {
  const f = await fixture(t), first = await f.login(), token = first.body.sessionToken;
  for (let i = 0; i < 5; i++) {
    const rejected = await f.call('/api/v1/auth/mfa/setup', { token, data: { currentPassword: 'synthetic-wrong-password' } });
    assert.equal(rejected.status, i === 4 ? 429 : 403);
  }
  assert.equal(f.store.db.prepare('SELECT failures FROM mfa_failure_budgets WHERE user_id=?').get(f.userId).failures, 5);
  assert.equal((await f.call('/api/v1/auth/mfa/setup', { token, data: { currentPassword: password } })).status, 429);
  f.next(60001);
  assert.equal((await f.call('/api/v1/auth/mfa/setup', { token, data: { currentPassword: password } })).status, 201);
  assert.equal(f.store.db.prepare('SELECT count(*) n FROM account_mfa').get().n, 0, 'setup and lockout never enable MFA');
});
test('unreadable encrypted factor is a fixed unavailable error, never MFA-OFF or a session', async t => {
  const f = await fixture(t), e = await f.enroll(), c = challenge(await f.login()); f.next();
  f.store.db.prepare('UPDATE account_mfa SET factor_ciphertext=? WHERE user_id=?').run('synthetic-corrupt-ciphertext', f.userId);
  const unavailable = await f.verify(c, totpAt(e.secret, Math.floor(f.now() / 30000)));
  assert.equal(unavailable.status, 503); assert.equal(unavailable.body.error.code, 'mfa_unavailable');
  assert.equal(f.store.db.prepare('SELECT enabled FROM account_mfa WHERE user_id=?').get(f.userId).enabled, 1);
  assert.equal(f.store.db.prepare('SELECT count(*) n FROM sessions WHERE user_id=?').get(f.userId).n, 0);
});
test('actual preimage schema18 migration preserves existing authority/data and its separate backup remains readable by18', async t => {
  const dir = mkdtempSync(join(tmpdir(), 'multitap-mfa-schema18-')); t.after(() => rmSync(dir, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }));
  const filename = resolve(dir, 'synthetic18.sqlite'), backup = resolve(dir, 'before19.sqlite');
  const vault = new Vault(Buffer.alloc(32, 23)), legacy = new Store18(filename), token = newToken();
  const userId = legacy.addUser({ name: 'preserved', passwordHash: await hashed });
  const encrypted = vault.seal({ synthetic: 'preserved-setting' }); legacy.saveSetting('fixture-preserved', encrypted, userId);
  legacy.createSession(legacy.findUser('preserved'), tokenHash(token), Date.now(), 3600000, 'client');
  assert.equal(legacy.db.prepare('PRAGMA user_version').get().user_version, 18); legacy.close();
  copyFileSync(filename, backup);
  const migrated = new Store(filename);
  try {
    assert.equal(migrated.db.prepare('PRAGMA user_version').get().user_version, 19);
    assert.equal(migrated.findUser('preserved').id, userId); assert.equal(migrated.authenticate(tokenHash(token)).id, userId);
    assert.equal(migrated.db.prepare('SELECT count(*) n FROM account_mfa').get().n, 0);
    assert.equal(migrated.findUser('preserved').authorization_revision, 1);
    assert.equal(migrated.db.prepare('SELECT value FROM settings WHERE key=?').get('fixture-preserved').value, encrypted);
  } finally { migrated.close(); }
  assert.throws(() => new Store18(filename), /database_version_too_new/);
  const restored = new Store18(backup);
  try {
    assert.equal(restored.authenticate(tokenHash(token)).id, userId);
    assert.deepEqual(vault.open(restored.db.prepare('SELECT value FROM settings WHERE key=?').get('fixture-preserved').value), { synthetic: 'preserved-setting' });
  } finally { restored.close(); }
});
test('blocks in a row grow from 1 minute towards 1 hour, 30 failed codes in a day leave only a recovery code, and the log groups failures', async t => {
  const f = await fixture(t), e = await f.enroll();
  const budget = () => f.store.db.prepare('SELECT * FROM mfa_failure_budgets WHERE user_id=?').get(f.userId);
  const wrong = () => totpAt(e.secret, Math.floor(f.now() / 30000) + 1000);
  const minutes = [];
  for (let block = 1; block <= 6; block++) {
    const c = challenge(await f.login());
    for (let i = 0; i < 5; i++) assert.equal((await f.verify(c, wrong())).status, i === 4 ? 429 : 400, `block ${block} attempt ${i}`);
    const row = budget(); minutes.push((row.blocked_until - f.now()) / 60000);
    assert.equal(row.block_streak, block); assert.equal(row.day_failures, block * 5);
    // While blocked nothing is checked, not even a recovery code.
    assert.equal((await f.verify(challenge(await f.login()), e.codes[1], { recovery: true })).status, 429);
    f.next(row.blocked_until - f.now() + 1000);
  }
  assert.deepEqual(minutes, [1, 2, 4, 8, 16, 32]);
  // 30 failed authenticator codes today: a correct code is refused without being checked, a recovery code works.
  const late = challenge(await f.login()), refused = await f.verify(late, totpAt(e.secret, Math.floor(f.now() / 30000)));
  assert.equal(refused.status, 429); assert.equal(refused.body.error.code, 'too_many_attempts'); assert.deepEqual(refused.body.error.details, { recoveryCodeOnly: true });
  assert.equal(budget().day_failures, 30); assert.equal(budget().failures, 0, 'the refusal is not counted as another failure');
  assert.equal((await f.verify(late, e.codes[0], { recovery: true })).status, 200);
  assert.equal(budget().block_streak, 0, 'an accepted code starts the blocks in a row again'); assert.equal(budget().day_failures, 30, 'the day still counts');
  f.next(60000);
  assert.equal((await f.verify(challenge(await f.login()), totpAt(e.secret, Math.floor(f.now() / 30000)))).body.error?.details?.recoveryCodeOnly, true);
  // Once the 24 hours since the first failed code have passed, the authenticator works again.
  f.next(86400000);
  const back = await f.verify(challenge(await f.login()), totpAt(e.secret, Math.floor(f.now() / 30000)));
  assert.ok([200, 409].includes(back.status), JSON.stringify(back.body)); assert.notEqual(back.body.error?.code, 'too_many_attempts');
  // The log: one mfa_failed row per block (the five came within a minute) with its count, one mfa_locked per block and one for the day.
  const audit = event => f.store.db.prepare('SELECT subject FROM audit_events WHERE event=? ORDER BY id').all(event).map(row => row.subject);
  assert.deepEqual(audit('mfa_failed'), Array(6).fill(f.userId + ':5'));
  assert.deepEqual(audit('mfa_locked'), [...[1, 2, 4, 8, 16].map(m => f.userId + ':' + m), f.userId + ':day', f.userId + ':32']);
});
test('a failed code a minute after the last row starts a new mfa_failed row; blocks stop growing at 1 hour', async t => {
  const f = await fixture(t), e = await f.enroll();
  const wrong = () => totpAt(e.secret, Math.floor(f.now() / 30000) + 1000);
  const c = challenge(await f.login());
  assert.equal((await f.verify(c, wrong())).status, 400); f.next(30000);
  assert.equal((await f.verify(c, wrong())).status, 400); f.next(31000);
  assert.equal((await f.verify(c, wrong())).status, 400);
  assert.deepEqual(f.store.db.prepare("SELECT subject FROM audit_events WHERE event='mfa_failed' ORDER BY id").all().map(row => row.subject), [f.userId + ':2', f.userId + ':1']);
  // Seven and more blocks in a row: 1 hour each, never more.
  f.store.db.prepare('UPDATE mfa_failure_budgets SET block_streak=9,failures=0,window_started_at=0 WHERE user_id=?').run(f.userId);
  const d = challenge(await f.login());
  for (let i = 0; i < 5; i++) await f.verify(d, wrong());
  const row = f.store.db.prepare('SELECT * FROM mfa_failure_budgets WHERE user_id=?').get(f.userId);
  assert.equal(row.blocked_until - f.now(), 3600000);
});
