import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Store } from '../src/store.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';

// relay.revalidate() walks every live channel. Only a successful request that
// can change state asks for it (once per event-loop turn), never a flood of
// unauthenticated misses, refusals or reads.
test('50 unauthenticated 404s, refused requests and reads trigger no revalidate; a successful change triggers one', async t => {
  const store = new Store(':memory:'), origin = 'https://revalidate.fixture.invalid', password = 'Revalidate-fixture-password-61!';
  store.addUser({ name: 'fixture-user', passwordHash: await hashPassword(password) });
  // The relay's own periodic check is pushed out of the way; only requests count here.
  const server = await buildServer({ store, publicOrigin: origin, relayOptions: { authorizationMs: 3600000 }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  let runs = 0; const original = server.relay.revalidate.bind(server.relay);
  server.relay.revalidate = () => { runs++; return original(); };
  const base = `http://127.0.0.1:${server.address().port}`, settle = () => new Promise(resolve => setImmediate(resolve));
  const misses = await Promise.all(Array.from({ length: 50 }, (_, i) => fetch(base + '/no-such-page-' + i)));
  assert.deepEqual([...new Set(misses.map(response => response.status))], [404]);
  await Promise.all(misses.map(response => response.arrayBuffer()));
  const refused = await Promise.all([...Array.from({ length: 10 }, (_, i) => fetch(base + '/api/v1/no-such-route-' + i)),
    ...Array.from({ length: 5 }, () => fetch(base + '/api/v1/auth/login', { method: 'POST', headers: { Origin: origin, 'Content-Type': 'application/json' }, body: JSON.stringify({ username: 'fixture-user', password: 'wrong-password-only' }) }))]);
  assert.ok(refused.every(response => response.status >= 400)); await Promise.all(refused.map(response => response.arrayBuffer()));
  const health = await fetch(base + '/healthz'); assert.equal(health.status, 200); await health.arrayBuffer();
  await settle();
  assert.equal(runs, 0, 'nothing that failed or only read asked for a re-check');
  const login = await fetch(base + '/api/v1/auth/login', { method: 'POST', headers: { Origin: origin, 'Content-Type': 'application/json' }, body: JSON.stringify({ username: 'fixture-user', password }) });
  assert.equal(login.status, 200, JSON.stringify(await login.clone().json())); await login.arrayBuffer(); await settle();
  assert.equal(runs, 1, 'a successful sign-in re-checks once');
});
