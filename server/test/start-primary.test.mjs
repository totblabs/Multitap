import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { HostRequests } from '../src/host-requests.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword, newToken } from '../src/security.mjs';
import { startPrimaryProtocol } from '../src/initial-viewport.mjs';

// start-primary-v1: a Host whose stream start makes owned "0" primary itself
// (display manager v2) says so, and the broker then admits a physical PC whose
// "0" is on beside its monitor and not primary at rest. Real broker and
// WebSockets, fake Hosts and client: one approved physical PC and one VM.
const origin = 'https://start-primary-fixture.example';
const password = 'Start-primary-fixture-password!';
const vmUuid = '12345678-1234-1234-1234-123456789af2';
const pcUuid = 'a1234567-89ab-cdef-8123-0123456789b0';
const options = { maxWidth: 1280, maxHeight: 720, fps: 60, bitrate: 8000000, audio: false, monitor: '0' };
const viewport = { monitor: '0', width: 2560, height: 1440, fps: 60 };
const request = { mode: 'stream', options, resizeProtocol: 'epoch-v1', initialViewportProtocol: 'epoch-v1', initialViewport: viewport };
// What a v2 Host at rest reports: the console profile, "0" on and not primary.
const atRest = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true, dynamicResolution: true, viewportProvisioning: true, virtualMonitorControl: true,
  resizeProtocol: 'epoch-v1', initialViewportProtocol: 'epoch-v1', streamProfileProtocol: 'console-profile-v1', streamProfileViewportProtocol: 'epoch-v1',
  monitors: [{ id: '0', width: 1920, height: 1080, primary: false, modes: [{ width: 1920, height: 1080, refreshRate: 60 }] }] };
const marked = { ...atRest, startPrimaryProtocol: 'start-primary-v1' };

async function fixture(t) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const hash = await hashPassword(password);
  const admin = store.addUser({ name: 'admin', passwordHash: hash, role: 'admin' });
  store.addUser({ name: 'alice', passwordHash: hash, role: 'user' });
  const vm = { vmid: 910, node: 'pve', name: 'VM 910', status: 'running' };
  store.syncProxmox([vm], 'fixture');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve-fixture.example', tokenId: 'fixture@pve!broker', secret: 'only-fixture-secret' }), 'fixture');
  const proxmox = { machineIdentity: async () => ({ machineUuid: vmUuid }), inventory: async () => [vm] };
  // The physical PC: its Host's request, approved by its shown code (server/src/local-pc.mjs).
  const requests = new HostRequests({ store, vault, proxmox });
  const input = { requestKey: newToken(), hostname: 'Physical PC', machineUuid: pcUuid, version: '1.1.22' };
  const created = requests.create(input, 'fixture', true);
  const pcId = (await requests.approveLocalPc(created.requestId, { name: 'Fixture PC', acknowledgeCode: true, acknowledgeUnlinked: true }, () => admin)).desktopId;
  const pcToken = requests.status({ requestId: created.requestId, requestKey: input.requestKey }, 'fixture', true).credentials.hostToken;
  store.replaceVmAssignments('alice', ['pve-910', pcId], admin);
  const server = await buildServer({ store, vault, proxmox, publicOrigin: origin, relayOptions: { authorizationMs: 20, heartbeatMs: 10000 }, logger: { error: value => assert.fail(value) } });
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
  const adminSession = { cookie: adminLogin.headers.get('set-cookie').split(';')[0], csrf: (await adminLogin.json()).csrfToken };
  const alice = (await call('/auth/login', { method: 'POST', data: { username: 'alice', password } })).body.sessionToken;
  const wsUrl = path => base.replace('http:', 'ws:') + path;
  async function channel(token, capabilities) {
    const ws = await socket(wsUrl('/api/v1/hosts/channel'), 'Host ' + token);
    ws.json({ type: 'host.capabilities', capabilities });
    assert.equal((await ws.take(value => value.type === 'host.capabilities.ack')).accepted, true);
    return ws;
  }
  const pcHost = capabilities => channel(pcToken, capabilities);
  async function vmHost(capabilities) {
    const code = (await call('/admin/desktops/pve-910/enrollment', { ...adminSession, method: 'POST', data: {} })).body.code;
    const enrolled = await call('/hosts/enroll', { method: 'POST', data: { code, hostname: 'Fixture VM', machineUuid: vmUuid, version: '1.1.22' } });
    assert.equal(enrolled.status, 201);
    return channel(enrolled.body.hostToken, capabilities);
  }
  // Connect, attach the client, and take the session.start the Host was sent.
  async function start(hostSocket, desktopId, extra = {}) {
    const response = await call('/desktops/' + desktopId + '/connect', { method: 'POST', token: alice, data: { mode: 'stream', ...extra } });
    assert.equal(response.status, 200, JSON.stringify(response.body));
    const controller = await socket(wsUrl('/api/v1/sessions/' + response.body.sessionId), 'Bearer ' + alice);
    const command = await hostSocket.take(value => value.type === 'session.start');
    return { response, controller, command };
  }
  const desktop = async id => (await call('/desktops', { token: alice })).body.desktops.find(item => item.id === id);
  return { pcId, pcHost, vmHost, start, desktop, call, alice };
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

async function connect(f, desktopId) {
  return f.call('/desktops/' + desktopId + '/connect', { method: 'POST', token: f.alice, data: request });
}

test('A physical PC with "0" not primary at rest: refused without the marker, admitted with it', async t => {
  assert.equal(startPrimaryProtocol, 'start-primary-v1');
  const f = await fixture(t);
  const host = await f.pcHost(atRest);
  const refused = await connect(f, f.pcId);
  assert.equal(refused.status, 409); assert.equal(refused.body.error?.code ?? refused.body.code ?? refused.body.error, 'initial_viewport_unavailable');
  // The same Host reports again, now with the marker.
  host.json({ type: 'host.capabilities', capabilities: marked });
  assert.equal((await host.take(value => value.type === 'host.capabilities.ack')).accepted, true);
  const listed = await f.desktop(f.pcId);
  assert.equal(listed.host.connected, true);
  assert.equal(Object.hasOwn(listed.host.capabilities, 'startPrimaryProtocol'), false, 'clients never see it');
  const admitted = await connect(f, f.pcId);
  assert.equal(admitted.status, 200, JSON.stringify(admitted.body));
  assert.deepEqual(admitted.body.initialViewport && { monitor: admitted.body.initialViewport.monitor, width: admitted.body.initialViewport.width, height: admitted.body.initialViewport.height }, { monitor: '0', width: 2560, height: 1440 });
  assert.equal(Object.hasOwn(admitted.body, 'startPrimaryProtocol'), false);
  host.close();
});

test('Another marker value is not the marker', async t => {
  const f = await fixture(t);
  const host = await f.pcHost({ ...atRest, startPrimaryProtocol: 'start-primary-v2' });
  const refused = await connect(f, f.pcId);
  assert.equal(refused.status, 409);
  host.close();
});
