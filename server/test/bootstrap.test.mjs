import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, dirname, resolve, basename } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { Store } from '../src/store.mjs';
import { hashPassword, verifyPassword } from '../src/security.mjs';

test('Fresh bootstrap creates restricted admin/admin and repeated deployments preserve chosen credentials', async t => {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-bootstrap-'));
  t.after(async () => { assert.equal(dirname(resolve(directory)), resolve(tmpdir())); assert.ok(basename(directory).startsWith('multitap-bootstrap-')); await rm(directory, { recursive: true }); });
  const script = fileURLToPath(new URL('../src/bootstrap.mjs', import.meta.url));
  const run = mode => spawnSync(process.execPath, [script, ...(mode ? [mode] : [])], { env: { ...process.env, MULTITAP_DATA_DIR: directory }, encoding: 'utf8' });
  assert.equal(run().status, 0);
  const store = new Store(join(directory, 'broker.sqlite'));
  try {
    const initial = store.findUser('admin'); assert.equal(initial.must_change_credentials, 1); assert.equal(await verifyPassword('admin', initial.password_hash), true);
    store.completeInitialSetup(initial.id, 'owner', await hashPassword('Owner-test-password-71!'));
    assert.equal(run().status, 0);
    assert.equal(store.findUser('owner').must_change_credentials, 0);
    assert.equal(await verifyPassword('Owner-test-password-71!', store.findUser('owner').password_hash), true);
    assert.equal(store.findUser('admin'), undefined);
    assert.notEqual(run('reset-initial').status, 0);
    assert.equal(store.findUser('owner').must_change_credentials, 0);
  } finally { store.close(); }
});

test('reset-initial returns an untouched single administrator of any name to the first-login setup, nothing else', async t => {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-bootstrap-'));
  t.after(async () => { assert.equal(dirname(resolve(directory)), resolve(tmpdir())); assert.ok(basename(directory).startsWith('multitap-bootstrap-')); await rm(directory, { recursive: true }); });
  const script = fileURLToPath(new URL('../src/bootstrap.mjs', import.meta.url));
  const run = mode => spawnSync(process.execPath, [script, ...(mode ? [mode] : [])], { env: { ...process.env, MULTITAP_DATA_DIR: directory }, encoding: 'utf8' });
  const { writeFile } = await import('node:fs/promises');
  // A server whose recorded initial account (any name) still has its recorded password.
  const store = new Store(join(directory, 'broker.sqlite'));
  try { store.addUser({ name: 'operator-7', role: 'admin', passwordHash: await hashPassword('Recorded-initial-password-29!') }); } finally { store.close(); }
  await writeFile(join(directory, 'initial-account.json'), JSON.stringify({ username: 'operator-7', password: 'Recorded-initial-password-29!' }));
  const reset = run('reset-initial'); assert.equal(reset.status, 0, reset.stderr);
  const after = new Store(join(directory, 'broker.sqlite'));
  try {
    assert.equal(after.findUser('operator-7'), undefined);
    const admin = after.findUser('admin'); assert.equal(admin.must_change_credentials, 1); assert.equal(await verifyPassword('admin', admin.password_hash), true);
  } finally { after.close(); }
  // A changed password (the account was used) is never reset.
  const used = new Store(join(directory, 'broker.sqlite'));
  try { used.completeInitialSetup(used.findUser('admin').id, 'operator-8', await hashPassword('Chosen-password-30!')); } finally { used.close(); }
  await writeFile(join(directory, 'initial-account.json'), JSON.stringify({ username: 'operator-8', password: 'Recorded-initial-password-29!' }));
  assert.notEqual(run('reset-initial').status, 0);
});
