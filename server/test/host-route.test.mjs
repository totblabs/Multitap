import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { configuration } from '../src/config.mjs';

// host-route-v1: the Broker names the address Hosts in its network reach it at
// (MULTITAP_HOST_ROUTE), in host.capabilities.ack only.
const caps = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true, monitors: [{ id: '0', width: 1920, height: 1080, primary: true }] };

async function ack(t, relayOptions) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const vms = [{ vmid: 149, node: 'fixture', name: 'TESTER', status: 'running' }];
  store.syncProxmox(vms, 'fixture');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve.invalid', tokenId: 'fixture@pve!fixture', secret: 'fixture' }), 'fixture');
  const code = newToken(), uuid = '00000000-1234-1234-1234-000000000149', hostToken = newToken();
  store.issueEnrollment('pve-149', uuid, tokenHash(code), 'fixture');
  store.consumeEnrollment(tokenHash(code), uuid, { hostname: 'fixture-1', version: '0.2.131', tokenHash: tokenHash(hostToken) });
  const server = await buildServer({ store, vault, publicOrigin: 'https://route.invalid', proxmox: { inventory: async () => vms }, relayOptions: { authorizationMs: 20, heartbeatMs: 1000, ...relayOptions }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  const ws = new WebSocket(`ws://127.0.0.1:${server.address().port}/api/v1/hosts/channel`, { headers: { Authorization: 'Host ' + hostToken, 'X-Multitap-Host-Protocol': 'capabilities-v1' }, perMessageDeflate: false });
  await once(ws, 'open'); t.after(() => ws.close());
  const reply = new Promise(resolve => ws.on('message', data => { const value = JSON.parse(data.toString()); if (value.type === 'host.capabilities.ack') resolve(value); }));
  ws.send(JSON.stringify({ type: 'host.capabilities', capabilities: caps }));
  return reply;
}

test('the capabilities acknowledgement carries the configured route and nothing without one', async t => {
  const routed = await ack(t, { hostRoute: '192.0.2.101' });
  assert.equal(routed.accepted, true);
  assert.equal(routed.hostRoute, '192.0.2.101');
  const plain = await ack(t, {});
  assert.equal(plain.accepted, true);
  assert.equal('hostRoute' in plain, false);
});

test('MULTITAP_HOST_ROUTE takes an IPv4 address only', () => {
  const base = { MULTITAP_DATA_DIR: 'C:/fixture-route', MULTITAP_PUBLIC_ORIGIN: 'https://route.invalid' };
  const read = env => configuration({ ...base, ...env }, { readAccessFile: () => null });
  assert.equal(read({}).hostRoute, null);
  assert.equal(read({ MULTITAP_HOST_ROUTE: '192.0.2.101' }).hostRoute, '192.0.2.101');
  for (const bad of ['vdi.example.com', '192.0.2', '192.0.2.101:443', '::1']) assert.throws(() => read({ MULTITAP_HOST_ROUTE: bad }), /invalid_host_route/, bad);
});
