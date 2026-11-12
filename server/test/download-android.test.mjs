import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createServer, request } from 'node:http';
import { mkdtemp, writeFile, rm, mkdir, symlink } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createHash } from 'node:crypto';
import { downloadRoutes } from '../src/downloads.mjs';

const body = Buffer.from('Isolated Android download routing fixture; not an installable APK');
const android = (abi = 'arm64-v8a') => ({ product: 'android-client', version: '0.1.0-preview.2', abi,
  filename: `Multitap-0.1.0-preview.2-android-${abi}.apk`, format: 'apk', size: body.length,
  sha256: createHash('sha256').update(body).digest('hex') });
async function fixture(t) {
  const outer = await mkdtemp(join(tmpdir(), 'multitap-android-download-'));
  const directory = join(outer, 'releases'); await mkdir(directory);
  const manifest = join(directory, 'releases.json');
  const routes = downloadRoutes(directory);
  const server = createServer(async (req, res) => {
    if (!await routes(req, res, new URL(req.url, 'http://localhost').pathname)) { res.writeHead(404); res.end(); }
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); await rm(outer, { recursive: true, force: true }); });
  return { directory, outer, manifest, origin: `http://127.0.0.1:${server.address().port}` };
}
function raw(origin, path) {
  return new Promise((resolve, reject) => {
    const req = request(origin, { path }, res => { res.resume(); res.on('end', () => resolve(res.statusCode)); });
    req.on('error', reject); req.end();
  });
}

test('Android GET/HEAD use an exact APK manifest with all ABI metadata, Android MIME and no private fields', async t => {
  const f = await fixture(t), entries = ['arm64-v8a', 'armeabi-v7a', 'x86_64'].map(android);
  await writeFile(f.manifest, JSON.stringify(entries.map(e => ({ ...e, privateReceipt: 'must-not-appear' }))));
  for (const entry of entries) await writeFile(join(f.directory, entry.filename), body);
  const list = await (await fetch(f.origin + '/api/v1/downloads')).json();
  assert.equal(list.releases.length, 3);
  assert.deepEqual(list.releases.map(({ url, ...entry }) => entry), entries);
  for (const release of list.releases) {
    const response = await fetch(f.origin + release.url);
    assert.equal(response.status, 200);
    assert.equal(response.headers.get('content-type'), 'application/vnd.android.package-archive');
    assert.equal(response.headers.get('x-content-type-options'), 'nosniff');
    assert.equal(response.headers.get('content-length'), String(body.length));
    assert.equal(response.headers.get('content-disposition'), `attachment; filename="${release.filename.replace(/^Multitap(?=-)/, 'Multitap')}"`);
    assert.equal(response.headers.get('etag'), `"sha256-${release.sha256}"`);
    assert.deepEqual(Buffer.from(await response.arrayBuffer()), body);
    const head = await fetch(f.origin + release.url, { method: 'HEAD' });
    assert.equal(head.status, 200); assert.equal((await head.arrayBuffer()).byteLength, 0);
    assert.equal(head.headers.get('content-type'), response.headers.get('content-type'));
  }
});

test('adding Android keeps Windows list shape and archive/installer MIME unchanged', async t => {
  const f = await fixture(t), apk = android();
  const windows = ['zip', 'exe'].map(extension => ({ product: 'client', version: '0.4.12',
    filename: `Multitap-0.4.12-windows-x64.${extension}`, size: body.length, sha256: apk.sha256 }));
  await writeFile(f.manifest, JSON.stringify([...windows, apk]));
  for (const e of [...windows, apk]) await writeFile(join(f.directory, e.filename), body);
  const list = await (await fetch(f.origin + '/api/v1/downloads')).json();
  for (const e of windows) assert.deepEqual(list.releases.find(value=>value.filename===e.filename), { ...e, format: e.filename.endsWith('.exe') ? 'installer' : 'archive', url: '/downloads/' + e.filename });
  for (const [entry, mime] of [[windows[0], 'application/zip'], [windows[1], 'application/vnd.microsoft.portable-executable']]) {
    const response = await fetch(f.origin + '/downloads/' + entry.filename);
    assert.equal(response.headers.get('content-type'), mime);
    assert.deepEqual(Buffer.from(await response.arrayBuffer()), body);
  }
});

test('Android invalid product/version/ABI/format/filename/hash/size entries are never served', async t => {
  const f = await fixture(t), valid = android();
  await writeFile(join(f.directory, valid.filename), body);
  const invalid = [
    { product: 'client' }, { product: 'host' }, { format: 'installer' }, { format: undefined },
    { abi: 'arm64' }, { abi: '../arm64-v8a' }, { version: '0.1.0-preview.0' }, { version: '0.1.0-preview.2+secret' },
    { version: '0.1.0-rc.1' }, { version: ['0.1.0-preview.2'] }, { version: '01.1.0-preview.2' },
    { version: '0.1.0-preview.2', filename: 'Multitap-0.1.0-preview.1-android-arm64-v8a.apk' },
    { filename: '../' + valid.filename }, { filename: valid.filename + '.exe' },
    { sha256: 'A'.repeat(64) }, { sha256: 'a'.repeat(63) }, { sha256: null },
    { size: 0 }, { size: -1 }, { size: 1.1 }, { size: '64' }, { size: 128 * 1024 * 1024 + 1 },
  ];
  for (const change of invalid) {
    await writeFile(f.manifest, JSON.stringify([{ ...valid, ...change }]));
    assert.deepEqual(await (await fetch(f.origin + '/api/v1/downloads')).json(), { releases: [] }, JSON.stringify(change));
    assert.equal((await fetch(f.origin + '/downloads/' + valid.filename)).status, 404);
  }
});

test('Android requests reject path escapes, changed size, nonfiles, excess catalog entries and unsupported methods', async t => {
  const f = await fixture(t), entry = android();
  await writeFile(f.manifest, JSON.stringify([entry]));
  await writeFile(join(f.directory, entry.filename), body);
  for (const path of ['/downloads/../broker.sqlite', '/downloads/%2e%2e%2fbroker.sqlite',
    '/downloads/' + entry.filename + '/extra', '/downloads/%2f' + entry.filename,
    '/downloads/' + entry.filename.replace('.apk', '.APK'), '/downloads/releases.json']) {
    assert.equal(await raw(f.origin, path), 404, path);
  }
  const post = await fetch(f.origin + '/downloads/' + entry.filename, { method: 'POST' });
  assert.equal(post.status, 405); assert.equal(post.headers.get('allow'), 'GET, HEAD');
  await writeFile(join(f.directory, entry.filename), 'wrong-size');
  assert.equal((await fetch(f.origin + '/downloads/' + entry.filename)).status, 404);
  await rm(join(f.directory, entry.filename)); await mkdir(join(f.directory, entry.filename));
  assert.equal((await fetch(f.origin + '/downloads/' + entry.filename)).status, 404);
  await writeFile(f.manifest, JSON.stringify(Array.from({ length: 33 }, () => entry)));
  assert.deepEqual(await (await fetch(f.origin + '/api/v1/downloads')).json(), { releases: [] });
});

test('Android APK symlinks to outside or another file within the release root are rejected', async t => {
  const f = await fixture(t), entry = android(), target = join(f.directory, entry.filename);
  const outside = join(f.outer, 'outside.apk'), inside = join(f.directory, 'other.apk');
  await writeFile(outside, body); await writeFile(inside, body);
  await writeFile(f.manifest, JSON.stringify([entry]));
  try { await symlink(outside, target, 'file'); }
  catch (error) { if (process.platform === 'win32' && ['EPERM', 'EACCES'].includes(error.code)) { t.skip('Windows account cannot create a file symlink; run this test on Linux.'); return; } throw error; }
  assert.equal((await fetch(f.origin + '/downloads/' + entry.filename)).status, 404);
  await rm(target); await symlink(inside, target, 'file');
  assert.equal((await fetch(f.origin + '/downloads/' + entry.filename)).status, 404);
});

const windowsEntry = i => ({ product: 'client', version: `0.4.${i}`,
  filename: `Multitap-0.4.${i}-windows-x64.exe`, size: 64, sha256: 'a'.repeat(64), retainedField: 'unchanged' });

test('32-entry mixed public catalog remains compatible while a 33rd raw release and old URLs coexist', async t => {
  const f = await fixture(t);
  const apks = ['arm64-v8a','armeabi-v7a','x86_64'].map(android);
  const windows = Array.from({ length: 29 }, (_, i) => ({ ...windowsEntry(i), size: body.length, sha256: android().sha256 }));
  const entries = [...windows,...apks];
  await writeFile(f.manifest, JSON.stringify(entries));
  for (const entry of entries) await writeFile(join(f.directory, entry.filename), body);
  const list = await (await fetch(f.origin + '/api/v1/downloads')).json();
  assert.equal(list.releases.length, 32);
  for (const entry of [windows[0],windows[28],...apks]) {
    const response = await fetch(f.origin + '/downloads/' + entry.filename);
    assert.equal(response.status, 200);
    assert.deepEqual(Buffer.from(await response.arrayBuffer()), body);
    const head = await fetch(f.origin + '/downloads/' + entry.filename, { method: 'HEAD' });
    assert.equal(head.status, 200); assert.equal((await head.arrayBuffer()).byteLength, 0);
    assert.equal(head.headers.get('content-type'), entry.format === 'apk' ? 'application/vnd.android.package-archive' : 'application/vnd.microsoft.portable-executable');
  }
  await writeFile(f.manifest, JSON.stringify([...entries,windowsEntry(100)]));
  const extended = await (await fetch(f.origin + '/api/v1/downloads')).json();
  assert.equal(extended.releases.length, 32);
  assert.ok(extended.releases.some(e=>e.filename===windowsEntry(100).filename));
  assert.equal((await fetch(f.origin + '/downloads/' + apks[0].filename)).status, 200);
});
