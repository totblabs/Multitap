import test from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import { generateKeyPairSync, sign } from 'node:crypto';
import { feedUrl, fetchBytes, decodeFeed, digest } from '../update/protocol.mjs';
import { packPackage, unpackPackage } from '../update/package.mjs';
import { UpdateEngine } from '../update/engine.mjs';

test('actual HTTP signed feed and full artifact require explicit opt-in and preserve signature, digest and no-redirect validation', async t => {
  const keys = generateKeyPairSync('ed25519'), publicKey = keys.publicKey.export({ type: 'spki', format: 'pem' }), version = '0.5.84';
  const files = new Map(['src/index.mjs', 'src/http.mjs', 'src/store.mjs', 'src/bootstrap.mjs', 'admin/index.html', 'admin/app.js', 'update/agent.mjs', 'update/provision-broker.sh', 'node_modules/ws/package.json'].map(name => [name, Buffer.from('// fixture, never executed\n')]));
  files.set('package.json', Buffer.from(JSON.stringify({ name: 'multitap-broker', version }))); files.set('package-lock.json', Buffer.from(JSON.stringify({ version, packages: { '': { version } } })));
  const archive = packPackage(files), artifact = { path: '/downloads/Multitap-Server-0.5.84-linux-x64.tar.gz', size: archive.length, sha256: digest(archive) };
  const release = { schema: 1, product: 'multitap-broker', version, platform: 'linux-x64-systemd', releasedAt: '2026-10-29T00:00:00.000Z', changelog: 'Explicit HTTP feed keeps signed metadata and exact file verification.', artifact };
  const feed = Buffer.from(JSON.stringify({ release, signature: sign(null, Buffer.from(JSON.stringify(release)), keys.privateKey).toString('base64') }));
  let requests = 0, followed = 0;
  const server = http.createServer((req, res) => { requests++; if (req.url === '/redirect') { res.writeHead(302, { Location: '/must-not-follow' }); return res.end(); } if (req.url === '/must-not-follow') followed++; const bytes = req.url === '/downloads/server-updates.json' ? feed : archive; res.writeHead(200, { 'Content-Length': bytes.length }); res.end(bytes); });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { server.closeAllConnections(); await new Promise(resolve => server.close(resolve)); });
  const origin = 'http://127.0.0.1:' + server.address().port, url = origin + '/downloads/server-updates.json';
  assert.throws(() => feedUrl(url), /feed_invalid/); assert.throws(() => new UpdateEngine({ directory: '/not-used', feed: url, publicKey, adapter: {} }), /feed_invalid/);
  await assert.rejects(fetchBytes(url, 32768), /download_failed/); assert.equal(requests, 0);
  const options = { allowInsecureHttp: true }, parsed = decodeFeed(await fetchBytes(feedUrl(url, options), 32768, fetch, () => {}, options), publicKey);
  const bytes = await fetchBytes(new URL(parsed.release.artifact.path, url), artifact.size, fetch, () => {}, options);
  assert.equal(bytes.length, artifact.size); assert.equal(digest(bytes), artifact.sha256); assert.equal(unpackPackage(bytes, version).size, files.size);
  const changed = JSON.parse(feed); changed.release.changelog += ' changed'; assert.throws(() => decodeFeed(Buffer.from(JSON.stringify(changed)), publicKey), /signature/);
  await assert.rejects(fetchBytes(origin + '/redirect', 32768, fetch, () => {}, options), /download_failed/); assert.equal(followed, 0);
  // Exercise the production check path and propagation of explicit HTTP, with
  // memory status only. It does not apply code or claim an operating install.
  const engine = new UpdateEngine({ directory: '/not-used', feed: url, publicKey, allowInsecureHttp: true, adapter: { platform: 'linux-x64-systemd', version: async () => '0.5.83' } });
  engine.set = async values => { engine.state = { ...engine.state, ...values }; };
  engine.check(); await engine.task; assert.equal(engine.state.phase, 'available'); assert.equal(engine.state.releaseId, parsed.releaseId);
});
