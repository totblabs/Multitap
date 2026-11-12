// `sudo multitap restore` (deploy/docker/multitap) end to end, with Docker
// replaced by a small fake: `docker run` of the server image runs this
// checkout's restore-cli.mjs, tar and find directly on a folder that stands
// for the data volume; `docker compose` only records whether the server runs;
// the health address answers while it runs. What this cannot show is real
// Docker: the image, its user, mounts and the container's own limits.
// Needs bash (Git Bash on Windows); skipped without it.
import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync, readFileSync, readdirSync, existsSync, statSync } from 'node:fs';
import { join, relative } from 'node:path';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { writeBackup } from '../src/backup-format.mjs';
import { snapshotDatabase, sanitizeSnapshot } from '../src/backup-database.mjs';
import { VERSION } from '../src/restore-cli.mjs';
import { seedServer, temporaryDirectory } from './backup-fixture.mjs';

const serverDir = fileURLToPath(new URL('..', import.meta.url)), script = fileURLToPath(new URL('../../deploy/docker/multitap', import.meta.url));
const fast = { name: 'scrypt', N: 32768, r: 8, p: 1 }, passphrase = 'restore from the terminal 42!';
function findBash() {
  if (process.env.MULTITAP_TEST_BASH) return process.env.MULTITAP_TEST_BASH;
  const candidates = process.platform === 'win32' ? ['C:\\Program Files\\Git\\bin\\bash.exe', fileURLToPath(new URL('../../.tools/git/bin/bash.exe', import.meta.url))] : ['/bin/bash', '/usr/bin/bash'];
  return candidates.find(path => existsSync(path)) ?? null;
}
const bash = findBash();
// Git Bash wants /c/... paths (a "C:" would read as a remote tar host).
const posix = path => process.platform === 'win32' ? '/' + path[0].toLowerCase() + path.slice(2).replaceAll('\\', '/') : path;

const fakes = {
  // Read as text, so a checkout with CRLF line ends still gives bash LF.
  docker: readFileSync(fileURLToPath(new URL('./fake-docker.sh', import.meta.url)), 'utf8').replaceAll('\r\n', '\n').replace(/^/, '#!/usr/bin/env bash\n'),
  curl: '#!/usr/bin/env bash\n[ "$(cat "$FAKE_ROOT/state")" = running ] || exit 7\n[ -e "$FAKE_ROOT/volume/poison" ] && exit 22\nprintf \'{"version":"%s"}\' "$FAKE_VERSION"\n',
  id: '#!/usr/bin/env bash\necho 0\n',
  chown: '#!/usr/bin/env bash\nexit 0\n',
  sleep: '#!/usr/bin/env bash\nexit 0\n',
  // The broker's account does not exist here, and Windows folders take no Unix modes: owner, group
  // and mode are dropped (what they would be is not tested here).
  install: '#!/usr/bin/env bash\ndir=no; paths=(); skip=no\nfor a in "$@"; do if [ $skip = yes ]; then skip=no; continue; fi; case "$a" in -o|-g|-m) skip=yes ;; -d) dir=yes ;; *) paths+=("$a") ;; esac; done\nif [ $dir = yes ]; then mkdir -p "${paths[@]}"; else cp "${paths[0]}" "${paths[1]}"; fi\n',
};

function files(directory) {
  const out = {};
  const walk = path => { for (const name of readdirSync(path)) { const full = join(path, name); if (statSync(full).isDirectory()) walk(full); else out[relative(directory, full).replaceAll('\\', '/')] = createHash('sha256').update(readFileSync(full)).digest('hex'); } };
  walk(directory);
  return out;
}

async function machine(t) {
  const root = temporaryDirectory(t, 'multitap-docker-restore-'), opt = join(root, 'opt'), fake = join(root, 'fake'), bin = join(root, 'bin'), a = join(root, 'a'), volume = join(fake, 'volume');
  for (const path of [opt, join(opt, 'backups'), fake, volume, bin, a]) mkdirSync(path, { recursive: true });
  for (const [name, text] of Object.entries(fakes)) writeFileSync(join(bin, name), text, { mode: 0o755 });
  writeFileSync(join(opt, '.env'), `MULTITAP_VERSION=${VERSION}\nMULTITAP_LANG=en\nMULTITAP_SERVER_ADDRESS=192.0.2.20\nMULTITAP_HTTP_ORIGIN=http://192.0.2.20:18480\n`);
  writeFileSync(join(opt, 'compose.yaml'), 'name: multitap\n'); writeFileSync(join(fake, 'state'), 'running\n'); writeFileSync(join(fake, 'calls'), '');
  // Server A makes the backup; server B is the running one whose volume is restored.
  const seeded = seedServer(a, { seed: 'a' }), snapshot = join(root, 'snapshot.sqlite');
  await snapshotDatabase(seeded.store.db, snapshot); sanitizeSnapshot(snapshot); const key = Buffer.from(seeded.vault.key); seeded.store.close();
  const backupFile = join(root, 'multitap-backup-20261110-0730.mtbk');
  await writeBackup({ passphrase, createdAt: '2026-11-10T07:30:00.000Z', serverVersion: VERSION, kdf: fast, databasePath: snapshot, outputPath: backupFile,
    manifest: { schema: 1, origin: 'https://vdi-a.fixture.example', deploymentKind: 'docker', database: { schemaVersion: 19 }, vaultKey: key.toString('base64'), serverAccess: null, excluded: [] } });
  seedServer(volume, { seed: 'b' }).store.close();
  mkdirSync(join(volume, 'downloads')); writeFileSync(join(volume, 'downloads', 'MultitapClient.exe'), 'client of B');
  const original = files(volume);
  const run = (args, env = {}) => {
    const result = spawnSync(bash, ['-c', 'PATH="$FAKE_BIN:$PATH"; exec bash "$SCRIPT" "$@"', 'multitap', ...args], { encoding: 'utf8', input: '', timeout: 120000,
      env: { ...process.env, LANG: 'C.UTF-8', NO_COLOR: '1', MULTITAP_DIR: posix(opt), FAKE_ROOT: posix(fake), FAKE_APP: posix(serverDir), FAKE_BIN: posix(bin), SCRIPT: posix(script), NODE: posix(process.execPath), FAKE_VERSION: VERSION, ...env } });
    return { status: result.status, out: result.stdout + result.stderr, calls: readFileSync(join(fake, 'calls'), 'utf8'), state: readFileSync(join(fake, 'state'), 'utf8').trim() };
  };
  // A multitap backup copy of a folder, as `multitap backup` makes it (tar czf -C <data> .).
  const copyOf = (folder, name) => { const file = join(root, name); const made = spawnSync(bash, ['-c', 'tar czf "$1" -C "$2" .', 'tar', posix(file), posix(folder)], { encoding: 'utf8' }); assert.equal(made.status, 0, made.stderr); return file; };
  const safety = () => readdirSync(join(opt, 'backups')).filter(name => name.endsWith('-before-restore.tar.gz'));
  return { root, opt, fake, volume, a, key, backupFile, original, run, copyOf, safety };
}

test('a console backup file is restored: the database and vault key are the backup\'s, the rest of the volume stays, a safety copy is kept', { skip: !bash && 'no bash' }, async t => {
  const m = await machine(t);
  const result = m.run(['restore', posix(m.backupFile), '--yes'], { MULTITAP_BACKUP_PASSPHRASE: passphrase });
  assert.equal(result.status, 0, result.out);
  assert.match(result.out, /A backup file from the admin console/); assert.match(result.out, /4 accounts \(2 admins\) · 4 desktops · 4 assignments · 4 Hosts/);
  assert.match(result.out, /Proxmox connection: https:\/\/pve\.fixture\.example:8006/); assert.match(result.out, /Restored\./);
  assert.deepEqual(readFileSync(join(m.volume, 'vault.key')), m.key);
  assert.equal(readFileSync(join(m.volume, 'downloads', 'MultitapClient.exe'), 'utf8'), 'client of B');
  assert.notEqual(files(m.volume)['broker.sqlite'], m.original['broker.sqlite']);
  assert.equal(m.safety().length, 1, result.out);
  assert.equal(existsSync(join(m.opt, 'restore-work')), false, 'the decrypted files are gone');
  assert.equal(result.state, 'running');
  assert.equal(result.out.includes(passphrase), false); assert.equal(result.calls.includes(passphrase), false, 'the passphrase is never a docker argument');
  assert.ok(result.calls.indexOf('compose stop broker') < result.calls.indexOf('apply --work'), 'stopped before the apply');
});

test('a wrong passphrase changes nothing: the server is not even stopped', { skip: !bash && 'no bash' }, async t => {
  const m = await machine(t);
  const result = m.run(['restore', posix(m.backupFile), '--yes'], { MULTITAP_BACKUP_PASSPHRASE: 'this is not the passphrase' });
  assert.equal(result.status, 1);
  assert.match(result.out, /The backup password is wrong or the file is damaged\. Nothing was changed\./);
  assert.deepEqual(files(m.volume), m.original); assert.deepEqual(m.safety(), []);
  assert.equal(result.calls.includes('compose stop'), false); assert.equal(result.state, 'running');
  const missing = m.run(['restore', posix(join(m.root, 'nothing.mtbk')), '--yes']);
  assert.equal(missing.status, 1); assert.match(missing.out, /Cannot read the file/);
  writeFileSync(join(m.root, 'other.bin'), 'not a backup at all');
  assert.match(m.run(['restore', posix(join(m.root, 'other.bin')), '--yes']).out, /Not a Multitap backup file/);
  assert.match(m.run(['restore', posix(m.backupFile), '--bogus']).out, /Unknown option: --bogus/);
});

test('a failure after the stop puts the safety backup back and says why', { skip: !bash && 'no bash' }, async t => {
  const m = await machine(t);
  const failed = m.run(['restore', posix(m.backupFile), '--yes'], { MULTITAP_BACKUP_PASSPHRASE: passphrase, FAKE_FAIL_APPLY: '1' });
  assert.equal(failed.status, 1);
  assert.match(failed.out, /The restore failed: The checked restore files changed afterwards\./);
  assert.match(failed.out, /Went back to the state before the restore\. The server is running again\./);
  assert.deepEqual(files(m.volume), m.original); assert.equal(failed.state, 'running'); assert.equal(m.safety().length, 1);
  assert.equal((failed.calls.match(/^run .*apply --work/gm) ?? []).length, 1, 'no automatic retry');
  // A restored server that does not answer: the same way back.
  writeFileSync(join(m.a, 'poison'), 'the fake health check fails while this is in the volume');
  const unhealthy = m.run(['restore', posix(m.copyOf(m.a, 'multitap-data-1.0.51-20261110-073000.tar.gz')), '--yes']);
  assert.equal(unhealthy.status, 1);
  assert.match(unhealthy.out, /The restored server did not answer\./); assert.match(unhealthy.out, /Went back to the state before the restore/);
  assert.deepEqual(files(m.volume), m.original); assert.equal(unhealthy.state, 'running');
});

test('a multitap backup copy is restored whole; a damaged or foreign archive is refused before anything changes', { skip: !bash && 'no bash' }, async t => {
  const m = await machine(t);
  const copy = m.copyOf(m.a, 'multitap-data-1.0.51-20261110-073000.tar.gz'), expected = files(m.a);
  const result = m.run(['restore', posix(copy), '--yes']);
  assert.equal(result.status, 0, result.out);
  assert.match(result.out, /A multitap backup copy\. The file is intact\./); assert.match(result.out, /Made: 2026-11-10 07:30 {3}Server version: 1\.0\.51/);
  assert.match(result.out, /4 accounts \(2 admins\)/);
  assert.deepEqual(files(m.volume), expected, 'the whole volume is the copy');
  assert.equal(m.safety().length, 1);
  const before = files(m.volume);
  const damaged = join(m.root, 'damaged.tar.gz'), bytes = readFileSync(copy); writeFileSync(damaged, bytes.subarray(0, Math.floor(bytes.length / 2)));
  assert.match(m.run(['restore', posix(damaged), '--yes']).out, /The backup copy is damaged/);
  const empty = join(m.root, 'empty'); mkdirSync(empty); writeFileSync(join(empty, 'readme.txt'), 'x');
  assert.match(m.run(['restore', posix(m.copyOf(empty, 'foreign.tar.gz')), '--yes']).out, /Not a multitap backup copy \(no broker\.sqlite\)/);
  assert.deepEqual(files(m.volume), before); assert.equal(m.safety().length, 1);
});

test('a multitap backup copy with anything but plain files and folders (here a hard link) is refused before anything changes', { skip: !bash && 'no bash' }, async t => {
  const m = await machine(t);
  const odd = join(m.root, 'odd'); mkdirSync(odd);
  for (const name of ['broker.sqlite', 'vault.key']) writeFileSync(join(odd, name), readFileSync(join(m.a, name)));
  const file = join(m.root, 'multitap-data-1.0.51-20261110-073000.tar.gz');
  const made = spawnSync(bash, ['-c', 'cd "$1" && ln vault.key linked.key && tar czf "$2" .', 'tar', posix(odd), posix(file)], { encoding: 'utf8' });
  assert.equal(made.status, 0, made.stderr);
  assert.match(spawnSync(bash, ['-c', 'tar tvzf "$1"', 'tar', posix(file)], { encoding: 'utf8' }).stdout, /^h/m, 'the copy really holds a hard link');
  const before = files(m.volume), result = m.run(['restore', posix(file), '--yes']);
  assert.notEqual(result.status, 0);
  assert.match(result.out, /This copy holds something other than plain files and folders\. Nothing was changed\./);
  assert.deepEqual(files(m.volume), before); assert.equal(m.safety().length, 0);
  assert.doesNotMatch(result.calls, /compose stop/, 'the server was not even stopped');
  // The unpack itself runs as the server's account without the recorded owners and modes.
  const script = readFileSync(fileURLToPath(new URL('../../deploy/docker/multitap', import.meta.url)), 'utf8');
  const unpack = script.slice(script.indexOf('put_volume() {'), script.indexOf('\n}\n', script.indexOf('put_volume() {')));
  assert.ok(unpack.includes('--user "$BROKER_UID:$BROKER_GID" --cap-drop ALL --security-opt no-new-privileges --entrypoint tar'), unpack);
  assert.ok(unpack.includes('xzf - -C /data --no-same-owner --no-same-permissions'), unpack);
  assert.ok(unpack.startsWith('put_volume() { # put_volume <file>\n  plain_copy "$1" ||'), 'the copy is checked first');
  assert.doesNotMatch(script, /--numeric-owner/);
});
