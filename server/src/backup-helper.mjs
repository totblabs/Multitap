// Helper process of 백업·복원 (backup.mjs starts it and ends it after a time
// limit). It does the database work that must not run inside the broker:
// cleaning a snapshot (a VACUUM would stop the broker's event loop) and
// reading an uploaded database (untrusted input stays out of the process that
// holds the live database and every session).
// Input: one JSON object on stdin; paths are chosen by the broker, never by
// a file. Output: one JSON line on stdout. Nothing else is printed.
import { pathToFileURL } from 'node:url';
import { sanitizeSnapshot, rebuildFromBackup } from './backup-database.mjs';
import { BackupError } from './backup-format.mjs';

export function runBackupHelper(input) {
  if (!input || typeof input !== 'object' || Array.isArray(input)) throw new BackupError('backup_unavailable');
  const text = value => { if (typeof value !== 'string' || !value || value.length > 1024 || value.includes('\0')) throw new BackupError('backup_unavailable'); return value; };
  if (input.mode === 'sanitize') return sanitizeSnapshot(text(input.path), { now: input.now });
  if (input.mode === 'rebuild') {
    const vaultKey = Buffer.from(text(input.vaultKey), 'base64');
    if (vaultKey.length !== 32) throw new BackupError('backup_unavailable');
    try { return rebuildFromBackup({ source: text(input.source), work: text(input.work), output: text(input.output), vaultKey, marker: input.marker ?? null, now: input.now }); }
    finally { vaultKey.fill(0); }
  }
  throw new BackupError('backup_unavailable');
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  let result;
  try {
    let size = 0; const chunks = [];
    for await (const chunk of process.stdin) { size += chunk.length; if (size > 65536) throw new BackupError('backup_unavailable'); chunks.push(chunk); }
    result = { ok: true, result: runBackupHelper(JSON.parse(Buffer.concat(chunks).toString('utf8'))) };
  } catch (error) {
    // Only a fixed code leaves this process: no SQL text, path or row content.
    result = { ok: false, code: error instanceof BackupError && /^backup_[a-z_]{1,60}$/.test(error.code) ? error.code : 'backup_helper_failed' };
  }
  process.stdout.write(JSON.stringify(result) + '\n');
}
