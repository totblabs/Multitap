import { test } from 'node:test';
import assert from 'node:assert/strict';
import { DatabaseSync } from 'node:sqlite';
import { mkdtemp, copyFile, readFile, rm } from 'node:fs/promises';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { Store } from '../src/store.mjs';
import { Store as Store18 } from './fixtures/mfa-store18.mjs';
import { tokenHash } from '../src/security.mjs';
import { clientSessionPolicy, pcSessionPolicy, replacedSessionsPerUser, replacedSessionsTotal } from '../src/client-session-policy.mjs';

const pc = { issuer: pcSessionPolicy, loginIntent: 'interactive-v1' };
function fixture(t) {
  const store = new Store(':memory:'); t.after(() => store.close());
  store.addUser({ name: 'alice', passwordHash: 'fixture-hash' });
  const user = store.findUser('alice'), now = Date.now();
  const issue = (name, options = pc, purpose = 'client', at = now) => {
    const hash = tokenHash(name); assert.equal(store.createSession(user, hash, at, 60000, purpose, options), true); return hash;
  };
  return { store, user, now, issue };
}

test('Exact additive policy/intent headers reject malformed/duplicate fields without widening body grammar', () => {
  assert.deepEqual(clientSessionPolicy({ headers: {} }), { issuer: 'legacy' });
  for (const loginIntent of ['interactive-v1', 'recovery-v1'])
    assert.deepEqual(clientSessionPolicy({ headers: { 'x-multitap-session-policy': pcSessionPolicy, 'x-multitap-login-intent': loginIntent } }), { issuer: pcSessionPolicy, loginIntent });
  for (const headers of [
    { 'x-multitap-session-policy': pcSessionPolicy }, { 'x-multitap-login-intent': 'interactive-v1' },
    { 'x-multitap-session-policy': 'WINDOWS-PC-V1', 'x-multitap-login-intent': 'interactive-v1' },
    { 'x-multitap-session-policy': [pcSessionPolicy], 'x-multitap-login-intent': 'interactive-v1' },
    { 'x-multitap-session-policy': pcSessionPolicy, 'x-multitap-login-intent': 'interactive-v1, recovery-v1' }
  ]) assert.throws(() => clientSessionPolicy({ headers }), error => error.code === 'invalid_session_policy' && error.status === 400);
  for (const name of ['X-Multitap-Session-Policy', 'X-Multitap-Login-Intent'])
    assert.throws(() => clientSessionPolicy({ headers: { 'x-multitap-session-policy': pcSessionPolicy, 'x-multitap-login-intent': 'interactive-v1' }, rawHeaders: [name, 'one', name.toLowerCase(), 'two'] }), /invalid_session_policy/);
});

test('Atomic Windows replacement overrides only its own live protection and keeps original expiry', t => {
  const f = fixture(t), old = f.issue('old');
  const expires = f.store.db.prepare('SELECT expires_at FROM sessions WHERE token_hash=?').get(old).expires_at;
  const current = f.issue('current', { ...pc, protectedHashes: new Set([old]) }, 'client', f.now + 1);
  assert.equal(f.store.authenticate(old, f.now + 2), undefined);
  assert.equal(f.store.sessionRevocationReason(old, f.now + 2, f.user.id), 'pc_login_replaced');
  assert.equal(f.store.sessionRevocationReason(old, f.now + 2, 'foreign-user'), null);
  assert.equal(f.store.sessionRevocationReason(tokenHash('unknown'), f.now + 2), null);
  assert.equal(f.store.authenticate(current, f.now + 2).id, f.user.id);
  assert.equal(f.store.renewSession(old, f.now + 2, 60000, 120000), null);
  assert.equal(f.store.db.prepare('SELECT expires_at FROM sessions WHERE token_hash=?').get(old).expires_at, expires);
  assert.equal(f.store.sessionRevocationReason(old, expires), null);
  assert.throws(() => f.store.db.prepare('UPDATE sessions SET replaced_at=NULL WHERE token_hash=?').run(old), /UNIQUE/);
});

test('Delayed recovery and failed transaction cannot replace the committed explicit owner', t => {
  const f = fixture(t), owner = f.issue('owner');
  assert.throws(() => f.issue('automatic', { ...pc, loginIntent: 'recovery-v1' }), /pc_login_requires_interaction/);
  assert.equal(f.store.authenticate(owner).id, f.user.id);
  assert.throws(() => f.issue('owner'), /UNIQUE/, 'duplicate insert rolls back previous replacement');
  assert.equal(f.store.authenticate(owner).id, f.user.id);
  assert.equal(f.store.sessionRevocationReason(owner), null);
});

test('Stale credential, setup-required, disabled and wrong issuer/purpose cannot mutate the current owner', t => {
  const f = fixture(t), owner = f.issue('owner');
  assert.equal(f.store.createSession({ ...f.user, password_hash: 'stale' }, tokenHash('stale'), f.now, 60000, 'client', pc), false);
  assert.throws(() => f.issue('wrong-purpose', pc, 'admin'), /invalid_session_policy/);
  assert.throws(() => f.issue('wrong-intent', { issuer: 'browser-console', loginIntent: 'interactive-v1' }), /invalid_session_policy/);
  f.store.db.prepare('UPDATE users SET must_change_credentials=1 WHERE id=?').run(f.user.id);
  assert.equal(f.store.createSession(f.user, tokenHash('setup'), f.now, 60000, 'client', pc), false);
  f.store.db.prepare('UPDATE users SET must_change_credentials=0,disabled=1 WHERE id=?').run(f.user.id);
  assert.equal(f.store.createSession(f.user, tokenHash('disabled'), f.now, 60000, 'client', pc), false);
  assert.equal(f.store.db.prepare('SELECT replaced_at FROM sessions WHERE token_hash=?').get(owner).replaced_at, null);
});

test('Only Windows is retired: legacy/Android-unknown/admin/independent console sessions stay valid', t => {
  const f = fixture(t);
  const others = [f.issue('legacy', { issuer: 'legacy' }), f.issue('android-unknown', {}),
    f.issue('console', { issuer: 'browser-console' }), f.issue('admin', { issuer: 'admin' }, 'admin')];
  const old = f.issue('old'), current = f.issue('current');
  for (let index = 0; index < others.length; index++) assert.equal(f.store.authenticate(others[index], f.now, index === 3 ? 'admin' : 'client').id, f.user.id);
  assert.equal(f.store.authenticate(old), undefined); assert.equal(f.store.authenticate(current).id, f.user.id);
  assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM sessions WHERE replaced_at IS NULL').get().n, 5);
});

test('Recovery can acquire a truly expired owner slot, but never renew an old replaced authority', t => {
  const f = fixture(t), old = f.issue('old');
  const current = f.issue('recovery', { ...pc, loginIntent: 'recovery-v1' }, 'client', f.now + 60001);
  assert.equal(f.store.authenticate(old, f.now + 60001), undefined);
  assert.equal(f.store.authenticate(current, f.now + 60001).id, f.user.id);
  assert.equal(f.store.sessionRevocationReason(old, f.now + 60001), null);
});

test('Reason retention is bounded per account and total without reviving pruned credentials', t => {
  const f = fixture(t), oldest = f.issue('oldest');
  for (let index = 0; index < replacedSessionsPerUser + 3; index++) f.issue('replacement-' + index, pc, 'client', f.now + index + 1);
  assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM sessions WHERE replaced_at IS NOT NULL').get().n, replacedSessionsPerUser);
  assert.equal(f.store.sessionRevocationReason(oldest, f.now), null);
  assert.equal(f.store.authenticate(oldest, f.now), undefined);
  const insert = f.store.db.prepare("INSERT INTO sessions(token_hash,user_id,created_at,expires_at,purpose,issuer,replaced_at) VALUES(?,?,?,?,'client','windows-pc-v1',?)");
  for (let index = 0; index < replacedSessionsTotal; index++) insert.run(tokenHash('synthetic-retained-' + index), f.user.id, f.now, f.now + 60000, f.now);
  assert.ok(f.store.pruneReplacedSessions(f.now) > 0);
  assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM sessions WHERE replaced_at IS NOT NULL').get().n, replacedSessionsTotal);
  assert.ok(f.store.pruneReplacedSessions(f.now + 120000) > 0);
});

test('Actual schema16 migration and reopen preserve old authorities; rollback copy stays schema16', async t => {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-pc-policy-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const path = join(directory, 'fixture.sqlite'), backup = join(directory, 'before.sqlite'), restored = join(directory, 'rollback.sqlite');
  let store = new Store18(path);
  store.addUser({ name: 'legacy', passwordHash: 'fixture-hash' }); const user = store.findUser('legacy'), hash = tokenHash('legacy-session'), now = Date.now();
  store.createSession(user, hash, now, 60000); store.close();
  // Reconstruct the exact pre-policy sessions table by removing the additive schema17/18 fields and tables.
  const old = new DatabaseSync(path);
  old.exec('DROP TABLE session_end_reasons;DROP TABLE session_activity;DROP INDEX sessions_windows_owner; DROP INDEX sessions_replaced; ALTER TABLE sessions DROP COLUMN replaced_at; ALTER TABLE sessions DROP COLUMN issuer; PRAGMA user_version=16;'); old.close();
  await copyFile(path, backup);
  const beforeBytes = await readFile(backup);
  store = new Store(path);
  assert.equal(store.db.prepare('PRAGMA user_version').get().user_version, 19);
  assert.equal(store.authenticate(hash).id, user.id);
  assert.equal(store.db.prepare('SELECT issuer,replaced_at FROM sessions').get().issuer, 'legacy');
  store.close(); store = new Store(path); assert.equal(store.authenticate(hash).id, user.id); store.close();
  const original = new DatabaseSync(backup);
  assert.equal(original.prepare('PRAGMA user_version').get().user_version, 16);
  assert.equal(original.prepare('SELECT count(*) AS n FROM sessions').get().n, 1);
  assert.equal(original.prepare("SELECT count(*) AS n FROM pragma_table_info('sessions') WHERE name='issuer'").get().n, 0); original.close();
  await copyFile(backup, restored);
  assert.deepEqual(await readFile(restored), beforeBytes, 'actual rollback copy restores exact pre-migration bytes');
  const rollback = new DatabaseSync(restored);
  assert.equal(rollback.prepare('PRAGMA user_version').get().user_version, 16);
  assert.equal(rollback.prepare('SELECT user_id FROM sessions WHERE token_hash=?').get(hash).user_id, user.id);
  assert.equal(rollback.prepare('SELECT count(*) AS n FROM users').get().n, 1);
  rollback.close();
  assert.deepEqual(await readFile(backup), beforeBytes, 'migration and rollback never alter the original backup');
});
