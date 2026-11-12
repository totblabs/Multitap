// NEW regressions: actual private340 Store/provider/HTTP, synthetic identities.
// Real BEGIN is delegated unchanged; clock advance models elapsed lock wait.
import test from 'node:test';
import assert from 'node:assert/strict';
import { tokenHash } from '../src/security.mjs';
import { totpAt } from '../src/mfa-primitives.mjs';
import { fixture, password } from './helpers/mfa-management-fixture.mjs';

function decode32(text) {
  let bits = 0, value = 0; const out = [];
  for (const char of text) { value = (value << 5) | 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567'.indexOf(char); bits += 5; if (bits >= 8) out.push((value >>> (bits -= 8)) & 255); }
  return Buffer.from(out);
}
const pending = response => { assert.equal(response.status, 202); return response.body.mfa.challengeToken; };

test('enabled-factor replacement rechecks five-minute MFA proof after actual BEGIN completes late while setup is still fresh', async t => {
  const f = await fixture(t), initialFactor = await f.enroll();
  const login = await f.verify(pending(await f.login()), initialFactor.codes[0], { recovery: true }); assert.equal(login.status, 200);
  const token = login.body.sessionToken;
  const recent = await f.call('/api/v1/auth/mfa/reauth', { token, data: { currentPassword: password, recoveryCode: initialFactor.codes[1] } }); assert.equal(recent.status, 200);
  f.next(299000);
  const setup = await f.call('/api/v1/auth/mfa/setup', { token, data: { currentPassword: password } }); assert.equal(setup.status, 201);
  const secret = decode32(setup.body.secret); t.after(() => secret.fill(0));
  const factorBefore = f.store.db.prepare('SELECT * FROM account_mfa WHERE user_id=?').get(f.userId);
  const recoveryBefore = f.store.db.prepare('SELECT * FROM mfa_recovery_codes WHERE user_id=? ORDER BY code_hash').all(f.userId);
  const sessionsBefore = f.store.db.prepare('SELECT * FROM sessions ORDER BY token_hash').all();
  const actualExec = f.store.db.exec.bind(f.store.db); let advanced = false;
  f.store.db.exec = sql => {
    const value = actualExec(sql);
    if (sql === 'BEGIN IMMEDIATE' && !advanced) { advanced = true; f.next(2000); }
    return value;
  };
  const candidate = totpAt(secret, Math.floor(f.now() / 30000));
  const rejected = await f.call('/api/v1/auth/mfa/enable', { token, data: { setupToken: setup.body.setupToken, code: candidate } });
  assert.equal(advanced, true);
  assert.ok(Date.parse(setup.body.expiresAt) > f.now(), 'setup remains fresh; only the old recent-MFA proof expired');
  assert.equal(rejected.status, 403);
  assert.equal(rejected.body.error.code, 'mfa_reauth_required');
  assert.equal(rejected.body.recoveryCodes, undefined);
  assert.deepEqual(f.store.db.prepare('SELECT * FROM account_mfa WHERE user_id=?').get(f.userId), factorBefore);
  assert.deepEqual(f.store.db.prepare('SELECT * FROM mfa_recovery_codes WHERE user_id=? ORDER BY code_hash').all(f.userId), recoveryBefore);
  assert.deepEqual(f.store.db.prepare('SELECT * FROM sessions ORDER BY token_hash').all(), sessionsBefore);
  assert.equal((await f.call('/api/v1/auth/me', { token })).status, 200);
});

test('failed actual enable retains due accepted-input dirty state until a later real autocommit flush, without extending idle authority', async t => {
  const f = await fixture(t), login = await f.login(); assert.equal(login.status, 200);
  const token = login.body.sessionToken, hash = tokenHash(token);
  const setup = await f.call('/api/v1/auth/mfa/setup', { token, data: { currentPassword: password } }); assert.equal(setup.status, 201);
  const secret = decode32(setup.body.secret); t.after(() => secret.fill(0));
  assert.equal((await f.call('/api/v1/auth/activity', { token, data: {} })).status, 200);
  f.next(1000);
  const latest = await f.call('/api/v1/auth/activity', { token, data: {} }); assert.equal(latest.status, 200);
  const rowBefore = f.store.db.prepare('SELECT * FROM session_activity WHERE token_hash=?').get(hash);
  const latestInput = f.now(), deadline = latest.body.idleDeadline;
  assert.equal(latestInput - rowBefore.last_input_at, 1000, 'there is real not-yet-durable accepted input');
  assert.equal(Date.parse(deadline), latestInput + 1800000);
  const sessionsBefore = f.store.db.prepare('SELECT * FROM sessions ORDER BY token_hash').all();
  const inside = [], actualGuard = f.store.sessionAccessGuard;
  f.store.sessionAccessGuard = value => {
    const inTransaction = f.store.db.isTransaction;
    const allowed = actualGuard(value);
    if (inTransaction && value === hash) inside.push({ allowed,
      lastInputAt: f.store.db.prepare('SELECT last_input_at FROM session_activity WHERE token_hash=?').get(hash)?.last_input_at });
    return allowed;
  };
  f.store.db.exec(`CREATE TRIGGER synthetic_mfa_abort_factor BEFORE INSERT ON account_mfa
    WHEN NEW.user_id='${f.userId}' BEGIN SELECT RAISE(ABORT,'synthetic_management_rollback'); END;`);
  const actualExec = f.store.db.exec.bind(f.store.db); let advanced = false;
  f.store.db.exec = sql => {
    const value = actualExec(sql);
    if (sql === 'BEGIN IMMEDIATE' && !advanced) { advanced = true; f.next(4000); }
    return value;
  };
  const failed = await f.call('/api/v1/auth/mfa/enable', { token, data: { setupToken: setup.body.setupToken, code: totpAt(secret, Math.floor(f.now() / 30000)) } });
  assert.equal(failed.status, 500); assert.equal(advanced, true);
  assert.equal(f.errors.length, 1); assert.equal(JSON.parse(f.errors[0]).event, 'request_error'); f.errors.length = 0;
  assert.ok(inside.length > 0 && inside.every(row => row.allowed), 'exact current/idle guard still ran inside the real transaction');
  assert.deepEqual(f.store.db.prepare('SELECT * FROM sessions ORDER BY token_hash').all(), sessionsBefore);
  assert.deepEqual(f.store.db.prepare('SELECT * FROM session_activity WHERE token_hash=?').get(hash), rowBefore, 'SQL rollback restores the previous durable row');
  assert.equal(f.store.db.prepare('SELECT count(*) n FROM account_mfa WHERE user_id=?').get(f.userId).n, 0);
  assert.equal(f.server.sessionAccess.describe(hash).idleDeadline, deadline, 'RAM deadline remains the real latest input');
  f.store.db.exec('DROP TRIGGER synthetic_mfa_abort_factor');
  assert.equal((await f.call('/api/v1/auth/me', { token })).status, 200);
  assert.equal(f.store.db.prepare('SELECT last_input_at FROM session_activity WHERE token_hash=?').get(hash).last_input_at, latestInput,
    'the successful outside guard persists the SAME accepted input after rollback');
  assert.equal(f.server.sessionAccess.describe(hash).idleDeadline, deadline, 'persistence never invents new input at flush time');
  f.next(Date.parse(deadline) - f.now() + 1);
  const expired = await f.call('/api/v1/auth/me', { token }); assert.equal(expired.status, 401); assert.equal(expired.body.error.code, 'session_idle_timeout');
});
