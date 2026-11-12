import { test } from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { WebSocket } from 'ws';
import { DriveRelay } from '../src/drive.mjs';

// Actual relay handlers, in-memory peers. No listener, credentials or filesystem.
class Peer extends EventEmitter {
  readyState = WebSocket.OPEN; bufferedAmount = 0; sent = [];
  send(text, options, done) { this.sent.push(JSON.parse(text)); done?.(); }
  close() { this.readyState = WebSocket.CLOSED; this.emit('close'); }
  terminate() { this.close(); }
  pause() {} resume() {} ping() {}
  json(value) { this.emit('message', Buffer.from(JSON.stringify(value)), false); }
  last(type) { return this.sent.findLast(x => x.type === type); }
}
function fixture(t, { client = true, host = true } = {}) {
  let allowed = true, authorized = true;
  const session = { id: 'fixture-session', mode: 'stream', driveEnabled: true,
    userId: 'user', desktopId: 'vm', ws: { readyState: WebSocket.OPEN } };
  const owner = { current: s => s === session, authorized: () => authorized,
    store: { devicePolicy: () => ({ drives: allowed }) } };
  const relay = new DriveRelay(owner), c = new Peer(), h = new Peer();
  const req = enabled => ({ headers: enabled ? { 'x-multitap-drive-open': 'drive.open-v1' } : {} });
  relay.attach(c, session, 'client', req(client)); relay.attach(h, session, 'host', req(host));
  t.after(() => relay.close());
  return { relay, c, h, session, deny: () => { allowed = false; }, revoke: () => { authorized = false; } };
}
const share = { id: 'docs', name: 'Documents', readOnly: false };
const select = f => f.c.json({ type: 'drive.shares', shares: [share] });
const mapped = (f, letter = 'Z') => f.h.json({ type: 'drive.mounted', shares: [{ id: 'docs', state: 'mapped', letter }] });
const open = (f, id = 1) => f.c.json({ type: 'drive.open', id, share: 'docs' });
const ack = (f, id = 1) => f.h.json({ type: 'drive.opened', id, share: 'docs', ok: true });

test('open marker is bilateral; released peers keep exact ready fields', t => {
  for (const options of [{ client: false }, { host: false }, { client: false, host: false }]) {
    const f = fixture(t, options);
    assert.deepEqual(Object.keys(f.c.last('drive.ready')).sort(), ['limits', 'protocol', 'sessionId', 'type']);
    select(f); mapped(f); open(f);
    assert.equal(f.h.last('drive.open'), undefined);
    assert.equal(f.session.driveClosed, true);
  }
});
test('selection alone never authorizes opening; current mapped share does', t => {
  const f = fixture(t); select(f); open(f);
  assert.equal(f.c.last('drive.opened').error, 'drive_share_unknown');
  assert.equal(f.h.last('drive.open'), undefined);
  mapped(f); open(f, 2);
  assert.deepEqual(f.h.last('drive.open'), { type: 'drive.open', id: 2, share: 'docs' });
  ack(f, 2); assert.equal(f.c.last('drive.opened').ok, true);
});
test('no caller path, shell arguments or noninteger IDs are accepted', t => {
  for (const extra of [{ path: 'C:\\Windows' }, { command: 'explorer.exe' }, { id: 1.5 }, { id: 0 }]) {
    const f = fixture(t); select(f); mapped(f);
    f.c.json({ type: 'drive.open', id: 1, share: 'docs', ...extra });
    assert.equal(f.h.last('drive.open'), undefined); assert.equal(f.session.driveClosed, true);
  }
});
test('single actual slot survives ordinary request timeout observation', t => {
  const f = fixture(t); select(f); mapped(f); open(f);
  f.session.drivePair.createdAt = 0; f.relay.requestMs = -1; f.relay.revalidate();
  open(f, 2); assert.equal(f.c.last('drive.opened').error, 'drive_busy');
  assert.equal(f.h.sent.filter(x => x.type === 'drive.open').length, 1);
  ack(f); open(f, 3); assert.equal(f.h.last('drive.open').id, 3);
});
test('removed or replaced mapping cannot bless a late success', t => {
  for (const remove of [false, true]) {
    const f = fixture(t); select(f); mapped(f); open(f);
    if (remove) f.c.json({ type: 'drive.shares', shares: [] }); else mapped(f, 'Y');
    ack(f); assert.equal(f.c.last('drive.opened').error, 'drive_share_unknown');
  }
});
test('readonly-only change preserves the same current mapping', t => {
  const f = fixture(t); select(f); mapped(f); open(f);
  f.c.json({ type: 'drive.shares', shares: [{ ...share, readOnly: true }] });
  ack(f); assert.equal(f.c.last('drive.opened').ok, true);
});
test('permission loss and exact session end retire opens before forwarding', t => {
  for (const finish of [f => f.deny(), f => f.revoke(), f => f.relay.closeSession(f.session, 'session_ended')]) {
    const f = fixture(t); select(f); mapped(f); open(f);
    finish(f); const count = f.c.sent.length; ack(f);
    assert.equal(f.c.sent.slice(count).some(x => x.type === 'drive.opened'), false);
    assert.equal(f.session.driveClosed, true);
  }
});
test('old channel replies cannot reach a replacement pair', t => {
  const f = fixture(t); select(f); mapped(f); open(f);
  const count = f.c.sent.length; f.session.drivePair = {};
  ack(f); assert.equal(f.c.sent.length, count);
});

test('actual terminal unknown permits a later explicit request without replay', t => {
  const f = fixture(t); select(f); mapped(f); open(f);
  f.h.json({ type: 'drive.opened', id: 1, share: 'docs', ok: false, error: 'drive_open_unknown' });
  assert.equal(f.h.sent.filter(x => x.type === 'drive.open').length, 1);
  open(f, 2); assert.equal(f.h.last('drive.open').id, 2);
});
