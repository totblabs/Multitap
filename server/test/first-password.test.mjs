// 첫 로그인 비밀번호 (user decision 2026-11-16): an account whose password an
// administrator set (made in the admin console, or reset there by another
// administrator) picks its own password before it can sign in anywhere.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';

const origin = 'https://first-password-tests.example';
const password = 'First-password-tests-71!';

async function setup(t, options = {}) {
  const store = new Store(':memory:'); const vault = new Vault(randomBytes(32));
  const hash = await hashPassword(password);
  store.addUser({ name: 'admin', passwordHash: hash, role: 'admin' });
  store.addUser({ name: 'second', passwordHash: hash, role: 'admin' });
  store.addUser({ name: 'alice', passwordHash: hash });
  store.saveSetting('proxmox_initialized', '1', 'protocol-test-fixture');
  const server = await buildServer({ store, vault, publicOrigin: origin, logger: { error: value => assert.fail(value) }, ...options });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  let cookie = '', csrf = '';
  async function call(path, method = 'GET', data, headers = {}) {
    const response = await fetch(`http://127.0.0.1:${server.address().port}` + path, { method, headers: { Cookie: cookie, ...(data !== undefined ? { 'Content-Type': 'application/json' } : {}), ...(method !== 'GET' ? { Origin: origin, 'X-CSRF-Token': csrf } : {}), ...headers }, body: data === undefined ? undefined : JSON.stringify(data) });
    return { status: response.status, body: await response.json() };
  }
  async function adminLogin(name = 'admin', secret = password) {
    const result = await fetch(`http://127.0.0.1:${server.address().port}/api/v1/admin/login`, { method: 'POST', headers: { 'Content-Type': 'application/json', Origin: origin }, body: JSON.stringify({ username: name, password: secret }) });
    const body = await result.json();
    if (result.status === 200) { cookie = result.headers.get('set-cookie').split(';')[0]; csrf = body.csrfToken; }
    return { status: result.status, body };
  }
  const initial = (username, current, next, confirmation = next) => call('/api/v1/auth/password/initial', 'POST', { username, password: current, newPassword: next, passwordConfirmation: confirmation });
  const clientLogin = (username, secret) => call('/api/v1/auth/login', 'POST', { username, password: secret });
  return { store, call, adminLogin, initial, clientLogin };
}

const created = 'Made-by-admin-71!', own = 'Picked-by-owner-71!';

test('an account made in the admin console signs in only after picking its own password', async t => {
  const f = await setup(t, { loginLimit: 40 });
  assert.equal((await f.adminLogin()).status, 200);
  assert.equal((await f.call('/api/v1/admin/users', 'POST', { username: '홍길동', password: created, passwordConfirmation: created, role: 'user' })).status, 201);
  const listed = (await f.call('/api/v1/admin/users')).body.users.find(u => u.username === '홍길동');
  assert.equal(listed.password_change_required, 1, 'the admin console sees it has not been changed yet');
  // Every sign-in path refuses it, with the same answer, and makes no session.
  assert.deepEqual([(await f.clientLogin('홍길동', created)).status, (await f.clientLogin('홍길동', created)).body.error.code], [428, 'password_change_required']);
  assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM sessions s JOIN users u ON u.id=s.user_id WHERE u.username=?').get('홍길동').n, 0);
  // A wrong password learns nothing about the state.
  assert.deepEqual([(await f.clientLogin('홍길동', 'Not-the-password-71!')).status], [401]);
  assert.equal((await f.initial('홍길동', 'Not-the-password-71!', own)).status, 401);
  assert.equal((await f.initial('nobody', created, own)).status, 401);
  // Input rules: confirmation, length, and a password different from the first one.
  assert.equal((await f.initial('홍길동', created, own, 'Something-else-71!')).body.error.code, 'password_confirmation_mismatch');
  assert.equal((await f.initial('홍길동', created, 'short', 'short')).body.error.code, 'password_length_10_to_256');
  assert.deepEqual([(await f.initial('홍길동', created, created)).status, (await f.initial('홍길동', created, created)).body.error.code], [400, 'password_unchanged']);
  assert.equal((await f.call('/api/v1/auth/password/initial', 'POST', { username: '홍길동', password: created, newPassword: own, passwordConfirmation: own, extra: 1 })).status, 400);
  assert.equal((await f.call('/api/v1/auth/password/initial', 'POST', { username: '홍길동', password: created, newPassword: own, passwordConfirmation: own }, { Origin: 'https://elsewhere.example' })).status, 403);
  // The change.
  assert.deepEqual(await f.initial('홍길동', created, own), { status: 200, body: { ok: true } });
  assert.equal((await f.clientLogin('홍길동', created)).status, 401, 'the first password is gone');
  const signedIn = await f.clientLogin('홍길동', own);
  assert.equal(signedIn.status, 200); assert.ok(signedIn.body.sessionToken);
  assert.equal(f.store.findUser('홍길동').password_change_required, 0);
  // Only once: the endpoint is not another way to change a password.
  const kept = f.store.findUser('홍길동').password_hash;
  assert.deepEqual([(await f.initial('홍길동', own, 'Another-one-71!!')).status, (await f.initial('홍길동', own, 'Another-one-71!!')).body.error.code], [409, 'password_change_not_required']);
  assert.equal(f.store.findUser('홍길동').password_hash, kept);
});

test('a reset by another administrator asks again; the account itself and the server commands do not', async t => {
  const f = await setup(t);
  assert.equal((await f.adminLogin()).status, 200);
  const alice = f.store.findUser('alice');
  const token = (await f.clientLogin('alice', password)).body.sessionToken;
  assert.equal((await f.call('/api/v1/admin/users/alice/password', 'PUT', { password: created, passwordConfirmation: created, expectedAccountId: alice.id })).status, 200);
  assert.equal((await f.call('/api/v1/auth/me', 'GET', undefined, { Authorization: 'Bearer ' + token })).status, 401, 'the reset ends the sign-ins as before');
  assert.equal((await f.clientLogin('alice', created)).body.error.code, 'password_change_required');
  // Changing one's own password in the app clears the state too.
  f.store.changeOwnPassword(alice.id, f.store.findUser('alice').password_hash, await hashPassword(own));
  assert.equal(f.store.findUser('alice').password_change_required, 0);
  assert.equal((await f.clientLogin('alice', own)).status, 200);
  // An administrator resetting its own password in the console is not asked again.
  const admin = f.store.findUser('admin');
  assert.equal((await f.call('/api/v1/admin/users/admin/password', 'PUT', { password: created, passwordConfirmation: created, expectedAccountId: admin.id })).status, 200);
  assert.equal(f.store.findUser('admin').password_change_required, 0);
  // The server's recovery command (actor server-admin) is not asked again either.
  f.store.changePassword('alice', await hashPassword(created), 'server-admin');
  assert.equal(f.store.findUser('alice').password_change_required, 0);
});

test('an administrator whose password another administrator set picks its own before the admin console opens', async t => {
  const f = await setup(t);
  assert.equal((await f.adminLogin('admin', password)).status, 200);
  const second = f.store.findUser('second');
  assert.equal((await f.call('/api/v1/admin/users/second/password', 'PUT', { password: created, passwordConfirmation: created, expectedAccountId: second.id })).status, 200);
  const refused = await f.adminLogin('second', created);
  assert.deepEqual([refused.status, refused.body.error.code], [428, 'password_change_required']);
  assert.equal((await f.initial('second', created, own)).status, 200);
  assert.equal((await f.adminLogin('second', own)).status, 200);
  // The first-server-setup state is a different thing and stays as it was.
  assert.equal(f.store.findUser('second').must_change_credentials, 0);
});

test('the new-password request spends the same per-account sign-in budget as a sign-in', async t => {
  const f = await setup(t);
  for (let n = 0; n < 8; n++) assert.equal((await f.initial('alice', 'Wrong-password-71!', own)).status, 401);
  assert.equal((await f.initial('alice', 'Wrong-password-71!', own)).status, 429);
  assert.equal((await f.clientLogin('alice', password)).status, 429, 'one budget for both');
});
