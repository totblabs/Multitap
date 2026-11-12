import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { setTimeout as sleep } from 'node:timers/promises';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { desktopPreviewProtocol, thumbnailCaptureProtocol, previewMaxBytes } from '../src/desktop-previews.mjs';

// desktop-preview-v1 / thumbnail-capture-v1 through the actual server: a
// simulated Host channel answers capture requests; real accounts, assignments,
// privacy epochs and the administrator switch. No screen is read anywhere.
const machineUuid = 'b1234567-89ab-cdef-8123-0123456789ab';
const unlocked = '1234567890abcdef0000000000000001', relocked = '1234567890abcdef0000000000000002';
const jpeg = Buffer.from([0xff, 0xd8, 0xff, 0xe0, 1, 2, 3, 4, 0xff, 0xd9]);
const caps = (privacy = { epoch: unlocked, allowed: true }, capture = true) => ({
  service: true, interactiveSession: privacy.allowed, streaming: true, virtualDisplayReady: true,
  monitors: [{ id: '0', width: 1920, height: 1080, primary: true }],
  thumbnailPrivacy: { protocol: 'thumbnail-privacy-v1', ...privacy },
  ...(capture ? { thumbnailCaptureProtocol } : {}),
});
const packet = (id, bytes = jpeg) => Buffer.concat([Buffer.from('TVP1'), Buffer.from(id, 'latin1'), bytes]);

async function fixture(t, { capture = true, previewOptions = {}, authorizationMs = 20 } = {}) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const admin = store.addUser({ name: 'preview-admin', role: 'admin', passwordHash: 'fixture-no-login' });
  store.addUser({ name: 'preview-user', role: 'user', passwordHash: 'fixture-no-login' });
  store.addUser({ name: 'preview-other', role: 'user', passwordHash: 'fixture-no-login' });
  const vms = [{ vmid: 149, node: 'fixture', name: 'VM', status: 'running', machineUuid }];
  store.syncProxmox(vms, admin);
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://fixture.invalid', tokenId: 'fixture@pve!preview', secret: 'fixture-not-real' }), admin);
  store.replaceVmAssignments('preview-user', ['pve-149'], admin);
  const tokens = Object.fromEntries(['preview-admin', 'preview-user', 'preview-other'].map(name => {
    const token = newToken(); store.createSession(store.findUser(name), tokenHash(token), Date.now(), 3600000, name === 'preview-admin' ? 'admin' : 'client'); return [name, token];
  }));
  const code = newToken(), hostToken = newToken();
  store.issueEnrollment('pve-149', machineUuid, tokenHash(code), 'fixture');
  store.consumeEnrollment(tokenHash(code), machineUuid, { hostname: 'fixture-149', version: '0.2.146', tokenHash: tokenHash(hostToken) });
  const origin = 'https://preview-fixture.invalid';
  const server = await buildServer({ store, vault, publicOrigin: origin, proxmox: { inventory: async () => vms }, relayOptions: { authorizationMs, heartbeatMs: 1000, previewOptions }, logger: { error() {} } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const base = 'http://127.0.0.1:' + server.address().port;
  // The simulated Host: answers each capture request with `reply(requestId)`.
  const requests = [];
  let reply = id => [packet(id), { type: 'host.thumbnail.result', requestId: id, state: 'captured', epoch: unlocked, bytes: jpeg.length, width: 480, height: 270 }];
  let ws;
  const connect = async (capabilities = caps(undefined, capture)) => {
    ws = new WebSocket(base.replace('http', 'ws') + '/api/v1/hosts/channel', { headers: { Authorization: 'Host ' + hostToken, 'X-Multitap-Host-Protocol': 'capabilities-v1' }, perMessageDeflate: false });
    await once(ws, 'open');
    const acked = new Promise(resolve => ws.on('message', (data, binary) => {
      if (binary) return;
      const value = JSON.parse(data.toString());
      if (value.type === 'host.capabilities.ack') resolve(value);
      if (value.type === 'host.thumbnail.request') {
        requests.push(value);
        for (const message of reply(value.requestId)) ws.send(Buffer.isBuffer(message) ? message : JSON.stringify(message), { binary: Buffer.isBuffer(message) });
      }
    }));
    ws.send(JSON.stringify({ type: 'host.capabilities', capabilities }));
    await acked;
  };
  await connect();
  t.after(async () => { ws?.close(); await new Promise(resolve => server.close(resolve)); store.close(); });
  const call = async (path, { method = 'GET', name = 'preview-user', data, admin: adminCookie = false } = {}) => {
    const token = tokens[name];
    const response = await fetch(base + '/api/v1' + path, { method, headers: {
      Origin: origin,
      ...(adminCookie ? { Cookie: '__Secure-multitap-admin=' + token, 'X-CSRF-Token': vault.csrf(token) } : { Authorization: 'Bearer ' + token }),
      ...(data ? { 'Content-Type': 'application/json' } : {}),
    }, body: data ? JSON.stringify(data) : undefined });
    return { status: response.status, body: await response.json() };
  };
  return { store, server, call, requests, setReply: value => { reply = value; }, send: value => ws.send(JSON.stringify(value)), sendBinary: value => ws.send(value, { binary: true }), connect, closeHost: async () => { const closed = once(ws, 'close'); ws.close(); await closed; } };
}
const preview = f => f.call('/desktops/pve-149/preview');

test('an assigned account sees the picture without connecting; the Host is asked once per 30 s', async t => {
  const f = await fixture(t);
  const list = await f.call('/desktops');
  assert.equal(list.body.desktops[0].previewProtocol, desktopPreviewProtocol);
  assert.equal(list.body.desktops[0].host.capabilities.thumbnailCaptureProtocol, undefined, 'the Host protocol stays between broker and Host');
  assert.equal(f.requests.length, 0, 'listing alone captures nothing');
  const first = await preview(f);
  assert.equal(first.status, 200);
  assert.equal(first.body.protocol, desktopPreviewProtocol);
  assert.equal(first.body.state, 'captured');
  assert.deepEqual(Buffer.from(first.body.image, 'base64'), jpeg);
  assert.ok(Math.abs(Date.parse(first.body.takenAt) - Date.now()) < 5000);
  assert.ok(Number.isInteger(first.body.ageMs) && first.body.ageMs >= 0 && first.body.ageMs < 5000);
  assert.deepEqual(Object.keys(first.body).sort(), ['ageMs', 'image', 'protocol', 'state', 'takenAt']);
  assert.equal(f.requests.length, 1);
  assert.match(f.requests[0].requestId, /^[0-9a-f]{32}$/);
  const again = await preview(f);
  assert.equal(again.body.state, 'captured'); assert.equal(again.body.takenAt, first.body.takenAt);
  assert.equal(f.requests.length, 1, 'a second look within 30 s reuses the picture');
});

test('an account the VM is not assigned to gets nothing and triggers nothing', async t => {
  const f = await fixture(t);
  const other = await f.call('/desktops/pve-149/preview', { name: 'preview-other' });
  assert.equal(other.status, 404); assert.equal(other.body.error.code, 'desktop_not_found');
  assert.equal((await f.call('/desktops/pve-404/preview')).status, 404);
  assert.equal((await f.call('/desktops/pve-149/preview', { name: 'preview-admin' })).status, 401, 'an administrator console token is not a client session');
  assert.equal(f.requests.length, 0);
  assert.equal((await f.call('/desktops/pve-149/preview', { method: 'POST', data: {} })).status, 405);
});

test('locked and sign-in screens are states, never pictures; a lock drops the kept picture', async t => {
  const f = await fixture(t, { previewOptions: { intervalMs: 50, retryMs: 50 } });
  assert.equal((await preview(f)).body.state, 'captured');
  f.setReply(id => [{ type: 'host.thumbnail.result', requestId: id, state: 'locked' }]);
  await sleep(60);
  const locked = await preview(f);
  assert.deepEqual(locked.body, { protocol: desktopPreviewProtocol, state: 'locked' });
  f.setReply(id => [{ type: 'host.thumbnail.result', requestId: id, state: 'signin' }]);
  await sleep(60);
  assert.deepEqual((await preview(f)).body, { protocol: desktopPreviewProtocol, state: 'signin' });
  // A Host that sends a picture with a locked answer: the picture is not kept.
  f.setReply(id => [packet(id), { type: 'host.thumbnail.result', requestId: id, state: 'locked' }]);
  await sleep(60);
  assert.equal((await preview(f)).body.image, undefined);
});

test('a moved privacy epoch (lock, sign-out, user switch) stops serving the picture at once', async t => {
  const f = await fixture(t);
  assert.equal((await preview(f)).body.state, 'captured');
  f.setReply(id => [{ type: 'host.thumbnail.result', requestId: id, state: 'locked' }]);
  f.send({ type: 'host.capabilities', capabilities: caps({ epoch: relocked, allowed: false }) });
  await sleep(50);
  const value = await preview(f);
  assert.equal(value.body.image, undefined);
  assert.equal(value.body.state, 'locked');
});

test('unlocking (a new allowed epoch) asks the Host again within seconds, not after 30 s', async t => {
  const f = await fixture(t, { previewOptions: { retryMs: 50 } });
  const third = '1234567890abcdef0000000000000003';
  f.setReply(id => [{ type: 'host.thumbnail.result', requestId: id, state: 'locked' }]);
  f.send({ type: 'host.capabilities', capabilities: caps({ epoch: relocked, allowed: false }) });
  await sleep(50);
  assert.equal((await preview(f)).body.state, 'locked');
  f.setReply(id => [packet(id), { type: 'host.thumbnail.result', requestId: id, state: 'captured', epoch: third, bytes: jpeg.length, width: 480, height: 270 }]);
  f.send({ type: 'host.capabilities', capabilities: caps({ epoch: third, allowed: true }) });
  await sleep(60);
  const value = await preview(f);
  assert.equal(value.body.state, 'captured');
  assert.equal(f.requests.length, 2);
});

test('a dark output keeps the last picture with its age; without one it says asleep', async t => {
  const f = await fixture(t, { previewOptions: { intervalMs: 50 } });
  f.setReply(id => [{ type: 'host.thumbnail.result', requestId: id, state: 'blank', epoch: unlocked }]);
  assert.deepEqual((await preview(f)).body, { protocol: desktopPreviewProtocol, state: 'asleep' });
  f.setReply(id => [packet(id), { type: 'host.thumbnail.result', requestId: id, state: 'captured', epoch: unlocked, bytes: jpeg.length, width: 480, height: 270 }]);
  await sleep(60);
  const taken = await preview(f);
  assert.equal(taken.body.state, 'captured');
  f.setReply(id => [{ type: 'host.thumbnail.result', requestId: id, state: 'blank', epoch: unlocked }]);
  await sleep(60);
  const kept = await preview(f);
  assert.equal(kept.body.state, 'captured'); assert.equal(kept.body.takenAt, taken.body.takenAt);
  assert.equal(f.requests.length, 3);
});

test('malformed or mismatched pictures are never served', async t => {
  for (const reply of [
    id => [packet('f'.repeat(32)), { type: 'host.thumbnail.result', requestId: id, state: 'captured', epoch: unlocked, bytes: jpeg.length, width: 480, height: 270 }],
    id => [packet(id, Buffer.from([1, 2, 3, 4, 5])), { type: 'host.thumbnail.result', requestId: id, state: 'captured', epoch: unlocked, bytes: 5, width: 480, height: 270 }],
    id => [packet(id, Buffer.concat([Buffer.from([0xff, 0xd8]), Buffer.alloc(previewMaxBytes), Buffer.from([0xff, 0xd9])])), { type: 'host.thumbnail.result', requestId: id, state: 'captured', epoch: unlocked, bytes: previewMaxBytes + 4, width: 480, height: 270 }],
    id => [packet(id), { type: 'host.thumbnail.result', requestId: id, state: 'captured', epoch: unlocked, bytes: jpeg.length + 1, width: 480, height: 270 }],
    id => [packet(id), { type: 'host.thumbnail.result', requestId: id, state: 'captured', epoch: 'nope', bytes: jpeg.length, width: 480, height: 270 }],
    id => [packet(id), { type: 'host.thumbnail.result', requestId: id, state: 'captured', epoch: unlocked, bytes: jpeg.length, width: 4800, height: 270 }],
    id => [{ type: 'host.thumbnail.result', requestId: id, state: 'captured', epoch: unlocked, bytes: jpeg.length, width: 480, height: 270 }],
    id => [packet(id), { type: 'host.thumbnail.result', requestId: id, state: 'captured', epoch: relocked, bytes: jpeg.length, width: 480, height: 270 }],
  ]) {
    const f = await fixture(t, { previewOptions: { waitMs: 500 } });
    f.setReply(reply);
    const value = await preview(f);
    assert.equal(value.status, 200);
    assert.equal(value.body.image, undefined, reply.toString());
    assert.equal(value.body.state, 'unavailable');
  }
});

test('a Host that does not answer leaves the card as today', async t => {
  const f = await fixture(t, { previewOptions: { waitMs: 100 } });
  f.setReply(() => []);
  const began = Date.now();
  assert.deepEqual((await preview(f)).body, { protocol: desktopPreviewProtocol, state: 'unavailable' });
  assert.ok(Date.now() - began < 2000, 'the client is not held past the wait');
});

test('an older Host (no capture protocol) keeps today\'s behaviour: no flag and nothing asked', async t => {
  const f = await fixture(t, { capture: false });
  const list = await f.call('/desktops');
  assert.equal(list.body.desktops[0].previewProtocol, undefined);
  assert.deepEqual((await preview(f)).body, { protocol: desktopPreviewProtocol, state: 'unavailable' });
  assert.equal(f.requests.length, 0);
});

test('a Host that disconnects takes its picture with it', async t => {
  const f = await fixture(t);
  assert.equal((await preview(f)).body.state, 'captured');
  await f.closeHost(); await sleep(50);
  assert.equal((await f.call('/desktops')).body.desktops[0].previewProtocol, undefined);
  assert.equal((await preview(f)).body.state, 'unavailable');
  await f.connect(); await sleep(20);
  const fresh = await preview(f);
  assert.equal(fresh.body.state, 'captured');
  assert.equal(f.requests.length, 2, 'the reconnected Host is asked again, never served the old picture');
});

test('the administrator switch (미리보기 화면 끄기) turns previews off, forgets pictures and is on by default', async t => {
  const f = await fixture(t);
  const inventory = await f.call('/admin/inventory', { name: 'preview-admin', admin: true });
  assert.deepEqual(inventory.body.desktopPreviews, { enabled: true });
  assert.equal((await preview(f)).body.state, 'captured');
  assert.equal((await f.call('/admin/desktop-previews', { method: 'PUT', name: 'preview-admin', admin: true, data: { enabled: 'no' } })).status, 400);
  assert.equal((await f.call('/admin/desktop-previews', { method: 'PUT', name: 'preview-user', data: { enabled: false } })).status, 401, 'not for client accounts');
  const off = await f.call('/admin/desktop-previews', { method: 'PUT', name: 'preview-admin', admin: true, data: { enabled: false } });
  assert.deepEqual(off.body, { enabled: false });
  assert.equal((await f.call('/desktops')).body.desktops[0].previewProtocol, undefined);
  const refused = await preview(f);
  assert.equal(refused.status, 409); assert.equal(refused.body.error.code, 'desktop_previews_disabled');
  assert.deepEqual((await f.call('/admin/inventory', { name: 'preview-admin', admin: true })).body.desktopPreviews, { enabled: false });
  await f.call('/admin/desktop-previews', { method: 'PUT', name: 'preview-admin', admin: true, data: { enabled: true } });
  assert.equal((await preview(f)).body.state, 'captured');
  assert.equal(f.requests.length, 2, 'turning it back on never serves a picture kept from before');
});

test('the picture never reaches the administrator console, the audit log or the database', async t => {
  const f = await fixture(t);
  const taken = await preview(f);
  const encoded = taken.body.image;
  assert.ok(encoded);
  const inventory = await f.call('/admin/inventory', { name: 'preview-admin', admin: true });
  const hosts = await f.call('/admin/hosts', { name: 'preview-admin', admin: true });
  for (const value of [inventory.body, hosts.body]) assert.equal(JSON.stringify(value).includes(encoded), false);
  const tables = f.store.db.prepare("SELECT name FROM sqlite_master WHERE type='table'").all().map(row => row.name);
  for (const table of tables) for (const row of f.store.db.prepare(`SELECT * FROM "${table}"`).all()) {
    const text = JSON.stringify(row, (_, value) => Buffer.isBuffer(value) || value instanceof Uint8Array ? Buffer.from(value).toString('base64') : value);
    assert.equal(text.includes(encoded), false, table);
  }
});

test('while another account\'s session runs on the desktop, nothing new is captured for this viewer and only an older picture may show', async t => {
  // The relay's periodic check is moved out of the way: the other account's session below is a stand-in.
  const f = await fixture(t, { previewOptions: { intervalMs: 50, retryMs: 50 }, authorizationMs: 3600000 });
  const other = f.store.findUser('preview-other');
  const before = await preview(f); assert.equal(before.body.state, 'captured'); assert.equal(f.requests.length, 1);
  const sessions = f.server.relay.sessions, id = 'other-account-session';
  // The stand-in session must be gone before the server closes, also when an
  // assertion below fails (it used to hang the run instead of failing it).
  try {
    // Started a minute from now, so every picture of this test is from before it;
    // a capture already under way when it is set may still finish (120 ms).
    sessions.set(id, { id, desktopId: 'pve-149', userId: other.id, createdAt: Date.now() + 60000 });
    assert.equal(f.server.relay.othersSince('pve-149', other.id), null, 'its own session is no other account\'s');
    await new Promise(resolve => setTimeout(resolve, 120));
    const kept = await preview(f), counted = f.requests.length;
    await new Promise(resolve => setTimeout(resolve, 80));
    const during = await preview(f);
    assert.equal(f.requests.length, counted, 'no capture while the other session lasts');
    assert.equal(during.body.state, 'captured'); assert.equal(during.body.takenAt, kept.body.takenAt, 'only the picture from before it began');
    // A session that began before the kept picture was taken: no picture at all.
    sessions.set(id, { id, desktopId: 'pve-149', userId: other.id, createdAt: Date.parse(kept.body.takenAt) - 1000 });
    const older = await preview(f);
    assert.deepEqual(older.body, { protocol: desktopPreviewProtocol, state: 'unavailable' }); assert.equal(f.requests.length, counted);
    // Once it ends, previews resume.
    sessions.delete(id);
    const after = await preview(f);
    assert.ok(f.requests.length > counted); assert.equal(after.body.state, 'captured'); assert.notEqual(after.body.takenAt, kept.body.takenAt);
  } finally { sessions.delete(id); }
});
