import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { caretCapability, requestedCaret, negotiateCaret, validCaretWatch, caretReport, caretAllowed, caretMaxPerSecond } from '../src/caret.mjs';

const origin = 'https://caret-fixture.example';
const password = 'Caret-fixture-password-only!';
const machineUuid = '12345678-1234-1234-1234-123456789ad2';
const releasedHost = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true };
const caretHost = { ...releasedHost, caretProtocol: 'caret-v1' };
const streamInfo = { type: 'stream.info', codec: 'h264', width: 1920, height: 1080, sourceWidth: 1920, sourceHeight: 1080, fpsTarget: 60, audio: { sampleRate: 48000, channels: 2, format: 's16le', available: false } };

async function fixture(t) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const hash = await hashPassword(password);
  store.addUser({ name: 'admin', passwordHash: hash, role: 'admin' });
  store.addUser({ name: 'alice', passwordHash: hash, role: 'user' });
  store.syncProxmox([{ vmid: 181, node: 'pve', name: 'VM 181', status: 'running' }], 'fixture');
  store.assign('alice', 'pve-181');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve-fixture.example', tokenId: 'fixture@pve!broker', secret: 'only-fixture-secret' }), 'fixture');
  const proxmox = { machineIdentity: async () => ({ machineUuid }) };
  const server = await buildServer({ store, vault, proxmox, publicOrigin: origin, relayOptions: { authorizationMs: 20, heartbeatMs: 200 }, logger: { error: value => assert.fail(value) } });
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
  const code = (await call('/admin/desktops/pve-181/enrollment', { ...admin, method: 'POST', data: {} })).body.code;
  const wsUrl = path => base.replace('http:', 'ws:') + path;
  async function host(capabilities) {
    const enrolled = await call('/hosts/enroll', { method: 'POST', data: { code, hostname: 'Fixture VM', machineUuid, version: '0.2.70' } });
    assert.equal(enrolled.status, 201);
    const ws = await socket(wsUrl('/api/v1/hosts/channel'), 'Host ' + enrolled.body.hostToken);
    ws.json({ type: 'host.capabilities', capabilities });
    assert.equal((await ws.take(value => value.type === 'host.capabilities.ack')).accepted, true);
    return ws;
  }
  const connect = extra => call('/desktops/pve-181/connect', { method: 'POST', token: alice, data: { mode: 'stream', ...extra } });
  const client = sessionId => socket(wsUrl('/api/v1/sessions/' + sessionId), 'Bearer ' + alice);
  async function stream(hostSocket, extra = {}) {
    const response = await connect(extra);
    assert.equal(response.status, 200, JSON.stringify(response.body));
    const controller = await client(response.body.sessionId);
    const start = await hostSocket.take(value => value.type === 'session.start');
    hostSocket.json({ ...streamInfo, ...(start.resizeProtocol ? { epoch: 0 } : {}) }); await controller.take(value => value.type === 'stream.info');
    return { response, controller, start, sessionId: response.body.sessionId };
  }
  return { call, alice, host, connect, client, stream };
}
async function socket(url, authorization) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url, { headers: { Authorization: authorization }, perMessageDeflate: false });
    ws.once('error', reject);
    ws.once('open', () => {
      const queue = [], waiters = [];
      ws.on('message', (data, binary) => {
        const value = binary ? data : JSON.parse(data.toString('utf8'));
        const index = waiters.findIndex(waiter => waiter.predicate(value));
        if (index >= 0) { const waiter = waiters.splice(index, 1)[0]; clearTimeout(waiter.timer); waiter.resolve(value); } else queue.push(value);
      });
      ws.queue = queue;
      ws.take = (predicate = () => true) => {
        const index = queue.findIndex(predicate);
        if (index >= 0) return Promise.resolve(queue.splice(index, 1)[0]);
        return new Promise((done, fail) => { const waiter = { predicate, resolve: done, timer: setTimeout(() => fail(new Error('message_timeout')), 2000) }; waiters.push(waiter); });
      };
      ws.json = value => ws.send(JSON.stringify(value));
      resolve(ws);
    });
  });
}
const closed = ws => new Promise(resolve => { if (ws.readyState === WebSocket.CLOSED) resolve(); else ws.once('close', resolve); });
const code = fn => { try { fn(); return 'accepted'; } catch (error) { return error.code; } };
const isTouch = item => item.type === 'input' && item.event === 'touch';


const caretOn = { type: 'caret.watch', on: true };
const report = (sessionId, extra = { has: true, x: 100, y: 1200, w: 2, h: 30, sw: 1080, sh: 2400 }) => ({ type: 'caret', sessionId, ...extra });

test('Capability, request, negotiation and message rules are exact', () => {
  assert.equal(caretCapability(caretHost), true);
  for (const value of [releasedHost, { ...releasedHost, caretProtocol: 'caret-v2' }, { ...releasedHost, caretProtocol: true }, undefined, null]) assert.equal(caretCapability(value), false);
  assert.equal(requestedCaret('stream', undefined), false);
  assert.equal(requestedCaret('stream', 'caret-v1'), true);
  for (const [mode, protocol] of [['rdp', 'caret-v1'], ['stream', 'caret-v2'], ['stream', ''], ['stream', 1], ['stream', null], ['stream', true]]) {
    assert.throws(() => requestedCaret(mode, protocol), error => error.code === 'invalid_caret_protocol', `${mode} ${protocol}`);
  }
  assert.equal(negotiateCaret(true, { capabilities: caretHost }, null), true);
  assert.equal(negotiateCaret(false, { capabilities: caretHost }, null), false, 'never granted unrequested');
  assert.equal(negotiateCaret(true, { capabilities: releasedHost }, null), false, 'released Host: no caret, not refused');
  assert.equal(negotiateCaret(true, { capabilities: caretHost }, [{ id: '0' }]), false, 'independent outputs have no caret');
  assert.equal(validCaretWatch({ type: 'caret.watch', on: true }), true);
  assert.equal(validCaretWatch({ type: 'caret.watch', on: false }), true);
  for (const value of [{ type: 'caret.watch' }, { type: 'caret.watch', on: 1 }, { type: 'caret.watch', on: 'true' }, { type: 'caret.watch', on: true, x: 1 }, { type: 'caret', on: true }, null, [], 'x']) assert.equal(validCaretWatch(value), false, JSON.stringify(value));
  assert.deepEqual(caretReport(report('s1')), report('s1'));
  assert.deepEqual(caretReport({ type: 'caret', sessionId: 's1', has: false }), { type: 'caret', sessionId: 's1', has: false });
  for (const value of [report('s1', { has: true, x: 1, y: 2, w: 3, h: 4, sw: 5 }), report('s1', { has: true, x: -1, y: 2, w: 3, h: 4, sw: 5, sh: 6 }), report('s1', { has: true, x: 1.5, y: 2, w: 3, h: 4, sw: 5, sh: 6 }),
    report('s1', { has: true, x: 1, y: 2, w: 3, h: 4, sw: 1, sh: 6 }), report('s1', { has: true, x: 1, y: 2, w: 3, h: 70000, sw: 5, sh: 6 }), report('s1', { has: true, x: 1, y: 2, w: 3, h: 4, sw: 5, sh: 6, text: 'secret' }),
    report('s1', { has: false, x: 1 }), report('s1', { has: 1 }), { type: 'caret', has: false }, { type: 'caret', sessionId: '', has: false }, null, []]) assert.equal(caretReport(value), null, JSON.stringify(value));
});

test('A granted session relays watch to the Host and reports to the client; ungranted peers see nothing', async t => {
  const f = await fixture(t); const host = await f.host(caretHost);
  const inventory = await f.call('/desktops', { token: f.alice });
  assert.equal(inventory.body.desktops[0].host.capabilities.caretProtocol, 'caret-v1');
  const { response, controller, start, sessionId } = await f.stream(host, { caretProtocol: 'caret-v1' });
  assert.equal(response.body.caretProtocol, 'caret-v1');
  assert.equal(start.caretProtocol, 'caret-v1');
  controller.json(caretOn);
  assert.deepEqual(await host.take(value => value.type === 'caret.watch'), { type: 'caret.watch', sessionId, on: true });
  host.json(report(sessionId));
  assert.deepEqual(await controller.take(value => value.type === 'caret'), report(sessionId));
  host.json(report('another-session'));
  host.json({ type: 'caret', sessionId, has: false });
  assert.equal((await controller.take(value => value.type === 'caret')).has, false, 'a retired session report is ignored');
  controller.json({ type: 'caret.watch', on: false });
  assert.equal((await host.take(value => value.type === 'caret.watch')).on, false);
  // A malformed client message ends the session; a malformed Host report drops the Host.
  controller.json({ type: 'caret.watch', on: 'yes' });
  assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_input');
  await closed(controller);
});

test('Released Hosts and ungranted sessions: no capability, no grant, watch ends the session, reports drop the Host', async t => {
  const f = await fixture(t); const old = await f.host(releasedHost);
  const inventory = await f.call('/desktops', { token: f.alice });
  assert.equal(inventory.body.desktops[0].host.capabilities.caretProtocol, undefined);
  const { response, controller, start, sessionId } = await f.stream(old, { caretProtocol: 'caret-v1' });
  assert.equal(response.body.caretProtocol, undefined, 'not refused, only ungranted');
  assert.equal(start.caretProtocol, undefined);
  controller.json(caretOn);
  assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_input');
  await closed(controller);
  const f2 = await fixture(t); const capable = await f2.host(caretHost);
  const plain = await f2.stream(capable);
  assert.equal(plain.response.body.caretProtocol, undefined, 'never granted unrequested');
  assert.equal(plain.start.caretProtocol, undefined);
  capable.json(report(plain.sessionId));
  await closed(capable);
  void sessionId;
});

test('A malformed connect field is refused before any session', async t => {
  const f = await fixture(t); await f.host(caretHost);
  for (const caretProtocol of ['caret-v2', 1, null, '']) assert.equal((await f.connect({ caretProtocol })).status, 400, String(caretProtocol));
  // RDP is not offered: refused as such before its fields are read.
  const rdp = await f.call('/desktops/pve-181/connect', { method: 'POST', token: f.alice, data: { mode: 'rdp', caretProtocol: 'caret-v1' } });
  assert.equal(rdp.status, 409); assert.equal(rdp.body.error.code, 'rdp_not_supported');
});
test('A caret rectangle must lie inside the streamed output', () => {
  const at = extra => report('s1', { has: true, x: 100, y: 200, w: 2, h: 30, sw: 1080, sh: 2400, ...extra });
  assert.notEqual(caretReport(at({ x: 1078, w: 2 })), null, 'touching the right edge is inside');
  assert.notEqual(caretReport(at({ y: 2370, h: 30 })), null, 'touching the bottom edge is inside');
  for (const extra of [{ x: 1079, w: 2 }, { x: 1080, w: 1 }, { y: 2371, h: 30 }, { x: 65535, w: 65535, sw: 65535 }, { w: 0 }, { h: 0 }, { w: 2000 }, { h: 5000 }]) {
    assert.equal(caretReport(at(extra)), null, JSON.stringify(extra));
  }
});

test('Caret reports are rate limited per session window, the excess is dropped', () => {
  const state = { at: 0, count: 0 };
  let allowed = 0;
  for (let n = 0; n < 200; n++) if (caretAllowed(state, 10_000 + n)) allowed++;
  assert.equal(allowed, caretMaxPerSecond);
  assert.equal(caretAllowed(state, 11_000), true, 'a new second starts a new window');
});

test('A Host that floods caret reports is not dropped and the client gets a bounded number', async t => {
  const f = await fixture(t); const host = await f.host(caretHost);
  const { controller, sessionId } = await f.stream(host, { caretProtocol: 'caret-v1' });
  controller.json(caretOn); await host.take(value => value.type === 'caret.watch');
  for (let n = 0; n < 300; n++) host.json(report(sessionId, { has: true, x: n % 1000, y: 10, w: 2, h: 30, sw: 1080, sh: 2400 }));
  await new Promise(resolve => setTimeout(resolve, 400));
  const received = controller.queue.filter(value => value.type === 'caret').length;
  assert.ok(received >= 1 && received <= caretMaxPerSecond + caretMaxPerSecond, `relayed ${received}`);
  assert.equal(host.readyState, WebSocket.OPEN, 'the flood does not drop the Host');
  host.json(report(sessionId, { has: true, x: 1, y: 1, w: 1080, h: 2400, sw: 1080, sh: 2400 }));
  host.json(report(sessionId, { has: true, x: 1, y: 1, w: 1080, h: 1, sw: 1080, sh: 2400 }));
  await closed(host);
});