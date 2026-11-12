// Pure production pacing + actual write queue; memory transports/clock only.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { ConsoleReadBudget, consoleReadLimits } from '../src/console-read-budget.mjs';
import { ConsoleWriteQueue } from '../src/console-rfb.mjs';

class Clock {
  time = 0; next = 0; timers = new Map(); maxTimers = 0;
  now = () => this.time;
  set = (fn, ms) => {
    const timer = { id: ++this.next, at: this.time + ms, fn, unref() {} };
    this.timers.set(timer.id, timer); this.maxTimers = Math.max(this.maxTimers, this.timers.size); return timer;
  };
  clear = timer => this.timers.delete(timer.id);
  advance(ms) {
    const target = this.time + ms;
    for (let steps = 0; steps < 10000; steps++) {
      const due = [...this.timers.values()].filter(timer => timer.at <= target).sort((a, b) => a.at - b.at)[0];
      if (!due) { this.time = target; return; }
      this.time = due.at; this.timers.delete(due.id); due.fn();
    }
    throw new Error('memory clock did not settle');
  }
}
const caps = { rate: 64, burst: 16, bytes: 16, packets: 4, message: 16, chunk: 8 };
function fixture({ limits = caps, consume } = {}) {
  const clock = new Clock(), chunks = [], failures = [];
  const socket = { pauses: 0, resumes: 0, pause() { this.pauses++; }, resume() { this.resumes++; } };
  const budget = new ConsoleReadBudget(socket, bytes => { chunks.push(Buffer.from(bytes)); consume?.(bytes); }, reason => failures.push(reason),
    { now: clock.now, setTimer: clock.set, clearTimer: clock.clear, limits });
  return { clock, socket, budget, chunks, failures };
}

test('production caps retain 64MiB/s and 16MiB burst/message/queue without configurable expansion', () => {
  assert.deepEqual(consoleReadLimits, { rate: 67108864, burst: 16777216, bytes: 16777216, packets: 256, message: 16777216, chunk: 65536 });
  assert.throws(() => fixture({ limits: { ...caps, rate: consoleReadLimits.rate + 1 } }), RangeError);
});

test('credit exhaustion pauses only its upstream and resumes with ordered exact bytes and one timer', () => {
  const f = fixture(), first = Buffer.from(Array.from({ length: 16 }, (_, i) => i)), next = Buffer.from([16, 17, 18, 19, 20, 21, 22, 23]);
  f.budget.push(first); f.budget.push(next);
  assert.equal(f.socket.pauses, 1); assert.equal(f.socket.resumes, 0); assert.equal(f.clock.timers.size, 1);
  assert.equal(Buffer.concat(f.chunks).length, 16);
  next.fill(255); // Pacing owns exact bytes, not a caller's mutable view.
  f.clock.advance(250);
  assert.deepEqual(Buffer.concat(f.chunks), Buffer.from(Array.from({ length: 24 }, (_, i) => i)));
  assert.deepEqual(f.failures, []); assert.equal(f.socket.resumes, 1); assert.equal(f.clock.maxTimers, 1);
  f.budget.close(); assert.equal(f.clock.timers.size, 0);
});

test('sustained output above one-second credit waits instead of disconnecting or dropping bytes', () => {
  const f = fixture();
  for (let i = 0; i < 80; i++) {
    for (let step = 0; f.budget.paused && step < 1000; step++) f.clock.advance(16);
    assert.equal(f.budget.paused, false);
    f.budget.push(Buffer.from([i]));
  }
  f.clock.advance(32);
  assert.deepEqual(Buffer.concat(f.chunks), Buffer.from(Array.from({ length: 80 }, (_, i) => i)));
  assert.ok(f.clock.time >= 1000); assert.deepEqual(f.failures, []); assert.equal(f.budget.closed, false); f.budget.close();
});

test('a partly consumed large allocation remains fully charged to the queue byte bound', () => {
  const f = fixture(); f.budget.push(Buffer.alloc(16)); f.budget.push(Buffer.alloc(16));
  f.clock.advance(16); assert.equal(f.budget.bytes, 16);
  f.budget.push(Buffer.alloc(1)); assert.deepEqual(f.failures, ['console_limits_exceeded']);
  assert.equal(f.budget.closed, true); assert.equal(f.budget.bytes, 0); assert.equal(f.clock.timers.size, 0);
});

test('queued packet count is independently bounded before its byte allowance is full', () => {
  const f = fixture({ limits: { ...caps, packets: 2 } }); f.budget.push(Buffer.alloc(16));
  f.budget.push(Buffer.alloc(1)); f.budget.push(Buffer.alloc(1)); assert.equal(f.budget.bytes, 2);
  f.budget.push(Buffer.alloc(1)); assert.deepEqual(f.failures, ['console_limits_exceeded']); assert.equal(f.budget.closed, true);
});

test('an oversized single upstream message is rejected before copy or consumption', () => {
  const f = fixture(); f.budget.push(Buffer.alloc(17));
  assert.deepEqual(f.failures, ['console_limits_exceeded']); assert.equal(f.chunks.length, 0); assert.equal(f.clock.timers.size, 0);
});

test('synchronous consumer retirement cannot reuse cleared queue state or resume the socket', () => {
  let budget; const seen = [], failures = [], clock = new Clock(), socket = { pause() {}, resume() { assert.fail('retired upstream resumed'); } };
  budget = new ConsoleReadBudget(socket, bytes => { seen.push(Buffer.from(bytes)); budget.close(); }, reason => failures.push(reason),
    { now: clock.now, setTimer: clock.set, clearTimer: clock.clear, limits: caps });
  budget.push(Buffer.alloc(16, 7)); assert.equal(Buffer.concat(seen).length, 8); assert.equal(budget.bytes, 0); assert.deepEqual(failures, []);
  clock.advance(1000); assert.equal(seen.length, 1);
});

test('a queued late timer after close cannot deliver bytes or revive the exact upstream', () => {
  const f = fixture(); f.budget.push(Buffer.alloc(16)); f.budget.push(Buffer.alloc(8));
  const late = [...f.clock.timers.values()][0].fn, count = f.chunks.length;
  f.budget.close(); late(); f.clock.advance(1000); f.budget.push(Buffer.alloc(1));
  assert.equal(f.chunks.length, count); assert.equal(f.socket.resumes, 0); assert.equal(f.clock.timers.size, 0); assert.deepEqual(f.failures, []);
});

test('a backward clock cannot mint credit or replay a burst on an already-due timer', () => {
  const f = fixture(); f.budget.push(Buffer.alloc(16)); f.budget.push(Buffer.alloc(8));
  const timer = [...f.clock.timers.values()][0]; f.clock.timers.delete(timer.id); f.clock.time = -100; timer.fn();
  assert.equal(Buffer.concat(f.chunks).length, 16); assert.equal(f.budget.credit, 0);
  f.clock.advance(100); assert.equal(Buffer.concat(f.chunks).length, 16); f.budget.close();
});

test('one upstream credit lease cannot pause or resume another console transport', () => {
  const a = fixture(), b = fixture(); a.budget.push(Buffer.alloc(16)); a.budget.push(Buffer.alloc(8));
  b.budget.push(Buffer.from([4])); assert.equal(b.socket.pauses, 0); assert.equal(b.socket.resumes, 0);
  a.budget.close(); a.clock.advance(1000); assert.equal(a.socket.resumes, 0); assert.equal(b.budget.closed, false); b.budget.close();
});

test('a synchronous resume enqueue is retained and drained in order without a second timer', () => {
  const f = fixture(); let emitted = false;
  f.socket.resume = function () { this.resumes++; if (!emitted) { emitted = true; f.budget.push(Buffer.from([42])); } };
  f.budget.push(Buffer.alloc(16)); f.clock.advance(100);
  assert.equal(Buffer.concat(f.chunks).length, 17); assert.equal(Buffer.concat(f.chunks).at(-1), 42);
  assert.equal(f.clock.maxTimers, 1); assert.deepEqual(f.failures, []); f.budget.close();
});

test('a slow actual ConsoleWriteQueue retains its in-flight byte bound and retires paced input', async () => {
  const clock = new Clock(), sent = [], pending = [], failures = [];
  const upstream = { pause() {}, resume() { assert.fail('retired upstream resumed'); } };
  const downstream = { readyState: 1, send(bytes, _options, done) { sent.push(Buffer.from(bytes)); pending.push(done); } };
  let budget;
  const writer = new ConsoleWriteQueue(downstream, reason => { failures.push(reason); budget.close(); }, { limit: 8 });
  budget = new ConsoleReadBudget(upstream, bytes => writer.push(bytes), reason => failures.push(reason),
    { now: clock.now, setTimer: clock.set, clearTimer: clock.clear, limits: caps });
  budget.push(Buffer.alloc(16)); assert.deepEqual(failures, ['console_backpressure']); assert.equal(budget.closed, true);
  assert.equal(sent.length, 1); assert.equal(writer.bytes, 8); writer.close(); pending[0](); await Promise.resolve();
  assert.equal(sent.length, 1); assert.equal(clock.timers.size, 0);
});

function sinkFixture() {
  const clock = new Clock(), sent = [], pending = [], failures = [];
  const upstream = { pauses: 0, resumes: 0, pause() { this.pauses++; }, resume() { this.resumes++; } };
  const downstream = { readyState: 1, send(bytes, _options, done) { sent.push(Buffer.from(bytes)); pending.push(done); } };
  let budget, writer;
  const retire = reason => { failures.push(reason); budget.close(); writer.close(); };
  writer = new ConsoleWriteQueue(downstream, retire, { limit: 12, timeoutMs: 5000,
    setTimer: clock.set, clearTimer: clock.clear, onWritable: () => budget.drain() });
  budget = new ConsoleReadBudget(upstream, bytes => writer.push(bytes), retire,
    { now: clock.now, setTimer: clock.set, clearTimer: clock.clear,
      limits: { ...caps, chunk: 4 }, canConsume: () => writer.hasCapacity(8, 2) });
  return { clock, sent, pending, failures, upstream, writer, budget };
}

test('actual sink capacity pauses upstream without a polling timer and resumes only after successful write completion', async () => {
  const f = sinkFixture(), bytes = Buffer.from(Array.from({ length: 16 }, (_, i) => i));
  try {
    f.budget.push(bytes);
    assert.equal(f.budget.sinkBlocked, true); assert.equal(f.upstream.pauses, 1);
    assert.equal(f.writer.bytes, 8); assert.equal(f.budget.bytes, 16);
    assert.equal(f.clock.timers.size, 1, 'only the existing in-flight write timeout, no sink polling timer');
    f.clock.advance(100); assert.equal(f.sent.length, 1); assert.equal(f.upstream.resumes, 0);
    while (f.pending.length) { f.pending.shift()(); await Promise.resolve(); }
    assert.deepEqual(Buffer.concat(f.sent), bytes); assert.deepEqual(f.failures, []);
    assert.equal(f.writer.bytes, 0); assert.equal(f.budget.bytes, 0);
    assert.equal(f.budget.closed, false); assert.equal(f.upstream.resumes, 1);
    assert.equal(f.clock.timers.size, 0);
  } finally { f.budget.close(); f.writer.close(); }
});

test('a sink that never completes retains the existing five-second bound and late completion cannot resume a retired reader', async () => {
  const f = sinkFixture(); f.budget.push(Buffer.alloc(16, 7));
  const late = f.pending[0];
  f.clock.advance(4999); assert.deepEqual(f.failures, []); assert.equal(f.budget.sinkBlocked, true);
  f.clock.advance(1);
  assert.deepEqual(f.failures, ['console_backpressure']); assert.equal(f.budget.closed, true);
  assert.equal(f.writer.closed, true); assert.equal(f.writer.bytes, 0); assert.equal(f.budget.bytes, 0);
  const count = f.sent.length; late(); await Promise.resolve(); f.clock.advance(5000);
  assert.equal(f.sent.length, count); assert.equal(f.upstream.resumes, 0); assert.equal(f.clock.timers.size, 0);
});

test('revocation while sink-blocked clears both queues and a queued successful callback cannot reopen its upstream', async () => {
  const f = sinkFixture(); f.budget.push(Buffer.alloc(16, 9));
  f.budget.close(); f.writer.close(); const count = f.sent.length;
  for (const done of f.pending) done(); await Promise.resolve(); f.clock.advance(5000);
  assert.deepEqual(f.failures, []); assert.equal(f.sent.length, count); assert.equal(f.upstream.resumes, 0);
  assert.equal(f.budget.bytes, 0); assert.equal(f.writer.bytes, 0); assert.equal(f.clock.timers.size, 0);
});
