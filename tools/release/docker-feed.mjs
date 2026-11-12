// Docker update feed for a GitHub release (docs/releasing.md):
//
//   node tools/release/docker-feed.mjs <server dir> <production deps root> <UTF-8 notes>
//        <server-update private key> <multitap-broker-image-<version>.json> <new output dir>
//
// Runs on the release PC after .github/workflows/broker-image.yml pushed the
// image and attached its metadata (repository@sha256 digest, size). It signs
// that digest, the version and the notes with the project's server-update key
// through tools/build-server-update.mjs (platform linux-x64-docker), then checks
// the result the way Docker installs do: the pinned public key in
// server/src/docker-update-check.mjs. CI never holds the key; the maintainer
// attaches the written server-updates-docker.json to the release.
import { spawnSync } from 'node:child_process';
import { createPrivateKey, createPublicKey } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { publicImageRepository, serverUpdatePublicKey, dockerUpdatePlatform } from '../../server/src/docker-update-check.mjs';
import { decodeFeed, digest } from '../../server/update/protocol.mjs';
import { validateDockerImage } from '../../server/update/docker.mjs';
import { unpackPackage } from '../../server/update/package.mjs';

const buildTool = fileURLToPath(new URL('../build-server-update.mjs', import.meta.url));
const spki = key => createPublicKey(key).export({ type: 'spki', format: 'pem' });

export async function writeDockerFeed({ server, dependencies, notes, key, imageMetadata, output, repository = publicImageRepository, publicKey = serverUpdatePublicKey }) {
  const version = JSON.parse(await readFile(join(server, 'package.json'), 'utf8')).version;
  const metadataBytes = await readFile(imageMetadata);
  // The image the workflow pushed for exactly this broker version, in the public repository.
  const metadata = validateDockerImage(JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(metadataBytes)), version, repository);
  // Refuse before signing: any other key would publish a feed no installation accepts.
  if (spki(createPrivateKey(await readFile(key))) !== spki(publicKey)) throw Error('Signing key differs from the pinned server-update public key');
  const run = spawnSync(process.execPath, [buildTool, server, dependencies, notes, key, output, imageMetadata], { encoding: 'utf8' });
  if (run.status !== 0) throw Error('build-server-update.mjs failed: ' + (run.stderr || run.stdout).trim().split('\n').at(-1));
  // Read back what an installation reads.
  const feedPath = join(output, 'server-updates-docker.json');
  const { release, releaseId } = decodeFeed(await readFile(feedPath), publicKey);
  if (release.platform !== dockerUpdatePlatform || release.version !== version) throw Error('Written feed is not the Docker release of ' + version);
  const archive = await readFile(join(output, release.artifact.path.split('/').at(-1)));
  if (archive.length !== release.artifact.size || digest(archive) !== release.artifact.sha256) throw Error('Written archive differs from the signed release');
  if (!unpackPackage(archive, version).get('update/docker-image.json')?.equals(Buffer.from(JSON.stringify(metadata)))) throw Error('Signed archive does not carry the image metadata');
  return { version, releaseId, image: metadata.image, imageBytes: metadata.imageBytes, feed: feedPath };
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  try {
    const args = process.argv.slice(2);
    if (args.length !== 6) throw Error('Usage: docker-feed.mjs <server dir> <production deps root> <notes> <private key> <image metadata json> <new output dir>');
    const [server, dependencies, notes, key, imageMetadata, output] = args.map(value => resolve(value));
    console.log(JSON.stringify(await writeDockerFeed({ server, dependencies, notes, key, imageMetadata, output })));
  } catch (error) { console.error('Multitap Docker feed: ' + error.message); process.exitCode = 1; }
}
