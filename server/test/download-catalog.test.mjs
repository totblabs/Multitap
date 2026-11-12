import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { createHash } from 'node:crypto';
import { mkdtemp, writeFile, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { downloadRoutes, publishedRelease, publicDownloadCatalog, maxCatalogEntries, maxPublicCatalogEntries, maxReleaseManifestBytes } from '../src/downloads.mjs';
import { bundleEntries, mergeDownloadCatalog } from '../src/download-bundle.mjs';

const body = Buffer.from('Isolated immutable catalog fixture, not an executable');
const sha256 = createHash('sha256').update(body).digest('hex');
const windows = (i, product = 'client', extension = 'exe') => ({ product, version: `0.4.${i}`, filename: `Multitap${product === 'host' ? '-Host' : ''}-0.4.${i}-windows-x64.${extension}`, size: body.length, sha256 });
const android = (i, abi = 'arm64-v8a') => ({ product: 'android-client', version: `0.1.0-preview.${i}`, abi, format: 'apk', filename: `Multitap-0.1.0-preview.${i}-android-${abi}.apk`, size: body.length, sha256 });
async function fixture(t) {
  const root = await mkdtemp(join(tmpdir(), 'multitap-catalog-'));
  const route = downloadRoutes(root), server = createServer(async (req, res) => {
    if (!await route(req, res, new URL(req.url, 'http://localhost').pathname)) { res.writeHead(404); res.end(); }
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); await rm(root, { recursive: true, force: true }); });
  return { root, manifest: join(root, 'releases.json'), origin: `http://127.0.0.1:${server.address().port}` };
}
const catalog = entries => publicDownloadCatalog(entries.map(publishedRelease));

test('latest per product/format/ABI precedes deterministic round-robin numeric history', () => {
  const entries = [];
  for (let i = 1; i <= 12; i++) {
    for (const product of ['client', 'host']) for (const ext of ['exe', 'zip']) entries.push(windows(i, product, ext));
    for (const abi of ['arm64-v8a', 'armeabi-v7a', 'x86_64']) entries.push(android(i, abi));
  }
  const projected = catalog(entries), reversed = catalog([...entries].reverse());
  assert.deepEqual(projected, reversed, 'input/publication order must not change the public feed');
  assert.equal(projected.length, maxPublicCatalogEntries);
  for (let round = 0; round < 4; round++) {
    const group = projected.slice(round * 7, round * 7 + 7);
    assert.equal(new Set(group.map(e => `${e.product}/${e.format}/${e.abi ?? ''}`)).size, 7);
    assert.ok(group.every(e => e.version.endsWith(`.${12 - round}`)));
  }
  assert.deepEqual(catalog([windows(9), windows(10)]).map(e => e.version), ['0.4.10', '0.4.9']);
  const stable = { ...android(1), version: '0.1.0', filename: 'Multitap-0.1.0-android-arm64-v8a.apk' };
  assert.deepEqual(catalog([android(9), android(10), stable]).map(e => e.version), ['0.1.0', '0.1.0-preview.10', '0.1.0-preview.9']);
});

test('the open-source 1.0.0 sorts above every 0.x of each product', async () => {
  const at = (product, version, extension = 'exe') => ({ ...windows(0, product, extension), version, filename: `Multitap${product === 'host' ? '-Host' : ''}-${version}-windows-x64.${extension}` });
  const apk = (version, abi = 'arm64-v8a') => ({ ...android(1, abi), version, filename: `Multitap-${version}-android-${abi}.apk` });
  const entries = [at('client', '0.4.179'), at('client', '1.0.0'), at('client', '0.4.180'), at('host', '0.2.155'), at('host', '1.0.0'), at('host', '0.2.99'),
    apk('0.1.0-preview.66'), apk('1.0.0'), apk('0.1.0-preview.65')];
  const projected = catalog(entries);
  assert.equal(projected.length, entries.length);
  for (const product of ['client', 'host', 'android-client']) assert.equal(projected.find(e => e.product === product).version, '1.0.0', product);
  assert.deepEqual(projected.filter(e => e.product === 'host').map(e => e.version), ['1.0.0', '0.2.155', '0.2.99']);
  assert.deepEqual(projected.filter(e => e.product === 'android-client').map(e => e.version), ['1.0.0', '0.1.0-preview.66', '0.1.0-preview.65']);
  // The public pages and /get/<name> short links (admin/download-choices.js) point at 1.0.0 too.
  const { latestWindows, latestAndroid, latestFile, compareVersions } = await import('../admin/download-choices.js');
  assert.equal(latestWindows(projected, 'client').version, '1.0.0'); assert.equal(latestWindows(projected, 'host').version, '1.0.0');
  assert.equal(latestAndroid(projected).version, '1.0.0'); assert.equal(latestFile(projected, 'android').filename, 'Multitap-1.0.0-android-arm64-v8a.apk');
  assert.equal(compareVersions('1.0.0', '0.1.0-preview.999'), 1); assert.equal(compareVersions('1.0.0', '0.5.125'), 1);
  // The broker updater's own order (update/protocol.mjs) offers broker 1.0.0 over 0.5.125.
  const { compareVersion } = await import('../update/protocol.mjs');
  assert.equal(compareVersion('1.0.0', '0.5.125'), 1); assert.equal(compareVersion('0.5.125', '1.0.0'), -1); assert.equal(compareVersion('1.0.1', '1.0.0'), 1);
});

test('1024 immutable raw releases project to a legacy-compatible API while old unprojected URLs still GET/HEAD', async t => {
  const f = await fixture(t), entries = Array.from({ length: maxCatalogEntries }, (_, i) => ({ ...windows(i), privateField: 'never public', retained: i }));
  assert.equal(maxCatalogEntries, 1024);
  const original = Buffer.from(JSON.stringify(entries));
  await writeFile(f.manifest, original);
  for (const e of [entries[0], entries[32], entries.at(-1)]) await writeFile(join(f.root, e.filename), body);
  const response = await fetch(f.origin + '/api/v1/downloads'), bytes = Buffer.from(await response.arrayBuffer()), list = JSON.parse(bytes);
  assert.equal(list.releases.length, 32); assert.ok(bytes.length <= 512 * 1024, 'released clients read at most 512 KiB');
  assert.equal(list.releases[0].version, '0.4.1023');
  assert.ok(list.releases.every(e => !Object.hasOwn(e, 'privateField') && !Object.hasOwn(e, 'retained')));
  assert.ok(!list.releases.some(e => e.filename === entries[32].filename));
  for (const e of [entries[0], entries[32], entries.at(-1)]) {
    const get = await fetch(f.origin + '/downloads/' + e.filename);
    assert.equal(get.status, 200); assert.equal(get.headers.get('content-type'), 'application/vnd.microsoft.portable-executable');
    assert.deepEqual(Buffer.from(await get.arrayBuffer()), body);
    const head = await fetch(f.origin + '/downloads/' + e.filename, { method: 'HEAD' });
    assert.equal(head.status, 200); assert.equal(head.headers.get('content-length'), String(body.length));
    assert.equal((await head.arrayBuffer()).byteLength, 0);
  }
  assert.deepEqual(await readFile(f.manifest), original, 'read projection never rewrites immutable history');
});

test('raw count and UTF-8 byte caps are independent; malformed UTF-8 and duplicate identities fail closed', async t => {
  const f = await fixture(t), e = windows(1);
  await writeFile(join(f.root, e.filename), body);
  const tooMany = Array.from({ length: maxCatalogEntries + 1 }, (_, i) => windows(i));
  // Valid notes at the 8000-byte limit; newlines double in JSON.
  const tooLarge = Array.from({ length: 900 }, (_, i) => ({ ...windows(i), changelog: '\n'.repeat(7000) }));
  assert.ok(Buffer.byteLength(JSON.stringify(tooMany)) < maxReleaseManifestBytes);
  assert.ok(Buffer.byteLength(JSON.stringify(tooLarge)) > maxReleaseManifestBytes);
  for (const bytes of [Buffer.from(JSON.stringify(tooMany)), Buffer.from(JSON.stringify(tooLarge)), Buffer.from([0xff]), Buffer.from(JSON.stringify([e, { ...e, sha256: 'b'.repeat(64) }]))]) {
    await writeFile(f.manifest, bytes);
    assert.deepEqual(await (await fetch(f.origin + '/api/v1/downloads')).json(), { releases: [] });
    assert.equal((await fetch(f.origin + '/downloads/' + e.filename)).status, 404);
  }
});

test('bundle merge accepts the 33rd release without changing old objects and refuses 129 or byte overflow', () => {
  const old = Array.from({ length: 32 }, (_, i) => ({ ...windows(i), retained: `original ${i}` }));
  const merged = mergeDownloadCatalog(old, [windows(32)]);
  assert.equal(merged.entries.length, 33);
  for (let i = 0; i < old.length; i++) assert.equal(merged.entries[i], old[i]);
  assert.equal(bundleEntries(merged.bytes).length, 33);
  assert.throws(() => mergeDownloadCatalog(Array.from({ length: maxCatalogEntries }, (_, i) => windows(i)), [windows(maxCatalogEntries)]), /catalog is full/);
  assert.throws(() => mergeDownloadCatalog(old, [{ ...old[0], sha256: 'b'.repeat(64) }]), /Immutable/);
  assert.throws(() => mergeDownloadCatalog(Array.from({ length: 900 }, (_, i) => ({ ...windows(i), changelog: '\n'.repeat(7000) })), []), /exceeds limit/);
});
