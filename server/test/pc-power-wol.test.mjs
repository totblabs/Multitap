import { test } from 'node:test';
import assert from 'node:assert/strict';
import dgram from 'node:dgram';
import { once } from 'node:events';
import { magicPacket, broadcastTargets, sendWakePackets, wakeAdapters } from '../src/pc-power.mjs';

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
