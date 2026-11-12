// A bounded observer of one upstream ws, not a second RFB parser. Timer/clock
// injection keeps pacing and retirement executable in memory-only fixtures.
export const consoleReadLimits = Object.freeze({
  rate: 64 * 1024 * 1024,
  burst: 16 * 1024 * 1024,
  bytes: 16 * 1024 * 1024,
  packets: 256,
  message: 16 * 1024 * 1024,
  chunk: 64 * 1024,
});

export class ConsoleReadBudget {
  constructor(socket, consume, failConnection, {
    now = Date.now, setTimer = setTimeout, clearTimer = clearTimeout,
    limits = consoleReadLimits, canConsume = () => true,
  } = {}) {
    // Tests may lower caps; no caller may weaken the production hard bounds.
    for (const key of Object.keys(consoleReadLimits)) {
      if (!Number.isSafeInteger(limits[key]) || limits[key] < 1 || limits[key] > consoleReadLimits[key]) throw new RangeError('console read bound');
    }
    this.socket = socket; this.consume = consume; this.failConnection = failConnection;
    this.now = now; this.setTimer = setTimer; this.clearTimer = clearTimer;
    if (typeof canConsume !== 'function') throw new TypeError('console sink admission');
    this.canConsume = canConsume; this.sinkBlocked = false;
    this.limits = { ...limits }; this.credit = limits.burst; this.at = now();
    this.parts = []; this.bytes = 0; this.timer = null; this.epoch = 0;
    this.paused = false; this.closed = false; this.draining = false;
  }
  refill() {
    const now = this.now();
    // A backward wall clock must not mint a second burst or borrow future time.
    if (Number.isFinite(now) && now > this.at) {
      this.credit = Math.min(this.limits.burst, this.credit + (now - this.at) * this.limits.rate / 1000);
      this.at = now;
    }
  }
  fail(reason) {
    if (this.closed) return;
    this.close(); this.failConnection(reason);
  }
  push(data) {
    if (this.closed) return;
    if (!Buffer.isBuffer(data) && !(data instanceof Uint8Array)) { this.fail('console_protocol_error'); return; }
    if (!data.byteLength) return;
    if (data.byteLength > this.limits.message || this.bytes + data.byteLength > this.limits.bytes || this.parts.length >= this.limits.packets) {
      this.fail('console_limits_exceeded'); return;
    }
    // Charge the whole owned allocation until its last byte is consumed. A
    // partially consumed 16MiB buffer cannot masquerade as a one-byte tail.
    const bytes = Buffer.from(data);
    this.parts.push({ bytes, offset: 0 }); this.bytes += bytes.length;
    this.drain();
  }
  pause() {
    if (this.paused || this.closed) return;
    this.paused = true;
    try { this.socket.pause(); } catch { this.fail('console_transport_closed'); }
  }
  arm() {
    if (this.timer !== null || this.closed) return;
    const epoch = ++this.epoch;
    const delay = Math.max(1, Math.ceil((1 - Math.min(this.credit, 1)) * 1000 / this.limits.rate));
    this.timer = this.setTimer(() => {
      if (this.closed || epoch !== this.epoch) return;
      this.timer = null; this.drain();
    }, delay);
    this.timer?.unref?.();
  }
  drain() {
    if (this.closed || this.draining) return;
    this.draining = true; this.sinkBlocked = false;
    try {
      this.refill();
      let deliveries = 0;
      while (!this.closed && this.parts.length && this.credit >= 1 && deliveries++ < this.limits.packets) {
        // Credit is a rate bound, not evidence that the actual client socket
        // has drained. Its queue keeps an in-flight write timeout while this
        // reader waits for the explicit write-completion callback.
        if (!this.canConsume()) { this.sinkBlocked = true; this.pause(); return; }
        const part = this.parts[0];
        const length = Math.min(part.bytes.length - part.offset, Math.floor(this.credit), this.limits.chunk);
        const bytes = part.bytes.subarray(part.offset, part.offset + length);
        part.offset += length; this.credit -= length;
        if (part.offset === part.bytes.length) { this.parts.shift(); this.bytes -= part.bytes.length; }
        // Consume may synchronously retire the entire relay and clear parts.
        // Never reuse part/queue state after this callback without closed check.
        this.consume(bytes);
        if (this.closed) return;
      }
      if (this.closed) return;
      if (!this.canConsume()) { this.sinkBlocked = true; this.pause(); return; }
      if (this.parts.length || this.credit < 1) {
        this.pause(); if (!this.closed) this.arm();
      } else if (this.paused) {
        this.paused = false;
        try { this.socket.resume(); } catch { this.fail('console_transport_closed'); }
      }
    } catch {
      this.fail('console_transport_closed');
    } finally {
      this.draining = false;
      // A resume callback may synchronously enqueue a new packet in a fake or
      // adapter. Keep that work bounded and scheduled rather than losing it.
      if (!this.closed && !this.sinkBlocked && this.parts.length && this.timer === null) {
        this.pause(); if (!this.closed) this.arm();
      }
    }
  }
  close() {
    if (this.closed) return;
    this.closed = true; ++this.epoch;
    if (this.timer !== null) this.clearTimer(this.timer);
    this.timer = null; this.parts = []; this.bytes = 0;
    // The relay owns termination. Never resume a retired or revoked upstream.
  }
}
