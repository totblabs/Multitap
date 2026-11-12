// Static checks of the shipped console page; the behaviour itself is covered
// by tools/test-browser-console.mjs in a headless browser.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { parseHTML } from 'linkedom';

const read = name => readFile(new URL('../admin/console/' + name, import.meta.url), 'utf8');
const html = await read('index.html'), script = await read('console.js');
const { document } = parseHTML(html);

test('every console command is an icon button with a name and a tooltip, and every icon exists', () => {
  const tools = [...document.querySelectorAll('.tools .tool')];
  assert.ok(tools.length >= 9);
  for (const tool of tools) {
    assert.ok(tool.getAttribute('aria-label')?.trim(), tool.id);
    assert.ok(tool.getAttribute('data-tip')?.trim(), tool.id);
    assert.equal(tool.textContent.trim(), '', `${tool.id} shows an icon, not a text label`);
  }
  const symbols = new Set([...document.querySelectorAll('symbol')].map(symbol => symbol.id));
  for (const use of document.querySelectorAll('use')) assert.ok(symbols.has(use.getAttribute('href').slice(1)), use.getAttribute('href'));
  for (const icon of script.matchAll(/(?:progress|wait|error|ended|lock):'([a-z]+)'/g)) assert.ok(symbols.has('i-' + icon[1]), icon[1]);
});

test('the app window hides logout, new window, theme and the wordmark; the browser keeps them', () => {
  const browserOnly = id => !!document.getElementById(id).closest('.browser-only');
  for (const id of ['logout', 'new-window', 'theme']) assert.ok(browserOnly(id), id);
  assert.ok(document.querySelector('.brand').classList.contains('browser-only'));
  for (const id of ['reconnect', 'send-cad', 'clipboard-open', 'fullscreen', 'help-open']) assert.equal(browserOnly(id), false, id);
  assert.match(script, /document\.body\.classList\.add\(embeddedConsole\?'embedded':'browser'\)/);
});

test('the status card carries its own retry and folds details behind 자세히', () => {
  const card = document.getElementById('notice');
  assert.ok(card.querySelector('#retry.primary'));
  assert.equal(card.querySelector('details#notice-more > summary').textContent, '자세히');
  assert.ok(card.querySelector('.card-icon .spinner'));
});

test('the close question is short and answers the app only with fixed strings', () => {
  const dialog = document.getElementById('close-dialog');
  assert.equal(dialog.querySelector('h2').textContent, '콘솔을 닫을까요?');
  assert.equal(dialog.querySelector('p').textContent, '데스크톱과 작업은 그대로 유지돼요.');
  assert.deepEqual([...dialog.querySelectorAll('button')].map(button => [button.id, button.textContent]), [['close-cancel', '취소'], ['close-confirm', '닫기']]);
  assert.ok(dialog.querySelector('#close-confirm').classList.contains('primary'));
  const strings = [...new Set(script.match(/multitap-console:[a-z-]+/g))].sort();
  assert.deepEqual(strings, ['multitap-console:close', 'multitap-console:close-cancel', 'multitap-console:close-request', 'multitap-console:close-shown', 'multitap-console:ready']);
  // Only the app's exact request is acted on, and only in the app window.
  assert.match(script, /if\(event\.data!=='multitap-console:close-request'\)return;/);
  assert.match(script, /const appWindow ?= ?embeddedConsole&&/);
  // 전체 화면 only where the window can fill the screen for the page.
  assert.match(script, /const fullscreenWorks ?= ?!embeddedConsole\|\|!!appWindow;/);
});

test('the page stays inside its CSP: no inline script or event handler attributes', () => {
  assert.equal(document.querySelectorAll('script:not([src])').length, 0);
  for (const node of document.querySelectorAll('*')) for (const { name } of node.attributes) assert.ok(!/^on/i.test(name), `${node.tagName} ${name}`);
});

test('every console error code keeps its message', () => {
  const table = script.slice(script.indexOf('const errors = {'), script.indexOf('\n};', script.indexOf('const errors = {')));
  const codes = new Set([...table.matchAll(/^\s+([a-z_]+):/gm)].map(match => match[1]));
  for (const code of ['invalid_credentials', 'login_failed', 'rate_limited', 'too_many_attempts', 'login_busy', 'invalid_credentials_format',
    'credentials_change_required', 'csrf_required', 'desktop_forbidden', 'desktop_not_found', 'forbidden', 'console_preparing', 'console_cleanup_pending',
    'console_busy', 'console_unavailable', 'console_timeout', 'console_protocol_error', 'console_disabled', 'console_limit_reached',
    'console_permission_denied', 'additional_authentication', 'cleanup_failed', 'logout_failed', 'transport_closed', 'transport_refused', 'authorization_revoked', 'server_stopping', 'protocol_error',
    ...['inactive', 'state_missing', 'state_invalid', 'identity_changed', 'foreign_changed', 'unavailable', 'cancelled', 'busy', 'timeout'].map(reason => 'console_display_' + reason)])
    assert.ok(codes.has(code), code);
});
