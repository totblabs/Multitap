import { test } from 'node:test';
import assert from 'node:assert/strict';
import { request } from 'node:http';
import { randomBytes } from 'node:crypto';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword, tokenHash, verifyPassword } from '../src/security.mjs';

const origin = 'http://password.fixture.invalid';
const password = 'Fixture-password-only-73!';
const replacement = 'Replacement-fixture-only-74!';
const resetPassword = 'Administrator-fixture-only-75!';
// Fixed fixture credentials only; no installed data or external network.
const initialHash = hashPassword(password), resetHash = hashPassword(resetPassword);

async function fixture(t, options = {}) {
  const store = new Store(':memory:'), errors = [];
  const hashed = await initialHash;
  const owner = store.addUser({ name: 'owner', role: 'admin', passwordHash: hashed });
  const alice = store.addUser({ name: 'alice', passwordHash: hashed });
  const bobby = store.addUser({ name: 'bobby', passwordHash: hashed });
  store.saveSetting('proxmox_initialized', '1', 'isolated-fixture');
  const server = await buildServer({ store, vault: new Vault(randomBytes(32)), publicOrigin: origin,
    allowInsecureHttp: true, logger: { error: value => errors.push(value) }, ...options });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { server.closeAllConnections(); await new Promise(resolve => server.close(resolve)); store.close(); });
  const base = 'http://127.0.0.1:' + server.address().port;
  async function call(path, { method = 'GET', data, headers = {} } = {}) {
    const account = /^\/admin\/users\/([^/]+)(?:\/password|\/enabled)?$/.exec(path);
    if (account && ['PUT', 'DELETE'].includes(method)) data = { ...data, expectedAccountId: store.findUser(decodeURIComponent(account[1])).id };
    const response = await fetch(base + '/api/v1' + path, { method,
      headers: { Origin: origin, ...headers, ...(data === undefined ? {} : { 'Content-Type': 'application/json' }) },
      body: data === undefined ? undefined : JSON.stringify(data) });
    return { status: response.status, body: await response.json(), headers: response.headers };
  }
  const login = await call('/auth/login', { method: 'POST', data: { username: 'alice', password } });
  assert.equal(login.status, 200);
  const token = login.body.sessionToken, hash = tokenHash(token), auth = { Authorization: 'Bearer ' + token };
  const input = { currentPassword: password, newPassword: replacement, passwordConfirmation: replacement };
  const change = (data = input) => call('/auth/password', { method: 'POST', headers: auth, data });
  async function admin() {
    const result = await call('/admin/login', { method: 'POST', data: { username: 'owner', password } });
    assert.equal(result.status, 200);
    return { Cookie: result.headers.get('set-cookie').split(';')[0], 'X-CSRF-Token': result.body.csrfToken };
  }
  async function pendingBody() {
    const payload = JSON.stringify(input);
    let authenticatedResolve;
    const authenticated = new Promise(resolve => { authenticatedResolve = resolve; });
    const authenticate = store.authenticate;
    // Rendezvous at actual bearer admission, not merely the request event:
    // preceding asynchronous routing can still be pending at that event.
    store.authenticate = function (...args) {
      const current = authenticate.apply(this, args);
      if (args[0] === hash) { store.authenticate = authenticate; authenticatedResolve(); }
      return current;
    };
    let writer;
    const response = new Promise((resolve, reject) => {
      writer = request(base + '/api/v1/auth/password', { method: 'POST', headers: { Origin: origin, ...auth,
        'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(payload) } }, res => {
        const chunks = []; res.on('data', chunk => chunks.push(chunk));
        res.on('end', () => { try { resolve({ status: res.statusCode, body: JSON.parse(Buffer.concat(chunks).toString()) }); } catch (error) { reject(error); } });
      });
      writer.on('error', reject); writer.setTimeout(5000, () => writer.destroy(Error('fixture timeout')));
      writer.write(payload.slice(0, 1));
    });
    void response.catch(() => {}); // cleanup may abort it after a preceding assertion fails
    // Ensure cleanup also ends a request if the fixture's intervening assertion fails.
    t.after(() => writer.destroy());
    await authenticated;
    return () => { writer.end(payload.slice(1)); return response; };
  }
  const row = id => store.db.prepare('SELECT * FROM users WHERE id=?').get(id);
  const auditCount = () => store.db.prepare("SELECT count(*) AS n FROM audit_events WHERE event='password_changed'").get().n;
  return { store, errors, owner, alice, bobby, call, hash, auth, input, change, admin, pendingBody, row, auditCount };
}

function refused(result) {
  assert.ok([401, 409].includes(result.status), 'revoked operation must return authentication/conflict refusal');
  assert.ok(['session_expired', 'credentials_changed'].includes(result.body.error.code));
}

for (const mutation of ['logout', 'delete', 'disable', 'expiry', 'reset', 'setup', 'username-reuse', 'foreign-token-owner']) {
  test('Actual HTTP incomplete password body is retired by ' + mutation, { timeout: 10000 }, async t => {
    const f = await fixture(t), before = f.row(f.alice).password_hash, auditBefore = f.auditCount();
    const admin = ['delete', 'disable', 'reset'].includes(mutation) ? await f.admin() : null;
    const finish = await f.pendingBody();
    if (mutation === 'logout') assert.equal((await f.call('/auth/logout', { method: 'POST', data: {}, headers: f.auth })).status, 200);
    if (mutation === 'delete') assert.equal((await f.call('/admin/users/alice', { method: 'DELETE', headers: admin })).status, 200);
    if (mutation === 'disable') assert.equal((await f.call('/admin/users/alice/enabled', { method: 'PUT', data: { enabled: false }, headers: admin })).status, 200);
    if (mutation === 'reset') assert.equal((await f.call('/admin/users/alice/password', { method: 'PUT', data: { password: resetPassword, passwordConfirmation: resetPassword }, headers: admin })).status, 200);
    if (mutation === 'expiry') f.store.db.prepare('UPDATE sessions SET expires_at=? WHERE token_hash=?').run(Date.now() - 1, f.hash);
    if (mutation === 'setup') f.store.db.prepare('UPDATE users SET must_change_credentials=1 WHERE id=?').run(f.alice);
    let recreated;
    if (mutation === 'username-reuse') {
      f.store.deleteUser('alice', f.owner);
      recreated = f.store.addUser({ name: 'alice', passwordHash: before });
      assert.notEqual(recreated, f.alice);
    }
    if (mutation === 'foreign-token-owner') f.store.db.prepare('UPDATE sessions SET user_id=? WHERE token_hash=?').run(f.bobby, f.hash);
    const expectedHash = f.row(f.alice)?.password_hash, auditAfterMutation = f.auditCount();
    refused(await finish());
    assert.equal(f.row(f.alice)?.password_hash, expectedHash, 'no stale password write');
    assert.equal(f.row(f.bobby).password_hash, before, 'no replacement account adoption');
    if (recreated) assert.equal(f.row(recreated).password_hash, before, 'same username has a distinct immutable identity');
    assert.equal(f.auditCount(), auditAfterMutation, 'no password-change audit for a rejected mutation');
    if (mutation !== 'reset') assert.equal(auditAfterMutation, auditBefore);
    assert.equal(f.errors.length, 0, 'normal revocation is not an internal error');
  });
}

for (const mutation of ['logout', 'setup', 'foreign-token-owner']) {
  test('Actual asynchronous password job revalidates ' + mutation + ' before reaching Store commit', { timeout: 10000 }, async t => {
    const f = await fixture(t), before = f.row(f.alice).password_hash;
    const authenticate = f.store.authenticate.bind(f.store), commit = f.store.changeOwnPassword.bind(f.store);
    let reads = 0, didMutate = false, commits = 0;
    f.store.authenticate = (...args) => {
      const current = authenticate(...args);
      if (args[0] === f.hash && ++reads === 2) queueMicrotask(() => {
        didMutate = true;
        if (mutation === 'logout') f.store.logout(f.hash);
        if (mutation === 'setup') f.store.db.prepare('UPDATE users SET must_change_credentials=1 WHERE id=?').run(f.alice);
        if (mutation === 'foreign-token-owner') f.store.db.prepare('UPDATE sessions SET user_id=? WHERE token_hash=?').run(f.bobby, f.hash);
      });
      return current;
    };
    f.store.changeOwnPassword = (...args) => { commits++; return commit(...args); };
    refused(await f.change());
    assert.equal(didMutate, true, 'fixture revokes during the real asynchronous hash/verify job');
    assert.equal(commits, 0, 'post-job authentication rejects before mutation boundary');
    assert.equal(f.row(f.alice).password_hash, before);
    assert.equal(f.row(f.bobby).password_hash, before);
    assert.equal(f.auditCount(), 0); assert.equal(f.errors.length, 0);
  });
}

for (const mutation of ['logout', 'expiry', 'disable', 'delete', 'reset', 'setup', 'foreign-token-owner', 'wrong-purpose', 'password-hash-cas']) {
  test('Store transaction refuses ' + mutation + ' after the HTTP final check', { timeout: 10000 }, async t => {
    const f = await fixture(t), before = f.row(f.alice).password_hash;
    const commit = f.store.changeOwnPassword.bind(f.store);
    let reached = 0, expectedHash = before;
    f.store.changeOwnPassword = (...args) => {
      reached++;
      if (mutation === 'logout') f.store.logout(f.hash);
      if (mutation === 'expiry') f.store.db.prepare('UPDATE sessions SET expires_at=? WHERE token_hash=?').run(Date.now() - 1, f.hash);
      if (mutation === 'disable') f.store.setUserEnabled('alice', false, f.owner);
      if (mutation === 'delete') f.store.deleteUser('alice', f.owner);
      if (mutation === 'reset') f.store.changePassword('alice', expectedHash = 'fixture-concurrent-reset', f.owner);
      if (mutation === 'setup') f.store.db.prepare('UPDATE users SET must_change_credentials=1 WHERE id=?').run(f.alice);
      if (mutation === 'foreign-token-owner') f.store.db.prepare('UPDATE sessions SET user_id=? WHERE token_hash=?').run(f.bobby, f.hash);
      if (mutation === 'wrong-purpose') f.store.db.prepare("UPDATE sessions SET purpose='admin' WHERE token_hash=?").run(f.hash);
      if (mutation === 'password-hash-cas') f.store.db.prepare('UPDATE users SET password_hash=? WHERE id=?').run(expectedHash = 'fixture-concurrent-hash', f.alice);
      return commit(...args);
    };
    refused(await f.change()); assert.equal(reached, 1, 'real HTTP verify/hash reaches exact Store boundary');
    assert.equal(f.row(f.alice)?.password_hash, mutation === 'delete' ? undefined : expectedHash);
    assert.equal(f.row(f.bobby).password_hash, before);
    assert.equal(f.auditCount(), mutation === 'reset' ? 1 : 0); assert.equal(f.errors.length, 0);
  });
}

test('A renamed original account remains the owner even if its former username is reused', { timeout: 10000 }, async t => {
  const f = await fixture(t), before = f.row(f.alice).password_hash;
  const finish = await f.pendingBody();
  f.store.db.prepare('UPDATE users SET username=? WHERE id=?').run('renamed-alice', f.alice);
  const replacementAccount = f.store.addUser({ name: 'alice', passwordHash: await resetHash });
  assert.equal((await finish()).status, 200);
  assert.equal(await verifyPassword(replacement, f.row(f.alice).password_hash), true);
  assert.equal(f.row(f.alice).username, 'renamed-alice');
  assert.equal(await verifyPassword(resetPassword, f.row(replacementAccount).password_hash), true);
  assert.notEqual(f.row(f.alice).password_hash, before);
  assert.equal(f.errors.length, 0);
});

test('Normal authorized change retains ID, retires client sessions, preserves another administrator and verifies the replacement password', { timeout: 10000 }, async t => {
  const f = await fixture(t);
  const second = await f.call('/auth/login', { method: 'POST', data: { username: 'alice', password } });
  const admin = await f.admin();
  const result = await f.change();
  assert.equal(result.status, 200); assert.equal(result.body.requiresLogin, true);
  assert.equal(f.row(f.alice).id, f.alice); assert.equal(f.auditCount(), 1);
  for (const headers of [f.auth, { Authorization: 'Bearer ' + second.body.sessionToken }])
    assert.equal((await f.call('/auth/me', { headers })).status, 401);
  assert.equal((await f.call('/admin/session', { headers: admin })).status, 200, 'other account session is preserved');
  assert.equal((await f.call('/auth/login', { method: 'POST', data: { username: 'alice', password } })).status, 401);
  assert.equal((await f.call('/auth/login', { method: 'POST', data: { username: 'alice', password: replacement } })).status, 200);
  assert.equal(f.errors.length, 0);
});

test('Current password/confirmation/input validation and per-account throttling keep existing refusals', { timeout: 10000 }, async t => {
  const f = await fixture(t, { loginLimit: 2 }), before = f.row(f.alice).password_hash;
  const mismatch = await f.change({ ...f.input, passwordConfirmation: 'different' });
  assert.equal(mismatch.status, 400); assert.equal(mismatch.body.error.code, 'password_confirmation_mismatch');
  for (const input of [{ ...f.input, currentPassword: '' }, { ...f.input, newPassword: 'short', passwordConfirmation: 'short' }])
    assert.equal((await f.change(input)).status, 400);
  for (let attempt = 0; attempt < 2; attempt++) {
    const result = await f.change({ ...f.input, currentPassword: 'incorrect-fixture' });
    assert.equal(result.status, 403); assert.equal(result.body.error.code, 'current_password_incorrect');
  }
  const blocked = await f.change(); assert.equal(blocked.status, 429); assert.equal(blocked.body.error.code, 'too_many_attempts');
  assert.equal(f.row(f.alice).password_hash, before); assert.equal(f.auditCount(), 0); assert.equal(f.errors.length, 0);
});

test('Trusted three-argument Store password changes retain hash CAS behavior', async t => {
  const store = new Store(':memory:'); t.after(() => store.close());
  const id = store.addUser({ name: 'alice', passwordHash: 'fixture-initial' });
  assert.throws(() => store.changeOwnPassword(id, 'stale-fixture', 'must-not-write'), /credentials_changed/);
  assert.equal(store.findUser('alice').password_hash, 'fixture-initial');
  store.changeOwnPassword(id, 'fixture-initial', 'fixture-replacement');
  assert.equal(store.findUser('alice').password_hash, 'fixture-replacement');
});
