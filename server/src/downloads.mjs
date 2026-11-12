import { open, readFile, realpath, lstat } from 'node:fs/promises';
import { constants } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { pipeline } from 'node:stream/promises';

const filenamePattern = /^Multitap(?:-Host)?-[0-9]+\.[0-9]+\.[0-9]+-windows-x64\.(?:zip|exe)$/;
const androidVersion = /^(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)(?:-preview(?:\.[1-9][0-9]*)?)?$/;
const androidAbis = new Set(['arm64-v8a', 'armeabi-v7a', 'x86_64']);
export const maxDownloadSize = 128 * 1024 * 1024;
// The on-disk immutable history is larger than the legacy client's public feed.
// Keep both limits independent of the total UTF-8 manifest byte budget.
// 1024 files: disk is plentiful. Release notes do not
// depend on this limit: every published version also keeps a notes-only
// record (release-history.mjs), so pruned installers keep their notes.
export const maxCatalogEntries = 1024;
export const maxPublicCatalogEntries = 32;
// 1024 entries with 8000-byte notes and signatures (about 9 MB) fit.
export const maxReleaseManifestBytes = 12 * 1024 * 1024;
const maxSize = maxDownloadSize;

// Release notes are text, never executable markup. Keep the complete UTF-8
// budget (rather than JS UTF-16 length) and reject malformed/control text.
export function releaseMetadata(entry) {
  const result = {};
  if (Object.hasOwn(entry, 'changelog')) {
    if (typeof entry.changelog !== 'string' || !entry.changelog.isWellFormed() ||
      Buffer.byteLength(entry.changelog, 'utf8') > 8000 || /[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/u.test(entry.changelog)) return null;
    result.changelog = entry.changelog;
  }
  if (Object.hasOwn(entry, 'releasedAt')) {
    if (typeof entry.releasedAt !== 'string' || !/^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(?:\.\d{1,3})?Z$/.test(entry.releasedAt)) return null;
    const date = new Date(entry.releasedAt);
    if (!Number.isFinite(date.valueOf())) return null;
    const canonical = date.toISOString();
    // Date.parse normalizes impossible dates such as February 30; do not accept them.
    if (canonical.slice(0, 19) !== entry.releasedAt.slice(0, 19)) return null;
    result.releasedAt = canonical;
  }
  return result;
}

function androidRelease(entry) {
  if (!entry || entry.product !== 'android-client' || entry.format !== 'apk' || typeof entry.version !== 'string' ||
    entry.version.length > 40 || !androidVersion.test(entry.version) || !androidAbis.has(entry.abi) ||
    entry.filename !== `Multitap-${entry.version}-android-${entry.abi}.apk` ||
    typeof entry.sha256 !== 'string' || !/^[a-f0-9]{64}$/.test(entry.sha256) ||
    !Number.isSafeInteger(entry.size) || entry.size <= 0 || entry.size > maxSize) return null;
  const metadata = releaseMetadata(entry);
  if (!metadata) return null;
  const { product, version, abi, filename, size, sha256 } = entry;
  return { product, version, abi, filename, size, sha256, format: 'apk', url: '/downloads/' + filename, ...metadata };
}

// The project's release signature of a Windows installer (Ed25519 over the
// canonical product/version/filename/size/SHA-256 message, tools/
// windows-release-signature.mjs). The broker only passes it through; client
// and Host builds verify it against their pinned key. Released clients ignore
// the field. {} when absent, null when present but not canonical base64.
export function releaseSignature(entry) {
  if (!Object.hasOwn(entry, 'signature')) return {};
  const value = entry.signature;
  if (typeof value !== 'string' || !/^[A-Za-z0-9+/]{86}==$/.test(value) || Buffer.from(value, 'base64').toString('base64') !== value ||
    typeof entry.filename !== 'string' || !entry.filename.endsWith('.exe')) return null;
  return { signature: value };
}

export function publishedRelease(entry) {
  if (entry?.product === 'android-client') return androidRelease(entry);
  if (!(entry && ['client', 'host'].includes(entry.product) && typeof entry.version === 'string' &&
    /^[0-9]+\.[0-9]+\.[0-9]+$/.test(entry.version) && typeof entry.filename === 'string' && filenamePattern.test(entry.filename) &&
    ['zip', 'exe'].some(extension => entry.filename === `Multitap${entry.product === 'host' ? '-Host' : ''}-${entry.version}-windows-x64.${extension}`) &&
    typeof entry.sha256 === 'string' && /^[a-f0-9]{64}$/.test(entry.sha256) && Number.isSafeInteger(entry.size) && entry.size > 0 && entry.size <= maxSize)) return null;
  const metadata = releaseMetadata(entry), signature = releaseSignature(entry);
  if (!metadata || !signature) return null;
  const { product, version, filename, size, sha256 } = entry;
  return { product, version, filename, size, sha256, format: filename.endsWith('.exe') ? 'installer' : 'archive', url: '/downloads/' + filename, ...metadata, ...signature };
}

const compareText = (a, b) => a === b ? 0 : a < b ? -1 : 1;
function compareNumber(a, b) {
  a = a.replace(/^0+(?=\d)/, ''); b = b.replace(/^0+(?=\d)/, '');
  return a.length === b.length ? compareText(a, b) : a.length < b.length ? -1 : 1;
}
function compareVersion(a, b) {
  const [av, ap] = a.split('-'), [bv, bp] = b.split('-');
  const aa = av.split('.'), bb = bv.split('.');
  for (let i = 0; i < 3; i++) { const result = compareNumber(aa[i], bb[i]); if (result) return result; }
  if (ap === undefined || bp === undefined) return ap === bp ? 0 : ap === undefined ? 1 : -1;
  return compareNumber(ap.split('.')[1] ?? '0', bp.split('.')[1] ?? '0');
}

// First take the latest of every product/format/ABI, then one older release per
// group per round. Publication order and timestamps cannot starve a product.
// Callers pass only publishedRelease projections, never raw private metadata.
export function publicDownloadCatalog(entries) {
  const products = ['client', 'host', 'android-client'], formats = ['installer', 'archive', 'apk'];
  const key = entry => `${products.indexOf(entry.product)}:${formats.indexOf(entry.format)}:${entry.abi ?? ''}`;
  const groups = new Map();
  for (const entry of entries) {
    const group = key(entry);
    if (!groups.has(group)) groups.set(group, []);
    groups.get(group).push(entry);
  }
  const ordered = [...groups].sort(([a], [b]) => compareText(a, b)).map(([, releases]) =>
    releases.sort((a, b) => compareVersion(b.version, a.version) || compareText(a.filename, b.filename)));
  const result = [];
  for (let round = 0; result.length < maxPublicCatalogEntries; round++) {
    let found = false;
    for (const group of ordered) if (round < group.length) {
      result.push(group[round]); found = true;
      if (result.length === maxPublicCatalogEntries) break;
    }
    if (!found) break;
  }
  return result;
}

// The validated release manifest of this installation (publishedRelease
// projections only). Also the broker's own source for remote Host updates.
export async function readReleases(directory) {
  if (!directory) return [];
  try {
    const raw = await readFile(resolve(directory, 'releases.json'));
    if (raw.length > maxReleaseManifestBytes) return [];
    const entries = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(raw));
    if (!Array.isArray(entries) || entries.length > maxCatalogEntries) return [];
    const published = entries.map(publishedRelease).filter(Boolean), names = new Set();
    // Ambiguous identities must never select an arbitrary immutable payload.
    for (const entry of published) { if (names.has(entry.filename)) return []; names.add(entry.filename); }
    return published;
  } catch { return []; }
}
export { compareVersion as compareReleaseVersions };

// Only explicitly published product archives may be served. The broker database,
// vault and host enrollment material never belong in this directory or manifest.
export function downloadRoutes(directory) {
  const releases = () => readReleases(directory);
  return async function serve(req, res, path) {
    if (path !== '/api/v1/downloads' && !path.startsWith('/downloads/')) return false;
    const headers = { 'X-Content-Type-Options': 'nosniff', 'Cache-Control': 'no-cache', 'Content-Security-Policy': "default-src 'none'; frame-ancestors 'none'" };
    const end = (status, body = '') => { res.writeHead(status, { ...headers, 'Content-Type': 'application/json; charset=utf-8' }); res.end(req.method === 'HEAD' ? undefined : body); return true; };
    if (!['GET', 'HEAD'].includes(req.method)) { res.setHeader('Allow', 'GET, HEAD'); return end(405); }
    const entries = await releases();
    if (path === '/api/v1/downloads') return end(200, JSON.stringify({ releases: publicDownloadCatalog(entries) }));
    const filename = path.slice('/downloads/'.length), entry = entries.find(value => value.filename === filename);
    if (!entry || (entry.format !== 'apk' && !filenamePattern.test(filename))) return end(404);
    let file;
    try {
      const root = await realpath(directory), target = await realpath(resolve(root, filename));
      if (dirname(target) !== root) return end(404);
      const candidate = resolve(root, filename);
      let before;
      if (entry.format === 'apk') {
        before = await lstat(candidate);
        if (before.isSymbolicLink() || !before.isFile() || target !== candidate) return end(404);
      }
      file = await open(target, entry.format === 'apk' ? constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0) : 'r');
      const stat = await file.stat();
      if (!stat.isFile() || stat.size !== entry.size || (before && (before.dev !== stat.dev || before.ino !== stat.ino))) { await file.close(); return end(404); }
    } catch { await file?.close(); return end(404); }
    const etag = `"sha256-${entry.sha256}"`, range = byteRange(req.headers.range, req.headers['if-range'], etag, entry.size);
    if (range === 'unsatisfiable') { await file.close(); res.writeHead(416, { ...headers, 'Content-Range': `bytes */${entry.size}` }); res.end(); return true; }
    const [start, last] = range ?? [0, entry.size - 1];
    res.writeHead(range ? 206 : 200, { ...headers, 'Content-Type': entry.format === 'apk' ? 'application/vnd.android.package-archive' : entry.format === 'installer' ? 'application/vnd.microsoft.portable-executable' : 'application/zip', 'Content-Length': last - start + 1,
      'Accept-Ranges': 'bytes', ...(range ? { 'Content-Range': `bytes ${start}-${last}/${entry.size}` } : {}), 'Content-Disposition': `attachment; filename="${entry.filename}"`, ETag: etag });
    if (req.method === 'HEAD') { await file.close(); res.end(); return true; }
    try { await pipeline(file.createReadStream({ start, end: last }), res); }
    catch { res.destroy(); }
    return true;
  };
}

// An interrupted download continues: one `bytes=a-` or
// `bytes=a-b` range, and only while If-Range (when sent) names this very file.
// Anything else is served whole, as RFC 9110 allows; null means that.
export function byteRange(header, ifRange, etag, size) {
  if (typeof header !== 'string' || (ifRange !== undefined && ifRange !== etag)) return null;
  const match = /^bytes=(\d{1,15})-(\d{0,15})$/.exec(header.trim());
  if (!match) return null;
  const start = Number(match[1]), last = match[2] === '' ? Infinity : Number(match[2]);
  if (last < start) return null;
  if (start >= size) return 'unsatisfiable';
  return [start, Math.min(last, size - 1)];
}
