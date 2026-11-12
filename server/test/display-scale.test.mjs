import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { displayScaleCapability, displayScaleOutputsCapability, requestedDisplayScale, requestedDisplayScaleOutputs, negotiateDisplayScale, negotiateDisplayScaleControl, validDisplayScaleRequest, displayScaleResult, displayScalePercents } from '../src/display-scale.mjs';
import { safeClientDiagnosticReport } from '../src/client-diagnostics.mjs';
import { requests as trackRequests, options as trackOptions, info as trackInfo } from './track-fixture.mjs';

// Negotiated remote display scale (display-scale-v1): exact negotiation, strict
// relay validation in both directions, and released peers that keep exactly
// today's behaviour. Real broker and WebSockets, fake Host/client.
const origin = 'https://scale-fixture.example';
const password = 'Scale-fixture-password-only!';
const machineUuid = '12345678-1234-1234-1234-123456789ac2';
const releasedHost = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true };
const scaleHost = { ...releasedHost, displayScaleProtocol: 'display-scale-v1' };
const streamInfo = { type: 'stream.info', codec: 'h264', width: 2400, height: 1080, sourceWidth: 2400, sourceHeight: 1080, fpsTarget: 60, audio: { sampleRate: 48000, channels: 2, format: 's16le', available: false } };
const applied = (sessionId, extra = {}) => ({ type: 'display.scale.applied', sessionId, protocol: 'display-scale-v1', ok: true, scale: 175, maximum: 175, code: 0, ...extra });
// display-scale-outputs-v1: a dual-output Host and its per-output result.
const trackMonitors = trackRequests.map(t => ({ id: t.id, width: t.width, height: t.height, primary: t.id === '0' }));
const outputsHost = { ...scaleHost, displayScaleOutputsProtocol: 'display-scale-outputs-v1', videoTrackProtocol: 'tracks-v1', trackViewportProvisioning: true, monitors: trackMonitors };
const trackOnlyScaleHost = { ...scaleHost, videoTrackProtocol: 'tracks-v1', trackViewportProvisioning: true, monitors: trackMonitors };
const outputsRequest = { displayScaleProtocol: 'display-scale-v1', displayScaleOutputsProtocol: 'display-scale-outputs-v1' };
const trackConnect = extra => ({ options: trackOptions, mediaProtocol: 'tracks-v1', tracks: trackRequests, ...extra });
const perOutput = (sessionId, extra = {}) => applied(sessionId, { scale: 175, maximum: 200, outputs: [{ id: '0', ok: true, scale: 175, maximum: 175, code: 0 }, { id: '1', ok: true, scale: 200, maximum: 200, code: 0 }], ...extra });

async function fixture(t, { heartbeatMs = 200 } = {}) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const hash = await hashPassword(password);
  store.addUser({ name: 'admin', passwordHash: hash, role: 'admin' });
  store.addUser({ name: 'alice', passwordHash: hash, role: 'user' });
  store.syncProxmox([{ vmid: 174, node: 'pve', name: 'VM 174', status: 'running' }], 'fixture');
  store.assign('alice', 'pve-174');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve-fixture.example', tokenId: 'fixture@pve!broker', secret: 'only-fixture-secret' }), 'fixture');
  const proxmox = { machineIdentity: async () => ({ machineUuid }) };
  const server = await buildServer({ store, vault, proxmox, publicOrigin: origin, relayOptions: { authorizationMs: 20, heartbeatMs }, logger: { error: value => assert.fail(value) } });
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
  const code = (await call('/admin/desktops/pve-174/enrollment', { ...admin, method: 'POST', data: {} })).body.code;
  const wsUrl = path => base.replace('http:', 'ws:') + path;
  let hostToken;
  async function host(capabilities) {
    const enrolled = await call('/hosts/enroll', { method: 'POST', data: { code, hostname: 'Fixture VM', machineUuid, version: '0.2.72' } });
    assert.equal(enrolled.status, 201); hostToken = enrolled.body.hostToken;
    const ws = await socket(wsUrl('/api/v1/hosts/channel'), 'Host ' + enrolled.body.hostToken);
    ws.json({ type: 'host.capabilities', capabilities });
    assert.equal((await ws.take(value => value.type === 'host.capabilities.ack')).accepted, true);
    return ws;
  }
  const connect = extra => call('/desktops/pve-174/connect', { method: 'POST', token: alice, data: { mode: 'stream', ...extra } });
  const client = sessionId => socket(wsUrl('/api/v1/sessions/' + sessionId), 'Bearer ' + alice);
  async function stream(hostSocket, extra = {}) {
    const response = await connect(extra);
    assert.equal(response.status, 200, JSON.stringify(response.body));
    const controller = await client(response.body.sessionId);
    const start = await hostSocket.take(value => value.type === 'session.start');
    hostSocket.json(streamInfo); await controller.take(value => value.type === 'stream.info');
    return { response, controller, start, sessionId: response.body.sessionId };
  }
  // A dual-output (tracks-v1) session with its track tunnel and first metadata.
  async function tracks(hostSocket, extra = {}) {
    const response = await connect(trackConnect(extra));
    assert.equal(response.status, 200, JSON.stringify(response.body));
    const controller = await client(response.body.sessionId);
    const start = await hostSocket.take(value => value.type === 'session.start');
    const source = await socket(wsUrl(new URL(start.tunnelUrl).pathname), 'Host ' + hostToken);
    source.json(trackInfo(response.body.sessionId)); await controller.take(value => value.type === 'stream.tracks.info');
    return { response, controller, start, source, sessionId: response.body.sessionId };
  }
  return { call, alice, host, connect, client, stream, tracks };
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
  assert.equal(displayScaleCapability(scaleHost), true);
  for (const value of [releasedHost, { ...releasedHost, displayScaleProtocol: 'display-scale-v2' }, { ...releasedHost, displayScaleProtocol: true }, undefined, null])
    assert.equal(displayScaleCapability(value), false);
  assert.equal(requestedDisplayScale('stream', undefined, undefined), null);
  assert.equal(requestedDisplayScale('stream', 'display-scale-v1', undefined), null, 'runtime control requests no initial DPI value');
  assert.equal(negotiateDisplayScaleControl('display-scale-v1', { capabilities: scaleHost }, null), true);
  assert.equal(negotiateDisplayScaleControl(undefined, { capabilities: scaleHost }, null), false);
  assert.equal(negotiateDisplayScaleControl('display-scale-v1', { capabilities: releasedHost }, null), false);
  assert.equal(negotiateDisplayScaleControl('display-scale-v1', { capabilities: scaleHost }, [{ id: '0' }]), false);
  for (const percent of displayScalePercents) assert.equal(requestedDisplayScale('stream', 'display-scale-v1', percent), percent);
  for (const [mode, protocol, scale] of [['rdp', 'display-scale-v1', 150], ['stream', 'display-scale-v2', 150], ['stream', undefined, 150], ['stream', 'display-scale-v1', 160], ['stream', 'display-scale-v1', 99], ['stream', 'display-scale-v1', 600], ['stream', 'display-scale-v1', '150'], ['stream', 'display-scale-v1', 150.5], ['stream', 'display-scale-v1', null]])
    assert.equal(code(() => requestedDisplayScale(mode, protocol, scale)), 'invalid_display_scale', `${mode} ${protocol} ${scale}`);
  assert.equal(negotiateDisplayScale(175, { capabilities: scaleHost }, null), 175);
  assert.equal(negotiateDisplayScale(null, { capabilities: scaleHost }, null), null, 'never granted unrequested');
  assert.equal(negotiateDisplayScale(175, { capabilities: releasedHost }, null), null, 'released Host: not granted, not refused');
  assert.equal(negotiateDisplayScale(175, { capabilities: scaleHost }, [{ id: '0' }]), null, 'independent outputs never');

  assert.equal(validDisplayScaleRequest({ type: 'display.scale', scale: 200 }), true);
  for (const value of [{ type: 'display.scale', scale: 160 }, { type: 'display.scale', scale: '200' }, { type: 'display.scale' }, { type: 'display.scale', scale: 200, sessionId: 'x' }, { type: 'display.scales', scale: 200 }, null, [], 'display.scale'])
    assert.equal(validDisplayScaleRequest(value), false, JSON.stringify(value));

  assert.deepEqual(displayScaleResult(applied('s1')), applied('s1'));
  assert.deepEqual(displayScaleResult(applied('s1', { ok: false, scale: 0, maximum: 0, code: 1 })), applied('s1', { ok: false, scale: 0, maximum: 0, code: 1 }));
  for (const value of [applied('s1', { protocol: 'display-scale-v2' }), applied('s1', { scale: 160 }), applied('s1', { maximum: 99 }), applied('s1', { code: 6 }), applied('s1', { code: 1 }), applied('s1', { scale: 0 }), applied('s1', { extra: 1 }), applied(''), applied('s1', { ok: 'yes' }), null, []])
    assert.equal(displayScaleResult(value), null, JSON.stringify(value));
});

test('A capable Host advertises it; the grant reaches the client and session.start; requests and results are relayed exactly', async t => {
  const f = await fixture(t); const host = await f.host(scaleHost);
  const inventory = await f.call('/desktops', { token: f.alice });
  assert.equal(inventory.body.desktops[0].host.capabilities.displayScaleProtocol, 'display-scale-v1');
  const { response, controller, start, sessionId } = await f.stream(host, { displayScaleProtocol: 'display-scale-v1', displayScale: 175 });
  assert.equal(response.body.displayScaleProtocol, 'display-scale-v1');
  assert.equal(response.body.displayScale, 175);
  assert.equal(start.displayScaleProtocol, 'display-scale-v1');
  assert.equal(start.displayScale, 175);
  // The Host's result reaches the client with its fixed fields only.
  host.json(applied(sessionId));
  assert.deepEqual(await controller.take(value => value.type === 'display.scale.applied'), applied(sessionId));
  // A result of a retired session is ignored.
  host.json(applied('another-session'));
  // A runtime request carries the session id to the Host.
  controller.json({ type: 'display.scale', scale: 125 });
  assert.deepEqual(await host.take(value => value.type === 'display.scale'), { type: 'display.scale', sessionId, scale: 125 });
  // Anything else ends the session.
  controller.json({ type: 'display.scale', scale: 130 });
  assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_input');
  await closed(controller);
  assert.equal(controller.queue.some(value => value.type === 'display.scale.applied' && value.sessionId === 'another-session'), false);
  assert.equal(host.queue.some(value => value.type === 'display.scale' && value.scale === 130), false);
});

test('A malformed Host result drops the Host', async t => {
  const f = await fixture(t); const host = await f.host(scaleHost);
  const { sessionId } = await f.stream(host, { displayScaleProtocol: 'display-scale-v1', displayScale: 150 });
  host.json(applied(sessionId, { scale: 160 }));
  await closed(host);
});

test('Marker-only grant preserves initial DPI and keeps runtime selections in one session', async t => {
  const f = await fixture(t); const host = await f.host(scaleHost);
  const { response, controller, start, sessionId } = await f.stream(host, { displayScaleProtocol: 'display-scale-v1' });
  assert.equal(response.body.displayScaleProtocol, 'display-scale-v1');
  assert.equal(Object.hasOwn(response.body, 'displayScale'), false);
  assert.equal(start.displayScaleProtocol, 'display-scale-v1');
  assert.equal(Object.hasOwn(start, 'displayScale'), false, 'Host receives no initial DPI value to apply');
  for (const scale of [125, 150, 200, 100]) {
    controller.json({ type: 'display.scale', scale });
    assert.deepEqual(await host.take(value => value.type === 'display.scale'), { type: 'display.scale', sessionId, scale });
    host.json(applied(sessionId, { scale, maximum: 200 }));
    assert.deepEqual(await controller.take(value => value.type === 'display.scale.applied'), applied(sessionId, { scale, maximum: 200 }));
    assert.equal(controller.readyState, WebSocket.OPEN);
    assert.equal(host.queue.some(value => ['session.stop', 'session.start'].includes(value.type)), false);
  }
  controller.close();
});

test('Marker-only request remains ungranted for a released Host', async t => {
  const f = await fixture(t); const host = await f.host(releasedHost);
  const { response, controller, start } = await f.stream(host, { displayScaleProtocol: 'display-scale-v1' });
  assert.equal(Object.hasOwn(response.body, 'displayScaleProtocol'), false);
  assert.equal(Object.hasOwn(start, 'displayScaleProtocol'), false);
  controller.json({ type: 'display.scale', scale: 125 });
  assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_input');
  assert.equal(host.queue.some(value => value.type === 'display.scale'), false);
  await closed(controller);
});

test('Ungranted sessions never carry a scale: released Host, unrequested, malformed, RDP', async t => {
  const f = await fixture(t); const host = await f.host(releasedHost);
  const inventory = await f.call('/desktops', { token: f.alice });
  assert.equal(inventory.body.desktops[0].host.capabilities.displayScaleProtocol, undefined);
  const released = await f.stream(host, { displayScaleProtocol: 'display-scale-v1', displayScale: 175 });
  assert.equal(released.response.body.displayScaleProtocol, undefined, 'requested from a released Host: not granted, not refused');
  assert.equal(released.response.body.displayScale, undefined);
  assert.equal(released.start.displayScaleProtocol, undefined);
  assert.equal(released.start.displayScale, undefined);
  released.controller.json({ type: 'display.scale', scale: 150 });
  assert.equal((await released.controller.take(value => value.type === 'session.ended')).reason, 'invalid_input');
  await host.take(value => value.type === 'session.stop'); await closed(released.controller);
  assert.equal(host.queue.some(item => item.type === 'display.scale'), false, 'ungranted requests never reach the Host');
  assert.equal((await f.connect({ displayScaleProtocol: 'display-scale-v1', displayScale: 160 })).body.error.code, 'invalid_display_scale');
  assert.equal((await f.connect({ displayScale: 150 })).body.error.code, 'invalid_display_scale');
  // RDP is not offered: refused as such, so it never carries a scale.
  const rdp = await f.call('/desktops/pve-174/connect', { method: 'POST', token: f.alice, data: { mode: 'rdp', displayScaleProtocol: 'display-scale-v1', displayScale: 150 } });
  assert.equal(rdp.status, 409); assert.equal(rdp.body.error.code, 'rdp_not_supported'); assert.equal(rdp.body.displayScale, undefined);
});

test('A capable Host without the client request keeps today\'s session.start', async t => {
  const f = await fixture(t); const host = await f.host(scaleHost);
  const { response, controller, start, sessionId } = await f.stream(host, {});
  assert.equal(response.body.displayScaleProtocol, undefined);
  assert.equal(start.displayScaleProtocol, undefined);
  assert.equal(start.displayScale, undefined);
  // A Host result for an ungranted session is an invalid control.
  host.json(applied(sessionId));
  await closed(host);
  controller.close();
});

test('Independent outputs: capability, request, negotiation and per-output result rules are exact', () => {
  const tracks = [{ id: '0' }, { id: '1' }];
  assert.equal(displayScaleOutputsCapability(outputsHost), true);
  for (const value of [scaleHost, trackOnlyScaleHost, { ...outputsHost, displayScaleProtocol: undefined }, { ...outputsHost, displayScaleOutputsProtocol: 'display-scale-outputs-v2' }, undefined])
    assert.equal(displayScaleOutputsCapability(value), false, JSON.stringify(value));
  assert.equal(requestedDisplayScaleOutputs('stream', 'display-scale-v1', undefined, tracks), false);
  assert.equal(requestedDisplayScaleOutputs('stream', 'display-scale-v1', 'display-scale-outputs-v1', tracks), true);
  for (const [mode, protocol, outputs, requested] of [['stream', 'display-scale-v1', 'display-scale-outputs-v1', null], ['stream', undefined, 'display-scale-outputs-v1', tracks],
    ['stream', 'display-scale-v1', 'display-scale-outputs-v2', tracks], ['stream', 'display-scale-v1', true, tracks], ['rdp', 'display-scale-v1', 'display-scale-outputs-v1', tracks]])
    assert.equal(code(() => requestedDisplayScaleOutputs(mode, protocol, outputs, requested)), 'invalid_display_scale', `${mode} ${protocol} ${outputs} ${!!requested}`);
  assert.equal(negotiateDisplayScaleControl('display-scale-v1', { capabilities: outputsHost }, tracks, true), true);
  assert.equal(negotiateDisplayScaleControl('display-scale-v1', { capabilities: outputsHost }, tracks, false), false, 'tracks without the outputs request: never');
  assert.equal(negotiateDisplayScaleControl('display-scale-v1', { capabilities: trackOnlyScaleHost }, tracks, true), false, 'a Host without outputs: not granted, not refused');
  assert.equal(negotiateDisplayScaleControl('display-scale-v1', { capabilities: outputsHost }, null), true, 'single output unchanged');
  assert.equal(negotiateDisplayScale(150, { capabilities: outputsHost }, tracks, true), 150);
  assert.equal(negotiateDisplayScale(150, { capabilities: trackOnlyScaleHost }, tracks, true), null);

  assert.deepEqual(displayScaleResult(perOutput('s1'), true), perOutput('s1'));
  const partial = perOutput('s1', { ok: false, code: 1, outputs: [{ id: '0', ok: true, scale: 175, maximum: 175, code: 0 }, { id: '1', ok: false, scale: 0, maximum: 0, code: 1 }] });
  assert.deepEqual(displayScaleResult(partial, true), partial);
  assert.equal(displayScaleResult(perOutput('s1')), null, 'outputs only in a session granted them');
  assert.equal(displayScaleResult(applied('s1'), true), null, 'a granted session needs every output');
  const entry = { id: '0', ok: true, scale: 175, maximum: 175, code: 0 };
  for (const outputs of [[], [entry, { ...entry, id: '1' }, { ...entry, id: '1' }], [entry, entry], [{ ...entry, id: '2' }], [{ ...entry, id: 0 }], [{ ...entry, scale: 160 }], [{ ...entry, code: 6 }],
    [{ ...entry, ok: true, scale: 0 }], [{ ...entry, ok: true, code: 3 }], [{ ...entry, extra: 1 }], [{ id: '0', ok: true, scale: 175, code: 0 }], [null], {}, 'outputs'])
    assert.equal(displayScaleResult(perOutput('s1', { outputs }), true), null, JSON.stringify(outputs));
});

test('A dual-output session gets the outputs grant end to end: inventory, response, session.start, request, per-output result', async t => {
  const f = await fixture(t, { heartbeatMs: 10000 }); const host = await f.host(outputsHost);
  const inventory = await f.call('/desktops', { token: f.alice });
  assert.equal(inventory.body.desktops[0].host.capabilities.displayScaleOutputsProtocol, 'display-scale-outputs-v1');
  const { response, controller, start, source, sessionId } = await f.tracks(host, { ...outputsRequest, displayScale: 150 });
  assert.equal(response.body.transport, 'media-tracks-v1');
  assert.equal(response.body.displayScaleProtocol, 'display-scale-v1');
  assert.equal(response.body.displayScaleOutputsProtocol, 'display-scale-outputs-v1');
  assert.equal(response.body.displayScale, 150);
  assert.equal(start.displayScaleProtocol, 'display-scale-v1');
  assert.equal(start.displayScaleOutputsProtocol, 'display-scale-outputs-v1');
  assert.equal(start.displayScale, 150);
  // The worker's per-output result arrives on the track tunnel and is relayed exactly.
  source.json(perOutput(sessionId));
  assert.deepEqual(await controller.take(value => value.type === 'display.scale.applied'), perOutput(sessionId));
  // A runtime request goes to the Host's control channel, which owns the worker.
  controller.json({ type: 'display.scale', scale: 125 });
  assert.deepEqual(await host.take(value => value.type === 'display.scale'), { type: 'display.scale', sessionId, scale: 125 });
  assert.equal(controller.readyState, WebSocket.OPEN);
  // A malformed per-output result ends the session.
  source.json(perOutput(sessionId, { outputs: [{ id: '2', ok: true, scale: 175, maximum: 175, code: 0 }] }));
  assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_track_control');
  await closed(controller);
});

test('A granted dual-output session takes its result only from the track tunnel, never the control channel', { timeout: 10000 }, async t => {
  const f = await fixture(t, { heartbeatMs: 10000 }); const host = await f.host(outputsHost);
  const { controller, sessionId } = await f.tracks(host, { ...outputsRequest, displayScale: 150 });
  host.json(applied(sessionId));
  await closed(host);
  assert.equal(controller.queue.some(value => value.type === 'display.scale.applied'), false);
});

test('Independent outputs without the outputs grant keep today\'s behaviour', async t => {
  // A client that asks only display-scale-v1 with tracks: not granted, a request ends the session.
  { const f = await fixture(t, { heartbeatMs: 10000 }); const host = await f.host(outputsHost);
    const { response, controller, start } = await f.tracks(host, { displayScaleProtocol: 'display-scale-v1' });
    assert.equal(response.body.displayScaleProtocol, undefined); assert.equal(response.body.displayScaleOutputsProtocol, undefined);
    assert.equal('displayScaleProtocol' in start, false); assert.equal('displayScaleOutputsProtocol' in start, false);
    controller.json({ type: 'display.scale', scale: 125 });
    assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_input');
    assert.equal(host.queue.some(value => value.type === 'display.scale'), false);
    await closed(controller); }
  // A Host with display-scale-v1 only: the outputs request is not granted and its result would end the session.
  { const f = await fixture(t, { heartbeatMs: 10000 }); const host = await f.host(trackOnlyScaleHost);
    const inventory = await f.call('/desktops', { token: f.alice });
    assert.equal(inventory.body.desktops[0].host.capabilities.displayScaleOutputsProtocol, undefined);
    const { response, controller, start, source, sessionId } = await f.tracks(host, outputsRequest);
    assert.equal(response.body.displayScaleProtocol, undefined); assert.equal('displayScaleOutputsProtocol' in start, false);
    source.json(perOutput(sessionId));
    assert.equal((await controller.take(value => value.type === 'session.ended')).reason, 'invalid_track_control');
    await closed(controller); }
  // Malformed requests are refused before any session.
  { const f = await fixture(t, { heartbeatMs: 10000 }); await f.host(outputsHost);
    for (const extra of [{ displayScaleOutputsProtocol: 'display-scale-outputs-v1' }, { ...outputsRequest, displayScaleOutputsProtocol: 'display-scale-outputs-v2' }])
      assert.equal((await f.connect(trackConnect(extra))).body.error.code, 'invalid_display_scale', JSON.stringify(extra));
    assert.equal((await f.connect(outputsRequest)).body.error.code, 'invalid_display_scale', 'the outputs marker without tracks-v1'); }
});

test('Display scale diagnostics pass the client diagnostics projection', () => {
  const report = safeClientDiagnosticReport({ protocol: 'client-diagnostics-v1', runId: 'b'.repeat(32), sequence: 1, version: null,
    events: [{ event: 'streamStats', time: '2026-10-26T12:00:00.000Z', values: { samples: 10, displayScale: 175, displayScaleWanted: 200, pixelScale: 1, futureField: 7 } }] });
  assert.deepEqual(report.events[0].values, { samples: 10, displayScale: 175, displayScaleWanted: 200, pixelScale: 1 });
});
