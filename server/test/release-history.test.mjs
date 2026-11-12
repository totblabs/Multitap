import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { createHash, randomUUID } from 'node:crypto';
import * as fs from 'node:fs/promises';
import { constants } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { downloadRoutes, publicDownloadCatalog, publishedRelease, maxCatalogEntries, maxPublicCatalogEntries, maxReleaseManifestBytes, readReleases } from '../src/downloads.mjs';
import { releaseHistoryRoutes, releaseHistoryPage, collectLocalReleaseHistory } from '../src/release-history.mjs';
import {
  releaseHistoryRecord, releaseHistoryMerge, releaseHistoryWrite, releaseHistoryBackfill, releaseHistoryParseFileName,
  releaseHistoryFileName, releaseHistoryBytes, compareReleaseHistoryVersions, releaseHistoryDirectory,
} from '../src/release-history-core.mjs';
import { HttpError } from '../src/errors.mjs';

const io = { ...fs, randomUUID };
const body = Buffer.from('Isolated release-history fixture; never an executable');
const sha256 = createHash('sha256').update(body).digest('hex');
const windows = (version, product = 'client', extra = {}) => ({ product, version, filename: `Multitap${product === 'host' ? '-Host' : ''}-${version}-windows-x64.exe`, size: body.length, sha256, ...extra });
const android = (version, abi = 'arm64-v8a') => ({ product: 'android-client', version, abi, format: 'apk', filename: `Multitap-${version}-android-${abi}.apk`, size: body.length, sha256 });

async function directory(t) {
  const root = await fs.mkdtemp(join(tmpdir(), 'multitap-history-'));
  t.after(() => fs.rm(root, { recursive: true, force: true }));
  return root;
}
// The broker's own error reply for HttpErrors (http.mjs), around the route.
async function serve(t, root) {
  const route = releaseHistoryRoutes(root), downloads = downloadRoutes(root);
  const server = createServer(async (req, res) => {
    const url = new URL(req.url, 'http://localhost');
    try {
      if (await downloads(req, res, url.pathname) || await route(req, res, url)) return;
      res.writeHead(404); res.end();
    } catch (error) {
      if (!(error instanceof HttpError)) throw error;
      res.writeHead(error.status, { 'Content-Type': 'application/json' }); res.end(JSON.stringify({ error: { code: error.code } }));
    }
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(() => new Promise(resolve => server.close(resolve)));
  return `http://127.0.0.1:${server.address().port}`;
}
const history = (origin, query) => fetch(`${origin}/api/v1/releases/history?${query}`);

test('records are notes-only projections with canonical names, bytes and version order', () => {
  const entry = windows('0.4.78', 'client', { changelog: '- 변경 내역 사이드바', releasedAt: '2026-10-26T01:02:03Z', signature: 'x', privateField: 'never' });
  assert.deepEqual(releaseHistoryRecord(entry), { product: 'client', version: '0.4.78', releasedAt: '2026-10-26T01:02:03.000Z', changelog: '- 변경 내역 사이드바' });
  assert.equal(releaseHistoryFileName(releaseHistoryRecord(entry)), 'client-0.4.78.json');
  assert.deepEqual(releaseHistoryParseFileName('android-client-0.1.0-preview.5.json'), { product: 'android-client', version: '0.1.0-preview.5' });
  for (const name of ['client-0.4.07.json', 'server-0.5.1.json', 'client-0.4.1-preview.json', '../client-0.4.1.json', 'client-0.4.1.json.part', '.client-0.4.1.json.x.part']) {
    assert.equal(releaseHistoryParseFileName(name), null, name);
  }
  for (const bad of [{ changelog: 'bell\u0007' }, { changelog: 'x'.repeat(8001) }, { releasedAt: '2026-02-30T00:00:00Z' }, { version: '0.4' }, { product: 'server' }]) {
    assert.equal(releaseHistoryRecord({ ...windows('0.4.1'), ...bad }), null, JSON.stringify(bad).slice(0, 40));
  }
  assert.deepEqual(releaseHistoryRecord({ ...windows('0.4.1'), changelog: '  \n' }), { product: 'client', version: '0.4.1' }, 'blank notes count as none');
  assert.equal(releaseHistoryBytes({ changelog: 'n', version: '1.0.0', product: 'host' }).toString(), '{\n  "product": "host",\n  "version": "1.0.0",\n  "changelog": "n"\n}\n');
  const versions = ['0.1.0-preview.10', '0.1.0', '0.1.0-preview.2', '0.1.0-preview', '0.0.9'];
  assert.deepEqual(versions.sort((a, b) => compareReleaseHistoryVersions(b, a)), ['0.1.0', '0.1.0-preview.10', '0.1.0-preview.2', '0.1.0-preview', '0.0.9']);
  assert.deepEqual(['0.4.9', '0.4.10', '0.4.100'].sort((a, b) => compareReleaseHistoryVersions(b, a)), ['0.4.100', '0.4.10', '0.4.9']);
});

test('merge keeps the first source per field, fills gaps from later ones and drafts never add a version', () => {
  const merged = releaseHistoryMerge([
    windows('0.4.70', 'client', { changelog: 'corrected' }),
    windows('0.4.70', 'client', { changelog: 'original', releasedAt: '2026-10-20T00:00:00.000Z' }),
    { entry: { product: 'client', version: '0.4.70', changelog: 'draft' }, fillOnly: true },
    { entry: { product: 'client', version: '0.4.69', changelog: 'draft only' }, fillOnly: true },
    { entry: { product: 'host', version: '0.2.60', changelog: 'host draft' }, fillOnly: true },
    windows('0.2.60', 'host'),
    android('0.1.0-preview.5'), android('0.1.0-preview.5', 'x86_64'),
  ]);
  assert.deepEqual(merged, [
    { product: 'client', version: '0.4.70', changelog: 'corrected', releasedAt: '2026-10-20T00:00:00.000Z' },
    { product: 'host', version: '0.2.60', changelog: 'host draft' },
    { product: 'android-client', version: '0.1.0-preview.5' },
  ]);
});

test('records are written once, completed with missing fields, never silently changed and survive unreadable neighbours', async t => {
  const root = await directory(t);
  const record = { product: 'client', version: '0.4.78', changelog: '- 첫 기록' };
  assert.deepEqual(await releaseHistoryWrite(io, constants, root, [record]), { written: 1, completed: 0, replaced: 0, unchanged: 0, conflicts: [] });
  const path = join(root, releaseHistoryDirectory, 'client-0.4.78.json');
  assert.deepEqual(JSON.parse(await fs.readFile(path, 'utf8')), record);
  assert.deepEqual(await releaseHistoryWrite(io, constants, root, [record]), { written: 0, completed: 0, replaced: 0, unchanged: 1, conflicts: [] });
  // A date found later completes the record; its notes stay.
  const dated = { ...record, releasedAt: '2026-10-26T00:00:00.000Z' };
  assert.equal((await releaseHistoryWrite(io, constants, root, [dated])).completed, 1);
  assert.deepEqual(JSON.parse(await fs.readFile(path, 'utf8')), dated);
  // Different notes are a conflict unless a correction explicitly allows it.
  const changed = { ...record, changelog: '- 다른 내용' };
  assert.deepEqual((await releaseHistoryWrite(io, constants, root, [changed])).conflicts, ['client-0.4.78.json']);
  assert.deepEqual(JSON.parse(await fs.readFile(path, 'utf8')), dated);
  assert.equal((await releaseHistoryWrite(io, constants, root, [{ product: 'client', version: '0.4.78', changelog: '- 정정' }], { replace: old => old.changelog === '- 첫 기록' })).replaced, 1);
  assert.deepEqual(JSON.parse(await fs.readFile(path, 'utf8')), { ...dated, changelog: '- 정정' }, 'a correction keeps the date');
  // Foreign content is never overwritten.
  await fs.writeFile(join(root, releaseHistoryDirectory, 'host-0.2.1.json'), 'not a record');
  assert.deepEqual((await releaseHistoryWrite(io, constants, root, [{ product: 'host', version: '0.2.1', changelog: 'x' }])).conflicts, ['host-0.2.1.json']);
  assert.equal(await fs.readFile(join(root, releaseHistoryDirectory, 'host-0.2.1.json'), 'utf8'), 'not a record');
  assert.ok(!(await fs.readdir(join(root, releaseHistoryDirectory))).some(name => name.endsWith('.part')), 'no temporary file remains');
  // Dry runs report without creating anything.
  const empty = await directory(t);
  assert.equal((await releaseHistoryWrite(io, constants, empty, [record], { dryRun: true })).written, 1);
  assert.deepEqual(await fs.readdir(empty), []);
});

test('history pages list record-only and catalog versions newest first, with bounded pages and a next cursor', async t => {
  const root = await directory(t), origin = await serve(t, root);
  // 150 pruned client versions exist only as records.
  const records = Array.from({ length: 150 }, (_, i) => ({ product: 'client', version: `0.3.${i}`, releasedAt: new Date(Date.UTC(2026, 0, 1) + i * 3600000).toISOString(), changelog: `- 기록 ${i}` }));
  await releaseHistoryWrite(io, constants, root, records);
  // The live catalog: newer versions, one with a corrected note over its record.
  await releaseHistoryWrite(io, constants, root, [{ product: 'client', version: '0.4.1', changelog: 'old text' }, { product: 'host', version: '0.2.9', changelog: 'host record' }]);
  await fs.writeFile(join(root, 'releases.json'), JSON.stringify([
    windows('0.4.2', 'client', { changelog: '- 최신', releasedAt: '2026-10-26T00:00:00.000Z', privateField: 'never public' }),
    windows('0.4.1', 'client', { changelog: 'corrected text' }),
    windows('0.2.10', 'host'),
  ]));
  let response = await history(origin, 'product=client&limit=100');
  assert.equal(response.status, 200);
  assert.equal(response.headers.get('content-type'), 'application/json; charset=utf-8');
  assert.equal(response.headers.get('x-content-type-options'), 'nosniff');
  let page = await response.json();
  assert.equal(page.product, 'client'); assert.equal(page.total, 152); assert.equal(page.releases.length, 100);
  assert.deepEqual(page.releases[0], { version: '0.4.2', releasedAt: '2026-10-26T00:00:00.000Z', changelog: '- 최신' });
  assert.deepEqual(page.releases[1], { version: '0.4.1', changelog: 'corrected text' }, 'the catalog notes win while it lists the release');
  assert.equal(page.releases[2].version, '0.3.149');
  assert.ok(!JSON.stringify(page).match(/privateField|sha256|filename|url|size/), 'no file metadata or private fields');
  assert.equal(page.next, page.releases.at(-1).version);
  const seen = page.releases.map(r => r.version);
  page = await (await history(origin, `product=client&limit=100&before=${page.next}`)).json();
  assert.equal(page.releases.length, 52); assert.equal(page.next, null);
  seen.push(...page.releases.map(r => r.version));
  assert.equal(new Set(seen).size, 152, 'pages neither repeat nor skip a version');
  assert.equal(seen.at(-1), '0.3.0');
  // Default page, clamped limit, a cursor between versions, other products.
  assert.equal((await (await history(origin, 'product=client')).json()).releases.length, 30);
  assert.equal((await (await history(origin, 'product=client&limit=5000')).json()).releases.length, 100);
  assert.deepEqual((await (await history(origin, 'product=client&limit=2&before=0.3.200')).json()).releases.map(r => r.version), ['0.3.149', '0.3.148']);
  assert.deepEqual(await (await history(origin, 'product=host')).json(), { product: 'host', total: 2, releases: [{ version: '0.2.10' }, { version: '0.2.9', changelog: 'host record' }], next: null });
  assert.deepEqual(await (await history(origin, 'product=android-client')).json(), { product: 'android-client', total: 0, releases: [], next: null });
  // HEAD carries no body.
  const head = await fetch(`${origin}/api/v1/releases/history?product=host`, { method: 'HEAD' });
  assert.equal(head.status, 200); assert.equal((await head.arrayBuffer()).byteLength, 0);
});

test('history queries are validated and other methods refused', async t => {
  const root = await directory(t), origin = await serve(t, root);
  for (const [query, code] of [['', 'invalid_product'], ['product=server', 'invalid_product'], ['product=client&limit=0', 'invalid_limit'], ['product=client&limit=-1', 'invalid_limit'],
    ['product=client&limit=1.5', 'invalid_limit'], ['product=client&before=0.4', 'invalid_before'], ['product=client&before=0.1.0-preview.1', 'invalid_before'],
    ['product=client&product=host', 'invalid_query']]) {
    const response = await history(origin, query);
    assert.equal(response.status, 400, query); assert.equal((await response.json()).error.code, code, query);
  }
  const post = await fetch(`${origin}/api/v1/releases/history?product=client`, { method: 'POST' });
  assert.equal(post.status, 405); assert.equal(post.headers.get('allow'), 'GET, HEAD');
  // An unreadable record still lists its version, without notes.
  await fs.mkdir(join(root, releaseHistoryDirectory));
  await fs.writeFile(join(root, releaseHistoryDirectory, 'client-1.0.0.json'), '{broken');
  await fs.writeFile(join(root, releaseHistoryDirectory, 'notes.txt'), 'ignored');
  assert.deepEqual((await (await history(origin, 'product=client')).json()).releases, [{ version: '1.0.0' }]);
  // No download directory configured: an empty history.
  assert.deepEqual(await releaseHistoryPage(undefined, { product: 'client' }), { product: 'client', total: 0, releases: [], next: null });
});

test('backfill merges the catalog, archives and drafts once; a second run changes nothing', async t => {
  const root = await directory(t), downloads = join(root, 'downloads'), archive = join(root, 'archive');
  await fs.mkdir(downloads); await fs.mkdir(archive);
  await fs.writeFile(join(downloads, 'releases.json'), JSON.stringify([windows('0.4.78', 'client', { changelog: '- live notes' }), windows('0.2.67', 'host', { changelog: '- live host', releasedAt: '2026-10-26T03:00:00.000Z' })]));
  // The live installer dates its record from its publication time.
  await fs.writeFile(join(downloads, windows('0.4.78').filename), body);
  const published = new Date('2026-10-26T05:06:07.000Z');
  await fs.utimes(join(downloads, windows('0.4.78').filename), published, published);
  // An archived (pruned) catalog, newer and older captures.
  const older = join(archive, 'releases-old.json'), newer = join(archive, 'releases-new.json');
  await fs.writeFile(older, JSON.stringify([windows('0.4.10', 'client', { changelog: 'before correction' }), windows('0.4.9', 'client', { changelog: '- 0.4.9' })]));
  await fs.writeFile(newer, JSON.stringify({ catalog: [windows('0.4.10', 'client', { changelog: 'after correction' }), { product: 'client', version: '9.9.9', changelog: 'not a catalog entry' }] }));
  await fs.utimes(older, new Date('2026-10-01'), new Date('2026-10-01'));
  await fs.utimes(newer, new Date('2026-10-20'), new Date('2026-10-20'));
  const tiers = [{ path: archive }, { candidates: [android('0.1.0-preview.5')] },
    { candidates: [{ product: 'client', version: '0.4.9', changelog: 'draft' }, { product: 'client', version: '0.4.8', changelog: 'unpublished draft' }, { product: 'client', version: '0.4.78', changelog: 'draft' }], fillOnly: true }];
  const dry = await releaseHistoryBackfill(io, constants, downloads, { tiers, dryRun: true });
  assert.equal(dry.written, 5); assert.equal(dry.dryRun, true);
  await assert.rejects(fs.lstat(join(downloads, releaseHistoryDirectory)), { code: 'ENOENT' });
  const first = await releaseHistoryBackfill(io, constants, downloads, { tiers });
  assert.deepEqual({ written: first.written, records: first.records, products: first.products, datedFromFiles: first.datedFromFiles, conflicts: first.conflicts },
    { written: 5, records: 5, products: { client: 3, host: 1, 'android-client': 1 }, datedFromFiles: 1, conflicts: [] });
  const read = async name => JSON.parse(await fs.readFile(join(downloads, releaseHistoryDirectory, name), 'utf8'));
  assert.deepEqual(await read('client-0.4.78.json'), { product: 'client', version: '0.4.78', releasedAt: published.toISOString(), changelog: '- live notes' });
  assert.equal((await read('client-0.4.10.json')).changelog, 'after correction', 'the newest archived capture wins');
  assert.equal((await read('client-0.4.9.json')).changelog, '- 0.4.9');
  await assert.rejects(fs.lstat(join(downloads, releaseHistoryDirectory, 'client-0.4.8.json')), { code: 'ENOENT' });
  await assert.rejects(fs.lstat(join(downloads, releaseHistoryDirectory, 'client-9.9.9.json')), { code: 'ENOENT' });
  const before = await fs.readdir(join(downloads, releaseHistoryDirectory));
  const second = await releaseHistoryBackfill(io, constants, downloads, { tiers });
  assert.equal(second.written + second.completed + second.replaced, 0); assert.equal(second.unchanged, 5);
  assert.deepEqual(await fs.readdir(join(downloads, releaseHistoryDirectory)), before);
  // After pruning the live catalog the notes are still served.
  await fs.writeFile(join(downloads, 'releases.json'), '[]');
  const page = await releaseHistoryPage(downloads, { product: 'client', limit: 10 });
  assert.deepEqual(page.releases.map(r => r.version), ['0.4.78', '0.4.10', '0.4.9']);
  assert.equal(page.releases[0].changelog, '- live notes');
});

test('the raised file catalog limit keeps the public feed newest-32 and its legacy shape', async t => {
  assert.equal(maxCatalogEntries, 1024); assert.equal(maxPublicCatalogEntries, 32);
  const root = await directory(t), origin = await serve(t, root);
  const entries = Array.from({ length: 1024 }, (_, i) => windows(`0.4.${i}`, i % 2 ? 'host' : 'client', { changelog: '가'.repeat(2000), releasedAt: '2026-10-26T00:00:00.000Z' }));
  const bytes = Buffer.from(JSON.stringify(entries, null, 2));
  assert.ok(bytes.length > 512 * 1024 && bytes.length < maxReleaseManifestBytes);
  await fs.writeFile(join(root, 'releases.json'), bytes);
  assert.equal((await readReleases(root)).length, 1024);
  const feed = await (await fetch(origin + '/api/v1/downloads')).json();
  assert.deepEqual(Object.keys(feed), ['releases']);
  assert.equal(feed.releases.length, 32);
  assert.deepEqual(feed.releases[0], { ...publishedRelease(entries[1022]) });
  assert.deepEqual(feed, { releases: publicDownloadCatalog(entries.map(publishedRelease)) });
  // The history lists all 512 client versions page by page.
  let before = '', count = 0;
  for (;;) {
    const page = await (await history(origin, `product=client&limit=100${before}`)).json();
    count += page.releases.length;
    if (!page.next) break;
    before = '&before=' + page.next;
  }
  assert.equal(count, 512);
  await fs.writeFile(join(root, 'releases.json'), JSON.stringify([...entries, windows('0.5.0')]));
  assert.deepEqual(await readReleases(root), [], '1025 entries fail closed');
});

test('local collection takes published entries from captured catalogs and only plain draft changelogs', async t => {
  const root = await directory(t), artifacts = join(root, '.artifacts'), deploy = join(root, 'deploy', 'windows');
  await fs.mkdir(join(artifacts, 'nested'), { recursive: true }); await fs.mkdir(deploy, { recursive: true });
  await fs.writeFile(join(artifacts, 'nested', 'catalog-after.json'), JSON.stringify({ catalog: [windows('0.4.44', 'client', { changelog: '- 캡처된 노트' }), { product: 'client', version: '0.4.43', changelog: 'no file identity' }] }));
  await fs.writeFile(join(artifacts, 'client-0425-changelog.txt'), '- 초안');
  await fs.writeFile(join(artifacts, 'client-0432-changelog.txt'), '- 원본');
  await fs.writeFile(join(artifacts, 'client-0432-changelog-corrected.txt'), '- 정정본');
  await fs.writeFile(join(artifacts, 'host-0224-changelog.txt'), Buffer.from([0xff, 0xfe]));
  await fs.writeFile(join(deploy, 'changelog-host-0.2.15.txt'), '- 오래된 Host');
  const local = await collectLocalReleaseHistory(root);
  assert.deepEqual(local.snapshots.map(e => [e.product, e.version, e.changelog]), [['client', '0.4.44', '- 캡처된 노트']]);
  assert.deepEqual(local.drafts, [
    { product: 'client', version: '0.4.32', changelog: '- 정정본' },
    { product: 'client', version: '0.4.25', changelog: '- 초안' },
    { product: 'client', version: '0.4.32', changelog: '- 원본' },
    { product: 'host', version: '0.2.15', changelog: '- 오래된 Host' },
  ]);
  // Corrected drafts come first, so they win the merge.
  assert.equal(releaseHistoryMerge([windows('0.4.32'), ...local.drafts.map(entry => ({ entry, fillOnly: true }))])[0].changelog, '- 정정본');
});
