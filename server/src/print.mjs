import { createHash } from 'node:crypto';
import { WebSocketServer, WebSocket } from 'ws';
import { HttpError } from './errors.mjs';
import { tokenHash } from './security.mjs';

// Streaming printer redirection (print-v1; see docs/remote-session-protocol.md
// "Printing"). While a granted stream session runs, the Host keeps one printer
// queue in the VM ("Multitap 프린터 (PC)", the in-box Microsoft Print To PDF
// driver on a loopback port only the spooler may use). Each document printed
// there comes to the client as a PDF over this session's own relayed socket
// pair, and the client prints it on a local printer (or saves it) after the
// user chooses. The grant is the per-assignment (user x VM) `printers`
// device permission; default denied.
//
// Every field exists only in a session whose client requested it, whose Host
// advertised it and whose administrator allowed it; released peers never see
// it. Revoking the permission closes this channel (the Host then removes its
// printer) without ending the stream.
export const printProtocol = 'print-v1';
// A chunk of 128 KiB is 174764 base64 characters; eight may be unacknowledged.
export const printLimits = Object.freeze({ chunkBytes: 131072, windowChunks: 8, maxJobBytes: 256 * 1024 * 1024 });
const maxMessage = 256 * 1024;
const maxTitle = 200;
const jobPattern = /^[A-Za-z0-9_-]{1,64}$/;
const results = new Set(['printed', 'saved', 'cancelled', 'failed']);
// Jobs the Host received but could not offer (fixed reasons only).
const skipReasons = new Set(['print_job_too_large', 'print_storage_unavailable', 'print_job_failed']);
// Why the Host could not prepare its printer queue (fixed reasons only).
const queueReasons = new Set(['print_driver_unavailable', 'print_spooler_unavailable', 'print_name_conflict', 'print_setup_failed']);
const protocolErrors = new Set(['print_invalid_message', 'print_out_of_order', 'print_limits_exceeded', 'print_hash_mismatch', 'print_busy']);
const exact = (value, keys) => Object.keys(value).length === keys.length && keys.every(key => Object.hasOwn(value, key));
function require(ok, reason = 'print_invalid_message') { if (!ok) throw new Error(reason); }

// Host capability: exactly the marker; anything else is omitted.
export function printCapability(value) {
  return value?.printProtocol === printProtocol;
}

// Why a Host cannot offer printing (print-v1 is then absent): the Print Spooler
// service is not running, or the driver / port monitor the printer needs is
// missing. Shown to a client only where the assignment allows printers
// (visibleCapabilities), so the session can say why there is no printer.
export const printUnavailableCapability = value => ['print_spooler_unavailable', 'print_driver_unavailable'].includes(value?.printUnavailableReason) && value.printProtocol === undefined && value.service === true;

// Connect body field. Malformed requests are refused before any session.
export function requestedPrint(mode, protocol) {
  if (protocol === undefined) return false;
  if (mode !== 'stream' || protocol !== printProtocol) throw new HttpError(400, 'invalid_print_protocol');
  return true;
}

// Never a reason to refuse a connection: an incapable Host or a denied
// assignment leaves it ungranted. Independent outputs (tracks-v1) print too.
export function negotiatePrint(requested, host, redirectionPolicy) {
  return requested === true && printCapability(host?.capabilities) && redirectionPolicy?.printers === true;
}

// A document title for the client's dialog: plain text, bounded, may be empty.
export function validPrintTitle(value) {
  return typeof value === 'string' && value.length <= maxTitle && value.isWellFormed() && !/[\x00-\x1f\x7f]/.test(value);
}

// Relay only: no file is kept here. One document is in transfer per session;
// its bytes are hashed on the way and must match the Host's end digest.
export class PrintRelay {
  constructor(relay, { pairReadyMs = 15000, idleMs = 60000, totalMs = 1800000, maxQueue = 2 * 1024 * 1024 } = {}) {
    Object.assign(this, { relay, pairReadyMs, idleMs, totalMs, maxQueue });
    this.pairs = new Set(); this.windowAt = Date.now(); this.windowBytes = 0;
    this.wss = new WebSocketServer({ noServer: true, maxPayload: maxMessage, perMessageDeflate: false, maxFragments: 128, closeTimeout: 1000 });
  }
  permitted(session) { return this.relay.store.devicePolicy(session.userId, session.desktopId).printers === true; }
  available(session, signinOwner = null) {
    return session.mode === 'stream' && session.print === true && this.relay.current(session) && session.ws?.readyState === WebSocket.OPEN &&
      this.relay.authorized(session) && this.permitted(session) && (!session.signinPolicy || this.relay.signinRedirectAvailable?.(session, signinOwner) === true);
  }
  unavailableReason(session) {
    // channel-resume-v1: the stream waits for a side; the channel may be dialled again.
    if (this.relay.channels?.away(session)) return 'print_stream_away';
    return !this.relay.current(session) || !this.relay.authorized(session) ? 'authorization_revoked' : !this.permitted(session) ? 'print_policy_changed' : 'print_unavailable';
  }
  upgrade(req, socket, head, url, reject) {
    const hostId = /^\/api\/v1\/hosts\/sessions\/([^/]+)\/print$/.exec(url.pathname)?.[1];
    const clientId = /^\/api\/v1\/sessions\/([^/]+)\/print$/.exec(url.pathname)?.[1];
    if (hostId === undefined && clientId === undefined) return false;
    const role = hostId === undefined ? 'client' : 'host', session = this.relay.sessions.get(hostId ?? clientId);
    // channel-resume-v1 (channel-resume.mjs): not now (503) while the stream is away, never again (409) after a final end.
    const refusal = session ? this.relay.channels?.refusal(session, 'print') ?? 0 : 0;
    if (!session || (!refusal && !this.available(session))) { reject(401, 'Unauthorized'); return true; }
    if (role === 'client') {
      const token = /^Bearer ([A-Za-z0-9_-]{43})$/.exec(req.headers.authorization ?? '')?.[1];
      if (!token || tokenHash(token) !== session.tokenHash) { reject(401, 'Unauthorized'); return true; }
    } else {
      const token = /^Host ([A-Za-z0-9_-]{43})$/.exec(req.headers.authorization ?? '')?.[1], hash = token ? tokenHash(token) : null;
      const identity = hash ? this.relay.store.authenticateHost(hash) : null;
      if (!identity || identity.id !== session.host.identity.id || hash !== session.host.tokenHash) { reject(401, 'Unauthorized'); return true; }
    }
    if (refusal) { reject(refusal, refusal === 503 ? 'Unavailable' : 'Conflict'); return true; }
    // The same peer dials again while its old socket is still held: that pair is gone.
    if (session.printPair?.[role] && !session.printPair.closed && this.relay.channels?.enabled(session)) this.closePair(session.printPair, 'print_transport_closed');
    if (session.printPair?.[role] || session.printPair?.closed) { reject(409, 'Conflict'); return true; }
    if (!session.printPair && (this.pairs.size >= 128 || this.relay.channels?.admit(session, 'print') === false)) { reject(503, 'Unavailable'); return true; }
    const observedPair = session.printPair;
    const guard = () => !socket.destroyed && this.available(session) && session.printPair === observedPair && !observedPair?.[role] && !observedPair?.closed && (observedPair || this.pairs.size < 128);
    const accept = () => this.wss.handleUpgrade(req, socket, head, ws => this.attach(ws, session, role));
    if (session.signinPolicy && this.relay.admitSigninRedirect) void this.relay.admitSigninRedirect(session, guard, accept, () => reject(401, 'Unauthorized'));
    else if (!session.signinPolicy && guard()) accept(); else reject(401, 'Unauthorized'); return true;
  }
  attach(ws, session, role) {
    if (!this.available(session)) { ws.close(1008, 'redirect_unavailable'); return; }
    let pair = session.printPair;
    if (!pair) { pair = { session, signinOwner: this.relay.signinRedirectOwner?.(session) ?? null, createdAt: Date.now(), ready: false, closed: false, transfer: null, seen: new Set(), ended: new Set(), cancelled: new Set() }; session.printPair = pair; this.pairs.add(pair); }
    pair[role] = ws; pair[role + 'Alive'] = true;
    const rate = { at: Date.now(), messages: 0, bytes: 0 };
    ws.on('pong', () => { if (this.current(pair, role, ws)) pair[role + 'Alive'] = true; });
    ws.on('error', () => this.closePair(pair, 'print_transport_closed'));
    ws.on('close', () => this.closePair(pair, 'print_transport_closed'));
    ws.on('message', (data, binary) => {
      if (!this.current(pair, role, ws)) return;
      if (!this.available(session, pair.signinOwner)) return this.closePair(pair, this.unavailableReason(session));
      const now = Date.now();
      if (now - rate.at >= 1000) { rate.at = now; rate.messages = 0; rate.bytes = 0; }
      if (now - this.windowAt >= 1000) { this.windowAt = now; this.windowBytes = 0; }
      if (++rate.messages > 512 || (rate.bytes += data.length) > 24 * 1024 * 1024 || (this.windowBytes += data.length) > 64 * 1024 * 1024) return this.closePair(pair, 'print_rate_limit');
      if (binary || data.length > maxMessage || !pair.ready) return this.closePair(pair, 'print_invalid_message');
      try {
        const value = JSON.parse(data.toString('utf8'));
        require(value && typeof value === 'object' && !Array.isArray(value) && typeof value.type === 'string');
        this.receive(pair, role, value);
      } catch (error) { this.closePair(pair, protocolErrors.has(error.message) ? error.message : 'print_invalid_message'); }
    });
    if (pair.client && pair.host) {
      pair.ready = true;
      const ready = { type: 'print.ready', sessionId: session.id, protocol: printProtocol, limits: printLimits };
      this.send(pair, 'client', ready); this.send(pair, 'host', ready);
    }
  }
  current(pair, role, ws) { return !pair.closed && pair.session.printPair === pair && pair[role] === ws; }
  send(pair, role, value) {
    if (pair.closed) return false;
    const ws = pair[role], encoded = JSON.stringify(value), size = Buffer.byteLength(encoded);
    if (ws?.readyState !== WebSocket.OPEN || size > maxMessage || ws.bufferedAmount + size > this.maxQueue) { this.closePair(pair, 'print_backpressure'); return false; }
    ws.send(encoded, error => { if (error) this.closePair(pair, 'print_transport_closed'); }); return true;
  }
  other(role) { return role === 'host' ? 'client' : 'host'; }
  // Messages of a job that just ended or was cancelled may still cross on the
  // other socket; they are dropped, never applied to a later job.
  retired(pair, jobId) { return pair.ended.has(jobId) || pair.cancelled.has(jobId); }
  receive(pair, role, value) {
    const transfer = pair.transfer;
    if (transfer) transfer.at = Date.now();
    if (value.type === 'print.cancel') {
      require(exact(value, ['type', 'jobId']) && typeof value.jobId === 'string');
      if (transfer?.jobId !== value.jobId) { require(this.retired(pair, value.jobId), 'print_out_of_order'); return; }
      pair.transfer = null; pair.cancelled.add(value.jobId);
      while (pair.cancelled.size > 64) pair.cancelled.delete(pair.cancelled.values().next().value);
      this.send(pair, this.other(role), value); return;
    }
    if (role === 'host') {
      if ((value.type === 'print.chunk' || value.type === 'print.end') && typeof value.jobId === 'string' && transfer?.jobId !== value.jobId && pair.cancelled.has(value.jobId)) return;
      if (value.type === 'print.job') {
        require(exact(value, ['type', 'jobId', 'format', 'size', 'title']) && typeof value.jobId === 'string' && jobPattern.test(value.jobId) && value.format === 'pdf' && validPrintTitle(value.title));
        require(Number.isSafeInteger(value.size) && value.size >= 1 && value.size <= printLimits.maxJobBytes, 'print_limits_exceeded');
        require(!transfer, 'print_busy');
        require(!pair.seen.has(value.jobId) && pair.seen.size < 4096);
        pair.seen.add(value.jobId);
        const now = Date.now();
        pair.transfer = { jobId: value.jobId, size: value.size, sent: 0, acked: 0, hash: createHash('sha256'), startedAt: now, at: now };
        this.send(pair, 'client', value); return;
      }
      if (value.type === 'print.chunk') {
        require(exact(value, ['type', 'jobId', 'offset', 'data']) && typeof value.data === 'string' && value.data.length <= Math.ceil(printLimits.chunkBytes / 3) * 4);
        require(transfer?.jobId === value.jobId && Number.isSafeInteger(value.offset) && value.offset === transfer.sent, 'print_out_of_order');
        const decoded = Buffer.from(value.data, 'base64');
        require(decoded.length >= 1 && decoded.length <= printLimits.chunkBytes && decoded.toString('base64') === value.data && decoded.length <= transfer.size - transfer.sent, 'print_limits_exceeded');
        require(transfer.sent + decoded.length - transfer.acked <= printLimits.chunkBytes * printLimits.windowChunks, 'print_out_of_order');
        transfer.hash.update(decoded); transfer.sent += decoded.length;
        this.send(pair, 'client', value); return;
      }
      if (value.type === 'print.end') {
        require(exact(value, ['type', 'jobId', 'sha256']) && typeof value.sha256 === 'string' && /^[a-f0-9]{64}$/.test(value.sha256));
        require(transfer?.jobId === value.jobId && transfer.sent === transfer.size, 'print_out_of_order');
        require(transfer.hash.digest('hex') === value.sha256, 'print_hash_mismatch');
        pair.transfer = null; pair.ended.add(value.jobId);
        while (pair.ended.size > 64) pair.ended.delete(pair.ended.values().next().value);
        this.send(pair, 'client', value); return;
      }
      if (value.type === 'print.skipped') {
        require(exact(value, ['type', 'reason']) && skipReasons.has(value.reason));
        this.send(pair, 'client', value); return;
      }
      // The VM's printer queue is ready, or why it could not be prepared.
      if (value.type === 'print.queue') {
        require(value.state === 'ready' ? exact(value, ['type', 'state']) : value.state === 'unavailable' && exact(value, ['type', 'state', 'reason']) && queueReasons.has(value.reason));
        this.send(pair, 'client', value); return;
      }
      throw new Error('print_invalid_message');
    }
    if (value.type === 'print.ack') {
      require(exact(value, ['type', 'jobId', 'offset']) && typeof value.jobId === 'string');
      // Acknowledgements of a just ended or cancelled job are stale, not wrong.
      if (transfer?.jobId !== value.jobId) { require(this.retired(pair, value.jobId), 'print_out_of_order'); return; }
      require(Number.isSafeInteger(value.offset) && value.offset > transfer.acked && value.offset <= transfer.sent, 'print_out_of_order');
      transfer.acked = value.offset; this.send(pair, 'host', value); return;
    }
    if (value.type === 'print.result') {
      require(exact(value, ['type', 'jobId', 'result']) && typeof value.jobId === 'string' && results.has(value.result));
      require(pair.ended.delete(value.jobId), 'print_out_of_order');
      this.send(pair, 'host', value); return;
    }
    throw new Error('print_invalid_message');
  }
  closePair(pair, reason) {
    if (pair.closed) return;
    pair.closed = true; pair.transfer = null; this.pairs.delete(pair);
    // channel-resume-v1: after a transport end the same session may pair again.
    if (this.relay.channels?.closed(pair.session, 'print', reason) && pair.session.printPair === pair) pair.session.printPair = null;
    for (const role of ['client', 'host']) {
      const ws = pair[role]; if (!ws) continue;
      if (ws.readyState === WebSocket.OPEN) {
        const value = JSON.stringify({ type: 'print.error', reason });
        if (ws.bufferedAmount + Buffer.byteLength(value) <= this.maxQueue) ws.send(value);
        ws.close(1008, reason.slice(0, 100));
      } else ws.terminate();
    }
  }
  closeSession(session, reason) { if (session.printPair) this.closePair(session.printPair, reason); this.relay.channels?.closed(session, 'print', reason); }
  // The administrator changed this user's grants: end printing where the
  // `printers` permission is gone (the authorization sweep would within 250 ms).
  policyChanged(userId, desktopId) {
    for (const pair of this.pairs) {
      const session = pair.session;
      if (session.userId === userId && (desktopId === undefined || session.desktopId === desktopId) && !this.permitted(session)) this.closePair(pair, 'print_policy_changed');
    }
  }
  revalidate() {
    const now = Date.now();
    for (const pair of this.pairs) {
      if (!this.available(pair.session, pair.signinOwner)) { this.closePair(pair, this.unavailableReason(pair.session)); continue; }
      if (!pair.ready && now - pair.createdAt > this.pairReadyMs) { this.closePair(pair, 'print_pair_timeout'); continue; }
      const transfer = pair.transfer;
      if (transfer && (now - transfer.at > this.idleMs || now - transfer.startedAt > this.totalMs)) this.closePair(pair, 'print_timeout');
    }
  }
  heartbeat() {
    for (const pair of this.pairs) for (const role of ['client', 'host']) {
      const ws = pair[role]; if (ws?.readyState !== WebSocket.OPEN) continue;
      if (pair[role + 'Alive'] === false) { this.closePair(pair, 'print_transport_closed'); break; }
      pair[role + 'Alive'] = false; ws.ping();
    }
  }
  close() { for (const pair of this.pairs) this.closePair(pair, 'server_stopping'); this.wss.close(); }
}
