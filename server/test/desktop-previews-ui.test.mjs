import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile, readdir } from 'node:fs/promises';
import * as system from '../admin/js/system.js';
import { hostEntries } from '../admin/js/timeline.js';

// The administrator console has no 미리보기 화면 page (previews
// are turned on and off in the client). The
// server-wide value and PUT /api/v1/admin/desktop-previews stay as they were
// (desktop-previews.test.mjs covers the route and the default, on).
test('The console has no preview page, entry or switch, and reads the server-wide value nowhere', async () => {
  assert.equal('previews' in system, false); assert.equal('setPreviews' in system, false);
  const admin = new URL('../admin/', import.meta.url);
  const files = ['index.html', 'app.js', 'i18n/ko.js', 'i18n/en.js', ...(await readdir(new URL('js/', admin))).filter(name => name.endsWith('.js')).map(name => 'js/' + name)];
  for (const file of files) {
    const source = await readFile(new URL(file, admin), 'utf8');
    assert.doesNotMatch(source, /desktopPreviews|desktop-previews|previewSaving/, file);
    assert.doesNotMatch(source, /미리보기 화면 (끄기|켜기)|data-page="previews"/, file);
  }
  // The old addresses still open a page.
  const app = await readFile(new URL('app.js', admin), 'utf8');
  assert.match(app, /previews: \['server-info', null\]/); assert.match(app, /previews: 'server-info'/);
});

test('The Host timeline names the preview answer and size, never the picture', () => {
  const rows = hostEntries([{ desktopId: 'pve-149', diagnostics: { version: '0.2.146', events: [
    { time: '2026-11-02T10:00:00.000Z', event: 'hostAction', values: { action: 280, stage: 1, ok: true, kib: 9 } },
    { time: '2026-11-02T10:01:00.000Z', event: 'hostAction', values: { action: 280, stage: 3, ok: true, kib: 0 } },
  ] } }]);
  assert.equal(rows.length, 2);
  assert.equal(rows[0].title, 'Host 작업 · 데스크톱 미리보기 화면');
  assert.equal(rows[0].detail[0], '화면을 찍음');
  assert.equal(rows[1].detail.join(' '), '잠겨 있어 찍지 않음');
});
