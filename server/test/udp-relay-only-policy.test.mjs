// Synthetic local sockets only: relay reachability is not a field path test.
import test from 'node:test';
import assert from 'node:assert/strict';
import { createSocket } from 'node:dgram';
import { setTimeout as delay } from 'node:timers/promises';
import { configuration } from '../src/config.mjs';
import { UdpRelay } from '../src/udp-relay.mjs';
import { udpGrantMessage } from '../src/udp-media.mjs';
import { ackDatagram, bindDatagram, dataDatagram } from '../src/media-udp.mjs';
import { udpPeerDirectAllowed } from '../src/udp-direct.mjs';

const until = async predicate => { const end = Date.now() + 4000; while (!predicate()) { assert.ok(Date.now() < end, 'fixture_timeout'); await delay(3); } };
test('relay-only policy: only a strict trusted boolean enables the dormant predicate', () => {
  for (const value of [undefined, null, false, 0, 1, 'true', 'false', {}, []]) assert.equal(udpPeerDirectAllowed({ allowPeerDirect: value }), false);
  assert.equal(udpPeerDirectAllowed(null), false); assert.equal(udpPeerDirectAllowed({ allowPeerDirect: true }), true);
});
test('relay-only policy: normal config never enables peers and has no default STUN', () => {
  for (const extra of [{}, { MULTITAP_UDP_DIRECT: 'on', MULTITAP_ALLOW_PEER_DIRECT: 'true' }, { MULTITAP_STUN_SERVERS: 'stun.example:3478' }]) {
    const udp = configuration({ MULTITAP_UDP_PORT: '18481', MULTITAP_UDP_MEDIA: 'on', ...extra }, { readAccessFile: () => null }).udp;
    assert.equal(udp.allowPeerDirect, false);
    if (!extra.MULTITAP_STUN_SERVERS) assert.deepEqual(udp.stun, []);
    assert.equal(udp.media, true);
  }
});
test('relay-only policy: grant formatter cannot invent peer permission from request flags', () => {
  const grant = { expiresAt: 123 }, leg = { id: Buffer.alloc(8), ticket: Buffer.alloc(32) }, relays = [{ host: 'relay.example', port: 18481 }];
  for (const allowed of [undefined, false, 0, 1, 'true', {}]) {
    const value = udpGrantMessage(grant, relays, leg, true, true, [{ host: 'stun.example', port: 3478 }], allowed);
    assert.equal(value.directProtocol, undefined); assert.equal(value.stun, undefined);
    assert.equal(value.keys, true); assert.deepEqual(value.relays, relays); assert.equal(value.expiresAt, 123);
  }
});
for (const tracks of [null, [{ track: '0', topologyEpoch: 7, epoch: 11, generation: 1 }, { track: '1', topologyEpoch: 7, epoch: 12, generation: 1 }]]) {
  test(`relay-only policy: ${tracks ? 'tracks' : 'composite'} real relay forwards while default peer offers stay off`, async t => {
    const relay = new UdpRelay({ port: 0, host: '127.0.0.1', live: id => id === 'fixture', logger: { warn() {} } }), offers = [];
    relay.onPeer = (...value) => offers.push(value); t.after(() => relay.close()); await until(() => relay.running);
    const grant = relay.issue('fixture', { direct: true, tracks }), rows = tracks ? grant.tracks : [grant];
    for (const row of rows) {
      const peers = [];
      for (const leg of [row.client, row.host]) {
        const socket = createSocket('udp4'), received = []; socket.on('message', bytes => received.push(bytes));
        await new Promise(done => socket.bind(0, '127.0.0.1', done)); t.after(() => new Promise(done => socket.close(done)));
        const send = bytes => new Promise((done, fail) => socket.send(bytes, relay.port, '127.0.0.1', error => error ? fail(error) : done()));
        const ack = ackDatagram(leg.ticket, leg.id);
        for (let n = 0; n < 100 && !received.some(b => b.equals(ack)); n++) { await send(bindDatagram(leg.ticket, leg.id)); await delay(5); }
        assert.ok(received.some(b => b.equals(ack)), 'relay BIND acknowledgement'); peers.push({ received, send });
      }
      const packet = dataDatagram(row.client.id, Buffer.from([0x91, tracks ? row.epoch : 1]));
      await peers[0].send(packet); await until(() => peers[1].received.some(b => b.equals(packet)));
    }
    assert.equal(relay.stats().bindings, rows.length * 2);
    assert.ok(relay.stats().forwarded > 0, 'positive relay data control');
    assert.equal(relay.direct.size, 0); assert.deepEqual(offers, []);
    // Even a stale/forged map cannot bypass the constructor policy.
    const row = { client: { id: 'client', observed: { address: '127.0.0.1', port: 1234 } }, host: { id: 'host' } };
    relay.direct.set('fixture', [row]);
    relay.bound({ session: 'fixture', id: 'host', address: '127.0.0.1', port: 2345 });
    relay.local('fixture', 'client', { protocol: 'udp-direct-v1', address: '192.168.1.2', port: 3456 });
    relay.offerPeer('fixture', 'client', row); assert.deepEqual(offers, []);
    relay.revoke('fixture'); await until(() => relay.stats().bindings === 0);
    assert.equal(relay.direct.size, 0); assert.equal(relay.stats().sessions, 0);
  });
}
