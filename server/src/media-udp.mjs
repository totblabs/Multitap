import { createHmac } from 'node:crypto';

// Relay framing of UDP media. Custom and crypto-adjacent: it needs independent
// review before any Host or client depends on it. Known limits to review:
// - The proof cannot cover the source address (NAT rewrites it), so whoever
//   sees a first datagram in flight can race it and take that leg's binding.
// - There is no return-routability check: a ticket holder that spoofs its
//   source aims the other leg's traffic at that address, up to the buckets.
// - DATA is not authenticated here. Only the pinned source guards injection;
//   the end-to-end protection planned for media is what must reject forgeries.
//
//   [0]     type
//   [1..8]  connection ID of the sending leg: random, not secret
//   [9..]   BIND, ACK: HMAC-SHA256(ticket, label || id). DATA: opaque, never read
export const MAX_DATAGRAM = 1200, ID_BYTES = 8, TICKET_BYTES = 32, HEADER_BYTES = 1 + ID_BYTES, BIND_BYTES = HEADER_BYTES + 32;
export const BIND = 1, ACK = 2, DATA = 3;

// Slots of the shared counter array. A fixed list stays bounded under any traffic.
export const dropReasons = ['oversize', 'malformed', 'unknown_id', 'bad_proof', 'ticket_expired', 'ticket_reused', 'wrong_source', 'unbound', 'rate_session', 'rate_global', 'send_queue', 'send_failed'];
export const counterNames = ['bindings', 'forwarded', 'forwardedBytes', ...dropReasons];

const proof = (ticket, label, id) => createHmac('sha256', ticket).update(label).update(id).digest();
const framed = (type, id, body) => Buffer.concat([Buffer.of(type), id, body]);
export const bindDatagram = (ticket, id) => framed(BIND, id, proof(ticket, 'multitap-udp-bind-v1', id));
export const ackDatagram = (ticket, id) => framed(ACK, id, proof(ticket, 'multitap-udp-ack-v1', id));
export const dataDatagram = (id, payload) => framed(DATA, id, payload);
