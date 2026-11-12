import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { syncBuiltinESMExports } from 'node:module';
import { mkdtemp, writeFile, chmod, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { readProtectedAccessFile } from '../src/config.mjs';

async function fixture(t) {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-access-reader-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const path = join(directory, 'policy.json'), replacement = join(directory, 'replacement.json');
  await writeFile(path, 'original-protected-bytes', { mode: 0o600 });
  await writeFile(replacement, 'replacement-path-bytes', { mode: 0o600 });
  await chmod(path, 0o600); await chmod(replacement, 0o600);
  return { directory, path, replacement };
}
// Windows cannot exercise Unix chmod/uid semantics. Only that metadata is
// projected there; dev/ino, rename, open, descriptor reads and closes remain
// the actual filesystem operations. Linux uses its actual metadata unchanged.
function windowsMetadata(t) {
  if (process.platform !== 'win32') return;
  const project = value => {
    const result = Object.create(value);
    Object.defineProperty(result, 'mode', { value: typeof value.mode === 'bigint' ? value.mode & ~0o022n : value.mode & ~0o022 }); return result;
  };
  const lstat = fs.lstatSync, fstat = fs.fstatSync;
  t.mock.method(fs, 'lstatSync', (...args) => project(lstat(...args)));
  t.mock.method(fs, 'fstatSync', (...args) => project(fstat(...args)));
}
async function hooked(t, body) {
  windowsMetadata(t); syncBuiltinESMExports();
  try { await body(); } finally { t.mock.restoreAll(); syncBuiltinESMExports(); }
}

test('replacement between metadata check and open is rejected using exact inode identity', async t => {
  const f = await fixture(t); const open = fs.openSync; let replaced = false;
  await hooked(t, async () => {
    t.mock.method(fs, 'openSync', (path, ...args) => {
      if (path === f.path && !replaced) { replaced = true; fs.renameSync(f.path, join(f.directory, 'original.json')); fs.renameSync(f.replacement, f.path); }
      return open(path, ...args);
    }); syncBuiltinESMExports();
    assert.throws(() => readProtectedAccessFile(f.path, { production: false }), /server_access_storage_invalid/);
    assert.equal(replaced, true);
  });
});

test('rename after open cannot substitute path bytes for the already validated descriptor', async t => {
  const f = await fixture(t); const open = fs.openSync; let replaced = false;
  await hooked(t, async () => {
    t.mock.method(fs, 'openSync', (path, ...args) => {
      const fd = open(path, ...args);
      if (path === f.path && !replaced) { replaced = true; fs.renameSync(f.path, join(f.directory, 'original.json')); fs.renameSync(f.replacement, f.path); }
      return fd;
    }); syncBuiltinESMExports();
    assert.equal(readProtectedAccessFile(f.path, { production: false }), 'original-protected-bytes');
    assert.equal(fs.readFileSync(f.path, 'utf8'), 'replacement-path-bytes');
  });
});

test('growth during descriptor read is bounded and failure closes the descriptor', async t => {
  const f = await fixture(t); const open = fs.openSync, read = fs.readSync; let fd, grew = false; const requests = [];
  await hooked(t, async () => {
    t.mock.method(fs, 'openSync', (path, ...args) => { const value = open(path, ...args); if (path === f.path && fd === undefined) fd = value; return value; });
    t.mock.method(fs, 'readSync', (descriptor, buffer, offset, count, position) => {
      if (descriptor === fd) { requests.push(count); if (!grew) { grew = true; fs.appendFileSync(f.path, Buffer.alloc(100, 65)); } }
      return read(descriptor, buffer, offset, count, position);
    }); syncBuiltinESMExports();
    assert.throws(() => readProtectedAccessFile(f.path, { production: false, maximum: 32 }), /server_access_storage_invalid/);
    assert(requests.length && requests.every(count => count <= 33));
    assert.throws(() => fs.fstatSync(fd), { code: 'EBADF' });
  });
});

test('missing is optional only before observation; malformed UTF8 and unsafe read bounds are rejected', async t => {
  const f = await fixture(t); await writeFile(f.path, Buffer.from([0xc3, 0x28])); await chmod(f.path, 0o600);
  await hooked(t, async () => {
    assert.equal(readProtectedAccessFile(join(f.directory, 'missing.json'), { production: false }), null);
    assert.throws(() => readProtectedAccessFile(f.path, { production: false }), /server_access_storage_invalid/);
    for (const maximum of [0, -1, 1.5, Infinity, 1000000000]) assert.throws(() => readProtectedAccessFile(f.path, { maximum, production: false }), /server_access_storage_invalid/);
  });
});
