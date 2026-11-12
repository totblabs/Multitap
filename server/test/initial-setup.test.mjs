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
import { setupCodeSetting } from '../src/initial-setup.mjs';
import { normalizeSetupCode } from '../src/verification-codes.mjs';

// One-time first-administrator setup code: admin/admin alone does
// not complete the first setup of a fresh server; the code is printed only
// to the server's own log and stored as a hash.
const origin = 'https://setup-code.example';
const codePattern = /^[0-9A-HJKMNP-TV-Z]{4}-[0-9A-HJKMNP-TV-Z]{4}-[0-9A-HJKMNP-TV-Z]{4}$/;
const replacement = 'Setup-code-tests-password-71!';

async function fresh(t, { commissioned = false, path = ':memory:', store: existing } = {}) {
  const store = existing ?? new Store(path), vault = new Vault(randomBytes(32)), announced = [];
  if (!existing) {
    const id = store.addUser({ name: 'admin', role: 'admin', passwordHash: commissioned ? await hashPassword(replacement) : await initialPasswordHash() });
    if (!commissioned) store.db.prepare('UPDATE users SET must_change_credentials=1 WHERE id=?').run(id);
  }
  const server = await buildServer({ store, vault, publicOrigin: origin, logger: { error: value => assert.fail(value) }, initialSetupOptions: { announce: code => announced.push(code) } });
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
  return { store, announced, call, login, setup, close };
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
