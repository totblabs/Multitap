import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm, readFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, dirname, basename, resolve } from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { Store } from '../src/store.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { enrollFixtureHosts } from './fixtures/admin-assignment-input.mjs';

const password = 'Only-for-protocol-tests-42!';
async function fixture(t, options = {}) {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-test-'));
  const store = new Store(join(directory, 'broker.sqlite'));
  const hashed = await hashPassword(password);
  store.addUser({ name: 'alice', displayName: 'Alice', passwordHash: hashed });
  store.addUser({ name: 'bobby', displayName: 'Bobby', passwordHash: hashed, role: 'admin' });
  // These auth fixtures are synchronized managed VMs, not unapproved local PCs.
  store.addDesktop({ id: 'office', name: 'Office', kind: 'vm', vmid: 701 });
  store.addDesktop({ id: 'private-vm', name: 'Private VM', kind: 'vm', vmid: 702 });
  store.syncProxmox([
    { vmid: 701, name: 'Office', node: 'fixture-pve', status: 'running' },
    { vmid: 702, name: 'Private VM', node: 'fixture-pve', status: 'running' }
  ], 'test-fixture');
  // The client list projects assigned VMs with approved (here offline) Hosts.
  enrollFixtureHosts(store, ['office', 'private-vm']);
  store.assign('alice', 'office'); store.assign('bobby', 'private-vm');
  const server = await buildServer({ store, publicOrigin: 'https://broker-tests.example', logger: { error: value => assert.fail(value) }, ...options });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const url = `http://127.0.0.1:${server.address().port}`;
  t.after(async () => {
    await new Promise(resolve => server.close(resolve));
    store.close();
    assert.equal(dirname(resolve(directory)), resolve(tmpdir()));
    assert.ok(basename(directory).startsWith('multitap-test-'));
    await rm(directory, { recursive: true });
  });
  async function call(path, { token, method = 'GET', data, raw, headers = {} } = {}) {
    const response = await fetch(url + path, { method, headers: { ...(data !== undefined || raw !== undefined ? { 'Content-Type': 'application/json' } : {}), ...(token ? { Authorization: `Bearer ${token}` } : {}), ...headers }, body: raw ?? (data === undefined ? undefined : JSON.stringify(data)) });
    return { status: response.status, headers: response.headers, body: await response.json() };
  }
  async function login(name = 'alice', secret = password) {
    return call('/api/v1/auth/login', { method: 'POST', data: { username: name, password: secret } });
  }
  return { store, directory, call, login, url };
}

test('Real HTTP authentication stores only password and token hashes', async t => {
  const f = await fixture(t);
  for (const name of ['alice', 'nobody']) {
    const result = await f.login(name, 'incorrect-password');
    assert.equal(result.status, 401); assert.equal(result.body.error.code, 'invalid_credentials');
  }
  const result = await f.login('ALICE');
  assert.equal(result.status, 200);
  assert.equal(result.body.user.username, 'alice');
  assert.match(result.body.sessionToken, /^[A-Za-z0-9_-]{43}$/);
  assert.equal(result.headers.get('cache-control'), 'no-store');
  const rows = f.store.db.prepare('SELECT * FROM sessions').all();
  assert.equal(rows.length, 1); assert.notEqual(rows[0].token_hash, result.body.sessionToken);
  assert.equal(JSON.stringify(f.store.inventory()).includes('password_hash'), false);
  assert.notEqual(f.store.findUser('alice').password_hash, password);
  assert.equal((await f.call('/api/v1/auth/me', { token: result.body.sessionToken })).status, 200);
  assert.equal((await f.call('/api/v1/desktops')).status, 401);
});

test('Each account sees only assigned desktops, including the admin account', async t => {
  const f = await fixture(t);
  const alice = (await f.login()).body.sessionToken;
  const bobby = (await f.login('bobby')).body.sessionToken;
  assert.deepEqual((await f.call('/api/v1/desktops', { token: alice })).body.desktops.map(d => d.id), ['office']);
  assert.deepEqual((await f.call('/api/v1/desktops', { token: bobby })).body.desktops.map(d => d.id), ['private-vm']);
  for (const path of ['/api/v1/desktops/private-vm', '/api/v1/desktops/missing']) assert.equal((await f.call(path, { token: alice })).status, 404);
  assert.equal((await f.call('/api/v1/desktops/private-vm/connect', { token: alice, method: 'POST' })).status, 404);
  const own = await f.call('/api/v1/desktops/office/connect', { token: alice, method: 'POST' });
  assert.equal(own.status, 409); assert.equal(own.body.error.code, 'host_not_registered');
  for (const method of ['POST', 'PUT', 'DELETE']) assert.equal((await f.call('/api/v1/desktops', { token: alice, method, data: { id: 'intruder' } })).status, 404);
  assert.equal(f.store.inventory().desktops.length, 2);
});

test('Assignment revocation and inventory removal affect existing sessions immediately', async t => {
  const f = await fixture(t); const token = (await f.login()).body.sessionToken;
  f.store.revoke('alice', 'office');
  assert.deepEqual((await f.call('/api/v1/desktops', { token })).body.desktops, []);
  assert.equal((await f.call('/api/v1/desktops/office', { token })).status, 404);
  f.store.assign('alice', 'office');
  // Managed VMs ignore the retired enabled flag; an authoritative inventory
  // removal makes the assignment unavailable without silently deleting it.
  f.store.syncProxmox([{ vmid: 702, name: 'Private VM', node: 'fixture-pve', status: 'running' }], 'test-fixture');
  assert.equal((await f.call('/api/v1/desktops/office/connect', { token, method: 'POST' })).status, 404);
});

test('Logout, password reset and account disable revoke sessions', async t => {
  const f = await fixture(t); const token = (await f.login()).body.sessionToken;
  assert.equal((await f.call('/api/v1/auth/logout', { token, method: 'POST' })).status, 200);
  assert.equal((await f.call('/api/v1/auth/me', { token })).status, 401);
  const second = (await f.login()).body.sessionToken;
  f.store.changePassword('alice', await hashPassword('Replacement-test-password-7!'));
  assert.equal((await f.call('/api/v1/auth/me', { token: second })).status, 401);
  assert.equal((await f.login()).status, 401);
  const third = (await f.login('alice', 'Replacement-test-password-7!')).body.sessionToken;
  f.store.setUserEnabled('alice', false);
  assert.equal((await f.call('/api/v1/auth/me', { token: third })).status, 401);
  assert.equal((await f.login('alice', 'Replacement-test-password-7!')).status, 401);
});

test('A short actual session expires without a mocked clock', async t => {
  const f = await fixture(t, { sessionTtlMs: 100 });
  const token = (await f.login()).body.sessionToken;
  await new Promise(resolve => setTimeout(resolve, 140));
  assert.equal((await f.call('/api/v1/auth/me', { token })).status, 401);
});

test('Renewal slides the same token forward, within an absolute maximum lifetime', async t => {
  const f = await fixture(t, { sessionTtlMs: 400, sessionMaxLifetimeMs: 700 });
  const login = await f.login(); const token = login.body.sessionToken;
  const first = Date.parse(login.body.expiresAt);
  await new Promise(resolve => setTimeout(resolve, 250));
  const renewed = await f.call('/api/v1/auth/renew', { token, method: 'POST' });
  assert.equal(renewed.status, 200); assert.equal(renewed.headers.get('cache-control'), 'no-store');
  assert.ok(Date.parse(renewed.body.expiresAt) > first);
  assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM sessions').get().n, 1, 'renewal rotates nothing');
  await new Promise(resolve => setTimeout(resolve, 250)); // past the original expiry
  assert.equal((await f.call('/api/v1/auth/me', { token })).status, 200);
  const created = f.store.db.prepare('SELECT created_at FROM sessions').get().created_at;
  assert.ok(Date.parse(renewed.body.expiresAt) <= created + 700, 'never past the absolute maximum');
  await new Promise(resolve => setTimeout(resolve, 150));
  const capped = await f.call('/api/v1/auth/renew', { token, method: 'POST' });
  if (capped.status === 200) assert.ok(Date.parse(capped.body.expiresAt) <= created + 700);
  await new Promise(resolve => setTimeout(resolve, 250));
  const gone = await f.call('/api/v1/auth/renew', { token, method: 'POST' });
  assert.equal(gone.status, 401); assert.equal(gone.body.error.code, 'session_expired');
});

test('Renewal needs a valid client session and honours revocation', async t => {
  const f = await fixture(t);
  assert.equal((await f.call('/api/v1/auth/renew', { method: 'POST' })).status, 401);
  assert.equal((await f.call('/api/v1/auth/renew', { token: 'A'.repeat(43), method: 'POST' })).status, 401);
  const token = (await f.login()).body.sessionToken;
  assert.equal((await f.call('/api/v1/auth/renew', { token })).status, 405);
  assert.equal((await f.call('/api/v1/auth/renew', { token, method: 'POST', data: { x: 1 } })).status, 400);
  f.store.setUserEnabled('alice', false);
  assert.equal((await f.call('/api/v1/auth/renew', { token, method: 'POST' })).status, 401);
  f.store.setUserEnabled('alice', true);
  const again = (await f.login()).body.sessionToken;
  f.store.changePassword('alice', await hashPassword('Replacement-test-password-7!'));
  assert.equal((await f.call('/api/v1/auth/renew', { token: again, method: 'POST' })).status, 401);
});

test('Malformed, oversized and excessive login requests are rejected', async t => {
  const f = await fixture(t, { loginLimit: 2 });
  assert.equal((await f.call('/api/v1/auth/login', { method: 'POST', raw: '{' })).status, 400);
  assert.equal((await f.call('/api/v1/auth/login', { method: 'POST', raw: JSON.stringify({ data: 'x'.repeat(9000) }) })).status, 413);
  assert.equal((await f.call('/api/v1/auth/login', { method: 'POST', data: { username: 'alice', password, role: 'admin' } })).status, 400);
  await f.login('alice', 'bad'); await f.login('alice', 'bad');
  const limit = await f.login(); assert.equal(limit.status, 429); assert.equal(limit.headers.get('retry-after'), '60');
  assert.equal((await f.call('/api/v1/auth/login', { method: 'POST', data: { username: 'bobby', password }, headers: { Origin: 'https://untrusted.example' } })).status, 403);
});

test('Server-only CLI changes persist and authenticate over HTTP', async t => {
  const f = await fixture(t);
  const admin = fileURLToPath(new URL('../src/admin.mjs', import.meta.url));
  const run = (args, input = '') => spawnSync(process.execPath, [admin, ...args], { env: { ...process.env, MULTITAP_DATA_DIR: f.directory }, input, encoding: 'utf8' });
  const result = run(['user-add', 'charlie', '--password-stdin'], password + '\n');
  assert.equal(result.status, 0, result.stderr); assert.equal(result.stdout.includes(password), false);
  assert.equal(run(['assign', 'charlie', 'office']).status, 0);
  const login = await f.login('charlie'); assert.equal(login.status, 200);
  assert.deepEqual((await f.call('/api/v1/desktops', { token: login.body.sessionToken })).body.desktops.map(d => d.id), ['office']);
  const otherConnection = new Store(join(f.directory, 'broker.sqlite'));
  assert.equal(otherConnection.findUser('charlie').username, 'charlie');
  otherConnection.close();
  const raw = await readFile(join(f.directory, 'broker.sqlite-wal'));
  assert.equal(raw.includes(Buffer.from(password)), false);
});
