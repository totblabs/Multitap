// The link 서버 접속 설정 shows while a save waits for the administrator's
// own proof of an address (admin/js/server-access.js outsideLink), opened in
// the whole console: app.js keeps the code that follows the page's name, takes
// it out of the address bar, and the page sends it once the administrator is
// signed in. The server's answers are given here (admin-ui-harness.mjs).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { loadAdmin, ownerFixture } from './admin-ui-harness.mjs';
import { outsideLink } from '../admin/js/server-access.js';

const here = 'https://vdi.example.com', lan = 'http://192.0.2.10:18480', operationId = '11111111-2222-4333-8444-555555555555';
const policy = https => ({ protocol: 'server-access-v1', primaryOrigin: lan, https: { enabled: https, origin: https ? here : null, mode: 'proxy', listenHost: '0.0.0.0', listenPort: 18480, certFile: null, keyFile: null },
  http: { enabled: true, origin: lan, listenHost: '0.0.0.0', listenPort: 18480 }, trustedProxies: ['127.0.0.1'] });
// What the server reports at an address under proof: the save that waits, without the code.
const waiting = confirmed => ({ protocol: 'server-access-v1', revision: 'a'.repeat(64), applied: policy(false), supported: true, managementOrigin: here, state: 'awaiting_confirmation', operationId,
  pending: { config: policy(true), state: 'awaiting_confirmation', operationId }, outside: { origins: [{ origin: here, confirmed }], remainingMs: 240000, windowMs: 300000 } });
const ok = value => ({ ok: true, status: 200, json: async () => value });

async function consoleAt(t, suffix, hash) {
  const { user, inventory } = ownerFixture(), sent = [], replaced = [];
  let confirmed = false;
  // The browser's address bar: app.js rewrites it without a new history step.
  const history = Object.getOwnPropertyDescriptor(globalThis, 'history');
  globalThis.history = { pushState() {}, replaceState: (state, title, target) => { replaced.push(target); } };
  t.after(() => { if (history) Object.defineProperty(globalThis, 'history', history); else delete globalThis.history; });
  const f = await loadAdmin(t, suffix, user, inventory, { origin: here, hash, respond: async (url, options = {}) => {
    if (url === '/api/v1/admin/server-access/confirm') { sent.push(JSON.parse(options.body)); confirmed = true; return ok({ accepted: true, operationId, origin: here, remaining: [] }); }
    return url === '/api/v1/admin/server-access' ? ok(waiting(confirmed)) : null;
  } });
  return { ...f, sent, replaced };
}

test('the console opened by the link: the code is sent from that address once, and is not left in the address bar', async t => {
  assert.equal(outsideLink(here, '482913'), here + '/admin/#/server-access/confirm-482913');
  const f = await consoleAt(t, 'access-confirm-link', '#/server-access/confirm-482913');
  await f.settle();
  assert.equal(f.crumb(), '서버 접속 설정');
  assert.deepEqual(f.sent, [{ operationId, code: '482913' }]);
  assert.equal(f.writes.filter(write => write.path === '/api/v1/admin/server-access/confirm').length, 1);
  assert.deepEqual(f.replaced, ['#/server-access'], 'the address bar shows the page, without the code');
  assert.match(f.content.querySelector('[data-access-outside-wait]').textContent, /확인했어요\. 모든 주소가 확인되면 바로 적용해요\./);
  assert.equal(f.content.querySelector('[name="outsideCode"]'), null);
  // Looking again does not send it again.
  f.navigate('server-access'); await f.settle();
  assert.equal(f.sent.length, 1);
});

test('the console opened at that address without the link asks for the code and sends nothing by itself; a made-up code after the page name is not a code', async t => {
  for (const hash of ['#/server-access', '#/server-access/confirm-48291', '#/server-access/confirm-4829134', '#/server-access/confirm-abcdef', '#/users/confirm-482913']) {
    const f = await consoleAt(t, 'access-confirm-' + hash.replace(/\W/g, ''), hash);
    await f.settle();
    if (f.crumb() !== '서버 접속 설정') { f.navigate('server-access'); await f.settle(); }
    assert.deepEqual(f.sent, [], hash);
    assert.match(f.content.querySelector('[data-access-outside-wait]').textContent, /이 주소가 서버로 연결돼요\.\n저장한 화면에 보이는 확인 코드 6자리를 입력하세요\./, hash);
    assert.ok(f.content.querySelector('[name="outsideCode"]'), hash);
  }
});
