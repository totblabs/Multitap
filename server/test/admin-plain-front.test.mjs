import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { parseHTML } from 'linkedom';
import ko from '../admin/i18n/ko.js';
import en from '../admin/i18n/en.js';
import { renderLanding } from '../src/landing.mjs';
import { productsFor } from '../admin/download-choices.js';

// A plain front: the sign-in screen has no slogan, first setup no greeting, and the download
// cards (the console's 다운로드 and the server's own page) no eyebrow, icon tile or highlighted first card.
const index = async () => parseHTML(await readFile(new URL('../admin/index.html', import.meta.url), 'utf8')).document;

test('sign-in and first setup: one centred card with the mark and the title in its head, nothing beside it, no slogan', async () => {
  const document = await index();
  // Nothing stands beside the card (the slogan's old place is gone with it), and the mark is not repeated in the header.
  assert.equal(document.querySelector('.auth-intro,.auth-steps'), null);
  for (const id of ['login', 'setup']) {
    assert.deepEqual([...document.querySelector('#' + id + ' .auth-body').children].map(node => node.className), id === 'login' ? ['auth-card'] : ['auth-card', 'auth-card backup-setup']);
    assert.equal(document.querySelector('#' + id + ' .auth-top .brand,#' + id + ' .auth-top .brand-mark'), null, id + ': the mark is in the card');
  }
  const head = document.querySelector('#login-form > .auth-card-head');
  assert.equal(document.querySelector('#login-form').firstElementChild, head, 'the card starts with its head');
  assert.ok(head.querySelector('.auth-card-mark[href="/"] .brand-mark'));
  assert.deepEqual([head.querySelector('h1').getAttribute('data-i18n'), head.querySelector('h1').textContent, head.querySelector('.auth-card-for').textContent], ['html.4', 'Multitap 관리자', 'for Proxmox VE']);
  assert.equal(document.querySelectorAll('#login-form h1,#login-form h2').length, 1, 'one title, no second 관리자 로그인 heading');
  assert.equal(document.querySelector('#login .auth-note').textContent, '사용자는 Windows·Android 앱으로 접속해요.');
  // The footer's download is a text link in the footer row, not a pill.
  assert.ok(document.querySelector('#login .auth-foot > a.client-download[data-client-download]'));
  const css = await readFile(new URL('../admin/console.css', import.meta.url), 'utf8');
  const rule = /(^|\n)\.client-download\{([^}]*)\}/.exec(css)[2];
  assert.doesNotMatch(rule, /border|border-radius|background|padding/, 'a plain link: no border, fill or padding');
  assert.doesNotMatch(css, /\.auth-foot \.client-download/);
  // First setup: the same head, then where it is in the three steps.
  const setup = document.querySelector('#setup-form');
  assert.equal(setup.querySelector('.auth-card-head h1').textContent, '관리자 계정 설정');
  assert.equal(setup.querySelector('.auth-step').textContent, '1/3 관리자 계정 변경');
  assert.equal(setup.querySelectorAll('h1,h2').length, 1);
  assert.doesNotMatch(document.body.textContent, /한곳에서 관리|처음이시군요/);
  // The catalogs say the same, in both languages; the removed texts are gone from both.
  assert.deepEqual([ko['html.4'], en['html.4']], ['Multitap 관리자', 'Multitap admin console']);
  assert.deepEqual([ko['html.20'], en['html.20']], ['관리자 계정 설정', 'Set up the admin account']);
  assert.deepEqual([ko['html.11'], en['html.11']], ['사용자는 Windows·Android 앱으로 접속해요.', 'Users connect from the Windows or Android app.']);
  for (const id of ['html.5', 'html.6', 'html.21', 'html.24', 'app.42']) { assert.equal(ko[id], undefined, id); assert.equal(en[id], undefined, id); }
  // The sign-in footer's download is one sentence in each language (never pieces joined in Korean order).
  assert.deepEqual([ko['app.41'], en['app.41']], ['Windows 클라이언트 {version} 다운로드', 'Download Windows client {version}']);
});

test('the server\'s own page in English: product names, one plain line, the same short-link label, no eyebrow', () => {
  const file = (product, filename, extra = {}) => ({ product, version: /(\d+\.\d+\.\d+)/.exec(filename)[1], filename, url: '/downloads/' + filename, format: product === 'android-client' ? 'apk' : 'installer', ...extra });
  const releases = [file('client', 'Multitap-1.0.41-windows-x64.exe'), file('host', 'Multitap-Host-1.0.43-windows-x64.exe'), file('android-client', 'Multitap-1.0.28-arm64-v8a.apk', { abi: 'arm64-v8a' })];
  const { document } = parseHTML(renderLanding({ version: '1.0.51', publicOrigin: 'https://vdi.example', releases, lang: 'en' }));
  assert.deepEqual([...document.querySelectorAll('.dl-card')].map(node => [node.querySelector('.dl-title').textContent, node.querySelector('.dl-text').textContent, node.querySelector('.dl-short-label').textContent]),
    [['Windows client', 'Install it on the PC you connect from.', 'Short link'], ['Multitap Host', 'Install it on each Windows (VM or PC) you use as a desktop.', 'Short link'], ['Android app', 'Connect to your desktops from phones and tablets.', 'Short link']]);
  assert.equal(document.querySelector('.hero-eyebrow,.dl-where,.dl-icon,.dl-card.is-primary,a.dl-button.primary'), null);
  assert.equal(document.querySelector('.hero-inner').firstElementChild.tagName, 'H1', 'the heading is the first line of the band');
  for (const lang of ['ko', 'en']) for (const product of Object.values(productsFor(lang))) {
    assert.equal(product.where, undefined); assert.equal(product.icon, undefined);
    assert.doesNotMatch(product.text, /\. /, lang + ' ' + product.key + ': one line');
  }
});
