import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { adminRoutes } from '../src/admin-http.mjs';
import { tokenHash } from '../src/security.mjs';

const origin = 'https://account-target.fixture.invalid';
const oldHash = 'unused-original-fixture-hash';
const operations = {
  delete: { method: 'DELETE', suffix: '', input: {} },
  password: { method: 'PUT', suffix: '/password', input: { password: 'Fixture-only-password-93!', passwordConfirmation: 'Fixture-only-password-93!' } },
  profile: { method: 'PUT', suffix: '', input: { role: 'admin', notes: 'Intended for original account' } },
  enabled: { method: 'PUT', suffix: '/enabled', input: { enabled: false } },
};
function fixture(t, hooks = {}) {
  const store = new Store(':memory:'); t.after(() => store.close());
  const vault = new Vault(Buffer.alloc(32, 23)), token = 'Q'.repeat(43);
  const actor = store.addUser({ name: 'operator', role: 'admin', passwordHash: oldHash });
  const target = store.addUser({ name: 'target', passwordHash: oldHash });
  store.saveSetting('proxmox_initialized', '1', 'fixture');
  store.createSession(store.findUserById(actor), tokenHash(token), Date.now(), 60000, 'admin', { issuer: 'admin' });
  let payload, replacement;
  const replace = () => {
    store.deleteUser('target', actor);
    replacement = store.addUser({ name: 'target', passwordHash: oldHash });
    assert.notEqual(replacement, target);
    return replacement;
  };
  const routes = adminRoutes({ store, vault, publicOrigin: origin,
    body: async () => { await hooks.body?.(replace); return structuredClone(payload); },
    passwordJob: async job => { const result = await job(); await hooks.hash?.(replace); return result; },
    relay: { clipboardPolicyChanged() {}, devicePolicyChanged() {} },
  });
  async function invoke(operation, options = {}) {
    const spec = operations[operation];
    payload = { ...spec.input, expectedAccountId: target, ...options.input };
    if (options.omitIdentity) delete payload.expectedAccountId;
    let response;
    try {
      await routes({ method: spec.method, headers: { origin, 'content-type': 'application/json',
        cookie: '__Secure-multitap-admin=' + token, 'x-csrf-token': options.csrf === false ? '' : vault.csrf(token) } },
      { setHeader() {} }, '/api/v1/admin/users/' + (options.name ?? 'target') + spec.suffix,
      (status, body) => { response = { status, body }; });
    } catch (error) { response = { status: error.status, body: { error: { code: error.code } } }; }
    return response;
  }
  const unchangedReplacement = () => {
    const row = store.findUserById(replacement);
    assert.ok(row); assert.equal(row.password_hash, oldHash); assert.equal(row.role, 'user'); assert.equal(row.disabled, 0);
  };
  return { store, actor, target, replace, invoke, unchangedReplacement };
}

for (const kind of Object.keys(operations)) {
  test(`bound admin ${kind} keeps the stable-account positive control`, async t => {
    const f = fixture(t); assert.equal((await f.invoke(kind)).status, 200);
    const row = f.store.findUserById(f.target);
    if (kind === 'delete') assert.equal(row, undefined);
    if (kind === 'password') assert.notEqual(row.password_hash, oldHash);
    if (kind === 'profile') assert.equal(row.role, 'admin');
    if (kind === 'enabled') assert.equal(row.disabled, 1);
  });
  test(`bound admin ${kind} rejects the recreated same-name account`, async t => {
    const f = fixture(t); f.replace();
    const result = await f.invoke(kind);
    assert.equal(result.status, 409); assert.equal(result.body.error.code, 'account_identity_changed');
    f.unchangedReplacement();
  });
  test(`bound admin ${kind} refuses legacy identity-free and malformed requests`, async t => {
    const f = fixture(t);
    const missing = await f.invoke(kind, { omitIdentity: true });
    assert.equal(missing.status, 428); assert.equal(missing.body.error.code, 'account_identity_required');
    const invalid = await f.invoke(kind, { input: { expectedAccountId: ['not-an-id'] } });
    assert.equal(invalid.status, 400); assert.equal(invalid.body.error.code, 'invalid_account_identity');
    assert.equal((await f.invoke(kind, { csrf: false })).status, 403);
    assert.equal(f.store.findUserById(f.target).password_hash, oldHash);
  });
}
test('password hashing cannot rebind an expected account to its replacement', async t => {
  const f = fixture(t, { hash: replace => replace() });
  const result = await f.invoke('password');
  assert.equal(result.status, 409); assert.equal(result.body.error.code, 'account_identity_changed');
  f.unchangedReplacement();
});
for (const kind of ['delete', 'profile', 'enabled']) {
  test(`awaited ${kind} body cannot rebind an expected account to its replacement`, async t => {
    const f = fixture(t, { body: replace => replace() });
    assert.equal((await f.invoke(kind)).status, 409); f.unchangedReplacement();
  });
}
test('Store expected-account checks apply to the final transaction without mutation or audit', t => {
  const f = fixture(t); f.replace();
  let audits = 0; const audit = f.store.audit.bind(f.store);
  f.store.audit = (...args) => { ++audits; return audit(...args); };
  for (const mutate of [
    () => f.store.deleteUser('target', f.actor, f.target),
    () => f.store.changePassword('target', 'unused-new-hash', f.actor, f.target),
    () => f.store.setUserEnabled('target', false, f.actor, f.target),
    () => f.store.updateUser('target', { role: 'admin' }, f.actor, f.target),
  ]) assert.throws(mutate, /account_identity_changed/);
  assert.equal(audits, 0); f.unchangedReplacement();
});
