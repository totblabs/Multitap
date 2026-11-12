import { test } from 'node:test';
import assert from 'node:assert/strict';
import { ConsoleRfb, ConsoleWriteQueue, vncResponse } from '../src/console-rfb.mjs';

const banner = Buffer.from('RFB 003.008\n');
const format = Buffer.from([32,24,0,1,0,255,0,255,0,255,16,8,0,0,0,0]);
function init(w = 4, h = 3) {
  const bytes = Buffer.alloc(24); bytes.writeUInt16BE(w, 0); bytes.writeUInt16BE(h, 2); format.copy(bytes, 4); return bytes;
}
function ready({ fragmented = false, qemuKeyboard = false } = {}) {
  const up = [], down = [], notifications = [];
  const rfb = new ConsoleRfb({ password: 'server-only-password', qemuKeyboard, upstream: b => up.push(Buffer.from(b)), downstream: b => down.push(Buffer.from(b)), ready: () => notifications.push('ready') });
  const feed = (side, bytes) => { if (fragmented) for (const b of bytes) rfb[side](Buffer.from([b])); else rfb[side](bytes); };
  feed('upstream', Buffer.concat([banner, Buffer.from([1, 1]), Buffer.alloc(4), init()]));
  assert.equal(rfb.clientReady, false); assert.deepEqual(notifications, ['ready']);
  feed('downstream', Buffer.concat([banner, Buffer.from([1, 1])]));
  assert.equal(rfb.clientReady, true);
  return { rfb, up, down, feed, start: down.length };
}
function rect(encoding, width = 4, height = 3) {
  const bytes = Buffer.alloc(16); bytes[0] = 0; bytes.writeUInt16BE(1, 2);
  bytes.writeUInt16BE(width, 8); bytes.writeUInt16BE(height, 10); bytes.writeInt32BE(encoding, 12); return bytes;
}
function encodings(values = [0, 1, -223, -239, -258, -261]) {
  const bytes = Buffer.alloc(4 + values.length * 4); bytes[0] = 2; bytes.writeUInt16BE(values.length, 2);
  values.forEach((value, i) => bytes.writeInt32BE(value, 4 + i * 4)); return bytes;
}
function extendedKey(keycode, down = true, keysym = 0x61) {
  const bytes = Buffer.alloc(12); bytes[0] = 255; bytes.writeUInt16BE(down ? 1 : 0, 2);
  bytes.writeUInt32BE(keysym, 4); bytes.writeUInt32BE(keycode, 8); return bytes;
}
for (const fragmented of [false, true]) {
  test(`QEMU keyboard uses actual negotiated ACK and bounded LED framing (${fragmented ? 'bytewise' : 'coalesced'})`, () => {
    const f = ready({ fragmented, qemuKeyboard: true });
    f.feed('downstream', encodings());
    assert.deepEqual(f.up.at(-1), encodings());
    const ack = rect(-258), led = Buffer.concat([rect(-261, 1, 1), Buffer.from([4])]);
    f.feed('upstream', Buffer.concat([ack, led, rect(0), Buffer.alloc(4 * 3 * 4, 7)]));
    assert.deepEqual(Buffer.concat(f.down.slice(f.start)), Buffer.concat([ack, led, rect(0), Buffer.alloc(4 * 3 * 4, 7)]));
    assert.equal(f.rfb.qemuAcknowledged, true);
    for (const [code, symbol] of [[0x2a, 0xffe1], [0x1e, 0x61], [0xb8, 0xffea], [0x72, 0xff31], [0x71, 0xff34]]) {
      for (const down of [true, false]) {
        const bytes = extendedKey(code, down, symbol); f.feed('downstream', bytes); assert.deepEqual(f.up.at(-1), bytes);
      }
    }
    f.rfb.close();
  });
}
test('QEMU keyboard is denied on legacy, missing request, missing ACK or invalid subtype/keycode', () => {
  for (const setup of [f => {}, f => f.rfb.downstream(encodings()), f => {
    f.rfb.downstream(encodings()); f.rfb.upstream(rect(-258));
  }]) {
    const f = ready({ qemuKeyboard: true }); setup(f);
    const before = f.up.length; assert.throws(() => f.rfb.downstream(extendedKey(0x1e))); assert.equal(f.up.length, before); f.rfb.close();
  }
  const legacy = ready(); assert.throws(() => legacy.rfb.downstream(encodings())); legacy.rfb.close();
  for (const mutate of [b => { b[1] = 1; }, b => b.writeUInt16BE(2, 2), ...[0, 0x7f, 0x80, 0xff, 0x10000].map(n => b => b.writeUInt32BE(n, 8))]) {
    const f = ready({ qemuKeyboard: true }); f.rfb.downstream(encodings());
    f.rfb.upstream(Buffer.concat([rect(-258), rect(-261, 1, 1), Buffer.from([0])]));
    const bytes = extendedKey(0x1e); mutate(bytes); const before = f.up.length;
    assert.throws(() => f.rfb.downstream(bytes)); assert.equal(f.up.length, before); f.rfb.close();
  }
});
test('Unrequested QEMU pseudo-rectangles, invalid geometry and LED bits fail before payload forwarding', () => {
  for (const bytes of [rect(-258), Buffer.concat([rect(-261, 1, 1), Buffer.from([0])])]) {
    const f = ready({ qemuKeyboard: true }); assert.throws(() => f.rfb.upstream(bytes)); f.rfb.close();
  }
  for (const bytes of [rect(-258, 1, 1), Buffer.concat([rect(-261, 2, 1), Buffer.from([0])]), Buffer.concat([rect(-261, 1, 1), Buffer.from([8])])]) {
    const f = ready({ qemuKeyboard: true }); f.rfb.downstream(encodings()); assert.throws(() => f.rfb.upstream(bytes)); f.rfb.close();
  }
});
for (const fragmented of [false, true]) {
  test(`Actual RFB machine handles ${fragmented ? 'bytewise' : 'coalesced'} handshake and Raw/CopyRect without leaking server password`, () => {
    const f = ready({ fragmented });
    assert.ok(Buffer.concat(f.down).includes(banner));
    assert.equal(Buffer.concat(f.down).includes(Buffer.from('server-only-password')), false);
    const pixels = Buffer.alloc(4 * 3 * 4, 0x43);
    f.feed('upstream', Buffer.concat([rect(0), pixels, rect(1), Buffer.alloc(4)]));
    assert.deepEqual(Buffer.concat(f.down.slice(f.start)), Buffer.concat([rect(0), pixels, rect(1), Buffer.alloc(4)]));
    f.rfb.close();
  });
}
test('VNC auth uses reversed password bits and known DES vector, with server response never sent downstream', () => {
  const reverse = value => { let n=0; for(let i=0;i<8;i++){n=(n<<1)|(value&1);value>>>=1;} return n; };
  const key = Buffer.from('133457799bbcdff1', 'hex');
  const password = [...key].map(value => String.fromCharCode(reverse(value))).join('');
  const challenge = Buffer.from('0123456789abcdef0123456789abcdef', 'hex');
  assert.equal(vncResponse(password, challenge).toString('hex'), '85e813540f0ab40585e813540f0ab405');
  const up=[],down=[]; let readyCount=0;
  const rfb = new ConsoleRfb({ password, upstream:b=>up.push(Buffer.from(b)), downstream:b=>down.push(Buffer.from(b)),ready:()=>++readyCount });
  rfb.upstream(Buffer.concat([banner,Buffer.from([1,2]),challenge]));
  assert.equal(up[2].toString('hex'),'85e813540f0ab40585e813540f0ab405');
  assert.equal(down.length,0); assert.equal(readyCount,0);
  rfb.upstream(Buffer.concat([Buffer.alloc(4),init()]));
  assert.equal(readyCount,1); assert.equal(Buffer.concat(down).includes(up[2]),false); rfb.close();
});
test('An upstream auth failure or unsupported security is never fabricated into a None success', () => {
  for(const bytes of [Buffer.concat([banner,Buffer.from([1,9])]),Buffer.concat([banner,Buffer.from([1,1,0,0,0,1])])]){
    let readyCount=0;const down=[];
    const rfb=new ConsoleRfb({password:'secret',upstream:()=>{},downstream:b=>down.push(b),ready:()=>++readyCount});
    assert.throws(()=>rfb.upstream(bytes));assert.equal(readyCount,0);assert.equal(down.length,0);rfb.close();
  }
});
test('Clipboard output is dropped across fragments while the following Bell is preserved', () => {
  const f=ready({fragmented:true}), text=Buffer.from('PRIVATE_CLIPBOARD_SENTINEL');
  const header=Buffer.alloc(8);header[0]=3;header.writeUInt32BE(text.length,4);
  f.feed('upstream',Buffer.concat([header,text,Buffer.from([2])]));
  assert.deepEqual(Buffer.concat(f.down.slice(f.start)),Buffer.from([2]));f.rfb.close();
});
test('Clipboard input, extended encodings and changed pixel formats are refused before forwarding', () => {
  for(const bytes of [Buffer.from([6]),Buffer.from([2,0,0,1,0,0,0,16]),Buffer.alloc(20)]){
    const f=ready();const before=f.up.length;assert.throws(()=>f.rfb.downstream(bytes));assert.equal(f.up.length,before);f.rfb.close();
  }
});
test('Framebuffer, cursor, coordinate and byte bounds fail closed without allocating a claimed frame', () => {
  const cases=[rect(0,8192,8192),rect(-239,257,1),rect(16),Buffer.from([0,0,0x10,1])];
  for(const bytes of cases){const f=ready();assert.throws(()=>f.rfb.upstream(bytes));f.rfb.close();}
  const f=ready();assert.throws(()=>f.rfb.upstream(Buffer.alloc(16*1024*1024+1)));f.rfb.close();
});
test('Raw bytes remain incrementally forwarded in at most64KiB pieces', () => {
  const up=[],down=[];const rfb=new ConsoleRfb({password:'x',upstream:b=>up.push(b),downstream:b=>down.push(Buffer.from(b)),ready:()=>{}});
  rfb.upstream(Buffer.concat([banner,Buffer.from([1,1]),Buffer.alloc(4),init(512,512)]));rfb.downstream(Buffer.concat([banner,Buffer.from([1,1])]));
  const start=down.length;const pixels=Buffer.alloc(512*512*4,7);rfb.upstream(Buffer.concat([rect(0,512,512),pixels]));
  assert.ok(down.slice(start).every(bytes=>bytes.length<=65536));assert.equal(Buffer.concat(down.slice(start)).length,16+pixels.length);rfb.close();
});
test('Write queue retains the actual in-flight send in its bound and does not replay after close', async () => {
  const pending=[], sent=[], errors=[];
  const socket={readyState:1,send:(b,_o,done)=>{sent.push(b);pending.push(done);}};
  const queue=new ConsoleWriteQueue(socket,c=>errors.push(c),{limit:8});
  const original=Buffer.alloc(4);queue.push(original);original[0]=9;assert.equal(sent[0][0],0);
  queue.push(Buffer.alloc(4));assert.equal(sent.length,1);assert.equal(queue.bytes,8);
  queue.push(Buffer.alloc(1));assert.deepEqual(errors,['console_backpressure']);
  queue.close();pending[0]();await Promise.resolve();assert.equal(sent.length,1);assert.equal(queue.bytes,0);
});

test('Actual parser4096-rectangle update survives a pending write with bounded binary chunks and exact first-frame bytes', async () => {
  const f = ready(), pending = [], sent = [], errors = [];
  const socket = { readyState: 1, send: (bytes, options, done) => {
    sent.push({ bytes: Buffer.from(bytes), binary: options.binary }); pending.push(done);
  } };
  const queue = new ConsoleWriteQueue(socket, reason => { errors.push(reason); queue.close(); });
  f.rfb.down = bytes => queue.push(bytes);
  try {
    const header = Buffer.from([0, 0, 0x10, 0]);
    const one = Buffer.alloc(16); one.writeUInt16BE(1, 4); one.writeUInt16BE(1, 6);
    one.writeInt32BE(0, 8); one.writeUInt32BE(0x332211, 12);
    const frame = Buffer.concat([header, ...Array.from({ length: 4096 }, () => one)]);
    // The production read budget also splits incoming bytes at64KiB.
    for (let at = 0; at < frame.length; at += 65536) f.rfb.upstream(frame.subarray(at, at + 65536));
    assert.deepEqual(errors, []);
    assert.equal(queue.bytes, frame.length);
    assert.ok(queue.parts.length <= 2);
    assert.equal(sent.length, 1, 'the actual first send remains pending');
    while (pending.length) { pending.shift()(); await Promise.resolve(); }
    assert.deepEqual(Buffer.concat(sent.map(part => part.bytes)), frame);
    assert.ok(sent.every(part => part.binary && part.bytes.length <= 65536));
    assert.equal(queue.bytes, 0);
  } finally { queue.close(); f.rfb.close(); }
});

test('Binary tail coalescing owns caller bytes and preserves intervening text control message boundaries', async () => {
  const pending = [], sent = [], errors = [];
  const socket = { readyState: 1, send: (bytes, options, done) => {
    sent.push({ bytes: typeof bytes === 'string' ? bytes : Buffer.from(bytes), binary: options.binary }); pending.push(done);
  } };
  const queue = new ConsoleWriteQueue(socket, reason => errors.push(reason), { limit: 32 });
  try {
    queue.push(Buffer.from([1]));
    const a = Buffer.from([2]), b = Buffer.from([3]); queue.push(a); queue.push(b);
    a.fill(9); b.fill(9);
    queue.push('ready', false); queue.push(Buffer.from([4]));
    assert.equal(queue.bytes, 9);
    assert.equal(queue.parts.length, 3);
    while (pending.length) { pending.shift()(); await Promise.resolve(); }
    assert.deepEqual(errors, []);
    assert.deepEqual(sent, [
      { bytes: Buffer.from([1]), binary: true }, { bytes: Buffer.from([2, 3]), binary: true },
      { bytes: 'ready', binary: false }, { bytes: Buffer.from([4]), binary: true },
    ]);
    assert.equal(queue.bytes, 0);
  } finally { queue.close(); }
});
