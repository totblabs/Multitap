import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile, readdir } from 'node:fs/promises';
import ko from '../admin/i18n/ko.js';
import en from '../admin/i18n/en.js';

// The plain admin style in its small things: Korean text quotes with
// ‘ ’ (never the Chinese and Japanese 「」), no coloured bar on the side of a row or block, and no page subtitle that
// only narrates what the page is.
const read = path => readFile(new URL('../' + path, import.meta.url), 'utf8');
async function files(dir, pattern) {
  const out = [];
  for (const entry of await readdir(new URL('../' + dir, import.meta.url), { withFileTypes: true })) {
    if (entry.name === 'node_modules' || entry.name === 'vendor') continue;
    const path = dir + '/' + entry.name;
    if (entry.isDirectory()) out.push(...await files(path, pattern)); else if (pattern.test(entry.name)) out.push(path);
  }
  return out;
}

test('no 「」 corner brackets in the admin, the server or the setup script; Korean quotes with ‘ ’', async () => {
  for (const path of [...await files('admin', /\.(m?js|html|css)$/), ...await files('src', /\.(m?js|html)$/), ...await files('setup', /\.py$/)])
    assert.doesNotMatch(await read(path), /[「」]/, path);
  for (const [key, value] of [...Object.entries(ko), ...Object.entries(en)]) assert.doesNotMatch(JSON.stringify(value), /[「」]/, key);
  assert.match(ko['users.66'], /‘기본’은 아무것도 주고받지 않아요\. 새로 배정한 데스크톱은 ‘기본’으로 시작해요\./);
  assert.match(ko['hosts.30'], /‘연결이 끝나면 업데이트’/);
});

test('지금 할 일, the notice preview and the Proxmox token cell have no coloured side or top bar', async () => {
  const operations = await read('admin/operations.css'), notices = await read('admin/notices.css'), pages = await read('admin/pages.css');
  for (const rule of operations.match(/\.todo-(item|calm)[^{]*\{[^}]*\}/g)) assert.doesNotMatch(rule, /box-shadow|border-left/, rule);
  assert.doesNotMatch(/\.notice-preview\{[^}]*\}/.exec(notices)[0], /box-shadow|border-left/);
  assert.doesNotMatch(pages, /\.pve-detail\.is-warn\{[^}]*box-shadow/);
  assert.doesNotMatch(pages, /diagnostic-(grid|item)/, 'the old label-over-value grid is gone');
  // The state of a 지금 할 일 row is a dot before its title; the calm line is plain grey text.
  const overview = await read('admin/js/overview.js');
  assert.match(overview, /todoDot\(tone\)/);
  assert.match(operations, /\.todo-calm \.todo-text strong\{color:var\(--muted\)/);
  // A token that has more than a week left is no warning (amber only within 7 days).
  assert.match(await read('admin/js/system.js'), /days !== null && days <= 7 \? 'warn' : ''/);
  assert.match(overview, /tokenDays <= 7 \? 'bad' : 'info'/);
});

test('the Updates, Server access and Proxmox pages have no narrating subtitle; Downloads keeps its one fact', async () => {
  for (const key of ['system.58', 'system.60', 'system.90', 'system.160', 'server-access.141']) { assert.equal(ko[key], undefined, key); assert.equal(en[key], undefined, key); }
  assert.equal(ko['downloads.8'], '짧은 주소는 항상 최신 버전으로 연결돼요.');
});
