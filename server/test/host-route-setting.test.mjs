import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { hostRouteAddress, hostRouteSetting } from '../src/host-route.mjs';

// host-route-v1 saved in the administration console (host-route.mjs): what the Broker offers Hosts and lists first
// among their UDP relays, taken over at once; MULTITAP_HOST_ROUTE in .env decides where it is set.
const caps = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true, monitors: [{ id: '0', width: 1920, height: 1080, primary: true }] };
const origin = 'https://route.invalid';

test('a route is an IPv4 address a Host could be sent to', () => {
  for (const good of ['10.20.30.40', '192.168.0.10', '172.16.5.4', '203.0.113.7']) assert.equal(hostRouteAddress(good), good);
  for (const none of ['', null, undefined]) assert.equal(hostRouteAddress(none), null);
  for (const bad of ['0.0.0.0', '0.1.2.3', '127.0.0.1', '169.254.10.1', '224.0.0.1', '239.1.1.1', '255.255.255.255', '10.0.0', '010.0.0.1', ' 10.0.0.1', '10.0.0.1:443', 'host.example', '::1', 5, {}])
    assert.throws(() => hostRouteAddress(bad), error => error.status === 400 && error.code === 'invalid_host_route', String(bad));
});

async function fixture(t, { store = new Store(':memory:'), fixed = null } = {}) {
  const vault = new Vault(randomBytes(32));
  if (!store.findUser('route-admin')) {
    store.addUser({ name: 'route-admin', role: 'admin', passwordHash: 'fixture-no-login' });
    store.addUser({ name: 'route-user', role: 'user', passwordHash: 'fixture-no-login' });
    const vms = [{ vmid: 149, node: 'fixture', name: 'VM', status: 'running' }];
    store.syncProxmox(vms, 'fixture');
    store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve.invalid', tokenId: 'fixture@pve!route', secret: 'fixture-not-real' }), 'fixture');
  }
  const tokens = Object.fromEntries(['route-admin', 'route-user'].map(name => {
    const token = newToken(); store.createSession(store.findUser(name), tokenHash(token), Date.now(), 3600000, name === 'route-admin' ? 'admin' : 'client'); return [name, token];
  }));
  const code = newToken(), hostToken = newToken(), machine = '00000000-1234-1234-1234-' + randomBytes(6).toString('hex');
  store.issueEnrollment('pve-149', machine, tokenHash(code), 'fixture');
  store.consumeEnrollment(tokenHash(code), machine, { hostname: 'fixture-149', version: '1.1.51', tokenHash: tokenHash(hostToken) });
  const server = await buildServer({ store, vault, publicOrigin: origin, proxmox: { inventory: async () => [{ vmid: 149, node: 'fixture', name: 'VM', status: 'running' }] },
    relayOptions: { authorizationMs: 20, heartbeatMs: 1000, hostRoute: fixed, udp: { port: 0, host: '127.0.0.1', derivedPort: 18481, logger: { warn() {} } } }, logger: { error() {} } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const base = 'http://127.0.0.1:' + server.address().port, sockets = [];
  t.after(async () => { for (const ws of sockets) ws.terminate(); await new Promise(resolve => server.close(resolve)); });
  const call = async (path, { method = 'GET', name = 'route-admin', data } = {}) => {
    const token = tokens[name], admin = name === 'route-admin';
    const response = await fetch(base + '/api/v1' + path, { method, headers: { Origin: origin,
      ...(admin ? { Cookie: '__Secure-multitap-admin=' + token, 'X-CSRF-Token': vault.csrf(token) } : { Authorization: 'Bearer ' + token }),
      ...(data !== undefined ? { 'Content-Type': 'application/json' } : {}) }, body: data !== undefined ? JSON.stringify(data) : undefined });
    return { status: response.status, body: await response.json() };
  };
  // A Host channel opened now: what its acknowledgement says.
  const ack = async () => {
    const ws = new WebSocket(base.replace('http', 'ws') + '/api/v1/hosts/channel', { headers: { Authorization: 'Host ' + hostToken, 'X-Multitap-Host-Protocol': 'capabilities-v1' }, perMessageDeflate: false });
    sockets.push(ws); await once(ws, 'open');
    const reply = new Promise(resolve => ws.on('message', data => { const value = JSON.parse(data.toString()); if (value.type === 'host.capabilities.ack') resolve(value); }));
    ws.send(JSON.stringify({ type: 'host.capabilities', capabilities: caps }));
    const value = await reply; ws.close(); return value;
  };
  const hostRelays = () => server.relay.udp.relaysFor('host', origin).map(row => row.host + ':' + row.port);
  return { store, server, call, ack, hostRelays };
}

test('saved in the console, the route is offered and listed first at once; cleared, it is gone; it survives a restart', async t => {
  const store = new Store(':memory:');
  t.after(() => store.close());
  const f = await fixture(t, { store });
  assert.deepEqual((await f.call('/admin/host-route')).body, { fixed: null, saved: null, current: null });
  assert.equal('hostRoute' in await f.ack(), false);
  assert.deepEqual(f.hostRelays(), ['route.invalid:18481']);
  const saved = await f.call('/admin/host-route', { method: 'PUT', data: { address: '10.20.30.40' } });
  assert.equal(saved.status, 200);
  assert.deepEqual(saved.body, { fixed: null, saved: '10.20.30.40', current: '10.20.30.40' });
  assert.equal((await f.ack()).hostRoute, '10.20.30.40', 'a Host that connects now is offered it');
  assert.deepEqual(f.hostRelays(), ['10.20.30.40:18481', 'route.invalid:18481'], 'a session that starts now lists it first for the Host');
  assert.equal(f.server.relay.hostRoute, '10.20.30.40');
  assert.equal(store.setting(hostRouteSetting), '10.20.30.40');
  assert.ok(store.auditLog().some(row => row.event === 'integration_updated' && row.subject === hostRouteSetting), 'the change is in the audit log');
  // A new server on the same database starts with it.
  const again = await fixture(t, { store });
  assert.equal((await again.ack()).hostRoute, '10.20.30.40');
  assert.deepEqual(again.hostRelays(), ['10.20.30.40:18481', 'route.invalid:18481']);
  const cleared = await again.call('/admin/host-route', { method: 'PUT', data: { address: '' } });
  assert.deepEqual(cleared.body, { fixed: null, saved: null, current: null });
  assert.equal('hostRoute' in await again.ack(), false);
  assert.deepEqual(again.hostRelays(), ['route.invalid:18481']);
});

test('a wrong address is refused and changes nothing; only an administrator may save; .env decides where it sets one', async t => {
  const f = await fixture(t);
  await f.call('/admin/host-route', { method: 'PUT', data: { address: '192.168.0.10' } });
  for (const address of ['127.0.0.1', '10.0.0', 'host.example', 7]) {
    const refused = await f.call('/admin/host-route', { method: 'PUT', data: { address } });
    assert.equal(refused.status, 400); assert.equal(refused.body.error.code, 'invalid_host_route', String(address));
  }
  assert.equal((await f.call('/admin/host-route', { method: 'PUT', data: { address: '10.0.0.1', extra: 1 } })).status, 400);
  assert.equal((await f.call('/admin/host-route', { method: 'PUT', name: 'route-user', data: { address: '10.0.0.1' } })).status, 401);
  assert.equal((await f.call('/admin/host-route', { method: 'POST', data: { address: '10.0.0.1' } })).status, 405);
  assert.deepEqual((await f.call('/admin/host-route')).body, { fixed: null, saved: '192.168.0.10', current: '192.168.0.10' });
  assert.equal((await f.ack()).hostRoute, '192.168.0.10');

  const fixed = await fixture(t, { fixed: '192.0.2.101' });
  assert.deepEqual((await fixed.call('/admin/host-route')).body, { fixed: '192.0.2.101', saved: null, current: '192.0.2.101' });
  const locked = await fixed.call('/admin/host-route', { method: 'PUT', data: { address: '10.0.0.1' } });
  assert.equal(locked.status, 409); assert.equal(locked.body.error.code, 'host_route_fixed');
  assert.equal((await fixed.ack()).hostRoute, '192.0.2.101');
  assert.deepEqual(fixed.hostRelays(), ['192.0.2.101:18481', 'route.invalid:18481']);
});
