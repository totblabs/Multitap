// GitHub release tooling (docs/releasing.md): the download bundle the broker
// image workflow builds from release assets, and the Docker update feed signed
// on the release PC. Throwaway keys only; the project keys are read solely by
// the opt-in case at the end (MULTITAP_RELEASE_KEY_TEST=1 on the release PC).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash, generateKeyPairSync } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { mkdir, mkdtemp, readFile, readdir, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { DockerReleaseCheck, publicImageRepository, serverUpdatePublicKey } from '../src/docker-update-check.mjs';
import { decodeFeed } from '../update/protocol.mjs';
import { validateDownloadBundle } from '../src/download-bundle.mjs';

async function toolUrl(name) {
  for (const prefix of ['../../tools/', '../tools/']) {
    const url = new URL(prefix + name, import.meta.url);
    try { await readFile(url); return url; } catch (error) { if (error.code !== 'ENOENT') throw error; }
  }
  throw Error('Release tool missing from the test snapshot');
}
const { writeDockerFeed } = await import((await toolUrl('release/docker-feed.mjs')).href);
const { stageRelease, bundleRelease, inputName } = await import((await toolUrl('release/github-release-bundle.mjs')).href);
const { signRelease } = await import((await toolUrl('windows-release-signature.mjs')).href);
const sha = bytes => createHash('sha256').update(bytes).digest('hex');
const pem = pair => ({ privateKey: pair.privateKey.export({ type: 'pkcs8', format: 'pem' }), publicKey: pair.publicKey.export({ type: 'spki', format: 'pem' }) });

async function scratch(t, name) {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-' + name + '-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  return directory;
}

// The smallest broker tree tools/build-server-update.mjs packs and unpackPackage accepts.
async function brokerTree(root, version) {
  const files = {
    'package.json': JSON.stringify({ name: 'multitap-broker', version }), 'package-lock.json': JSON.stringify({ version, packages: { '': { version } } }),
    'src/index.mjs': '', 'src/http.mjs': '', 'src/store.mjs': '', 'src/bootstrap.mjs': '', 'admin/index.html': '', 'admin/app.js': '',
    'update/agent.mjs': '', 'update/provision-broker.sh': '', 'setup/proxmox.py': '', 'deps/node_modules/ws/package.json': '{"name":"ws"}',
  };
  for (const [name, text] of Object.entries(files)) { await mkdir(join(root, name, '..'), { recursive: true }); await writeFile(join(root, name), text); }
  return { server: root, dependencies: join(root, 'deps') };
}

async function feedInputs(t, { version = '1.0.0', image = publicImageRepository, imageVersion = version, pair = pem(generateKeyPairSync('ed25519')) } = {}) {
  const root = await scratch(t, 'docker-feed');
  const tree = await brokerTree(join(root, 'server'), version);
  await writeFile(join(root, 'key.pem'), pair.privateKey, { mode: 0o600 });
  await writeFile(join(root, 'notes.txt'), '서버 1.0.0\n공개 저장소 첫 릴리스');
  const metadata = { schema: 1, product: 'multitap-broker', version: imageVersion, image: image + '@sha256:' + 'b'.repeat(64), platform: 'linux/amd64', imageBytes: 123456789 };
  await writeFile(join(root, 'image.json'), JSON.stringify(metadata));
  return { ...tree, notes: join(root, 'notes.txt'), key: join(root, 'key.pem'), imageMetadata: join(root, 'image.json'), output: join(root, 'out'), publicKey: pair.publicKey, metadata };
}

test('the Docker feed is the signed linux-x64-docker release with the pushed image, and 1.0.0 is offered over 0.5.125', async t => {
  const input = await feedInputs(t), result = await writeDockerFeed(input);
  assert.equal(result.version, '1.0.0'); assert.equal(result.image, input.metadata.image); assert.match(result.releaseId, /^[a-f0-9]{64}$/);
  const bytes = await readFile(join(input.output, 'server-updates-docker.json'));
  const { release } = decodeFeed(bytes, input.publicKey);
  assert.equal(release.platform, 'linux-x64-docker'); assert.equal(release.changelog, '서버 1.0.0\n공개 저장소 첫 릴리스');
  assert.equal(release.artifact.path, '/downloads/Multitap-Server-1.0.0-linux-x64-docker.tar.gz');
  assert.deepEqual((await readdir(input.output)).sort(), ['Multitap-Server-1.0.0-linux-x64-docker.tar.gz', 'receipt.json', 'server-updates-docker.json']);
  // What a Docker install's console shows, from exactly these bytes.
  const offered = async current => new DockerReleaseCheck({ feed: 'https://github.com/fixture/multitap/releases/latest/download/server-updates-docker.json', currentVersion: current, publicKey: input.publicKey, fetcher: async () => new Response(bytes) }).discover();
  const update = await offered('0.5.125');
  assert.equal(update.state, 'available'); assert.equal(update.latest.version, '1.0.0');
  assert.deepEqual(update.manual.commands, ["sed -i '/^MULTITAP_VERSION=/d' .env && echo 'MULTITAP_VERSION=1.0.0' >> .env", 'docker compose pull && docker compose up -d']);
  assert.equal((await offered('1.0.0')).state, 'current');
  // Any other key, including the project's pinned one, rejects these fixture bytes.
  assert.throws(() => decodeFeed(bytes, serverUpdatePublicKey), /update_signature_invalid/);
});

test('the feed is refused for a key that is not pinned, another image repository or another version', async t => {
  // Default public key = the project's pinned key; a fixture private key never matches it.
  const unpinned = await feedInputs(t), { publicKey, ...rest } = unpinned;
  await assert.rejects(writeDockerFeed(rest), /differs from the pinned server-update public key/);
  await assert.rejects(readdir(unpinned.output), { code: 'ENOENT' }, 'nothing is written before the key check');
  await assert.rejects(writeDockerFeed(await feedInputs(t, { image: 'ghcr.io/someone-else/multitap-broker' })), /update_docker_image_invalid/);
  await assert.rejects(writeDockerFeed(await feedInputs(t, { imageVersion: '0.9.9' })), /update_docker_image_invalid/);
});

// Synthetic installers with the receipts the release tools write (see tools/oss/docker-smoke.sh).
async function releaseFiles(t, windowsKey, versions = { client: '1.0.0', host: '1.0.0', android: '1.0.0' }) {
  const source = await scratch(t, 'release-src'), catalog = [];
  for (const product of ['client', 'host']) {
    const version = versions[product], filename = `Multitap${product === 'host' ? '-Host' : ''}-${version}-windows-x64.exe`, bytes = Buffer.from('fixture ' + filename);
    const entry = { product, version, filename, size: bytes.length, sha256: sha(bytes) };
    const files = (product === 'host' ? ['bin/MultitapHostSettings.exe', 'multitap-host.json'] : ['bin/Multitap.exe', 'MultitapClientSetup.exe']).map(path => ({ path, size: 1, sha256: 'a'.repeat(64) }));
    await writeFile(join(source, filename), bytes); await writeFile(join(source, filename + '.json'), JSON.stringify({ ...entry, payloadVerified: true, files }));
    catalog.push({ ...entry, format: 'installer', url: '/downloads/' + filename, changelog: product + ' notes', releasedAt: '2026-11-03T00:00:00.000Z', signature: signRelease(entry, windowsKey) });
  }
  for (const abi of ['arm64-v8a', 'armeabi-v7a', 'x86_64']) {
    const version = versions.android, filename = `Multitap-${version}-android-${abi}.apk`, bytes = Buffer.from('fixture ' + filename);
    const entry = { product: 'android-client', version, abi, filename, size: bytes.length, sha256: sha(bytes) };
    await writeFile(join(source, filename), bytes);
    await writeFile(join(source, filename + '.verification.json'), JSON.stringify({ policyVersion: 1, ...entry, signerSha256: 'c'.repeat(64), payloadVerified: true, signatureVerified: true, contentSignaturesVerified: true, minSdk: 26, targetSdk: 36, versionCode: 100, entryCount: 1, expandedBytes: 1 }));
    catalog.push({ ...entry, format: 'apk', url: '/downloads/' + filename });
  }
  // An older release in the same broker catalog is never picked.
  catalog.push({ product: 'client', version: '0.4.179', filename: 'Multitap-0.4.179-windows-x64.exe', size: 1, sha256: 'd'.repeat(64), format: 'installer' });
  await writeFile(join(source, 'catalog.json'), JSON.stringify({ releases: catalog }));
  return { source, catalog: join(source, 'catalog.json') };
}

test('stage picks the five catalog entries and receipts; bundle verifies release signatures and builds the image bundle', async t => {
  const windows = generateKeyPairSync('ed25519'), { source, catalog } = await releaseFiles(t, windows.privateKey);
  const out = join(await scratch(t, 'release-assets'), 'assets');
  const staged = await stageRelease(catalog, ['1.0.0', '1.0.0', '1.0.0'], out, [join(source, 'missing'), source], { publicKey: windows.publicKey });
  assert.equal(staged.files.length, 11);
  assert.deepEqual((await readdir(out)).sort(), [...staged.files].sort());
  const input = JSON.parse(await readFile(join(out, inputName), 'utf8'));
  assert.deepEqual(input.map(e => e.filename), staged.files.filter(f => /\.(exe|apk)$/.test(f)));
  assert.ok(input.every(e => !('url' in e)), 'catalog URLs are not part of the upload');
  const bundle = join(await scratch(t, 'release-bundle'), 'release-bundle');
  await bundleRelease(out, bundle, { publicKey: windows.publicKey });
  const validated = await validateDownloadBundle(bundle);
  assert.equal(validated.entries.length, 5);
  assert.ok(validated.entries.filter(e => e.product !== 'android-client').every(e => /^[A-Za-z0-9+/]{86}==$/.test(e.signature)), 'clients refuse unsigned updates');
  // A second stage never mixes with an earlier upload folder.
  await assert.rejects(stageRelease(catalog, ['1.0.0', '1.0.0', '1.0.0'], out, [source], { publicKey: windows.publicKey }), { code: 'EEXIST' });
});

test('bundle refuses an unsigned installer, another key and the project key for fixture signatures', async t => {
  const windows = generateKeyPairSync('ed25519'), { source, catalog } = await releaseFiles(t, windows.privateKey);
  const root = await scratch(t, 'release-refuse'), out = join(root, 'assets');
  await stageRelease(catalog, ['1.0.0', '1.0.0', '1.0.0'], out, [source], { publicKey: windows.publicKey });
  // Default key = native/windows_admin/release_signature.hpp, which never signed these fixtures.
  await assert.rejects(bundleRelease(out, join(root, 'b1')), /Multitap-1\.0\.0-windows-x64\.exe: missing or wrong release signature/);
  await assert.rejects(bundleRelease(out, join(root, 'b2'), { publicKey: generateKeyPairSync('ed25519').publicKey }), /missing or wrong release signature/);
  const input = JSON.parse(await readFile(join(out, inputName), 'utf8'));
  delete input[1].signature; await writeFile(join(out, inputName), JSON.stringify(input));
  await assert.rejects(bundleRelease(out, join(root, 'b3'), { publicKey: windows.publicKey }), /Multitap-Host-1\.0\.0-windows-x64\.exe: missing or wrong release signature/);
  await assert.rejects(readdir(join(root, 'b3')), { code: 'ENOENT' }, 'no bundle is written');
});

test('stage refuses a version missing from the catalog, a missing receipt and unsafe names', async t => {
  const windows = generateKeyPairSync('ed25519'), { source, catalog } = await releaseFiles(t, windows.privateKey);
  const root = await scratch(t, 'release-stage');
  await assert.rejects(stageRelease(catalog, ['1.0.1', '1.0.0', '1.0.0'], join(root, 'a'), [source]), /Multitap-1\.0\.1-windows-x64\.exe: 0 catalog entries/);
  await rm(join(source, 'Multitap-1.0.0-android-x86_64.apk.verification.json'));
  await assert.rejects(stageRelease(catalog, ['1.0.0', '1.0.0', '1.0.0'], join(root, 'b'), [source], { publicKey: windows.publicKey }), /x86_64\.apk\.verification\.json is in none of the source dirs/);
  await assert.rejects(stageRelease(catalog, ['../1.0.0', '1.0.0', '1.0.0'], join(root, 'c'), [source]), /Unsafe release file name/);
});

// Release PC only: the real server-update key signs a harmless 0.0.1 feed that
// verifies with the key pinned in the broker. Opt-in, so test runs elsewhere
// never touch the private key.
test('the project server-update key signs a feed the pinned broker key accepts', async t => {
  if (process.env.MULTITAP_RELEASE_KEY_TEST !== '1') { t.skip('set MULTITAP_RELEASE_KEY_TEST=1 on the release PC'); return; }
  const common = join(execFileSync('git', ['rev-parse', '--path-format=absolute', '--git-common-dir'], { cwd: new URL('.', import.meta.url), encoding: 'utf8' }).trim(), '..');
  const input = await feedInputs(t, { version: '0.0.1' }), { publicKey, ...rest } = input;
  const result = await writeDockerFeed({ ...rest, key: join(common, '.local', 'release-signing', 'server-update-private.pem') });
  assert.equal(decodeFeed(await readFile(join(input.output, 'server-updates-docker.json')), serverUpdatePublicKey).releaseId, result.releaseId);
});
