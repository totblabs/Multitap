import test from 'node:test';
import assert from 'node:assert/strict';
import { createServer, request } from 'node:http';
import { randomBytes } from 'node:crypto';
import { buildServer } from '../src/http.mjs';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { normalizeServerAccess } from '../update/server-access-policy.mjs';

for (const [label, httpOrigin, expected] of [
  ['HTTPS only', null, 'max-age=31536000'],
  ['explicit same-host HTTP on another port', 'http://alpha.example:8080', 'max-age=0'],
  ['explicit HTTP on a different hostname', 'http://plain.example:8080', 'max-age=31536000'],
]) test('HTTPS security policy follows actual enabled hostname scope: ' + label, async t => {
  const reservation = createServer();
  await new Promise(resolve => reservation.listen(0, '127.0.0.1', resolve));
  const port = reservation.address().port;
  await new Promise(resolve => reservation.close(resolve));
  const origin = 'https://alpha.example:8443';
  const config = normalizeServerAccess({ protocol: 'server-access-v1', primaryOrigin: origin,
    https: { enabled: true, origin, mode: 'proxy', listenHost: '127.0.0.1', listenPort: port },
    http: { enabled: httpOrigin !== null, origin: httpOrigin, listenHost: '127.0.0.1', listenPort: port },
    trustedProxies: ['127.0.0.1'] });
  const store = new Store(':memory:');
  const server = await buildServer({ store, vault: new Vault(randomBytes(32)), publicOrigin: origin,
    allowInsecureHttp: httpOrigin !== null, accessConfig: config, trustedProxies: config.trustedProxies,
    initialSetupOptions: { announce: () => {} } });
  await new Promise(resolve => server.listen(port, '127.0.0.1', resolve));
  t.after(async () => { server.closeAllConnections(); await new Promise(resolve => server.close(resolve)); store.close(); });
  const get = (host, forwarded) => new Promise((resolve, reject) => {
    const req = request({ hostname: '127.0.0.1', port, path: '/healthz', headers: {
      Host: host, ...(forwarded ? { 'X-Forwarded-Proto': 'https' } : {}),
    } }, res => { res.resume(); res.once('end', () => resolve(res)); });
    req.on('error', reject); req.end();
  });
  const secure = await get('alpha.example:8443', true);
  assert.equal(secure.statusCode, 200);
  assert.equal(secure.headers['strict-transport-security'], expected);
  if (httpOrigin !== null) {
    const plain = await get(new URL(httpOrigin).host, false);
    assert.equal(plain.statusCode, 200);
    assert.equal(plain.headers['strict-transport-security'], undefined, 'HTTP cannot set or clear HSTS');
    assert.equal(plain.headers.location, undefined, 'explicit HTTP is an actual listener, not a redirect');
  }
});
