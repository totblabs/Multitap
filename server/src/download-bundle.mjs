// Image build/startup tooling only. This module never opens the broker DB/vault.
import { open, lstat, realpath, readdir, mkdir, copyFile, link, unlink, rename, chmod, chown } from 'node:fs/promises';
import { constants } from 'node:fs';
import { createHash, randomUUID } from 'node:crypto';
import { resolve, join, dirname } from 'node:path';
import { pathToFileURL } from 'node:url';
import { publishedRelease, maxCatalogEntries, maxReleaseManifestBytes, maxDownloadSize } from './downloads.mjs';
import { releaseHistoryMerge, releaseHistoryWrite, releaseHistoryRecord, releaseHistoryFileName, releaseHistoryReadRecord } from './release-history-core.mjs';

const hash = bytes => createHash('sha256').update(bytes).digest('hex');
const samePath = (a, b) => process.platform === 'win32' ? a.toLowerCase() === b.toLowerCase() : a === b;
const jsonBytes = value => Buffer.from(JSON.stringify(value, null, 2) + '\n');
const regular = stat => stat.isFile() && !stat.isSymbolicLink() && stat.nlink === 1;
const proofName = filename => filename + '.receipt.json';
function parseReleaseJson(bytes) {
  try { return JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes)); }
  catch { throw Error('Invalid release JSON or UTF-8'); } // Never echo input fragments into startup logs.
}

async function exactDirectory(directory) {
  const path = resolve(directory), stat = await lstat(path);
  if (!stat.isDirectory() || stat.isSymbolicLink() || !samePath(await realpath(path), path)) throw Error('Unsafe release directory');
  return path;
}

async function safeFile(path, cap, consume) {
  const before = await lstat(path);
  if (!regular(before) || before.size > cap || !samePath(await realpath(path), resolve(path))) throw Error('Unsafe release file');
  const handle = await open(path, constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0));
  try {
    const stat = await handle.stat();
    if (!regular(stat) || stat.dev !== before.dev || stat.ino !== before.ino || stat.size !== before.size) throw Error('Release file changed');
    const result = await consume(handle, stat);
    const after = await handle.stat();
    if (after.size !== stat.size || after.mtimeMs !== stat.mtimeMs || after.ctimeMs !== stat.ctimeMs) throw Error('Release file changed');
    return result;
  } finally { await handle.close(); }
}

async function boundedBytes(path, cap) {
  return safeFile(path, cap, async (handle, stat) => {
    const bytes = await handle.readFile();
    if (bytes.length !== stat.size || bytes.length > cap) throw Error('Release file size changed');
    return bytes;
  });
}

async function fileIntegrity(path, entry) {
  return safeFile(path, maxDownloadSize, async (handle, stat) => {
    if (stat.size !== entry.size) throw Error('Immutable release size conflict');
    const digest = createHash('sha256'); let count = 0;
    for await (const chunk of handle.createReadStream({ autoClose: false })) {
      count += chunk.length;
      if (count > entry.size) throw Error('Immutable release size conflict');
      digest.update(chunk);
    }
    if (count !== entry.size || digest.digest('hex') !== entry.sha256) throw Error('Immutable release hash conflict');
    return { dev: stat.dev, ino: stat.ino };
  });
}

// An image bundle carries the Windows client and Host installers and the
// Android APKs; never ZIP archives or other catalog formats.
const bundleFormat = release => (release.format === 'installer' && ['client', 'host'].includes(release.product)) || (release.format === 'apk' && release.product === 'android-client');
export const androidAbis = Object.freeze(['arm64-v8a', 'armeabi-v7a', 'x86_64']);

export function bundleEntries(bytes, { bundleOnly = false } = {}) {
  if (bytes.length > maxReleaseManifestBytes) throw Error('Release manifest exceeds limit');
  const entries = parseReleaseJson(bytes);
  if (!Array.isArray(entries) || entries.length > maxCatalogEntries) throw Error('Invalid release catalog');
  const names = new Set();
  for (const entry of entries) {
    const release = publishedRelease(entry);
    if (!release || names.has(entry.filename) || (bundleOnly && !bundleFormat(release))) throw Error('Invalid release catalog entry');
    names.add(entry.filename);
  }
  return entries;
}

function manifestEntry(entry) {
  const { url, format, ...publicEntry } = publishedRelease(entry);
  return entry.product === 'android-client' ? { ...publicEntry, format } : publicEntry;
}

// Keep only the reproducible payload inventory, never local paths/runtime state.
export function installerReceipt(raw, entry) {
  if (!raw || raw.payloadVerified !== true || raw.product !== entry.product || raw.version !== entry.version ||
    raw.filename !== entry.filename || raw.size !== entry.size || raw.sha256 !== entry.sha256 || !Array.isArray(raw.files) || !raw.files.length || raw.files.length > 4096) throw Error('Installer provenance mismatch');
  const allowed = new Set(entry.product === 'host'
    ? ['MultitapHost.exe','MultitapHostTool.exe','MultitapHostSettings.exe','MultitapDisplay.exe']
    : ['Multitap.exe','MultitapClientSetup.exe']);
  const paths = new Set();
  const files = raw.files.map(file => {
    if (!file || typeof file.path !== 'string' || file.path.length > 512 || file.path.includes('\\') ||
      /(^|\/)\.\.?($|\/)|^[\/]|:|[\u0000-\u001f\u007f]/u.test(file.path) ||
      /(^|\/)(settings\.json|login\.json|vault\.key|broker\.sqlite[^/]*|initial-account\.json|host\.dat|host-token[^/]*|credentials\.dpapi)$|\.(h264|frames|dpapi|pdb)$/i.test(file.path) ||
      (/\.exe$/i.test(file.path) && !allowed.has(file.path.split('/').at(-1))) ||
      typeof file.sha256 !== 'string' || !/^[a-f0-9]{64}$/.test(file.sha256) || !Number.isSafeInteger(file.size) || file.size < 0 || file.size > 1024 * 1024 * 1024 || paths.has(file.path.toLowerCase())) throw Error('Unsafe installer provenance inventory');
    paths.add(file.path.toLowerCase());
    return { path: file.path, size: file.size, sha256: file.sha256 };
  });
  // The Host installer carries an MSI package (tools/release/host-installer-receipt.ps1: multitap-host.json at the root).
  for (const required of entry.product === 'host' ? ['bin/MultitapHostSettings.exe','multitap-host.json'] : ['bin/Multitap.exe','MultitapClientSetup.exe']) {
    if (!paths.has(required.toLowerCase())) throw Error('Incomplete installer provenance');
  }
  return { product: entry.product, version: entry.version, filename: entry.filename, size: entry.size, sha256: entry.sha256, payloadVerified: true, files };
}

// The APK's verification receipt from tools/verify-android-release.ps1
// (<apk>.verification.json): signature, signer, package/SDK identity and the
// payload scan all passed for exactly these bytes. Only fixed fields are kept;
// the bundle copy passes this same check again.
const sdkLevel = value => Number.isSafeInteger(value) && value >= 21 && value <= 100;
export function androidReceipt(raw, entry) {
  if (!raw || entry.product !== 'android-client' || raw.policyVersion !== 1 || raw.product !== 'android-client' || raw.version !== entry.version || raw.abi !== entry.abi ||
    raw.filename !== entry.filename || raw.size !== entry.size || raw.sha256 !== entry.sha256 ||
    raw.payloadVerified !== true || raw.signatureVerified !== true || raw.contentSignaturesVerified !== true ||
    typeof raw.signerSha256 !== 'string' || !/^[a-f0-9]{64}$/.test(raw.signerSha256) || !sdkLevel(raw.minSdk) || !sdkLevel(raw.targetSdk) || raw.minSdk > raw.targetSdk ||
    !Number.isSafeInteger(raw.versionCode) || raw.versionCode < 1 || raw.versionCode > 2100000000 ||
    !Number.isSafeInteger(raw.entryCount) || raw.entryCount < 1 || raw.entryCount > 8192 ||
    !Number.isSafeInteger(raw.expandedBytes) || raw.expandedBytes < 1 || raw.expandedBytes > 512 * 1024 * 1024) throw Error('APK provenance mismatch');
  const { product, version, abi, filename, size, sha256, signerSha256, minSdk, targetSdk, versionCode, entryCount, expandedBytes } = raw;
  return { policyVersion: 1, product, version, abi, filename, size, sha256, signerSha256, payloadVerified: true, signatureVerified: true, contentSignaturesVerified: true, minSdk, targetSdk, versionCode, entryCount, expandedBytes };
}
export const bundleReceipt = (raw, entry) => entry.product === 'android-client' ? androidReceipt(raw, entry) : installerReceipt(raw, entry);
// Build outputs: <exe>.json from the Windows build, <apk>.verification.json from the Android verifier.
const sourceReceiptName = entry => entry.filename + (entry.product === 'android-client' ? '.verification.json' : '.json');

function requireProducts(entries, allowEmpty) {
  if (entries.length === 0 && allowEmpty) return;
  const android = new Map();
  for (const entry of entries) if (entry.product === 'android-client') android.set(entry.version, (android.get(entry.version) ?? new Set()).add(entry.abi));
  if (!entries.some(e => e.product === 'client') || !entries.some(e => e.product === 'host') || android.size === 0) throw Error('Bundle requires the Windows client and Host installers and the Android APKs; use explicit allow-empty only for development');
  // One Android version is one APK per ABI; /get/android and the app's updater pick by ABI.
  for (const abis of android.values()) if (abis.size !== androidAbis.length) throw Error('Bundle Android release must carry every ABI (' + androidAbis.join(', ') + ')');
}
// One application id has one signer; a bundle never mixes them.
function requireOneAndroidSigner(receipts) {
  if (new Set(receipts.filter(r => r.product === 'android-client').map(r => r.signerSha256)).size > 1) throw Error('Bundle Android APKs have different signers');
}

export async function validateDownloadBundle(directory, { allowEmpty = false } = {}) {
  const root = await exactDirectory(directory);
  const bytes = await boundedBytes(join(root, 'releases.json'), maxReleaseManifestBytes);
  const entries = bundleEntries(bytes, { bundleOnly: true });
  requireProducts(entries, allowEmpty);
  const proof = parseReleaseJson(await boundedBytes(join(root, 'bundle.json'), maxReleaseManifestBytes));
  if (proof?.schema !== 1 || proof.kind !== 'multitap-release-bundle' || proof.manifestSha256 !== hash(bytes) ||
    proof.empty !== (entries.length === 0) || !Array.isArray(proof.files) || proof.files.length !== entries.length) throw Error('Bundle provenance mismatch');
  const expected = new Set(['releases.json','bundle.json']), receipts = [];
  for (let i = 0; i < entries.length; i++) {
    const entry = entries[i], p = proof.files[i];
    if (!p || p.filename !== entry.filename || p.sha256 !== entry.sha256 || p.size !== entry.size || typeof p.receiptSha256 !== 'string') throw Error('Bundle provenance mismatch');
    await fileIntegrity(join(root, entry.filename), entry);
    const receiptBytes = await boundedBytes(join(root, proofName(entry.filename)), 2 * 1024 * 1024);
    if (hash(receiptBytes) !== p.receiptSha256) throw Error('Bundle provenance hash mismatch');
    receipts.push(bundleReceipt(parseReleaseJson(receiptBytes), entry));
    expected.add(entry.filename); expected.add(proofName(entry.filename));
  }
  requireOneAndroidSigner(receipts);
  const names = await readdir(root);
  if (names.length !== expected.size || names.some(name => !expected.has(name))) throw Error('Unexpected bundle content');
  return { root, entries, empty: entries.length === 0 };
}

// Builds a bundle from installers and their build receipts. The Windows
// payload inspection and apksigner run where the installers are built; the
// receipts carry their results and are checked here.
export async function createDownloadBundle(manifestPath, artifactDirectory, outputDirectory, { allowEmpty = false } = {}) {
  const source = await exactDirectory(artifactDirectory);
  const entries = bundleEntries(await boundedBytes(resolve(manifestPath), maxReleaseManifestBytes), { bundleOnly: true }).map(manifestEntry);
  requireProducts(entries, allowEmpty);
  const receipts = [];
  for (const entry of entries) {
    await fileIntegrity(join(source, entry.filename), entry);
    receipts.push(bundleReceipt(parseReleaseJson(await boundedBytes(join(source, sourceReceiptName(entry)), 2 * 1024 * 1024)), entry));
  }
  requireOneAndroidSigner(receipts);
  const output = resolve(outputDirectory);
  await exactDirectory(dirname(output));
  // Never replace a previous bundle or delete an arbitrary caller-supplied path.
  await mkdir(output, { mode: 0o750 });
  const manifest = jsonBytes(entries), files = [];
  for (let i = 0; i < entries.length; i++) {
    const entry = entries[i], receipt = jsonBytes(receipts[i]);
    await copyFile(join(source, entry.filename), join(output, entry.filename), constants.COPYFILE_EXCL);
    await fileIntegrity(join(output, entry.filename), entry);
    await writeExclusive(join(output, proofName(entry.filename)), receipt);
    files.push({ filename: entry.filename, size: entry.size, sha256: entry.sha256, receiptSha256: hash(receipt) });
  }
  await writeExclusive(join(output, 'releases.json'), manifest);
  await writeExclusive(join(output, 'bundle.json'), jsonBytes({ schema: 1, kind: 'multitap-release-bundle', empty: entries.length === 0, manifestSha256: hash(manifest), files }));
  return validateDownloadBundle(output, { allowEmpty });
}

async function writeExclusive(path, bytes) {
  const handle = await open(path, constants.O_WRONLY | constants.O_CREAT | constants.O_EXCL | (constants.O_NOFOLLOW ?? 0), 0o640);
  try { await handle.writeFile(bytes); await handle.sync(); } finally { await handle.close(); }
}

export function mergeDownloadCatalog(existing, incoming) {
  // Pure callers receive the same independent count/byte/identity checks as
  // startup readers, before any payload can be copied to the data volume.
  bundleEntries(jsonBytes(existing)); bundleEntries(jsonBytes(incoming));
  const next = [...existing], byName = new Map(existing.map(e => [e.filename, e]));
  let signed = false;
  for (const entry of incoming) {
    const old = byName.get(entry.filename);
    if (old) {
      if (old.sha256 !== entry.sha256 || old.size !== entry.size || ['changelog','releasedAt'].some(key => Object.hasOwn(entry, key) && old[key] !== entry[key]) ||
        (Object.hasOwn(old, 'signature') && Object.hasOwn(entry, 'signature') && old.signature !== entry.signature)) throw Error('Immutable release catalog conflict');
      // A release signature may be added to an existing unsigned entry (the
      // same publisher-verified bytes); it is never changed or removed.
      if (!Object.hasOwn(old, 'signature') && Object.hasOwn(entry, 'signature')) {
        const index = next.indexOf(old), updated = { ...old, signature: entry.signature };
        next[index] = updated; byName.set(entry.filename, updated); signed = true;
      }
    } else { next.push(entry); byName.set(entry.filename, entry); }
  }
  if (next.length > maxCatalogEntries) throw Error('Release catalog is full; existing releases will not be removed');
  const bytes = jsonBytes(next);
  if (bytes.length > maxReleaseManifestBytes) throw Error('Release manifest exceeds limit');
  return { entries: next, bytes, changed: next.length !== existing.length || signed };
}

// Every catalog entry of the volume also gets its notes-only history record
// (release-history-core.mjs), on each start: this is the Docker backfill, and
// it is idempotent. A record without a catalog date takes the installer's
// publication time on this volume, once: the file's time only fills a record
// that has no date yet. A record that already has one keeps it, whatever the
// file's time is now (a volume put back from a copy has whole-second or other
// file times, and that is not another release date). A date or notes the
// catalog itself states still have to agree with the record, or it is a
// conflict. Notes are not part of the integrity of the seed, so a failure is
// reported instead of stopping the broker's start.
async function recordSeededHistory(target, entries) {
  try {
    const io = { open, lstat, readdir, mkdir, link, unlink, rename, chmod, chown, randomUUID };
    const dated = new Map();
    const recordedDate = async entry => {
      const record = releaseHistoryRecord({ product: entry.product, version: entry.version });
      if (!record) return false;
      const name = releaseHistoryFileName(record);
      if (!dated.has(name)) {
        let existing = null;
        try { existing = await releaseHistoryReadRecord(io, constants, target, name); } catch { /* An unreadable record is reported by the write below. */ }
        dated.set(name, existing?.record?.releasedAt !== undefined);
      }
      return dated.get(name);
    };
    const candidates = [];
    for (const entry of entries) {
      let releasedAt = entry.releasedAt;
      if (releasedAt === undefined && !await recordedDate(entry)) {
        try {
          const stat = await lstat(join(target, entry.filename));
          if (stat.isFile() && !stat.isSymbolicLink() && stat.size === entry.size) releasedAt = new Date(Math.floor(stat.mtimeMs)).toISOString();
        } catch { /* An installer not on this volume has no date. */ }
      }
      candidates.push({ ...entry, releasedAt });
    }
    const result = await releaseHistoryWrite(io, constants, target, releaseHistoryMerge(candidates));
    return { recorded: result.written + result.completed, unchanged: result.unchanged, conflicts: result.conflicts.length };
  } catch { return { failed: true }; }
}

// The seed's lock names the seed. A start stopped in the middle of the seed
// (docker stop or kill, a recreate during a restart loop, a host crash) would
// otherwise leave .publish.lock behind and block every later start. Only one
// container may use a data volume and the seed runs before the broker, so a
// lock that names the seed is always such a leftover and is taken over. Locks
// of release tools (empty files) are never removed.
const seedLockOwner = 'multitap-docker-seed';
const seedLeftover = /^\.seed-(?:lock-)?[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;
const foreignLock = () => Object.assign(Error('Another release publication holds downloads/.publish.lock; if no release tool is running, remove that file and start again'), { code: 'EEXIST' });
async function seedLockOf(path) {
  try {
    const stat = await lstat(path);
    if (!stat.isFile() || stat.isSymbolicLink() || stat.size > 1024) return null;
    const handle = await open(path, constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0));
    let bytes; try { bytes = await handle.readFile(); } finally { await handle.close(); }
    const value = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes));
    return value?.owner === seedLockOwner && typeof value.nonce === 'string' ? { ...value, dev: stat.dev, ino: stat.ino } : null;
  } catch { return null; }
}
async function takeSeedLock(target) {
  const lockPath = join(target, '.publish.lock'), nonce = randomUUID(), part = join(target, '.seed-lock-' + randomUUID());
  // The lock appears with its owner already written (link is atomic and never replaces).
  await writeExclusive(part, Buffer.from(JSON.stringify({ owner: seedLockOwner, nonce, pid: process.pid, startedAt: new Date().toISOString() }) + '\n'));
  let recovered = 0;
  try {
    try { await link(part, lockPath); }
    catch (error) {
      if (error.code !== 'EEXIST') throw error;
      const leftover = await seedLockOf(lockPath);
      if (!leftover) throw foreignLock();
      const now = await lstat(lockPath);
      if (now.dev !== leftover.dev || now.ino !== leftover.ino) throw foreignLock();
      await unlink(lockPath); recovered++;
      try { await link(part, lockPath); } catch (again) { throw again.code === 'EEXIST' ? foreignLock() : again; }
    }
  } finally { try { await unlink(part); } catch (e) { if (e.code !== 'ENOENT') throw e; } }
  // Holding the lock: any other .seed-* file is a leftover of an interrupted seed.
  for (const name of await readdir(target)) {
    if (!seedLeftover.test(name)) continue;
    const stat = await lstat(join(target, name));
    if (stat.isFile() && !stat.isSymbolicLink()) { await unlink(join(target, name)); recovered++; }
  }
  return { lockPath, nonce, recovered };
}
async function releaseSeedLock({ lockPath, nonce }) {
  if ((await seedLockOf(lockPath))?.nonce === nonce) await unlink(lockPath);
}

export async function seedDownloadBundle(bundleDirectory, downloadDirectory, { allowEmpty = false } = {}) {
  // Validate the entire image bundle before touching the installation volume.
  const bundle = await validateDownloadBundle(bundleDirectory, { allowEmpty });
  if (bundle.empty) return { seeded: 0, catalogChanged: false, empty: true };
  const target = resolve(downloadDirectory);
  await exactDirectory(dirname(target));
  try { await mkdir(target, { mode: 0o750 }); } catch (e) { if (e.code !== 'EEXIST') throw e; }
  await exactDirectory(target);
  const lock = await takeSeedLock(target);
  const temporary = [], created = []; let committed = false;
  try {
    const manifestPath = join(target, 'releases.json'); let original = null, existing = [];
    try { original = await boundedBytes(manifestPath, maxReleaseManifestBytes); existing = bundleEntries(original); }
    catch (e) { if (e.code !== 'ENOENT') throw e; }
    const plan = mergeDownloadCatalog(existing, bundle.entries);
    const additions = [];
    // Detect every catalog/file conflict before publishing a single new byte.
    for (const entry of bundle.entries) {
      try { await fileIntegrity(join(target, entry.filename), entry); }
      catch (e) { if (e.code !== 'ENOENT') throw e; additions.push(entry); }
    }
    for (const entry of additions) {
      const part = join(target, '.seed-' + randomUUID()); temporary.push(part);
      await copyFile(join(bundle.root, entry.filename), part, constants.COPYFILE_EXCL);
      await fileIntegrity(part, entry);
      await chmod(part, 0o640);
      const handle = await open(part, constants.O_RDWR | (constants.O_NOFOLLOW ?? 0));
      try { await handle.sync(); } finally { await handle.close(); }
      // Hard-link creation is atomic and cannot overwrite even a racing filename.
      const final = join(target, entry.filename);
      await link(part, final); await unlink(part);
      created.push({ path: final, entry, ...await fileIntegrity(final, entry) });
    }
    let current = null;
    try { current = await boundedBytes(manifestPath, maxReleaseManifestBytes); } catch (e) { if (e.code !== 'ENOENT') throw e; }
    if ((original === null) !== (current === null) || (original && !original.equals(current))) throw Error('Release manifest changed during seed');
    if (plan.changed) {
      const part = join(target, '.seed-' + randomUUID()); temporary.push(part);
      await writeExclusive(part, plan.bytes);
      await rename(part, manifestPath);
    }
    committed = true;
    if (process.platform !== 'win32') {
      const directory = await open(target, constants.O_RDONLY | constants.O_DIRECTORY);
      try { await directory.sync(); } finally { await directory.close(); }
    }
    // recoveredLeftovers: lock/temporary files of an interrupted earlier start that this seed cleared.
    return { seeded: created.length, catalogChanged: plan.changed, catalogEntries: plan.entries.length, empty: false, history: await recordSeededHistory(target, plan.entries), ...(lock.recovered ? { recoveredLeftovers: lock.recovered } : {}) };
  } finally {
    try {
      if (!committed) for (const entry of created) {
        // Remove only exact files this transaction created, never a foreign replacement.
        try { const stat = await fileIntegrity(entry.path, entry.entry); if (stat.dev === entry.dev && stat.ino === entry.ino) await unlink(entry.path); }
        catch { /* Preserve uncertain/foreign files for an operator; do not overwrite. */ }
      }
      for (const part of temporary) { try { await unlink(part); } catch (e) { if (e.code !== 'ENOENT') throw e; } }
    } finally { await releaseSeedLock(lock); }
  }
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  try {
    const [action, ...args] = process.argv.slice(2), allowEmpty = args.includes('--allow-empty');
    const paths = args.filter(value => value !== '--allow-empty');
    let result;
    if (action === 'validate' && paths.length === 1) {
      const bundle = await validateDownloadBundle(paths[0], { allowEmpty });
      result = { validated: true, releases: bundle.entries.length, empty: bundle.empty };
    } else if (action === 'seed' && paths.length === 2) result = await seedDownloadBundle(paths[0], paths[1], { allowEmpty });
    else if (action === 'create' && paths.length === 3) {
      const bundle = await createDownloadBundle(...paths, { allowEmpty });
      result = { prepared: true, releases: bundle.entries.length, empty: bundle.empty };
    } else throw Error('Expected validate <bundle>, seed <bundle> <downloads>, or create <manifest> <artifacts> <new-bundle> [--allow-empty]');
    console.log(JSON.stringify(result));
  } catch (error) { console.error('Multitap release bundle: ' + error.message); process.exitCode = 1; }
}
