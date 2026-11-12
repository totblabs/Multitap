import { WebSocketServer, WebSocket } from 'ws';
import { HttpError } from './errors.mjs';
import { tokenHash } from './security.mjs';

// Streaming USB redirection (usb-v1; see docs/usb-redirection.md). A USB device
// plugged into the user's PC is made available inside the VM by the Host: the
// Host runs a loopback USB/IP server that only the VM's kernel (usbip-win2)
// connects to, and forwards the kernel's URBs (control, bulk, interrupt) to the
// client over this session's own relayed socket pair. The grant is the
// per-assignment (user x VM) `usbDevices` permission; default denied. A device whose descriptors include a class with its own
// permission (mass storage -> drives, printer -> printers, smart card ->
// smartCards, CDC -> serialPorts) also needs that permission.
//
// Every field exists only in a session whose client requested it, whose Host
// advertised it and whose administrator allowed it; released peers never see it.
// Revoking `usbDevices` closes this channel (the Host then detaches every
// device and the client gives them back) without ending the stream.
//
// Wire (the relay checks everything; nothing is trusted from either peer):
//   text frames (JSON, at most 16 KiB): usb.ready (relay), usb.status (Host),
//     usb.attach (client), usb.attached / usb.rejected (Host), usb.detach (both)
//   binary frames: a 24-byte little-endian header, then the payload.
//     off size field
//      0   1  type      1 submit (Host), 2 return (client), 3 unlink (Host),
//                       4 unlink-return (client)
//      1   1  device    1..255, never reused within a session
//      2   1  endpoint  bit 7 = IN, low 4 bits = number
//      3   1  transfer  0 control, 2 bulk, 3 interrupt (never isochronous)
//      4   4  seq       the Host's number; unique among the requests in flight
//      8   4  length    submit: bytes to write (OUT, payload follows) or to read
//                       (IN); return: bytes actually moved (IN: payload follows);
//                       unlink: the seq of the submit to cancel
//     12   4  status   (signed) submit/unlink: 0; return: 0 or one of usbStatuses
//     16   8  setup    control transfers only; zero otherwise
// The window (bytes and requests in flight) is shared by the whole session, so
// the relay's per-socket queue cap is never reached by honest peers and input
// and video never wait for this channel (they use other sockets).
export const usbProtocol = 'usb-v1';
// In a tracks-v1 grant: this broker puts usbUrl into that session.start too
// (brokers up to 0.5.111 granted usb-v1 there but never told the Host).
export const usbTracksProtocol = 'usb-tracks-v1';
export const usbLimits = Object.freeze({ maxUrbBytes: 262144, windowBytes: 1048576, maxInFlight: 32, maxDevices: 4, maxAttaches: 64, descriptorBytes: 4096 });
const headerBytes = 24;
const maxText = 16384;
const maxFrame = headerBytes + usbLimits.maxUrbBytes;
export const usbStatuses = Object.freeze([0, -2, -5, -19, -32, -62, -71, -75, -104, -108, -110]);
const typeSubmit = 1, typeReturn = 2, typeUnlink = 3, typeUnlinkReturn = 4;
const transferControl = 0, transferBulk = 2, transferInterrupt = 3;
const speeds = new Set(['low', 'full', 'high']);
// Fixed reasons only (no free text crosses this channel).
export const usbRejectReasons = Object.freeze(['usb_class_denied', 'usb_hid_boot_denied', 'usb_isochronous_denied', 'usb_descriptor_invalid', 'usb_class_not_granted', 'usb_vhci_unavailable', 'usb_limit_reached', 'usb_attach_failed', 'usb_busy']);
const rejectReasons = new Set(usbRejectReasons);
const detachReasons = new Set(['usb_user_stopped', 'usb_device_removed', 'usb_vm_detached', 'usb_policy_changed', 'usb_device_error', 'usb_session_closed']);
// Why the Host cannot attach devices at all (sent as usb.status).
export const usbStatusReasons = Object.freeze(['usb_driver_missing', 'usb_driver_install_failed', 'usb_vhci_unprotected', 'usb_vhci_unavailable', 'usb_setup_failed', 'usb_restart_required']);
const statusReasons = new Set(usbStatusReasons);
const protocolErrors = new Set(['usb_invalid_message', 'usb_out_of_order', 'usb_limits_exceeded', 'usb_busy']);
const exact = (value, keys) => Object.keys(value).length === keys.length && keys.every(key => Object.hasOwn(value, key));
function require(ok, reason = 'usb_invalid_message') { if (!ok) throw new Error(reason); }

// --- Descriptors and the device policy (shared test vectors with the Host and the client) ---

// Class codes whose interfaces the policy never redirects as raw USB.
const deniedClasses = new Map([[0x01, 'usb_isochronous_denied'], [0x09, 'usb_class_denied'], [0x0e, 'usb_isochronous_denied'], [0xe0, 'usb_class_denied']]);
// Class codes that also need another administrator permission.
export const usbClassPermissions = Object.freeze(new Map([[0x02, 'serialPorts'], [0x0a, 'serialPorts'], [0x07, 'printers'], [0x08, 'drives'], [0x0b, 'smartCards']]));

// Parses a device descriptor followed by every configuration descriptor in full.
// Throws on anything malformed; the result holds only what the policy needs.
export function parseUsbDescriptors(bytes) {
  require(bytes instanceof Uint8Array && bytes.length >= 18 + 9 && bytes.length <= usbLimits.descriptorBytes, 'usb_descriptor_invalid');
  require(bytes[0] === 18 && bytes[1] === 1, 'usb_descriptor_invalid');
  const device = { vendor: bytes[8] | bytes[9] << 8, product: bytes[10] | bytes[11] << 8, class: bytes[4], subclass: bytes[5], protocol: bytes[6], maxPacket0: bytes[7], configurations: [] };
  const count = bytes[17];
  require(count >= 1 && count <= 4 && [8, 16, 32, 64].includes(device.maxPacket0), 'usb_descriptor_invalid');
  let at = 18;
  for (let index = 0; index < count; ++index) {
    require(at + 9 <= bytes.length && bytes[at] === 9 && bytes[at + 1] === 2, 'usb_descriptor_invalid');
    const total = bytes[at + 2] | bytes[at + 3] << 8;
    require(total >= 9 && at + total <= bytes.length, 'usb_descriptor_invalid');
    const configuration = { value: bytes[at + 5], interfaces: [] };
    require(bytes[at + 4] >= 1 && configuration.value >= 1, 'usb_descriptor_invalid');
    let current = null;
    for (let cursor = at + 9; cursor < at + total;) {
      const length = bytes[cursor], type = bytes[cursor + 1];
      require(length >= 2 && cursor + length <= at + total, 'usb_descriptor_invalid');
      if (type === 4) {
        require(length === 9, 'usb_descriptor_invalid');
        current = { number: bytes[cursor + 2], alternate: bytes[cursor + 3], endpoints: [], declared: bytes[cursor + 4], class: bytes[cursor + 5], subclass: bytes[cursor + 6], protocol: bytes[cursor + 7] };
        configuration.interfaces.push(current);
      } else if (type === 5) {
        require(current && (length === 7 || length === 9), 'usb_descriptor_invalid');
        current.endpoints.push({ address: bytes[cursor + 2], type: bytes[cursor + 3] & 3, maxPacket: bytes[cursor + 4] | bytes[cursor + 5] << 8 });
      }
      cursor += length;
    }
    require(configuration.interfaces.length >= 1 && configuration.interfaces.every(item => item.endpoints.length === item.declared), 'usb_descriptor_invalid');
    device.configurations.push(configuration);
    at += total;
  }
  require(at === bytes.length, 'usb_descriptor_invalid');
  return device;
}

// The decision for one parsed device: { allowed } or { allowed: false, reason },
// plus `requires` (the other permissions its interfaces need) and `classes`.
export function usbDevicePolicy(device) {
  const requires = new Set(), classes = new Set();
  if (deniedClasses.has(device.class)) return { allowed: false, reason: deniedClasses.get(device.class), requires: [], classes: [device.class] };
  for (const configuration of device.configurations) for (const item of configuration.interfaces) {
    classes.add(item.class);
    if (item.endpoints.some(endpoint => endpoint.type === 1)) return { allowed: false, reason: 'usb_isochronous_denied', requires: [], classes: [...classes] };
    if (deniedClasses.has(item.class)) return { allowed: false, reason: deniedClasses.get(item.class), requires: [], classes: [...classes] };
    // Boot-protocol HID is a keyboard or mouse; the VM must keep using its own.
    if (item.class === 0x03 && item.subclass === 1) return { allowed: false, reason: 'usb_hid_boot_denied', requires: [], classes: [...classes] };
    if (usbClassPermissions.has(item.class)) requires.add(usbClassPermissions.get(item.class));
  }
  return { allowed: true, requires: [...requires].sort(), classes: [...classes].sort((a, b) => a - b) };
}

// Host capability: exactly the marker; anything else is omitted.
export function usbCapability(value) {
  return value?.usbProtocol === usbProtocol;
}

// Connect body field. Malformed requests are refused before any session.
export function requestedUsb(mode, protocol) {
  if (protocol === undefined) return false;
  if (mode !== 'stream' || protocol !== usbProtocol) throw new HttpError(400, 'invalid_usb_protocol');
  return true;
}

// Never a reason to refuse a connection: an incapable Host or a denied
// assignment leaves it ungranted. Independent outputs (tracks-v1) use USB too.
export function negotiateUsb(requested, host, redirectionPolicy) {
  return requested === true && usbCapability(host?.capabilities) && redirectionPolicy?.usbDevices === true;
}

function readHeader(data) {
  return {
    type: data[0], device: data[1], endpoint: data[2], transfer: data[3], seq: data.readUInt32LE(4), length: data.readUInt32LE(8), status: data.readInt32LE(12),
    setup: data.subarray(16, 24),
  };
}
const zeroSetup = setup => setup.every(byte => byte === 0);

// Relay only: no data is kept here. The relay is the bounds, state and
// permission check between the two peers.
export class UsbRelay {
  constructor(relay, { pairReadyMs = 15000, maxQueue = 4 * 1024 * 1024 } = {}) {
    Object.assign(this, { relay, pairReadyMs, maxQueue });
    this.pairs = new Set(); this.windowAt = Date.now(); this.windowBytes = 0;
    this.wss = new WebSocketServer({ noServer: true, maxPayload: maxFrame, perMessageDeflate: false, maxFragments: 128, closeTimeout: 1000 });
  }
  policy(session) { return this.relay.store.devicePolicy(session.userId, session.desktopId); }
  permitted(session) { return this.policy(session).usbDevices === true; }
  available(session, signinOwner = null) {
    return session.mode === 'stream' && session.usb === true && this.relay.current(session) && session.ws?.readyState === WebSocket.OPEN &&
      this.relay.authorized(session) && this.permitted(session) && (!session.signinPolicy || this.relay.signinRedirectAvailable?.(session, signinOwner) === true);
  }
  unavailableReason(session) {
    // channel-resume-v1: the stream waits for a side; the channel may be dialled again.
    if (this.relay.channels?.away(session)) return 'usb_stream_away';
    return !this.relay.current(session) || !this.relay.authorized(session) ? 'authorization_revoked' : !this.permitted(session) ? 'usb_policy_changed' : 'usb_unavailable';
  }
  upgrade(req, socket, head, url, reject) {
    const hostId = /^\/api\/v1\/hosts\/sessions\/([^/]+)\/usb$/.exec(url.pathname)?.[1];
    const clientId = /^\/api\/v1\/sessions\/([^/]+)\/usb$/.exec(url.pathname)?.[1];
    if (hostId === undefined && clientId === undefined) return false;
    const role = hostId === undefined ? 'client' : 'host', session = this.relay.sessions.get(hostId ?? clientId);
    // channel-resume-v1 (channel-resume.mjs): not now (503) while the stream is away, never again (409) after a final end.
    const refusal = session ? this.relay.channels?.refusal(session, 'usb') ?? 0 : 0;
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
    if (session.usbPair?.[role] && !session.usbPair.closed && this.relay.channels?.enabled(session)) this.closePair(session.usbPair, 'usb_transport_closed');
    if (session.usbPair?.[role] || session.usbPair?.closed) { reject(409, 'Conflict'); return true; }
    if (!session.usbPair && (this.pairs.size >= 128 || this.relay.channels?.admit(session, 'usb') === false)) { reject(503, 'Unavailable'); return true; }
    const observedPair = session.usbPair;
    const guard = () => !socket.destroyed && this.available(session) && session.usbPair === observedPair && !observedPair?.[role] && !observedPair?.closed && (observedPair || this.pairs.size < 128);
    const accept = () => this.wss.handleUpgrade(req, socket, head, ws => this.attach(ws, session, role));
    if (session.signinPolicy && this.relay.admitSigninRedirect) void this.relay.admitSigninRedirect(session, guard, accept, () => reject(401, 'Unauthorized'));
    else if (!session.signinPolicy && guard()) accept(); else reject(401, 'Unauthorized'); return true;
  }
  attach(ws, session, role) {
    if (!this.available(session)) { ws.close(1008, 'redirect_unavailable'); return; }
    let pair = session.usbPair;
    if (!pair) {
      pair = { session, signinOwner: this.relay.signinRedirectOwner?.(session) ?? null, createdAt: Date.now(), ready: false, closed: false, devices: new Map(), seen: new Set(), attaches: 0, flight: new Map(), unlinks: new Map(), flightBytes: 0, retired: new Set() };
      session.usbPair = pair; this.pairs.add(pair);
    }
    pair[role] = ws; pair[role + 'Alive'] = true;
    const rate = { at: Date.now(), messages: 0, bytes: 0 };
    ws.on('pong', () => { if (this.current(pair, role, ws)) pair[role + 'Alive'] = true; });
    ws.on('error', () => this.closePair(pair, 'usb_transport_closed'));
    ws.on('close', () => this.closePair(pair, 'usb_transport_closed'));
    ws.on('message', (data, binary) => {
      if (!this.current(pair, role, ws)) return;
      if (!this.available(session, pair.signinOwner)) return this.closePair(pair, this.unavailableReason(session));
      const now = Date.now();
      if (now - rate.at >= 1000) { rate.at = now; rate.messages = 0; rate.bytes = 0; }
      if (now - this.windowAt >= 1000) { this.windowAt = now; this.windowBytes = 0; }
      if (++rate.messages > 4096 || (rate.bytes += data.length) > 48 * 1024 * 1024 || (this.windowBytes += data.length) > 192 * 1024 * 1024) return this.closePair(pair, 'usb_rate_limit');
      if (!pair.ready) return this.closePair(pair, 'usb_invalid_message');
      try {
        if (binary) this.receiveFrame(pair, role, Buffer.isBuffer(data) ? data : Buffer.concat(data));
        else {
          require(data.length <= maxText);
          const value = JSON.parse(data.toString('utf8'));
          require(value && typeof value === 'object' && !Array.isArray(value) && typeof value.type === 'string');
          this.receive(pair, role, value);
        }
      } catch (error) { this.closePair(pair, protocolErrors.has(error.message) ? error.message : 'usb_invalid_message'); }
    });
    if (pair.client && pair.host) {
      pair.ready = true;
      const ready = { type: 'usb.ready', sessionId: session.id, protocol: usbProtocol, limits: usbLimits };
      this.send(pair, 'client', ready); this.send(pair, 'host', ready);
    }
  }
  current(pair, role, ws) { return !pair.closed && pair.session.usbPair === pair && pair[role] === ws; }
  sendRaw(pair, role, payload, binary) {
    if (pair.closed) return false;
    const ws = pair[role];
    if (ws?.readyState !== WebSocket.OPEN || ws.bufferedAmount + payload.length > this.maxQueue) { this.closePair(pair, 'usb_backpressure'); return false; }
    ws.send(payload, { binary }, error => { if (error) this.closePair(pair, 'usb_transport_closed'); }); return true;
  }
  send(pair, role, value) { return this.sendRaw(pair, role, JSON.stringify(value), false); }
  other(role) { return role === 'host' ? 'client' : 'host'; }

  // Drops a device and every request still in flight for it. Their returns may
  // still cross; the retired set makes those dropped instead of an error.
  removeDevice(pair, deviceId) {
    if (!pair.devices.delete(deviceId)) return false;
    pair.retired.add(deviceId);
    for (const [seq, entry] of pair.flight) if (entry.device === deviceId) { pair.flight.delete(seq); pair.flightBytes -= entry.length; }
    for (const [seq, entry] of pair.unlinks) if (entry.device === deviceId) pair.unlinks.delete(seq);
    return true;
  }
  receive(pair, role, value) {
    if (role === 'client') {
      if (value.type === 'usb.attach') {
        require(exact(value, ['type', 'device', 'descriptors', 'speed']) && Number.isInteger(value.device) && value.device >= 1 && value.device <= 255 &&
          typeof value.descriptors === 'string' && value.descriptors.length <= Math.ceil(usbLimits.descriptorBytes / 3) * 4 && speeds.has(value.speed));
        require(!pair.seen.has(value.device) && pair.attaches < usbLimits.maxAttaches);
        pair.seen.add(value.device); pair.attaches++;
        if (pair.devices.size >= usbLimits.maxDevices) { pair.retired.add(value.device); this.send(pair, 'client', { type: 'usb.rejected', device: value.device, reason: 'usb_limit_reached' }); return; }
        const decoded = Buffer.from(value.descriptors, 'base64');
        require(decoded.toString('base64') === value.descriptors);
        let reason = null, parsed = null;
        try { parsed = parseUsbDescriptors(new Uint8Array(decoded)); } catch { reason = 'usb_descriptor_invalid'; }
        if (parsed) {
          const decision = usbDevicePolicy(parsed);
          if (!decision.allowed) reason = decision.reason;
          else {
            const policy = this.policy(pair.session);
            if (decision.requires.some(key => policy[key] !== true)) reason = 'usb_class_not_granted';
            else pair.devices.set(value.device, { state: 'attaching', requires: decision.requires });
          }
        }
        if (reason) { pair.retired.add(value.device); this.send(pair, 'client', { type: 'usb.rejected', device: value.device, reason }); return; }
        this.send(pair, 'host', value); return;
      }
      if (value.type === 'usb.detach') {
        require(exact(value, ['type', 'device', 'reason']) && Number.isInteger(value.device) && detachReasons.has(value.reason));
        if (!pair.devices.has(value.device)) { require(pair.retired.has(value.device), 'usb_out_of_order'); return; }
        this.removeDevice(pair, value.device); this.send(pair, 'host', value); return;
      }
      throw new Error('usb_invalid_message');
    }
    if (value.type === 'usb.status') {
      require(value.state === 'ready' ? exact(value, ['type', 'state']) : value.state === 'unavailable' && exact(value, ['type', 'state', 'reason']) && statusReasons.has(value.reason));
      this.send(pair, 'client', value); return;
    }
    if (value.type === 'usb.attached') {
      require(exact(value, ['type', 'device']) && Number.isInteger(value.device));
      const device = pair.devices.get(value.device);
      require(device?.state === 'attaching', 'usb_out_of_order');
      device.state = 'attached'; this.send(pair, 'client', value); return;
    }
    if (value.type === 'usb.rejected') {
      require(exact(value, ['type', 'device', 'reason']) && Number.isInteger(value.device) && rejectReasons.has(value.reason));
      require(pair.devices.get(value.device)?.state === 'attaching', 'usb_out_of_order');
      this.removeDevice(pair, value.device); this.send(pair, 'client', value); return;
    }
    if (value.type === 'usb.detach') {
      require(exact(value, ['type', 'device', 'reason']) && Number.isInteger(value.device) && detachReasons.has(value.reason));
      if (!pair.devices.has(value.device)) { require(pair.retired.has(value.device), 'usb_out_of_order'); return; }
      this.removeDevice(pair, value.device); this.send(pair, 'client', value); return;
    }
    throw new Error('usb_invalid_message');
  }

  // Binary frames: Host submit/unlink toward the client, client returns toward
  // the Host. The bounds and the in-flight accounting live here.
  receiveFrame(pair, role, data) {
    require(data.length >= headerBytes && data.length <= maxFrame);
    const header = readHeader(data), payload = data.length - headerBytes;
    if (!pair.devices.has(header.device)) { require(pair.retired.has(header.device), 'usb_out_of_order'); return; }
    const device = pair.devices.get(header.device);
    require(device.state === 'attached', 'usb_out_of_order');
    if (role === 'host') {
      if (header.type === typeSubmit) {
        require(header.status === 0 && Number.isSafeInteger(header.seq) && !pair.flight.has(header.seq) && !pair.unlinks.has(header.seq), 'usb_out_of_order');
        require(header.length <= usbLimits.maxUrbBytes, 'usb_limits_exceeded');
        const input = (header.endpoint & 0x80) !== 0, number = header.endpoint & 15;
        require((header.endpoint & 0x70) === 0 && (header.transfer === transferControl ? number === 0 : (header.transfer === transferBulk || header.transfer === transferInterrupt) && number >= 1));
        if (header.transfer === transferControl) {
          // The setup packet's direction and length must agree with the request.
          require(((header.setup[0] & 0x80) !== 0) === input && (header.setup[6] | header.setup[7] << 8) === header.length);
        } else require(zeroSetup(header.setup));
        require(payload === (input ? 0 : header.length));
        require(pair.flight.size < usbLimits.maxInFlight && pair.flightBytes + header.length <= usbLimits.windowBytes, 'usb_limits_exceeded');
        pair.flight.set(header.seq, { device: header.device, endpoint: header.endpoint, transfer: header.transfer, length: header.length });
        pair.flightBytes += header.length;
        this.sendRaw(pair, 'client', data, true); return;
      }
      if (header.type === typeUnlink) {
        require(payload === 0 && header.status === 0 && header.endpoint === 0 && header.transfer === 0 && zeroSetup(header.setup) && !pair.unlinks.has(header.seq) && !pair.flight.has(header.seq) && pair.unlinks.size < usbLimits.maxInFlight, 'usb_out_of_order');
        // The target may have just returned: then the unlink is stale, not wrong,
        // but the client still has to answer it, so it is relayed as it is.
        pair.unlinks.set(header.seq, { device: header.device });
        this.sendRaw(pair, 'client', data, true); return;
      }
      throw new Error('usb_invalid_message');
    }
    if (header.type === typeReturn) {
      const entry = pair.flight.get(header.seq);
      require(entry && entry.device === header.device && entry.endpoint === header.endpoint && entry.transfer === header.transfer, 'usb_out_of_order');
      require(usbStatuses.includes(header.status) && zeroSetup(header.setup));
      const input = (entry.endpoint & 0x80) !== 0;
      require(header.length <= entry.length && payload === (input ? header.length : 0), 'usb_limits_exceeded');
      if (header.status !== 0) require(payload === 0 && header.length === 0);
      pair.flight.delete(header.seq); pair.flightBytes -= entry.length;
      this.sendRaw(pair, 'host', data, true); return;
    }
    if (header.type === typeUnlinkReturn) {
      const entry = pair.unlinks.get(header.seq);
      require(entry && entry.device === header.device, 'usb_out_of_order');
      require(payload === 0 && header.length === 0 && [0, -2, -104].includes(header.status) && zeroSetup(header.setup));
      pair.unlinks.delete(header.seq);
      this.sendRaw(pair, 'host', data, true); return;
    }
    throw new Error('usb_invalid_message');
  }
  closePair(pair, reason) {
    if (pair.closed) return;
    pair.closed = true; pair.devices.clear(); pair.flight.clear(); pair.unlinks.clear(); this.pairs.delete(pair);
    // channel-resume-v1: after a transport end the same session may pair again.
    if (this.relay.channels?.closed(pair.session, 'usb', reason) && pair.session.usbPair === pair) pair.session.usbPair = null;
    for (const role of ['client', 'host']) {
      const ws = pair[role]; if (!ws) continue;
      if (ws.readyState === WebSocket.OPEN) {
        const value = JSON.stringify({ type: 'usb.error', reason });
        if (ws.bufferedAmount + Buffer.byteLength(value) <= this.maxQueue) ws.send(value);
        ws.close(1008, reason.slice(0, 100));
      } else ws.terminate();
    }
  }
  closeSession(session, reason) { if (session.usbPair) this.closePair(session.usbPair, reason); this.relay.channels?.closed(session, 'usb', reason); }
  // The administrator changed this user's grants (or the sweep found one gone):
  // end USB where `usbDevices` is gone, and detach just the devices whose class
  // permission is gone. The sweep below repeats this within 250 ms.
  enforce(pair) {
    const session = pair.session;
    if (!this.permitted(session)) { this.closePair(pair, 'usb_policy_changed'); return; }
    const policy = this.policy(session);
    for (const [id, device] of [...pair.devices]) {
      if (device.requires.every(key => policy[key] === true)) continue;
      this.removeDevice(pair, id);
      const detach = { type: 'usb.detach', device: id, reason: 'usb_policy_changed' };
      this.send(pair, 'client', detach); this.send(pair, 'host', detach);
    }
  }
  policyChanged(userId, desktopId) {
    for (const pair of [...this.pairs]) {
      const session = pair.session;
      if (session.userId === userId && (desktopId === undefined || session.desktopId === desktopId)) this.enforce(pair);
    }
  }
  revalidate() {
    const now = Date.now();
    for (const pair of [...this.pairs]) {
      if (!this.available(pair.session, pair.signinOwner)) { this.closePair(pair, this.unavailableReason(pair.session)); continue; }
      if (!pair.ready && now - pair.createdAt > this.pairReadyMs) { this.closePair(pair, 'usb_pair_timeout'); continue; }
      this.enforce(pair);
    }
  }
  heartbeat() {
    for (const pair of this.pairs) for (const role of ['client', 'host']) {
      const ws = pair[role]; if (ws?.readyState !== WebSocket.OPEN) continue;
      if (pair[role + 'Alive'] === false) { this.closePair(pair, 'usb_transport_closed'); break; }
      pair[role + 'Alive'] = false; ws.ping();
    }
  }
  close() { for (const pair of this.pairs) this.closePair(pair, 'server_stopping'); this.wss.close(); }
}
