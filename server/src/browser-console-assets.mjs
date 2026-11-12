import { createHash } from 'node:crypto';
import { constants } from 'node:fs';
import { lstat, open } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { HttpError } from './errors.mjs';

const maximumFile = 4 * 1024 * 1024, maximumTotal = 32 * 1024 * 1024;
const mimes = new Map(Object.entries({ html:'text/html', js:'text/javascript', css:'text/css', json:'application/json', woff2:'font/woff2', woff:'font/woff', png:'image/png', svg:'image/svg+xml', jpg:'image/jpeg', jpeg:'image/jpeg', webp:'image/webp', txt:'text/plain', md:'text/plain' }));
const plain = value => value !== null && typeof value === 'object' && !Array.isArray(value);
const exact = (value, keys) => plain(value) && Object.keys(value).length === keys.length && keys.every(key => Object.hasOwn(value, key));
const safePath = value => typeof value === 'string' && value.length <= 240 && /^[A-Za-z0-9_.@/-]+$/.test(value) && value.split('/').every(part => part && part !== '.' && part !== '..');

// The shipped manifest is the only static allowlist. Read bounded regular files
// once; a partial/tampered package disables this viewer, never ordinary APIs.
export class BrowserConsoleAssets {
  constructor(directory = fileURLToPath(new URL('../admin/console/', import.meta.url))) { this.directory = resolve(directory); this.pending = null; }
  async read(path, maximum) {
    const parts = path.split('/');
    let current = this.directory;
    for (const part of ['', ...parts.slice(0, -1)]) {
      if (part) current = join(current, part);
      const stat = await lstat(current);
      if (!stat.isDirectory() || stat.isSymbolicLink()) throw Error('invalid_asset_directory');
    }
    const filename = join(this.directory, ...parts), before = await lstat(filename);
    if (!before.isFile() || before.isSymbolicLink() || before.size < 1 || before.size > maximum) throw Error('invalid_asset_file');
    const handle = await open(filename, constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0));
    try {
      const stat = await handle.stat();
      if (!stat.isFile() || stat.dev !== before.dev || stat.ino !== before.ino || stat.size !== before.size) throw Error('asset_changed');
      const bytes = Buffer.alloc(stat.size + 1);
      let used = 0;
      while (used < bytes.length) { const read = await handle.read(bytes, used, bytes.length - used, used); if (!read.bytesRead) break; used += read.bytesRead; }
      if (used !== stat.size) throw Error('asset_changed');
      return bytes.subarray(0, used);
    } finally { await handle.close(); }
  }
  async load() {
    const manifest = JSON.parse(await this.read('assets-manifest.json', 262144));
    if (!exact(manifest, ['schema', 'product', 'files']) || manifest.schema !== 1 || manifest.product !== 'multitap-browser-console' || !Array.isArray(manifest.files) || !manifest.files.length || manifest.files.length > 2048) throw Error('invalid_asset_manifest');
    const assets = new Map(); let total = 0;
    for (const entry of manifest.files) {
      if (!exact(entry, ['path', 'bytes', 'sha256']) || !safePath(entry.path) || entry.path === 'assets-manifest.json' || assets.has(entry.path) || !Number.isSafeInteger(entry.bytes) || entry.bytes < 1 || entry.bytes > maximumFile || !/^[a-f0-9]{64}$/.test(entry.sha256)) throw Error('invalid_asset_manifest');
      total += entry.bytes; if (total > maximumTotal) throw Error('asset_limit');
      const bytes = await this.read(entry.path, entry.bytes);
      if (bytes.length !== entry.bytes || createHash('sha256').update(bytes).digest('hex') !== entry.sha256) throw Error('asset_hash_mismatch');
      const extension = entry.path.split('.').pop().toLowerCase();
      assets.set(entry.path, { bytes, mime: mimes.get(extension) ?? 'text/plain' });
    }
    if (!assets.has('index.html')) throw Error('missing_console_shell');
    return assets;
  }
  async get(path) {
    this.pending ??= this.load().catch(() => { throw new HttpError(503, 'console_assets_unavailable'); });
    const asset = (await this.pending).get(path);
    if (!asset) throw new HttpError(404, 'not_found');
    return asset;
  }
}
