import { test } from 'node:test';
import assert from 'node:assert/strict';
import dgram from 'node:dgram';
import { once } from 'node:events';
import { readFile } from 'node:fs/promises';
import { magicPacket, broadcastTargets, routeBroadcasts, sendWakePackets, wakeAdapters, wakeFailureReason } from '../src/pc-power.mjs';
import { localAddresses } from '../update/server-access-deployment.mjs';

test('The magic packet is six 0xFF bytes and the MAC sixteen times', () => {
  const packet = magicPacket('D8:BB:C1:12:34:56');
  assert.equal(packet.length, 102);
  assert.deepEqual([...packet.subarray(0, 6)], [255, 255, 255, 255, 255, 255]);
  for (let i = 0; i < 16; i++) assert.equal(packet.subarray(6 + i * 6, 12 + i * 6).toString('hex'), 'd8bbc1123456');
});

test('Wake targets are the limited broadcast and the broker\'s own IPv4 networks only', () => {
  const targets = broadcastTargets({
    lo: [{ family: 'IPv4', address: '127.0.0.1', netmask: '255.0.0.0', internal: true }],
    eth0: [{ family: 'IPv4', address: '192.0.2.121', netmask: '255.255.255.0', internal: false }, { family: 'IPv6', address: 'fe80::1', netmask: 'ffff:ffff:ffff:ffff::', internal: false }],
    wg0: [{ family: 'IPv4', address: '198.51.100.5', netmask: '255.255.255.255', internal: false }],
    br: [{ family: 4, address: '198.18.0.1', netmask: '255.255.0.0', internal: false }],
  });
  assert.deepEqual(targets, ['255.255.255.255', '192.0.2.255', '198.18.255.255']);
});

// The managed systemd server's routing table (eth0 192.0.2.121/24, a default route, a host route, a down route).
const routeTable = [
  'Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT',
  'eth0\t00000000\t010200C0\t0003\t0\t0\t0\t00000000\t0\t0\t0',
  'eth0\t000200C0\t00000000\t0001\t0\t0\t0\t00FFFFFF\t0\t0\t0',
  'wg0\t056433C6\t00000000\t0005\t0\t0\t0\tFFFFFFFF\t0\t0\t0',
  'br0\t000012C6\t00000000\t0000\t0\t0\t0\t0000FFFF\t0\t0\t0',
  'br1\t000012C6\t00000000\t0001\t0\t0\t0\t0000FFFF\t0\t0\t0',
  'bad\tzz\t00000000\t0001\t0\t0\t0\t00FFFFFF\t0\t0\t0',
].join('\n');

test('A broker that cannot read its networks reads them from the routing table, and failing that sends the limited broadcast', () => {
  // The broker's systemd sandbox has no netlink sockets, so os.networkInterfaces() throws EAFNOSUPPORT there
  // (seen on the managed systemd server: every 켜기 failed before a packet left).
  const sandboxed = () => { throw Object.assign(new Error('uv_interface_addresses returned Unknown system error 97'), { code: 'ERR_SYSTEM_ERROR' }); };
  assert.deepEqual(routeBroadcasts(routeTable), ['192.0.2.255', '198.18.255.255'], 'connected and up only: no default, host or down route');
  assert.deepEqual(broadcastTargets(undefined, sandboxed, () => routeTable), ['255.255.255.255', '192.0.2.255', '198.18.255.255']);
  assert.deepEqual(broadcastTargets(undefined, sandboxed, () => { throw new Error('ENOENT'); }), ['255.255.255.255']);
  assert.deepEqual(broadcastTargets(undefined, () => ({ eth0: [{ family: 'IPv4', address: '192.0.2.121', netmask: '255.255.255.0', internal: false }] })), ['255.255.255.255', '192.0.2.255']);
  assert.equal(localAddresses(sandboxed).size, 0, 'the updater treats its own addresses as unknown instead of failing');
  assert.ok(localAddresses(() => ({ eth0: [{ address: '192.0.2.121' }] })).has('192.0.2.121'));
});

test('The managed broker service keeps its sandbox: no netlink sockets for wake broadcasts', async () => {
  for (const path of ['../update/provision-broker.sh', '../../deploy/linux/provision-broker.sh']) {
    const unit = (await readFile(new URL(path, import.meta.url), 'utf8')).match(/^RestrictAddressFamilies=(.*)$/m);
    assert.ok(unit, path);
    assert.deepEqual(unit[1].split(' ').sort(), ['AF_INET', 'AF_INET6', 'AF_UNIX'], path);
  }
});

test('A wake that sends nothing fails with the first system error as a fixed token', async () => {
  const fake = ({ bindError = null, sendError = null } = {}) => () => {
    const handlers = new Map(), socket = {
      sent: 0, closed: false,
      once: (event, handler) => handlers.set(event, handler), off: event => handlers.delete(event),
      bind: (_port, done) => bindError ? handlers.get('error')(bindError) : done(),
      setBroadcast() {}, close() { socket.closed = true; },
      send: (_data, _port, _target, done) => sendError ? done(sendError) : (socket.sent++, done()),
    };
    return socket;
  };
  await assert.rejects(sendWakePackets(['D8:BB:C1:12:34:56'], { targets: ['255.255.255.255', '192.0.2.255'], createSocket: fake({ sendError: Object.assign(new Error('x'), { code: 'EPERM' }) }) }),
    error => error.message === 'wake_send_failed' && error.reason === 'EPERM');
  await assert.rejects(sendWakePackets(['D8:BB:C1:12:34:56'], { targets: ['255.255.255.255'], createSocket: fake({ bindError: Object.assign(new Error('x'), { code: 'EACCES' }) }) }),
    error => error.message === 'wake_send_failed' && error.reason === 'EACCES');
  assert.equal(await sendWakePackets(['D8:BB:C1:12:34:56'], { targets: ['255.255.255.255', '192.0.2.255'], rounds: 3, createSocket: fake() }), 6);
  assert.equal(wakeFailureReason({ reason: 'ENETUNREACH' }), 'ENETUNREACH');
  for (const bad of [null, {}, { code: 'uv error 97' }, { reason: 'a'.repeat(40) }, { code: 'eperm' }]) assert.equal(wakeFailureReason(bad), 'unknown');
});

test('Wake reports normalise MACs and keep only Windows\' read-only flags', () => {
  assert.deepEqual(wakeAdapters([{ mac: 'd8-bb-c1-12-34-56', wakeEnabled: false, magicPacket: true }, { mac: 'D8:BB:C1:12:34:56' }]), [{ mac: 'D8:BB:C1:12:34:56', wakeEnabled: false, magicPacket: true }]);
  assert.deepEqual(wakeAdapters([]), []);
  for (const bad of [null, {}, [{ mac: 'D8BBC1123456' }], [{ mac: '03:00:00:00:00:01' }], [{ mac: 'D8:BB:C1:12:34:56', wakeEnabled: 1 }]]) assert.equal(wakeAdapters(bad), null);
});

test('The real sender puts magic packets on the wire (loopback listener)', async t => {
  const listener = dgram.createSocket('udp4');
  t.after(() => listener.close());
  listener.bind(0, '127.0.0.1'); await once(listener, 'listening');
  const received = [];
  listener.on('message', data => received.push(data));
  const sent = await sendWakePackets(['D8:BB:C1:12:34:56'], { targets: ['127.0.0.1'], port: listener.address().port, rounds: 2 });
  assert.equal(sent, 2);
  for (let i = 0; i < 100 && received.length < 2; i++) await new Promise(resolve => setTimeout(resolve, 10));
  assert.equal(received.length, 2);
  assert.deepEqual(received[0], magicPacket('D8:BB:C1:12:34:56'));
});
