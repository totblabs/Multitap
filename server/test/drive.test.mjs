import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { devicePolicyKeys } from '../src/device-policy.mjs';
import { driveFsCapability, requestedDriveFs, driveCapability, requestedDrive, negotiateDrive, safeDriveName, safeDrivePath, driveLimits } from '../src/drive.mjs';
import { safeClientDiagnosticReport } from '../src/client-diagnostics.mjs';

// Streaming drive redirection (drive-v1; docs/drive-redirection.md): exact
// negotiation behind the per-assignment `drives` permission, a strictly checked
// relay pair (shares, paths, ids, in-flight bound, binary read/write), broker
// refusals of unknown/read-only shares, revocation that ends only the drive
// pair, and released peers that keep today's behaviour. Real broker and
// WebSockets, fake Host/client.
const origin = 'https://drive-fixture.example';
const password = 'Drive-fixture-password-only!';
const machineUuid = '12345678-1234-1234-1234-123456789ad7';
const releasedHost = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true };
const driveHost = { ...releasedHost, driveProtocol: 'drive-v1' };
const deny = Object.fromEntries(devicePolicyKeys.map(key => [key, false]));
const streamInfo = { type: 'stream.info', codec: 'h264', width: 1920, height: 1080, sourceWidth: 1920, sourceHeight: 1080, fpsTarget: 60, audio: { sampleRate: 48000, channels: 2, format: 's16le', available: false } };
const entry = (name, extra = {}) => ({ name, dir: false, size: 3, modified: 1761300000000, created: 1761200000000, readOnly: false, ...extra });

async function fixture(t, relayOptions = {}) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const hash = await hashPassword(password);
  store.addUser({ name: 'admin', passwordHash: hash, role: 'admin' });
  store.addUser({ name: 'alice', passwordHash: hash, role: 'user' });
  store.addUser({ name: 'bob', passwordHash: hash, role: 'user' });
  store.syncProxmox([{ vmid: 187, node: 'pve', name: 'VM 187', status: 'running' }], 'fixture');
  store.assign('alice', 'pve-187'); store.assign('bob', 'pve-187');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve-fixture.example', tokenId: 'fixture@pve!broker', secret: 'only-fixture-secret' }), 'fixture');
  const proxmox = { machineIdentity: async () => ({ machineUuid }) };
  const server = await buildServer({ store, vault, proxmox, publicOrigin: origin, relayOptions: { authorizationMs: 20, heartbeatMs: 200, ...relayOptions }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const base = `http://127.0.0.1:${server.address().port}`;
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  async function call(path, { method = 'GET', data, token, cookie, csrf } = {}) {
    const response = await fetch(base + '/api/v1' + path, { method,
      headers: { ...(token ? { Authorization: 'Bearer ' + token } : {}), ...(cookie ? { Cookie: cookie } : {}), ...(method !== 'GET' ? { 'Content-Type': 'application/json', Origin: origin, ...(csrf ? { 'X-CSRF-Token': csrf } : {}) } : {}) },
      body: data === undefined ? undefined : JSON.stringify(data) });
    return { status: response.status, body: await response.json() };
  }
  const adminLogin = await fetch(base + '/api/v1/admin/login', { method: 'POST', headers: { 'Content-Type': 'application/json', Origin: origin }, body: JSON.stringify({ username: 'admin', password }) });
  const admin = { cookie: adminLogin.headers.get('set-cookie').split(';')[0], csrf: (await adminLogin.json()).csrfToken };
  const alice = (await call('/auth/login', { method: 'POST', data: { username: 'alice', password } })).body.sessionToken;
  const bob = (await call('/auth/login', { method: 'POST', data: { username: 'bob', password } })).body.sessionToken;
  const code = (await call('/admin/desktops/pve-187/enrollment', { ...admin, method: 'POST', data: {} })).body.code;
  const wsUrl = path => base.replace('http:', 'ws:') + path;
  const local = url => wsUrl(new URL(url).pathname);
  let hostToken;
  async function host(capabilities) {
    const enrolled = await call('/hosts/enroll', { method: 'POST', data: { code, hostname: 'Fixture VM', machineUuid, version: '0.2.70' } });
    assert.equal(enrolled.status, 201);
    hostToken = enrolled.body.hostToken;
    const ws = await socket(wsUrl('/api/v1/hosts/channel'), 'Host ' + hostToken);
    ws.json({ type: 'host.capabilities', capabilities });
    assert.equal((await ws.take(value => value.type === 'host.capabilities.ack')).accepted, true);
    return ws;
  }
  const allow = (user = 'alice', drives = true) => store.updateAssignmentPolicy(user, 'pve-187', { devicePolicy: { ...deny, drives } }, 'fixture');
  const connect = (extra, token = alice) => call('/desktops/pve-187/connect', { method: 'POST', token, data: { mode: 'stream', ...extra } });
  async function stream(hostSocket, extra = {}, token = alice) {
    const response = await connect(extra, token);
    assert.equal(response.status, 200, JSON.stringify(response.body));
    const controller = await socket(wsUrl('/api/v1/sessions/' + response.body.sessionId), 'Bearer ' + token);
    const start = await hostSocket.take(value => value.type === 'session.start');
    hostSocket.json({ ...streamInfo, ...(start.resizeProtocol ? { epoch: 0 } : {}) }); await controller.take(value => value.type === 'stream.info');
    return { response, controller, start, sessionId: response.body.sessionId };
  }
  // A granted stream with both drive sockets attached and ready.
  async function pair(hostSocket, { shares, notices = false, driveFs = false } = {}) {
    const session = await stream(hostSocket, { driveProtocol: 'drive-v1', ...(driveFs ? { driveFs: true } : {}) });
    const client = await socket(local(session.response.body.driveWebSocketUrl), 'Bearer ' + alice, notices ? { 'X-Multitap-Drive-Notices': '1' } : {});
    const hostDrive = await socket(local(session.start.driveUrl), 'Host ' + hostToken);
    const ready = [await client.take(value => value.type === 'drive.ready'), await hostDrive.take(value => value.type === 'drive.ready')];
    if (shares) { client.json({ type: 'drive.shares', shares }); await hostDrive.take(value => value.type === 'drive.shares'); }
    return { ...session, client, hostDrive, ready };
  }
  return { store, call, alice, bob, host, allow, connect, stream, pair, socket, wsUrl, local, hostToken: () => hostToken };
}
async function socket(url, authorization, extra = {}) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url, { headers: { Authorization: authorization, ...extra }, perMessageDeflate: false });
    ws.once('error', reject);
    ws.once('unexpected-response', (_, response) => reject(Object.assign(new Error('rejected'), { status: response.statusCode })));
    ws.once('open', () => {
      const queue = [], waiters = [];
      ws.on('message', (data, binary) => {
        const value = binary ? data : JSON.parse(data.toString('utf8'));
        const index = waiters.findIndex(waiter => waiter.predicate(value));
        if (index >= 0) { const waiter = waiters.splice(index, 1)[0]; clearTimeout(waiter.timer); waiter.resolve(value); } else queue.push(value);
      });
      ws.queue = queue;
      ws.take = (predicate = () => true, waitMs = 2000) => {
        const index = queue.findIndex(predicate);
        if (index >= 0) return Promise.resolve(queue.splice(index, 1)[0]);
        return new Promise((done, fail) => { const waiter = { predicate, resolve: done, timer: setTimeout(() => fail(new Error('message_timeout')), waitMs) }; waiters.push(waiter); });
      };
      ws.json = value => ws.send(JSON.stringify(value));
      resolve(ws);
    });
  });
}
const closed = ws => new Promise(resolve => { if (ws.readyState === WebSocket.CLOSED) resolve(); else ws.once('close', resolve); });
const code = fn => { try { fn(); return 'accepted'; } catch (error) { return error.code; } };
const driveError = value => value.type === 'drive.error';
const isBinary = value => Buffer.isBuffer(value);
const readData = (id, bytes) => Buffer.concat([Buffer.from([1]), Buffer.from(new Uint32Array([id]).buffer), Buffer.from(bytes)]);
function writeFrame(id, header, data) {
  const json = Buffer.from(JSON.stringify(header));
  const head = Buffer.alloc(7); head[0] = 2; head.writeUInt32LE(id, 1); head.writeUInt16LE(json.length, 5);
  return Buffer.concat([head, json, Buffer.from(data)]);
}
const rejected = async promise => { try { await promise; return 'open'; } catch (error) { return error.status ?? error.message; } };

test('Capability, request, negotiation, names and paths are exact', () => {
  assert.equal(driveCapability(driveHost), true);
  for (const value of [releasedHost, { ...releasedHost, driveProtocol: 'drive-v2' }, { ...releasedHost, driveProtocol: true }, { ...driveHost, interactiveSession: false }, { ...driveHost, service: false }, undefined, null])
    assert.equal(driveCapability(value), false);
  assert.equal(requestedDrive('stream', undefined), false);
  assert.equal(requestedDrive('stream', 'drive-v1'), true);
  for (const [mode, protocol] of [['rdp', 'drive-v1'], ['stream', 'drive-v2'], ['stream', ''], ['stream', 1], ['stream', null], ['stream', true]])
    assert.equal(code(() => requestedDrive(mode, protocol)), 'invalid_drive_protocol', `${mode} ${protocol}`);
  const allowed = { ...deny, drives: true };
  assert.equal(negotiateDrive(true, { capabilities: driveHost }, allowed), true);
  assert.equal(negotiateDrive(false, { capabilities: driveHost }, allowed), false, 'never granted unrequested');
  assert.equal(negotiateDrive(true, { capabilities: releasedHost }, allowed), false, 'released Host: none, not refused');
  assert.equal(negotiateDrive(true, { capabilities: driveHost }, deny), false, 'default denied');
  assert.equal(negotiateDrive(true, { capabilities: driveHost }, { ...deny, printers: true }), false, 'only the drives permission');

  for (const name of ['a', 'Documents', 'report.final.docx', '한글 폴더', 'con.txt.bak'.replace('con', 'cone'), 'x'.repeat(255), '😀', 'COM0', 'LPT10', 'CONSOLE', '.hidden'])
    assert.equal(safeDriveName(name), true, name);
  for (const name of ['', '.', '..', 'a.', 'a ', 'a/b', 'a\\b', 'C:', 'a:b', 'a*', 'a?', 'a"', 'a<', 'a>', 'a|', 'a\x00', 'a\x1f', 'x'.repeat(256), 'CON', 'con', 'Con.txt', 'NUL.tar.gz', 'COM1', 'lpt9.log', 'COM¹', 'CONIN$', 'conout$.x', 'AUX .txt', '\ud800', 'a\udc00b', 1, null])
    assert.equal(safeDriveName(name), false, JSON.stringify(name));
  for (const path of ['', 'a', 'a/b/c', '한글/파일.txt', Array(64).fill('d').join('/'), 'x'.repeat(255) + '/' + 'y'.repeat(255)])
    assert.equal(safeDrivePath(path), true, path);
  for (const path of ['/', '/a', 'a/', 'a//b', '..', 'a/..', 'a/../b', './a', 'a\\..\\b', 'C:/Windows', 'C:', '\\\\server\\share', 'a/CON', Array(65).fill('d').join('/'), Array(5).fill('y'.repeat(250)).join('/'), null, undefined, 3])
    assert.equal(safeDrivePath(path), false, JSON.stringify(path));
  assert.deepEqual(driveLimits, { maxShares: 8, chunkBytes: 262144, maxInFlight: 8, listPage: 256, maxPath: 1024, maxName: 255, maxDepth: 64 });
});

test('Granted only with the drives permission: inventory, connect response and session.start', async t => {
  const f = await fixture(t); const host = await f.host(driveHost);
  // Default denied: the capability is not shown to this user and not granted.
  let inventory = await f.call('/desktops', { token: f.alice });
  assert.equal(inventory.body.desktops[0].host.capabilities.driveProtocol, undefined);
  let session = await f.stream(host, { driveProtocol: 'drive-v1' });
  for (const key of ['driveProtocol', 'driveWebSocketUrl']) assert.equal(session.response.body[key], undefined, key);
  for (const key of ['driveProtocol', 'driveUrl']) assert.equal(session.start[key], undefined, key);
  session.controller.json({ type: 'session.stop' }); await host.take(value => value.type === 'session.stop'); await closed(session.controller);
  // Malformed requests are refused before any session.
  for (const driveProtocol of ['drive-v2', true]) {
    const refused = await f.connect({ driveProtocol });
    assert.equal(refused.status, 400); assert.equal(refused.body.error.code, 'invalid_drive_protocol');
  }
  // RDP is not offered: refused as such before its drive field is read.
  const rdp = await f.connect({ mode: 'rdp', driveProtocol: 'drive-v1' });
  assert.equal(rdp.status, 409); assert.equal(rdp.body.error.code, 'rdp_not_supported');
  // Allowed for alice only: her inventory shows it, bob's does not.
  f.allow('alice');
  inventory = await f.call('/desktops', { token: f.alice });
  assert.equal(inventory.body.desktops[0].host.capabilities.driveProtocol, 'drive-v1');
  assert.equal((await f.call('/desktops', { token: f.bob })).body.desktops[0].host.capabilities.driveProtocol, undefined);
  session = await f.stream(host, { driveProtocol: 'drive-v1' });
  assert.equal(session.response.body.driveProtocol, 'drive-v1');
  assert.equal(session.response.body.driveWebSocketUrl, 'wss://drive-fixture.example/api/v1/sessions/' + session.sessionId + '/drive');
  assert.equal(session.start.driveProtocol, 'drive-v1');
  assert.equal(session.start.driveUrl, 'wss://drive-fixture.example/api/v1/hosts/sessions/' + session.sessionId + '/drive');
  session.controller.json({ type: 'session.stop' }); await host.take(value => value.type === 'session.stop'); await closed(session.controller);
  // Never granted unrequested.
  session = await f.stream(host);
  assert.equal(session.response.body.driveProtocol, undefined); assert.equal(session.start.driveUrl, undefined);
});

test('WebClient unavailable in the VM: the reason is shown only to a user the drives permission allows', async t => {
  const f = await fixture(t);
  await f.host({ ...releasedHost, driveUnavailableReason: 'drive_webclient_unavailable' });
  // Not allowed: nothing about drives, neither the capability nor the reason.
  let caps = (await f.call('/desktops', { token: f.alice })).body.desktops[0].host.capabilities;
  assert.equal(caps.driveProtocol, undefined); assert.equal(caps.driveUnavailableReason, undefined);
  f.allow('alice');
  caps = (await f.call('/desktops', { token: f.alice })).body.desktops[0].host.capabilities;
  assert.equal(caps.driveProtocol, undefined);
  assert.equal(caps.driveUnavailableReason, 'drive_webclient_unavailable');
  // Another user of the same VM without the permission still sees nothing.
  assert.equal((await f.call('/desktops', { token: f.bob })).body.desktops[0].host.capabilities.driveUnavailableReason, undefined);
});

test('The unavailable reason is fixed: other values, a Windows login screen or a capable Host drop it', async t => {
  for (const extra of [{ driveUnavailableReason: 'anything else' }, { driveUnavailableReason: true }, { driveUnavailableReason: 'drive_webclient_unavailable', interactiveSession: false }, { ...driveHost, driveUnavailableReason: 'drive_webclient_unavailable' }]) {
    const f = await fixture(t); f.allow('alice');
    await f.host({ ...releasedHost, ...extra });
    const caps = (await f.call('/desktops', { token: f.alice })).body.desktops[0].host.capabilities;
    assert.equal(caps.driveUnavailableReason, undefined, JSON.stringify(extra));
  }
});

test('A released Host gets nothing new; a malformed capability is omitted, not fatal', async t => {
  const f = await fixture(t); f.allow('alice');
  const host = await f.host({ ...releasedHost, driveProtocol: 'drive-v9' });
  const inventory = await f.call('/desktops', { token: f.alice });
  assert.equal(inventory.body.desktops[0].canConnect, true);
  assert.equal(inventory.body.desktops[0].host.capabilities.driveProtocol, undefined);
  const session = await f.stream(host, { driveProtocol: 'drive-v1' });
  assert.equal(session.response.body.driveProtocol, undefined);
  assert.equal(session.start.driveUrl, undefined);
  assert.equal(await rejected(f.socket(f.wsUrl('/api/v1/sessions/' + session.sessionId + '/drive'), 'Bearer ' + f.alice)), 401);
});

test('The pair: exact authentication, one socket per role, ready with limits', async t => {
  const f = await fixture(t); f.allow('alice'); const host = await f.host(driveHost);
  const session = await f.stream(host, { driveProtocol: 'drive-v1' });
  const clientUrl = f.local(session.response.body.driveWebSocketUrl), hostUrl = f.local(session.start.driveUrl);
  assert.equal(await rejected(f.socket(clientUrl, 'Bearer ' + f.bob)), 401, 'another account');
  assert.equal(await rejected(f.socket(clientUrl, 'Host ' + f.hostToken())), 401, 'the Host token is not a client token');
  assert.equal(await rejected(f.socket(hostUrl, 'Bearer ' + f.alice)), 401, 'a client token is not a Host token');
  assert.equal(await rejected(f.socket(f.wsUrl('/api/v1/sessions/00000000-0000-0000-0000-000000000000/drive'), 'Bearer ' + f.alice)), 401);
  const client = await f.socket(clientUrl, 'Bearer ' + f.alice);
  assert.equal(await rejected(f.socket(clientUrl, 'Bearer ' + f.alice)), 409, 'one client socket');
  // Nothing may be sent before both are attached.
  const hostDrive = await f.socket(hostUrl, 'Host ' + f.hostToken());
  const ready = await client.take(value => value.type === 'drive.ready');
  assert.deepEqual(ready, { type: 'drive.ready', sessionId: session.sessionId, protocol: 'drive-v1', limits: driveLimits });
  assert.deepEqual(await hostDrive.take(value => value.type === 'drive.ready'), { ...ready, notices: true }, 'the Host is also told it may send notices');
  assert.equal(await rejected(f.socket(hostUrl, 'Host ' + f.hostToken())), 409, 'one Host socket');
});

test('Shares, requests and responses are relayed exactly; data is binary in both directions', async t => {
  const f = await fixture(t); f.allow('alice'); const host = await f.host(driveHost);
  const shares = [{ id: 'docs', name: 'Documents', readOnly: false }, { id: 'pics', name: '사진', readOnly: true }];
  const { client, hostDrive, controller } = await f.pair(host);
  client.json({ type: 'drive.shares', shares });
  assert.deepEqual(await hostDrive.take(value => value.type === 'drive.shares'), { type: 'drive.shares', shares });
  const mounted = { type: 'drive.mounted', shares: [{ id: 'docs', state: 'mapped', letter: 'Z' }, { id: 'pics', state: 'failed', error: 'drive_letter_unavailable' }] };
  hostDrive.json(mounted);
  assert.deepEqual(await client.take(value => value.type === 'drive.mounted'), mounted);

  // stat of the root (with space), list with paging, a failure.
  const stat = { type: 'drive.request', id: 1, op: 'stat', share: 'docs', path: '' };
  hostDrive.json(stat); assert.deepEqual(await client.take(value => value.type === 'drive.request'), stat);
  const statResponse = { type: 'drive.response', id: 1, ok: true, entry: entry('', { dir: true, size: 0, space: { free: 10, total: 20 } }) };
  client.json(statResponse); assert.deepEqual(await hostDrive.take(value => value.type === 'drive.response'), statResponse);
  const list = { type: 'drive.request', id: 2, op: 'list', share: 'pics', path: '여행', cursor: 0 };
  hostDrive.json(list); await client.take(value => value.id === 2);
  const listResponse = { type: 'drive.response', id: 2, ok: true, entries: [entry('a.jpg'), entry('b', { dir: true, size: 0 })], next: 2 };
  client.json(listResponse); assert.deepEqual(await hostDrive.take(value => value.id === 2), listResponse);
  hostDrive.json({ type: 'drive.request', id: 3, op: 'stat', share: 'docs', path: 'missing.txt' }); await client.take(value => value.id === 3);
  client.json({ type: 'drive.response', id: 3, ok: false, error: 'drive_not_found' });
  assert.deepEqual(await hostDrive.take(value => value.id === 3), { type: 'drive.response', id: 3, ok: false, error: 'drive_not_found' });

  // read: JSON request, binary answer of at most the requested length.
  hostDrive.json({ type: 'drive.request', id: 4, op: 'read', share: 'pics', path: '여행/a.jpg', offset: 0, length: 3 }); await client.take(value => value.id === 4);
  client.send(readData(4, [7, 8, 9]));
  assert.deepEqual([...await hostDrive.take(isBinary)], [...readData(4, [7, 8, 9])]);
  // write: one binary message, header + data, forwarded unchanged.
  const write = writeFrame(5, { op: 'write', share: 'docs', path: 'new.txt', offset: 0, truncate: true }, [1, 2, 3]);
  hostDrive.send(write);
  assert.deepEqual([...await client.take(isBinary)], [...write]);
  client.json({ type: 'drive.response', id: 5, ok: true });
  assert.deepEqual(await hostDrive.take(value => value.id === 5), { type: 'drive.response', id: 5, ok: true });
  for (const [id, op, extra] of [[6, 'create', { dir: true, truncate: false }], [7, 'rename', { to: 'b.txt', replace: false }], [8, 'touch', { modified: 1761300000000 }], [9, 'delete', {}]]) {
    const value = { type: 'drive.request', id, op, share: 'docs', path: 'a.txt', ...extra };
    hostDrive.json(value); assert.deepEqual(await client.take(item => item.id === id), value);
    client.json({ type: 'drive.response', id, ok: true }); await hostDrive.take(item => item.id === id);
  }
  // The relay itself answers what the share list decides.
  hostDrive.json({ type: 'drive.request', id: 10, op: 'stat', share: 'gone', path: '' });
  assert.deepEqual(await hostDrive.take(value => value.id === 10), { type: 'drive.response', id: 10, ok: false, error: 'drive_share_unknown' });
  for (const [id, op, extra] of [[11, 'create', { dir: false, truncate: true }], [12, 'rename', { to: 'b', replace: true }], [13, 'delete', {}], [14, 'touch', { modified: 0 }]]) {
    hostDrive.json({ type: 'drive.request', id, op, share: 'pics', path: 'a', ...extra });
    assert.deepEqual(await hostDrive.take(value => value.id === id), { type: 'drive.response', id, ok: false, error: 'drive_read_only' });
  }
  hostDrive.send(writeFrame(15, { op: 'write', share: 'pics', path: 'a', offset: 0, truncate: true }, [1]));
  assert.deepEqual(await hostDrive.take(value => value.id === 15), { type: 'drive.response', id: 15, ok: false, error: 'drive_read_only' });
  // Reads on a read-only share reach the client.
  hostDrive.json({ type: 'drive.request', id: 16, op: 'read', share: 'pics', path: 'a', offset: 5, length: 10 });
  assert.equal((await client.take(value => value.id === 16)).op, 'read');
  assert.equal(client.queue.filter(value => !isBinary(value) && [10, 11, 12, 13, 14].includes(value.id)).length, 0, 'refused requests never reach the PC');
  assert.equal(controller.readyState, WebSocket.OPEN);
});

for (const [name, act, reason] of [
  ['an unknown message type', p => p.client.json({ type: 'drive.hello' }), 'drive_invalid_message'],
  ['a path escape', p => p.hostDrive.json({ type: 'drive.request', id: 1, op: 'stat', share: 'docs', path: '../secret' }), 'drive_invalid_message'],
  ['a backslash path', p => p.hostDrive.json({ type: 'drive.request', id: 1, op: 'stat', share: 'docs', path: 'a\\b' }), 'drive_invalid_message'],
  ['an extra request field', p => p.hostDrive.json({ type: 'drive.request', id: 1, op: 'stat', share: 'docs', path: '', recursive: true }), 'drive_invalid_message'],
  ['a read over the chunk size', p => p.hostDrive.json({ type: 'drive.request', id: 1, op: 'read', share: 'docs', path: 'a', offset: 0, length: 262145 }), 'drive_invalid_message'],
  ['a rename of the root', p => p.hostDrive.json({ type: 'drive.request', id: 1, op: 'rename', share: 'docs', path: '', to: 'a', replace: false }), 'drive_invalid_message'],
  ['a repeated id', async p => { p.hostDrive.json({ type: 'drive.request', id: 5, op: 'stat', share: 'docs', path: '' }); await p.client.take(v => v.id === 5); p.hostDrive.json({ type: 'drive.request', id: 5, op: 'stat', share: 'docs', path: '' }); }, 'drive_out_of_order'],
  ['an unsolicited response', p => p.client.json({ type: 'drive.response', id: 1, ok: true }), 'drive_out_of_order'],
  ['unsolicited read data', p => p.client.send(readData(1, [1])), 'drive_out_of_order'],
  ['more read data than requested', async p => { p.hostDrive.json({ type: 'drive.request', id: 1, op: 'read', share: 'docs', path: 'a', offset: 0, length: 2 }); await p.client.take(v => v.id === 1); p.client.send(readData(1, [1, 2, 3])); }, 'drive_limits_exceeded'],
  ['a read answered as JSON', async p => { p.hostDrive.json({ type: 'drive.request', id: 1, op: 'read', share: 'docs', path: 'a', offset: 0, length: 2 }); await p.client.take(v => v.id === 1); p.client.json({ type: 'drive.response', id: 1, ok: true }); }, 'drive_invalid_message'],
  ['an unknown error code', async p => { p.hostDrive.json({ type: 'drive.request', id: 1, op: 'stat', share: 'docs', path: 'a' }); await p.client.take(v => v.id === 1); p.client.json({ type: 'drive.response', id: 1, ok: false, error: 'C:\\Users\\alice\\secret.txt' }); }, 'drive_invalid_message'],
  ['an unsafe listed name', async p => { p.hostDrive.json({ type: 'drive.request', id: 1, op: 'list', share: 'docs', path: '', cursor: 0 }); await p.client.take(v => v.id === 1); p.client.json({ type: 'drive.response', id: 1, ok: true, entries: [entry('..')] }); }, 'drive_invalid_message'],
  ['too many entries', async p => { p.hostDrive.json({ type: 'drive.request', id: 1, op: 'list', share: 'docs', path: '', cursor: 0 }); await p.client.take(v => v.id === 1); p.client.json({ type: 'drive.response', id: 1, ok: true, entries: Array.from({ length: 257 }, (_, n) => entry('f' + n)) }); }, 'drive_limits_exceeded'],
  ['nine requests in flight', async p => { for (let id = 1; id <= 9; id++) p.hostDrive.json({ type: 'drive.request', id, op: 'stat', share: 'docs', path: '' }); }, 'drive_limits_exceeded'],
  ['nine shares', p => p.client.json({ type: 'drive.shares', shares: Array.from({ length: 9 }, (_, n) => ({ id: 's' + n, name: 'S' + n, readOnly: false })) }), 'drive_limits_exceeded'],
  ['a duplicate share id', p => p.client.json({ type: 'drive.shares', shares: [{ id: 'a', name: 'A', readOnly: false }, { id: 'a', name: 'B', readOnly: true }] }), 'drive_invalid_message'],
  ['an unsafe share label', p => p.client.json({ type: 'drive.shares', shares: [{ id: 'a', name: 'C:', readOnly: false }] }), 'drive_invalid_message'],
  ['a write without data', p => p.hostDrive.send(writeFrame(1, { op: 'write', share: 'docs', path: 'a', offset: 0, truncate: true }, [])), 'drive_limits_exceeded'],
  ['a truncating write at an offset', p => p.hostDrive.send(writeFrame(1, { op: 'write', share: 'docs', path: 'a', offset: 4, truncate: true }, [1])), 'drive_invalid_message'],
  ['a write header with an extra field', p => p.hostDrive.send(writeFrame(1, { op: 'write', share: 'docs', path: 'a', offset: 0, truncate: false, sync: true }, [1])), 'drive_invalid_message'],
  ['a mounted letter outside D-Z', p => p.hostDrive.json({ type: 'drive.mounted', shares: [{ id: 'docs', state: 'mapped', letter: 'C' }] }), 'drive_invalid_message'],
  ['a client request', p => p.client.json({ type: 'drive.request', id: 1, op: 'stat', share: 'docs', path: '' }), 'drive_invalid_message'],
  ['a Host share list', p => p.hostDrive.json({ type: 'drive.shares', shares: [] }), 'drive_invalid_message'],
]) test('The pair closes on ' + name + ' and the stream continues', async t => {
  const f = await fixture(t); f.allow('alice'); const host = await f.host(driveHost);
  const p = await f.pair(host, { shares: [{ id: 'docs', name: 'Documents', readOnly: false }] });
  await act(p);
  assert.equal((await p.client.take(driveError)).reason, reason);
  assert.equal((await p.hostDrive.take(driveError)).reason, reason);
  await closed(p.client); await closed(p.hostDrive);
  assert.equal(p.controller.readyState, WebSocket.OPEN, 'the stream is not ended by a drive failure');
  assert.equal(host.queue.some(value => value.type === 'session.stop'), false);
  // A closed pair is final for this session.
  assert.equal(await rejected(f.socket(f.local(p.response.body.driveWebSocketUrl), 'Bearer ' + f.alice)), 409);
});

test('Revoking drives closes only the drive pair; ending the session closes it too', async t => {
  const f = await fixture(t); f.allow('alice'); const host = await f.host(driveHost);
  let p = await f.pair(host, { shares: [{ id: 'docs', name: 'Documents', readOnly: false }] });
  hostRequest(p, 1);
  await p.client.take(value => value.id === 1);
  f.allow('alice', false);
  assert.equal((await p.client.take(driveError)).reason, 'drive_policy_changed');
  assert.equal((await p.hostDrive.take(driveError)).reason, 'drive_policy_changed');
  await closed(p.client); await closed(p.hostDrive);
  await new Promise(resolve => setTimeout(resolve, 100));
  assert.equal(p.controller.readyState, WebSocket.OPEN, 'the stream continues');
  assert.equal(host.queue.some(value => value.type === 'session.stop'), false);
  p.controller.json({ type: 'session.stop' }); await host.take(value => value.type === 'session.stop'); await closed(p.controller);

  f.allow('alice');
  p = await f.pair(host);
  p.controller.json({ type: 'session.stop' });
  assert.equal((await p.client.take(driveError)).reason, 'session_ended');
  assert.equal((await p.hostDrive.take(driveError)).reason, 'session_ended');
  await closed(p.client); await closed(p.hostDrive);
});

function hostRequest(p, id) { p.hostDrive.json({ type: 'drive.request', id, op: 'stat', share: 'docs', path: '' }); }

test('An unanswered request times out the pair', async t => {
  const f = await fixture(t, { driveOptions: { requestMs: 150 } }); f.allow('alice'); const host = await f.host(driveHost);
  const p = await f.pair(host, { shares: [{ id: 'docs', name: 'Documents', readOnly: false }] });
  hostRequest(p, 1); await p.client.take(value => value.id === 1);
  assert.equal((await p.client.take(driveError)).reason, 'drive_timeout');
  assert.equal(p.controller.readyState, WebSocket.OPEN);
});

test('A drive pair that never completes times out; the stream continues', async t => {
  const f = await fixture(t, { driveOptions: { pairReadyMs: 100 } }); f.allow('alice'); const host = await f.host(driveHost);
  const session = await f.stream(host, { driveProtocol: 'drive-v1' });
  const client = await f.socket(f.local(session.response.body.driveWebSocketUrl), 'Bearer ' + f.alice);
  assert.equal((await client.take(driveError)).reason, 'drive_pair_timeout');
  assert.equal(session.controller.readyState, WebSocket.OPEN);
});

test('Host notices reach only a client that asked for them; unknown notices close the pair', async t => {
  const f = await fixture(t); f.allow('alice'); const host = await f.host(driveHost);
  const shares = [{ id: 'docs', name: 'Documents', readOnly: false }];
  let p = await f.pair(host, { shares, notices: true });
  p.hostDrive.json({ type: 'drive.notice', code: 'drive_webclient_old_limit' });
  assert.deepEqual(await p.client.take(value => value.type === 'drive.notice'), { type: 'drive.notice', code: 'drive_webclient_old_limit' });
  p.hostDrive.json({ type: 'drive.notice', code: 'drive_file_too_large' });
  assert.equal((await p.client.take(value => value.type === 'drive.notice')).code, 'drive_file_too_large');
  p.controller.json({ type: 'session.stop' }); await host.take(value => value.type === 'session.stop'); await closed(p.controller); await closed(p.client);
  // An older client (no header) is never sent a message it would reject.
  p = await f.pair(host, { shares });
  p.hostDrive.json({ type: 'drive.notice', code: 'drive_webclient_old_limit' });
  hostRequest(p, 1); await p.client.take(value => value.id === 1);
  assert.equal(p.client.queue.some(value => value.type === 'drive.notice'), false);
  assert.equal(p.client.readyState, WebSocket.OPEN);
  p.controller.json({ type: 'session.stop' }); await host.take(value => value.type === 'session.stop'); await closed(p.controller); await closed(p.client);
  for (const bad of [{ type: 'drive.notice', code: 'C:\\\\secret' }, { type: 'drive.notice', code: 'drive_file_too_large', extra: 1 }, { type: 'drive.notice' }]) {
    p = await f.pair(host, { shares, notices: true });
    p.hostDrive.json(bad);
    assert.equal((await p.client.take(driveError)).reason, 'drive_invalid_message', JSON.stringify(bad));
    await closed(p.client); await closed(p.hostDrive);
    assert.equal(p.controller.readyState, WebSocket.OPEN);
    p.controller.json({ type: 'session.stop' }); await host.take(value => value.type === 'session.stop'); await closed(p.controller);
  }
  // Only the Host sends notices.
  p = await f.pair(host, { shares, notices: true });
  p.client.json({ type: 'drive.notice', code: 'drive_file_too_large' });
  assert.equal((await p.hostDrive.take(driveError)).reason, 'drive_invalid_message');
});

// A host-driven copy of `total` bytes through a pair: 8 reads in flight, the
// client answers each at once (the fastest a LAN peer can do).
async function copyThrough(p, total, onSample) {
  const chunk = Buffer.alloc(262144, 7), count = total / 262144;
  p.hostDrive.removeAllListeners('message'); p.client.removeAllListeners('message');
  let sent = 0, received = 0, failure = null;
  const request = () => { if (sent < count) { sent++; p.hostDrive.json({ type: 'drive.request', id: sent, op: 'read', share: 'docs', path: 'big.bin', offset: (sent - 1) * 262144, length: 262144 }); } };
  p.client.on('message', (data, binary) => { const value = binary ? null : JSON.parse(data.toString('utf8')); if (value?.type === 'drive.error') failure = value.reason; if (value?.type === 'drive.request') p.client.send(readData(value.id, chunk), { binary: true }); });
  const done = new Promise(resolve => {
    p.hostDrive.on('message', (data, binary) => {
      if (!binary) { const value = JSON.parse(data.toString('utf8')); if (value.type === 'drive.error') { failure = value.reason; resolve(); } return; }
      received += data.length - 5; request(); onSample?.(received); if (received >= total) resolve();
    });
    p.hostDrive.once('close', resolve);
  });
  for (let n = 0; n < 8; n++) request();
  await done;
  return { received, failure };
}

test('A 1 GiB copy at full speed completes without closing the pair and with bounded broker memory', { timeout: 180000 }, async t => {
  const f = await fixture(t); f.allow('alice'); const host = await f.host(driveHost);
  const p = await f.pair(host, { shares: [{ id: 'docs', name: 'Documents', readOnly: false }] });
  global.gc?.(); const base = process.memoryUsage().rss; let peak = base;
  const result = await copyThrough(p, 1024 * 1024 * 1024, () => { peak = Math.max(peak, process.memoryUsage().rss); });
  assert.equal(result.failure, null);
  assert.equal(result.received, 1024 * 1024 * 1024);
  assert.equal(p.client.readyState, WebSocket.OPEN);
  assert.equal(p.hostDrive.readyState, WebSocket.OPEN);
  // The whole test process (broker and both peers) must not hold the copy.
  assert.ok(peak - base < 400 * 1024 * 1024, `rss grew ${(peak - base) / 1048576} MiB`);
});

test('Going over the byte budget pauses the sender instead of closing the pair', async t => {
  const f = await fixture(t, { driveOptions: { softBytes: 8 * 1024 * 1024, softTotalBytes: 8 * 1024 * 1024 } }); f.allow('alice'); const host = await f.host(driveHost);
  const p = await f.pair(host, { shares: [{ id: 'docs', name: 'Documents', readOnly: false }] });
  const started = Date.now();
  const result = await copyThrough(p, 40 * 1024 * 1024);
  assert.equal(result.failure, null);
  assert.equal(result.received, 40 * 1024 * 1024);
  assert.ok(Date.now() - started >= 2000, 'a 40 MiB copy over an 8 MiB/s budget takes seconds (paused, not instant)');
  assert.equal(p.client.readyState, WebSocket.OPEN);
});

test('A flood of tiny frames far past the message budget is still closed as abusive', async t => {
  const f = await fixture(t, { driveOptions: { softMessages: 100 } }); f.allow('alice'); const host = await f.host(driveHost);
  const p = await f.pair(host, { shares: [{ id: 'docs', name: 'Documents', readOnly: false }] });
  const shares = JSON.stringify({ type: 'drive.shares', shares: [{ id: 'docs', name: 'Documents', readOnly: false }] });
  for (let n = 0; n < 3000 && p.client.readyState === WebSocket.OPEN; n++) p.client.send(shares);
  // 3000 frames take longer than the usual 2 s wait on a busy machine (a build
  // running beside the suite, say); the verdict is the same.
  assert.equal((await p.hostDrive.take(driveError, 15000)).reason, 'drive_rate_limit');
  await closed(p.client); await closed(p.hostDrive);
  assert.equal(p.controller.readyState, WebSocket.OPEN, 'the stream continues');
});
test('Client diagnostics keep drive rows: fixed codes and counts only', () => {
  const report = safeClientDiagnosticReport({ protocol: 'client-diagnostics-v1', runId: 'a'.repeat(32), sequence: 1, version: '0.4.83+97', events: [
    { event: 'drive', time: '2026-10-26T10:00:00.000Z', code: 'drive_policy_changed', values: { ok: false, stage: 3, shares: 2, mapped: 1, requests: 40, failedRequests: 2, path: 'C:\\secret' } },
    { event: 'drive', time: '2026-10-26T10:00:01.000Z', code: 'C:\\Users\\alice', values: {} }] });
  assert.deepEqual(report.events[0].values, { ok: false, stage: 3, shares: 2, mapped: 1, requests: 40, failedRequests: 2 });
  assert.equal(report.events[0].code, 'drive_policy_changed');
  assert.equal(report.events[1].code, 'unknown_failure');
  const steps = safeClientDiagnosticReport({ protocol: 'client-diagnostics-v1', runId: 'b'.repeat(32), sequence: 1, version: '0.4.92+1', events: [
    { event: 'drive', time: '2026-10-27T10:00:00.000Z', code: 'drive_denied', values: { ok: false, stage: 8, error: 5, shares: 0, remembered: 2, restored: 1, elapsedMs: 120, name: 'x' } }] });
  assert.deepEqual(steps.events[0].values, { ok: false, stage: 8, error: 5, shares: 0, remembered: 2, restored: 1, elapsedMs: 120 });
});

// The ranged-file-system revision (WinFsp backend): extra ops only on a
// session that negotiated `driveFs`; every released combination keeps drive-v1 as it was.
const fsHost = { ...driveHost, driveFs: true };
test('driveFs capability and connect field are exact and never a reason to refuse', () => {
  assert.equal(driveFsCapability(fsHost), true);
  for (const value of [driveHost, releasedHost, { ...fsHost, driveFs: 'true' }, { ...fsHost, driveFs: 1 }, { ...releasedHost, driveFs: true }, { ...fsHost, interactiveSession: false }, undefined, null])
    assert.equal(driveFsCapability(value), false, JSON.stringify(value));
  assert.equal(requestedDriveFs('stream', 'drive-v1', undefined), false);
  assert.equal(requestedDriveFs('stream', 'drive-v1', true), true);
  for (const [mode, protocol, fs] of [['stream', 'drive-v1', false], ['stream', 'drive-v1', 'true'], ['stream', undefined, true], ['rdp', 'drive-v1', true], ['stream', 'drive-v1', null]])
    assert.equal(code(() => requestedDriveFs(mode, protocol, fs)), 'invalid_drive_protocol', `${mode} ${protocol} ${fs}`);
});

test('driveFs is granted only when requested, advertised and allowed; released combinations stay on drive-v1', async t => {
  const f = await fixture(t);
  const host = await f.host(fsHost);
  f.allow();
  let inventory = await f.call('/desktops', { token: f.alice });
  assert.equal(inventory.body.desktops[0].host.capabilities.driveFs, true);
  assert.equal((await f.call('/desktops', { token: f.bob })).body.desktops[0].host.capabilities.driveFs, undefined, 'hidden without the permission');
  let session = await f.stream(host, { driveProtocol: 'drive-v1', driveFs: true });
  assert.equal(session.response.body.driveFs, true); assert.equal(session.start.driveFs, true);
  session.controller.json({ type: 'session.stop' }); await host.take(value => value.type === 'session.stop'); await closed(session.controller);
  session = await f.stream(host, { driveProtocol: 'drive-v1' });
  assert.equal(session.response.body.driveProtocol, 'drive-v1'); assert.equal(session.response.body.driveFs, undefined); assert.equal(session.start.driveFs, undefined);
  session.controller.json({ type: 'session.stop' }); await host.take(value => value.type === 'session.stop'); await closed(session.controller);
  const refused = await f.connect({ driveFs: true });
  assert.equal(refused.status, 400, 'driveFs without driveProtocol is malformed');
  f.store.updateAssignmentPolicy('alice', 'pve-187', { devicePolicy: { ...deny, drives: false } }, 'fixture');
  session = await f.stream(host, { driveProtocol: 'drive-v1', driveFs: true });
  assert.equal(session.response.body.driveFs, undefined); assert.equal(session.start.driveFs, undefined);
  host.close();
  const released = await fixture(t);
  const releasedSocket = await released.host(driveHost); released.allow();
  const plain = await released.stream(releasedSocket, { driveProtocol: 'drive-v1', driveFs: true });
  assert.equal(plain.response.body.driveProtocol, 'drive-v1'); assert.equal(plain.response.body.driveFs, undefined, 'a Host without the capability is not granted it, not refused');
  releasedSocket.close();
});

test('setsize and flush are relayed only on an fs pair and are strictly validated', async t => {
  const f = await fixture(t);
  const host = await f.host(fsHost); f.allow();
  const shares = [{ id: 'a1', name: 'Docs', readOnly: false }, { id: 'b2', name: 'Ro', readOnly: true }];
  const p = await f.pair(host, { shares, driveFs: true });
  for (const [id, op, extra] of [[1, 'setsize', { size: 5368709120 }], [2, 'flush', {}], [3, 'setsize', { size: 0 }]]) {
    const value = { type: 'drive.request', id, op, share: 'a1', path: 'big.bin', ...extra };
    p.hostDrive.json(value); assert.deepEqual(await p.client.take(item => item.id === id), value);
    p.client.json({ type: 'drive.response', id, ok: true });
    assert.deepEqual(await p.hostDrive.take(item => item.id === id), { type: 'drive.response', id, ok: true });
  }
  // flush may name the root; the read-only share refuses setsize itself, not flush.
  p.hostDrive.json({ type: 'drive.request', id: 4, op: 'flush', share: 'a1', path: '' }); assert.equal((await p.client.take(item => item.id === 4)).op, 'flush');
  p.client.json({ type: 'drive.response', id: 4, ok: true }); await p.hostDrive.take(item => item.id === 4);
  p.hostDrive.json({ type: 'drive.request', id: 5, op: 'setsize', share: 'b2', path: 'x', size: 1 });
  assert.deepEqual(await p.hostDrive.take(item => item.id === 5), { type: 'drive.response', id: 5, ok: false, error: 'drive_read_only' });
  p.hostDrive.json({ type: 'drive.request', id: 6, op: 'flush', share: 'b2', path: 'x' }); assert.equal((await p.client.take(item => item.id === 6)).op, 'flush');
  p.client.json({ type: 'drive.response', id: 6, ok: true }); await p.hostDrive.take(item => item.id === 6);
  // A response to setsize carries nothing else.
  p.hostDrive.json({ type: 'drive.request', id: 7, op: 'setsize', share: 'a1', path: 'x', size: 2 }); await p.client.take(item => item.id === 7);
  p.client.json({ type: 'drive.response', id: 7, ok: true, entry: entry('x') });
  assert.equal((await p.hostDrive.take(driveError)).reason, 'drive_invalid_message');
  await closed(p.hostDrive);
  assert.equal(p.controller.readyState, WebSocket.OPEN, 'only the drive pair ends');
  // Malformed requests end the pair.
  for (const bad of [{ op: 'setsize', path: 'x', size: -1 }, { op: 'setsize', path: 'x', size: 1.5 }, { op: 'setsize', path: 'x', size: 9007199254740992 }, { op: 'setsize', path: '', size: 1 }, { op: 'setsize', path: 'x' }, { op: 'setsize', path: 'x', size: 1, extra: 1 }, { op: 'setsize', path: 'x', size: '1' }, { op: 'flush', path: 'x', size: 1 }, { op: 'flush', path: '../x' }]) {
    const g = await fixture(t); const h = await g.host(fsHost); g.allow();
    const q = await g.pair(h, { shares, driveFs: true });
    q.hostDrive.json({ type: 'drive.request', id: 1, share: 'a1', ...bad });
    assert.equal((await q.client.take(driveError)).reason, 'drive_invalid_message', JSON.stringify(bad));
    h.close();
  }
  host.close();
});

test('Without a negotiated driveFs the new ops are invalid messages (released peers and WebDAV sessions)', async t => {
  for (const [capabilities, extra] of [[fsHost, {}], [driveHost, { driveFs: true }]]) {
    for (const op of [{ op: 'setsize', size: 1 }, { op: 'flush' }]) {
      const f = await fixture(t); const host = await f.host(capabilities); f.allow();
      const p = await f.pair(host, { shares: [{ id: 'a1', name: 'Docs', readOnly: false }], ...(extra.driveFs ? { driveFs: true } : {}) });
      p.hostDrive.json({ type: 'drive.request', id: 1, share: 'a1', path: 'x', ...op });
      assert.equal((await p.client.take(driveError)).reason, 'drive_invalid_message', JSON.stringify([capabilities, op]));
      host.close();
    }
  }
});