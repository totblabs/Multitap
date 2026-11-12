// The public release-notes history: GET /api/v1/releases/history?product=
// client|host|android-client[&limit=1..100][&before=<version>], newest first.
// It lists every version recorded in <downloads>/release-notes plus every
// version still in the download catalog, so notes stay readable after their
// installers were pruned or archived. Only versions, dates and notes leave
// the server: no file name, size, hash, URL or private catalog field.
import * as fs from 'node:fs/promises';
import { constants } from 'node:fs';
import { randomUUID } from 'node:crypto';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { HttpError } from './errors.mjs';
import { readReleases } from './downloads.mjs';
import {
  releaseHistoryDirectory, releaseHistoryProducts, releaseHistoryVersionValid, releaseHistoryParseFileName,
  releaseHistoryReadRecord, compareReleaseHistoryVersions, releaseHistoryBackfill, releaseHistoryCandidates,
  releaseHistoryPublishedEntry, releaseHistoryRecord,
} from './release-history-core.mjs';

export const releaseHistoryPageSize = 100;
export const releaseHistoryDefaultPage = 30;
const io = { ...fs, randomUUID };
const present = text => typeof text === 'string' && text.trim() ? text : undefined;

// One page of [product]'s history. [before] is exclusive; `next` is the
// `before` of the following page, null after the oldest version.
export async function releaseHistoryPage(directory, { product, limit = releaseHistoryDefaultPage, before = null }) {
  const versions = new Map();
  if (directory) {
    let names = [];
    try { names = await fs.readdir(join(directory, releaseHistoryDirectory)); }
    catch (error) { if (!['ENOENT', 'ENOTDIR'].includes(error.code)) throw error; }
    for (const name of names) {
      const identity = releaseHistoryParseFileName(name);
      if (identity?.product === product) versions.set(identity.version, { name, catalog: null });
    }
    // The catalog's own entries: corrected notes there win while it lists them.
    for (const entry of await readReleases(directory)) {
      if (entry.product !== product || !releaseHistoryVersionValid(product, entry.version)) continue;
      const known = versions.get(entry.version) ?? { name: null, catalog: null };
      if (!known.catalog || (!present(known.catalog.changelog) && present(entry.changelog))) known.catalog = entry;
      versions.set(entry.version, known);
    }
  }
  const ordered = [...versions.keys()].sort((a, b) => compareReleaseHistoryVersions(b, a));
  const start = before === null ? 0 : ordered.findIndex(version => compareReleaseHistoryVersions(version, before) < 0);
  const page = start < 0 ? [] : ordered.slice(start, start + limit);
  const releases = [];
  for (const version of page) {
    const { name, catalog } = versions.get(version);
    let record = null;
    if (name) {
      try { record = (await releaseHistoryReadRecord(io, constants, directory, name))?.record ?? null; }
      catch { record = null; }
    }
    const releasedAt = catalog?.releasedAt ?? record?.releasedAt, changelog = present(catalog?.changelog) ?? record?.changelog;
    releases.push({ version, ...(releasedAt ? { releasedAt } : {}), ...(changelog ? { changelog } : {}) });
  }
  const more = start >= 0 && start + limit < ordered.length;
  return { product, total: ordered.length, releases, next: more ? page.at(-1) : null };
}

function query(url) {
  const values = {};
  for (const key of ['product', 'limit', 'before']) {
    const all = url.searchParams.getAll(key);
    if (all.length > 1) throw new HttpError(400, 'invalid_query');
    if (all.length) values[key] = all[0];
  }
  if (!releaseHistoryProducts.includes(values.product)) throw new HttpError(400, 'invalid_product');
  let limit = releaseHistoryDefaultPage;
  if (values.limit !== undefined) {
    if (!/^[1-9][0-9]{0,5}$/.test(values.limit)) throw new HttpError(400, 'invalid_limit');
    limit = Math.min(Number(values.limit), releaseHistoryPageSize);
  }
  if (values.before !== undefined && !releaseHistoryVersionValid(values.product, values.before)) throw new HttpError(400, 'invalid_before');
  return { product: values.product, limit, before: values.before ?? null };
}

// A public, unauthenticated read like /api/v1/downloads. Returns false for
// any other path. Errors are HttpErrors for the server's JSON error reply.
export function releaseHistoryRoutes(directory) {
  return async function serve(req, res, url) {
    if (url.pathname !== '/api/v1/releases/history') return false;
    if (!['GET', 'HEAD'].includes(req.method)) { res.setHeader('Allow', 'GET, HEAD'); throw new HttpError(405, 'method_not_allowed'); }
    const body = JSON.stringify(await releaseHistoryPage(directory, query(url)));
    res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-cache', 'X-Content-Type-Options': 'nosniff',
      'Content-Security-Policy': "default-src 'none'; frame-ancestors 'none'", 'Content-Length': Buffer.byteLength(body) });
    res.end(req.method === 'HEAD' ? undefined : body);
    return true;
  };
}

// --- Operator tools (never run by the broker itself) -------------------------

// Local notes for tools/backfill-release-history.ps1: published catalog
// entries from captured catalogs under .artifacts (newest capture first) and
// draft changelog files, which may only fill notes of a version a catalog
// already knows.
export async function collectLocalReleaseHistory(projectRoot) {
  const artifacts = join(projectRoot, '.artifacts'), files = [];
  async function walk(directory, depth) {
    let entries;
    try { entries = await fs.readdir(directory, { withFileTypes: true }); } catch { return; }
    for (const entry of entries) {
      const path = join(directory, entry.name);
      if (entry.isDirectory()) { if (depth < 3 && !entry.isSymbolicLink()) await walk(path, depth + 1); continue; }
      if (!entry.isFile() || !entry.name.endsWith('.json')) continue;
      const stat = await fs.stat(path);
      if (stat.size < 64 || stat.size > 8 * 1024 * 1024) continue;
      files.push({ path, time: stat.mtimeMs });
    }
  }
  await walk(artifacts, 0);
  files.sort((a, b) => b.time - a.time || (a.path < b.path ? -1 : 1));
  // One entry per published file: the newest capture sets each field, older
  // ones only fill a missing date or notes (the same rule as the merge).
  const snapshots = new Map();
  let conflicts = 0;
  const seen = new Map();
  for (const { path } of files) {
    let value;
    try {
      const text = await fs.readFile(path, 'utf8');
      if (!text.includes('"changelog"') && !text.includes('"sha256"')) continue;
      value = JSON.parse(text.replace(/^\uFEFF/, ''));
    } catch { continue; }
    for (const entry of releaseHistoryCandidates(value)) {
      if (!releaseHistoryPublishedEntry(entry)) continue;
      const record = releaseHistoryRecord(entry);
      if (!record) continue;
      const key = record.product + '\n' + record.version;
      if (record.changelog !== undefined) {
        if (seen.has(key) && seen.get(key) !== record.changelog) conflicts++;
        else seen.set(key, record.changelog);
      }
      const known = snapshots.get(entry.filename);
      if (known) {
        known.releasedAt ??= record.releasedAt;
        known.changelog ??= record.changelog;
        continue;
      }
      snapshots.set(entry.filename, { product: entry.product, version: entry.version, abi: entry.abi, filename: entry.filename, size: entry.size, sha256: entry.sha256,
        releasedAt: record.releasedAt, changelog: record.changelog });
    }
  }
  // Draft notes: corrected/final variants first, then the plain file, then the
  // older deploy/windows copies.
  const drafts = [], decoder = new TextDecoder('utf-8', { fatal: true });
  const draftFiles = [];
  const names = async directory => { try { return await fs.readdir(directory); } catch { return []; } };
  // .artifacts/client-0425-changelog[-corrected|-final].txt is 0.4.25.
  for (const name of await names(artifacts)) {
    const match = /^(client|host)-0(\d)(\d{2,4})-changelog(-corrected|-final)?\.txt$/.exec(name);
    if (match) draftFiles.push({ product: match[1], version: `0.${Number(match[2])}.${Number(match[3])}`, path: join(artifacts, name), rank: match[4] ? 0 : 1 });
  }
  const deploy = join(projectRoot, 'deploy', 'windows');
  for (const name of await names(deploy)) {
    const match = /^changelog-(client|host)-(\d+)\.(\d+)\.(\d+)\.txt$/.exec(name);
    if (match) draftFiles.push({ product: match[1], version: `${Number(match[2])}.${Number(match[3])}.${Number(match[4])}`, path: join(deploy, name), rank: 2 });
  }
  draftFiles.sort((a, b) => a.rank - b.rank || (a.path < b.path ? -1 : 1));
  for (const { product, version, path } of draftFiles) {
    let changelog;
    try { changelog = decoder.decode(await fs.readFile(path)).replace(/^\uFEFF/, ''); } catch { continue; }
    const record = releaseHistoryRecord({ product, version, changelog });
    if (record?.changelog) drafts.push(record);
  }
  return { snapshots: [...snapshots.values()], drafts, snapshotFiles: files.length, snapshotConflicts: conflicts };
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  try {
    const [action, ...args] = process.argv.slice(2);
    let result;
    if (action === 'collect' && (args.length === 1 || args.length === 2)) {
      // node src/release-history.mjs collect <project-root> [<new-output-file>]
      // The notes go to the file as UTF-8 (a console code page could not
      // carry Korean); stdout gets the ASCII summary.
      const local = await collectLocalReleaseHistory(resolve(args[0]));
      const summary = { snapshotFiles: local.snapshotFiles, snapshotEntries: local.snapshots.length, drafts: local.drafts.length, snapshotConflicts: local.snapshotConflicts };
      for (const product of ['client', 'host', 'android-client']) {
        summary[product] = new Set([...local.snapshots, ...local.drafts].filter(e => e.product === product).map(e => e.version)).size;
      }
      if (args[1]) await fs.writeFile(resolve(args[1]), JSON.stringify({ snapshots: local.snapshots, drafts: local.drafts }), { flag: 'wx' });
      result = args[1] ? summary : { ...summary, snapshots: local.snapshots, drafts: local.drafts };
    } else if (action === 'backfill' && args.length >= 1) {
      // node src/release-history.mjs backfill <downloads> [--source <file|dir>]... [--dry-run] [--archive-file-dates]
      const downloads = resolve(args[0]), tiers = [];
      let dryRun = false, fileDates = false;
      for (let i = 1; i < args.length; i++) {
        if (args[i] === '--dry-run') dryRun = true;
        else if (args[i] === '--archive-file-dates') fileDates = true;
        else if (args[i] === '--source' && args[i + 1]) tiers.push({ path: resolve(args[++i]) });
        else throw Error('Unexpected argument');
      }
      const owner = process.getuid?.() === 0 ? await fs.lstat(downloads).then(stat => ({ uid: stat.uid, gid: stat.gid })) : null;
      const lock = join(downloads, '.publish.lock');
      const handle = dryRun ? null : await fs.open(lock, constants.O_WRONLY | constants.O_CREAT | constants.O_EXCL | (constants.O_NOFOLLOW ?? 0), 0o600);
      try { result = await releaseHistoryBackfill(io, constants, downloads, { tiers, owner, dryRun, fileDates }); }
      finally { if (handle) { await handle.close(); await fs.unlink(lock); } }
    } else throw Error('Expected collect <project-root> or backfill <downloads> [--source <path>]... [--dry-run] [--archive-file-dates]');
    console.log(JSON.stringify(result));
  } catch (error) { console.error('Multitap release history: ' + error.message); process.exitCode = 1; }
}
