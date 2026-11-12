// Download bundle of a GitHub release (docs/releasing.md).
//
//   node tools/release/github-release-bundle.mjs stage <catalog.json> <client version> <host version>
//        <android version> <new assets dir> <source dir>...
//     Release PC. Picks the five installers from a broker catalog (the
//     /api/v1/downloads answer of the broker they were published to: sizes,
//     SHA-256, notes, release signatures), copies each one and its build receipt
//     (.exe.json, .apk.verification.json) from the source dirs, writes
//     multitap-release-input.json and runs `bundle` on a scratch copy. Upload
//     every file of the new dir to the release.
//
//   node tools/release/github-release-bundle.mjs bundle <assets dir> <new bundle dir>
//     .github/workflows/broker-image.yml. Every Windows installer must carry the
//     project release signature for the key pinned in client and Host builds
//     (clients refuse unsigned updates); then server/src/download-bundle.mjs
//     builds and validates the bundle with the same rules as the image build.
//     The Windows payload policy and apksigner checks stay on the release PC;
//     here the build receipts carry them.
import { copyFile, mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { constants } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { createPublicKey } from 'node:crypto';
import { androidAbis, bundleEntries, createDownloadBundle } from '../../server/src/download-bundle.mjs';
import { pinnedPublicKey, verifyRelease } from '../windows-release-signature.mjs';

export const inputName = 'multitap-release-input.json';
const receiptName = entry => entry.filename + (entry.product === 'android-client' ? '.verification.json' : '.json');
const rawKey = raw => createPublicKey({ key: { kty: 'OKP', crv: 'Ed25519', x: Buffer.from(raw).toString('base64url') }, format: 'jwk' });

export async function bundleRelease(assets, output, { publicKey } = {}) {
  const key = publicKey ?? rawKey(await pinnedPublicKey());
  const entries = bundleEntries(await readFile(join(assets, inputName)), { bundleOnly: true });
  for (const entry of entries.filter(e => e.product !== 'android-client')) {
    if (!verifyRelease(entry, entry.signature, key)) throw Error(`${entry.filename}: missing or wrong release signature`);
  }
  const bundle = await createDownloadBundle(join(assets, inputName), assets, output);
  return { bundle: output, releases: bundle.entries.map(e => e.filename) };
}

export async function stageRelease(catalogPath, [client, host, android], output, sources, options) {
  const catalog = JSON.parse(await readFile(catalogPath, 'utf8')), all = Array.isArray(catalog) ? catalog : catalog?.releases;
  if (!Array.isArray(all)) throw Error('Catalog must be a release array or a /api/v1/downloads answer');
  const wanted = [`Multitap-${client}-windows-x64.exe`, `Multitap-Host-${host}-windows-x64.exe`, ...androidAbis.map(abi => `Multitap-${android}-android-${abi}.apk`)];
  if (wanted.some(name => !/^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(name) || name.includes('..'))) throw Error('Unsafe release file name');
  const entries = wanted.map(filename => {
    const found = all.filter(e => e?.filename === filename);
    if (found.length !== 1) throw Error(`${filename}: ${found.length} catalog entries`);
    const { url, ...entry } = found[0]; // the bundle tool derives the URL
    return entry;
  });
  await mkdir(output); // a new dir only: never mixes with an older upload
  try {
    for (const entry of entries) for (const name of [entry.filename, receiptName(entry)]) {
      let copied = false;
      for (const dir of sources) {
        try { await copyFile(join(dir, name), join(output, name), constants.COPYFILE_EXCL); copied = true; break; }
        catch (error) { if (error.code !== 'ENOENT') throw error; }
      }
      if (!copied) throw Error(`${name} is in none of the source dirs`);
    }
    await writeFile(join(output, inputName), JSON.stringify(entries, null, 2) + '\n', { flag: 'wx' });
    // The workflow's own check, before anything is uploaded.
    const scratch = await mkdtemp(join(tmpdir(), 'multitap-release-check-'));
    try { await bundleRelease(output, join(scratch, 'bundle'), options); } finally { await rm(scratch, { recursive: true, force: true }); }
  } catch (error) { await rm(output, { recursive: true, force: true }); throw error; } // only the dir created above
  return { assets: output, files: [inputName, ...entries.flatMap(e => [e.filename, receiptName(e)])] };
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  try {
    const [action, ...args] = process.argv.slice(2);
    let result;
    if (action === 'bundle' && args.length === 2) result = await bundleRelease(resolve(args[0]), resolve(args[1]));
    else if (action === 'stage' && args.length >= 6) result = await stageRelease(resolve(args[0]), args.slice(1, 4), resolve(args[4]), args.slice(5).map(dir => resolve(dir)));
    else throw Error('Expected stage <catalog.json> <client version> <host version> <android version> <new assets dir> <source dir>... | bundle <assets dir> <new bundle dir>');
    console.log(JSON.stringify(result, null, 2));
  } catch (error) { console.error('Multitap GitHub release bundle: ' + error.message); process.exitCode = 1; }
}
