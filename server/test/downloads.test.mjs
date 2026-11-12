import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { mkdtemp, writeFile, rm, symlink } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createHash } from 'node:crypto';
import { downloadRoutes } from '../src/downloads.mjs';

test('Downloads serve only published product archives and never arbitrary broker files', async t => {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-download-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const archive = Buffer.from('Isolated product archive fixture'), filename = 'Multitap-0.3.0-windows-x64.zip';
  const entry = { product: 'client', version: '0.3.0', filename, size: archive.length, sha256: createHash('sha256').update(archive).digest('hex'), secret: 'must-not-be-exposed' };
  await writeFile(join(directory, filename), archive);
  await writeFile(join(directory, 'vault.key'), 'fixture-private-data');
  const manifest = join(directory, 'releases.json');
  await writeFile(manifest, JSON.stringify([entry, { ...entry, filename: 'vault.key' }]));
  const routes = downloadRoutes(directory);
  const server = createServer(async (req, res) => { if (!await routes(req, res, new URL(req.url, 'http://localhost').pathname)) { res.writeHead(404); res.end(); } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(() => new Promise(resolve => server.close(resolve)));
  const base = 'http://127.0.0.1:' + server.address().port;
  const list = await (await fetch(base + '/api/v1/downloads')).json();
  assert.equal(list.releases.length, 1); assert.equal('secret' in list.releases[0], false);
  const response = await fetch(base + list.releases[0].url);
  assert.equal(response.status, 200); assert.equal(response.headers.get('content-type'), 'application/zip');
  assert.equal(response.headers.get('content-disposition'), 'attachment; filename="Multitap-0.3.0-windows-x64.zip"');
  assert.equal(list.releases[0].filename, filename, 'catalog/update identity stays compatible with released clients');
  assert.deepEqual(Buffer.from(await response.arrayBuffer()), archive);
  const head = await fetch(base + '/downloads/' + filename, { method: 'HEAD' });
  assert.equal(head.status, 200); assert.equal((await head.arrayBuffer()).byteLength, 0);
  for (const path of ['/downloads/vault.key', '/downloads/release.json', '/downloads/Multitap-0.2.0-windows-x64.zip', '/downloads/%2e%2e%2fvault.key']) assert.equal((await fetch(base + path)).status, 404);
  assert.equal((await fetch(base + '/downloads/' + filename, { method: 'POST' })).status, 405);
  await writeFile(join(directory, filename), 'changed-size');
  assert.equal((await fetch(base + '/downloads/' + filename)).status, 404);
  await writeFile(manifest, 'not json');
  assert.deepEqual(await (await fetch(base + '/api/v1/downloads')).json(), { releases: [] });
});

test('An interrupted installer download continues from a byte range of the same file only', async t => {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-download-range-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const installer = Buffer.from(Array.from({ length: 4096 }, (_, i) => i % 251)), filename = 'Multitap-0.4.170-windows-x64.exe';
  const sha256 = createHash('sha256').update(installer).digest('hex'), etag = `"sha256-${sha256}"`;
  await writeFile(join(directory, filename), installer);
  await writeFile(join(directory, 'releases.json'), JSON.stringify([{ product: 'client', version: '0.4.170', format: 'installer', filename, size: installer.length, sha256 }]));
  const routes = downloadRoutes(directory);
  const server = createServer(async (req, res) => { if (!await routes(req, res, new URL(req.url, 'http://localhost').pathname)) { res.writeHead(404); res.end(); } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(() => new Promise(resolve => server.close(resolve)));
  const url = `http://127.0.0.1:${server.address().port}/downloads/${filename}`;
  const whole = await fetch(url);
  assert.equal(whole.status, 200); assert.equal(whole.headers.get('accept-ranges'), 'bytes'); assert.equal(whole.headers.get('etag'), etag);
  assert.deepEqual(Buffer.from(await whole.arrayBuffer()), installer);
  // The rest from byte 1000, as the client asks after an interruption.
  const rest = await fetch(url, { headers: { Range: 'bytes=1000-', 'If-Range': etag } });
  assert.equal(rest.status, 206);
  assert.equal(rest.headers.get('content-range'), `bytes 1000-4095/4096`);
  assert.equal(rest.headers.get('content-length'), '3096');
  assert.deepEqual(Buffer.from(await rest.arrayBuffer()), installer.subarray(1000));
  const middle = await fetch(url, { headers: { Range: 'bytes=10-19' } });
  assert.equal(middle.status, 206); assert.deepEqual(Buffer.from(await middle.arrayBuffer()), installer.subarray(10, 20));
  // Another file (If-Range), several, reversed or malformed ranges: the whole file.
  for (const headers of [{ Range: 'bytes=1000-', 'If-Range': '"sha256-' + 'a'.repeat(64) + '"' }, { Range: 'bytes=0-1,5-9' }, { Range: 'bytes=20-10' }, { Range: 'bytes=-500' }, { Range: 'items=0-' }]) {
    const response = await fetch(url, { headers });
    assert.equal(response.status, 200, JSON.stringify(headers)); assert.equal((await response.arrayBuffer()).byteLength, installer.length);
  }
  const beyond = await fetch(url, { headers: { Range: 'bytes=4096-' } });
  assert.equal(beyond.status, 416); assert.equal(beyond.headers.get('content-range'), 'bytes */4096'); await beyond.arrayBuffer();
});

test('Download archives cannot escape the release directory through a symlink', { skip: process.platform === 'win32' }, async t => {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-download-link-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const outside = join(directory, 'outside.zip'), releaseDir = join(directory, 'releases');
  const { mkdir } = await import('node:fs/promises'); await mkdir(releaseDir);
  await writeFile(outside, 'outside');
  const filename = 'Multitap-Host-0.1.0-windows-x64.zip';
  await symlink(outside, join(releaseDir, filename));
  await writeFile(join(releaseDir, 'releases.json'), JSON.stringify([{ product: 'host', version: '0.1.0', filename, size: 7, sha256: 'a'.repeat(64) }]));
  const routes = downloadRoutes(releaseDir);
  const server = createServer((req, res) => routes(req, res, new URL(req.url, 'http://localhost').pathname));
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve)); t.after(() => new Promise(resolve => server.close(resolve)));
  assert.equal((await fetch('http://127.0.0.1:' + server.address().port + '/downloads/' + filename)).status, 404);
});
