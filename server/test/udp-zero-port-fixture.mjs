// Run by udp-relay.test.mjs with this file as a preload too: every source port the
// relay worker sees reads as 0, which a real Windows socket cannot send.
import dgram from 'node:dgram';
import { isMainThread } from 'node:worker_threads';
import { setTimeout as sleep } from 'node:timers/promises';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';

if (!isMainThread) {
  const emit = dgram.Socket.prototype.emit;
  dgram.Socket.prototype.emit = function (name, message, from) {
    return emit.call(this, name, message, name === 'message' ? { ...from, port: 0 } : from);
  };
} else if (resolve(process.argv[1] ?? '') === fileURLToPath(import.meta.url)) {
  const { UdpRelay } = await import('../src/udp-relay.mjs');
  const { bindDatagram } = await import('../src/media-udp.mjs');
  const relay = new UdpRelay({ port: 0, host: '127.0.0.1', relays: { host: [{ host: '127.0.0.1', port: 1 }], client: [{ host: '127.0.0.1', port: 1 }] }, live: () => true, restartMs: 100, logger: { warn() {} } });
  while (!relay.running) await sleep(10);
  const other = relay.issue('other-session'), attacker = relay.issue('attacker-session');
  const socket = dgram.createSocket('udp4');
  socket.send(bindDatagram(attacker.client.ticket, attacker.client.id), relay.port, '127.0.0.1');
  await sleep(400);
  const { bindings, dropped } = relay.stats();
  console.log(JSON.stringify({ running: relay.running, restarts: relay.restarts, other: relay.grants.has('other-session'), issued: !!other, bindings, malformed: dropped.malformed }));
  socket.close();
  await relay.close();
  process.exit(0);
}
