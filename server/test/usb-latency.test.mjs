import { test } from 'node:test';
import assert from 'node:assert/strict';
import { once } from 'node:events';
import { randomBytes } from 'node:crypto';
import { Socket } from 'node:net';
import { Worker } from 'node:worker_threads';
import { setTimeout as delay } from 'node:timers/promises';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { descriptors } from './usb-descriptors.mjs';

// 게임 마우스 over usb-v1, measured through the real broker with its production timers: how long the relay
// takes to pass one interrupt read (Host -> client) and its 7-byte report (client -> Host), alone and while
// the same session streams 30 or 100 Mbps of video. The fake Host and client run in worker threads
// (usb-latency-peer.mjs), so this event loop does only the broker's work; loopback, no TLS. A measurement:
// it asserts only that every report crossed, that the video really flowed and that no relay socket waits for
// Nagle (the usb pair included: a 31-byte read must never wait for an earlier one's acknowledgement).
const origin = 'https://usb-latency-fixture.example';
const password = 'Usb-latency-fixture-password!';
const machineUuid = '12345678-1234-1234-1234-123456789ad4';
const usbHost = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true, usbProtocol: 'usb-v1' };
const streamInfo = { type: 'stream.info', codec: 'h264', width: 1920, height: 1080, sourceWidth: 1920, sourceHeight: 1080, fpsTarget: 60, audio: { sampleRate: 48000, channels: 2, format: 's16le', available: false } };
// The virtual mouse's shape for the relay: report-protocol HID, interrupt IN 0x81, 7-byte reports.
const mouse = descriptors({ vendor: 0x1209, product: 0x0001, configs: [{ interfaces: [{ number: 0, class: 3, endpoints: [[0x81, 3, 7]] }] }] });
const policy = { drives: false, printers: false, usbDevices: true, smartCards: false, serialPorts: false };

test('usb-v1 mouse reads and reports cross the relay at once, also beside 30 and 100 Mbps of video (measurement)', { timeout: 180000 }, async t => {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const hash = await hashPassword(password);
  store.addUser({ name: 'admin', passwordHash: hash, role: 'admin' });
  store.addUser({ name: 'alice', passwordHash: hash, role: 'user' });
  store.syncProxmox([{ vmid: 183, node: 'pve', name: 'VM 183', status: 'running' }], 'fixture');
  store.assign('alice', 'pve-183');
  store.updateAssignmentPolicy('alice', 'pve-183', { devicePolicy: policy }, 'fixture');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve-fixture.example', tokenId: 'fixture@pve!broker', secret: 'only-fixture-secret' }), 'fixture');
  const server = await buildServer({ store, vault, proxmox: { machineIdentity: async () => ({ machineUuid }) }, publicOrigin: origin, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const port = server.address().port, base = `http://127.0.0.1:${port}`;
  const workers = [], noDelay = [], setNoDelay = Socket.prototype.setNoDelay;
  Socket.prototype.setNoDelay = function (...args) { if (this.localPort === port) noDelay.push(args.length === 0 || args[0] !== false); return setNoDelay.apply(this, args); };
  t.after(async () => { Socket.prototype.setNoDelay = setNoDelay; for (const worker of workers) await worker.terminate(); await new Promise(resolve => server.close(resolve)); store.close(); });
  const json = async (path, body, headers = {}) => (await fetch(base + '/api/v1' + path, { method: 'POST', headers: { 'Content-Type': 'application/json', Origin: origin, ...headers }, body: JSON.stringify(body) }));
  const adminLogin = await json('/admin/login', { username: 'admin', password });
  const admin = { Cookie: adminLogin.headers.get('set-cookie').split(';')[0], 'X-CSRF-Token': (await adminLogin.json()).csrfToken };
  const alice = (await (await json('/auth/login', { username: 'alice', password })).json()).sessionToken;
  const code = (await (await json('/admin/desktops/pve-183/enrollment', {}, admin)).json()).code;
  const peer = data => { const worker = new Worker(new URL('./usb-latency-peer.mjs', import.meta.url), { workerData: { base, ...data } }); workers.push(worker); return worker; };

  const video = peer({ role: 'video', code, machineUuid, alice, capabilities: usbHost, streamInfo });
  const [{ sessionId, hostToken }] = await once(video, 'message');
  const usb = peer({ role: 'usb', sessionId, hostToken, alice, mouse: mouse.toString('base64') });
  await once(usb, 'message');
  // Host channel, controller and both usb sockets, all without Nagle.
  assert.ok(noDelay.length >= 4 && noDelay.every(Boolean), `a relay socket kept Nagle: ${JSON.stringify(noDelay)}`);
  // Video as the real Host sends it (8 KiB fragments: many small pieces of work on this loop) and whole (one
  // unmask per message; ws without bufferutil takes about 0.8 ms for a 100 Mbps key frame here).
  const results = {};
  for (const [name, mbps, fragment] of [['idle', 0], ['video30Mbps', 30, 8192], ['video100Mbps', 100, 8192], ['video100MbpsWhole', 100, 0], ['idleAgain', 0]]) {
    if (mbps) { video.postMessage({ mbps, fragment }); await delay(300); }
    usb.postMessage({ rounds: 2000 });
    const [timed] = await once(usb, 'message');
    let flowed = {};
    if (mbps) { video.postMessage({ mbps: 0 }); [flowed] = await once(video, 'message'); }
    assert.equal(timed.submitToClient.samples, 2000); assert.equal(timed.reportToHost.samples, 2000);
    if (mbps) assert.ok(flowed.deliveredMbps >= mbps * 0.8, `the video did not flow: ${JSON.stringify(flowed)}`);
    results[name] = { ...timed, ...flowed };
  }
  t.diagnostic(JSON.stringify(results));
});
