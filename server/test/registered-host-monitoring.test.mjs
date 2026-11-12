import test from 'node:test';
import assert from 'node:assert/strict';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { enrolledHosts } from '../src/admin-monitoring.mjs';

test('an open but quarantined Host remains listed and is never reported connected', () => {
  const store = new Store(':memory:');
  try {
    const uuid = '11223344-1234-1234-1234-112233445566', token = newToken(), code = newToken();
    store.syncProxmox([{ node: 'fixture', vmid: 174, name: 'Fixture VM', status: 'running', machineUuid: uuid }], 'fixture');
    store.issueEnrollment('pve-174', uuid, tokenHash(code), 'fixture');
    store.consumeEnrollment(tokenHash(code), uuid, { hostname: 'fixture', version: '1.0.12', tokenHash: tokenHash(token) });
    const identity = store.authenticateHost(tokenHash(token)); assert.ok(identity);
    const live = { identity, ws: { readyState: WebSocket.OPEN } };
    const relay = { hosts: new Map([['pve-174', live]]), hostAuthenticated: value => store.authenticateHost(tokenHash(token))?.id === value.identity.id };
    assert.equal(enrolledHosts(store, relay).hosts[0].connected, true);
    store.db.prepare('UPDATE desktops SET pve_available=0 WHERE id=?').run('pve-174');
    assert.equal(store.authenticateHost(tokenHash(token)), undefined);
    const view = enrolledHosts(store, relay).hosts;
    assert.equal(view.length, 1); assert.equal(view[0].hostId, identity.id); assert.equal(view[0].connected, false);
    assert.equal(live.ws.readyState, WebSocket.OPEN, 'authorization loss is visible before socket cleanup');
    store.db.prepare('UPDATE desktops SET pve_available=1 WHERE id=?').run('pve-174');
    assert.equal(enrolledHosts(store, relay).hosts[0].connected, true);
    assert.equal(enrolledHosts(store, { hosts: relay.hosts }).hosts[0].connected, false, 'missing verifier is not a positive observation');
  } finally { store.close(); }
});
