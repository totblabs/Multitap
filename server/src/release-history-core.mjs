// Notes-only release history: one small record per published product version
// in <downloads>/release-notes/<product>-<version>.json with the version, its
// release date and its notes, never a file. Records outlive the download
// catalog (releases.json), so pruning or archiving installers keeps the notes,
// and their number has no limit.
//
// This file is also embedded, with its `export ` keywords removed, into the
// root publication programs of tools/publish-*-release.ps1,
// tools/correct-windows-release-notes.ps1 and tools/backfill-release-history.ps1.
// It therefore imports nothing: file access arrives as an `io` object
// (node:fs/promises functions plus randomUUID) and `constants`, and every name
// starts with releaseHistory so it cannot collide with the host program.

export const releaseHistoryDirectory = 'release-notes';
export const releaseHistoryProducts = Object.freeze(['client', 'host', 'android-client']);
// A record holds at most 8000 UTF-8 bytes of notes; JSON escaping can double
// newline-heavy text.
export const releaseHistoryRecordBytes = 32 * 1024;
// A catalog snapshot a backfill reads (releases.json copies, public feeds).
export const releaseHistorySnapshotBytes = 32 * 1024 * 1024;

export function releaseHistoryVersionValid(product, version) {
  if (typeof version !== 'string' || version.length > 40) return false;
  if (product === 'client' || product === 'host') return /^(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)$/.test(version);
  if (product === 'android-client') return /^(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)(?:-preview(?:\.[1-9][0-9]*)?)?$/.test(version);
  return false;
}

// The catalog's release-note rule (downloads.mjs releaseMetadata): plain
// well-formed text, at most 8000 UTF-8 bytes, no control characters.
export function releaseHistoryNotesValid(text) {
  return typeof text === 'string' && text.isWellFormed() && Buffer.byteLength(text, 'utf8') <= 8000 &&
    !/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/u.test(text);
}

// A canonical UTC ISO timestamp, or null. Impossible dates are refused.
export function releaseHistoryDate(value) {
  if (typeof value !== 'string' || !/^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(?:\.\d{1,3})?Z$/.test(value)) return null;
  const date = new Date(value);
  if (!Number.isFinite(date.valueOf())) return null;
  const canonical = date.toISOString();
  return canonical.slice(0, 19) === value.slice(0, 19) ? canonical : null;
}

// The notes-only projection {product, version, releasedAt?, changelog?} of a
// catalog entry or record; null when any present field is invalid. Blank
// notes count as none.
export function releaseHistoryRecord(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value) || !releaseHistoryVersionValid(value.product, value.version)) return null;
  const record = { product: value.product, version: value.version };
  if (value.releasedAt !== undefined && value.releasedAt !== null) {
    const date = releaseHistoryDate(value.releasedAt);
    if (!date) return null;
    record.releasedAt = date;
  }
  if (value.changelog !== undefined && value.changelog !== null) {
    if (!releaseHistoryNotesValid(value.changelog)) return null;
    if (value.changelog.trim()) record.changelog = value.changelog;
  }
  return record;
}

export const releaseHistoryFileName = record => `${record.product}-${record.version}.json`;

export function releaseHistoryParseFileName(name) {
  const match = typeof name === 'string' ? /^(client|host|android-client)-([0-9][0-9A-Za-z.-]{0,39})\.json$/.exec(name) : null;
  return match && releaseHistoryVersionValid(match[1], match[2]) ? { product: match[1], version: match[2] } : null;
}

// The exact bytes of a record: fixed key order, so an identical record is
// recognised byte for byte.
export function releaseHistoryBytes(record) {
  const ordered = { product: record.product, version: record.version };
  if (record.releasedAt !== undefined) ordered.releasedAt = record.releasedAt;
  if (record.changelog !== undefined) ordered.changelog = record.changelog;
  return Buffer.from(JSON.stringify(ordered, null, 2) + '\n', 'utf8');
}

function releaseHistoryCompareNumber(a, b) {
  a = a.replace(/^0+(?=\d)/, ''); b = b.replace(/^0+(?=\d)/, '');
  return a.length === b.length ? (a === b ? 0 : a < b ? -1 : 1) : a.length < b.length ? -1 : 1;
}
// The catalog's version order (downloads.mjs): numeric parts, then a final
// release after its previews, then the preview number.
export function compareReleaseHistoryVersions(a, b) {
  const [av, ap] = a.split('-'), [bv, bp] = b.split('-');
  const aa = av.split('.'), bb = bv.split('.');
  for (let i = 0; i < 3; i++) { const result = releaseHistoryCompareNumber(aa[i], bb[i]); if (result) return result; }
  if (ap === undefined || bp === undefined) return ap === bp ? 0 : ap === undefined ? 1 : -1;
  return releaseHistoryCompareNumber(ap.split('.')[1] ?? '0', bp.split('.')[1] ?? '0');
}

// Every release-like object of a catalog snapshot: a releases.json array, the
// public feed ({releases}), an audit capture ({catalog}) or {entries}.
export function releaseHistoryCandidates(value) {
  const lists = Array.isArray(value) ? [value]
    : value && typeof value === 'object' ? ['releases', 'catalog', 'entries'].map(key => value[key]).filter(Array.isArray) : [];
  return lists.flat().filter(entry => entry && typeof entry === 'object' && !Array.isArray(entry));
}

// A catalog entry exactly as a publisher writes it (product, version, its
// one canonical file name, SHA-256 and size), so stray objects in a snapshot
// never become history.
export function releaseHistoryPublishedEntry(entry) {
  if (!entry || !releaseHistoryVersionValid(entry.product, entry.version) || typeof entry.sha256 !== 'string' ||
    !/^[a-f0-9]{64}$/.test(entry.sha256) || !Number.isSafeInteger(entry.size) || entry.size <= 0) return false;
  if (entry.product === 'android-client') return ['arm64-v8a', 'armeabi-v7a', 'x86_64'].includes(entry.abi) && entry.filename === `Multitap-${entry.version}-android-${entry.abi}.apk`;
  return ['exe', 'zip'].some(extension => entry.filename === `Multitap${entry.product === 'host' ? '-Host' : ''}-${entry.version}-windows-x64.${extension}`);
}

// One record per product/version, newest first. Candidates come in
// precedence order: the first one sets each field, later ones only fill a
// field still missing. {fillOnly: true} candidates (local draft notes) never
// introduce a version of their own.
export function releaseHistoryMerge(candidates) {
  const records = new Map(), later = [];
  for (const candidate of candidates) {
    const record = releaseHistoryRecord(candidate?.entry ?? candidate);
    if (!record) continue;
    const key = record.product + '\n' + record.version, current = records.get(key);
    if (!current) {
      if (candidate?.fillOnly) later.push([key, record]);
      else records.set(key, record);
      continue;
    }
    if (current.releasedAt === undefined && record.releasedAt !== undefined) current.releasedAt = record.releasedAt;
    if (current.changelog === undefined && record.changelog !== undefined) current.changelog = record.changelog;
  }
  // Drafts fill only after every catalog source had its say.
  for (const [key, record] of later) {
    const current = records.get(key);
    if (!current) continue;
    if (current.releasedAt === undefined && record.releasedAt !== undefined) current.releasedAt = record.releasedAt;
    if (current.changelog === undefined && record.changelog !== undefined) current.changelog = record.changelog;
  }
  return [...records.values()].sort((a, b) => a.product === b.product
    ? compareReleaseHistoryVersions(b.version, a.version)
    : releaseHistoryProducts.indexOf(a.product) - releaseHistoryProducts.indexOf(b.product));
}

// A regular (non-link) file of at most [cap] bytes, or null when absent.
export async function releaseHistoryReadFile(io, constants, path, cap) {
  let before;
  try { before = await io.lstat(path); } catch (error) { if (error.code === 'ENOENT') return null; throw error; }
  if (!before.isFile() || before.isSymbolicLink() || before.size > cap) throw Error('release_history_unsafe_file');
  const handle = await io.open(path, constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0));
  try {
    const stat = await handle.stat();
    if (!stat.isFile() || stat.dev !== before.dev || stat.ino !== before.ino || stat.size > cap) throw Error('release_history_unsafe_file');
    const bytes = await handle.readFile();
    if (bytes.length > cap) throw Error('release_history_unsafe_file');
    return { bytes, stat };
  } finally { await handle.close(); }
}

function releaseHistoryJson(bytes) {
  try { return JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes)); } catch { return undefined; }
}

// The stored record called [name]: null when absent; {record: null} when the
// file is not a valid record of exactly that product/version.
export async function releaseHistoryReadRecord(io, constants, downloads, name) {
  const identity = releaseHistoryParseFileName(name);
  if (!identity) return null;
  const file = await releaseHistoryReadFile(io, constants, `${downloads}/${releaseHistoryDirectory}/${name}`, releaseHistoryRecordBytes);
  if (!file) return null;
  const record = releaseHistoryRecord(releaseHistoryJson(file.bytes));
  return { bytes: file.bytes, stat: file.stat, record: record && record.product === identity.product && record.version === identity.version ? record : null };
}

async function releaseHistoryEnsureDirectory(io, directory, owner) {
  let created = false;
  try { await io.mkdir(directory, { mode: 0o750 }); created = true; }
  catch (error) { if (error.code !== 'EEXIST') throw error; }
  const stat = await io.lstat(directory);
  if (!stat.isDirectory() || stat.isSymbolicLink()) throw Error('release_history_unsafe_directory');
  // The broker reads the records through the download directory's group.
  if (created && owner) { await io.chown(directory, owner.uid, owner.gid); await io.chmod(directory, 0o750); }
}

async function releaseHistoryCommit(io, constants, directory, name, bytes, replace, owner) {
  const part = `${directory}/.${name}.${io.randomUUID()}.part`;
  const handle = await io.open(part, constants.O_WRONLY | constants.O_CREAT | constants.O_EXCL | (constants.O_NOFOLLOW ?? 0), 0o640);
  try {
    try {
      if (owner) { await handle.chown(owner.uid, owner.gid); await handle.chmod(0o640); }
      await handle.writeFile(bytes); await handle.sync();
    } finally { await handle.close(); }
    // A new record is linked, which never replaces a racing writer's file; only
    // an allowed completion or correction renames over the old record.
    if (replace) {
      for (let attempt = 0; ; attempt++) {
        try { await io.rename(part, `${directory}/${name}`); break; }
        catch (error) {
          // Windows only: a virus scanner may hold the old record a moment.
          if (attempt >= 5 || globalThis.process?.platform !== 'win32' || !['EPERM', 'EBUSY', 'EACCES'].includes(error.code)) throw error;
          await new Promise(resolve => setTimeout(resolve, 40 * (attempt + 1)));
        }
      }
    } else await io.link(part, `${directory}/${name}`);
  } finally {
    try { await io.unlink(part); } catch (error) { if (error.code !== 'ENOENT') throw error; }
  }
}

// Records [records] (notes-only projections). An existing record is never
// changed except to add a field it lacks, or when [replace](existing, record)
// allows an editorial correction. Conflicting or unreadable records are kept
// and reported. Returns counts and the conflicting file names.
export async function releaseHistoryWrite(io, constants, downloads, records, { owner = null, replace = null, dryRun = false } = {}) {
  const directory = `${downloads}/${releaseHistoryDirectory}`;
  const result = { written: 0, completed: 0, replaced: 0, unchanged: 0, conflicts: [] };
  if (!records.length) return result;
  let present = true;
  if (dryRun) {
    try { const stat = await io.lstat(directory); if (!stat.isDirectory() || stat.isSymbolicLink()) throw Error('release_history_unsafe_directory'); }
    catch (error) { if (error.code !== 'ENOENT') throw error; present = false; }
  } else await releaseHistoryEnsureDirectory(io, directory, owner);
  for (const input of records) {
    const record = releaseHistoryRecord(input);
    if (!record) throw Error('release_history_invalid_record');
    const name = releaseHistoryFileName(record), existing = present ? await releaseHistoryReadRecord(io, constants, downloads, name) : null;
    let next = record, action = 'written';
    if (existing) {
      const old = existing.record;
      if (!old) { result.conflicts.push(name); continue; }
      const compatible = ['releasedAt', 'changelog'].every(key => old[key] === undefined || record[key] === undefined || old[key] === record[key]);
      if (compatible) {
        next = { product: old.product, version: old.version, releasedAt: old.releasedAt ?? record.releasedAt, changelog: old.changelog ?? record.changelog };
        if (next.releasedAt === undefined) delete next.releasedAt;
        if (next.changelog === undefined) delete next.changelog;
        if (releaseHistoryBytes(next).equals(existing.bytes)) { result.unchanged++; continue; }
        action = 'completed';
      } else if (replace?.(old, record)) {
        // A correction changes what it names and keeps the rest (its date).
        next = { product: old.product, version: old.version, releasedAt: record.releasedAt ?? old.releasedAt, changelog: record.changelog ?? old.changelog };
        if (next.releasedAt === undefined) delete next.releasedAt;
        if (next.changelog === undefined) delete next.changelog;
        action = 'replaced';
      } else { result.conflicts.push(name); continue; }
    }
    if (!dryRun) {
      try { await releaseHistoryCommit(io, constants, directory, name, releaseHistoryBytes(next), !!existing, owner); }
      catch (error) { if (error.code !== 'EEXIST') throw error; result.conflicts.push(name); continue; }
    }
    result[action]++;
  }
  return result;
}

// The candidates of one catalog snapshot file; [] when it is not one.
async function releaseHistorySnapshot(io, constants, path) {
  let file;
  try { file = await releaseHistoryReadFile(io, constants, path, releaseHistorySnapshotBytes); }
  catch { return []; }
  return file ? releaseHistoryCandidates(releaseHistoryJson(file.bytes)) : [];
}

// A one-time, idempotent backfill. [tiers] are sources in precedence order
// after this installation's own releases.json: {path} is a catalog snapshot
// file or a directory of them (newest first; archived installers there can
// date their records with [fileDates]); {candidates, fillOnly?} are entries
// collected elsewhere. Only exact published catalog entries introduce a
// version. A record without a date takes its installer's publication time
// (file modification time) from the download directory.
export async function releaseHistoryBackfill(io, constants, downloads, { tiers = [], owner = null, dryRun = false, fileDates = false } = {}) {
  const ordered = [], installers = new Map(), dateDirectories = [downloads], summary = { sources: 0, snapshots: 0 };
  const addEntries = (entries, fillOnly) => {
    for (const entry of entries) {
      if (!fillOnly && !releaseHistoryPublishedEntry(entry)) continue;
      ordered.push(fillOnly ? { entry, fillOnly: true } : entry);
      if (!fillOnly) {
        const key = entry.product + '\n' + entry.version;
        if (!installers.has(key)) installers.set(key, []);
        installers.get(key).push({ filename: entry.filename, size: entry.size });
      }
    }
  };
  addEntries(await releaseHistorySnapshot(io, constants, `${downloads}/releases.json`), false);
  for (const tier of tiers) {
    summary.sources++;
    if (Array.isArray(tier.candidates)) { addEntries(tier.candidates, !!tier.fillOnly); continue; }
    let stat;
    try { stat = await io.lstat(tier.path); } catch (error) { if (error.code === 'ENOENT') continue; throw error; }
    if (stat.isSymbolicLink()) continue;
    if (stat.isFile()) { addEntries(await releaseHistorySnapshot(io, constants, tier.path), !!tier.fillOnly); summary.snapshots++; continue; }
    if (!stat.isDirectory()) continue;
    if (fileDates) dateDirectories.push(tier.path);
    const files = [];
    for (const name of await io.readdir(tier.path)) {
      if (!/\.json$|^releases\.json\./.test(name) || name.startsWith('.')) continue;
      try { const info = await io.lstat(`${tier.path}/${name}`); if (info.isFile() && !info.isSymbolicLink()) files.push({ name, time: info.mtimeMs }); }
      catch { /* Changed while scanning: skipped. */ }
    }
    files.sort((a, b) => b.time - a.time || (a.name < b.name ? -1 : 1));
    for (const { name } of files) {
      const entries = await releaseHistorySnapshot(io, constants, `${tier.path}/${name}`);
      if (entries.length) summary.snapshots++;
      addEntries(entries, !!tier.fillOnly);
    }
  }
  const records = releaseHistoryMerge(ordered);
  let dated = 0;
  for (const record of records) {
    if (record.releasedAt !== undefined) continue;
    let earliest = null;
    for (const { filename, size } of installers.get(record.product + '\n' + record.version) ?? []) {
      if (!/^Multitap[-A-Za-z0-9.]+\.(?:exe|zip|apk)$/.test(filename)) continue;
      for (const directory of dateDirectories) {
        try {
          const info = await io.lstat(`${directory}/${filename}`);
          if (info.isFile() && !info.isSymbolicLink() && info.size === size && (earliest === null || info.mtimeMs < earliest)) earliest = info.mtimeMs;
        } catch { /* Not kept here. */ }
      }
    }
    if (earliest !== null) { record.releasedAt = new Date(Math.floor(earliest)).toISOString(); dated++; }
  }
  const counts = {};
  for (const record of records) counts[record.product] = (counts[record.product] ?? 0) + 1;
  const written = await releaseHistoryWrite(io, constants, downloads, records, { owner, dryRun });
  return { ...summary, candidates: ordered.length, records: records.length, products: counts, withNotes: records.filter(r => r.changelog !== undefined).length, datedFromFiles: dated, dryRun, ...written };
}
