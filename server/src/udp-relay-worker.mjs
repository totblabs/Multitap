import { parentPort, workerData } from 'node:worker_threads';
import { createSocket } from 'node:dgram';
import { timingSafeEqual } from 'node:crypto';
import { BIND, BIND_BYTES, DATA, HEADER_BYTES, MAX_DATAGRAM, counterNames } from './media-udp.mjs';

const { host, port, counters, sessionBytesPerSecond, globalBytesPerSecond, burstMs } = workerData;
// Charged as wire bytes, so a flood of tiny datagrams cannot dodge a byte limit.
const WIRE_OVERHEAD = 64;
const MAX_SEND_QUEUE = 8 * 1024 * 1024, SOCKET_BUFFER = 4 * 1024 * 1024;
const counts = new Float64Array(counters), slot = Object.fromEntries(counterNames.map((name, index) => [name, index]));
const drop = reason => { counts[slot[reason]]++; };
const legs = new Map(), grants = new Map();

class TokenBucket {
  constructor(bytesPerSecond) { this.perMs = bytesPerSecond / 1000; this.capacity = this.tokens = this.perMs * burstMs; this.at = performance.now(); }
  take(cost, now) {
    this.tokens = Math.min(this.capacity, this.tokens + (now - this.at) * this.perMs); this.at = now;
    if (this.tokens < cost) return false;
    this.tokens -= cost; return true;
  }
}
const globalBucket = new TokenBucket(globalBytesPerSecond);
const socket = createSocket({ type: 'udp4', recvBufferSize: SOCKET_BUFFER, sendBufferSize: SOCKET_BUFFER });
const sent = error => { if (error) drop('send_failed'); };

function transmit(leg, datagram, target) {
  const cost = datagram.length + WIRE_OVERHEAD, now = performance.now();
  if (!leg.bucket.take(cost, now)) { drop('rate_session'); return false; }
  if (!globalBucket.take(cost, now)) { drop('rate_global'); return false; }
  if (socket.getSendQueueSize() > MAX_SEND_QUEUE) { drop('send_queue'); return false; }
  // A bad target throws instead of reaching the callback, and an exception here
  // would end the worker and with it every session's grant.
  try { socket.send(datagram, target.port, target.address, sent); }
  catch { drop('send_failed'); return false; }
  return true;
}

function bind(leg, datagram, from, pinned) {
  if (datagram.length !== BIND_BYTES || !timingSafeEqual(datagram, leg.bind)) return drop('bad_proof');
  if (leg.address && !pinned) return drop('ticket_reused');
  if (!leg.address) {
    if (Date.now() >= leg.expiresAt) return drop('ticket_expired');
    leg.address = from.address; leg.port = from.port;
    counts[slot.bindings]++;
    parentPort.postMessage({ type: 'bound', session: leg.session, id: leg.id, address: leg.address, port: leg.port });
  }
  // Repeated for a pinned peer that lost the ack. It binds nothing new, and the
  // ack is exactly the size of the proof it answers.
  transmit(leg, leg.ack, leg);
}

function receive(datagram, from) {
  // Port 0 can never be answered, so it must not be pinned.
  if (!from.port) return drop('malformed');
  if (datagram.length > MAX_DATAGRAM) return drop('oversize');
  if (datagram.length < HEADER_BYTES || (datagram[0] !== BIND && datagram[0] !== DATA)) return drop('malformed');
  const leg = legs.get(datagram.toString('latin1', 1, HEADER_BYTES));
  if (!leg) return drop('unknown_id');
  const pinned = leg.port === from.port && leg.address === from.address;
  if (datagram[0] === BIND) return bind(leg, datagram, from, pinned);
  if (!leg.address) return drop('unbound');
  if (!pinned) return drop('wrong_source');
  if (!leg.peer.address) return drop('unbound');
  if (transmit(leg, datagram, leg.peer)) { counts[slot.forwarded]++; counts[slot.forwardedBytes] += datagram.length; }
}

function grant({ session, legs: rows, expiresAt }) {
  if(!Array.isArray(rows)||![2,4].includes(rows.length))throw new TypeError('invalid_udp_grant');
  const bucket = new TokenBucket(sessionBytesPerSecond);
  const pair = rows.map(leg => ({ ...leg, session, bucket, expiresAt, address: null, port: 0, peer: null }));
  for(let n=0;n<pair.length;n+=2)[pair[n].peer,pair[n+1].peer]=[pair[n+1],pair[n]];
  for (const leg of pair) legs.set(leg.id, leg);
  grants.set(session, pair);
}

function revoke({ session }) {
  for (const leg of grants.get(session) ?? []) {
    legs.delete(leg.id);
    if (leg.address) counts[slot.bindings]--;
  }
  grants.delete(session);
}

parentPort.on('message', message => (message.type === 'grant' ? grant : revoke)(message));
socket.on('message', receive);
// No 'error' listener: a socket failure ends this worker and the controller,
// which owns recovery, starts a clean one without any spent ticket.
// The kernel caps a requested size (Linux: net.core.rmem_max/wmem_max, then doubles
// it for its own accounting), so the main thread reports what it actually got.
socket.bind(port, host, () => parentPort.postMessage({ port: socket.address().port, recvBuffer: socket.getRecvBufferSize(), sendBuffer: socket.getSendBufferSize() }));
