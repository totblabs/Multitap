import { createHash } from 'node:crypto';
import { readFile, stat } from 'node:fs/promises';
import { resolve } from 'node:path';

// Notices a change of the public download catalog (releases.json) so the
// broker can tell connected clients at once instead of each client asking on a
// timer. Robust to atomic replacement (write temp + rename): the check compares
// the file's identity (inode, size, mtime) on every tick and, when that moved,
// the SHA-256 of its content; a touch without a content change pushes nothing.
// The first check only records the baseline.
export class CatalogWatch {
  constructor({ directory, onChange, intervalMs = 2000, maxBytes = 4 * 1024 * 1024 }) {
    Object.assign(this, { directory, onChange, maxBytes });
    this.identity = undefined; this.digest = undefined; this.checking = null; this.closed = false;
    if (!directory) return;
    this.timer = setInterval(() => { void this.check(); }, intervalMs); this.timer.unref();
    void this.check();
  }
  async signature() {
    try {
      const info = await stat(resolve(this.directory, 'releases.json'));
      return `${info.ino}:${info.size}:${info.mtimeMs}`;
    } catch { return null; }
  }
  async content() {
    try {
      const raw = await readFile(resolve(this.directory, 'releases.json'));
      return raw.length > this.maxBytes ? 'oversize' : createHash('sha256').update(raw).digest('hex');
    } catch { return null; }
  }
  // Resolves true when the catalog's content changed since the last check.
  check() {
    if (this.closed || !this.directory) return Promise.resolve(false);
    if (this.checking) return this.checking;
    this.checking = (async () => {
      const identity = await this.signature();
      if (identity === this.identity) return false;
      const digest = identity === null ? null : await this.content();
      const first = this.identity === undefined;
      this.identity = identity;
      if (digest === this.digest) return false;
      this.digest = digest;
      if (first || this.closed) return false;
      try { this.onChange(); } catch { /* A listener cannot break the watch. */ }
      return true;
    })().finally(() => { this.checking = null; });
    return this.checking;
  }
  close() { this.closed = true; if (this.timer) clearInterval(this.timer); }
}
