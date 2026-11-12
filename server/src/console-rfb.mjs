import { createCipheriv } from 'node:crypto';

const MAX_BYTES = 16 * 1024 * 1024, CHUNK = 64 * 1024;
const FORMAT = Buffer.from([32, 24, 0, 1, 0, 255, 0, 255, 0, 255, 16, 8, 0, 0, 0, 0]);
// Official noVNC 1.7.0 requests RGBX; the released native client uses BGRX.
const NOVNC_FORMAT = Buffer.from([32, 24, 0, 1, 0, 255, 0, 255, 0, 255, 0, 8, 16, 0, 0, 0]);
const BANNER = Buffer.from('RFB 003.008\n');
const NAME = Buffer.from('Multitap Emergency Console');
export class ConsoleProtocolError extends Error {
  constructor(code = 'console_protocol_error') { super(code); this.code = code; }
}
const fail = code => { throw new ConsoleProtocolError(code); };
function geometry(w, h) { if (!w || !h || w > 8192 || h > 8192 || w * h > 16777216) fail(); }
function reverse(value) { let result = 0; for (let i = 0; i < 8; i++) { result = (result << 1) | (value & 1); value >>>= 1; } return result; }
// EDE(K,K,K) is single DES, without requesting OpenSSL's legacy provider.
// There is no insecure provider/FIPS/security fallback if this cipher is absent.
export function vncResponse(password, challenge) {
  if (typeof password !== 'string' || !password.length || password.length > 512 || challenge.length !== 16) fail('console_auth_failed');
  const key = Buffer.alloc(24);
  for (let i = 0; i < 8; i++) key[i] = reverse(i < password.length ? password.charCodeAt(i) & 255 : 0);
  key.copy(key, 8, 0, 8); key.copy(key, 16, 0, 8);
  try {
    const cipher = createCipheriv('des-ede3', key, null); cipher.setAutoPadding(false);
    return Buffer.concat([cipher.update(challenge), cipher.final()]);
  } catch { fail('console_crypto_unavailable'); }
  finally { key.fill(0); }
}

// Pure bounded byte machine. It owns no socket, timer, credentials or OS state.
// Callbacks may enqueue bounded writes, never block or invoke a second parser.
export class ConsoleRfb {
  constructor({ password, upstream, downstream, ready, qemuKeyboard = false, profile = 'native', clipboardPolicy, acceptedInput } = {}) {
    if (!['native', 'novnc'].includes(profile)) fail('console_rfb_unsupported');
    this.novnc = profile === 'novnc'; this.format = this.novnc ? NOVNC_FORMAT : FORMAT;
    this.clipboardToRemote = this.novnc && clipboardPolicy?.textToRemote === true;
    this.clipboardToLocal = this.novnc && clipboardPolicy?.textToLocal === true;
    this.password = password; this.up = upstream; this.down = downstream; this.ready = ready;
    this.acceptedInput = acceptedInput;
    this.u = Buffer.alloc(0); this.d = Buffer.alloc(0); this.us = 'version'; this.ds = 'waiting';
    this.width = 0; this.height = 0; this.rects = 0; this.raw = 0; this.discard = 0;
    this.clientReady = false; this.closed = false; this.rect = false;
    this.qemuKeyboard = qemuKeyboard === true;
    this.qemuRequested = false; this.ledRequested = false; this.qemuAcknowledged = false; this.ledAcknowledged = false;
  }
  close() { this.closed = true; this.password = null; this.u = this.d = Buffer.alloc(0); }
  append(old, data) {
    if (!Buffer.isBuffer(data)) data = Buffer.from(data);
    if (data.length > MAX_BYTES || old.length + data.length > MAX_BYTES) fail('console_limits_exceeded');
    return old.length ? Buffer.concat([old, data]) : data;
  }
  take(side, count) { const value = this[side].subarray(0, count); this[side] = this[side].subarray(count); return value; }
  send(fn, bytes) { for (let i = 0; i < bytes.length; i += CHUNK) fn(bytes.subarray(i, Math.min(bytes.length, i + CHUNK))); }
  upstream(data) { if (this.closed) return; this.u = this.append(this.u, data); this.readUpstream(); }
  downstream(data) { if (this.closed) return; this.d = this.append(this.d, data); this.readDownstream(); }
  readUpstream() {
    while (!this.closed) {
      if (this.us === 'version') {
        if (this.u.length < 12) return;
        if (!this.take('u', 12).equals(BANNER)) fail('console_rfb_unsupported');
        this.up(BANNER); this.us = 'types';
      } else if (this.us === 'types') {
        if (!this.u.length) return;
        const count = this.u[0]; if (!count || count > 32) fail('console_auth_failed');
        if (this.u.length < count + 1) return;
        const types = this.take('u', count + 1).subarray(1);
        const type = types.includes(2) ? 2 : types.includes(1) ? 1 : 0;
        if (!type) fail('console_rfb_unsupported');
        this.up(Buffer.from([type])); this.us = type === 2 ? 'challenge' : 'result';
      } else if (this.us === 'challenge') {
        if (this.u.length < 16) return;
        this.up(vncResponse(this.password, this.take('u', 16))); this.password = null; this.us = 'result';
      } else if (this.us === 'result') {
        if (this.u.length < 4) return;
        if (this.take('u', 4).readUInt32BE() !== 0) fail('console_auth_failed');
        this.password = null; this.up(Buffer.from([1])); this.us = 'init';
      } else if (this.us === 'init') {
        if (this.u.length < 24) return;
        const length = this.u.readUInt32BE(20); if (length > 4096) fail();
        if (this.u.length < 24 + length) return;
        const init = this.take('u', 24 + length);
        this.width = init.readUInt16BE(0); this.height = init.readUInt16BE(2); geometry(this.width, this.height);
        // Always request 32-bit true-colour little-endian before any update.
        this.up(Buffer.concat([Buffer.from([0, 0, 0, 0]), this.format]));
        this.init = Buffer.alloc(24 + NAME.length);
        this.init.writeUInt16BE(this.width, 0); this.init.writeUInt16BE(this.height, 2);
        this.format.copy(this.init, 4); this.init.writeUInt32BE(NAME.length, 20); NAME.copy(this.init, 24);
        this.us = 'messages'; this.ds = 'version'; this.ready(); this.down(BANNER);
      } else {
        if (!this.clientReady) return;
        if (this.raw || this.discard) {
          const remaining = this.raw || this.discard;
          if (!this.u.length) return;
          const length = Math.min(remaining, this.u.length, CHUNK), bytes = this.take('u', length);
          if (this.raw) { this.raw -= length; this.down(bytes); } else this.discard -= length;
          continue;
        }
        if (this.rect) {
          if (this.u.length < 12) return;
          const header = this.u.subarray(0, 12), x = header.readUInt16BE(0), y = header.readUInt16BE(2);
          const w = header.readUInt16BE(4), h = header.readUInt16BE(6), encoding = header.readInt32BE(8);
          // QEMU v10.0.0 ui/vnc.c: -258 ACK has no body; -261 has one
          // actual LED byte. Neither pseudo-rectangle contains picture pixels.
          if (encoding === -258) {
            if (!this.qemuRequested || x || y || w !== this.width || h !== this.height) fail('console_rfb_unsupported');
            this.qemuAcknowledged = true;
          } else if (encoding === -261) {
            if (!this.ledRequested || x || y || w !== 1 || h !== 1) fail('console_rfb_unsupported');
            if (this.u.length < 13) return;
            if (this.u[12] > 7) fail();
            this.ledAcknowledged = true;
            this.down(this.take('u', 13)); this.rect = --this.rects > 0; continue;
          } else if (encoding === -223) { geometry(w, h); if (x || y) fail(); this.width = w; this.height = h; }
          else if (encoding === -239) { if (w > 256 || h > 256 || (w && x >= w) || (h && y >= h)) fail(); }
          else if (encoding !== 0 && encoding !== 1) fail('console_rfb_unsupported');
          else if (!w || !h || x + w > this.width || y + h > this.height) fail();
          if (encoding === 1) {
            if (this.u.length < 16) return;
            if (this.u.readUInt16BE(12) + w > this.width || this.u.readUInt16BE(14) + h > this.height) fail();
          }
          this.down(this.take('u', 12));
          this.raw = encoding === 0 ? w * h * 4 : encoding === 1 ? 4 : encoding === -239 ? w * h * 4 + Math.ceil(w / 8) * h : 0;
          this.rect = --this.rects > 0;
          continue;
        }
        if (!this.u.length) return;
        const type = this.u[0];
        if (type === 0) {
          if (this.u.length < 4) return;
          const count = this.u.readUInt16BE(2); if (count > 4096) fail('console_limits_exceeded');
          this.rects = count; this.rect = count > 0; this.down(this.take('u', 4));
        } else if (type === 2) this.down(this.take('u', 1));
        else if (type === 3) {
          if (this.u.length < 8) return;
          const length = this.u.readUInt32BE(4); if (length > 65536) fail('console_limits_exceeded');
          const header = this.take('u', 8);
          if (this.clipboardToLocal) { this.down(header); this.raw = length; }
          else this.discard = length; // Native and ungranted browser clipboard stay denied.
        } else fail('console_rfb_unsupported');
      }
    }
  }
  readDownstream() {
    while (!this.closed) {
      if (this.ds === 'waiting') { if (this.d.length) fail(); return; }
      if (this.ds === 'version') {
        if (this.d.length < 12) return;
        if (!this.take('d', 12).equals(BANNER)) fail('console_rfb_unsupported');
        this.down(Buffer.from([1, 1])); this.ds = 'select';
      } else if (this.ds === 'select') {
        if (!this.d.length) return; if (this.take('d', 1)[0] !== 1) fail();
        this.down(Buffer.alloc(4)); this.ds = 'init';
      } else if (this.ds === 'init') {
        if (!this.d.length) return; if (this.take('d', 1)[0] > 1) fail();
        this.down(this.init); this.init = null; this.clientReady = true; this.ds = 'messages'; this.readUpstream();
      } else {
        if (!this.d.length) return;
        const type = this.d[0]; let count, replacement;
        if (type === 0) count = 20;
        else if (type === 2) {
          if (this.d.length < 4) return;
          const encodings = this.d.readUInt16BE(2); if (encodings > (this.novnc ? 64 : 8)) fail('console_limits_exceeded'); count = 4 + encodings * 4;
          if (this.d.length < count) return;
          const allowed = this.qemuKeyboard ? [0, 1, -223, -239, -258, -261] : [0, 1, -223, -239];
          const requested = [];
          for (let i = 0; i < encodings; i++) {
            const encoding = this.d.readInt32BE(4 + i * 4);
            if (!allowed.includes(encoding)) { if (!this.novnc) fail('console_rfb_unsupported'); continue; }
            if (!this.novnc || !requested.includes(encoding)) requested.push(encoding);
          }
          if (this.novnc) {
            // Raw is RFB's mandatory fallback; unsupported compressions and
            // extensions never reach QEMU just because noVNC advertises them.
            if (!requested.includes(0)) requested.push(0);
            replacement = Buffer.alloc(4 + requested.length * 4); replacement[0] = 2; replacement.writeUInt16BE(requested.length, 2);
            requested.forEach((encoding, i) => replacement.writeInt32BE(encoding, 4 + i * 4));
          }
          this.qemuRequested = requested.includes(-258);
          this.ledRequested = requested.includes(-261);
          this.qemuAcknowledged = false;
          this.ledAcknowledged = false;
        } else if (type === 3) count = 10;
        else if (type === 4) count = 8;
        else if (type === 5) count = 6;
        else if (type === 255 && this.qemuRequested && this.qemuAcknowledged && (this.novnc || (this.ledRequested && this.ledAcknowledged))) count = 12;
        else if (type === 6 && this.clipboardToRemote) {
          if (this.d.length < 8) return;
          const length = this.d.readUInt32BE(4); if (length > 65536) fail('console_limits_exceeded'); count = 8 + length;
        }
        else fail(type === 6 ? 'console_clipboard_denied' : 'console_rfb_unsupported');
        if (this.d.length < count) return;
        const message = this.take('d', count);
        if (type === 0 && !message.subarray(4).equals(this.format)) fail('console_rfb_unsupported');
        if (type === 3 && (message[1] > 1 || message.readUInt16BE(2) + message.readUInt16BE(6) > this.width || message.readUInt16BE(4) + message.readUInt16BE(8) > this.height)) fail();
        if (type === 4 && message[1] > 1) fail();
        if (type === 5 && (message[1] > 127 || message.readUInt16BE(2) >= this.width || message.readUInt16BE(4) >= this.height)) fail();
        if (type === 255) {
          const keycode = message.readUInt32BE(8);
          if (message[1] !== 0 || message.readUInt16BE(2) > 1 ||
              keycode < 1 || keycode > 0xfe || keycode === 0x7f || keycode === 0x80) fail();
        }
        if ([4,5,255].includes(type) && this.acceptedInput?.() === false) fail('authorization_revoked');
        this.up(replacement ?? message);
      }
    }
  }
}

// One actual native send at a time; queued bytes are bounded and dropped only
// when the complete connection is retired. Raw RFB bytes are never skipped.
export class ConsoleWriteQueue {
  constructor(socket, failConnection, { limit = MAX_BYTES, timeoutMs = 5000, now = Date.now,
    onWritable = () => {}, setTimer = setTimeout, clearTimer = clearTimeout } = {}) {
    this.socket = socket; this.fail = failConnection; this.limit = limit; this.timeoutMs = timeoutMs; this.now = now;
    this.onWritable = onWritable; this.setTimer = setTimer; this.clearTimer = clearTimer;
    this.parts = []; this.bytes = 0; this.sending = false; this.closed = false;
  }
  hasCapacity(bytes, parts = 1) {
    return !this.closed && Number.isSafeInteger(bytes) && bytes >= 0 &&
      Number.isSafeInteger(parts) && parts > 0 && this.bytes + bytes <= this.limit && this.parts.length + parts <= 1024;
  }
  cancelTimer() {
    if (this.timer !== undefined) { this.clearTimer(this.timer); this.timer = undefined; }
  }
  push(bytes, binary = true) {
    if (this.closed) return;
    const size = Buffer.byteLength(bytes);
    // RFB is a byte stream: a valid update can contain4096 tiny rectangles,
    // each emitted as a header and pixels. Bound owned bytes and queued chunks,
    // rather than refusing that update solely for its parser callback count.
    // Only merge unsent binary bytes; an in-flight send or a text control
    // message must retain its original allocation, framing and ordering.
    const tail = this.parts.at(-1);
    const merge = binary && tail?.binary && typeof bytes !== 'string' &&
      typeof tail.bytes !== 'string' && tail.size + size <= CHUNK;
    if (size > this.limit || this.bytes + size > this.limit || (!merge && this.parts.length >= 1024)) { this.fail('console_backpressure'); return; }
    // Own exact bytes: a one-byte subarray must not retain a16MiB receive
    // buffer without being charged to this queue's byte bound.
    const owned = typeof bytes === 'string' ? bytes : Buffer.from(bytes);
    if (merge) { tail.bytes = Buffer.concat([tail.bytes, owned]); tail.size += size; }
    else this.parts.push({ bytes: owned, binary, size });
    this.bytes += size; this.pump();
  }
  pump() {
    if (this.closed || this.sending || !this.parts.length) return;
    if (this.socket.readyState !== 1) { this.fail('console_transport_closed'); return; }
    const part = this.parts.shift(); this.sending = true;
    this.timer = this.setTimer(() => this.fail('console_backpressure'), this.timeoutMs); this.timer.unref?.();
    try { this.socket.send(part.bytes, { binary: part.binary }, error => {
      this.cancelTimer(); if (this.closed) return;
      this.sending = false; this.bytes -= part.size;
      if (error) { this.fail('console_transport_closed'); return; }
      queueMicrotask(() => {
        if (this.closed) return;
        // The owner may revalidate/revoke before any queued bytes are sent.
        try { this.onWritable(); } catch { this.fail('console_transport_closed'); }
        if (!this.closed) this.pump();
      });
    }); } catch { this.cancelTimer(); this.fail('console_transport_closed'); }
  }
  close() { this.closed = true; this.cancelTimer(); this.parts = []; this.bytes = 0; }
}
