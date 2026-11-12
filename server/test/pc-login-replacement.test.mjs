import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { request } from 'node:http';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword, tokenHash } from '../src/security.mjs';

const password = 'Only-isolated-PC-login-fixture-41!';
const hashed = hashPassword(password);
const policy = { 'X-Multitap-Session-Policy': 'windows-pc-v1', 'X-Multitap-Login-Intent': 'confirmable-v1' };
const recovery = { ...policy, 'X-Multitap-Login-Intent': 'recovery-v1' };
async function fixture(t) {
  const store = new Store(':memory:'), errors = [];
  for (const name of ['owner', 'alice', 'bobby']) store.addUser({ name, role: name === 'owner' ? 'admin' : 'user', passwordHash: await hashed });
  store.saveSetting('proxmox_initialized', '1', 'fixture');
  const server = await buildServer({ store, vault: new Vault(randomBytes(32)), publicOrigin: 'http://pc.fixture.invalid', allowInsecureHttp: true,
    loginLimit: 40, addressLimit: 80, logger: { error: error => errors.push(error) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { server.closeAllConnections(); await new Promise(resolve => server.close(resolve)); store.close(); });
  const base = 'http://127.0.0.1:' + server.address().port;
  async function call(path, { method = 'GET', data, headers = {}, token } = {}) {
    const response = await fetch(base + '/api/v1' + path, { method,
      headers: { Origin: 'http://pc.fixture.invalid', ...headers, ...(token ? { Authorization: 'Bearer ' + token } : {}), ...(data ? { 'Content-Type': 'application/json' } : {}) },
      body: data ? JSON.stringify(data) : undefined });
    return { status: response.status, body: await response.json(), policy: response.headers.get('x-multitap-session-policy') };
  }
  const login = (headers = policy, username = 'alice', secret = password) => call('/auth/login', { method: 'POST', headers, data: { username, password: secret } });
  const confirm = offer => {
    assert.equal(offer.status, 409); assert.equal(offer.body.error.code, 'login_confirmation_required');
    return call('/auth/login/confirm', { method: 'POST', headers: policy, data: { challengeToken: offer.body.error.details.challengeToken, confirm: true } });
  };
  // Call sites explicitly choose confirmation. Raw login remains available to
  // assert that conflict/automatic admission cannot silently evict anyone.
  const confirmedLogin = async (...args) => { const offer = await login(...args); return offer.status === 200 ? offer : confirm(offer); };
  const me = token => call('/auth/me', { token });
  return { store, server, errors, call, login, confirmedLogin, confirm, me, base };
}

test('Explicit A to B to A confirmed PC login replaces the previous client authority only after yes', async t => {
  const f = await fixture(t), a = await f.login(), offer = await f.login();
  assert.equal(offer.status, 409); assert.equal(offer.body.error.code, 'login_confirmation_required');
  assert.equal((await f.me(a.body.sessionToken)).status, 200, 'unconfirmed conflict preserves the old device');
  const b = await f.confirm(offer);
  assert.equal(a.status, 200); assert.equal(b.status, 200);
  assert.equal(b.policy, 'windows-pc-v1');
  const retired = await f.me(a.body.sessionToken);
  assert.equal(retired.status, 401); assert.equal(retired.body.error.code, 'pc_login_replaced');
  assert.equal((await f.me(b.body.sessionToken)).status, 200);
  const again = await f.confirmedLogin(); assert.equal(again.status, 200);
  assert.equal((await f.me(b.body.sessionToken)).body.error.code, 'pc_login_replaced');
  assert.equal((await f.me(again.body.sessionToken)).status, 200);
  assert.equal(f.errors.length, 0);
});

test('Replacement while a Windows password body is pending returns its fixed terminal reason and never changes the hash', async t => {
  const f = await fixture(t), a = await f.login(), hash = tokenHash(a.body.sessionToken), before = f.store.findUser('alice').password_hash;
  const payload = JSON.stringify({ currentPassword: password, newPassword: 'Replacement-fixture-password-49!', passwordConfirmation: 'Replacement-fixture-password-49!' });
  let admittedResolve; const admitted = new Promise(resolve => { admittedResolve = resolve; });
  const authenticate = f.store.authenticate;
  f.store.authenticate = function (...args) {
    const current = authenticate.apply(this, args);
    if (args[0] === hash) { f.store.authenticate = authenticate; admittedResolve(); }
    return current;
  };
  let writer;
  const response = new Promise((resolve, reject) => {
    writer = request(f.base + '/api/v1/auth/password', { method: 'POST', headers: { Origin: 'http://pc.fixture.invalid',
      Authorization: 'Bearer ' + a.body.sessionToken, 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(payload) } }, res => {
      const chunks = []; res.on('data', bytes => chunks.push(bytes)); res.on('end', () => resolve({ status: res.statusCode, body: JSON.parse(Buffer.concat(chunks).toString()) }));
    });
    writer.on('error', reject); writer.setTimeout(5000, () => writer.destroy(Error('fixture timeout'))); writer.write('{');
  });
  void response.catch(() => {}); t.after(() => writer.destroy());
  await admitted; const b = await f.confirmedLogin(); writer.end(payload.slice(1)); const result = await response;
  assert.equal(result.status, 401); assert.equal(result.body.error.code, 'pc_login_replaced');
  assert.equal(f.store.findUser('alice').password_hash, before);
  assert.equal((await f.me(b.body.sessionToken)).status, 200); assert.equal(f.errors.length, 0);
});

test('Replacement after password hashing at the Store boundary maps the same terminal reason without weakening CAS', async t => {
  const f = await fixture(t), a = await f.login(), before = f.store.findUser('alice'), commit = f.store.changeOwnPassword.bind(f.store);
  const winner = tokenHash('isolated-password-commit-winner');
  f.store.changeOwnPassword = (...args) => {
    // A separately confirmed server-owned winner commits through the SAME
    // SessionAccess/real SQL adapter, not the retired createSession login hook.
    const offer = f.server.sessionAccess.begin(before);
    assert.equal(offer.kind, 'confirmation-required');
    assert.equal(f.server.sessionAccess.confirm(offer.challenge, { tokenHash: winner, ttlMs: 60000, issuer: 'windows-pc-v1' }).created, true);
    return commit(...args);
  };
  const result = await f.call('/auth/password', { method: 'POST', token: a.body.sessionToken,
    data: { currentPassword: password, newPassword: 'Replacement-fixture-password-49!', passwordConfirmation: 'Replacement-fixture-password-49!' } });
  assert.equal(result.status, 401); assert.equal(result.body.error.code, 'pc_login_replaced');
  assert.equal(f.store.findUser('alice').password_hash, before.password_hash);
  assert.equal(f.store.authenticate(winner).id, before.id); assert.equal(f.errors.length, 0);
});

test('Automatic recovery cannot replace an owner but may acquire an empty Windows slot', async t => {
  const f = await fixture(t), automatic = await f.login(recovery);
  assert.equal(automatic.status, 200); assert.equal(automatic.policy, 'windows-pc-v1');
  const occupied = await f.login(recovery);
  assert.equal(occupied.status, 409); assert.equal(occupied.body.error.code, 'pc_login_requires_interaction');
  assert.equal((await f.me(automatic.body.sessionToken)).status, 200);
  const explicit = await f.confirmedLogin(); assert.equal(explicit.status, 200);
  const refused = await f.login(recovery);
  assert.equal(refused.status, 409); assert.equal(refused.body.error.code, 'session_reauthentication_required');
  assert.equal((await f.me(explicit.body.sessionToken)).status, 200);
  assert.equal((await f.call('/auth/renew', { method: 'POST', token: automatic.body.sessionToken })).body.error.code, 'pc_login_replaced');
  assert.equal(f.errors.length, 0);
});

test('An automatic HTTP login already past Module.begin cannot retire a newly committed owner at BEFORE BEGIN', async t => {
  const f = await fixture(t), transaction = f.store.transaction.bind(f.store), winnerHash = tokenHash('isolated-concurrent-explicit-winner');
  let reached = 0, injecting = false;
  f.store.transaction = action => {
    if (!injecting && reached === 0) {
      injecting = true; reached++;
      try {
        const admission = f.server.sessionAccess.begin(f.store.findUser('alice'));
        assert.equal(admission.kind, 'ready');
        assert.equal(f.server.sessionAccess.commitDirect(admission.admission, { tokenHash: winnerHash, ttlMs: 60000, issuer: 'windows-pc-v1' }).created, true);
      } finally { injecting = false; }
    }
    return transaction(action);
  };
  const result = await f.login(recovery);
  assert.equal(result.status, 409); assert.equal(result.body.error.code, 'login_confirmation_changed');
  assert.equal(reached, 1, 'actual HTTP password and Module.begin completed before exact Store transaction admission');
  assert.equal(f.store.authenticate(winnerHash).id, f.store.findUser('alice').id);
  assert.equal(f.store.sessionRevocationReason(winnerHash), null); assert.equal(f.errors.length, 0);
});

test('Wrong password and failed replacement insert leave the exact previous owner valid', async t => {
  const f = await fixture(t), previous = await f.login();
  assert.equal((await f.login(policy, 'alice', 'wrong-fixture')).status, 401);
  assert.equal((await f.me(previous.body.sessionToken)).status, 200);
  f.store.db.exec("CREATE TRIGGER fixture_no_insert BEFORE INSERT ON sessions BEGIN SELECT RAISE(ABORT,'fixture_insert_failure'); END");
  assert.equal((await f.confirmedLogin()).status, 500);
  assert.equal((await f.me(previous.body.sessionToken)).status, 200, 'atomic rollback restores prior authority');
  assert.equal(f.errors.length, 1, 'injected storage fault is observed, not misclassified as normal auth');
});

test('Parallel first logins issue one session and one conflict; explicit yes replaces and late old response cannot renew', async t => {
  const f = await fixture(t), results = await Promise.all([f.login(), f.login()]);
  assert.deepEqual(results.map(value => value.status).sort(), [200, 409]);
  const first = results.find(value => value.status === 200), pending = results.find(value => value.status === 409);
  assert.equal((await f.me(first.body.sessionToken)).status, 200);
  const replacement = await f.confirm(pending); assert.equal(replacement.status, 200);
  assert.equal((await f.me(first.body.sessionToken)).body.error.code, 'pc_login_replaced');
  assert.equal((await f.call('/auth/renew', { method: 'POST', token: first.body.sessionToken })).status, 401);
  assert.equal(f.store.authenticate(tokenHash(first.body.sessionToken)), undefined);
  assert.equal((await f.me(replacement.body.sessionToken)).status, 200);
});

test('Legacy client participates in confirmation while another account and administrator purpose survive', async t => {
  const f = await fixture(t), legacy = await f.login({}), other = await f.login(policy, 'bobby');
  const admin = await f.call('/admin/login', { method: 'POST', headers: policy, data: { username: 'owner', password } });
  assert.equal(admin.status, 200); assert.equal(admin.policy, null, 'headers cannot classify admin login as Windows');
  const old = await f.confirmedLogin(), current = await f.confirmedLogin();
  assert.equal((await f.me(old.body.sessionToken)).status, 401);
  assert.equal((await f.me(legacy.body.sessionToken)).body.error.code, 'pc_login_replaced');
  for (const session of [other, current]) assert.equal((await f.me(session.body.sessionToken)).status, 200);
  assert.equal(f.store.db.prepare("SELECT count(*) AS n FROM sessions WHERE user_id=? AND purpose='admin'").get(f.store.findUser('owner').id).n, 1);
});

test('New client admission refuses to evict five administrator-purpose authorities', async t => {
  const f = await fixture(t), user = f.store.findUser('owner'), old = [];
  for (let index = 0; index < 5; index++) {
    const hash = tokenHash('isolated-admin-cap-' + index); old.push(hash);
    assert.equal(f.store.createSession(user, hash, Date.now(), 60000, 'admin', { issuer: 'admin' }), true);
  }
  const result = await f.login(policy, 'owner'); assert.equal(result.status, 409); assert.equal(result.body.error.code, 'active_session_limit');
  for (const hash of old) assert.equal(f.store.authenticate(hash, Date.now(), 'admin').id, user.id);
});

test('Policy is additive to strict username/password body and malformed policy intent is refused', async t => {
  const f = await fixture(t);
  assert.equal((await f.login({})).policy, null);
  for (const headers of [{ 'X-Multitap-Session-Policy': 'windows-pc-v1' }, { ...policy, 'X-Multitap-Login-Intent': 'invalid' }, { ...policy, 'X-Multitap-Session-Policy': 'unknown' }])
    assert.equal((await f.login(headers)).status, 400);
  assert.equal((await f.call('/auth/login', { method: 'POST', headers: policy, data: { username: 'alice', password, platform: 'windows' } })).status, 400);
});
