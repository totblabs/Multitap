import { test } from 'node:test';
import assert from 'node:assert/strict';
import { generateKeyPairSync, sign, createPublicKey } from 'node:crypto';
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { execFileSync } from 'node:child_process';
import { join } from 'node:path';
import { DockerReleaseCheck, dockerUpdateFeed, defaultDockerUpdateFeed, fetchFeed, dockerUpdateCommands, selectServerUpdate, serverUpdatePublicKey, publicRepository, publicImageRepository } from '../src/docker-update-check.mjs';
import { ServerUpdateClient } from '../src/server-update.mjs';
import { configuration } from '../src/config.mjs';
import { adminRoutes } from '../src/admin-http.mjs';

const keys = generateKeyPairSync('ed25519'), publicKey = keys.publicKey.export({ type: 'spki', format: 'pem' });
const feedUrl = 'https://github.com/fixture-owner/multitap/releases/latest/download/server-updates-docker.json';
const release = (version, extra = {}) => ({ schema: 1, product: 'multitap-broker', version, platform: 'linux-x64-docker', releasedAt: '2026-11-03T00:00:00.000Z', changelog: '서버 개선\n안정성 수정', artifact: { path: `/downloads/Multitap-Server-${version}-linux-x64-docker.tar.gz`, size: 4096, sha256: 'a'.repeat(64) }, ...extra });
const envelope = (value, key = keys.privateKey) => ({ release: value, signature: sign(null, Buffer.from(JSON.stringify(value)), key).toString('base64') });
const json = (value, init) => new Response(typeof value === 'string' ? value : JSON.stringify(value), init);
const redirect = location => new Response(null, { status: 302, headers: { Location: location } });

function check({ current = '0.5.125', feed = envelope(release('1.0.0')), now: start = Date.parse('2026-11-03T01:00:00.000Z'), ...options } = {}) {
  let now = start; const requests = [];
  const fetcher = options.fetcher ?? (async (url, init) => { requests.push({ url, init }); return typeof feed === 'function' ? feed(url) : json(feed); });
  const instance = new DockerReleaseCheck({ feed: feedUrl, currentVersion: current, publicKey, fetcher, now: () => now, ...options });
  return { instance, requests, advance: ms => { now += ms; } };
}

test('feed setting: project default until publication, off, or an https mirror without credentials or query', () => {
  // The public owner/repository are filled in at publication; a placeholder never fetches.
  if (/[<>]/.test(defaultDockerUpdateFeed)) assert.equal(dockerUpdateFeed(undefined), null);
  else assert.equal(dockerUpdateFeed(undefined), defaultDockerUpdateFeed);
  assert.match(defaultDockerUpdateFeed, /^https:\/\/github\.com\/[^/]+\/[^/]+\/releases\/latest\/download\/server-updates-docker\.json$/);
  assert.equal(defaultDockerUpdateFeed, `https://github.com/${publicRepository}/releases/latest/download/server-updates-docker.json`);
  assert.match(publicImageRepository, /^ghcr\.io\/[a-z0-9-]+\/multitap-broker$/, 'GHCR image names are lower case');
  assert.equal(dockerUpdateFeed(''), dockerUpdateFeed(undefined));
  assert.equal(dockerUpdateFeed('off'), null);
  assert.equal(dockerUpdateFeed('https://mirror.example.com/multitap/server-updates-docker.json'), 'https://mirror.example.com/multitap/server-updates-docker.json');
  for (const bad of ['http://mirror.example.com/feed.json', 'https://user:pass@mirror.example.com/feed.json', 'https://mirror.example.com/feed.json?x=1', 'https://mirror.example.com/feed.json#x', 'mirror.example.com/feed.json', 'OFF', ' https://mirror.example.com/feed.json'])
    assert.throws(() => dockerUpdateFeed(bad), /invalid_docker_update_feed/, bad);
  assert.equal(configuration({ MULTITAP_DOCKER_UPDATE_FEED: 'https://mirror.example.com/feed.json' }).dockerUpdateFeed, 'https://mirror.example.com/feed.json');
  assert.throws(() => configuration({ MULTITAP_DOCKER_UPDATE_FEED: 'http://mirror.example.com/feed.json' }), /invalid_docker_update_feed/);
});

test('only a Docker install without an updater socket gets the read-only check; the native updater path is unchanged', () => {
  const socket = '/run/multitap-update/control.sock', feed = 'https://mirror.example.com/feed.json';
  assert.ok(selectServerUpdate({ deploymentKind: 'systemd', updateSocket: socket, dockerUpdateFeed: feed }, '0.5.125') instanceof ServerUpdateClient);
  assert.ok(selectServerUpdate({ deploymentKind: 'docker', updateSocket: '/run/multitap-docker-update/control.sock', dockerUpdateFeed: feed }, '0.5.125') instanceof ServerUpdateClient);
  assert.ok(selectServerUpdate({ deploymentKind: 'docker', updateSocket: null, dockerUpdateFeed: feed }, '0.5.125') instanceof DockerReleaseCheck);
  assert.equal(selectServerUpdate({ deploymentKind: 'docker', updateSocket: null, dockerUpdateFeed: null }, '0.5.125'), null);
  for (const kind of ['systemd', 'external', 'unknown']) assert.equal(selectServerUpdate({ deploymentKind: kind, updateSocket: null, dockerUpdateFeed: feed }, '0.5.125'), null, kind);
});

test('a signed newer release is offered with the exact Compose commands and is never applied by the broker', async () => {
  const f = check(), result = await f.instance.discover();
  assert.equal(result.state, 'available'); assert.equal(result.feedConfigured, true); assert.equal(result.applySupported, false);
  assert.equal(result.channel, 'docker-release-feed'); assert.equal(result.currentVersion, '0.5.125');
  assert.deepEqual(result.latest, { version: '1.0.0', releasedAt: '2026-11-03T00:00:00.000Z', changelog: '서버 개선\n안정성 수정' });
  assert.match(result.releaseId, /^[a-f0-9]{64}$/);
  assert.deepEqual(result.manual.commands, ["sed -i '/^MULTITAP_VERSION=/d' .env && echo 'MULTITAP_VERSION=1.0.0' >> .env", 'docker compose pull && docker compose up -d']);
  assert.deepEqual(result.manual.sourceCommands, ['git pull', 'docker compose -f compose.yaml -f compose.build.yaml up -d --build']);
  assert.throws(() => f.instance.apply({ releaseId: result.releaseId, fromVersion: '0.5.125' }), error => error.status === 409 && error.code === 'update_manual_docker');
  // Only the signed metadata was read: never the archive, never a POST.
  assert.deepEqual(f.requests.map(r => r.url), [feedUrl]);
  assert.equal(f.requests[0].init.redirect, 'manual'); assert.equal(f.requests[0].init.method, undefined);
  assert.match(f.requests[0].init.headers['User-Agent'], /^Multitap-Broker\/0\.5\.125$/);
});

test('version comparison is numeric: 1.0.0 is newer than every 0.x, equal and older releases are current', async () => {
  const cases = [['0.5.125', '1.0.0', 'available'], ['0.99.999', '1.0.0', 'available'], ['1.0.0', '1.0.0', 'current'], ['1.0.0', '0.5.125', 'current'],
    ['1.0.9', '1.0.10', 'available'], ['1.0.10', '1.0.9', 'current'], ['9.99.99', '10.0.0', 'available'], ['1.2.3', '1.10.0', 'available']];
  for (const [current, offered, state] of cases) {
    const result = await check({ current, feed: envelope(release(offered)) }).instance.discover();
    assert.equal(result.state, state, `${current} -> ${offered}`);
    assert.equal(Object.hasOwn(result, 'manual'), state === 'available', 'commands appear only for a newer release');
  }
  assert.throws(() => dockerUpdateCommands("1.0.0'; rm -rf /"), /update_version_invalid/);
  assert.throws(() => new DockerReleaseCheck({ feed: feedUrl, currentVersion: '1.0', publicKey }), /update_version_invalid/);
});

test('only the pinned key, the exact signed bytes and the Docker platform are accepted', async () => {
  const other = generateKeyPairSync('ed25519').privateKey, tampered = envelope(release('1.0.0'));
  tampered.release.changelog = '변경된 내용';
  const cases = [
    [envelope(release('1.0.0'), other), 'update_signature_invalid'],
    [tampered, 'update_signature_invalid'],
    [{ ...envelope(release('1.0.0')), extra: true }, 'update_signature_invalid'],
    [envelope(release('1.0.0', { platform: 'linux-x64-systemd', artifact: { path: '/downloads/Multitap-Server-1.0.0-linux-x64.tar.gz', size: 1, sha256: 'a'.repeat(64) } })), 'update_platform_unsupported'],
    [envelope(release('1.0.0', { unexpected: 'field' })), 'update_manifest_invalid'],
    [envelope(release('1.0.0', { product: 'other' })), 'update_manifest_invalid'],
    ['{not json', 'update_manifest_invalid'],
    ['x'.repeat(40000), 'update_download_size'],
  ];
  for (const [feed, code] of cases) {
    const result = await check({ feed }).instance.discover();
    assert.equal(result.state, 'failed'); assert.equal(result.errorCode, code);
    assert.equal(result.latest, undefined); assert.equal(result.manual, undefined);
  }
  // The broker's pinned key is a valid Ed25519 key; a check without a test key uses it.
  assert.equal(createPublicKey(serverUpdatePublicKey).asymmetricKeyType, 'ed25519');
  const pinned = new DockerReleaseCheck({ feed: feedUrl, currentVersion: '0.5.125', fetcher: async () => json(envelope(release('1.0.0'))) });
  assert.equal((await pinned.discover()).errorCode, 'update_signature_invalid', 'a fixture key never verifies against the project key');
});

test('the pinned key is the project server-update key the release PC signs with', async t => {
  let directory;
  try { directory = join(execFileSync('git', ['rev-parse', '--path-format=absolute', '--git-common-dir'], { cwd: new URL('.', import.meta.url), encoding: 'utf8' }).trim(), '..', '.local', 'release-signing'); } catch { }
  let pem;
  try { pem = await readFile(join(directory ?? '', 'server-update-public.pem'), 'utf8'); } catch { }
  if (!pem) { t.skip('No release-signing public key on this machine (public checkouts have none).'); return; }
  assert.equal(createPublicKey(serverUpdatePublicKey).export({ type: 'spki', format: 'pem' }), createPublicKey(pem).export({ type: 'spki', format: 'pem' }));
});

test('redirects: https only, at most four hops, credentials refused, and other statuses fail', async () => {
  const hops = ['https://github.com/fixture-owner/multitap/releases/download/v1.0.0/server-updates-docker.json', 'https://objects.example.com/asset?token=signed'];
  const seen = [];
  const fetcher = async url => { seen.push(url); return url === feedUrl ? redirect(hops[0]) : url === hops[0] ? redirect(hops[1]) : json(envelope(release('1.0.0'))); };
  assert.equal(JSON.parse(await fetchFeed(feedUrl, { fetcher })).release.version, '1.0.0');
  assert.deepEqual(seen, [feedUrl, ...hops]);
  const relative = []; await fetchFeed(feedUrl, { fetcher: async url => { relative.push(url); return relative.length === 1 ? redirect('/fixture-owner/multitap/releases/download/v1/feed.json#frag') : json('{}'); } });
  assert.equal(relative[1], 'https://github.com/fixture-owner/multitap/releases/download/v1/feed.json');
  for (const location of ['http://objects.example.com/asset', 'https://user:secret@objects.example.com/asset', 'ftp://objects.example.com/asset', '']) {
    await assert.rejects(fetchFeed(feedUrl, { fetcher: async () => redirect(location) }), /update_feed_redirect_invalid/, location);
  }
  let count = 0;
  await assert.rejects(fetchFeed(feedUrl, { fetcher: async () => { count++; return redirect('https://loop.example.com/' + count); } }), /update_feed_redirect_invalid/);
  assert.equal(count, 5, 'the original request plus four redirects');
  for (const status of [404, 500, 304, 204]) await assert.rejects(fetchFeed(feedUrl, { fetcher: async () => new Response(status === 204 || status === 304 ? null : 'x', { status }) }), /update_download_failed/, String(status));
  await assert.rejects(fetchFeed(feedUrl, { fetcher: async () => { throw new TypeError('network'); } }), /update_download_failed/);
  await assert.rejects(fetchFeed(feedUrl, { fetcher: async () => json('{}', { headers: { 'Content-Length': '99999' } }) }), /update_download_size/);
  await assert.rejects(fetchFeed('http://github.com/feed.json', { fetcher: async () => assert.fail('no plain HTTP request') }), /update_feed_invalid/);
});

test('real fetch follows a redirect chain with a manual redirect policy and an overall deadline', async t => {
  const body = JSON.stringify(envelope(release('1.0.0'))), paths = [];
  const server = createServer((req, res) => {
    paths.push(req.url);
    if (req.url === '/latest') { res.writeHead(302, { Location: '/v1/feed.json' }); res.end(); }
    else if (req.url === '/v1/feed.json') { res.writeHead(302, { Location: '/asset?signed=1' }); res.end(); }
    else if (req.url === '/asset?signed=1') { res.writeHead(200, { 'Content-Type': 'application/octet-stream' }); res.end(body); }
    else if (req.url === '/slow') { res.writeHead(200); res.write('{'); }
    else { res.writeHead(404); res.end(); }
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { server.closeAllConnections(); await new Promise(resolve => server.close(resolve)); });
  const origin = `http://127.0.0.1:${server.address().port}`;
  const instance = new DockerReleaseCheck({ feed: origin + '/latest', currentVersion: '0.5.125', publicKey, allowInsecureHttp: true });
  const result = await instance.discover();
  assert.equal(result.state, 'available'); assert.deepEqual(paths, ['/latest', '/v1/feed.json', '/asset?signed=1']);
  const started = performance.now();
  await assert.rejects(fetchFeed(origin + '/slow', { allowInsecureHttp: true, timeoutMs: 150 }), /update_download_failed/);
  assert.ok(performance.now() - started < 3000);
});

test('discovery caches an hour, retries failures after a minute, deduplicates and lets the administrator recheck', async () => {
  let version = '1.0.0', down = false;
  const f = check({ feed: () => down ? new Response('x', { status: 503 }) : json(envelope(release(version))) });
  const [a, b] = await Promise.all([f.instance.discover(), f.instance.discover()]);
  assert.equal(a.state, 'available'); assert.deepEqual(b, a); assert.equal(f.requests.length, 1, 'one request for concurrent administrators');
  version = '1.0.1';
  f.advance(60 * 60 * 1000 - 1); assert.equal((await f.instance.discover()).latest.version, '1.0.0'); assert.equal(f.requests.length, 1);
  f.advance(1); assert.equal((await f.instance.discover()).latest.version, '1.0.1'); assert.equal(f.requests.length, 2);
  // 업데이트 확인 rechecks at once, but repeated presses within seconds reuse the answer.
  version = '1.0.2'; f.advance(10000);
  assert.deepEqual(f.instance.check(), { accepted: true }); assert.equal(f.instance.check().accepted, true);
  assert.equal((await f.instance.discover()).latest.version, '1.0.2'); assert.equal(f.requests.length, 3);
  f.advance(9999); f.instance.check(); assert.equal(f.requests.length, 3);
  down = true; f.advance(1); f.instance.check(); const failed = await f.instance.discover();
  assert.equal(failed.state, 'failed'); assert.equal(failed.errorCode, 'update_download_failed'); assert.equal(failed.manual, undefined);
  f.advance(59999); await f.instance.discover(); assert.equal(f.requests.length, 4);
  down = false; f.advance(1); assert.equal((await f.instance.discover()).state, 'available'); assert.equal(f.requests.length, 5);
});

test('a slow feed answers checking at once and settles for the next read', async () => {
  let release1; const pending = new Promise(resolve => { release1 = resolve; });
  const f = check({ waitMs: 20, feed: async () => { await pending; return json(envelope(release('1.0.0'))); } });
  const first = await f.instance.discover();
  assert.equal(first.state, 'checking'); assert.equal(first.applySupported, false);
  release1(); await f.instance.inflight;
  assert.equal((await f.instance.discover()).state, 'available');
});

test('server-info carries the Docker check to the console; the check never runs for a revoked administrator', async () => {
  const user = { id: 'fixture-owner', role: 'admin', must_change_credentials: 0 }, f = check();
  let authenticated = true, sent;
  const routes = adminRoutes({ store: { authenticate: () => authenticated ? user : null }, vault: {}, publicOrigin: 'https://fixture.invalid', serverInfo: { service: 'multitap-broker', version: '0.5.125', deployment: { kind: 'docker', configured: true } }, serverUpdate: f.instance });
  const req = { method: 'GET', headers: { cookie: '__Secure-multitap-admin=' + 'a'.repeat(43) } };
  await routes(req, {}, '/api/v1/admin/server-info', (status, body) => { sent = { status, body }; });
  assert.equal(sent.status, 200); assert.equal(sent.body.update.state, 'available'); assert.equal(sent.body.update.manual.version, '1.0.0');
  const revoked = check(); authenticated = false;
  const other = adminRoutes({ store: { authenticate: () => authenticated ? user : null }, vault: {}, serverInfo: {}, serverUpdate: revoked.instance });
  await assert.rejects(other(req, {}, '/api/v1/admin/server-info', () => assert.fail('no data for a signed-out administrator')), error => error.status === 401);
  assert.equal(revoked.requests.length, 0);
});

test('public compose files run the published image, keep the data volume and publish the UDP media port', async () => {
  const read = path => readFile(new URL('../../' + path, import.meta.url), 'utf8');
  const [compose, build, direct, env, docker, installer] = await Promise.all(['compose.yaml', 'compose.build.yaml', 'compose.direct-https.yaml', '.env.example', 'server/Dockerfile', 'deploy/docker/install.sh'].map(read));
  const version = JSON.parse(await readFile(new URL('../package.json', import.meta.url), 'utf8')).version;
  // The published image name follows publicRepository (docker-update-check.mjs) everywhere.
  assert.ok(compose.includes('image: ${MULTITAP_IMAGE:-' + publicImageRepository + '}:${MULTITAP_VERSION:-' + version + '}'));
  assert.ok(direct.includes('image: ${MULTITAP_IMAGE:-' + publicImageRepository + '}:${MULTITAP_VERSION:?'));
  assert.ok(env.includes('# MULTITAP_IMAGE=' + publicImageRepository + '\n') || env.includes('# MULTITAP_IMAGE=' + publicImageRepository + '\r\n'));
  assert.equal(compose.split('MULTITAP_VERSION:-').length - 1, 1, 'the release bump edits exactly one version default');
  assert.doesNotMatch(compose, /^\s+build:/m); assert.doesNotMatch(compose, /pull_policy/);
  // The server's days (모니터링) are local days. A container without TZ runs in
  // UTC; install.sh writes the machine's own zone into the TZ line of .env.
  for (const text of [compose, direct]) assert.match(text, /^      TZ: \$\{TZ:-UTC\}\r?$/m);
  assert.match(env, /^TZ=\r?$/m);
  assert.match(installer, /^\s+set_env TZ "\$zone"$/m);
  for (const text of [compose, direct]) {
    assert.match(text, /- broker-data:\/var\/lib\/multitap/);
    assert.match(text, /name: \$\{MULTITAP_DATA_VOLUME:-multitap-broker-data\}/);
    assert.match(text, /"\$\{MULTITAP_UDP_PUBLISH_PORT:-18481\}:\$\{MULTITAP_UDP_PORT:-18481\}\/udp"/);
    // The UDP relay listens like the native install (18481); the relay lists decide whether sessions use it.
    assert.ok(text.includes('MULTITAP_UDP_PORT: ${MULTITAP_UDP_PORT:-18481}'));
    for (const name of ['MULTITAP_DEPLOYMENT_KIND: docker', 'MULTITAP_DOCKER_UPDATE_FEED', 'MULTITAP_UDP_HOST_RELAYS', 'MULTITAP_UDP_CLIENT_RELAYS', 'MULTITAP_UDP_MEDIA']) assert.ok(text.includes(name), name);
    assert.doesNotMatch(text, /MULTITAP_UPDATE_SOCKET|docker\.sock/);
  }
  assert.match(build, /context: \.\/server/); assert.match(build, /pull_policy: build/); assert.doesNotMatch(build, /MULTITAP_VERSION/);
  assert.doesNotMatch(direct, /MULTITAP_VERSION:-/, 'the standalone file never carries a stale version default');
  assert.match(env, /^MULTITAP_VERSION=\r?$/m); assert.match(env, /COMPOSE_FILE=compose\.yaml:compose\.build\.yaml/);
  assert.match(docker, /EXPOSE 18481\/udp/);
  for (const text of [compose, build, direct, env]) assert.doesNotMatch(text, /totb\.me|10\.100\./);
});
