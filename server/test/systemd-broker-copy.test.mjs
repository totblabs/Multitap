import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, readFile, rm, writeFile, link, symlink, stat, lstat, mkdir } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { copyBrokerFile } from '../update/systemd.mjs';

// The systemd recovery copies the validated backup out of the broker's own
// data directory as root. A link, a hard-linked file, another account's file
// or a directory is refused, and an existing target is never overwritten.
test('systemd recovery copies only a plain file of the broker account and never follows a link', async t => {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-broker-copy-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const from = join(directory, 'backup.sqlite');
  await writeFile(from, 'synthetic backup bytes');
  const self = { uid: (await stat(from)).uid, gid: (await stat(from)).gid };
  await copyBrokerFile(from, join(directory, 'restore'), self);
  assert.equal(await readFile(join(directory, 'restore'), 'utf8'), 'synthetic backup bytes');
  const refused = error => error instanceof Error && ['update_backup_invalid', 'EEXIST', 'ELOOP', 'EISDIR', 'EPERM', 'EACCES'].some(code => error.message === code || error.code === code);
  await assert.rejects(copyBrokerFile(from, join(directory, 'restore'), self), refused, 'an existing target is not overwritten');
  await assert.rejects(copyBrokerFile(from, join(directory, 'other-owner'), { uid: self.uid + 1, gid: self.gid }), /update_backup_invalid/, 'another account\'s file is refused');
  await writeFile(join(directory, 'twice'), 'x'); await link(join(directory, 'twice'), join(directory, 'twice-2'));
  await assert.rejects(copyBrokerFile(join(directory, 'twice'), join(directory, 'from-twice'), self), /update_backup_invalid/, 'a hard-linked file is refused');
  await mkdir(join(directory, 'folder'));
  await assert.rejects(copyBrokerFile(join(directory, 'folder'), join(directory, 'from-folder'), self), refused, 'a directory is refused');
  let linked = true;
  try { await symlink(join(directory, 'backup.sqlite'), join(directory, 'link')); } catch { linked = false; }
  if (linked) await assert.rejects(copyBrokerFile(join(directory, 'link'), join(directory, 'from-link'), self), refused, 'a link is not followed');
  for (const name of ['from-twice', 'from-folder', 'from-link', 'other-owner']) await assert.rejects(lstat(join(directory, name)), { code: 'ENOENT' }, name + ' left nothing behind');
});
