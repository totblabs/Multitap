import { WebSocketServer, WebSocket } from 'ws';
import { HttpError } from './errors.mjs';
import { tokenHash } from './security.mjs';

// Streaming drive redirection (drive-v1; docs/drive-redirection.md). Folders
// the user shares on the PC are served to the VM's in-box WebDAV redirector by
// the Host; every file operation is a bounded request relayed here to the
// client. The relay authorizes the pair, checks every message, keeps the
// client's share list (unknown share / read-only refused here), bounds
// requests in flight and closes the pair (never the stream) on revocation.
// It never stores file data, names or paths.
export const driveProtocol = 'drive-v1';
export const driveLimits = Object.freeze({ maxShares: 8, chunkBytes: 262144, maxInFlight: 8, listPage: 256, maxPath: 1024, maxName: 255, maxDepth: 64 });
const maxMessage = 288 * 1024, maxJson = 256 * 1024, maxHeader = 4096, maxLabel = 64;
const shareId = /^[A-Za-z0-9_-]{1,32}$/;
export const driveErrors = Object.freeze(['drive_not_found', 'drive_exists', 'drive_not_empty', 'drive_read_only', 'drive_denied', 'drive_link_refused', 'drive_invalid_path', 'drive_share_unknown', 'drive_busy', 'drive_no_space', 'drive_is_directory', 'drive_not_directory', 'drive_io_failed']);
export const driveMountErrors = Object.freeze(['drive_webclient_unavailable', 'drive_letter_unavailable', 'drive_map_failed', 'no_interactive_session']);
const errors = new Set(driveErrors), mountErrors = new Set(driveMountErrors);
// One-time hints from the Host to the person at the PC (not errors of a request):
// WebClient still runs with its old 50 MB file cap; a file of 4 GiB or more was seen.
export const driveNoticeCodes = Object.freeze(['drive_webclient_old_limit', 'drive_file_too_large']);
const noticeCodes = new Set(driveNoticeCodes);
const pairReasons = new Set(['drive_invalid_message', 'drive_limits_exceeded', 'drive_out_of_order', 'drive_rate_limit']);
const modifying = new Set(['create', 'rename', 'delete', 'touch', 'write', 'setsize']);
const maxTime = 253402300799999; // 9999-12-31T23:59:59.999Z
const maxSize = Number.MAX_SAFE_INTEGER;
const exact = (value, keys) => Object.keys(value).length === keys.length && keys.every(key => Object.hasOwn(value, key));
const object = value => !!value && typeof value === 'object' && !Array.isArray(value);
const integer = (value, min, max) => Number.isSafeInteger(value) && value >= min && value <= max;
function require(ok, reason = 'drive_invalid_message') { if (!ok) throw new Error(reason); }

// Host capability: exactly the marker of a service with an interactive session.
export const driveCapability = value => value?.driveProtocol === driveProtocol && value.service === true && value.interactiveSession === true;
// Why a Host with a Windows user cannot offer drives (drive-v1 is then absent):
// the VM's WebClient (WebDAV) service is disabled or missing. Shown to a client
// only where the assignment allows drives (visibleCapabilities), so the
// session can say why there is no 드라이브 button instead of hiding it.
export const driveUnavailableCapability = value => value?.driveUnavailableReason === 'drive_webclient_unavailable' && value.driveProtocol === undefined && value.service === true && value.interactiveSession === true;

// The ranged-file-system revision (WinFsp backend): the Host also advertises
// `driveFs: true`, and a session that negotiated it may carry the extra ops `setsize` and
// `flush`. Released peers never negotiate it, so they never see those ops (WebDAV semantics).
export const driveFsCapability = value => driveCapability(value) && value.driveFs === true;
export function requestedDriveFs(mode, protocol, fs) {
  if (fs === undefined) return false;
  if (fs !== true || protocol === undefined || mode !== 'stream') throw new HttpError(400, 'invalid_drive_protocol');
  return true;
}
// Connect body field. Malformed requests are refused before any session.
export function requestedDrive(mode, protocol) {
  if (protocol === undefined) return false;
  if (mode !== 'stream' || protocol !== driveProtocol) throw new HttpError(400, 'invalid_drive_protocol');
  return true;
}
// Never a reason to refuse a connection: an incapable Host or an assignment
// without `drives` leaves it ungranted.
export const negotiateDrive = (requested, host, policy) => requested === true && driveCapability(host?.capabilities) && policy?.drives === true;

// One plain Windows file name (the same rule as the Host and the client).
export function safeDriveName(name, max = driveLimits.maxName) {
  if (typeof name !== 'string' || !name || name.length > max || !name.isWellFormed() || /[\x00-\x1f<>:"/\\|?*]/.test(name) || /[. ]$/.test(name) || name === '.' || name === '..') return false;
  const stem = name.split('.')[0].replace(/[ .]+$/, '').toUpperCase();
  return !/^(?:CON|PRN|AUX|NUL|CONIN\$|CONOUT\$|(?:COM|LPT)[1-9¹²³])$/.test(stem);
}
// '' (the share root) or 1-64 safe segments joined with '/', at most 1024 units.
export function safeDrivePath(path) {
  if (path === '') return true;
  if (typeof path !== 'string' || path.length > driveLimits.maxPath) return false;
  const segments = path.split('/');
  return segments.length <= driveLimits.maxDepth && segments.every(segment => safeDriveName(segment));
}
function entry(value, root, stat) {
  require(object(value));
  const keys = Object.keys(value), allowed = ['name', 'dir', 'size', 'modified', 'created', 'readOnly', ...(root && stat ? ['space'] : [])];
  require(['name', 'dir', 'size', 'modified', 'created', 'readOnly'].every(key => Object.hasOwn(value, key)) && keys.every(key => allowed.includes(key)));
  require(root && stat ? value.name === '' : safeDriveName(value.name));
  require(typeof value.dir === 'boolean' && typeof value.readOnly === 'boolean' && integer(value.size, 0, maxSize) && (!value.dir || value.size === 0) && integer(value.modified, 0, maxTime) && integer(value.created, 0, maxTime));
  if (Object.hasOwn(value, 'space')) require(object(value.space) && exact(value.space, ['free', 'total']) && integer(value.space.free, 0, maxSize) && integer(value.space.total, 0, maxSize) && value.space.free <= value.space.total);
}
function shares(value) {
  require(exact(value, ['type', 'shares']) && Array.isArray(value.shares), 'drive_invalid_message');
  require(value.shares.length <= driveLimits.maxShares, 'drive_limits_exceeded');
  const list = new Map();
  for (const share of value.shares) {
    require(object(share) && exact(share, ['id', 'name', 'readOnly']) && typeof share.id === 'string' && shareId.test(share.id) && safeDriveName(share.name, maxLabel) && typeof share.readOnly === 'boolean' && !list.has(share.id));
    list.set(share.id, share.readOnly);
  }
  return list;
}
function mounted(value) {
  require(exact(value, ['type', 'shares']) && Array.isArray(value.shares) && value.shares.length <= driveLimits.maxShares);
  const ids = new Set();
  for (const share of value.shares) {
    require(object(share) && typeof share.id === 'string' && shareId.test(share.id) && !ids.has(share.id)); ids.add(share.id);
    if (share.state === 'mapped') require(exact(share, ['id', 'state', 'letter']) && typeof share.letter === 'string' && /^[D-Z]$/.test(share.letter));
    else if (share.state === 'failed') require(exact(share, ['id', 'state', 'error']) && mountErrors.has(share.error));
    else require(share.state === 'pending' && exact(share, ['id', 'state']));
  }
}
// A Host request's operation fields (beyond type/id/op/share/path).
const requestFields = { stat: [], list: ['cursor'], read: ['offset', 'length'], create: ['dir', 'truncate'], rename: ['to', 'replace'], delete: [], touch: ['modified'], setsize: ['size'], flush: [] };
const fsOps = new Set(['setsize', 'flush']);
function request(value, fs = false) {
  require(!fsOps.has(value.op) || fs === true);
  const fields = requestFields[value.op];
  require(typeof value.op === 'string' && Object.hasOwn(requestFields, value.op) && exact(value, ['type', 'id', 'op', 'share', 'path', ...fields]));
  require(typeof value.share === 'string' && shareId.test(value.share) && safeDrivePath(value.path));
  if (value.op === 'list') require(integer(value.cursor, 0, 0xffffffff));
  if (value.op === 'read') require(integer(value.offset, 0, maxSize) && integer(value.length, 1, driveLimits.chunkBytes));
  if (value.op === 'create') require(typeof value.dir === 'boolean' && typeof value.truncate === 'boolean' && !(value.dir && value.truncate) && value.path !== '');
  if (value.op === 'rename') require(typeof value.replace === 'boolean' && value.path !== '' && value.to !== '' && safeDrivePath(value.to) && value.to !== value.path);
  if (value.op === 'delete') require(value.path !== '');
  if (value.op === 'touch') require(value.path !== '' && integer(value.modified, 0, maxTime));
  if (value.op === 'setsize') require(value.path !== '' && integer(value.size, 0, maxSize));
}

// Optional bilateral extension. No path/command is ever accepted from the client.
export const driveOpenProtocol = 'drive.open-v1';
const openErrors = new Set(['drive_share_unknown', 'drive_open_unavailable', 'drive_open_failed', 'drive_open_unknown', 'drive_busy', 'no_interactive_session']);
export class DriveRelay {
  // Flow budgets (per second). Honest peers are bounded by the protocol window
  // (8 requests in flight x 256 KiB), so a fast LAN copy is limited only by the
  // RTT and can exceed any fixed byte rate: going over a soft budget pauses
  // reading the socket until the window rolls over (TCP backpressure to the
  // sender, nothing buffered in the broker); only a peer that keeps sending
  // far past the pause (hard budget) is closed as abusive.
  constructor(relay, { pairReadyMs = 15000, requestMs = 60000, maxQueue = 4 * 1024 * 1024, softMessages = 2048, softBytes = 160 * 1024 * 1024, softTotalBytes = 512 * 1024 * 1024, hardMessageFactor = 8, hardByteFactor = 2 } = {}) {
    Object.assign(this, { relay, pairReadyMs, requestMs, maxQueue, softMessages, softBytes, softTotalBytes, hardMessageFactor, hardByteFactor });
    this.pairs = new Set(); this.windowAt = Date.now(); this.windowBytes = 0;
    this.wss = new WebSocketServer({ noServer: true, maxPayload: maxMessage, perMessageDeflate: false, maxFragments: 128, closeTimeout: 1000 });
  }
  allowed(session) { return this.relay.store.devicePolicy(session.userId, session.desktopId)?.drives === true; }
  available(session, signinOwner = null) { return session.mode === 'stream' && session.driveEnabled === true && this.relay.current(session) && session.ws?.readyState === WebSocket.OPEN && this.relay.authorized(session) && this.allowed(session) && (!session.signinPolicy || this.relay.signinRedirectAvailable?.(session, signinOwner) === true); }
  // channel-resume-v1: while the stream waits for a side the channel may be dialled again.
  unavailableReason(session) { return this.relay.channels?.away(session) ? 'drive_stream_away' : !this.relay.current(session) || !this.relay.authorized(session) ? 'authorization_revoked' : !this.allowed(session) ? 'drive_policy_changed' : 'drive_unavailable'; }
  upgrade(req, socket, head, url, reject) {
    const hostId = /^\/api\/v1\/hosts\/sessions\/([^/]+)\/drive$/.exec(url.pathname)?.[1];
    const clientId = /^\/api\/v1\/sessions\/([^/]+)\/drive$/.exec(url.pathname)?.[1];
    if (hostId === undefined && clientId === undefined) return false;
    const role = hostId === undefined ? 'client' : 'host', session = this.relay.sessions.get(hostId ?? clientId);
    // channel-resume-v1 (channel-resume.mjs): not now (503) while the stream is away, never again (409) after a final end.
    const refusal = session ? this.relay.channels?.refusal(session, 'drive') ?? 0 : 0;
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
    if (session.drivePair?.[role] && this.relay.channels?.enabled(session)) this.closePair(session.drivePair, 'drive_transport_closed');
    // One pair per session and one socket per role; a closed pair is final.
    if (session.drivePair?.[role] || session.drivePair?.closed || session.driveClosed) { reject(409, 'Conflict'); return true; }
    if (!session.drivePair && (this.pairs.size >= 128 || this.relay.channels?.admit(session, 'drive') === false)) { reject(503, 'Unavailable'); return true; }
    const observedPair = session.drivePair;
    const guard = () => !socket.destroyed && this.available(session) && session.drivePair === observedPair && !observedPair?.[role] && !observedPair?.closed && (observedPair || this.pairs.size < 128);
    const accept = () => this.wss.handleUpgrade(req, socket, head, ws => this.attach(ws, session, role, req));
    if (session.signinPolicy && this.relay.admitSigninRedirect) void this.relay.admitSigninRedirect(session, guard, accept, () => reject(401, 'Unauthorized'));
    else if (!session.signinPolicy && guard()) accept(); else reject(401, 'Unauthorized');
    return true;
  }
  attach(ws, session, role, req) {
    if (!this.available(session)) { ws.close(1008, 'redirect_unavailable'); return; }
    let pair = session.drivePair;
    if (!pair) { pair = { session, signinOwner: this.relay.signinRedirectOwner?.(session) ?? null, createdAt: Date.now(), ready: false, closed: false, shares: new Map(), mapped: new Map(), open: null, lastOpenId: 0, lastId: 0, outstanding: new Map() }; session.drivePair = pair; this.pairs.add(pair); }
    pair[role] = ws; pair[role + 'Alive'] = true;
    pair[role + 'Open'] = req?.headers?.['x-multitap-drive-open'] === driveOpenProtocol;
    // A client that understands drive.notice says so in a header (an older client
    // never does, and would close the pair on a message it does not know).
    if (role === 'client') pair.clientNotices = req?.headers?.['x-multitap-drive-notices'] === '1';
    const rate = { at: Date.now(), messages: 0, bytes: 0 };
    ws.on('pong', () => { if (this.current(pair, role, ws)) pair[role + 'Alive'] = true; });
    ws.on('error', () => this.closePair(pair, 'drive_transport_closed'));
    ws.on('close', () => this.closePair(pair, 'drive_transport_closed'));
    ws.on('message', (data, binary) => {
      if (!this.current(pair, role, ws)) return;
      if (!this.available(session, pair.signinOwner)) return this.closePair(pair, this.unavailableReason(session));
      const now = Date.now();
      if (now - rate.at >= 1000) { rate.at = now; rate.messages = 0; rate.bytes = 0; }
      if (now - this.windowAt >= 1000) { this.windowAt = now; this.windowBytes = 0; }
      rate.messages++; rate.bytes += data.length; this.windowBytes += data.length;
      if (rate.messages > this.softMessages * this.hardMessageFactor || rate.bytes > this.softBytes * this.hardByteFactor) return this.closePair(pair, 'drive_rate_limit');
      if (rate.messages > this.softMessages || rate.bytes > this.softBytes || this.windowBytes > this.softTotalBytes) this.throttle(pair, role, ws, Math.max(1, Math.min(rate.at, this.windowAt) + 1000 - now));
      if (!pair.ready || data.length > maxMessage) return this.closePair(pair, 'drive_invalid_message');
      try {
        if (binary) this.binary(pair, role, data);
        else {
          require(data.length <= maxJson, 'drive_limits_exceeded');
          const value = JSON.parse(data.toString('utf8'));
          require(object(value) && typeof value.type === 'string');
          this.receive(pair, role, value);
        }
      } catch (error) { this.closePair(pair, pairReasons.has(error.message) ? error.message : 'drive_invalid_message'); }
    });
    if (pair.client && pair.host) {
      pair.ready = true;
      const ready = { type: 'drive.ready', sessionId: session.id, protocol: driveProtocol, limits: driveLimits };
      pair.openEnabled = pair.clientOpen === true && pair.hostOpen === true;
      if (pair.openEnabled) ready.openProtocol = driveOpenProtocol;
      // The Host is told it may send notices (released Hosts ignore the extra field).
      this.send(pair, 'client', ready); this.send(pair, 'host', { ...ready, notices: true });
    }
  }
  // Stop reading this peer's socket for ms; the sender's TCP window fills and it waits.
  throttle(pair, role, ws, ms) {
    if (pair[role + 'Paused']) return;
    pair[role + 'Paused'] = true; this.throttled = (this.throttled ?? 0) + 1; ws.pause();
    setTimeout(() => { pair[role + 'Paused'] = false; if (this.current(pair, role, ws)) ws.resume(); }, ms).unref();
  }
  current(pair, role, ws) { return !pair.closed && pair.session.drivePair === pair && pair[role] === ws; }
  send(pair, role, value, binary = false) {
    if (pair.closed) return false;
    const ws = pair[role], payload = binary ? value : JSON.stringify(value), size = binary ? value.length : Buffer.byteLength(payload);
    if (ws?.readyState !== WebSocket.OPEN || size > maxMessage || ws.bufferedAmount + size > this.maxQueue) { this.closePair(pair, 'drive_backpressure'); return false; }
    ws.send(payload, { binary }, error => { if (error) this.closePair(pair, 'drive_transport_closed'); });
    return true;
  }
  // A Host request id: strictly increasing, bounded in flight.
  admit(pair, id) {
    require(integer(id, pair.lastId + 1, 0xffffffff), 'drive_out_of_order');
    require(pair.outstanding.size < driveLimits.maxInFlight, 'drive_limits_exceeded');
    pair.lastId = id;
  }
  // The relay answers what the client's current share list already decides.
  refusal(pair, op, share) {
    if (!pair.shares.has(share)) return 'drive_share_unknown';
    if (modifying.has(op) && pair.shares.get(share) === true) return 'drive_read_only';
    return null;
  }
  receive(pair, role, value) {
    if (role === 'client' && value.type === 'drive.shares') {
      pair.shares = shares(value);
      for (const id of pair.mapped.keys()) if (!pair.shares.has(id)) pair.mapped.delete(id);
      this.send(pair, 'host', value); return;
    }
    if (role === 'client' && value.type === 'drive.open') {
      require(pair.openEnabled && exact(value, ['type', 'id', 'share']) && integer(value.id, pair.lastOpenId + 1, 0xffffffff) && typeof value.share === 'string' && shareId.test(value.share));
      pair.lastOpenId = value.id;
      const mapping = pair.mapped.get(value.share);
      const error = pair.open ? 'drive_busy' : !pair.shares.has(value.share) || !mapping ? 'drive_share_unknown' : null;
      if (error) { this.send(pair, 'client', { type: 'drive.opened', id: value.id, share: value.share, ok: false, error }); return; }
      pair.open = { id: value.id, share: value.share, mapping };
      this.send(pair, 'host', value); return;
    }
    if (role === 'host' && value.type === 'drive.opened') {
      const pending = pair.open;
      require(pair.openEnabled && pending && value.id === pending.id && value.share === pending.share &&
        (value.ok === true ? exact(value, ['type', 'id', 'share', 'ok']) : value.ok === false && exact(value, ['type', 'id', 'share', 'ok', 'error']) && openErrors.has(value.error)));
      // A removed/replaced mapping cannot turn a late shell acknowledgement into success.
      const current = pair.shares.has(pending.share) && pair.mapped.get(pending.share) === pending.mapping;
      pair.open = null; // actual Host terminal response, not the client observation deadline
      this.send(pair, 'client', current ? value : { type: 'drive.opened', id: value.id, share: value.share, ok: false, error: 'drive_share_unknown' });
      return;
    }
    if (role === 'host' && value.type === 'drive.notice') {
      require(exact(value, ['type', 'code']) && noticeCodes.has(value.code));
      if (pair.clientNotices) this.send(pair, 'client', value);
      return;
    }
    if (role === 'host' && value.type === 'drive.mounted') {
      mounted(value);
      const current = new Map();
      for (const item of value.shares) if (item.state === 'mapped' && pair.shares.has(item.id)) {
        const old = pair.mapped.get(item.id);
        current.set(item.id, old?.letter === item.letter ? old : { letter: item.letter });
      }
      pair.mapped = current; this.send(pair, 'client', value); return;
    }
    if (role === 'host' && value.type === 'drive.request') {
      request(value, pair.session.driveFs === true); this.admit(pair, value.id);
      const refused = this.refusal(pair, value.op, value.share);
      if (refused) { this.send(pair, 'host', { type: 'drive.response', id: value.id, ok: false, error: refused }); return; }
      pair.outstanding.set(value.id, { op: value.op, path: value.path, length: value.length ?? 0, cursor: value.cursor ?? 0, at: Date.now() });
      this.send(pair, 'client', value); return;
    }
    if (role === 'client' && value.type === 'drive.response') {
      const pending = pair.outstanding.get(value.id);
      require(integer(value.id, 1, 0xffffffff) && pending, 'drive_out_of_order');
      if (value.ok === false) require(exact(value, ['type', 'id', 'ok', 'error']) && errors.has(value.error));
      else {
        require(value.ok === true && pending.op !== 'read');
        if (pending.op === 'stat') { require(exact(value, ['type', 'id', 'ok', 'entry'])); entry(value.entry, pending.path === '', true); }
        else if (pending.op === 'list') {
          require((exact(value, ['type', 'id', 'ok', 'entries']) || exact(value, ['type', 'id', 'ok', 'entries', 'next'])) && Array.isArray(value.entries));
          require(value.entries.length <= driveLimits.listPage, 'drive_limits_exceeded');
          for (const item of value.entries) entry(item, false, false);
          if (Object.hasOwn(value, 'next')) require(integer(value.next, pending.cursor + 1, 0xffffffff));
        } else require(exact(value, ['type', 'id', 'ok']));
      }
      pair.outstanding.delete(value.id); this.send(pair, 'host', value); return;
    }
    throw new Error('drive_invalid_message');
  }
  binary(pair, role, data) {
    if (role === 'client') {
      require(data.length >= 5 && data[0] === 1);
      const id = data.readUInt32LE(1), pending = pair.outstanding.get(id);
      require(pending?.op === 'read', 'drive_out_of_order');
      require(data.length - 5 <= pending.length, 'drive_limits_exceeded');
      pair.outstanding.delete(id); this.send(pair, 'host', data, true); return;
    }
    require(data.length >= 7 && data[0] === 2);
    const id = data.readUInt32LE(1), size = data.readUInt16LE(5);
    require(size >= 2 && size <= maxHeader && data.length >= 7 + size);
    let header;
    try { header = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(data.subarray(7, 7 + size))); } catch { throw new Error('drive_invalid_message'); }
    require(object(header) && exact(header, ['op', 'share', 'path', 'offset', 'truncate']) && header.op === 'write' && typeof header.share === 'string' && shareId.test(header.share));
    require(safeDrivePath(header.path) && header.path !== '' && integer(header.offset, 0, maxSize) && typeof header.truncate === 'boolean' && (!header.truncate || header.offset === 0));
    const length = data.length - 7 - size;
    require(length >= 1 && length <= driveLimits.chunkBytes, 'drive_limits_exceeded');
    this.admit(pair, id);
    const refused = this.refusal(pair, 'write', header.share);
    if (refused) { this.send(pair, 'host', { type: 'drive.response', id, ok: false, error: refused }); return; }
    pair.outstanding.set(id, { op: 'write', at: Date.now() });
    this.send(pair, 'client', data, true);
  }
  closePair(pair, reason) {
    if (pair.closed) return;
    pair.closed = true; pair.outstanding.clear(); pair.open = null; pair.mapped.clear(); this.pairs.delete(pair);
    // channel-resume-v1: after a transport end the same session may pair again.
    const again = this.relay.channels?.closed(pair.session, 'drive', reason) === true;
    if (pair.session.drivePair === pair) { pair.session.drivePair = null; if (!again) pair.session.driveClosed = true; }
    for (const role of ['client', 'host']) {
      const ws = pair[role]; if (!ws) continue;
      if (ws.readyState === WebSocket.OPEN) {
        const value = JSON.stringify({ type: 'drive.error', reason });
        if (ws.bufferedAmount + Buffer.byteLength(value) <= this.maxQueue) ws.send(value);
        ws.close(reason === 'session_ended' ? 1000 : 1008, reason.slice(0, 100));
      } else ws.terminate();
    }
  }
  closeSession(session, reason) { session.driveClosed = true; if (session.drivePair) this.closePair(session.drivePair, reason); this.relay.channels?.closed(session, 'drive', reason); }
  revalidate() {
    const now = Date.now();
    for (const pair of this.pairs) {
      if (!this.available(pair.session, pair.signinOwner)) { this.closePair(pair, this.unavailableReason(pair.session)); continue; }
      if (!pair.ready && now - pair.createdAt > this.pairReadyMs) { this.closePair(pair, 'drive_pair_timeout'); continue; }
      for (const pending of pair.outstanding.values()) if (now - pending.at > this.requestMs) { this.closePair(pair, 'drive_timeout'); break; }
    }
  }
  heartbeat() {
    for (const pair of this.pairs) for (const role of ['client', 'host']) {
      const ws = pair[role]; if (ws?.readyState !== WebSocket.OPEN) continue;
      if (pair[role + 'Paused']) continue; // a throttled socket cannot answer pings until it is read again
      if (pair[role + 'Alive'] === false) { this.closePair(pair, 'drive_transport_closed'); break; }
      pair[role + 'Alive'] = false; ws.ping();
    }
  }
  close() { for (const pair of this.pairs) this.closePair(pair, 'server_stopping'); this.wss.close(); }
}
