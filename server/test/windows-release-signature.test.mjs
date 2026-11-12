// multitap-windows-release-signature-v1: the release tooling signs, the
// publisher re-verifies and keeps signatures immutable, and the broker passes
// them through to clients and signature-aware Hosts. Throwaway keys only; the
// project release key is never read here.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash, generateKeyPairSync } from 'node:crypto';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { downloadRoutes, publishedRelease, releaseSignature } from '../src/downloads.mjs';
import { bundleEntries, mergeDownloadCatalog } from '../src/download-bundle.mjs';

async function toolUrl(name) {
  for (const prefix of ['../../tools/', '../tools/']) {
    const url = new URL(prefix + name, import.meta.url);
    try { await readFile(url); return url; } catch (error) { if (error.code !== 'ENOENT') throw error; }
  }
  throw Error('Release tool missing from the test snapshot');
}
const toolPath = await toolUrl('windows-release-signature.mjs');
const tool = await import(toolPath.href);
const sha = bytes => createHash('sha256').update(bytes).digest('hex');
const keys = generateKeyPairSync('ed25519'), other = generateKeyPairSync('ed25519');
const rawKey = tool.rawPublicKey(keys.publicKey).toString('hex');

async function sampleArtifact(t, product = 'client', version = '0.4.90') {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-release-signature-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const filename = `Multitap${product === 'host' ? '-Host' : ''}-${version}-windows-x64.exe`, bytes = Buffer.concat([Buffer.from('MZ'), Buffer.alloc(4093, 7)]);
  await writeFile(join(directory, filename), bytes);
  const stored = await readFile(join(directory, filename));
  return { directory, entry: { product, version, filename, size: stored.length, sha256: sha(stored) } };
}

test('signing a sample artifact round-trips; every bound field, the key and the encoding are checked', async t => {
  const { entry } = await sampleArtifact(t);
  const signature = tool.signRelease(entry, keys.privateKey);
  assert.match(signature, /^[A-Za-z0-9+/]{86}==$/);
  assert.equal(tool.signRelease(entry, keys.privateKey), signature, 'Ed25519 is deterministic');
  assert.equal(tool.verifyRelease(entry, signature, keys.publicKey), true);
  assert.equal(tool.releaseMessage(entry).toString('ascii'),
    `multitap-windows-release-signature-v1\nproduct=client\nversion=0.4.90\nfilename=Multitap-0.4.90-windows-x64.exe\nsize=${entry.size}\nsha256=${entry.sha256}\n`);
  for (const patch of [{ size: entry.size + 1 }, { sha256: 'b'.repeat(64) }, { version: '0.4.91', filename: 'Multitap-0.4.91-windows-x64.exe' },
    { product: 'host', filename: 'Multitap-Host-0.4.90-windows-x64.exe' }]) assert.equal(tool.verifyRelease({ ...entry, ...patch }, signature, keys.publicKey), false, JSON.stringify(patch));
  assert.equal(tool.verifyRelease(entry, signature, other.publicKey), false, 'another key');
  const flipped = Buffer.from(signature, 'base64'); flipped[3] ^= 1;
  for (const bad of [undefined, '', 'x', flipped.toString('base64'), signature.slice(0, 85) + 'B==', signature + '=']) assert.equal(tool.verifyRelease(entry, bad, keys.publicKey), false, String(bad));
  // Nothing outside the catalog rules is ever signed (no line can be injected).
  for (const patch of [{ version: '0.4.90\nsize=1' }, { filename: 'other.exe' }, { product: 'server' }, { size: 0 }, { size: 128 * 1024 * 1024 + 1 }, { sha256: 'A'.repeat(64) }, { size: '4096' }])
    assert.throws(() => tool.signRelease({ ...entry, ...patch }, keys.privateKey), /release_signature_input_invalid/, JSON.stringify(patch));
  // The command line verifier agrees.
  const publicPath = join(tmpdir(), `multitap-release-public-${process.pid}-${Date.now()}.pem`);
  await writeFile(publicPath, keys.publicKey.export({ type: 'spki', format: 'pem' }));
  t.after(() => rm(publicPath, { force: true }));
  const cli = (...args) => spawnSync(process.execPath, [fileURLToPath(toolPath), 'verify', publicPath, entry.product, entry.version, entry.filename, String(entry.size), entry.sha256, ...args], { encoding: 'utf8' });
  assert.equal(cli(signature).status, 0);
  assert.equal(cli(flipped.toString('base64')).status, 1);
});

test('publication signs only with the key pinned in client/Host builds; a key is created once and never replaced', async t => {
  const { directory, entry } = await sampleArtifact(t, 'host', '0.2.90');
  const privatePath = join(directory, 'private.pem');
  await writeFile(privatePath, keys.privateKey.export({ type: 'pkcs8', format: 'pem' }));
  const header = pin => join(directory, `pin-${pin}.hpp`);
  await writeFile(header('own'), `inline constexpr char kReleasePublicKeyHex[] = "${rawKey}";\n`);
  await writeFile(header('other'), `inline constexpr char kReleasePublicKeyHex[] = "${tool.rawPublicKey(other.publicKey).toString('hex')}";\n`);
  const signature = await tool.signForPublication(privatePath, entry, header('own'));
  assert.equal(tool.verifyRelease(entry, signature, keys.publicKey), true);
  await assert.rejects(tool.signForPublication(privatePath, entry, header('other')), /differs from the key pinned/);
  // The repository pins a real 32-byte key in the native header (absent from
  // the server-only Linux snapshot).
  let pinned = null;
  try { pinned = await tool.pinnedPublicKey(); } catch (error) { if (error.code !== 'ENOENT') throw error; }
  if (pinned) assert.equal(pinned.length, 32);
  const keyDirectory = join(directory, 'release-signing');
  const created = await tool.createReleaseKey(keyDirectory), again = await tool.createReleaseKey(keyDirectory);
  assert.equal(created.created, true); assert.equal(again.created, false); assert.equal(again.publicKey, created.publicKey);
  assert.equal(tool.rawPublicKey(await readFile(join(keyDirectory, tool.publicKeyName))).toString('hex'), created.publicKey);
  await writeFile(join(keyDirectory, tool.publicKeyName), other.publicKey.export({ type: 'spki', format: 'pem' }));
  await assert.rejects(tool.createReleaseKey(keyDirectory), /mismatch/);
});

test('the broker passes a valid signature through and drops a malformed entry; old entries stay valid', async t => {
  const { entry } = await sampleArtifact(t);
  const signature = tool.signRelease(entry, keys.privateKey);
  assert.deepEqual(releaseSignature(entry), {}); assert.deepEqual(releaseSignature({ ...entry, signature }), { signature });
  assert.equal(publishedRelease({ ...entry, signature }).signature, signature);
  assert.equal(Object.hasOwn(publishedRelease(entry), 'signature'), false);
  for (const bad of ['', 'x', signature.slice(0, 85) + 'B==', 7, null]) assert.equal(publishedRelease({ ...entry, signature: bad }), null);
  assert.equal(publishedRelease({ ...entry, filename: 'Multitap-0.4.90-windows-x64.zip', signature }), null);
  const root = await mkdtemp(join(tmpdir(), 'multitap-signed-catalog-'));
  t.after(() => rm(root, { recursive: true, force: true }));
  const older = { product: 'client', version: '0.4.89', filename: 'Multitap-0.4.89-windows-x64.exe', size: 64, sha256: 'c'.repeat(64) };
  await writeFile(join(root, 'releases.json'), JSON.stringify([{ ...entry, signature }, older, { ...older, version: '0.4.88', filename: 'Multitap-0.4.88-windows-x64.exe', signature: 'bad' }]));
  const routes = downloadRoutes(root), server = createServer(async (req, res) => { if (!await routes(req, res, new URL(req.url, 'http://localhost').pathname)) { res.writeHead(404); res.end(); } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(() => new Promise(resolve => server.close(resolve)));
  const list = await (await fetch(`http://127.0.0.1:${server.address().port}/api/v1/downloads`)).json();
  assert.deepEqual(list.releases.map(e => [e.version, e.signature]), [['0.4.90', signature], ['0.4.89', undefined]]);
});

test('bundle seeding may add a signature to an unsigned entry but never change one', async t => {
  const { entry } = await sampleArtifact(t);
  const signature = tool.signRelease(entry, keys.privateKey), signed = { ...entry, signature };
  assert.equal(bundleEntries(Buffer.from(JSON.stringify([signed])))[0].signature, signature);
  const merged = mergeDownloadCatalog([entry], [signed]);
  assert.equal(merged.changed, true); assert.deepEqual(merged.entries, [signed]);
  const kept = mergeDownloadCatalog([signed], [entry]);
  assert.equal(kept.changed, false); assert.deepEqual(kept.entries, [signed]);
  assert.equal(mergeDownloadCatalog([signed], [signed]).changed, false);
  assert.throws(() => mergeDownloadCatalog([signed], [{ ...entry, signature: tool.signRelease(entry, other.privateKey) }]), /Immutable/);
});
