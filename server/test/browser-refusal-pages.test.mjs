// What a browser shows at an address the server refuses or does not have (not raw JSON such as
// {"error":{"code":"origin_not_allowed"}} for a person who typed the IP, used http:// or mistyped a path).
// A card in the request's language with the same status; apps, scripts and the console's own requests keep the JSON.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { request as httpRequest } from 'node:http';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { parseHTML } from 'linkedom';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { initialPasswordHash } from '../src/security.mjs';
import { consoleHttpsPagePolicy } from '../src/browser-console.mjs';
import { normalizeServerAccess } from '../update/server-access-policy.mjs';

const browser = 'text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8';
async function serve(t, options, prepare = () => {}) {
  const work = await mkdtemp(join(tmpdir(), 'multitap-refusal-')), store = new Store(':memory:');
  await prepare(store);
  const server = await buildServer({ store, vault: new Vault(randomBytes(32)), dataDir: work, downloadDir: join(work, 'downloads'), logger: { error: value => assert.fail(value) }, initialSetupOptions: { announce() {} }, ...options });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); await rm(work, { recursive: true, force: true }); });
  const base = 'http://127.0.0.1:' + server.address().port;
  // `host`: the name a browser asked for (fetch cannot set Host, node:http can).
  return async (path, { accept = browser, language = 'ko-KR,ko;q=0.9', method = 'GET', host = null } = {}) => {
    const sent = { ...(accept ? { Accept: accept } : {}), 'Accept-Language': language };
    const response = host ? await new Promise((resolve, reject) => {
      const request = httpRequest(base + path, { method, headers: { ...sent, Host: host } }, incoming => { const chunks = []; incoming.on('data', chunk => chunks.push(chunk)); incoming.on('end', () => resolve({ status: incoming.statusCode, headers: new Headers(Object.entries(incoming.headers).map(([name, value]) => [name, String(value)])), text: async () => Buffer.concat(chunks).toString('utf8') })); });
      request.on('error', reject); request.end();
    }) : await fetch(base + path, { method, redirect: 'manual', headers: sent });
    const type = response.headers.get('content-type') ?? '', text = await response.text();
    return { status: response.status, type, text, headers: response.headers, document: type.startsWith('text/html') ? parseHTML(text).document : null, code: type.startsWith('application/json') ? JSON.parse(text).error?.code : null };
  };
}
const card = result => [result.document.querySelector('h1').textContent, ...[...result.document.querySelectorAll('p')].map(node => node.textContent)];

test('a page that does not exist: a card for a browser in its language, JSON for everyone else', async t => {
  const get = await serve(t, { publicOrigin: 'https://refusal-tests.example' });
  const ko = await get('/get/hos');
  assert.equal(ko.status, 404); assert.equal(ko.type, 'text/html; charset=utf-8');
  assert.equal(ko.headers.get('content-security-policy'), consoleHttpsPagePolicy); assert.equal(ko.headers.get('cache-control'), 'no-store');
  assert.equal(ko.document.documentElement.getAttribute('lang'), 'ko');
  assert.deepEqual(card(ko), ['페이지를 찾을 수 없어요', '주소를 다시 확인해 주세요.', '앱을 받으려면 /download 를 여세요.']);
  const en = await get('/nope', { language: 'en-US,en;q=0.9' });
  assert.equal(en.status, 404); assert.deepEqual(card(en), ['Page not found', 'Check the address again.', 'To get the apps, open /download.']);
  assert.equal(/[가-힣]/.test(en.text), false, 'no Hangul on the English page');
  assert.equal((await get('/nope', { method: 'HEAD' })).text, '');
  // Not a browser navigation, or an API path: the JSON as before.
  for (const [path, options] of [['/nope', { accept: '*/*' }], ['/nope', { accept: null }], ['/nope', { accept: 'application/json' }], ['/admin/nope.js', { accept: '*/*' }]]) {
    const result = await get(path, options);
    assert.equal(result.status, 404, path); assert.equal(result.type, 'application/json; charset=utf-8', path); assert.equal(result.code, 'not_found', path);
  }
  // Refusals with a meaning of their own stay JSON for a browser too.
  const session = await get('/api/v1/admin/session');
  assert.equal(session.status, 401); assert.equal(session.code, 'authentication_required');
});

test('an address that is not registered, and plain HTTP while HTTP is off', async t => {
  const accessConfig = normalizeServerAccess({ protocol: 'server-access-v1', primaryOrigin: 'https://vdi.refusal-tests.example', trustedProxies: ['127.0.0.1'],
    https: { enabled: true, origin: 'https://vdi.refusal-tests.example', mode: 'proxy', listenHost: '127.0.0.1', listenPort: 18443, certFile: null, keyFile: null },
    http: { enabled: false, origin: null, listenHost: '127.0.0.1', listenPort: 18480 } });
  const get = await serve(t, { publicOrigin: 'https://vdi.refusal-tests.example', accessConfig, accessRevision: 'a'.repeat(64), accessInstallationId: 'b'.repeat(64), trustedProxies: ['127.0.0.1'] });
  // By the listener's own address (the IP a person typed): not one of the registered addresses.
  for (const path of ['/', '/admin/', '/download']) {
    const result = await get(path);
    assert.equal(result.status, 403, path);
    assert.deepEqual(card(result), ['이 주소로는 열 수 없어요', '서버에 등록된 주소로만 열 수 있어요.', '주소를 모르면 관리자에게 물어보세요.'], path);
  }
  assert.deepEqual(card(await get('/', { language: 'en' })), ['This address cannot be opened', 'The server opens only at its registered addresses.', 'If you do not know the address, ask your administrator.']);
  // The registered name over plain HTTP.
  const plain = await get('/', { host: 'vdi.refusal-tests.example' });
  assert.equal(plain.status, 403);
  assert.deepEqual(card(plain), ['HTTPS 주소로 열어 주세요', '이 서버는 https://로 시작하는 주소에서만 열려요.', '주소를 모르면 관리자에게 물어보세요.']);
  // Apps and scripts keep the codes.
  assert.equal((await get('/', { accept: '*/*' })).code, 'origin_not_allowed');
  assert.equal((await get('/', { accept: 'application/json', host: 'vdi.refusal-tests.example' })).code, 'http_disabled');
  assert.equal((await get('/api/v1/admin/session')).code, 'origin_not_allowed');
});

test('before the first setup a page other than the setup says so', async t => {
  const get = await serve(t, { publicOrigin: null, bootstrap: true }, async store => { const id = store.addUser({ name: 'admin', role: 'admin', passwordHash: await initialPasswordHash() }); store.db.prepare('UPDATE users SET must_change_credentials=1 WHERE id=?').run(id); });
  const root = await get('/');
  assert.equal(root.status, 302); assert.equal(root.headers.get('location'), '/admin/');
  const download = await get('/download');
  assert.equal(download.status, 403);
  assert.deepEqual(card(download), ['처음 설정을 먼저 마쳐 주세요', '이 서버는 아직 처음 설정 전이라 이 주소는 열 수 없어요.', '관리자 페이지(/admin/)에서 설정을 마치면 열려요.']);
  assert.deepEqual(card(await get('/download', { language: 'en' })), ['Finish the first setup first', 'This server is not set up yet, so this address cannot be opened.', 'It opens once the setup is finished in the admin console (/admin/).']);
  assert.equal((await get('/download', { accept: '*/*' })).code, 'http_disabled');
});
