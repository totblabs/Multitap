import test from 'node:test';
import assert from 'node:assert/strict';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { BrowserConsole } from '../src/browser-console.mjs';
import { ConsoleRfb } from '../src/console-rfb.mjs';
import { newToken, tokenHash } from '../src/security.mjs';

async function fixture(t) {
  const store = new Store(':memory:'), vault = new Vault(Buffer.alloc(32, 21)); let clock = Date.now();
  const userId = store.addUser({ name: 'console-fixture', passwordHash: 'synthetic-unused-hash' });
  const parentHash = tokenHash(newToken()), user = store.findUser('console-fixture');
  store.createSession(user, parentHash, clock, 3600000, 'client');
  const origin = 'https://console-activity.fixture.invalid';
  store.syncProxmox([{ vmid: 174, node: 'fixture', name: 'Fixture VM', status: 'running', machineUuid: '11223344-1234-1234-1234-112233445566' }], userId);
  store.assign(user.username, 'pve-174');
  const server = await buildServer({ store, vault, publicOrigin: origin,
    sessionAccessOptions: { now: () => clock, policy: { idleTimeoutMs: 60000, warningMs: 10000 } }, logger: { error: error => assert.fail(error) } });
  // Use the production principal manager and bind its actual callbacks to the
  // production HTTP activity closure. No WebView or peer supplies parentHash.
  const consoleOwner = new BrowserConsole({ store, vault, relay: server.relay, webviewOptions: { now: () => clock } });
  const grant = consoleOwner.webviews.create(store.authenticate(parentHash), parentHash, 'pve-174', origin);
  const child = consoleOwner.webviews.consume(grant.code, 'pve-174', origin);
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  const value = { tokenHash: child.hash, userId, desktopId: 'pve-174', origin };
  return { store, server, consoleOwner, parentHash, child, value, advance: ms => { clock += ms; }, now: () => clock };
}

function rfbInput(callback, message) {
  const writes = [], banner = Buffer.from('RFB 003.008\n'), init = Buffer.alloc(24);
  init.writeUInt16BE(4, 0); init.writeUInt16BE(3, 2);
  Buffer.from([32,24,0,1,0,255,0,255,0,255,16,8,0,0,0,0]).copy(init, 4);
  const rfb = new ConsoleRfb({ password: 'synthetic-vnc', profile: 'novnc', acceptedInput: callback,
    upstream: bytes => writes.push(Buffer.from(bytes)), downstream: () => {}, ready: () => {} });
  try {
    rfb.upstream(Buffer.concat([banner, Buffer.from([1,1]), Buffer.alloc(4), init]));
    rfb.downstream(Buffer.concat([banner, Buffer.from([1,1])])); assert.equal(rfb.clientReady, true);
    const count = writes.length; rfb.downstream(message); assert.equal(writes.length, count + 1); assert.deepEqual(writes.at(-1), message);
  } finally { rfb.close(); }
}

test('scoped derived console key and pointer input forward and update only the real parent activity', async t => {
  const f = await fixture(t);
  const frame = Buffer.from([3,1,0,0,0,0,0,4,0,3]);
  f.advance(20000); rfbInput(() => f.server.relay.consoleInputAccepted(f.value), frame);
  assert.equal(f.store.db.prepare('SELECT last_input_at FROM session_activity WHERE token_hash=?').get(f.parentHash).last_input_at, null);
  for (const input of [Buffer.from([4,1,0,0,0,0,0,97]), Buffer.from([5,0,0,1,0,1])]) {
    f.advance(10000); rfbInput(() => f.server.relay.consoleInputAccepted(f.value), input);
    assert.equal(f.store.db.prepare('SELECT last_input_at FROM session_activity WHERE token_hash=?').get(f.parentHash).last_input_at, f.now());
  }
  assert.equal(f.store.db.prepare('SELECT 1 FROM sessions WHERE token_hash=?').get(f.child.hash), undefined);
  assert.equal(f.store.db.prepare('SELECT 1 FROM session_activity WHERE token_hash=?').get(f.child.hash), undefined);
});

test('console activity rejects a foreign origin, desktop, account, removed assignment and deleted child', async t => {
  const f = await fixture(t), accept = value => f.server.relay.consoleInputAccepted(value);
  for (const changed of [{ origin: 'https://foreign.fixture.invalid' }, { desktopId: 'pve-175' }, { userId: 'foreign-user' }, { tokenHash: tokenHash(newToken()) }])
    assert.equal(accept({ ...f.value, ...changed }), false);
  f.store.db.prepare('DELETE FROM assignments WHERE user_id=?').run(f.value.userId);
  assert.equal(accept(f.value), false);
  f.store.assign('console-fixture', 'pve-174'); assert.equal(accept(f.value), false, 'a revoked child stays revoked when an assignment returns');
  const grant = f.consoleOwner.webviews.create(f.store.authenticate(f.parentHash), f.parentHash, 'pve-174', f.value.origin);
  const child = f.consoleOwner.webviews.consume(grant.code, 'pve-174', f.value.origin), fresh = { ...f.value, tokenHash: child.hash };
  assert.equal(accept(fresh), true);
  f.consoleOwner.webviews.logout(child.hash); assert.equal(accept(fresh), false);
  assert.ok(f.store.authenticate(f.parentHash));
});

test('derived console input after the parent idle deadline cannot revive or renew it', async t => {
  const f = await fixture(t); f.advance(60001);
  const input = Buffer.from([4,1,0,0,0,0,0,97]);
  assert.throws(() => rfbInput(() => f.server.relay.consoleInputAccepted(f.value), input), error => error.code === 'authorization_revoked');
  assert.equal(f.store.authenticate(f.parentHash), undefined);
  assert.equal(f.store.sessionRevocationReason(f.parentHash, f.now()), 'session_idle_timeout');
});
