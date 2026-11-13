// The second round of the browser check (2026-11-13) in English: release notes show the English part only, the
// first-setup line, the account security dialog over http, a wrong password in the backup dialog in 기록 (Log), and
// 업데이트 (Updates) while the server does not answer. Its own process: the stored choice 'en' is set before any
// console module is imported, as in i18n-admin-en.test.mjs.
import test from 'node:test';
import assert from 'node:assert/strict';
import { parseHTML } from 'linkedom';

const stored = new Map([['multitap-lang', 'en']]);
globalThis.localStorage = { getItem: key => stored.get(key) ?? null, setItem: (key, value) => { stored.set(key, String(value)); }, removeItem: key => { stored.delete(key); } };
const { loadAdmin, ownerFixture } = await import('./admin-ui-harness.mjs');
const { localNotes, lang } = await import('../admin/js/i18n.js');
const HANGUL = /[ᄀ-ᇿ㄰-㆏가-힯]/;
const ok = (value, status = 200) => ({ ok: status < 400, status, json: async () => value });

test('release notes in an English console: the title and the English part', async t => {
  assert.equal(lang, 'en');
  const notes = 'Multitap Server 1.1.2\n\n- 서버 정보에 \'서버 다시 시작\' 버튼이 생겼어요.\n\n- Server information has a "Restart server" button.';
  assert.equal(localNotes(notes), 'Multitap Server 1.1.2\n\n- Server information has a "Restart server" button.');
  const { user, inventory } = ownerFixture();
  const f = await loadAdmin(t, 'en-notes', user, inventory, { deploymentKind: 'docker', respond: url => url.endsWith('/server-info') ? ok({ service: 'multitap-broker', version: '1.1.1', deployment: { kind: 'docker', configured: true },
    update: { feedConfigured: true, applySupported: false, channel: 'docker-release-feed', state: 'available', latest: { version: '1.1.2', releasedAt: '2026-11-13T00:00:00.000Z', changelog: notes } }, checkedAt: '2026-11-13T01:00:00.000Z' }) : null });
  f.navigate('updates'); await f.settle();
  const shown = f.content.querySelector('.release-notes').textContent;
  assert.match(shown, /Restart server/); assert.doesNotMatch(shown, HANGUL);
});

test('the first-setup line and the sign-in that ended with typed content, in English', async t => {
  const f = await loadAdmin(t, 'en-first-setup', null, { users: [], desktops: [], assignments: [], audit: [], proxmox: { configured: false } }, { respond: url => url === '/api/v1/admin/first-setup' ? ok({ pending: true }) : null });
  await f.settle();
  const line = document.querySelector('#login-first-setup');
  assert.equal(line.hidden, false); assert.equal(line.textContent, 'For the first setup, sign in as admin / admin, then enter the setup code you got when installing.');
});

test('account security over http names where to go, in English', async () => {
  const { createAccountSecurityView } = await import('../admin/js/account-security-view.js');
  const { document } = parseHTML('<!doctype html><html><body></body></html>');
  const view = createAccountSecurityView({ document, accountName: 'admin-en', onAction: () => {} });
  view.open(); view.render({ stage: 'uncertain', unavailable: true });
  assert.equal(view.element.querySelector('[data-security-https-link]').parentNode.textContent, 'Add an HTTPS address in Server access.');
  assert.equal(view.element.querySelector('[data-security-password-link]').parentNode.textContent, 'Change your password with “Reset password” in Users › admin-en.');
  assert.doesNotMatch(view.element.textContent, HANGUL);
  view.destroy();
});

test('a wrong password in the backup dialog is named in the Log, in English', async () => {
  const { auditEntry, causeCell } = await import('../admin/js/timeline.js');
  const entry = auditEntry({ id: 1, created_at: Date.now(), event: 'backup_reauthentication_failed', actor: 'u1', subject: '' }, new Map([['u1', { id: 'u1', username: 'admin-en' }]]), new Map());
  assert.equal(causeCell(entry), 'The administrator password was wrong');
});

test('Updates while the server does not answer, and a proxy range, in English', async t => {
  const { user, inventory } = ownerFixture();
  let down = false;
  const f = await loadAdmin(t, 'en-down', user, inventory, { respond: url => { if (down && url.startsWith('/api/v1/admin/')) throw TypeError('Failed to fetch'); return null; } });
  f.navigate('updates'); await f.settle();
  down = true; document.querySelector('#refresh').dispatchEvent(new f.window.Event('click')); await f.settle();
  assert.equal(f.content.querySelector('[data-server-down]').textContent, 'The server is not answering, so its version and updates cannot be checked.\nThis page changes by itself when it answers again.');
  assert.doesNotMatch(f.content.textContent, HANGUL);
  const { t: text } = await import('../admin/js/i18n.js');
  assert.equal(text('server-access.proxyRange', { value: '10.0.0.0/8' }), 'IP ranges such as 10.0.0.0/8 are not accepted.\nEnter one proxy IP per line.');
  assert.equal(text('server-access.addressExtraUnreached'), 'Not reached from the server. Check that it opens from outside.');
});
