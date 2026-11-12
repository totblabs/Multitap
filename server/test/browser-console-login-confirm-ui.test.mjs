// console.js text comes from console/i18n.js (docs/i18n.md); its imports are removed below, so the context gets the same functions.
import { t, translatePage } from '../admin/console/i18n.js';
import { createConsoleAuthentication } from '../admin/console/console-auth.js';
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { runInNewContext } from 'node:vm';
import { parseHTML } from 'linkedom';

const html = await readFile(new URL('../admin/console/index.html', import.meta.url), 'utf8');
const script = (await readFile(new URL('../admin/console/console.js', import.meta.url), 'utf8'))
  .replace(/^import .+;\r?\n/gm, '').replace(/\bboot\(\);\s*$/, '');
const token = 'C'.repeat(43);
const challenge = (details = {}) => ({ status: 409, body: { error: { code: 'login_confirmation_required', details: { protocol: 'session-access-v1', challengeToken: token, expiresInMs: 120000, ...details } } } });
const session = { status: 200, body: { csrfToken: 'X'.repeat(43), user: { username: 'fixture-user' } } };
const tick = () => new Promise(resolve => setImmediate(resolve));

function fixture(responses) {
  const { document, window: dom } = parseHTML(html), window = new EventTarget();
  const requests = [], timers = new Map(); let nextTimer = 0;
  for (const dialog of document.querySelectorAll('dialog')) {
    dialog.open = false;
    dialog.showModal = () => { dialog.open = true; };
    dialog.close = () => { dialog.open = false; dialog.dispatchEvent(new dom.Event('close')); };
  }
  const fetch = async (path, options) => {
    requests.push({ path, ...options, body: options.body ? JSON.parse(options.body) : undefined });
    const next = responses.shift(); assert.ok(next, `unexpected request ${path}`);
    const response = typeof next === 'function' ? await next() : next;
    return { status: response.status, ok: response.status >= 200 && response.status < 300, json: async () => response.body };
  };
  runInNewContext(script, {
    document, window, location: { pathname: '/console/pve-170', origin: 'https://fixture.invalid', protocol: 'https:', hostname: 'fixture.invalid' },
    navigator: { userAgent: 'Fixture browser' }, matchMedia: () => ({ matches: false, addEventListener() {} }),
    fetch, AbortSignal, URL, createConsoleAuthentication, initLogging() {}, releaseCapture() {}, createHangulKeyAdapter: () => ({ reset() {} }), t, translatePage,
    setTimeout: fn => { const id = ++nextTimer; timers.set(id, fn); return id; }, clearTimeout: id => timers.delete(id),
  });
  const $ = id => document.getElementById(id);
  const submit = () => { $('username').value = 'fixture-user'; $('password').value = 'synthetic-password'; $('login-form').dispatchEvent(new dom.Event('submit', { cancelable: true })); };
  const click = id => $(id).dispatchEvent(new dom.Event('click', { cancelable: true }));
  return { $, submit, click, requests, timers, window, dom };
}

test('duplicate login waits for explicit consent; cancel and Escape send no confirmation', async () => {
  for (const escape of [false, true]) {
    const f = fixture([challenge()]); f.submit(); await tick();
    assert.equal(f.$('login-confirm-dialog').open, true); assert.equal(f.$('password').value, '');
    if (escape) f.$('login-confirm-dialog').dispatchEvent(new f.dom.Event('cancel', { cancelable: true })); else f.click('login-confirm-cancel');
    await tick(); assert.equal(f.requests.length, 1); assert.equal(f.$('login-submit').disabled, false);
    assert.equal(f.$('login-confirm-dialog').open, false); assert.equal(f.timers.size, 0);
  }
});

test('explicit confirmation is single use, password free, cookie scoped and then connects', async () => {
  const f = fixture([challenge(), session, { status: 202, body: { code: 'console_preparing' } }]);
  f.submit(); await tick(); f.click('login-confirm-accept'); f.click('login-confirm-accept'); await tick();
  assert.deepEqual(f.requests.map(r => r.path), ['/console/api/login', '/console/api/login/confirm', '/console/api/desktops/pve-170/connect']);
  assert.deepEqual(f.requests[1].body, { challengeToken: token, confirm: true });
  assert.equal(f.requests[1].credentials, 'same-origin'); assert.equal(f.requests[1].headers.Authorization, undefined);
  assert.equal(f.requests[2].headers['X-Multitap-Console-CSRF'], session.body.csrfToken);
  assert.equal(f.$('password').value, ''); assert.equal(f.$('login-panel').hidden, true);
});

test('malformed or unsupported challenges never show a takeover question or send confirmation', async () => {
  for (const details of [{ protocol: 'other' }, { challengeToken: 'short' }, { expiresInMs: 0 }, { expiresInMs: 120001 }, { expiresInMs: 1.5 }]) {
    const f = fixture([challenge(details)]); f.submit(); await tick();
    assert.equal(f.$('login-confirm-dialog').open, false); assert.equal(f.requests.length, 1); assert.equal(f.$('login-submit').disabled, false);
  }
});

test('expiry closes the question, explains the cause and cannot submit a stale challenge', async () => {
  const f = fixture([challenge()]); f.submit(); await tick();
  [...f.timers.values()][0](); await tick(); f.click('login-confirm-accept'); await tick();
  assert.equal(f.requests.length, 1); assert.equal(f.$('login-confirm-dialog').open, false);
  assert.match(f.$('login-message').textContent, /로그인 확인 시간이 끝났어요/);
});

test('leaving the page while a question is open cancels its authority', async () => {
  const f = fixture([challenge()]); f.submit(); await tick();
  f.window.dispatchEvent(new Event('pagehide')); f.click('login-confirm-accept'); await tick();
  assert.equal(f.requests.length, 1); assert.equal(f.$('login-confirm-dialog').open, false); assert.equal(f.timers.size, 0);
});

test('late confirmation response after page exit cannot open a remote connection', async () => {
  let finish;
  const f = fixture([challenge(), () => new Promise(resolve => { finish = resolve; }), { status: 204 }]);
  f.submit(); await tick(); f.click('login-confirm-accept'); await tick();
  f.window.dispatchEvent(new Event('pagehide')); finish(session); await tick();
  assert.deepEqual(f.requests.map(r => r.path), ['/console/api/login', '/console/api/login/confirm', '/console/api/logout']);
  assert.equal(f.requests[2].headers['X-Multitap-Console-CSRF'], session.body.csrfToken);
  assert.equal(f.requests.some(r => r.path.endsWith('/connect')), false);
  assert.equal(f.$('logout').hidden, true);
});

test('an ordinary first login connects without a confirmation request', async () => {
  const f = fixture([session, { status: 202, body: { code: 'console_preparing' } }]); f.submit(); await tick();
  assert.deepEqual(f.requests.map(r => r.path), ['/console/api/login', '/console/api/desktops/pve-170/connect']);
  assert.equal(f.$('login-confirm-dialog').open, false);
});

test('server rejected confirmation is explained without credential replay', async () => {
  const f = fixture([challenge(), { status: 409, body: { error: { code: 'login_confirmation_changed' } } }]);
  f.submit(); await tick(); f.click('login-confirm-accept'); await tick();
  assert.equal(f.requests.length, 2); assert.match(f.$('login-message').textContent, /로그인 상태가 바뀌었어요/);
  assert.equal(f.$('password').value, ''); assert.equal(f.$('login-submit').disabled, false);
});
