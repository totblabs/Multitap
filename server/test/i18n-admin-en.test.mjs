// The admin console, the browser Console and the server's own pages in
// English (docs/i18n.md). This file runs in its own process: the stored choice
// below ('en') is set before any console module is imported, as a browser
// that chose English has it. The other admin tests keep the Korean source.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { parseHTML } from 'linkedom';

const stored = new Map([['multitap-lang', 'en']]);
globalThis.localStorage = { getItem: key => stored.get(key) ?? null, setItem: (key, value) => { stored.set(key, String(value)); }, removeItem: key => { stored.delete(key); } };

const { loadAdmin, registeredWorkspaceFixture, iso } = await import('./admin-ui-harness.mjs');
const i18n = await import('../admin/js/i18n.js');
const ko = (await import('../admin/i18n/ko.js')).default, en = (await import('../admin/i18n/en.js')).default;
const HANGUL = /[ᄀ-ᇿ㄰-㆏가-힯]/;
const holes = text => [...String(text).matchAll(/\{(\w+)\}/g)].map(match => match[1]).sort().join(',');

// Text a person can read: every text node and the readable attributes.
function readable(root) {
  const out = [];
  const walk = node => {
    if (node.nodeType === 3) { if (node.nodeValue.trim()) out.push(node.nodeValue.trim()); return; }
    if (node.nodeType !== 1 || ['SCRIPT', 'STYLE', 'svg', 'SVG'].includes(node.tagName)) return;
    for (const name of ['aria-label', 'title', 'placeholder', 'aria-description', 'alt', 'data-tip', 'data-label']) { const value = node.getAttribute(name); if (value) out.push(value); }
    for (const child of node.childNodes) walk(child);
  };
  walk(root);
  return out;
}
// The language switch names both languages in their own words, on purpose.
const ownNames = new Set(['한국어', '언어 / Language']);
const leftovers = (root, data = []) => readable(root).filter(text => !ownNames.has(text) && !data.includes(text) && HANGUL.test(text));
const unfilled = root => readable(root).filter(text => /\{[A-Za-z]\w*\}/.test(text));

const englishFixture = () => {
  const f = registeredWorkspaceFixture();
  f.user = { ...f.user, username: 'admin-en' };
  f.inventory.users = [{ ...f.inventory.users[0], username: 'admin-en' }, { id: 'member-en', username: 'jamie', role: 'user', disabled: 0, notes: 'Finance team' }];
  f.inventory.assignments = [{ username: 'jamie', desktop_id: 'pve-105' }, { username: 'admin-en', desktop_id: 'pve-100' }];
  f.monitor.sessions = [{ account: 'jamie', desktopId: 'pve-107', startedAt: iso(-600000), kind: 'stream' }];
  return f;
};

test('the catalogs: the same ids, English for every one, the same placeholders, no Hangul', () => {
  assert.deepEqual(Object.keys(en).sort(), Object.keys(ko).sort());
  for (const [id, value] of Object.entries(en)) {
    const forms = typeof value === 'string' ? [value] : [value.one, value.other];
    assert.equal(typeof value === 'string' || (typeof value.other === 'string'), true, id);
    for (const form of forms) assert.doesNotMatch(form, HANGUL, id);
    assert.equal(holes(typeof value === 'string' ? value : value.other), holes(ko[id]), id + ' keeps its placeholders');
  }
});

test('the language is the stored choice, else the browser\'s first language; Korean only for ko', () => {
  assert.equal(i18n.lang, 'en'); assert.equal(i18n.locale, 'en-US');
  assert.equal(i18n.chooseLanguage(null, ['ko-KR', 'en']), 'ko');
  assert.equal(i18n.chooseLanguage(null, ['en-US', 'ko-KR']), 'en');
  assert.equal(i18n.chooseLanguage(null, ['ja']), 'en');
  assert.equal(i18n.chooseLanguage(null, []), 'en');
  assert.equal(i18n.chooseLanguage('ko', ['en-US']), 'ko');
  assert.equal(i18n.chooseLanguage('fr', ['ko']), 'ko', 'an unknown stored value is ignored');
  assert.equal(i18n.t('users.25', { count: 1 }).startsWith('Its 1 assigned desktop '), true, 'a count picks the singular');
  assert.match(i18n.t('users.25', { count: 3 }), /Its 3 assigned desktops /);
});

test('sign-in and first setup screens are English, with <html lang="en"> and the switch showing English', async t => {
  const f = await loadAdmin(t, 'en-login', null, { users: [], desktops: [], assignments: [], audit: [], proxmox: { configured: false } });
  assert.equal(document.documentElement.getAttribute('lang'), 'en');
  assert.equal(document.querySelector('#login').hidden, false);
  assert.deepEqual(leftovers(document.querySelector('#login')), []);
  assert.equal(document.querySelector('title').textContent, 'Multitap admin console');
  assert.equal(document.querySelector('#login-form button[type=submit]').textContent, 'Sign in');
  const pressed = [...document.querySelectorAll('#login [data-language]')].map(node => [node.getAttribute('data-language'), node.getAttribute('aria-pressed')]);
  assert.deepEqual(pressed, [['ko', 'false'], ['en', 'true']]);
  assert.equal(document.querySelector('#login [data-language-switch]').getAttribute('aria-label'), '언어 / Language', 'its name is in both languages');
  const fresh = { id: 'fresh', username: 'admin', role: 'admin', mustChangeCredentials: true };
  const g = await loadAdmin(t, 'en-setup', fresh, { users: [], desktops: [], assignments: [], audit: [], proxmox: { configured: false } });
  await g.settle();
  const setup = document.querySelector('#setup');
  assert.equal(setup.hidden, false);
  assert.deepEqual(leftovers(setup), []);
  assert.match(setup.textContent, /setup code/i);
  assert.match(setup.querySelector('small.field-hint').innerHTML, /<code>multitap code<\/code>/, 'inline markup survives');
});

test('the main pages and windows render no Korean and no unfilled placeholder', async t => {
  const { user, inventory, monitor } = englishFixture();
  inventory.proxmox = { configured: true, state: 'ok', endpoint: 'https://pve.fixture.example:8006', tokenId: 'multitap-fixture@pve!broker', hasSecret: true, ca: '' };
  const f = await loadAdmin(t, 'en-pages', user, inventory, { monitor });
  const data = ['admin-en', 'jamie', 'Finance team'];
  const check = where => {
    assert.deepEqual(leftovers(document.body, data), [], where);
    assert.deepEqual(unfilled(document.body), [], where + ': placeholders filled');
  };
  assert.ok(f.content.textContent.length > 200, 'the home page has content'); check('home');
  for (const page of ['desktops', 'users', 'log', 'monitoring', 'notices', 'updates', 'downloads', 'server-access', 'proxmox', 'backup', 'server-info', 'licenses']) {
    f.navigate(page); await f.settle(); check(page);
  }
  f.navigate('desktops'); await f.settle();
  f.pickVm('pve-105'); await f.settle();
  assert.ok(document.querySelector('.win-frame'), 'the desktop window is open');
  check('desktop window');
  // The form table is plain English values: one state dot, the power as a word.
  const desktopFacts = Object.fromEntries([...document.querySelectorAll('.desktop-host-summary .facts > div')].map(row => [row.querySelector('dt').textContent, row.querySelector('dd')]));
  assert.ok(['On', 'Off', "Couldn't check"].includes(desktopFacts.Power.textContent), desktopFacts.Power.textContent);
  assert.equal(desktopFacts.Status.querySelectorAll('.state-dot').length, 1); assert.equal(document.querySelector('.desktop-host-summary .pill'), null);
  for (const tab of document.querySelectorAll('.win-tabs [role=tab]')) { tab.dispatchEvent(new f.window.Event('click')); await f.settle(); check('desktop window tab ' + tab.textContent); }
  f.navigate('users'); await f.settle();
  f.follow(f.content, 'jamie'); await f.settle();
  assert.ok(document.querySelector('.win-frame'), 'the user window is open');
  check('user window');
  // A member's role is "User" (not the page name "Users"), and the count of desktops is "Assigned" (not the button "Assign").
  const userFacts = Object.fromEntries([...document.querySelectorAll('.user-detail .facts > div')].map(row => [row.querySelector('dt').textContent, row.querySelector('dd').textContent]));
  assert.equal(userFacts.Role, 'User'); assert.ok('Assigned' in userFacts, Object.keys(userFacts).join());
  assert.deepEqual([...document.querySelectorAll('.user-detail .win-actions button')].map(node => node.textContent), ['Delete', 'Reset password', 'Edit info', 'Send notice', 'Disable account']);
});

test('the Proxmox step shows the setup command with --lang en', async t => {
  const { enrollmentCommand, renewalCommand, enableConsoleCommand } = await import('../admin/proxmox-shell-setup.js');
  const sha256 = 'ab'.repeat(32);
  assert.match(enrollmentCommand('https://pve-step.example', { sha256 }), /^sh -c '.+' multitap-setup 'https:\/\/pve-step\.example\/setup\/proxmox\.py' (?:ab){32} --lang en$/);
  assert.match(enrollmentCommand('https://pve-step.example', { lang: 'ko', sha256 }), / --lang ko$/);
  assert.match(renewalCommand('https://pve-step.example', 'multitap-fixture@pve!broker', 'https://pve.example:8006', { sha256 }), / --lang en --renew 'multitap-fixture@pve!broker' --endpoint 'https:\/\/pve\.example:8006'$/);
  assert.match(enableConsoleCommand('https://pve-step.example', 'multitap-fixture@pve!broker', { sha256 }), / --lang en --enable-console 'multitap-fixture@pve!broker'$/);
  const f = await loadAdmin(t, 'en-proxmox', { id: 'p', username: 'owner', role: 'admin', mustChangeCredentials: false }, { users: [{ id: 'p', username: 'owner', role: 'admin', disabled: 0, notes: '' }], desktops: [], assignments: [], audit: [], proxmox: { configured: false, setupScriptSha256: sha256 } });
  f.navigate('proxmox'); await f.settle();
  const commands = [...document.querySelectorAll('.shell-command code')].map(node => node.textContent);
  assert.ok(commands.some(command => command.endsWith(sha256 + ' --lang en')), JSON.stringify(commands));
  assert.deepEqual(leftovers(document.body, ['owner']), []);
});

test('the switch stores the choice, sets <html lang> and reloads; storage refused keeps working', async t => {
  const f = await loadAdmin(t, 'en-switch', { id: 'p', username: 'owner', role: 'admin', mustChangeCredentials: false }, { users: [{ id: 'p', username: 'owner', role: 'admin', disabled: 0, notes: '' }], desktops: [], assignments: [], audit: [], proxmox: { configured: true, state: 'ok' } });
  let reloads = 0; globalThis.location.reload = () => { reloads++; };
  const korean = document.querySelector('.topbar [data-language="ko"]');
  assert.ok(korean, 'the top bar has the switch'); assert.equal(korean.textContent, '한국어'); assert.equal(korean.getAttribute('lang'), 'ko');
  korean.dispatchEvent(new f.window.Event('click'));
  assert.equal(stored.get('multitap-lang'), 'ko'); assert.equal(document.documentElement.getAttribute('lang'), 'ko'); assert.equal(reloads, 1);
  document.querySelector('.topbar [data-language="en"]').dispatchEvent(new f.window.Event('click'));
  assert.equal(stored.get('multitap-lang'), 'en'); assert.equal(document.documentElement.getAttribute('lang'), 'en'); assert.equal(reloads, 1, 'the language shown needs no reload');
  const saved = globalThis.localStorage;
  globalThis.localStorage = { getItem() { throw Error('denied'); }, setItem() { throw Error('denied'); } };
  try { assert.equal(i18n.chooseStoredLanguage('ko'), true); assert.equal(reloads, 2); } finally { globalThis.localStorage = saved; stored.set('multitap-lang', 'en'); }
});

test('the browser Console page and its messages are English', async () => {
  const consoleText = await import('../admin/console/i18n.js');
  assert.equal(consoleText.lang, 'en');
  const { document } = parseHTML(await readFile(new URL('../admin/console/index.html', import.meta.url), 'utf8'));
  consoleText.translatePage(document);
  assert.equal(document.documentElement.getAttribute('lang'), 'en');
  assert.deepEqual(leftovers(document.body), []);
  assert.equal(document.querySelector('title').textContent, 'Multitap · Console');
  assert.equal(consoleText.t('console.console-auth.13', { count: 1, String: '05' }), 'Time left 1:05');
  const source = await readFile(new URL('../admin/console/i18n.js', import.meta.url), 'utf8');
  const tables = await import('data:text/javascript;base64,' + Buffer.from(source.replace(/^const table = catalogs\[lang\];$/m, 'const table = catalogs[lang]; export { catalogs };')).toString('base64'));
  assert.deepEqual(Object.keys(tables.catalogs.en), Object.keys(tables.catalogs.ko));
  for (const [id, value] of Object.entries(tables.catalogs.en)) { assert.doesNotMatch(value, HANGUL, id); assert.equal(holes(value), holes(tables.catalogs.ko[id]), id); }
});

test('the server\'s pages follow Accept-Language: Korean first is Korean, anything else English', async () => {
  const { requestLanguage } = await import('../src/i18n.mjs');
  assert.equal(requestLanguage('ko-KR,ko;q=0.9,en-US;q=0.8'), 'ko');
  assert.equal(requestLanguage('en-US,en;q=0.9,ko;q=0.8'), 'en');
  assert.equal(requestLanguage('en;q=0.5,ko;q=0.9'), 'ko', 'the highest quality wins');
  assert.equal(requestLanguage(''), 'en'); assert.equal(requestLanguage(undefined), 'en'); assert.equal(requestLanguage('*'), 'en'); assert.equal(requestLanguage('ko;q=0'), 'en');
  const { renderLanding, renderDownloadPage, renderMissingRelease } = await import('../src/landing.mjs');
  const release = { product: 'client', version: '1.0.0', filename: 'Multitap-Client-1.0.0.exe', url: '/downloads/Multitap-Client-1.0.0.exe', format: 'installer', size: 52428800, sha256: 'a'.repeat(64), releasedAt: '2026-11-01T00:00:00Z' };
  for (const html of [renderLanding({ version: '1.0.0', publicOrigin: 'https://en.example', releases: [release], userAgent: 'iPhone', lang: 'en' }), renderDownloadPage({ version: '1.0.0', publicOrigin: 'http://192.0.2.1:18480', releases: [], userAgent: 'Linux', lang: 'en' }), renderMissingRelease({ version: '1.0.0', key: 'host', lang: 'en' })]) {
    const { document } = parseHTML(html);
    assert.equal(document.documentElement.getAttribute('lang'), 'en');
    assert.deepEqual(leftovers(document), []);
  }
  assert.match(renderLanding({ version: '1.0.0', publicOrigin: 'https://en.example', releases: [release] }), /<html lang="ko">/, 'Korean stays the default of the renderer');
  // The plain download page in English points at the English pages of multitap.me.
  const { document } = parseHTML(renderLanding({ version: '1.0.0', publicOrigin: 'https://en.example', releases: [release], lang: 'en' }));
  assert.equal(document.querySelector('h1').textContent, 'Get the Multitap apps');
  assert.equal(document.querySelector('.dl-help').textContent, 'Windows warning while installing? Install help');
  assert.equal(document.querySelector('.dl-help a').getAttribute('href'), 'https://multitap.me/en/install#edge');
  assert.equal(document.querySelector('.site-foot-about a').getAttribute('href'), 'https://multitap.me/en');
});
