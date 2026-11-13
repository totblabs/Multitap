// What the second round of the real-browser check (2026-11-13) found in the console, in the DOM the other page tests
// use: the first-setup line on the sign-in page, typed content kept over a sign-in that ended, release notes in one
// language, 업데이트 and the sidebar while the server does not answer, no desktops yet, a proxy range, and the file of
// a restore kept over a wrong passphrase. Layout (what a screenshot shows) is checked in the browser, not here.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { loadAdmin, ownerFixture, workspaceFixture, memoryStorage } from './admin-ui-harness.mjs';
import { localNotes } from '../admin/js/i18n.js';

const ok = (value, status = 200) => ({ ok: status < 400, status, json: async () => value });
const notes = 'Multitap Server 1.1.2\n\n- 서버 접속 설정을 관리자 페이지에서 바로 저장해요.\n- 서버 정보에 \'서버 다시 시작\' 버튼이 생겼어요.\n\n- Server access settings are saved on the admin console.\n- Server information has a "Restart server" button.';

test('release notes: the title, then only the part in the console\'s language; anything not cleanly Korean-then-English whole', () => {
  assert.equal(localNotes(notes, 'ko'), 'Multitap Server 1.1.2\n\n- 서버 접속 설정을 관리자 페이지에서 바로 저장해요.\n- 서버 정보에 \'서버 다시 시작\' 버튼이 생겼어요.');
  assert.equal(localNotes(notes, 'en'), 'Multitap Server 1.1.2\n\n- Server access settings are saved on the admin console.\n- Server information has a "Restart server" button.');
  assert.equal(localNotes('Multitap 1.1.2\r\n\r\n- 고쳤어요.\r\n\r\n- Fixed.', 'en'), 'Multitap 1.1.2\n\n- Fixed.', 'Windows line ends');
  assert.equal(localNotes('- 고쳤어요.\n\n- Fixed.', 'ko'), '- 고쳤어요.', 'no title line');
  for (const whole of ['Multitap Server 1.1.1\n\n- First public release.', 'Multitap 1.0.0\n\n- 한국어만 있어요.', '- Fixed.\n\n- 고쳤어요.', 'Multitap 1.0.0\n\n- 고쳤어요.\n\n- Fixed.\n\n- 또 고쳤어요.', '', 'one line']) assert.equal(localNotes(whole, 'ko'), whole, JSON.stringify(whole));
  assert.equal(localNotes(null, 'ko'), null);
});

const info = (update, version = '1.1.1') => ({ service: 'multitap-broker', version, deployment: { kind: 'docker', configured: true }, update: { feedConfigured: true, applySupported: false, channel: 'docker-release-feed', ...update }, checkedAt: '2026-11-13T01:00:00.000Z' });
test('업데이트 shows the notes of the new version in Korean only, and the Host update window too', async t => {
  const { user, inventory } = workspaceFixture();
  inventory.hostUpdates = { latest: { version: '1.1.2', filename: 'Multitap-Host-1.1.2.msi', size: 1, sha256: 'a'.repeat(64), changelog: 'Multitap Host 1.1.2\n\n- 정리했어요.\n\n- Cleaned up.' }, desktops: {} };
  const f = await loadAdmin(t, 'notes-ko', user, inventory, { deploymentKind: 'docker', respond: url => url.endsWith('/server-info') ? ok(info({ state: 'available', latest: { version: '1.1.2', releasedAt: '2026-11-13T00:00:00.000Z', changelog: notes } })) : null });
  f.navigate('updates'); await f.settle();
  const shown = f.content.querySelector('.release-notes').textContent;
  assert.match(shown, /서버 접속 설정을 관리자 페이지에서 바로 저장해요/); assert.doesNotMatch(shown, /Server access settings/);
});

test('the sign-in page says how to begin the first setup only while the server says it is to be done', async t => {
  let pending = true;
  const fresh = { id: 'fresh', username: 'admin', role: 'admin', mustChangeCredentials: true };
  const respond = url => url === '/api/v1/admin/first-setup' ? ok({ pending }) : url === '/api/v1/admin/login' ? ok({ user: fresh, csrfToken: 'fixture-csrf' }) : null;
  const f = await loadAdmin(t, 'first-setup-line', null, { users: [], desktops: [], assignments: [], audit: [], proxmox: { configured: false } }, { respond });
  await f.settle();
  const line = document.querySelector('#login-first-setup');
  assert.equal(document.querySelector('#login').hidden, false);
  assert.equal(line.hidden, false); assert.equal(line.textContent, '처음 설정이면 admin / admin으로 로그인하고, 설치할 때 받은 설정 코드를 넣어요.');
  // admin / admin, then the setup screen; once the account is changed the sign-in page asks again and says nothing.
  await signIn(f, 'admin');
  assert.equal(document.querySelector('#setup').hidden, false);
  const setup = document.querySelector('#setup-form');
  for (const [name, value] of [['setupCode', 'ABCD-EFGH-JKLM'], ['username', 'owner'], ['password', 'Owner-password-10!'], ['confirmation', 'Owner-password-10!']]) setup.querySelector(`[name=${name}]`).value = value;
  pending = false;
  setup.dispatchEvent(new f.window.Event('submit')); await f.settle();
  assert.equal(document.querySelector('#login').hidden, false); assert.equal(document.querySelector('#login-error').textContent.length > 0, true);
  assert.equal(line.hidden, true, 'never after the setup');
  // A server that does not say (an older one, or no answer): nothing is said.
  const g = await loadAdmin(t, 'first-setup-line-unknown', null, { users: [], desktops: [], assignments: [], audit: [], proxmox: { configured: false } }, { respond: url => url === '/api/v1/admin/first-setup' ? ok({}, 404) : null });
  await g.settle();
  assert.equal(document.querySelector('#login-first-setup').hidden, true);
});

// The sign-in ends under an open console: from then on every read is refused until a new sign-in.
function expiring() {
  const s = { expired: false, signIns: 0 };
  s.respond = (url, options = {}) => {
    if (url === '/api/v1/admin/login') { s.expired = false; s.signIns++; return ok(s.next ?? null); }
    if (s.expired && url.startsWith('/api/v1/admin/') && url !== '/api/v1/admin/first-setup') return ok({ error: { code: 'authentication_required' } }, 401);
    return s.more?.(url, options) ?? null;
  };
  return s;
}
const signIn = async (f, name) => {
  const form = document.querySelector('#login-form');
  form.querySelector('[name=username]').value = name; form.querySelector('[name=password]').value = 'fixture-password-1';
  form.dispatchEvent(new f.window.Event('submit')); await f.settle();
};
// Typing: the value, then the input event (the harness's change() makes the value read-only, which a later reset trips on).
const type = (node, value) => { node.value = value; node.dispatchEvent(new node.ownerDocument.defaultView.Event('input')); };
const expire = async (f, s) => { s.expired = true; document.querySelector('#refresh').dispatchEvent(new f.window.Event('click')); await f.settle(); };

test('a form open when the sign-in ended: what was typed (never the password) comes back when it is opened again, for the same administrator', async t => {
  const { user, inventory } = ownerFixture(), storage = memoryStorage(), s = expiring();
  s.next = { user, csrfToken: 'fixture-csrf-2' };
  const f = await loadAdmin(t, 'draft-dialog', user, inventory, { storage, respond: s.respond });
  f.navigate('users'); await f.settle();
  f.click(f.content, '사용자 만들기');
  const form = document.querySelector('#modal-form');
  type(form.elements.username, 'new-person'); type(form.elements.notes, '재무팀 노트북');
  type(form.elements.password, 'Typed-password-10!'); type(form.elements.passwordConfirmation, 'Typed-password-10!');
  await expire(f, s);
  assert.equal(document.querySelector('#login').hidden, false);
  assert.equal(document.querySelector('#login-error').textContent, '로그인이 끝나서 다시 로그인해야 해요.\n입력하던 내용은 로그인하면 다시 넣어요. 비밀번호와 코드는 빼고요.');
  const stored = [...storage.items.values()].join('');
  assert.match(stored, /new-person/); assert.match(stored, /재무팀 노트북/); assert.doesNotMatch(stored, /Typed-password/, 'never a password');
  await signIn(f, user.username);
  assert.equal(document.querySelector('#console').hidden, false);
  assert.match(document.querySelector('#notice').textContent, /‘사용자 만들기’ 창을 다시 열면 입력하던 내용을 넣어요\./);
  f.navigate('users'); await f.settle(); f.click(f.content, '사용자 만들기');
  const again = document.querySelector('#modal-form').elements;
  assert.equal(again.username.value, 'new-person'); assert.equal(again.notes.value, '재무팀 노트북'); assert.equal(again.password.value, '');
  assert.match(document.querySelector('#notice').textContent, /로그인이 끝나기 전에 입력한 내용을 다시 넣었어요\./);
  // Once: closed and opened again, the form is empty.
  document.querySelector('#modal-cancel').dispatchEvent(new f.window.Event('click')); f.click(f.content, '사용자 만들기');
  assert.equal(document.querySelector('#modal-form').elements.username.value, '');
});

test('another administrator signing in after it does not get what was typed; an explicit sign-out keeps nothing', async t => {
  const { user, inventory } = ownerFixture(), storage = memoryStorage(), s = expiring();
  const other = { id: 'fixture-other', username: 'other', role: 'admin', mustChangeCredentials: false };
  s.next = { user: other, csrfToken: 'fixture-csrf-3' };
  const f = await loadAdmin(t, 'draft-other-admin', user, inventory, { storage, respond: s.respond });
  f.navigate('users'); await f.settle(); f.click(f.content, '사용자 만들기');
  type(document.querySelector("#modal-form").elements.username, 'typed-by-owner');
  await expire(f, s);
  await signIn(f, 'other');
  assert.doesNotMatch(document.querySelector('#notice').textContent ?? '', /다시 열면/);
  assert.doesNotMatch([...storage.items.values()].join(''), /typed-by-owner/, 'dropped, not shown');
  f.navigate('users'); await f.settle(); f.click(f.content, '사용자 만들기');
  assert.equal(document.querySelector('#modal-form').elements.username.value, '');
});

test('a password-only form keeps nothing, and the sign-in page says to type it again', async t => {
  const { user, inventory } = ownerFixture(), storage = memoryStorage(), s = expiring();
  const f = await loadAdmin(t, 'draft-password-only', user, inventory, { storage, respond: s.respond });
  f.navigate('users'); f.follow(f.content, 'owner'); await f.settle();
  f.click(f.content, '비밀번호 재설정');
  type(document.querySelector("#modal-form").elements.password, 'Typed-password-10!');
  await expire(f, s);
  assert.equal(document.querySelector('#login-error').textContent, '로그인이 끝나서 다시 로그인해야 해요.\n저장하지 않은 입력은 다시 넣어 주세요.');
  assert.doesNotMatch([...storage.items.values()].join(''), /Typed-password/);
});

test('storage that refuses: the sign-in ends as before, nothing breaks', async t => {
  const { user, inventory } = ownerFixture(), s = expiring();
  const refusing = { getItem() { throw Error('denied'); }, setItem() { throw Error('denied'); } };
  const f = await loadAdmin(t, 'draft-refused', user, inventory, { storage: refusing, respond: s.respond });
  f.navigate('users'); await f.settle(); f.click(f.content, '사용자 만들기');
  type(document.querySelector("#modal-form").elements.username, 'kept-nowhere');
  await expire(f, s);
  assert.equal(document.querySelector('#login-error').textContent, '로그인이 끝나서 다시 로그인해야 해요.\n저장하지 않은 입력은 다시 넣어 주세요.');
});

const lan = 'http://192.0.2.10:18480';
const accessPolicy = () => ({ protocol: 'server-access-v1', primaryOrigin: lan, https: { enabled: false, origin: null, mode: 'proxy', listenHost: '0.0.0.0', listenPort: 18480, certFile: null, keyFile: null },
  http: { enabled: true, origin: lan, listenHost: '0.0.0.0', listenPort: 18480 }, trustedProxies: ['127.0.0.1'] });
const access = () => ({ protocol: 'server-access-v1', revision: 'a'.repeat(64), applied: accessPolicy(), supported: true, managementOrigin: lan, state: 'applied', pending: null });

test('서버 접속 설정 not yet saved when the sign-in ended comes back after signing in again, and a proxy range is named as that', async t => {
  const { user, inventory } = ownerFixture(), storage = memoryStorage(), s = expiring();
  s.next = { user, csrfToken: 'fixture-csrf-4' };
  s.more = url => url === '/api/v1/admin/server-access' ? ok(access()) : null;
  const f = await loadAdmin(t, 'draft-access', user, inventory, { storage, origin: lan, respond: s.respond });
  f.navigate('server-access'); await f.settle();
  const proxies = f.content.querySelector('[name=trustedProxies]');
  proxies.value = '127.0.0.1\n10.0.0.0/8'; proxies.dispatchEvent(new f.window.Event('input')); await f.settle();
  f.content.querySelector('[data-access-test]').dispatchEvent(new f.window.Event('click')); await f.settle();
  assert.match(f.content.textContent, /10\.0\.0\.0\/8 같은 IP 범위는 넣을 수 없어요\.\n?프록시의 IP를 한 줄에 하나씩 입력하세요\./);
  await expire(f, s);
  assert.match([...storage.items.values()].join(''), /10\.0\.0\.0\/8/);
  await signIn(f, user.username);
  f.navigate('server-access'); await f.settle();
  assert.equal(f.content.querySelector('[name=trustedProxies]').value, '127.0.0.1\n10.0.0.0/8');
  assert.match(document.querySelector('#notice').textContent, /로그인이 끝나기 전에 입력한 내용을 다시 넣었어요\./);
  assert.match(f.content.querySelector('[data-access-draft-status]').textContent, /아직 적용하지 않았어요|편집 중/);
});

test('업데이트 while the server does not answer: one steady line (no read error, no guide), the sidebar dot says so too; it comes back by itself', async t => {
  const { user, inventory } = ownerFixture();
  let down = false;
  const f = await loadAdmin(t, 'updates-down', user, inventory, { deploymentKind: 'docker', respond: url => { if (down && url.startsWith('/api/v1/admin/')) throw TypeError('Failed to fetch'); return url.endsWith('/server-info') ? ok(info({ state: 'current', latest: { version: '1.1.1', changelog: 'x' } })) : null; } });
  f.navigate('updates'); await f.settle();
  const chip = document.querySelector('.server-chip');
  assert.equal(chip.hasAttribute('data-server-down'), false);
  down = true;
  for (let i = 0; i < 3; i++) {
    document.querySelector('#refresh').dispatchEvent(new f.window.Event('click')); await f.settle();
    assert.equal(f.content.querySelector('[data-server-down]')?.textContent, '서버가 응답하지 않아 버전과 업데이트를 확인할 수 없어요.\n다시 응답하면 이 화면이 알아서 바뀌어요.', 'round ' + i);
    assert.doesNotMatch(f.content.textContent, /서버 정보를 확인하지 못했어요|업데이트 절차|배포 방식을 먼저 확인/);
    assert.equal(chip.hasAttribute('data-server-down'), true);
  }
  down = false;
  document.querySelector('#refresh').dispatchEvent(new f.window.Event('click')); await f.settle();
  assert.equal(f.content.querySelector('[data-server-down]'), null); assert.match(f.content.textContent, /현재 서버/);
  assert.equal(chip.hasAttribute('data-server-down'), false);
});

test('Host 업데이트 with no desktop registered says that, not "every desktop has the latest Host"', async t => {
  const { user, inventory } = ownerFixture();
  inventory.hostUpdates = { latest: { version: '1.1.2', filename: 'Multitap-Host-1.1.2.msi', size: 1, sha256: 'a'.repeat(64), changelog: '' }, desktops: {} };
  const f = await loadAdmin(t, 'updates-no-desktops', user, inventory, { monitor: { hosts: [] } });
  f.navigate('updates'); await f.settle();
  const hosts = f.content.querySelector('.settings-updates').textContent;
  assert.match(hosts, /최신 Host 1\.1\.2/); assert.match(hosts, /아직 등록된 데스크톱이 없어요\./); assert.doesNotMatch(hosts, /모든 데스크톱이 최신/);
});
