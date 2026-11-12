import { captureAssignmentInput } from './fixtures/admin-assignment-input.mjs';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash, randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { printCapability, requestedPrint, negotiatePrint, validPrintTitle, printLimits } from '../src/print.mjs';

// Streaming printer redirection (print-v1): exact negotiation on the shared
// per-assignment `printers` permission, the relayed job transfer (order,
// window, digest), revocation that ends printing but not the stream, and
// released peers that keep exactly today's behaviour. Real broker and
// WebSockets, fake Host/client.
const origin = 'https://print-fixture.example';
const password = 'Print-fixture-password-only!';
const machineUuid = '12345678-1234-1234-1234-123456789ad2';
const releasedHost = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true };
const printHost = { ...releasedHost, printProtocol: 'print-v1' };
const streamInfo = { type: 'stream.info', codec: 'h264', width: 1920, height: 1080, sourceWidth: 1920, sourceHeight: 1080, fpsTarget: 60, audio: { sampleRate: 48000, channels: 2, format: 's16le', available: false } };
const deny = { drives: false, printers: false, usbDevices: false, smartCards: false, serialPorts: false };

async function fixture(t, { printers = true } = {}) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const hash = await hashPassword(password);
  store.addUser({ name: 'admin', passwordHash: hash, role: 'admin' });
  store.addUser({ name: 'alice', passwordHash: hash, role: 'user' });
  store.syncProxmox([{ vmid: 182, node: 'pve', name: 'VM 182', status: 'running' }], 'fixture');
  store.assign('alice', 'pve-182');
  if (printers) store.updateAssignmentPolicy('alice', 'pve-182', { devicePolicy: { ...deny, printers: true } }, 'fixture');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve-fixture.example', tokenId: 'fixture@pve!broker', secret: 'only-fixture-secret' }), 'fixture');
  const proxmox = { machineIdentity: async () => ({ machineUuid }) };
  const server = await buildServer({ store, vault, proxmox, publicOrigin: origin, relayOptions: { authorizationMs: 20, heartbeatMs: 200 }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const base = `http://127.0.0.1:${server.address().port}`;
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  async function call(path, { method = 'GET', data, token, cookie, csrf } = {}) {
    data = captureAssignmentInput(store, path, data);
    const response = await fetch(base + '/api/v1' + path, { method,
      headers: { ...(token ? { Authorization: 'Bearer ' + token } : {}), ...(cookie ? { Cookie: cookie } : {}), ...(method !== 'GET' ? { 'Content-Type': 'application/json', Origin: origin, ...(csrf ? { 'X-CSRF-Token': csrf } : {}) } : {}) },
      body: data === undefined ? undefined : JSON.stringify(data) });
    return { status: response.status, body: await response.json() };
  }
  const adminLogin = await fetch(base + '/api/v1/admin/login', { method: 'POST', headers: { 'Content-Type': 'application/json', Origin: origin }, body: JSON.stringify({ username: 'admin', password }) });
  const admin = { cookie: adminLogin.headers.get('set-cookie').split(';')[0], csrf: (await adminLogin.json()).csrfToken };
  const alice = (await call('/auth/login', { method: 'POST', data: { username: 'alice', password } })).body.sessionToken;
  const code = (await call('/admin/desktops/pve-182/enrollment', { ...admin, method: 'POST', data: {} })).body.code;
  const wsUrl = path => base.replace('http:', 'ws:') + path;
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
  const connect = extra => call('/desktops/pve-182/connect', { method: 'POST', token: alice, data: { mode: 'stream', ...extra } });
  async function stream(hostSocket, extra = {}) {
    const response = await connect(extra);
    assert.equal(response.status, 200, JSON.stringify(response.body));
    const controller = await socket(wsUrl('/api/v1/sessions/' + response.body.sessionId), 'Bearer ' + alice);
    const start = await hostSocket.take(value => value.type === 'session.start');
    hostSocket.json({ ...streamInfo }); await controller.take(value => value.type === 'stream.info');
    return { response, controller, start, sessionId: response.body.sessionId };
  }
  // Both ends of the print channel, ready.
  async function printPair(sessionId) {
    const printHostSocket = await socket(wsUrl(`/api/v1/hosts/sessions/${sessionId}/print`), 'Host ' + hostToken);
    const printClient = await socket(wsUrl(`/api/v1/sessions/${sessionId}/print`), 'Bearer ' + alice);
    const ready = await printClient.take(value => value.type === 'print.ready');
    assert.deepEqual(await printHostSocket.take(value => value.type === 'print.ready'), ready);
    return { printHost: printHostSocket, printClient, ready };
  }
  const refused = (path, authorization) => new Promise(resolve => {
    const ws = new WebSocket(wsUrl(path), { headers: { Authorization: authorization }, perMessageDeflate: false });
    ws.once('unexpected-response', (_, response) => { resolve(response.statusCode); ws.terminate(); });
    ws.once('open', () => { resolve('open'); ws.close(); });
    ws.once('error', () => {});
  });
  return { store, call, alice, admin, host, connect, stream, printPair, refused, token: () => hostToken };
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
const digest = bytes => createHash('sha256').update(bytes).digest('hex');
const chunks = (bytes, size = printLimits.chunkBytes) => Array.from({ length: Math.ceil(bytes.length / size) }, (_, n) => bytes.subarray(n * size, (n + 1) * size));

test('Capability, request, negotiation and title rules are exact', () => {
  assert.equal(printCapability(printHost), true);
  for (const value of [releasedHost, { ...releasedHost, printProtocol: 'print-v2' }, { ...releasedHost, printProtocol: true }, undefined, null])
    assert.equal(printCapability(value), false);
  assert.equal(requestedPrint('stream', undefined), false);
  assert.equal(requestedPrint('stream', 'print-v1'), true);
  for (const [mode, protocol] of [['rdp', 'print-v1'], ['stream', 'print-v2'], ['stream', ''], ['stream', 1], ['stream', null], ['stream', true]])
    assert.equal(code(() => requestedPrint(mode, protocol)), 'invalid_print_protocol', `${mode} ${protocol}`);
  const allowed = { ...deny, printers: true };
  assert.equal(negotiatePrint(true, { capabilities: printHost }, allowed), true);
  assert.equal(negotiatePrint(false, { capabilities: printHost }, allowed), false, 'never granted unrequested');
  assert.equal(negotiatePrint(true, { capabilities: releasedHost }, allowed), false, 'released Host: not granted, not refused');
  assert.equal(negotiatePrint(true, { capabilities: printHost }, deny), false, 'default denied');
  assert.equal(negotiatePrint(true, { capabilities: printHost }, { ...deny, drives: true }), false, 'only the printers permission grants');
  for (const value of ['', '보고서.docx', 'Q3 report - Word', 'x'.repeat(200)]) assert.equal(validPrintTitle(value), true, value);
  for (const value of ['x'.repeat(201), 'tab\there', 'line\nbreak', '\x7f', '\ud800', null, 1, undefined]) assert.equal(validPrintTitle(value), false, String(value));
  // The largest chunk stays within the print socket's message bound.
  const largest = JSON.stringify({ type: 'print.chunk', jobId: 'j'.repeat(64), offset: printLimits.maxJobBytes, data: Buffer.alloc(printLimits.chunkBytes).toString('base64') });
  assert.ok(Buffer.byteLength(largest) < 256 * 1024);
});

test('A VM that cannot print says why, only to a user the printers permission allows', async t => {
  for (const reason of ['print_spooler_unavailable', 'print_driver_unavailable']) {
    const allowed = await fixture(t);
    await allowed.host({ ...releasedHost, printUnavailableReason: reason });
    const caps = (await allowed.call('/desktops', { token: allowed.alice })).body.desktops[0].host.capabilities;
    assert.equal(caps.printProtocol, undefined);
    assert.equal(caps.printUnavailableReason, reason);
    const denied = await fixture(t, { printers: false });
    await denied.host({ ...releasedHost, printUnavailableReason: reason });
    const hidden = (await denied.call('/desktops', { token: denied.alice })).body.desktops[0].host.capabilities;
    assert.equal(hidden.printUnavailableReason, undefined, 'not shown without the permission');
  }
});

test('The printer reason is fixed: other values, or alongside a working print-v1, are dropped', async t => {
  for (const extra of [{ printUnavailableReason: 'anything' }, { printUnavailableReason: 3 }, { printUnavailableReason: 'print_driver_unavailable', service: false }, { printProtocol: 'print-v1', printUnavailableReason: 'print_driver_unavailable' }]) {
    const f = await fixture(t);
    const ws = await f.host({ ...releasedHost, ...extra }).catch(() => null);
    const caps = ws ? (await f.call('/desktops', { token: f.alice })).body.desktops[0]?.host?.capabilities : null;
    assert.equal(caps?.printUnavailableReason, undefined, JSON.stringify(extra));
  }
});

test('A granted session carries print-v1 to the client and the Host; a job reaches the client in order with its digest', async t => {
  const f = await fixture(t); const host = await f.host(printHost);
  const inventory = await f.call('/desktops', { token: f.alice });
  assert.equal(inventory.body.desktops[0].host.capabilities.printProtocol, 'print-v1');
  const { response, controller, start, sessionId } = await f.stream(host, { printProtocol: 'print-v1' });
  assert.equal(response.body.printProtocol, 'print-v1');
  assert.equal(response.body.printWebSocketUrl, `wss://print-fixture.example/api/v1/sessions/${sessionId}/print`);
  assert.equal(start.printProtocol, 'print-v1');
  assert.equal(start.printUrl, `wss://print-fixture.example/api/v1/hosts/sessions/${sessionId}/print`);
  const { printHost: hostPrint, printClient, ready } = await f.printPair(sessionId);
  assert.deepEqual(ready, { type: 'print.ready', sessionId, protocol: 'print-v1', limits: { chunkBytes: 131072, windowChunks: 8, maxJobBytes: 268435456 } });
  hostPrint.json({ type: 'print.queue', state: 'ready' });
  assert.deepEqual(await printClient.take(value => value.type === 'print.queue'), { type: 'print.queue', state: 'ready' });
  const pdf = randomBytes(printLimits.chunkBytes * 2 + 4321);
  const job = { type: 'print.job', jobId: 'job-1', format: 'pdf', size: pdf.length, title: '분기 보고서.docx' };
  hostPrint.json(job);
  assert.deepEqual(await printClient.take(value => value.type === 'print.job'), job);
  let offset = 0;
  for (const part of chunks(pdf)) {
    const chunk = { type: 'print.chunk', jobId: 'job-1', offset, data: part.toString('base64') };
    hostPrint.json(chunk);
    assert.deepEqual(await printClient.take(value => value.type === 'print.chunk'), chunk);
    offset += part.length;
    printClient.json({ type: 'print.ack', jobId: 'job-1', offset });
    assert.deepEqual(await hostPrint.take(value => value.type === 'print.ack'), { type: 'print.ack', jobId: 'job-1', offset });
  }
  hostPrint.json({ type: 'print.end', jobId: 'job-1', sha256: digest(pdf) });
  assert.equal((await printClient.take(value => value.type === 'print.end')).sha256, digest(pdf));
  printClient.json({ type: 'print.result', jobId: 'job-1', result: 'printed' });
  assert.deepEqual(await hostPrint.take(value => value.type === 'print.result'), { type: 'print.result', jobId: 'job-1', result: 'printed' });
  // A second job reuses the channel; a Host notice of a skipped job passes.
  hostPrint.json({ type: 'print.skipped', reason: 'print_job_too_large' });
  assert.deepEqual(await printClient.take(value => value.type === 'print.skipped'), { type: 'print.skipped', reason: 'print_job_too_large' });
  const small = Buffer.from('%PDF-1.7\n%fixture\n');
  hostPrint.json({ type: 'print.job', jobId: 'job-2', format: 'pdf', size: small.length, title: '' });
  hostPrint.json({ type: 'print.chunk', jobId: 'job-2', offset: 0, data: small.toString('base64') });
  hostPrint.json({ type: 'print.end', jobId: 'job-2', sha256: digest(small) });
  await printClient.take(value => value.type === 'print.end' && value.jobId === 'job-2');
  // The stream itself is untouched.
  controller.json({ type: 'ping', id: 'still-streaming' }); await host.take(value => value.type === 'ping');
});

test('The relay enforces order, the acknowledgement window, bounds and the digest; failures end printing, not the stream', async t => {
  const f = await fixture(t); const host = await f.host(printHost);
  const cases = [
    ['digest', (pdf, send) => { send({ type: 'print.chunk', jobId: 'j', offset: 0, data: pdf.toString('base64') }); send({ type: 'print.end', jobId: 'j', sha256: '0'.repeat(64) }); }, 'print_hash_mismatch', 1000],
    ['offset', (pdf, send) => { send({ type: 'print.chunk', jobId: 'j', offset: 5, data: pdf.subarray(0, 10).toString('base64') }); }, 'print_out_of_order', 1000],
    ['oversize', (pdf, send) => { send({ type: 'print.chunk', jobId: 'j', offset: 0, data: Buffer.alloc(2000).toString('base64') }); }, 'print_limits_exceeded', 1000],
    ['early end', (pdf, send) => { send({ type: 'print.end', jobId: 'j', sha256: digest(pdf) }); }, 'print_out_of_order', 1000],
    ['window', (pdf, send) => { for (const [n, part] of chunks(pdf).entries()) send({ type: 'print.chunk', jobId: 'j', offset: n * printLimits.chunkBytes, data: part.toString('base64') }); }, 'print_out_of_order', printLimits.chunkBytes * 9],
    ['noncanonical', (pdf, send) => { send({ type: 'print.chunk', jobId: 'j', offset: 0, data: 'QUJD\n' }); }, 'print_limits_exceeded', 1000],
    ['extra field', (pdf, send) => { send({ type: 'print.chunk', jobId: 'j', offset: 0, data: pdf.toString('base64'), sessionId: 'x' }); }, 'print_invalid_message', 1000],
  ];
  for (const [name, act, reason, size] of cases) {
    const { controller, sessionId } = await f.stream(host, { printProtocol: 'print-v1' });
    const { printHost: hostPrint, printClient } = await f.printPair(sessionId);
    const pdf = randomBytes(size);
    hostPrint.json({ type: 'print.job', jobId: 'j', format: 'pdf', size: pdf.length, title: name });
    await printClient.take(value => value.type === 'print.job');
    act(pdf, value => hostPrint.json(value));
    assert.equal((await printClient.take(value => value.type === 'print.error')).reason, reason, name);
    assert.equal((await hostPrint.take(value => value.type === 'print.error')).reason, reason, name);
    await closed(printClient); await closed(hostPrint);
    controller.json({ type: 'ping', id: 'p-' + name.replace(/ /g, '-') }); await host.take(value => value.type === 'ping');
    assert.equal(controller.readyState, WebSocket.OPEN, 'the stream outlives a print failure: ' + name);
    // One pair per session: a closed channel is not reopened.
    assert.equal(await f.refused(`/api/v1/sessions/${sessionId}/print`, 'Bearer ' + f.alice), 409);
    controller.json({ type: 'session.stop' }); await host.take(value => value.type === 'session.stop'); await closed(controller);
  }
  // A queue notice carries only fixed reasons.
  {
    const { controller, sessionId } = await f.stream(host, { printProtocol: 'print-v1' });
    const { printHost: hostPrint, printClient } = await f.printPair(sessionId);
    hostPrint.json({ type: 'print.queue', state: 'unavailable', reason: 'print_name_conflict' });
    assert.equal((await printClient.take(value => value.type === 'print.queue')).reason, 'print_name_conflict');
    hostPrint.json({ type: 'print.queue', state: 'unavailable', reason: 'C:\\Users\\alice\\secret.pdf' });
    assert.equal((await printClient.take(value => value.type === 'print.error')).reason, 'print_invalid_message');
    controller.json({ type: 'session.stop' }); await host.take(value => value.type === 'session.stop'); await closed(controller);
  }
  // Header bounds: oversize job, wrong format, malformed id.
  for (const [job, reason] of [[{ jobId: 'big', format: 'pdf', size: printLimits.maxJobBytes + 1, title: '' }, 'print_limits_exceeded'], [{ jobId: 'x', format: 'xps', size: 10, title: '' }, 'print_invalid_message'], [{ jobId: 'bad id', format: 'pdf', size: 10, title: '' }, 'print_invalid_message'], [{ jobId: 'x', format: 'pdf', size: 10, title: 'a\nb' }, 'print_invalid_message']]) {
    const { controller, sessionId } = await f.stream(host, { printProtocol: 'print-v1' });
    const { printHost: hostPrint, printClient } = await f.printPair(sessionId);
    hostPrint.json({ type: 'print.job', ...job });
    assert.equal((await printClient.take(value => value.type === 'print.error')).reason, reason, JSON.stringify(job));
    controller.json({ type: 'session.stop' }); await host.take(value => value.type === 'session.stop'); await closed(controller);
  }
});

test('A cancel ends only its job; chunks still in flight are dropped; the next job works', async t => {
  const f = await fixture(t); const host = await f.host(printHost);
  const { sessionId } = await f.stream(host, { printProtocol: 'print-v1' });
  const { printHost: hostPrint, printClient } = await f.printPair(sessionId);
  const pdf = randomBytes(printLimits.chunkBytes + 10);
  hostPrint.json({ type: 'print.job', jobId: 'a', format: 'pdf', size: pdf.length, title: 'A' });
  hostPrint.json({ type: 'print.chunk', jobId: 'a', offset: 0, data: chunks(pdf)[0].toString('base64') });
  await printClient.take(value => value.type === 'print.chunk');
  printClient.json({ type: 'print.cancel', jobId: 'a' });
  assert.deepEqual(await hostPrint.take(value => value.type === 'print.cancel'), { type: 'print.cancel', jobId: 'a' });
  // The Host had already sent more of the cancelled job.
  hostPrint.json({ type: 'print.chunk', jobId: 'a', offset: printLimits.chunkBytes, data: chunks(pdf)[1].toString('base64') });
  hostPrint.json({ type: 'print.end', jobId: 'a', sha256: digest(pdf) });
  printClient.json({ type: 'print.ack', jobId: 'a', offset: printLimits.chunkBytes });
  const next = Buffer.from('%PDF-1.4 next');
  hostPrint.json({ type: 'print.job', jobId: 'b', format: 'pdf', size: next.length, title: 'B' });
  hostPrint.json({ type: 'print.chunk', jobId: 'b', offset: 0, data: next.toString('base64') });
  hostPrint.json({ type: 'print.end', jobId: 'b', sha256: digest(next) });
  assert.equal((await printClient.take(value => value.type === 'print.end')).jobId, 'b');
  assert.equal(printClient.queue.some(value => value.jobId === 'a' && value.type !== 'print.job'), false, 'nothing of the cancelled job after the cancel');
  // A result for a job that never ended is refused.
  printClient.json({ type: 'print.result', jobId: 'a', result: 'printed' });
  assert.equal((await printClient.take(value => value.type === 'print.error')).reason, 'print_out_of_order');
});

test('Revoking the printers permission closes printing at once and keeps the stream; new print sockets are refused', async t => {
  const f = await fixture(t); const host = await f.host(printHost);
  const { controller, sessionId } = await f.stream(host, { printProtocol: 'print-v1' });
  const { printHost: hostPrint, printClient } = await f.printPair(sessionId);
  hostPrint.json({ type: 'print.job', jobId: 'j', format: 'pdf', size: 100, title: '' });
  await printClient.take(value => value.type === 'print.job');
  // The administrator console's atomic assignment update (relay hook).
  const changed = await f.call('/admin/assignments/batch', { ...f.admin, method: 'PUT', data: { username: 'alice', assignments: [{ desktopId: 'pve-182', devicePolicy: deny }] } });
  assert.equal(changed.status, 200, JSON.stringify(changed.body));
  assert.equal((await printClient.take(value => value.type === 'print.error')).reason, 'print_policy_changed');
  assert.equal((await hostPrint.take(value => value.type === 'print.error')).reason, 'print_policy_changed');
  await closed(printClient); await closed(hostPrint);
  controller.json({ type: 'ping', id: 'after-revoke' }); await host.take(value => value.type === 'ping');
  assert.equal(controller.readyState, WebSocket.OPEN);
  assert.equal(await f.refused(`/api/v1/hosts/sessions/${sessionId}/print`, 'Host ' + f.token()), 401);
  // The next connection is not granted at all.
  controller.json({ type: 'session.stop' }); await host.take(value => value.type === 'session.stop'); await closed(controller);
  const next = await f.stream(host, { printProtocol: 'print-v1' });
  assert.equal(next.response.body.printProtocol, undefined);
  assert.equal(next.start.printProtocol, undefined);
  assert.equal(next.start.printUrl, undefined);
});

test('The session end closes printing; only the session owner and its Host may attach', async t => {
  const f = await fixture(t); const host = await f.host(printHost);
  const { controller, sessionId } = await f.stream(host, { printProtocol: 'print-v1' });
  assert.equal(await f.refused(`/api/v1/sessions/${sessionId}/print`, 'Bearer ' + 'A'.repeat(43)), 401);
  assert.equal(await f.refused(`/api/v1/hosts/sessions/${sessionId}/print`, 'Host ' + 'A'.repeat(43)), 401);
  assert.equal(await f.refused(`/api/v1/hosts/sessions/${sessionId}/print`, 'Bearer ' + f.alice), 401);
  const { printHost: hostPrint, printClient } = await f.printPair(sessionId);
  controller.json({ type: 'session.stop' });
  assert.equal((await printClient.take(value => value.type === 'print.error')).reason, 'session_ended');
  assert.equal((await hostPrint.take(value => value.type === 'print.error')).reason, 'session_ended');
  await closed(printClient); await closed(hostPrint);
  // A client may not send the Host's messages.
  const second = await f.stream(host, { printProtocol: 'print-v1' });
  const pair = await f.printPair(second.sessionId);
  pair.printClient.json({ type: 'print.job', jobId: 'x', format: 'pdf', size: 1, title: '' });
  assert.equal((await pair.printHost.take(value => value.type === 'print.error')).reason, 'print_invalid_message');
});

test('Ungranted sessions never carry print-v1: denied permission, released Host, unrequested, malformed, RDP', async t => {
  {
    const f = await fixture(t, { printers: false }); const host = await f.host(printHost);
    const denied = await f.stream(host, { printProtocol: 'print-v1' });
    assert.equal(denied.response.body.printProtocol, undefined, 'default denied: not granted, not refused');
    assert.equal(denied.response.body.printWebSocketUrl, undefined);
    assert.equal(denied.start.printProtocol, undefined);
    assert.equal(await f.refused(`/api/v1/sessions/${denied.sessionId}/print`, 'Bearer ' + f.alice), 401);
    assert.equal(await f.refused(`/api/v1/hosts/sessions/${denied.sessionId}/print`, 'Host ' + f.token()), 401);
  }
  const f = await fixture(t); const host = await f.host(releasedHost);
  const inventory = await f.call('/desktops', { token: f.alice });
  assert.equal(inventory.body.desktops[0].host.capabilities.printProtocol, undefined);
  const released = await f.stream(host, { printProtocol: 'print-v1' });
  assert.equal(released.response.body.printProtocol, undefined, 'released Host: not granted, not refused');
  assert.equal(released.start.printProtocol, undefined);
  released.controller.json({ type: 'session.stop' }); await host.take(value => value.type === 'session.stop'); await closed(released.controller);
  const plain = await f.stream(host, {});
  assert.equal(Object.keys(plain.response.body).some(key => key.startsWith('print')), false, 'unrequested: today\'s response');
  assert.equal(Object.keys(plain.start).some(key => key.startsWith('print')), false, 'unrequested: today\'s session.start');
  plain.controller.json({ type: 'session.stop' }); await host.take(value => value.type === 'session.stop'); await closed(plain.controller);
  assert.equal((await f.connect({ printProtocol: 'print-v2' })).body.error.code, 'invalid_print_protocol');
  // RDP is not offered: refused as such, so it never carries print-v1.
  const rdp = await f.call('/desktops/pve-182/connect', { method: 'POST', token: f.alice, data: { mode: 'rdp', printProtocol: 'print-v1' } });
  assert.equal(rdp.status, 409); assert.equal(rdp.body.error.code, 'rdp_not_supported'); assert.equal(rdp.body.printProtocol, undefined);
});
