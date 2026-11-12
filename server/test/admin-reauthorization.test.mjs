import { captureAssignmentInput, enrollFixtureHosts } from './fixtures/admin-assignment-input.mjs';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { adminRoutes } from '../src/admin-http.mjs';
import { Relay } from '../src/relay.mjs';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { HttpError } from '../src/errors.mjs';

const origin = 'https://admin-race-fixture.invalid';
const machineUuid = '11111111-1111-4111-8111-111111111111';
const vm = { vmid: 710, node: 'fixture-node', name: 'Fixture', status: 'stopped', machineUuid };
const proxmoxConfig = { endpoint: 'https://fixture.invalid', tokenId: 'fixture@pve!fixture', secret: 'not-a-real-secret', ca: '', expiresAt: null };
const password = { password: 'fixture-password-only', passwordConfirmation: 'fixture-password-only' };
function deferred() { let resolve; const promise = new Promise(done => { resolve = done; }); return { promise, resolve }; }

function fixture(t, { input = {}, phase = 'body', setup = false } = {}) {
  const store = new Store(':memory:');
  const vault = new Vault(randomBytes(32));
  t.after(() => store.close());
  const primary = store.addUser({ name: 'primary', role: 'admin', passwordHash: 'unused-fixture-hash' });
  const actor = store.addUser({ name: 'operator', role: 'admin', passwordHash: 'unused-fixture-hash' });
  store.addUser({ name: 'target', passwordHash: 'unused-target-hash' });
  store.syncProxmox([vm], 'fixture');
  enrollFixtureHosts(store, ['pve-710']);
  store.saveSetting('proxmox', vault.seal(proxmoxConfig), 'fixture');
  if (setup) store.db.prepare('UPDATE users SET must_change_credentials=1 WHERE id=?').run(actor);
  const token = newToken(), hash = tokenHash(token);
  store.createSession(store.findUser('operator'), hash, Date.now(), 60000, 'admin');
  assert.equal(store.authenticate(hash, Date.now(), 'admin').id, actor);
  const entered = deferred(), release = deferred(), calls = [];
  let csrfValid = true;
  const pause = async at => { if (phase === at) { entered.resolve(); await release.promise; } };
  const verifyCsrf = vault.verifyCsrf.bind(vault);
  vault.verifyCsrf = (token, value) => csrfValid && verifyCsrf(token, value);
  const proxmox = {
    machineIdentity: async () => { await pause('identity'); return { machineUuid }; },
    inventory: async () => { await pause('inventory'); return [vm]; },
    tokenStatus: async () => ({ state: 'ok' }),
    task: async () => ({ status: 'stopped' }),
  };
  const accepted = label => { calls.push(label); return { accepted: true }; };
  const relay = {
    clipboardPolicyChanged: () => {}, devicePolicyChanged: () => {},
    requestDiagnostics: () => accepted('diagnostics'),
    createEnrollment: (...args) => Relay.prototype.createEnrollment.call({ store, proxmox, config: () => proxmoxConfig, publicOrigin: origin }, ...args),
    hostUpdate: {
      request: async ({ authorize }) => { authorize(); return accepted('host-update'); },
      views: async () => ({ latest: null, desktops: {} }),
    },
  };
  const routes = adminRoutes({
    store, vault, proxmox, relay, publicOrigin: origin,
    initialSetup: { verify: () => {} },
    body: async () => { await pause('body'); return structuredClone(input); },
    // No real password computation: the gate is the async boundary itself.
    passwordJob: async () => { await pause('password'); return 'unused-new-fixture-hash'; },
    power: { submit: async authorize => { authorize(); const result = accepted('power'); await pause('accepted-power'); return result; } },
    hostRequests: { approve: async (_id, _input, authorize) => { authorize(); return accepted('approval'); }, reject: (_id, authorize) => { authorize(); return accepted('rejection'); } },
    serverInfo: { version: '0.5.69' },
    serverUpdate: { check: async () => { const result = accepted('server-update'); await pause('accepted-update'); return result; } },
  });
  const start = (path, method) => {
    input = captureAssignmentInput(store, path, input);
    const account = /^\/api\/v1\/admin\/users\/([^/]+)(?:\/password|\/enabled)?$/.exec(path);
    if (account && method === 'PUT') input = { ...input, expectedAccountId: store.findUser(decodeURIComponent(account[1])).id };
    let status;
    const pending = routes({ method, headers: { cookie: `__Secure-multitap-admin=${token}`, origin, 'x-csrf-token': vault.csrf(token) } }, { setHeader() {} }, path,
      value => { status = value; }).then(() => ({ status }), error => ({ error }));
    return pending;
  };
  const snapshot = () => JSON.stringify(['users', 'assignments', 'desktops', 'settings', 'host_enrollments', 'audit_events'].map(table => store.db.prepare(`SELECT * FROM ${table} ORDER BY rowid`).all()));
  const revoke = kind => {
    if (kind === 'demoted') store.updateUser('operator', { role: 'user' }, primary);
    else if (kind === 'disabled') store.setUserEnabled('operator', false, primary);
    else if (kind === 'logout') store.logout(hash);
    else if (kind === 'expired') store.db.prepare('UPDATE sessions SET expires_at=? WHERE token_hash=?').run(Date.now() - 1, hash);
    else if (kind === 'password-reset') store.changePassword('operator', 'unused-revoked-hash', primary);
    else if (kind === 'credentials-required') store.db.prepare('UPDATE users SET must_change_credentials=1 WHERE id=?').run(actor);
    else if (kind === 'csrf') csrfValid = false;
    else if (kind === 'renamed-account-recreated' || kind === 'principal-rebound') {
      store.deleteUser('operator', primary);
      store.addUser({ name: 'operator', role: 'admin', passwordHash: 'unused-recreated-hash' });
      if (kind === 'principal-rebound') {
        // Deliberately simulate a different principal under the same cookie
        // hash to test immutable identity, not just username/role equality.
        store.createSession(store.findUser('operator'), hash, Date.now(), 60000, 'admin');
        assert.notEqual(store.authenticate(hash, Date.now(), 'admin').id, actor);
      }
    } else throw new Error('unknown_fixture_revocation');
  };
  return { store, actor, primary, entered, release, start, snapshot, revoke, calls, token, hash };
}

const routeCases = [
  ['create admin', 'POST', '/api/v1/admin/users', { username: 'created', role: 'admin', ...password }],
  ['self admin restore', 'PUT', '/api/v1/admin/users/operator', { role: 'admin' }],
  ['edit account', 'PUT', '/api/v1/admin/users/target', { notes: 'changed', role: 'admin' }],
  ['disable account', 'PUT', '/api/v1/admin/users/target/enabled', { enabled: false }],
  ['reset password', 'PUT', '/api/v1/admin/users/target/password', password],
  ['batch assignment', 'PUT', '/api/v1/admin/assignments/batch', { username: 'target', desktopIds: ['pve-710'] }],
  ['grant assignment', 'POST', '/api/v1/admin/assignments', { username: 'target', desktopId: 'pve-710' }],
  ['revoke assignment', 'DELETE', '/api/v1/admin/assignments', { username: 'target', desktopId: 'pve-710' }],
  ['edit desktop', 'PUT', '/api/v1/admin/desktops/pve-710', { name: 'Changed fixture' }],
  ['request diagnostics', 'POST', '/api/v1/admin/desktops/pve-710/diagnostics', {}],
  ['legacy enrollment', 'POST', '/api/v1/admin/desktops/pve-710/enrollment', {}],
  ['power dispatch', 'POST', '/api/v1/admin/desktops/pve-710/power', { action: 'start' }],
  ['server update dispatch', 'POST', '/api/v1/admin/server-update/check', {}],
  ['host update dispatch', 'POST', '/api/v1/admin/desktops/pve-710/host-update', { version: '0.2.84' }],
  ['host approval', 'POST', `/api/v1/admin/hosts/requests/${machineUuid}/approve`, {}],
  ['host rejection', 'POST', `/api/v1/admin/hosts/requests/${machineUuid}/reject`, {}],
  ['proxmox setting', 'PUT', '/api/v1/admin/proxmox', proxmoxConfig],
  ['proxmox sync dispatch', 'POST', '/api/v1/admin/proxmox/sync', {}],
];
const revocations = ['demoted', 'disabled', 'logout', 'expired', 'password-reset', 'credentials-required', 'csrf', 'renamed-account-recreated', 'principal-rebound'];

for (const [label, method, path, input] of routeCases) {
  for (const revocation of revocations) {
    test(`revoked during body: ${label} / ${revocation}`, { timeout: 3000 }, async t => {
      const f = fixture(t, { input });
      // A valid assignment makes the DELETE case a meaningful no-mutation check.
      f.store.assign('target', 'pve-710', 'fixture');
      const request = f.start(path, method);
      await f.entered.promise;
      f.revoke(revocation);
      const before = f.snapshot();
      f.release.resolve();
      const result = await request;
      assert.ok(result.error instanceof HttpError, `privileged request was not rejected; response=${result.status}; changed=${f.snapshot() !== before}`);
      assert.equal(result.error.status, revocation === 'csrf' ? 403 : 401);
      assert.equal(f.snapshot(), before, 'revoked request changed database state');
      assert.deepEqual(f.calls, [], 'revoked request dispatched a privileged action');
    });
  }
  test(`current admin remains authorized: ${label}`, { timeout: 3000 }, async t => {
    const f = fixture(t, { input, phase: 'none' });
    f.store.assign('target', 'pve-710', 'fixture');
    const result = await f.start(path, method);
    assert.ifError(result.error);
    assert.ok([200, 201, 202].includes(result.status));
  });
}

for (const [label, method, path, input] of routeCases.filter(([label]) => ['create admin', 'reset password'].includes(label))) {
  for (const revocation of revocations) {
    test(`revoked during password job: ${label} / ${revocation}`, { timeout: 3000 }, async t => {
      const f = fixture(t, { input, phase: 'password' });
      const request = f.start(path, method);
      await f.entered.promise;
      f.revoke(revocation);
      const before = f.snapshot();
      f.release.resolve();
      const result = await request;
      assert.ok(result.error instanceof HttpError, `password mutation was not rejected; response=${result.status}; changed=${f.snapshot() !== before}`);
      assert.equal(result.error.status, revocation === 'csrf' ? 403 : 401);
      assert.equal(f.snapshot(), before);
    });
  }
}

for (const revocation of revocations) {
  test(`legacy enrollment identity await: ${revocation}`, { timeout: 3000 }, async t => {
    const f = fixture(t, { phase: 'identity' });
    const request = f.start('/api/v1/admin/desktops/pve-710/enrollment', 'POST');
    await f.entered.promise;
    f.revoke(revocation);
    const before = f.snapshot();
    f.release.resolve();
    const result = await request;
    assert.ok(result.error instanceof HttpError);
    assert.equal(result.error.status, revocation === 'csrf' ? 403 : 401);
    assert.equal(f.snapshot(), before, 'identity lookup issued an enrollment after revocation');
  });
}

for (const phase of ['body', 'password']) {
  for (const revocation of ['logout', 'expired', 'csrf']) {
    test(`initial setup ${phase} await: ${revocation}`, { timeout: 3000 }, async t => {
      const f = fixture(t, { phase, setup: true, input: { username: 'renamed-operator', ...password, setupCode: 'fixture' } });
      const request = f.start('/api/v1/admin/setup', 'POST');
      await f.entered.promise;
      f.revoke(revocation);
      const before = f.snapshot();
      f.release.resolve();
      const result = await request;
      assert.ok(result.error instanceof HttpError);
      assert.equal(result.error.status, revocation === 'csrf' ? 403 : 401);
      assert.equal(f.snapshot(), before);
    });
  }
}

test('initial setup succeeds and its must-change session can still log out', async t => {
  const f = fixture(t, { phase: 'none', setup: true, input: { username: 'renamed-operator', ...password, setupCode: 'fixture' } });
  const result = await f.start('/api/v1/admin/setup', 'POST');
  assert.ifError(result.error);
  assert.equal(result.status, 200);
  assert.equal(f.store.findUser('renamed-operator').must_change_credentials, 0);
  const g = fixture(t, { phase: 'none', setup: true });
  const logout = await g.start('/api/v1/admin/logout', 'POST');
  assert.ifError(logout.error);
  assert.equal(logout.status, 200);
});

for (const [operation, phase, path, input] of [
  ['server-update', 'accepted-update', '/api/v1/admin/server-update/check', {}],
  ['power', 'accepted-power', '/api/v1/admin/desktops/pve-710/power', { action: 'start' }],
]) {
  test(`already accepted ${operation} retains completion after logout`, { timeout: 3000 }, async t => {
    const f = fixture(t, { phase, input });
    const request = f.start(path, 'POST');
    await f.entered.promise;
    assert.deepEqual(f.calls, [operation], 'operation was accepted before revocation');
    f.revoke('logout');
    f.release.resolve();
    const result = await request;
    assert.ifError(result.error);
    assert.equal(result.status, 202);
    assert.deepEqual(f.calls, [operation]);
    if (operation === 'server-update') assert.ok(f.store.auditLog().some(row => row.event === 'server_update_checked'));
  });
}
