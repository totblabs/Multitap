// The fake peers of usb-latency.test.mjs, each role in its own worker thread, so the broker's event loop
// does only the broker's work (in a deployment the Host and the client are other machines).
//  - video: enrolls the Host, starts the stream session and, when told, sends H.264-sized media frames at
//    60 fps through the Host channel to the controller socket (every 120th frame four times the size), whole
//    or, like the real Host's WinHTTP sender (websocket_send.hpp), in 8 KiB fragments.
//  - usb: opens the session's usb-v1 pair, attaches a report-protocol HID mouse and, when told, times one
//    interrupt read (Host submit -> client) and its 7-byte report (client return -> Host), one per millisecond.
import { parentPort, workerData } from 'node:worker_threads';
import { setImmediate as yieldLoop } from 'node:timers/promises';
import { WebSocket } from 'ws';

const { role, base } = workerData;
const now = () => Number(process.hrtime.bigint()) / 1000;   // microseconds, one clock for every thread

function open(path, authorization) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(base.replace('http:', 'ws:') + path, { headers: { Authorization: authorization }, perMessageDeflate: false });
    const queue = [], waiters = [];
    ws.on('message', (data, binary) => {
      if (ws.onBinary && binary) { ws.onBinary(data); return; }
      const value = binary ? data : JSON.parse(data.toString('utf8'));
      const index = waiters.findIndex(waiter => waiter.predicate(value));
      if (index >= 0) waiters.splice(index, 1)[0].resolve(value); else queue.push(value);
    });
    ws.take = (predicate = () => true) => {
      const index = queue.findIndex(predicate);
      if (index >= 0) return Promise.resolve(queue.splice(index, 1)[0]);
      return new Promise(resolve => waiters.push({ predicate, resolve }));
    };
    ws.json = value => ws.send(JSON.stringify(value));
    ws.once('error', reject);
    ws.once('open', () => resolve(ws));
  });
}
async function post(path, data, token) {
  const response = await fetch(base + '/api/v1' + path, { method: 'POST', headers: { 'Content-Type': 'application/json', ...(token ? { Authorization: 'Bearer ' + token } : {}) }, body: JSON.stringify(data) });
  if (!response.ok) throw new Error(`${path}: ${response.status}`);
  return response.json();
}
function frame({ type, seq, length = 7, payload = Buffer.alloc(0) }) {
  const header = Buffer.alloc(24);
  header[0] = type; header[1] = 1; header[2] = 0x81; header[3] = 3;
  header.writeUInt32LE(seq, 4); header.writeUInt32LE(length, 8);
  return Buffer.concat([header, payload]);
}
function stats(values) {
  const sorted = [...values].sort((a, b) => a - b), at = q => Math.round(sorted[Math.min(sorted.length - 1, Math.round(q * (sorted.length - 1)))]);
  return { samples: sorted.length, p50Us: at(0.5), p95Us: at(0.95), p99Us: at(0.99), maxUs: at(1) };
}

if (role === 'video') {
  const { code, machineUuid, alice, capabilities, streamInfo } = workerData;
  const { hostToken } = await post('/hosts/enroll', { code, hostname: 'Fixture VM', machineUuid, version: '0.2.72' });
  const host = await open('/api/v1/hosts/channel', 'Host ' + hostToken);
  host.json({ type: 'host.capabilities', capabilities });
  await host.take(value => value.type === 'host.capabilities.ack');
  const { sessionId } = await post('/desktops/pve-183/connect', { mode: 'stream', usbProtocol: 'usb-v1' }, alice);
  const controller = await open('/api/v1/sessions/' + sessionId, 'Bearer ' + alice);
  await host.take(value => value.type === 'session.start');
  host.json(streamInfo);
  await controller.take(value => value.type === 'stream.info');
  let received = 0, timer = null, run = null;
  controller.onBinary = data => { received += data.length; };
  parentPort.on('message', command => {
    if (command.mbps > 0) {
      const bytes = Math.round(command.mbps * 1e6 / 8 / 60), start = performance.now();
      run = { start, sent: 0, received };
      let k = 0;
      const tick = () => {
        for (; performance.now() - start >= k * 1000 / 60; ++k) {
          const key = k % 120 === 0, media = Buffer.alloc(key ? 4 * bytes : bytes);
          media[0] = 1; media[1] = key ? 1 : 0;   // video, key frame flag, epoch 0
          const piece = command.fragment || media.length;
          for (let at = 0; at < media.length; at += piece) host.send(media.subarray(at, at + piece), { binary: true, fin: at + piece >= media.length });
          run.sent += media.length;
        }
        timer = setTimeout(tick, 1);
      };
      tick();
    } else if (command.mbps === 0) {
      clearTimeout(timer);
      const seconds = run ? (performance.now() - run.start) / 1000 : 0;
      parentPort.postMessage(run ? { sentMbps: Math.round(run.sent * 8 / seconds / 1e6), deliveredMbps: Math.round((received - run.received) * 8 / seconds / 1e6) } : {});
      run = null;
    } else if (command.end) { host.close(); controller.close(); parentPort.close(); }
  });
  parentPort.postMessage({ sessionId, hostToken });
}

if (role === 'usb') {
  const { sessionId, hostToken, alice, mouse } = workerData;
  const host = await open(`/api/v1/hosts/sessions/${sessionId}/usb`, 'Host ' + hostToken);
  const client = await open(`/api/v1/sessions/${sessionId}/usb`, 'Bearer ' + alice);
  await client.take(value => value.type === 'usb.ready'); await host.take(value => value.type === 'usb.ready');
  client.json({ type: 'usb.attach', device: 1, descriptors: mouse, speed: 'full' });
  await host.take(value => value.type === 'usb.attach');
  host.json({ type: 'usb.status', state: 'ready' }); host.json({ type: 'usb.attached', device: 1 });
  await client.take(value => value.type === 'usb.attached');
  let seq = 0;
  parentPort.on('message', async command => {
    if (command.end) { host.close(); client.close(); parentPort.close(); return; }
    const toClient = [], toHost = [];
    for (let i = 0; i < command.rounds; ++i) {
      const due = now() + 1000, number = ++seq;
      const sent = now();
      host.send(frame({ type: 1, seq: number }));
      await client.take(Buffer.isBuffer);
      const read = now();
      client.send(frame({ type: 2, seq: number, payload: Buffer.from([0, 1, 0, 1, 0, 0, 0]) }));
      await host.take(Buffer.isBuffer);
      toClient.push(read - sent); toHost.push(now() - read);
      while (now() < due) await yieldLoop();
    }
    parentPort.postMessage({ submitToClient: stats(toClient), reportToHost: stats(toHost) });
  });
  parentPort.postMessage({ ready: true });
}
