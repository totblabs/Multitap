import { Worker } from 'node:worker_threads';
import { randomBytes } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { ID_BYTES, TICKET_BYTES, ackDatagram, bindDatagram, counterNames, dropReasons } from './media-udp.mjs';
import { localCandidate, udpPeerDirectAllowed } from './udp-direct.mjs';
import { parseUdpRelays } from './udp-media.mjs';

const BINDINGS = counterNames.indexOf('bindings');
// /proc/net/udp row of the relay's port: rx_queue (bytes the kernel holds for it, in
// its own accounting) and drops (datagrams lost because that queue was full). Null
// where there is no such file (Windows, macOS) or no such row.
export function kernelSocket(port, read = process.platform === 'linux' ? () => readFileSync('/proc/net/udp', 'latin1') : null) {
  if (!read) return null;
  try {
    const local = ':' + port.toString(16).toUpperCase().padStart(4, '0');
    for (const line of read().split('\n').slice(1)) {
      const fields = line.trim().split(/\s+/);
      if (fields.length < 13 || !fields[1].endsWith(local) || fields[2] !== '00000000:0000') continue;
      const rxQueue = parseInt(fields[4].split(':')[1], 16), drops = Number(fields[12]);
      return Number.isSafeInteger(rxQueue) && Number.isSafeInteger(drops) ? { rxQueue, drops } : null;
    }
  } catch { }
  return null;
}
function newLeg() {
  const id = randomBytes(ID_BYTES), ticket = randomBytes(TICKET_BYTES);
  // Copied out of Buffer's shared pool, so the message carries only these bytes.
  return { id, ticket, wire: { id: id.toString('latin1'), bind: Uint8Array.from(bindDatagram(ticket, id)), ack: Uint8Array.from(ackDatagram(ticket, id)) } };
}

// Owns the worker from the broker's main thread. `live(sessionId)` answers
// whether a session may still hold a binding.
export class UdpRelay {
  // `derivedPort`: no relay list is configured and each leg is given the
  // address it already reaches the broker at, on this UDP port (config.mjs).
  constructor({ port, host = '0.0.0.0', media = false, allowPeerDirect = false, stun = [], relays = { host: [], client: [] }, derivedPort = null, hostRoute = null, live, logger = console, ticketMs = 30000, sweepMs = 100, restartMs = 5000, sessionBytesPerSecond = 32 * 1024 * 1024, globalBytesPerSecond = 128 * 1024 * 1024, burstMs = 100 }) {
    Object.assign(this, { media, relays, derivedPort, hostRoute, live, logger, ticketMs, restartMs });
    this.allowPeerDirect = allowPeerDirect === true;
    this.limits = { port, host, sessionBytesPerSecond, globalBytesPerSecond, burstMs };
    this.counters = new SharedArrayBuffer(counterNames.length * Float64Array.BYTES_PER_ELEMENT);
    this.counts = new Float64Array(this.counters);
    this.grants = new Set(); this.running = false; this.closed = false; this.restarts = 0;
    this.direct = new Map(); this.stun = stun;
    // Removing an assignment reaches the relay only through its 250 ms tick;
    // adding delivery to the worker to that would overrun the 250 ms limit.
    this.timer = setInterval(() => this.sweep(), sweepMs); this.timer.unref();
    this.start();
  }
  start() {
    let worker;
    try { worker = this.worker = new Worker(new URL('./udp-relay-worker.mjs', import.meta.url), { workerData: { ...this.limits, counters: this.counters } }); }
    catch (error) { this.failed(error); this.retry(); return; }
    worker.unref();
    worker.on('message', message => {
      if (message.type === 'bound') { this.bound(message); return; }
      const { port, recvBuffer, sendBuffer } = message;
      this.port = port; this.running = true; this.reported = false; this.buffers = { recvBuffer, sendBuffer };
      this.logger.info?.(JSON.stringify({ event: 'udp_relay_started', port, recvBuffer, sendBuffer }));
    });
    worker.on('error', error => this.failed(error));
    worker.on('exit', () => {
      // Tickets end with the worker: granting them again would revive spent ones.
      this.running = false; this.grants.clear(); this.direct.clear(); this.counts[BINDINGS] = 0;
      if (!this.closed) { this.restarts++; this.retry(); }
    });
  }
  retry() { this.restart = setTimeout(() => this.start(), this.restartMs); this.restart.unref(); }
  // Once per outage: a port that stays taken must not fill the log.
  failed(error) {
    if (this.reported) return;
    this.reported = true;
    this.logger.warn(JSON.stringify({ event: 'udp_relay_failed', code: /^[A-Z0-9_]{1,40}$/.test(error?.code ?? '') ? error.code : 'worker_error' }));
  }
  get available() { return this.running && (this.derivedPort !== null || (this.relays.host.length > 0 && this.relays.client.length > 0)); }
  // One leg's relay list. `origin` is the address that leg's own connection
  // to the broker was admitted on. A configured list is the same for everyone.
  // A derived one is that address's host on the published UDP port; a Host is
  // first given the address Hosts inside the network reach the broker at
  // (host-route-v1). Empty where no IPv4 endpoint follows from the address
  // (an IPv6 literal): that session stays on WebSocket.
  relaysFor(role, origin) {
    if (this.derivedPort === null) return this.relays[role];
    let name;
    try { name = new URL(origin).hostname.toLowerCase().replace(/\.$/, ''); } catch { return []; }
    const list = [];
    for (const host of new Set([...(role === 'host' && this.hostRoute ? [this.hostRoute] : []), name])) {
      try { list.push(...parseUdpRelays(host + ':' + this.derivedPort)); } catch { /* no endpoint for this address */ }
    }
    return list;
  }
  // For the administration console: what is configured, not what a session got.
  describe() {
    return { running: this.running, port: this.limits.port, derivedPort: this.derivedPort, media: this.media === true ? 'on' : this.media instanceof Set ? 'desktops' : 'off',
      relays: { host: this.relays.host.map(row => ({ ...row })), client: this.relays.client.map(row => ({ ...row })) }, hostRoute: this.derivedPort !== null ? this.hostRoute : null };
  }
  // A failed check must neither keep a binding nor throw into a timer.
  allowed(sessionId) { try { return !!this.live(sessionId); } catch { return false; } }
  // Null while the relay is down: the session simply stays on WebSocket.
  issue(sessionId, { direct = false, tracks = null } = {}) {
    if (!this.running || !this.allowed(sessionId)) return null;
    if(tracks!==null&&(!Array.isArray(tracks)||tracks.length<1||tracks.length>2||new Set(tracks.map(row=>row?.track)).size!==tracks.length||tracks.some(row=>!row||!['0','1'].includes(row.track)||!Number.isInteger(row.topologyEpoch)||row.topologyEpoch<1||row.topologyEpoch>0xffffffff||!Number.isInteger(row.epoch)||row.epoch<1||row.epoch>65535||!Number.isInteger(row.generation)||row.generation<1||row.generation>0xffffffff)||tracks.some(row=>row.topologyEpoch!==tracks[0].topologyEpoch||row.generation!==tracks[0].generation)))return null;
    this.revoke(sessionId);
    const pairs=(tracks??[null]).map(row=>({scope:row?{track:row.track,topologyEpoch:row.topologyEpoch,epoch:row.epoch,generation:row.generation}:null,client:newLeg(),host:newLeg()})),expiresAt=Date.now()+this.ticketMs;
    if(direct&&udpPeerDirectAllowed(this))this.direct.set(sessionId,pairs.map(row=>({scope:row.scope,client:{id:row.client.wire.id},host:{id:row.host.wire.id}})));
    this.worker.postMessage({type:'grant',session:sessionId,expiresAt,legs:pairs.flatMap(row=>[row.client.wire,row.host.wire])});
    this.grants.add(sessionId);
    const secret=leg=>({id:leg.id,ticket:leg.ticket});
    if(tracks)return {port:this.port,expiresAt,tracks:pairs.map(row=>({...row.scope,client:secret(row.client),host:secret(row.host)}))};
    return {port:this.port,expiresAt,client:secret(pairs[0].client),host:secret(pairs[0].host)};
  }
  revoke(sessionId) { this.direct.delete(sessionId); if (this.grants.delete(sessionId)) this.worker.postMessage({ type: 'revoke', session: sessionId }); }
  bound({ session, id, address, port }) {
    if (!udpPeerDirectAllowed(this)) return;
    const pair = this.direct.get(session)?.find(row=>row.client.id===id||row.host.id===id);
    if (!pair || !this.allowed(session)) return;
    const role = pair.client.id === id ? 'client' : pair.host.id === id ? 'host' : null;
    if (!role || pair[role].observed) return;
    pair[role].observed = { address, port }; this.offerPeer(session, role, pair);
  }
  local(session, role, message) {
    if (!udpPeerDirectAllowed(this)) return;
    const pair=this.direct.get(session)?.find(row=>row.scope?row.scope.track===message.track&&row.scope.topologyEpoch===message.topologyEpoch&&row.scope.epoch===message.epoch&&row.scope.generation===message.generation:message.track===undefined&&message.topologyEpoch===undefined&&message.epoch===undefined&&message.generation===undefined),candidate=localCandidate(message),kind=message.candidateType==='srflx'?'reflexive':'local';
    if (!pair || !['client', 'host'].includes(role) || !this.allowed(session) || !candidate || pair[role][kind]) return;
    pair[role][kind] = candidate; this.offerPeer(session, role, pair);
  }
  offerPeer(session, role, pair) {
    if (!udpPeerDirectAllowed(this)) return;
    const row = pair?.[role];
    if (!row?.observed) return;
    const candidates=[row.observed,...(row.reflexive?[row.reflexive]:[]),...(row.local?[row.local]:[])],opposite=role==='client'?'host':'client';
    if(pair.scope)this.onPeer?.(session,opposite,candidates,pair.scope);else this.onPeer?.(session,opposite,candidates);
  }
  sweep() { for (const sessionId of this.grants) if (!this.allowed(sessionId)) this.revoke(sessionId); }
  stats() {
    const [bindings, forwarded, forwardedBytes, ...drops] = this.counts;
    // recvBuffer/sendBuffer: the sizes the kernel granted (Linux reports twice the capped
    // request). kernel: the socket's receive queue and overflow drops, Linux only.
    const running = this.running;
    return { running, port: running ? this.port : null, sessions: this.grants.size, bindings, forwarded, forwardedBytes, restarts: this.restarts, dropped: Object.fromEntries(dropReasons.map((reason, index) => [reason, drops[index]])),
      recvBuffer: running ? this.buffers.recvBuffer : null, sendBuffer: running ? this.buffers.sendBuffer : null, kernel: running ? kernelSocket(this.port) : null };
  }
  close() {
    this.closed = true; this.running = false; this.grants.clear(); this.direct.clear();
    clearInterval(this.timer); clearTimeout(this.restart);
    return this.worker?.terminate();
  }
}
