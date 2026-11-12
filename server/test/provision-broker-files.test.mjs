import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, readFile, rm, writeFile, link } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';

// provision-broker.sh (systemd installs) runs as root over /var/lib/multitap,
// which belongs to the broker's account. Its helpers refuse links, other
// accounts' files, hard links and non-files, and do the work as that account.
// The helpers run here in a shell with `multitap` replaced by this account and
// runuser by a pass-through; skipped where that shell has no stat/id.
const root = fileURLToPath(new URL('../../', import.meta.url));
const bash = process.env.MULTITAP_TEST_BASH || (process.platform === 'win32' ? join(root, '.tools/git/usr/bin/sh.exe') : '/bin/bash');
const probe = spawnSync(bash, ['-c', 'command -v stat >/dev/null && command -v id >/dev/null && id -un'], { encoding: 'utf8' });
const account = probe.status === 0 ? probe.stdout.trim() : '';

test('provision-broker.sh copies broker-owned files only as plain files of the broker account', { skip: !account && 'no shell with stat and id' }, async t => {
  const source = await readFile(join(root, 'server/update/provision-broker.sh'), 'utf8');
  assert.equal(await readFile(join(root, 'deploy/linux/provision-broker.sh'), 'utf8'), source, 'both copies stay identical');
  const start = source.indexOf('# /var/lib/multitap belongs to the broker'), end = source.indexOf('\n}\n', source.indexOf('broker_backups() {')) + 3;
  assert.ok(start > 0 && end > start, 'helpers found');
  // Every root copy, mktemp and rename in the data directory goes through them.
  const backup = source.slice(source.indexOf("printf '%s\\n' 'MULTITAP_UPDATE_PHASE backing_up'"), source.indexOf("printf '%s\\n' 'MULTITAP_UPDATE_PHASE applying'"));
  const rootLines = backup.split('\n').filter(line => /(^|[\s(])(install -d|cp --|chown|chmod|mv -fT|mktemp)\b/.test(line) && !/as_broker (install|cp|chown|chmod|mv|mktemp)\b/.test(line));
  assert.deepEqual(rootLines, [], 'nothing in the backup step runs as root on the data directory');
  const rollback = source.slice(source.indexOf('rollback() {'), source.indexOf('trap rollback ERR'));
  assert.deepEqual(rollback.split('\n').filter(line => /\b(cp --|chown|mktemp \/var)/.test(line) && !/broker_file .*as_broker mktemp.*as_broker cp/.test(line)), [], 'the rollback restore checks the backup and copies it as the broker account');
  assert.match(backup, /systemctl stop multitap-broker\n {2}broker_file \/var\/lib\/multitap\/broker\.sqlite/, 'checked after the broker is stopped');
  const directory = await mkdtemp(join(tmpdir(), 'multitap-provision-files-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const slash = value => value.replaceAll('\\', '/');
  const helpers = source.slice(start, end).replaceAll('/var/lib/multitap/backups', slash(join(directory, 'backups'))).replaceAll("== multitap", '== ' + JSON.stringify(account)).replaceAll("!= multitap", '!= ' + JSON.stringify(account)).replaceAll('multitap:1', account + ':1');
  const run = script => spawnSync(bash, ['-c', 'set -euo pipefail\nrunuser() { shift 3; "$@"; }\n' + helpers + '\n' + script], { encoding: 'utf8' });
  await writeFile(join(directory, 'plain'), 'x'); await writeFile(join(directory, 'twice'), 'x'); await link(join(directory, 'twice'), join(directory, 'twice-2'));
  await mkdir(join(directory, 'folder'));
  const check = name => run(`broker_file ${JSON.stringify(slash(join(directory, name)))} && echo yes || echo no`).stdout.trim();
  assert.equal(check('plain'), 'yes');
  assert.equal(check('twice'), 'no', 'a hard-linked file is refused');
  assert.equal(check('folder'), 'no', 'a directory is refused');
  assert.equal(check('missing'), 'no');
  // The backups directory is made when missing, and refused when it is a file.
  assert.equal(run('broker_backups && echo made').stdout.trim(), 'made');
  assert.equal(run('broker_backups && echo again').stdout.trim(), 'again');
  await rm(join(directory, 'backups'), { recursive: true }); await writeFile(join(directory, 'backups'), 'not a directory');
  const refused = run('broker_backups && echo wrong');
  assert.notEqual(refused.status, 0); assert.match(refused.stderr, /not a plain directory of the broker account/);
});
