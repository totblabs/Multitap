import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { textInputCapability, requestedTextInput, negotiateTextInput, validTextInput, textInputMaxUnits } from '../src/text-input.mjs';
import { safeClientDiagnosticReport } from '../src/client-diagnostics.mjs';

// Negotiated committed-text input (text-input-v1): exact negotiation, strict
// relay validation and bounds, and released peers that keep exactly today's
// behaviour. Real broker and WebSockets, fake Host/client.
const origin = 'https://text-fixture.example';
const password = 'Text-fixture-password-only!';
const machineUuid = '12345678-1234-1234-1234-123456789ac1';
const releasedHost = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true };
const textHost = { ...releasedHost, textInputProtocol: 'text-input-v1' };
const streamInfo = { type: 'stream.info', codec: 'h264', width: 1920, height: 1080, sourceWidth: 1920, sourceHeight: 1080, fpsTarget: 60, audio: { sampleRate: 48000, channels: 2, format: 's16le', available: false } };
const text = value => ({ type: 'input', event: 'text', text: value });

async function fixture(t) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const hash = await hashPassword(password);
  store.addUser({ name: 'admin', passwordHash: hash, role: 'admin' });
  store.addUser({ name: 'alice', passwordHash: hash, role: 'user' });
  store.syncProxmox([{ vmid: 173, node: 'pve', name: 'VM 173', status: 'running' }], 'fixture');
  store.assign('alice', 'pve-173');
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
  const code = (await call('/admin/desktops/pve-173/enrollment', { ...admin, method: 'POST', data: {} })).body.code;
  const wsUrl = path => base.replace('http:', 'ws:') + path;
  async function host(capabilities) {
    const enrolled = await call('/hosts/enroll', { method: 'POST', data: { code, hostname: 'Fixture VM', machineUuid, version: '0.2.70' } });
    assert.equal(enrolled.status, 201);
    const ws = await socket(wsUrl('/api/v1/hosts/channel'), 'Host ' + enrolled.body.hostToken);
    ws.json({ type: 'host.capabilities', capabilities });
    assert.equal((await ws.take(value => value.type === 'host.capabilities.ack')).accepted, true);
    return ws;
  }
  const connect = extra => call('/desktops/pve-173/connect', { method: 'POST', token: alice, data: { mode: 'stream', ...extra } });
  const client = sessionId => socket(wsUrl('/api/v1/sessions/' + sessionId), 'Bearer ' + alice);
  async function stream(hostSocket, extra = {}) {
    const response = await connect(extra);
    assert.equal(response.status, 200, JSON.stringify(response.body));
    const controller = await client(response.body.sessionId);
    const start = await hostSocket.take(value => value.type === 'session.start');
    hostSocket.json(streamInfo); await controller.take(value => value.type === 'stream.info');
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

test('Capability, request, negotiation and message rules are exact', () => {
  assert.equal(textInputCapability(textHost), true);
  for (const value of [releasedHost, { ...releasedHost, textInputProtocol: 'text-input-v2' }, { ...releasedHost, textInputProtocol: true }, undefined, null])
    assert.equal(textInputCapability(value), false);
  assert.equal(requestedTextInput('stream', undefined), false);
  assert.equal(requestedTextInput('stream', 'text-input-v1'), true);
  for (const [mode, protocol] of [['rdp', 'text-input-v1'], ['stream', 'text-input-v2'], ['stream', ''], ['stream', 1], ['stream', null], ['stream', true]])
    assert.equal(code(() => requestedTextInput(mode, protocol)), 'invalid_text_input_protocol', `${mode} ${protocol}`);
  assert.equal(negotiateTextInput(true, { capabilities: textHost }, null), true);
  assert.equal(negotiateTextInput(false, { capabilities: textHost }, null), false, 'never granted unrequested');
  assert.equal(negotiateTextInput(true, { capabilities: releasedHost }, null), false, 'released Host: keys, not refused');
  assert.equal(negotiateTextInput(true, { capabilities: textHost }, [{ id: '0' }]), false, 'independent outputs keep keys');

  for (const value of ['a', '안녕하세요', '😀', 'ⓐ mixed 한글 😀 text', 'x'.repeat(textInputMaxUnits), '😀'.repeat(textInputMaxUnits / 2)])
    assert.equal(validTextInput(text(value)), true, value);
  for (const value of ['', 'x'.repeat(textInputMaxUnits + 1), '😀'.repeat(textInputMaxUnits / 2) + 'a', 'a\nb', '\t', '\r', 'a\u007f', '\u0085', '\ud83d', 'a\ude00', 1, null, ['a']])
    assert.equal(validTextInput(text(value)), false, JSON.stringify(value));
  for (const value of [{ ...text('a'), sample: 1 }, { ...text('a'), x: 0.5 }, { type: 'input', event: 'key', text: 'a' }, { type: 'input', text: 'a' }, null, [], 'a'])
    assert.equal(validTextInput(value), false, JSON.stringify(value));
});

test('A capable Host advertises it; the grant reaches the client and session.start; text is forwarded exactly', async t => {
  const f = await fixture(t); const host = await f.host(textHost);
  const inventory = await f.call('/desktops', { token: f.alice });
  assert.equal(inventory.body.desktops[0].host.capabilities.textInputProtocol, 'text-input-v1');
  const { response, controller, start } = await f.stream(host, { textInputProtocol: 'text-input-v1' });
  assert.equal(response.body.textInputProtocol, 'text-input-v1');
  assert.equal(start.textInputProtocol, 'text-input-v1');
  for (const value of ['안녕', '😀', 'a b']) {
    controller.json(text(value));
    assert.deepEqual(await host.take(item => item.type === 'input'), text(value));
  }
  // Keys keep working in the same session, in order.
  const enter = { type: 'input', event: 'key', vk: 13, scan: 28, down: true, extended: false };
  controller.json(enter); assert.deepEqual(await host.take(item => item.type === 'input'), enter);
  // A control character (or any malformed text) ends the session.
  controller.json(text('a\nb'));
  assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_input');
  await closed(controller);
  assert.equal(host.queue.some(item => item.type === 'input' && item.text === 'a\nb'), false);
});

test('Ungranted sessions never carry text: released Host, unrequested, RDP', async t => {
  const f = await fixture(t); const host = await f.host(releasedHost);
  const inventory = await f.call('/desktops', { token: f.alice });
  assert.equal(inventory.body.desktops[0].host.capabilities.textInputProtocol, undefined);
  const released = await f.stream(host, { textInputProtocol: 'text-input-v1' });
  assert.equal(released.response.body.textInputProtocol, undefined, 'requested from a released Host: not granted, not refused');
  assert.equal(released.start.textInputProtocol, undefined);
  released.controller.json(text('a'));
  assert.equal((await released.controller.take(value => value.type === 'session.ended')).reason, 'invalid_input');
  await host.take(value => value.type === 'session.stop'); await closed(released.controller);
  assert.equal(host.queue.some(item => item.type === 'input' && item.event === 'text'), false, 'ungranted text never reaches the Host');
  assert.equal((await f.connect({ textInputProtocol: 'text-input-v2' })).body.error.code, 'invalid_text_input_protocol');
  // RDP is not offered: refused as such, so it never carries text.
  const rdp = await f.call('/desktops/pve-173/connect', { method: 'POST', token: f.alice, data: { mode: 'rdp', textInputProtocol: 'text-input-v1' } });
  assert.equal(rdp.status, 409); assert.equal(rdp.body.error.code, 'rdp_not_supported'); assert.equal(rdp.body.textInputProtocol, undefined);
});

test('A capable Host without the client request keeps keys only', async t => {
  const f = await fixture(t); const host = await f.host(textHost);
  const { response, controller, start } = await f.stream(host, {});
  assert.equal(response.body.textInputProtocol, undefined);
  assert.equal(start.textInputProtocol, undefined);
  controller.json(text('a'));
  assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_input');
});

test('Android input counters pass the client diagnostics projection; unknown fields are dropped', () => {
  const report = safeClientDiagnosticReport({ protocol: 'client-diagnostics-v1', runId: 'a'.repeat(32), sequence: 1, version: null,
    events: [{ event: 'streamStats', time: '2026-10-26T12:00:00.000Z', values: { samples: 10, touchMode: 1, taps: 4, rightClicks: 1, dragStarts: 2, wheelSteps: 5, pinches: 1, keyboardOpens: 1, textCommits: 3, textUnits: 9, textKeyChars: 0, textDropped: 0, keyPresses: 2, mouseButtons: 0, textInput: true, futureField: 7, words: 'never' } }] });
  assert.deepEqual(report.events[0].values, { samples: 10, touchMode: 1, taps: 4, rightClicks: 1, dragStarts: 2, wheelSteps: 5, pinches: 1, keyboardOpens: 1, textCommits: 3, textUnits: 9, textKeyChars: 0, textDropped: 0, keyPresses: 2, mouseButtons: 0, textInput: true });
});
