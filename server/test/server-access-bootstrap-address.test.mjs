import test from 'node:test';
import assert from 'node:assert/strict';
import { resolveRequestAccess } from '../src/request-origin.mjs';
const request = (authority, localAddress, remoteAddress = '198.51.100.10') => ({
  headers: { host: authority }, rawHeaders: ['Host', authority],
  socket: { localAddress, remoteAddress, localPort: 18480 },
});
test('initial administration binds exact socket destination without requiring interface enumeration privileges', () => {
  const admitted = resolveRequestAccess(request('192.0.2.10:18480', '::ffff:192.0.2.10'), null, { bootstrap: true });
  assert.equal(admitted.origin, 'http://192.0.2.10:18480'); assert.equal(admitted.bootstrapOnly, true);
  for (const authority of ['192.0.2.11:18480', '192.0.2.10:18481', '127.0.0.1:18480', 'localhost:18480']) {
    assert.throws(() => resolveRequestAccess(request(authority, '192.0.2.10'), null, { bootstrap: true }), error => error.code === 'origin_not_allowed');
  }
});
test('loopback bootstrap accepts its real local path and refuses an external peer claiming loopback', () => {
  for (const [authority, local, peer] of [['127.0.0.1:18480', '127.0.0.1', '127.0.0.1'], ['localhost:18480', '::1', '::1'], ['[::1]:18480', '::1', '::1']]) {
    assert.equal(resolveRequestAccess(request(authority, local, peer), null, { bootstrap: true }).bootstrapOnly, true);
    assert.throws(() => resolveRequestAccess(request(authority, local, '198.51.100.10'), null, { bootstrap: true }), error => error.code === 'origin_not_allowed');
  }
});
