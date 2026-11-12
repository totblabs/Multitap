// The Console page's cards for what really happens around a connection: the shipped
// page run in a DOM with noVNC, the socket, fetch and the timers replaced. No server, no Proxmox.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { runInNewContext } from 'node:vm';
import { parseHTML } from 'linkedom';
import { t, translatePage } from '../admin/console/i18n.js';
import { createConsoleAuthentication } from '../admin/console/console-auth.js';
import { createAccountSecurityController } from '../admin/js/account-security-controller.js';
import { createAccountSecurityView } from '../admin/js/account-security-view.js';

const html = await readFile(new URL('../admin/console/index.html', import.meta.url), 'utf8');
const script = (await readFile(new URL('../admin/console/console.js', import.meta.url), 'utf8')).replace(/^import .+;\r?\n/gm, '').replace(/\bboot\(\);\s*$/, '')
  .replace(/import\('\/admin\/js\/account-security-controller\.js'\),import\('\/admin\/js\/account-security-view\.js'\),/, '{createAccountSecurityController},{createAccountSecurityView},');
const tick = async (turns = 8) => { for (let i = 0; i < turns; i++) await new Promise(resolve => setImmediate(resolve)); };
const id = '0d4b3f0e-6a52-4a7e-9a39-1f3c55d1a0b2';
const granted = extra => ({ status: 200, body: { id, wsPath: '/console/ws/' + id, expiresAt: new Date(Date.now() + 30000).toISOString(), desktop: { id: 'pve-170', name: '업무용' },
  clipboardPolicy: { textToRemote: false, textToLocal: false }, qemuConsolePreparation: { status: 'isolated' }, ...extra } });
const refused = (code, status = 409) => ({ status, body: { error: { code } } });
const cleaning = { status: 202, body: { code: 'console_cleanup_pending' } };

// `answers`: what POST …/connect answers, in order (the sign-in before it is answered here).
function consolePage(answers) {
  const { document, window: dom } = parseHTML(html), window = new EventTarget();
  const timers = new Map(), instances = [], sockets = [], requests = []; let sequence = 0;
  for (const node of document.querySelectorAll('dialog')) { node.open = false; node.showModal = () => { node.open = true; }; node.close = () => { node.open = false; }; }
  const fetch = async (url, options = {}) => {
    requests.push((options.method ?? 'GET') + ' ' + url + (options.body ? ' ' + options.body : ''));
    const next = url.endsWith('/login') ? { status: 200, body: { csrfToken: 'S'.repeat(43), user: { username: 'fixture-user' } } }
      : options.method === 'DELETE' ? { status: 204 } : url.endsWith('/connect') ? answers.shift() : { status: 200, body: { qemuConsolePreparation: { status: 'isolated' } } };
    assert.ok(next, 'unexpected request ' + url);
    return { status: next.status, ok: next.status < 400, json: async () => next.body };
  };
  class RFB extends EventTarget { constructor() { super(); instances.push(this); } disconnect() {} focus() {} blur() {} getImageData() { return { data: [] }; } }
  class WebSocket extends EventTarget { constructor() { super(); sockets.push(this); } close() {} }
  const schedule = (fn, ms) => { const key = ++sequence; timers.set(key, { fn, ms }); return key; };
  const context = { createAccountSecurityController, createAccountSecurityView, document, window, location: { pathname: '/console/pve-170', origin: 'https://fixture.invalid', protocol: 'https:', hostname: 'fixture.invalid' }, navigator: { userAgent: 'Fixture browser' },
    matchMedia: () => ({ matches: false, addEventListener() {} }), fetch, AbortSignal, URL, RFB, WebSocket, initLogging() {}, releaseCapture() {}, createHangulKeyAdapter: () => ({ reset() {} }), t, translatePage,
    setTimeout: schedule, clearTimeout: key => timers.delete(key),
    createConsoleAuthentication: options => createConsoleAuthentication({ ...options, schedule, cancel: key => timers.delete(key) }) };
  runInNewContext(script, context);
  const $ = name => document.getElementById(name);
  // Runs the pending timers of exactly this delay (as the clock would), then lets the page settle.
  const after = async ms => { for (const [key, timer] of [...timers]) if (timer.ms === ms) { timers.delete(key); timer.fn(); } await tick(); };
  return { $, instances, sockets, requests, timers, after, document,
    card: () => ({ title: $('notice-title').textContent, text: $('notice-text').textContent, tone: $('notice').dataset.tone, hidden: $('notice').hidden, retry: !$('retry').hidden, takeover: !$('takeover').hidden, code: $('notice-code').hidden ? '' : $('notice-code').textContent }),
    press(name) { $(name).dispatchEvent(new dom.Event('click')); },
    async signIn() { $('username').value = 'fixture-user'; $('password').value = 'synthetic-password'; $('login-form').dispatchEvent(new dom.Event('submit', { cancelable: true })); await tick(); },
    // The server's end of the socket: its close reason, then noVNC's own disconnect.
    close(reason = '') { const event = new Event('close'); event.reason = reason; sockets.at(-1).dispatchEvent(event); instances.at(-1).dispatchEvent(new Event('disconnect')); } };
}

test('a code the page has no words for is still shown to pass on to the administrator', async () => {
  const page = consolePage([refused('console_identity_unavailable')]);
  await page.signIn();
  assert.deepEqual(page.card(), { title: '연결하지 못했어요', text: '서버 연결 상태를 확인한 뒤 다시 시도하세요.', tone: 'error', hidden: false, retry: true, takeover: false, code: '오류 코드 console_identity_unavailable' });
  // Never a sentence: only a fixed code shape is shown.
  const odd = consolePage([refused('Something went wrong <b>here</b>')]);
  await odd.signIn();
  assert.equal(odd.card().code, '');
});

test('a socket that closes before the Console answered is a failure with its code, not a plain end', async () => {
  const page = consolePage([granted()]);
  await page.signIn();
  assert.equal(page.instances.length, 1);
  page.close();
  assert.deepEqual(page.card(), { title: '콘솔에 연결하지 못했어요', text: '서버와 콘솔 연결이 열리지 않았어요. 다시 연결해 보고, 계속되면 관리자에게 알려 주세요.', tone: 'error', hidden: false, retry: true, takeover: false, code: '오류 코드 transport_refused' });
});

test('after the Console answered, a plain close is an end; a lost assignment and a stopping server say so', async () => {
  const plain = consolePage([granted()]);
  await plain.signIn(); plain.instances[0].dispatchEvent(new Event('connect')); assert.equal(plain.card().hidden, true);
  plain.close();
  assert.deepEqual(plain.card(), { title: '콘솔 연결이 끝났어요', text: '데스크톱과 작업은 유지돼요. 다시 연결할 수 있어요.', tone: 'ended', hidden: false, retry: true, takeover: false, code: '' });
  const revoked = consolePage([granted()]);
  await revoked.signIn(); revoked.instances[0].dispatchEvent(new Event('connect')); revoked.close('authorization_revoked');
  assert.deepEqual(revoked.card(), { title: '이 데스크톱에 연결할 수 없어요', text: '관리자에게 데스크톱 배정 확인을 요청하세요.', tone: 'lock', hidden: false, retry: true, takeover: false, code: '오류 코드 authorization_revoked' });
  const stopping = consolePage([granted()]);
  await stopping.signIn(); stopping.instances[0].dispatchEvent(new Event('connect')); stopping.close('server_stopping');
  assert.deepEqual(stopping.card(), { title: '서버가 멈추거나 다시 시작하고 있어요', text: '잠시 후 다시 연결하세요.', tone: 'wait', hidden: false, retry: true, takeover: false, code: '오류 코드 server_stopping' });
});

test('스트리밍 끊고 연결 keeps one card while it asks again, then connects', async () => {
  const page = consolePage([refused('console_busy'), cleaning, cleaning, cleaning, granted()]);
  await page.signIn();
  assert.equal(page.card().takeover, true);
  page.press('takeover'); await tick();
  const waiting = { title: '이전 연결을 정리하고 있어요', text: '끝나면 바로 연결해요.', tone: 'progress', hidden: false, retry: false, takeover: false, code: '' };
  assert.deepEqual(page.card(), waiting);
  // Each second it asks again; the card does not change to 연결 중 and back in between.
  const seen = [];
  const observer = () => seen.push(page.$('notice-title').textContent);
  for (let n = 0; n < 2; n++) {
    for (const [key, timer] of [...page.timers]) if (timer.ms === 1000) { page.timers.delete(key); timer.fn(); observer(); }
    await tick(); observer();
    assert.deepEqual(page.card(), waiting, 'retry ' + n);
  }
  assert.deepEqual([...new Set(seen)], ['이전 연결을 정리하고 있어요']);
  await page.after(1000);
  assert.deepEqual(page.card(), { title: '콘솔에 연결 중', text: '콘솔 화면을 준비하고 있어요.', tone: 'progress', hidden: false, retry: false, takeover: false, code: '' });
  assert.equal(page.instances.length, 1);
  assert.deepEqual(page.requests.filter(line => line.includes('/connect')).map(line => line.split(' ').at(-1)), ['{}', '{"takeover":true}', '{}', '{}', '{}']);
});

test('without the takeover a cleanup answer is the waiting card with 다시 연결, as before', async () => {
  const page = consolePage([cleaning]);
  await page.signIn();
  assert.deepEqual(page.card(), { title: '이전 연결을 정리하고 있어요', text: '잠시 후 다시 연결하세요.', tone: 'wait', hidden: false, retry: true, takeover: false, code: '오류 코드 console_cleanup_pending' });
});

test('the sign-in screen hint goes away after 20 seconds', async () => {
  const page = consolePage([granted({ consoleHint: 'signin_other_display' })]);
  await page.signIn(); page.instances[0].dispatchEvent(new Event('connect'));
  assert.equal(page.$('frame-note').hidden, false);
  await page.after(20000);
  assert.equal(page.$('frame-note').hidden, true);
  // A later check of the picture does not bring it back (only the black-screen line may come, after its own wait).
  await page.after(1000); await page.after(5000);
  assert.equal(page.$('frame-note').hidden, true);
});
