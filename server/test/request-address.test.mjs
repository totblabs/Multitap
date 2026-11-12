import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { Store } from '../src/store.mjs';
import { buildServer, Limiter } from '../src/http.mjs';
import { hashPassword, requestAddress, trustedProxies } from '../src/security.mjs';
import { configuration } from '../src/config.mjs';

// The deployed broker sits behind a TLS reverse proxy, so every request arrives
// from the proxy's own address. Per-address limits keyed by that socket address
// were one global limit: any internet client could lock every account out.

const password = 'Only-for-address-tests-42!';
const peer = (remoteAddress, forwarded) => ({ socket: { remoteAddress }, headers: forwarded === undefined ? {} : { 'x-forwarded-for': forwarded } });

test('The rate-limit address is the forwarding client of a trusted proxy, never a spoofable header from anyone else', () => {
  const local = trustedProxies('private');
  // Default: loopback/private peers are the reverse proxy; the hop it appended is the client.
  assert.equal(requestAddress(peer('127.0.0.1', '203.0.113.7'), local), '203.0.113.7');
  assert.equal(requestAddress(peer('::ffff:10.0.0.2', '203.0.113.7'), local), '203.0.113.7');
  // A client-supplied prefix cannot choose the key: only the proxy's own (rightmost) hop counts.
  assert.equal(requestAddress(peer('10.0.0.2', '198.51.100.1, 203.0.113.7'), local), '203.0.113.7');
  // Public peers are clients themselves; their header is ignored.
  assert.equal(requestAddress(peer('198.51.100.20', '203.0.113.7'), local), '198.51.100.20');
  // Missing or malformed forwarding falls back to the peer (today's behaviour).
  assert.equal(requestAddress(peer('127.0.0.1'), local), '127.0.0.1');
  for (const bad of ['', 'not-an-ip', '203.0.113.7, ', 'x'.repeat(2000)]) assert.equal(requestAddress(peer('127.0.0.1', bad), local), '127.0.0.1');
  assert.equal(requestAddress(peer(undefined), local), 'unknown');
  // IPv6 clients are limited per /64, the unit one subscriber controls.
  assert.equal(requestAddress(peer('127.0.0.1', '2001:db8:1:2:aaaa::1'), local), '2001:db8:1:2::/64');
  assert.equal(requestAddress(peer('127.0.0.1', '2001:db8:1:2:ffff:ffff:ffff:fffe'), local), '2001:db8:1:2::/64');
  assert.equal(requestAddress(peer('2001:db8:9::5'), local), '2001:db8:9:0::/64');
  // An explicit list trusts exactly those proxies, walking past chained ones.
  const listed = trustedProxies('192.0.2.10, 192.0.2.11');
  assert.equal(requestAddress(peer('127.0.0.1', '203.0.113.7'), listed), '127.0.0.1');
  assert.equal(requestAddress(peer('192.0.2.10', '203.0.113.7, 192.0.2.11'), listed), '203.0.113.7');
  // "none" disables forwarding entirely.
  assert.equal(requestAddress(peer('127.0.0.1', '203.0.113.7'), trustedProxies('none')), '127.0.0.1');
  for (const invalid of ['192.0.2.0/24', 'proxy.example', '1.2.3.4,,', 'none,1.2.3.4']) assert.throws(() => trustedProxies(invalid), /invalid_trusted_proxies/);
  assert.throws(() => configuration({ MULTITAP_TRUSTED_PROXIES: 'proxy.example' }), /server_access_trusted_proxies_invalid/);
  assert.deepEqual(configuration({ MULTITAP_TRUSTED_PROXIES: '192.0.2.10' }).trustedProxies, ['192.0.2.10']);
});

test('A full limiter never forgets an account that is currently locked out', () => {
  const limiter = new Limiter(2, 60000, 4), now = 1000;
  assert.equal(limiter.take('victim', now), true); assert.equal(limiter.take('victim', now), true);
  assert.equal(limiter.take('victim', now), false);
  // Spraying fresh keys used to evict the oldest entry: the victim's counter.
  for (let index = 0; index < 50; index++) limiter.take('spray-' + index, now);
  assert.equal(limiter.take('victim', now), false);
  // Expired entries still make room, and the window still ends.
  assert.equal(limiter.take('later', now + 60000), true);
  assert.equal(limiter.take('victim', now + 60000), true);
});

async function fixture(t, options = {}) {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-address-'));
  const store = new Store(join(directory, 'broker.sqlite'));
  const hashed = await hashPassword(password);
  store.addUser({ name: 'alice', passwordHash: hashed }); store.addUser({ name: 'bobby', passwordHash: hashed });
  const server = await buildServer({ store, publicOrigin: 'https://address-tests.example', logger: { error: value => assert.fail(value) }, ...options });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); await rm(directory, { recursive: true }); });
  const url = `http://127.0.0.1:${server.address().port}`;
  const login = async (username, secret, from) => {
    const response = await fetch(url + '/api/v1/auth/login', { method: 'POST', headers: { 'Content-Type': 'application/json', ...(from ? { 'X-Forwarded-For': from } : {}) }, body: JSON.stringify({ username, password: secret }) });
    return { status: response.status, headers: response.headers, body: await response.json() };
  };
  return { url, login };
}

test('One client exhausting its login attempts behind the reverse proxy does not lock out everyone else', async t => {
  // The proxy runs on this machine and is the one configured (production passes its list; nothing configured trusts no header).
  const f = await fixture(t, { loginLimit: 1, addressLimit: 1, trustedProxies: ['127.0.0.1'] });
  assert.equal((await f.login('alice', 'wrong-password-123', '203.0.113.7')).status, 401);
  // The attacker's address is spent: further tries from it are refused...
  assert.equal((await f.login('bobby', 'wrong-password-123', '203.0.113.7')).status, 429);
  // ...without spending bobby's own account budget or anyone else's address.
  const other = await f.login('bobby', password, '198.51.100.9');
  assert.equal(other.status, 200);
});

test('A configured proxy list ignores the forwarding header from any other peer', async t => {
  const f = await fixture(t, { loginLimit: 5, addressLimit: 1, trustedProxies: ['192.0.2.10'] });
  assert.equal((await f.login('alice', 'wrong-password-123', '203.0.113.7')).status, 401);
  assert.equal((await f.login('bobby', password, '198.51.100.9')).status, 429);
});

test('Browser-facing pages and API responses ask browsers to stay on HTTPS', async t => {
  const f = await fixture(t);
  for (const path of ['/admin/', '/healthz', '/api/v1/desktops']) {
    const response = await fetch(f.url + path, { headers: { Accept: 'text/html' } });
    assert.equal(response.headers.get('strict-transport-security'), 'max-age=31536000', path);
  }
  const landing = await fetch(f.url + '/', { headers: { Accept: 'text/html' } });
  assert.equal(landing.headers.get('content-type'), 'text/html; charset=utf-8');
  assert.equal(landing.headers.get('strict-transport-security'), 'max-age=31536000');
});

test('Without a configured proxy the forwarding header is never trusted, by default as well', () => {
  for (const value of [undefined, null, '', [], 'none', ' NONE ']) {
    const trust = trustedProxies(value);
    assert.equal(trust.mode, 'none', String(value));
    assert.equal(requestAddress(peer('127.0.0.1', '203.0.113.7'), trust), '127.0.0.1');
  }
  assert.equal(requestAddress(peer('10.0.0.2', '203.0.113.7')), '10.0.0.2', 'requestAddress without a trust argument');
  // The production shape (config.mjs): the saved list, or [] when there is none.
  assert.equal(requestAddress(peer('192.0.2.10', '203.0.113.7'), trustedProxies(['192.0.2.10'])), '203.0.113.7');
});
