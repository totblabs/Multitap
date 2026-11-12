import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword, initialPasswordHash } from '../src/security.mjs';
import { setupCodeSetting, containerRun, InitialSetupCode } from '../src/initial-setup.mjs';
import { normalizeSetupCode } from '../src/verification-codes.mjs';

// One-time first-administrator setup code: admin/admin alone does
// not complete the first setup of a fresh server; the code is printed only
// to the server's own log and stored as a hash.
const origin = 'https://setup-code.example';
const codePattern = /^[0-9A-HJKMNP-TV-Z]{4}-[0-9A-HJKMNP-TV-Z]{4}-[0-9A-HJKMNP-TV-Z]{4}$/;
const replacement = 'Setup-code-tests-password-71!';

async function fresh(t, { commissioned = false, path = ':memory:', store: existing, run = null } = {}) {
  const store = existing ?? new Store(path), vault = new Vault(randomBytes(32)), announced = [], kept = [];
  if (!existing) {
    const id = store.addUser({ name: 'admin', role: 'admin', passwordHash: commissioned ? await hashPassword(replacement) : await initialPasswordHash() });
    if (!commissioned) store.db.prepare('UPDATE users SET must_change_credentials=1 WHERE id=?').run(id);
  }
  const server = await buildServer({ store, vault, publicOrigin: origin, logger: { error: value => assert.fail(value) }, initialSetupOptions: { announce: code => announced.push(code), announceKept: guide => kept.push(guide), run } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const close = () => new Promise(resolve => server.close(resolve));
  t.after(async () => { if (server.listening) await close(); if (!existing && path === ':memory:') store.close(); });
  const base = `http://127.0.0.1:${server.address().port}/api/v1/admin`;
  let cookie = '', csrf = '';
  const call = async (path, method = 'GET', data) => {
    const response = await fetch(base + path, { method, headers: { Cookie: cookie, ...(method !== 'GET' ? { Origin: origin, 'X-CSRF-Token': csrf, 'Content-Type': 'application/json' } : {}) }, body: data === undefined ? undefined : JSON.stringify(data) });
    return { status: response.status, headers: response.headers, body: await response.json() };
  };
  const login = async (username = 'admin', password = 'admin') => {
    const response = await call('/login', 'POST', { username, password });
    assert.equal(response.status, 200); cookie = response.headers.get('set-cookie').split(';')[0]; csrf = response.body.csrfToken; return response;
  };
  const setup = setupCode => call('/setup', 'POST', { username: 'owner', password: replacement, passwordConfirmation: replacement, ...(setupCode === undefined ? {} : { setupCode }) });
  return { store, announced, kept, call, login, setup, close };
}

test('A fresh server prints one code at startup, stores only its hash, and requires it for the first setup', async t => {
  const f = await fresh(t);
  assert.equal(f.announced.length, 1); assert.match(f.announced[0], codePattern);
  const stored = f.store.setting(setupCodeSetting);
  assert.match(stored, /^[a-f0-9]{64}$/); assert.equal(stored.includes(normalizeSetupCode(f.announced[0])), false);
  assert.equal(JSON.stringify(f.store.db.prepare('SELECT * FROM settings').all()).includes(f.announced[0].slice(0, 4) + '-'), false);
  await f.login();
  // admin/admin alone, a wrong code or a malformed one: refused before any password work.
  for (const code of [undefined, 'AAAA-BBBB-CCCC', 'not a code', 42]) {
    const refused = await f.setup(code);
    assert.equal(refused.status, 403, String(code)); assert.equal(refused.body.error.code, 'setup_code_invalid');
  }
  assert.equal(f.store.findUser('admin').must_change_credentials, 1);
  // The printed code also works typed in lower case without hyphens (Crockford: O=0, I/L=1).
  const typed = f.announced[0].replaceAll('-', '').toLowerCase();
  assert.equal((await f.setup(typed)).status, 200);
  assert.equal(f.store.setting(setupCodeSetting), undefined, 'the code is spent with the setup');
  assert.equal(f.store.findUser('owner').must_change_credentials, 0);
  assert.equal(f.announced.length, 1);
});

test('Restarting a still-fresh server issues a new code and the old one stops working', async t => {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-setup-code-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const store = new Store(join(directory, 'broker.sqlite'));
  const id = store.addUser({ name: 'admin', role: 'admin', passwordHash: await initialPasswordHash() });
  store.db.prepare('UPDATE users SET must_change_credentials=1 WHERE id=?').run(id);
  const first = await fresh(t, { store }); await first.close();
  const second = await fresh(t, { store });
  assert.notEqual(second.announced[0], first.announced[0]);
  await second.login();
  assert.equal((await second.setup(first.announced[0])).body.error.code, 'setup_code_invalid');
  assert.equal((await second.setup(second.announced[0])).status, 200);
  await second.close(); store.close();
});

test('A commissioned server never prints a code, clears a stale one, and its administration is unchanged', async t => {
  const store = new Store(':memory:');
  t.after(() => store.close());
  store.addUser({ name: 'owner', role: 'admin', passwordHash: await hashPassword(replacement) });
  store.db.prepare('INSERT INTO settings(key,value) VALUES(?,?)').run(setupCodeSetting, 'a'.repeat(64));
  const f = await fresh(t, { store });
  assert.deepEqual(f.announced, []); assert.equal(store.setting(setupCodeSetting), undefined);
  await f.login('owner', replacement);
  assert.equal((await f.call('/session')).body.user.mustChangeCredentials, false);
  assert.equal((await f.setup()).body.error.code, 'setup_not_required');
});

test('Code attempts are limited per admin session, so one hostile admin/admin session cannot lock out the operator', async t => {
  const f = await fresh(t);
  await f.login();
  for (let attempt = 0; attempt < 10; attempt++) assert.equal((await f.setup('AAAA-BBBB-CCCC')).status, 403);
  const limited = await f.setup(f.announced[0]);
  assert.equal(limited.status, 429); assert.equal(limited.body.error.code, 'too_many_setup_attempts');
  await f.login();
  assert.equal((await f.setup(f.announced[0])).status, 200);
});

// In a container the server starts several times in one run (it ends itself to
// apply a change of its access settings and the entrypoint starts it again),
// and the installer printed the code once.
const runA = 'a'.repeat(64), runB = 'b'.repeat(64);
async function freshStore(t) {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-setup-code-run-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const store = new Store(join(directory, 'broker.sqlite'));
  const id = store.addUser({ name: 'admin', role: 'admin', passwordHash: await initialPasswordHash() });
  store.db.prepare('UPDATE users SET must_change_credentials=1 WHERE id=?').run(id);
  return store;
}

test('In one container run the code stays the same across restarts of the server, is not printed again, and is spent with the setup', async t => {
  const store = await freshStore(t);
  const first = await fresh(t, { store, run: runA });
  assert.equal(first.announced.length, 1); assert.match(first.announced[0], codePattern); assert.deepEqual(first.kept, []);
  // Stored: the code's hash and the run, never the code.
  const stored = store.setting(setupCodeSetting);
  assert.match(stored, /^[a-f0-9]{64}:a{64}$/);
  assert.equal(JSON.stringify(store.db.prepare('SELECT * FROM settings').all()).includes(first.announced[0].slice(0, 4) + '-'), false);
  assert.equal(stored.includes(normalizeSetupCode(first.announced[0])), false);
  await first.close();
  // The server starts again in the same run, twice: no new code, the stored one untouched, and a line that says so.
  for (let index = 0; index < 2; index++) {
    const again = await fresh(t, { store, run: runA });
    assert.deepEqual(again.announced, [], 'the code is not printed a second time (only its hash is kept)'); assert.equal(again.kept.length, 1);
    assert.equal(store.setting(setupCodeSetting), stored);
    await again.login();
    assert.equal((await again.setup('AAAA-BBBB-CCCC')).body.error.code, 'setup_code_invalid');
    if (index === 1) {
      assert.equal((await again.setup(first.announced[0])).status, 200, 'the code the installer printed');
      assert.equal(store.setting(setupCodeSetting), undefined, 'spent with the setup');
    }
    await again.close();
  }
  // Commissioned: nothing is printed or kept any more.
  const done = await fresh(t, { store, run: runA });
  assert.deepEqual(done.announced, []); assert.deepEqual(done.kept, []); assert.equal(store.setting(setupCodeSetting), undefined);
  await done.close(); store.close();
});

test('A new container run, or a server that cannot name its run, issues a new code and the old one stops working', async t => {
  const store = await freshStore(t);
  const first = await fresh(t, { store, run: runA }); await first.close();
  // The container was started again (docker restart, an update, a reboot): its earlier log may be gone.
  const second = await fresh(t, { store, run: runB });
  assert.equal(second.announced.length, 1); assert.notEqual(second.announced[0], first.announced[0]); assert.deepEqual(second.kept, []);
  assert.match(store.setting(setupCodeSetting), /^[a-f0-9]{64}:b{64}$/);
  await second.login();
  assert.equal((await second.setup(first.announced[0])).body.error.code, 'setup_code_invalid');
  await second.close();
  // No run (not a Docker install, or one that cannot be read): a new code at every start, stored as the hash alone.
  const third = await fresh(t, { store }); await third.close();
  assert.match(store.setting(setupCodeSetting), /^[a-f0-9]{64}$/);
  const fourth = await fresh(t, { store });
  assert.notEqual(fourth.announced[0], third.announced[0]); assert.deepEqual(fourth.kept, []);
  await fourth.close();
  // A code stored without a run is never taken for this run's; nor is a run name that is no run name.
  const fifth = await fresh(t, { store, run: runA });
  assert.equal(fifth.announced.length, 1); assert.notEqual(fifth.announced[0], fourth.announced[0]);
  await fifth.close();
  for (const run of ['', 'not-a-run', 7]) { const odd = await fresh(t, { store, run }); assert.equal(odd.announced.length, 1); assert.match(store.setting(setupCodeSetting), /^[a-f0-9]{64}$/); await odd.close(); }
  store.close();
});

test('A setup that became pending after startup gets its code on the first attempt, with the run it was issued in', async t => {
  const store = new Store(':memory:');
  t.after(() => store.close());
  const announced = [], setup = new InitialSetupCode(store, { announce: (code, guide, options) => announced.push([code, options]), run: runA });
  setup.startup();
  assert.deepEqual(announced, []);
  const id = store.addUser({ name: 'admin', role: 'admin', passwordHash: await initialPasswordHash() });
  store.db.prepare('UPDATE users SET must_change_credentials=1 WHERE id=?').run(id);
  assert.throws(() => setup.verify('AAAA-BBBB-CCCC', 'session'), error => error.code === 'setup_code_required');
  assert.equal(announced.length, 1); assert.deepEqual(announced[0][1], { lasting: true });
  assert.match(store.setting(setupCodeSetting), /^[a-f0-9]{64}:a{64}$/);
  setup.verify(announced[0][0], 'session');
  assert.throws(() => setup.verify('AAAA-BBBB-CCCC', 'session'), error => error.code === 'setup_code_invalid');
});

test('A container run is named from process 1 and the boot of the machine; nothing readable, no run', () => {
  const files = { '/proc/1/stat': '1 (docker-init) S 0 1 1 0 -1 4194560 100 0 0 0 1 2 0 0 20 0 1 0 4242 1000000 100 18446744073709551615 1 1 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n', '/proc/sys/kernel/random/boot_id': '0f8b1c2d-3e4f-4a5b-8c6d-7e8f9a0b1c2d\n' };
  const read = extra => path => { const value = { ...files, ...extra }[path]; if (value === undefined) throw Error('ENOENT'); return value; };
  const run = containerRun(read({}));
  assert.match(run, /^[a-f0-9]{64}$/); assert.equal(containerRun(read({})), run, 'the same run, the same name');
  // A name with spaces and brackets does not move the fields.
  assert.equal(containerRun(read({ '/proc/1/stat': files['/proc/1/stat'].replace('(docker-init)', '(sh) (x) y)') })), run);
  // The container started again (another process 1), or the machine did: another run.
  assert.notEqual(containerRun(read({ '/proc/1/stat': files['/proc/1/stat'].replace(' 4242 ', ' 9001 ') })), run);
  assert.notEqual(containerRun(read({ '/proc/sys/kernel/random/boot_id': '11111111-2222-4333-8444-555555555555\n' })), run);
  for (const extra of [{ '/proc/1/stat': undefined }, { '/proc/1/stat': '1 (sh) S 0' }, { '/proc/1/stat': '' }, { '/proc/sys/kernel/random/boot_id': 'unknown' }, { '/proc/sys/kernel/random/boot_id': undefined }]) assert.equal(containerRun(read(extra)), null, JSON.stringify(extra));
  assert.equal(containerRun(null), null);
});
