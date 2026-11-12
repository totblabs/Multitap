// Actual Store/adminRoutes and loopback HTTP. No operating accounts or PVE.
import test from 'node:test';
import assert from 'node:assert/strict';
import { request } from 'node:http';
import { setImmediate as nextTurn } from 'node:timers/promises';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { adminRoutes } from '../src/admin-http.mjs';
import { buildServer } from '../src/http.mjs';
import { tokenHash } from '../src/security.mjs';
import { clipboardPolicyFromMask } from '../src/clipboard-policy.mjs';
import { devicePolicyFromMask } from '../src/device-policy.mjs';
import { enrollFixtureHosts } from './fixtures/admin-assignment-input.mjs';

const origin = 'https://assignment-target.fixture.invalid', clip = { ...clipboardPolicyFromMask(0), textToRemote: true }, redirects = { ...devicePolicyFromMask(0), drives: true };
const kinds = ['assign', 'revoke', 'batch', 'permissions'];
function fixture(t, hooks = {}) {
  const store = new Store(':memory:'), vault = new Vault(Buffer.alloc(32, 29)), token = 'R'.repeat(43), notices = [];
  const actor = store.addUser({ name: 'operator', role: 'admin', passwordHash: 'unused-fixture' });
  const target = store.addUser({ name: 'target', passwordHash: 'unused-fixture' }), second = store.addUser({ name: 'second', passwordHash: 'unused-fixture' });
  store.saveSetting('proxmox_initialized', '1', 'fixture');
  store.createSession(store.findUserById(actor), tokenHash(token), Date.now(), 60000, 'admin', { issuer: 'admin' });
  store.syncProxmox([710, 711, 712].map(vmid => ({ vmid, node: 'fixture', name: 'VM' + vmid, status: 'running' })), actor);
  enrollFixtureHosts(store, ['pve-710', 'pve-711', 'pve-712'], actor);
  store.assign('target', 'pve-710', actor); store.assign('target', 'pve-711', actor); store.assign('second', 'pve-710', actor);
  t.after(() => store.close());
  let payload, replacement;
  const replace = () => {
    store.deleteUser('target', actor); replacement = store.addUser({ name: 'target', passwordHash: 'unused-fixture' });
    store.assign('target', 'pve-710', actor); store.assign('target', 'pve-711', actor);
    assert.notEqual(replacement, target); return replacement;
  };
  const relay = { clipboardPolicyChanged: (...args) => notices.push(['clipboard', ...args]), devicePolicyChanged: (...args) => notices.push(['devices', ...args]) };
  const routes = adminRoutes({ store, vault, relay, publicOrigin: origin, body: async () => { await hooks.body?.(replace); return structuredClone(payload); } });
  const spec = kind => kind === 'assign' || kind === 'revoke' ? { method: kind === 'assign' ? 'POST' : 'DELETE', path: '/api/v1/admin/assignments', input: { username: 'target', expectedAccountId: target, desktopId: kind === 'assign' ? 'pve-712' : 'pve-710' } }
    : kind === 'batch' ? { method: 'PUT', path: '/api/v1/admin/assignments/batch', input: { username: 'target', expectedAccountId: target, assignments: [{ desktopId: 'pve-712', clipboardPolicy: clip }] } }
      : { method: 'PUT', path: '/api/v1/admin/desktops/pve-710/permissions', input: { permissions: [{ username: 'target', expectedAccountId: target, clipboardPolicy: clip }, { username: 'second', expectedAccountId: second, devicePolicy: redirects }] } };
  const snapshot = () => JSON.stringify(['users', 'assignments', 'audit_events'].map(table => store.db.prepare('SELECT * FROM ' + table + ' ORDER BY rowid').all()));
  async function invoke(kind, options = {}) {
    const operation = spec(kind); payload = structuredClone(options.input ?? operation.input);
    const rows = kind === 'permissions' ? payload.permissions : [payload];
    for (const row of rows) { if (options.omitIdentity) delete row.expectedAccountId; if (Object.hasOwn(options, 'identity')) row.expectedAccountId = options.identity; }
    let response;
    try { await routes({ method: operation.method, headers: { origin: options.foreignOrigin ? 'https://other.invalid' : origin,
      cookie: '__Secure-multitap-admin=' + token, 'x-csrf-token': options.csrf === false ? '' : vault.csrf(token) } }, { setHeader() {} }, operation.path,
      (status, body) => { response = { status, body }; }); }
    catch (error) { response = { status: error.status, body: { error: { code: error.code } } }; }
    return response;
  }
  return { store, vault, token, actor, target, second, replace, invoke, spec, snapshot, notices, replacement: () => replacement };
}
for (const kind of kinds) {
  test(`assignment identity ${kind}: stable account control commits`, async t => {
    const f = fixture(t); assert.equal((await f.invoke(kind)).status, 200);
    if (kind === 'assign') assert.ok(f.store.assignedDesktop(f.target, 'pve-712'));
    if (kind === 'revoke') assert.equal(f.store.assignedDesktop(f.target, 'pve-710'), undefined);
    if (kind === 'batch') assert.deepEqual(f.store.userAssignments(f.target).map(x => x.desktopId), ['pve-712']);
    if (kind === 'permissions') { assert.deepEqual(f.store.clipboardPolicy(f.target, 'pve-710'), clip); assert.deepEqual(f.store.devicePolicy(f.second, 'pve-710'), redirects); }
  });
  test(`assignment identity ${kind}: stale old callback cannot target recreated username`, async t => {
    const f = fixture(t); f.replace(); const before = f.snapshot();
    const result = await f.invoke(kind); assert.equal(result.status, 409); assert.equal(result.body.error.code, 'account_identity_changed');
    assert.equal(f.snapshot(), before); assert.deepEqual(f.notices, []);
  });
  test(`assignment identity ${kind}: missing/malformed target makes no audit or mutation`, async t => {
    const f = fixture(t), before = f.snapshot();
    const missing = await f.invoke(kind, { omitIdentity: true }); assert.equal(missing.status, 428); assert.equal(missing.body.error.code, 'account_identity_required');
    for (const identity of [null, '', [], {}, 'bad identity', '한글', 'x'.repeat(129)]) {
      const result = await f.invoke(kind, { identity }); assert.equal(result.status, 400); assert.equal(result.body.error.code, 'invalid_account_identity');
    }
    assert.equal(f.snapshot(), before); assert.deepEqual(f.notices, []);
    assert.equal((await f.invoke(kind, { csrf: false })).status, 403); assert.equal((await f.invoke(kind, { foreignOrigin: true })).status, 403);
  });
  test(`assignment identity ${kind}: async body replacement is rechecked`, async t => {
    let replaced = false; const f = fixture(t, { body: replace => { if (!replaced) { replaced = true; replace(); } } });
    const result = await f.invoke(kind); assert.equal(result.status, 409); assert.equal(result.body.error.code, 'account_identity_changed');
    assert.deepEqual(f.store.userAssignments(f.replacement()).map(x => x.desktopId), ['pve-710', 'pve-711']); assert.deepEqual(f.notices, []);
  });
  test(`assignment identity ${kind}: renamed account cannot be rebound through its old username`, async t => {
    const f = fixture(t);
    // Change this synthetic DB row without deleting its immutable ID.
    f.store.db.prepare('UPDATE users SET username=? WHERE id=?').run('renamed', f.target);
    const before = f.snapshot();
    assert.equal((await f.invoke(kind)).status, 409);
    assert.equal(f.snapshot(), before); assert.deepEqual(f.notices, []);
  });
  test(`assignment identity ${kind}: real Store final transaction rejects a last-moment replacement`, t => {
    const f = fixture(t), transaction = f.store.transaction.bind(f.store); let replaced = false, before;
    f.store.transaction = action => { if (!replaced) { replaced = true; f.replace(); before = f.snapshot(); } return transaction(action); };
    const input = f.spec(kind).input;
    const mutate = kind === 'assign' ? () => f.store.assign(input.username, input.desktopId, f.actor, f.target)
      : kind === 'revoke' ? () => f.store.revoke(input.username, input.desktopId, f.actor, f.target)
        : kind === 'batch' ? () => f.store.replaceVmAssignments(input.username, input.assignments, f.actor, f.target)
          : () => f.store.replaceDesktopPermissions('pve-710', input.permissions, f.actor);
    assert.throws(mutate, /account_identity_changed/); assert.equal(f.snapshot(), before);
  });
}
test('assignment identity: identity-free old assignment callback mutates no replacement', async t => {
  const f = fixture(t); f.replace(); const before = f.snapshot();
  assert.equal((await f.invoke('assign', { omitIdentity: true })).status, 428); assert.equal(f.snapshot(), before);
});
test('assignment identity: mixed bulk conflict is wholly rejected before policy/audit/notice', async t => {
  const f = fixture(t); f.replace(); const before = f.snapshot(); const input = f.spec('permissions').input;
  input.permissions.reverse(); // valid first row must not commit before stale row.
  const result = await f.invoke('permissions', { input }); assert.equal(result.status, 409);
  assert.equal(f.snapshot(), before); assert.deepEqual(f.notices, []); assert.deepEqual(f.store.devicePolicy(f.second, 'pve-710'), devicePolicyFromMask(0));
});

test('assignment identity: a later missing/malformed row rejects the entire permissions batch', async t => {
  const f = fixture(t), before = f.snapshot();
  for (const value of [undefined, null, 'bad identity']) {
    const input = f.spec('permissions').input;
    if (value === undefined) delete input.permissions[1].expectedAccountId;
    else input.permissions[1].expectedAccountId = value;
    assert.equal((await f.invoke('permissions', { input })).status, value === undefined ? 428 : 400);
    assert.equal(f.snapshot(), before); assert.deepEqual(f.notices, []);
  }
});
test('assignment identity: permission callbacks retain committed immutable ID after later username reuse', async t => {
  const f = fixture(t), replace = f.store.replaceVmAssignments.bind(f.store);
  f.store.replaceVmAssignments = (...args) => { const result = replace(...args); f.replace(); return result; };
  const input = f.spec('batch').input;
  input.assignments[0].desktopId = 'pve-710'; // Restrict/change an existing grant so callbacks run.
  assert.equal((await f.invoke('batch', { input })).status, 200);
  assert.deepEqual(f.notices, [['clipboard', f.target], ['devices', f.target]]);
});
for (const kind of ['batch', 'permissions']) test(`assignment identity ${kind}: actual HTTP partial body cannot rebind target`, async t => {
  const f = fixture(t), errors = [];
  const server = await buildServer({ store: f.store, vault: f.vault, publicOrigin: origin, logger: { error: x => errors.push(x) } });
  await new Promise(done => server.listen(0, '127.0.0.1', done)); t.after(async () => { await new Promise(done => server.close(done)); assert.deepEqual(errors, []); });
  const operation = f.spec(kind), body = JSON.stringify(operation.input), split = Math.floor(body.length / 2);
  let resolve, reject; const finished = new Promise((ok, fail) => { resolve = ok; reject = fail; });
  const req = request({ hostname: '127.0.0.1', port: server.address().port, path: operation.path, method: operation.method,
    headers: { Origin: origin, Cookie: '__Secure-multitap-admin=' + f.token, 'X-CSRF-Token': f.vault.csrf(f.token), 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body) } }, res => {
    const chunks = []; res.on('data', x => chunks.push(x)); res.on('end', () => resolve({ status: res.statusCode, body: JSON.parse(Buffer.concat(chunks).toString('utf8')) }));
  }); req.on('error', reject); req.write(body.slice(0, split)); await nextTurn(); await nextTurn();
  f.replace(); const before = f.snapshot(); req.end(body.slice(split)); const result = await finished;
  assert.equal(result.status, 409); assert.equal(result.body.error.code, 'account_identity_changed'); assert.equal(f.snapshot(), before);
});
