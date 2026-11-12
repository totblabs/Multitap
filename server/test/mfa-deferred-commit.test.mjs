// ADDON ONLY. Actual sealed340 product bytes, in-memory SQLite and loopback HTTP.
// Observers return original authority results unchanged. No real account/keys.
import test from 'node:test';
import assert from 'node:assert/strict';
import { createServer, request } from 'node:http';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword, tokenHash } from '../src/security.mjs';
import { totpAt } from '../src/mfa-primitives.mjs';
import { normalizeServerAccess } from '../update/server-access-policy.mjs';

const origin = 'https://mfa-deferred.fixture.invalid', password = 'Synthetic-deferred-MFA-password-48!';
const intent = { 'X-Multitap-Auth-Protocol': 'mfa-v1', 'X-Multitap-Session-Policy': 'windows-pc-v1', 'X-Multitap-Login-Intent': 'confirmable-v1' };
function decode32(text) {
  let bits = 0, value = 0; const result = [];
  for (const char of text) { value = (value << 5) | 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567'.indexOf(char); bits += 5; if (bits >= 8) result.push((value >>> (bits -= 8)) & 255); }
  return Buffer.from(result);
}

test('deferred COMMIT failure after allocating MFA409 cancels only its unpublished proof and preserves old accepted input/factor state', async t => {
  const reservation = createServer(); await new Promise(resolve => reservation.listen(0, '127.0.0.1', resolve));
  const port = reservation.address().port; await new Promise(resolve => reservation.close(resolve));
  const config = normalizeServerAccess({ protocol: 'server-access-v1', primaryOrigin: origin,
    https: { enabled: true, origin, mode: 'proxy', listenHost: '127.0.0.1', listenPort: port },
    http: { enabled: false, origin: null, listenHost: '127.0.0.1', listenPort: port }, trustedProxies: ['127.0.0.1'] });
  const store = new Store(':memory:'), vault = new Vault(Buffer.alloc(32, 29));
  const passwordHash = await hashPassword(password);
  const userId = store.addUser({ name: 'deferred-user', passwordHash });
  store.addUser({ name: 'deferred-admin', passwordHash, role: 'admin' });
  let now = Date.now(), mono = 0;
  const errors = [];
  const server = await buildServer({ store, vault, publicOrigin: origin, accessConfig: config, trustedProxies: config.trustedProxies,
    sessionAccessOptions: { now: () => now }, mfaOptions: { now: () => now, monotonic: () => mono }, logger: { error: value => errors.push(value) } });
  await new Promise(resolve => server.listen(port, '127.0.0.1', resolve));
  t.after(async () => {
    server.closeAllConnections(); await new Promise(resolve => server.close(resolve)); store.close();
    assert.equal(errors.length, 1, 'the single deliberately failed real COMMIT is the only internal error');
    assert.equal(JSON.parse(errors[0]).event, 'request_error');
  });
  function call(path, { data, token } = {}) {
    const payload = data === undefined ? undefined : JSON.stringify(data);
    return new Promise((resolve, reject) => {
      const req = request({ host: '127.0.0.1', port, path: '/api/v1' + path, method: payload === undefined ? 'GET' : 'POST', headers: {
        Host: new URL(origin).host, Origin: origin, 'X-Forwarded-Proto': 'https', ...intent,
        ...(token ? { Authorization: 'Bearer ' + token } : {}),
        ...(payload === undefined ? {} : { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(payload) }),
      } }, res => {
        const chunks = []; res.on('data', bytes => chunks.push(bytes)); res.on('end', () => {
          try { resolve({ status: res.statusCode, body: JSON.parse(Buffer.concat(chunks)), cookie: res.headers['set-cookie'] }); } catch (error) { reject(error); }
        });
      }); req.on('error', reject); req.setTimeout(5000, () => req.destroy(Error('synthetic request deadline'))); req.end(payload);
    });
  }
  const login = () => call('/auth/login', { data: { username: 'deferred-user', password } });
  const verify = (challengeToken, code) => call('/auth/mfa/verify', { data: { challengeToken, code } });
  const advance = ms => { now += ms; mono += ms; };
  const initial = await login(); assert.equal(initial.status, 200);
  const setup = await call('/auth/mfa/setup', { token: initial.body.sessionToken, data: { currentPassword: password } }); assert.equal(setup.status, 201);
  const secret = decode32(setup.body.secret);
  t.after(() => secret.fill(0));
  const enabled = await call('/auth/mfa/enable', { token: initial.body.sessionToken, data: {
    setupToken: setup.body.setupToken, code: totpAt(secret, Math.floor(now / 30000)),
  } }); assert.equal(enabled.status, 200);
  advance(30000);
  const firstChallenge = await login(); assert.equal(firstChallenge.status, 202);
  const first = await verify(firstChallenge.body.mfa.challengeToken, totpAt(secret, Math.floor(now / 30000))); assert.equal(first.status, 200);
  const oldToken = first.body.sessionToken, oldHash = tokenHash(oldToken);
  assert.equal((await call('/auth/activity', { token: oldToken, data: {} })).status, 200);
  advance(1000);
  const latestInput = await call('/auth/activity', { token: oldToken, data: {} }); assert.equal(latestInput.status, 200);
  const activityBefore = store.db.prepare('SELECT * FROM session_activity WHERE token_hash=?').get(oldHash);
  const deadlineBefore = server.sessionAccess.describe(oldHash).idleDeadline;
  assert.equal(deadlineBefore, latestInput.body.idleDeadline);
  assert.equal(Date.parse(deadlineBefore) - activityBefore.last_input_at, 1801000,
    'the exact latest accepted input is deliberately1000ms newer than durable activity');
  const sessionsBefore = store.db.prepare('SELECT * FROM sessions ORDER BY token_hash').all();
  const factorBefore = store.db.prepare('SELECT * FROM account_mfa WHERE user_id=?').get(userId);
  const auditBefore = store.db.prepare('SELECT * FROM audit_events ORDER BY id').all();
  const pending = await login(); assert.equal(pending.status, 202);
  const code = totpAt(secret, factorBefore.last_accepted_step + 1);
  const observed = [], inside = [], commits = [];
  const actualBegin = server.sessionAccess.begin.bind(server.sessionAccess);
  server.sessionAccess.begin = (...args) => {
    const value = actualBegin(...args);
    if (value.kind === 'confirmation-required') {
      observed.push(value.challenge);
      inside.push({ transaction: store.db.isTransaction,
        consumedStep: store.db.prepare('SELECT last_accepted_step FROM account_mfa WHERE user_id=?').get(userId).last_accepted_step,
        deferredRows: store.db.prepare('SELECT count(*) n FROM synthetic_mfa_deferred').get().n,
        oldSessions: store.db.prepare('SELECT count(*) n FROM sessions WHERE token_hash=?').get(oldHash).n });
    }
    return value;
  };
  // SQLite permits this FK violation during statements, then rejects COMMIT.
  // It fires only on real factor consumption, before finishLogin allocates409.
  store.db.exec(`CREATE TABLE synthetic_mfa_deferred(user_id TEXT REFERENCES users(id) DEFERRABLE INITIALLY DEFERRED);
    CREATE TRIGGER synthetic_mfa_late_failure AFTER UPDATE OF last_accepted_step ON account_mfa
    WHEN NEW.user_id='${userId}' BEGIN INSERT INTO synthetic_mfa_deferred(user_id) VALUES('synthetic-no-such-user'); END;`);
  const actualExec = store.db.exec.bind(store.db);
  store.db.exec = sql => {
    if (sql !== 'COMMIT') return actualExec(sql);
    try { const value = actualExec(sql); commits.push('committed'); return value; }
    catch (error) { commits.push('failed'); throw error; }
  };
  const failed = await verify(pending.body.mfa.challengeToken, code);
  assert.equal(failed.status, 500); assert.equal(failed.body.sessionToken, undefined); assert.equal(failed.cookie, undefined);
  assert.deepEqual(commits, ['failed'], 'failure was deferred until real outer COMMIT');
  assert.equal(observed.length, 1, 'actual finishLogin allocated its RAM confirmation before COMMIT failed');
  assert.deepEqual(inside[0], { transaction: true, consumedStep: factorBefore.last_accepted_step + 1, deferredRows: 1, oldSessions: 1 });
  assert.deepEqual(store.db.prepare('SELECT * FROM sessions ORDER BY token_hash').all(), sessionsBefore);
  assert.deepEqual(store.db.prepare('SELECT * FROM session_activity WHERE token_hash=?').get(oldHash), activityBefore);
  assert.deepEqual(store.db.prepare('SELECT * FROM account_mfa WHERE user_id=?').get(userId), factorBefore);
  assert.deepEqual(store.db.prepare('SELECT * FROM audit_events ORDER BY id').all(), auditBefore);
  assert.equal(store.db.prepare('SELECT count(*) n FROM synthetic_mfa_deferred').get().n, 0);
  assert.equal(server.sessionAccess.describe(oldHash).idleDeadline, deadlineBefore, 'accepted-input cache was not discarded');
  assert.throws(() => server.sessionAccess.describeConfirmation(observed[0], () => true), error => error.code === 'login_confirmation_invalid');
  const canceled = await call('/auth/login/confirm', { data: { challengeToken: observed[0], confirm: true } });
  assert.equal(canceled.status, 409); assert.equal(canceled.body.error.code, 'login_confirmation_changed');
  assert.equal((await call('/auth/me', { token: oldToken })).status, 200);
  store.db.exec('DROP TRIGGER synthetic_mfa_late_failure');
  // Retry is an EXPLICIT HTTP request with an unconsumed MFA proof. No old
  // replacement capability is restored, and the successful new one is distinct.
  const retried = await verify(pending.body.mfa.challengeToken, code);
  assert.equal(retried.status, 409); assert.equal(retried.body.error.code, 'login_confirmation_required');
  assert.equal(observed.length, 2); assert.notEqual(observed[1], observed[0]);
  assert.equal(retried.body.error.details.challengeToken, observed[1]);
  assert.equal((await call('/auth/me', { token: oldToken })).status, 200);
  assert.equal(server.sessionAccess.describe(oldHash).idleDeadline, deadlineBefore);
  assert.throws(() => server.sessionAccess.describeConfirmation(observed[0], () => true), error => error.code === 'login_confirmation_invalid');
  const accepted = await call('/auth/login/confirm', { data: { challengeToken: observed[1], confirm: true } });
  assert.equal(accepted.status, 200); assert.notEqual(accepted.body.sessionToken, oldToken);
  const retired = await call('/auth/me', { token: oldToken }); assert.equal(retired.status, 401); assert.equal(retired.body.error.code, 'pc_login_replaced');
});
