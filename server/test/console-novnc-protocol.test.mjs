// Execute the pinned official noVNC serializers with memory-only sockets.
// No DOM, foreground window, network, guest or native input is used here.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import { ConsoleRfb } from '../src/console-rfb.mjs';
import { encodings } from '../admin/console/vendor/novnc/1.7.0/core/encodings.js';

const source = readFileSync(new URL('../admin/console/vendor/novnc/1.7.0/core/rfb.js', import.meta.url), 'utf8');
const banner = Buffer.from('RFB 003.008\n');
const nativeFormat = Buffer.from([32,24,0,1,0,255,0,255,0,255,16,8,0,0,0,0]);
const rgbx = Buffer.from([32,24,0,1,0,255,0,255,0,255,0,8,16,0,0,0]);
function official(h264 = false) {
  const RFB = {}, context = vm.createContext({ RFB, encodings, supportsWebCodecsH264Decode: h264 });
  const start = source.indexOf('RFB.messages = {'), end = source.indexOf('RFB.cursors = {', start);
  assert.ok(start > 0 && end > start);
  vm.runInContext(source.slice(start, end), context, { timeout: 1000 });
  const method = source.indexOf('    _sendEncodings() {'), next = source.indexOf('\n    /* RFB protocol initialization states:', method);
  assert.ok(method > 0 && next > method);
  const sendEncodings = vm.runInContext('({' + source.slice(method, next) + '})._sendEncodings', context, { timeout: 1000 });
  return { messages: RFB.messages, sendEncodings };
}
function socket() {
  let pending = [];
  const frames = [];
  return { frames,
    sQpush8(value) { pending.push(value & 255); },
    sQpush16(value) { pending.push((value >>> 8) & 255, value & 255); },
    sQpush32(value) { pending.push((value >>> 24) & 255, (value >>> 16) & 255, (value >>> 8) & 255, value & 255); },
    flush() { frames.push(Buffer.from(pending)); pending = []; },
  };
}
function init() {
  const bytes = Buffer.alloc(24); bytes.writeUInt16BE(2, 0); bytes.writeUInt16BE(2, 2); nativeFormat.copy(bytes, 4); return bytes;
}
function fixture({ profile = 'novnc', fragmented = false, clipboardPolicy } = {}) {
  const up = [], down = [];
  const rfb = new ConsoleRfb({ password: 'server-only', profile, clipboardPolicy, qemuKeyboard: true,
    upstream: b => up.push(Buffer.from(b)), downstream: b => down.push(Buffer.from(b)), ready() {} });
  const feed = (side, bytes) => { if (fragmented) for (const byte of bytes) rfb[side](Buffer.from([byte])); else rfb[side](bytes); };
  feed('upstream', Buffer.concat([banner, Buffer.from([1,1]), Buffer.alloc(4), init()]));
  feed('downstream', Buffer.concat([banner, Buffer.from([1,1])]));
  return { rfb, up, down, feed };
}
function update(encoding, width = 2, height = 2, x = 0, y = 0) {
  const bytes = Buffer.alloc(16); bytes.writeUInt16BE(1, 2); bytes.writeUInt16BE(x, 4); bytes.writeUInt16BE(y, 6);
  bytes.writeUInt16BE(width, 8); bytes.writeUInt16BE(height, 10); bytes.writeInt32BE(encoding, 12); return bytes;
}
function encodingMessage(values) {
  const bytes = Buffer.alloc(4 + values.length * 4); bytes[0] = 2; bytes.writeUInt16BE(values.length, 2);
  values.forEach((value, i) => bytes.writeInt32BE(value, 4 + 4 * i)); return bytes;
}
function encodingValues(bytes) { return Array.from({ length: bytes.readUInt16BE(2) }, (_, i) => bytes.readInt32BE(4 + i * 4)); }
function cutText(bytes, client = true) {
  const header = Buffer.alloc(8); header[0] = client ? 6 : 3; header.writeUInt32BE(bytes.length, 4); return Buffer.concat([header, bytes]);
}

for (const fragmented of [false, true]) test(`official noVNC RGBX and raw/cursor/copy/resize bytes stay exact (${fragmented ? 'bytewise' : 'coalesced'})`, () => {
  const f = fixture({ fragmented }), wire = socket(); official().messages.pixelFormat(wire, 24, true);
  assert.deepEqual(wire.frames[0].subarray(4), rgbx);
  assert.deepEqual(f.up.at(-1), wire.frames[0], 'QEMU is requested to produce RGBX directly');
  assert.deepEqual(f.down.at(-1).subarray(4,20), rgbx, 'browser ServerInit describes the same pixels');
  f.feed('downstream', wire.frames[0]);
  const pixels = Buffer.from([255,0,0,0, 0,255,0,0, 0,0,255,0, 255,255,255,0]);
  const cursor = Buffer.from([255,0,0,0, 0,255,0,0, 0xc0]);
  const copy = Buffer.from([0,1,0,1]);
  const bytes = Buffer.concat([update(0), pixels, update(-239,2,1,1,0), cursor, update(1,1,1), copy, update(-223,3,2)]);
  const before = f.down.length; f.feed('upstream', bytes);
  assert.deepEqual(Buffer.concat(f.down.slice(before)), bytes);
  assert.equal(f.rfb.width, 3); assert.equal(f.rfb.height, 2); f.rfb.close();
});

for (const h264 of [false, true]) test(`official noVNC encoding advertisement is bounded and filters unsupported compression (H264=${h264})`, () => {
  const f = fixture(), wire = socket(), api = official(h264);
  api.sendEncodings.call({ _fbDepth:24, _qualityLevel:6, _compressionLevel:2, _sock:wire });
  const offered = wire.frames[0]; assert.equal(offered.readUInt16BE(2), h264 ? 25 : 24);
  f.rfb.downstream(offered);
  assert.deepEqual(encodingValues(f.up.at(-1)), [1,0,-223,-258,-261,-239]);
  const native = fixture({ profile:'native' }); assert.throws(() => native.rfb.downstream(offered), e => e.code === 'console_limits_exceeded');
  native.rfb.close(); f.rfb.close();
});

test('browser count, duplicate and unknown encoding handling never widens native/frame/input limits', () => {
  const f = fixture(); f.rfb.downstream(encodingMessage([0,0,1,1,-239,16,50,-309]));
  assert.deepEqual(encodingValues(f.up.at(-1)), [0,1,-239]);
  f.rfb.downstream(encodingMessage([16,50,-309])); assert.deepEqual(encodingValues(f.up.at(-1)), [0]);
  const tooMany = Buffer.from([2,0,0,65]); assert.throws(() => f.rfb.downstream(tooMany), e => e.code === 'console_limits_exceeded'); f.rfb.close();
  for (const bytes of [Buffer.from([150]), Buffer.from([251]), update(16), Buffer.from([0,0,0x10,1])]) {
    const current = fixture(); assert.throws(() => bytes[0] === 0 ? current.rfb.upstream(bytes) : current.rfb.downstream(bytes)); current.rfb.close();
  }
});

test('browser refuses incompatible pixel format changes before any QEMU forwarding', () => {
  for (const format of [nativeFormat, Buffer.from([32,24,1,1,0,255,0,255,0,255,0,8,16,0,0,0]), Buffer.alloc(16)]) {
    const f = fixture(), before = f.up.length;
    assert.throws(() => f.rfb.downstream(Buffer.concat([Buffer.alloc(4),format])), e => e.code === 'console_rfb_unsupported');
    assert.equal(f.up.length,before); f.rfb.close();
  }
});

test('official QEMU key presses/releases need QEMU ACK alone in browser, while native still requires LED ACK', () => {
  for (const profile of ['native','novnc']) {
    const f = fixture({ profile }), wire = socket(), api = official();
    f.rfb.downstream(encodingMessage([0,-258,-261])); f.rfb.upstream(update(-258));
    api.messages.QEMUExtendedKeyEvent(wire, 0xffea, 1, 0xe038);
    api.messages.QEMUExtendedKeyEvent(wire, 0xffea, 0, 0xe038);
    assert.equal(wire.frames[0].readUInt32BE(8), 0xb8);
    if (profile === 'native') assert.throws(() => f.rfb.downstream(wire.frames[0]));
    else for (const frame of wire.frames) { f.rfb.downstream(frame); assert.deepEqual(f.up.at(-1), frame); }
    f.rfb.close();
  }
  const missing = fixture(); missing.rfb.downstream(encodingMessage([0,-258]));
  const key = socket(); official().messages.QEMUExtendedKeyEvent(key, 0x61, 1, 0x1e);
  assert.throws(() => missing.rfb.downstream(key.frames[0])); missing.rfb.close();
});

test('official ordinary Unicode keys and pointer press/releases preserve bytes without claiming guest input acceptance', () => {
  const f = fixture({ fragmented:true }), wire = socket(), api = official();
  api.messages.keyEvent(wire, 0x0100ac00, 1); api.messages.keyEvent(wire, 0x0100ac00, 0);
  api.messages.pointerEvent(wire, 1, 1, 1); api.messages.pointerEvent(wire, 1, 1, 0);
  for (const frame of wire.frames) { f.feed('downstream', frame); assert.deepEqual(f.up.at(-1), frame); }
  f.rfb.close();
});

for (const fragmented of [false,true]) test(`browser clipboard direction grants stay separate and text stays bounded (${fragmented ? 'bytewise' : 'coalesced'})`, () => {
  const text = Buffer.from([0x61,0xff,0x00]), client = cutText(text), host = cutText(text,false);
  for (const [toRemote,toLocal] of [[false,false],[true,false],[false,true],[true,true]]) {
    const f = fixture({ fragmented, clipboardPolicy:{ textToRemote:toRemote,textToLocal:toLocal } });
    const before = f.down.length; f.feed('upstream', Buffer.concat([host,Buffer.from([2])]));
    assert.deepEqual(Buffer.concat(f.down.slice(before)), toLocal ? Buffer.concat([host,Buffer.from([2])]) : Buffer.from([2]));
    if (toRemote) { f.feed('downstream',client); assert.deepEqual(f.up.at(-1),client); }
    else assert.throws(() => f.feed('downstream',client), e => e.code === 'console_clipboard_denied');
    f.rfb.close();
  }
  const native = fixture({ profile:'native',clipboardPolicy:{textToRemote:true,textToLocal:true} });
  assert.throws(() => native.rfb.downstream(client), e => e.code === 'console_clipboard_denied'); native.rfb.close();
  for (const side of ['upstream','downstream']) {
    const f = fixture({clipboardPolicy:{textToRemote:true,textToLocal:true}}), header = Buffer.alloc(8); header[0] = side === 'upstream' ? 3 : 6; header.writeUInt32BE(65537,4);
    assert.throws(() => f.rfb[side](header), e => e.code === 'console_limits_exceeded'); f.rfb.close();
  }
});
