// The admin console's 오픈소스 라이선스 panel (admin/js/system.js): every
// original it links to is served by this server, and the ws version it states
// is the one server/package.json ships.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { randomBytes } from 'node:crypto';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';

const read = path => readFile(new URL(path, import.meta.url), 'utf8');

test('Open-source panel links only to originals this server serves, with the shipped ws version', async t => {
  // The rows as the panel has them (their words come from the text catalog, admin/i18n).
  const { serverOpenSource } = await import('../admin/js/system.js');
  const rows = serverOpenSource.map(([name, , href, detail]) => ({ name, href, detail: detail ?? null }));
  assert.ok(rows.length >= 5, 'the panel lists its components');
  const pkg = JSON.parse(await read('../package.json'));
  const ws = rows.find(row => row.name === 'ws');
  assert.equal(ws.detail.split(' ')[0], pkg.dependencies.ws, 'the panel states the ws version server/package.json ships');

  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const server = await buildServer({ store, vault, publicOrigin: 'https://licenses.invalid', proxmox: { inventory: async () => [] }, logger: { error() {} } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  const base = 'http://127.0.0.1:' + server.address().port;
  for (const row of rows) {
    const response = await fetch(base + row.href);
    const text = await response.text();
    assert.equal(response.status, 200, `${row.name}: ${row.href} is served`);
    assert.ok(text.length > 300, `${row.name}: ${row.href} is the original text, not a stub`);
  }
  assert.match(await (await fetch(base + '/admin/licenses/ws-LICENSE.txt')).text(), /Einar Otto Stangvik/);
  // The server's own licence: the row says AGPL-3.0 and links to the official AGPL version 3 text.
  const own = serverOpenSource[0];
  assert.deepEqual([own[1], own[2]], ['AGPL-3.0', '/admin/licenses/Multitap-AGPL-3.0.txt']);
  assert.match(await (await fetch(base + own[2])).text(), /^\s*GNU AFFERO GENERAL PUBLIC LICENSE\s+Version 3, 19 November 2007/);
  assert.equal((await fetch(base + '/admin/licenses/Multitap-GPL-3.0.txt')).status, 404, 'the old licence address is gone');
  assert.equal(pkg.license, 'AGPL-3.0-only');
});
