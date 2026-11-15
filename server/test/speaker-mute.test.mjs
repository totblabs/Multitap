import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { HostRequests } from '../src/host-requests.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword, newToken } from '../src/security.mjs';
import { Relay } from '../src/relay.mjs';
import { speakerMuteProtocol, speakerMuteCapability, speakerMuteStartFields } from '../src/speaker-mute.mjs';
import { requests as trackRequests, options as trackOptions } from './track-fixture.mjs';

// speaker-mute-v1: the broker tells a Host that advertises it which stream is
// a physical PC's, by a marker in that session.start. Real broker and
// WebSockets, fake Hosts and client: one approved physical PC and one VM.
const origin = 'https://speaker-mute-fixture.example';
const password = 'Speaker-fixture-password-only!';
const vmUuid = '12345678-1234-1234-1234-123456789af1';
const pcUuid = 'a1234567-89ab-cdef-8123-0123456789af';
const releasedHost = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true };
const speakerHost = { ...releasedHost, speakerMuteProtocol: 'speaker-mute-v1' };
const trackHost = { ...speakerHost, videoTrackProtocol: 'tracks-v1', trackViewportProvisioning: true, monitors: trackRequests.map(t => ({ id: t.id, width: t.width, height: t.height, primary: t.id === '0' })) };

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
  return { pcId, pcHost, vmHost, start, desktop };
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

test('Capability and the session.start field are exact: a physical PC whose Host advertises it, a stream, nothing else', () => {
  assert.equal(speakerMuteProtocol, 'speaker-mute-v1');
  assert.equal(speakerMuteCapability(speakerHost), true);
  for (const value of [releasedHost, { ...releasedHost, speakerMuteProtocol: 'speaker-mute-v2' }, { ...releasedHost, speakerMuteProtocol: true }, { ...releasedHost, speakerMute: 'speaker-mute-v1' }, undefined, null])
    assert.equal(speakerMuteCapability(value), false);
  const session = { localPc: true, mode: 'stream', host: { capabilities: speakerHost } };
  assert.deepEqual(speakerMuteStartFields(session), { speakerMuteProtocol: 'speaker-mute-v1' });
  assert.deepEqual(speakerMuteStartFields({ ...session, trackState: {} }), { speakerMuteProtocol: 'speaker-mute-v1' }, 'independent outputs too');
  assert.deepEqual(speakerMuteStartFields({ ...session, localPc: false }), {}, 'a VM never');
  assert.deepEqual(speakerMuteStartFields({ ...session, localPc: undefined }), {}, 'only a session known to be a physical PC');
  assert.deepEqual(speakerMuteStartFields({ ...session, mode: 'rdp' }), {});
  assert.deepEqual(speakerMuteStartFields({ ...session, host: { capabilities: releasedHost } }), {}, 'a Host that does not advertise it never sees the field');
  assert.deepEqual(speakerMuteStartFields({ ...session, host: {} }), {});
  assert.deepEqual(speakerMuteStartFields(undefined), {});
  // The capability is between the broker and the Host: a client's view of the Host never shows it.
  const relay = { store: { devicePolicy: () => ({}) } };
  assert.deepEqual(Relay.prototype.visibleCapabilities.call(relay, { ...speakerHost }, undefined, 'fixture', true), releasedHost);
  assert.deepEqual(Relay.prototype.visibleCapabilities.call(relay, { ...speakerHost }, undefined, 'fixture', false), releasedHost);
});

test('A physical PC whose Host advertises it gets the marker in session.start; the client neither asks for it nor sees it', async t => {
  const f = await fixture(t), host = await f.pcHost(speakerHost);
  const listed = await f.desktop(f.pcId);
  assert.equal(listed.kind, 'pc'); assert.equal(listed.host.connected, true);
  assert.equal(Object.hasOwn(listed.host.capabilities, 'speakerMuteProtocol'), false);
  const { response, command } = await f.start(host, f.pcId);
  assert.equal(command.speakerMuteProtocol, 'speaker-mute-v1');
  assert.equal(command.mode, 'stream');
  assert.equal(Object.hasOwn(response.body, 'speakerMuteProtocol'), false);
});

test('Independent outputs of a physical PC carry the marker too', async t => {
  const f = await fixture(t), host = await f.pcHost(trackHost);
  const { command } = await f.start(host, f.pcId, { options: trackOptions, mediaProtocol: 'tracks-v1', tracks: trackRequests });
  assert.equal(command.mediaProtocol, 'tracks-v1');
  assert.equal(command.speakerMuteProtocol, 'speaker-mute-v1');
});

test('A physical PC whose Host does not advertise it is sent session.start exactly as before', async t => {
  const f = await fixture(t), host = await f.pcHost(releasedHost);
  const { command } = await f.start(host, f.pcId);
  assert.equal(Object.hasOwn(command, 'speakerMuteProtocol'), false);
});

test('A VM never gets the marker, whatever its Host advertises', async t => {
  const f = await fixture(t), host = await f.vmHost(speakerHost);
  const listed = await f.desktop('pve-910');
  assert.equal(listed.kind, 'vm');
  assert.equal(Object.hasOwn(listed.host.capabilities, 'speakerMuteProtocol'), false);
  const { command } = await f.start(host, 'pve-910');
  assert.equal(Object.hasOwn(command, 'speakerMuteProtocol'), false);
  const tracks = await fixture(t), trackVm = await tracks.vmHost(trackHost);
  const outputs = await tracks.start(trackVm, 'pve-910', { options: trackOptions, mediaProtocol: 'tracks-v1', tracks: trackRequests });
  assert.equal(outputs.command.mediaProtocol, 'tracks-v1');
  assert.equal(Object.hasOwn(outputs.command, 'speakerMuteProtocol'), false);
});

test('A capability with another value is dropped from the Host report, not a reason to refuse it', async t => {
  const f = await fixture(t), host = await f.pcHost({ ...releasedHost, speakerMuteProtocol: 'speaker-mute-v9' });
  const { command } = await f.start(host, f.pcId);
  assert.equal(Object.hasOwn(command, 'speakerMuteProtocol'), false);
});
