// Limits that bound what one party can take from everybody else: sign-in attempts and in-app Console views.
import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { Store } from '../src/store.mjs';
import { buildServer } from '../src/http.mjs';
import { ConsoleWebviews } from '../src/console-webviews.mjs';
import { hashPassword, newToken, tokenHash } from '../src/security.mjs';

const password = 'Fixture-password-not-real-9!';
const origin = 'https://webview-limits.invalid';
const machine = id => '12345678-1234-1234-1234-' + String(id).padStart(12, '0');

async function server(t, options) {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-test-'));
  const store = new Store(join(directory, 'broker.sqlite'));
  store.addUser({ name: 'alice', passwordHash: await hashPassword(password) });
  const built = await buildServer({ store, publicOrigin: 'https://limits-tests.example', logger: { error: value => assert.fail(value) }, trustedProxies: '127.0.0.1', ...options });
  await new Promise(resolve => built.listen(0, '127.0.0.1', resolve));
  const url = `http://127.0.0.1:${built.address().port}`;
  t.after(async () => { await new Promise(resolve => built.close(resolve)); store.close(); await rm(directory, { recursive: true }); });
  const login = async (from, secret = password) => (await fetch(url + '/api/v1/auth/login', { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Forwarded-For': from }, body: JSON.stringify({ username: 'alice', password: secret }) })).status;
  return { login };
}

test('Failed sign-ins from one address lock that address out, not the account', async t => {
  const f = await server(t, { loginLimit: 2 });
  assert.equal(await f.login('203.0.113.7', 'wrong-fixture'), 401);
  assert.equal(await f.login('203.0.113.7', 'wrong-fixture'), 401);
  assert.equal(await f.login('203.0.113.7'), 429, 'the guessing address is refused, even with the right password');
  assert.equal(await f.login('198.51.100.1'), 200, 'the owner signs in from another address');
});

test('The account still has a budget across all addresses together', async t => {
  const f = await server(t, { loginLimit: 2 });
  // 2 per address, 16 (8 x 2) for the account in all.
  for (let i = 0; i < 8; i++) for (let j = 0; j < 2; j++) assert.equal(await f.login(`203.0.113.${10 + i}`, 'wrong-fixture'), 401);
  assert.equal(await f.login('198.51.100.1'), 429);
});

test('One sign-in keeps at most 4 Console views and one account 16; others still open theirs', t => {
  const store = new Store(':memory:'); t.after(() => store.close());
  store.syncProxmox([{ vmid: 149, node: 'fixture', name: 'VM', status: 'running', machineUuid: machine(149) }], 'fixture');
  for (const name of ['alice', 'other']) { store.addUser({ name, passwordHash: 'synthetic-only' }); store.assign(name, 'pve-149'); }
  const signIn = name => { const hash = tokenHash(newToken()); store.createSession(store.findUser(name), hash, Date.now(), 3600000, 'client'); return { hash, user: store.authenticate(hash) }; };
  const manager = new ConsoleWebviews(store);
  const open = ({ hash, user }) => manager.consume(manager.create(user, hash, 'pve-149', origin).code, 'pve-149', origin);
  const one = signIn('alice'), views = [];
  for (let i = 0; i < 6; i++) views.push(open(one));
  assert.equal([...manager.children.values()].filter(entry => entry.parentHash === one.hash).length, 4);
  assert.equal(manager.authenticate(views[0].hash), null, 'the oldest view went');
  assert.ok(manager.authenticate(views[5].hash), 'the newest view works');
  for (let s = 0; s < 4; s++) { const more = signIn('alice'); for (let i = 0; i < 4; i++) open(more); }
  const aliceViews = () => [...manager.children.values()].filter(entry => entry.userId === one.user.id).length;
  assert.equal(aliceViews(), 16);
  const other = signIn('other');
  assert.ok(manager.authenticate(open(other).hash), 'another account still opens Console');
  assert.equal(aliceViews(), 16);
});
