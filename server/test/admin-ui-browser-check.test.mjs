// What the browser check of 2026-11-13 found in the whole console (app.js, js/system.js), in the DOM the other page
// tests use: a server newer than the newest public release, the console opened by hand at an address a save is waiting
// to have proven, and the toast that must not swallow a click meant for the button it covers.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { loadAdmin, ownerFixture, workspaceFixture } from './admin-ui-harness.mjs';
import { readFile } from 'node:fs/promises';
import { newerVersion } from '../admin/js/system.js';

const ok = value => ({ ok: true, status: 200, json: async () => value });
const info = (version, update) => ({ service: 'multitap-broker', version, deployment: { kind: 'docker', configured: true }, update: { feedConfigured: true, applySupported: false, channel: 'docker-release-feed', ...update }, checkedAt: '2026-11-13T01:00:00.000Z' });

test('a version is compared by its numbers', () => {
  assert.equal(newerVersion('1.1.2', '1.1.1'), true); assert.equal(newerVersion('1.10.0', '1.9.9'), true); assert.equal(newerVersion('2.0.0', '1.99.99'), true);
  assert.equal(newerVersion('1.1.1', '1.1.1'), false); assert.equal(newerVersion('1.1.1', '1.1.2'), false);
  assert.equal(newerVersion('1.1', '1.0.0'), false); assert.equal(newerVersion('1.1.x', '1.0.0'), false); assert.equal(newerVersion(undefined, '1.0.0'), false);
});

test('업데이트 on a server that runs a newer release than the newest public one says so, and does not call the older release "latest"', async t => {
  const { user, inventory } = workspaceFixture();
  const latest = { version: '1.1.1', releasedAt: '2026-11-12T13:01:00.000Z', changelog: 'Multitap Server 1.1.1\n\n- 첫 공개 버전이에요.' };
  const f = await loadAdmin(t, 'update-ahead', user, inventory, { deploymentKind: 'docker', respond: url => url.endsWith('/server-info') ? ok(info('1.1.2', { state: 'current', latest })) : null });
  f.navigate('updates'); await f.settle();
  const card = f.content.querySelector('.server-release');
  assert.match(card.querySelector('h2, h3, .panel-head').textContent, /공개된 최신 버전 1\.1\.1/);
  assert.equal(card.querySelector('[data-server-ahead]').textContent, '이 서버(1.1.2)는 공개된 최신 버전(1.1.1)보다 새 버전이에요.');
  assert.doesNotMatch(card.textContent, /지금 최신 버전이에요/);
});

test('업데이트 on the newest public release still says it is the latest', async t => {
  const { user, inventory } = workspaceFixture();
  const latest = { version: '1.1.1', releasedAt: '2026-11-12T13:01:00.000Z', changelog: 'Multitap Server 1.1.1' };
  const f = await loadAdmin(t, 'update-current', user, inventory, { deploymentKind: 'docker', respond: url => url.endsWith('/server-info') ? ok(info('1.1.1', { state: 'current', latest })) : null });
  f.navigate('updates'); await f.settle();
  const card = f.content.querySelector('.server-release');
  assert.match(card.textContent, /지금 최신 버전이에요/); assert.equal(card.querySelector('[data-server-ahead]'), null);
  assert.match(card.querySelector('h2, h3, .panel-head').textContent, /버전 1\.1\.1/); assert.doesNotMatch(card.textContent, /공개된 최신 버전/);
});

// The console opened by its plain address (typed on a phone: /admin/ with no page and no code) at an address a save of
// 서버 접속 설정 is waiting to have proven: the page that asks for the code opens by itself.
const here = 'https://vdi.example.com', lan = 'http://192.0.2.10:18480', operationId = '11111111-2222-4333-8444-555555555555';
const policy = https => ({ protocol: 'server-access-v1', primaryOrigin: lan, https: { enabled: https, origin: https ? here : null, mode: 'proxy', listenHost: '0.0.0.0', listenPort: 18480, certFile: null, keyFile: null },
  http: { enabled: true, origin: lan, listenHost: '0.0.0.0', listenPort: 18480 }, trustedProxies: ['127.0.0.1'] });
const waiting = { protocol: 'server-access-v1', revision: 'a'.repeat(64), applied: policy(false), supported: true, managementOrigin: here, state: 'awaiting_confirmation', operationId,
  pending: { config: policy(true), state: 'awaiting_confirmation', operationId }, outside: { origins: [{ origin: here, confirmed: false }], remainingMs: 240000, windowMs: 300000 } };
const settled = { protocol: 'server-access-v1', revision: 'a'.repeat(64), applied: policy(true), supported: true, managementOrigin: here, state: 'applied', operationId, pending: null };

test('signed in at an address under proof without the link: 서버 접속 설정 opens with the code field, and nothing is sent by itself', async t => {
  const { user, inventory } = ownerFixture();
  const f = await loadAdmin(t, 'proof-typed-address', user, inventory, { origin: here, respond: url => url === '/api/v1/admin/server-access' ? ok(waiting) : null });
  await f.settle();
  assert.equal(f.crumb(), '서버 접속 설정');
  assert.ok(f.content.querySelector('[name="outsideCode"]'), 'the code is asked for');
  assert.deepEqual(f.writes, []);
});

test('signed in at an ordinary address: the console opens on 홈 as before', async t => {
  const { user, inventory } = ownerFixture();
  const f = await loadAdmin(t, 'proof-ordinary-address', user, inventory, { origin: here, respond: url => url === '/api/v1/admin/server-access' ? ok(settled) : null });
  await f.settle();
  assert.equal(f.crumb(), '홈');
});

test('a toast that goes away by itself lets the pointer through to the button it covers (only its close button takes the pointer)', async t => {
  const { user, inventory } = ownerFixture();
  const f = await loadAdmin(t, 'toast-passing', user, inventory, {});
  await f.settle();
  const css = await readFile(new URL('../admin/console.css', import.meta.url), 'utf8');
  assert.ok(css.includes('#notice[data-passing]{pointer-events:none}')); assert.ok(css.includes('#notice[data-passing] .notice-close{pointer-events:auto}'));
  const notice = document.querySelector('#notice');
  f.navigate('users'); f.follow(f.content, 'owner'); await f.settle();
  f.click(f.content, '비밀번호 재설정');
  const form = document.querySelector('#modal-form');
  // Refused by the page itself (the two do not match): the error is in the dialog, and no passing toast is left behind.
  form.elements.password.value = 'Design-pass-password-10!'; form.elements.passwordConfirmation.value = 'another-password-10!';
  await form.onsubmit({ preventDefault() {} }); await f.settle();
  assert.equal(notice.hasAttribute('data-passing'), false);
  form.elements.password.value = form.elements.passwordConfirmation.value = 'Design-pass-password-10!';
  await form.onsubmit({ preventDefault() {} }); await f.settle();
  assert.equal(notice.hidden, false); assert.equal(notice.getAttribute('class'), 'is-success'); assert.equal(notice.hasAttribute('data-passing'), true);
});
