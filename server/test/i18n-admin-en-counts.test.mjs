// English counts and embedded times (never "1 desktops", "1 users" or "Scheduled for Today 19:13").
// Its own process, English chosen before the console's i18n module loads (as i18n-admin-en.test.mjs).
import test from 'node:test';
import assert from 'node:assert/strict';

const stored = new Map([['multitap-lang', 'en']]);
globalThis.localStorage = { getItem: key => stored.get(key) ?? null, setItem: (key, value) => { stored.set(key, String(value)); }, removeItem: key => { stored.delete(key); } };
const { t, lang } = await import('../admin/js/i18n.js');

test('one of a thing is singular', () => {
  assert.equal(lang, 'en');
  assert.equal(t('desktops.126', { groupCount: 1, filter: 0 }), '1 desktop · Unreachable 0');
  assert.equal(t('desktops.126', { groupCount: 4, filter: 1 }), '4 desktops · Unreachable 1');
  assert.equal(t('permissions.35', { name: 'Design 1', listCount: 1, sharingName: 'Basic' }), 'Design 1: 1 user · Basic');
  assert.equal(t('permissions.35', { name: 'Design 1', listCount: 3, sharingName: 'Basic' }), 'Design 1: 3 users · Basic');
  assert.equal(t('permissions.36', { username: 'jamie', listCount: 2, sharingName: 'Basic' }), 'jamie: 2 desktops · Basic');
  assert.equal(t('users.39', { roleText: 'User', count: 1, value: 'Active', value2: '' }), 'User · 1 desktop · Active');
  assert.equal(t('users.39', { roleText: 'User', count: 0, value: 'Active', value2: '' }), 'User · 0 desktops · Active');
  assert.equal(t('usage.73', { connectedText: '2 h', connections: 1, value: '' }), 'Total 2 h · 1 connection');
  assert.equal(t('usage.73', { connectedText: '2 h', connections: 12, value: '' }), 'Total 2 h · 12 connections');
  // Two counts in one line: named, so neither needs a plural.
  assert.equal(t('usage.96', { logins: 1, connections: 1 }), ' · Sign-ins 1 · Connections 1');
});

test('a time inside a sentence is not capitalised', () => {
  assert.equal(t('notices.27', { whenText: t('notices.6', { hhmm: '19:13' }) }), 'Scheduled for today at 19:13');
  assert.equal(t('notices.71', { whenText: t('notices.7', { hhmm: '09:00' }) }), 'Until tomorrow at 09:00');
});

test('다운로드 in English: the page is named as its menu entry, the words are the public page\'s', () => {
  assert.equal(t('html.64'), 'Downloads'); assert.equal(t('downloads.6'), 'Verification values'); assert.equal(t('downloads.fileF4'), 'File');
  assert.equal(t('downloads.4', { title: 'Windows client' }), 'Copy the Windows client short link');
});
