// `sudo multitap restore` (deploy/docker/multitap) end to end, on the machine
// of docker-command-fixture.mjs: Docker is a small fake that runs this
// checkout's restore-cli.mjs, tar and find directly on a folder that stands
// for the data volume, and answers the container state the command waits on.
// What this cannot show is real Docker: the image, its user, mounts and the
// container's own limits. Needs bash (Git Bash on Windows); skipped without it.
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
import { seedServer } from './backup-fixture.mjs';
import { bash, posix, commandMachine } from './docker-command-fixture.mjs';

const fast = { name: 'scrypt', N: 32768, r: 8, p: 1 }, passphrase = 'restore from the terminal 42!';

function files(directory) {
  const out = {};
  const walk = path => { for (const name of readdirSync(path)) { const full = join(path, name); if (statSync(full).isDirectory()) walk(full); else out[relative(directory, full).replaceAll('\\', '/')] = createHash('sha256').update(readFileSync(full)).digest('hex'); } };
  walk(directory);
  return out;
}

async function machine(t) {
  const base = commandMachine(t, { version: VERSION, prefix: 'multitap-docker-restore-', env: ['MULTITAP_SERVER_ADDRESS=192.0.2.20', 'MULTITAP_HTTP_ORIGIN=http://192.0.2.20:18480', 'MULTITAP_ALLOW_INSECURE_HTTP=1', 'MULTITAP_PUBLISH_PORT=0.0.0.0:18480'] });
  const { root, opt, fake, volume, run } = base, a = join(root, 'a');
  mkdirSync(a);
  // Server A makes the backup; server B is the running one whose volume is restored.
  const seeded = seedServer(a, { seed: 'a' }), snapshot = join(root, 'snapshot.sqlite');
  await snapshotDatabase(seeded.store.db, snapshot); sanitizeSnapshot(snapshot); const key = Buffer.from(seeded.vault.key); seeded.store.close();
  const backupFile = join(root, 'multitap-backup-20261110-0730.mtbk');
  await writeBackup({ passphrase, createdAt: '2026-11-10T07:30:00.000Z', serverVersion: VERSION, kdf: fast, databasePath: snapshot, outputPath: backupFile,
    manifest: { schema: 1, origin: 'https://vdi-a.fixture.example', deploymentKind: 'docker', database: { schemaVersion: 19 }, vaultKey: key.toString('base64'), serverAccess: null, excluded: [] } });
  seedServer(volume, { seed: 'b' }).store.close();
  mkdirSync(join(volume, 'downloads')); writeFileSync(join(volume, 'downloads', 'MultitapClient.exe'), 'client of B');
  const original = files(volume);
  // A multitap backup copy of a folder, as `multitap backup` makes it (tar czf -C <data> .).
  const copyOf = (folder, name) => { const file = join(root, name); const made = spawnSync(bash, ['-c', 'tar czf "$1" -C "$2" .', 'tar', posix(file), posix(folder)], { encoding: 'utf8' }); assert.equal(made.status, 0, made.stderr); return file; };
  const safety = () => readdirSync(join(opt, 'backups')).filter(name => name.endsWith('-before-restore.tar.gz'));
  return { ...base, a, key, backupFile, original, copyOf, safety };
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
  writeFileSync(join(m.a, 'poison'), 'the fake container keeps restarting while this is in the volume');
  const unhealthy = m.run(['restore', posix(m.copyOf(m.a, 'multitap-data-1.0.51-20261110-073000.tar.gz')), '--yes']);
  assert.equal(unhealthy.status, 1);
  assert.match(unhealthy.out, /The restored server did not answer\./); assert.match(unhealthy.out, /Went back to the state before the restore/);
  assert.deepEqual(files(m.volume), m.original); assert.equal(unhealthy.state, 'running');
  assert.doesNotMatch(failed.curl + unhealthy.curl, /healthz/, 'the wait reads the container, never an address');
});

test('a multitap backup copy is restored: the data is that of the copy, installer files of this server stay; a damaged or foreign archive is refused before anything changes', { skip: !bash && 'no bash' }, async t => {
  const m = await machine(t);
  writeFileSync(join(m.volume, 'downloads', 'releases.json'), '[]'); writeFileSync(join(m.volume, 'left-behind.txt'), 'anything else of the volume goes');
  const copy = m.copyOf(m.a, 'multitap-data-1.0.51-20261110-073000.tar.gz'), expected = files(m.a);
  const result = m.run(['restore', posix(copy), '--yes']);
  assert.equal(result.status, 0, result.out);
  assert.match(result.out, /A multitap backup copy\. The file is intact\./); assert.match(result.out, /Made: 2026-11-10 07:30 {3}Server version: 1\.0\.51/);
  assert.match(result.out, /4 accounts \(2 admins\)/);
  // Installer files are only ever added to a server and are served by the list alone: they are left where they are.
  assert.deepEqual(files(m.volume), { ...expected, 'downloads/MultitapClient.exe': m.original['downloads/MultitapClient.exe'] }, 'the volume is the copy, beside the installer file that was there');
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
  assert.match(unpack, /^put_volume\(\) \{ # put_volume <file> <keep\|whole>\n  local empty unpack=\(\)\n  plain_copy "\$1" \|\|/, 'the copy is checked first');
  assert.doesNotMatch(script, /--numeric-owner/);
});

// ---- the access settings and the installer files across a restore ----------
const lan = 'http://192.0.2.20:18480', secure = 'https://vdi-b.fixture.example';
const accessFiles = m => Object.fromEntries(Object.entries(files(m.volume)).filter(([name]) => name.startsWith('access/')));

test('a restore never changes how the server is reached: the saved access settings stay this server\'s, whatever the copy carries', { skip: !bash && 'no bash' }, async t => {
  const m = await machine(t);
  // Server B saved its own addresses (as the console's page does); the journal of that save is there too.
  const set = m.run(['address', lan, secure]);
  assert.equal(set.status, 0, set.out);
  const mine = accessFiles(m);
  assert.deepEqual(Object.keys(mine).sort(), ['access/identity', 'access/policy.before-cli.json', 'access/policy.json']);
  // The copy comes from another server: its access/ names that machine's addresses and its installation ID.
  mkdirSync(join(m.a, 'access'));
  writeFileSync(join(m.a, 'access', 'policy.json'), JSON.stringify({ note: 'the addresses of server A' })); writeFileSync(join(m.a, 'access', 'identity'), 'a'.repeat(64) + '\n');
  mkdirSync(join(m.a, 'backup-work')); writeFileSync(join(m.a, 'backup-work', 'access'), 'a file that merely has the same name, deeper in the tree');
  const copy = m.copyOf(m.a, 'multitap-data-1.0.51-20261110-073000.tar.gz');
  const restored = m.run(['restore', posix(copy), '--yes']);
  assert.equal(restored.status, 0, restored.out);
  assert.match(restored.out, /The addresses stay as this server has them now\./);
  assert.deepEqual(accessFiles(m), mine, 'the policy, the identity and the copy for revert are byte for byte the ones from before');
  assert.equal(readFileSync(join(m.volume, 'backup-work', 'access'), 'utf8').startsWith('a file that merely'), true, 'only the top-level access/ of the copy is left out');
  assert.equal(files(m.volume)['broker.sqlite'], files(m.a)['broker.sqlite'], 'the data is the copy\'s');
  const shown = m.run(['address']);
  assert.match(shown.out, new RegExp(secure.replaceAll('.', '\\.') + ' \\(primary\\)')); assert.match(shown.out, /http:\/\/192\.0\.2\.20:18480/);

  // A console backup file: the same, and the restore record names this server's primary address from the saved policy.
  const console = m.run(['restore', posix(m.backupFile), '--yes'], { MULTITAP_BACKUP_PASSPHRASE: passphrase });
  assert.equal(console.status, 0, console.out);
  assert.match(console.out, /Kept: the addresses and the download files/);
  assert.deepEqual(accessFiles(m), mine);
  assert.match(console.calls, new RegExp('apply --work /restore --data /var/lib/multitap --origin ' + secure.replaceAll('.', '\\.') + '\\n'));
  assert.doesNotMatch(restored.out + console.out, /could not be read/);

  // Saved settings the server cannot use stay that way through a restore (they are this server's): the restored
  // server answers, without addresses, and the restore says so with the way out.
  writeFileSync(join(m.volume, 'access', 'policy.json'), '{not a policy');
  const degraded = m.run(['restore', posix(copy), '--yes']);
  assert.equal(degraded.status, 0, degraded.out); assert.match(degraded.out, /Restored\. Server [0-9.]+ is answering\./);
  assert.match(degraded.out, /The saved server access settings could not be read \(server_access_state_invalid\)\./); assert.match(degraded.out, /The admin console is open at http:\/\/192\.0\.2\.20:18480\./); assert.match(degraded.out, /multitap address --reset <url>/);
});

test('a server that has saved no access settings has none after a restore either, and the automatic way back puts the volume back whole', { skip: !bash && 'no bash' }, async t => {
  const m = await machine(t);
  mkdirSync(join(m.a, 'access')); writeFileSync(join(m.a, 'access', 'policy.json'), JSON.stringify({ note: 'the addresses of server A' }));
  const restored = m.run(['restore', posix(m.copyOf(m.a, 'multitap-data-1.0.51-20261110-073000.tar.gz')), '--yes']);
  assert.equal(restored.status, 0, restored.out);
  assert.equal(existsSync(join(m.volume, 'access')), false, 'the copy\'s access/ was not unpacked: the .env lines still start this server');

  // With saved settings and a restore that fails: the safety copy brings back everything, access/ included.
  const n = await machine(t);
  assert.equal(n.run(['address', lan, secure]).status, 0);
  const before = files(n.volume);
  writeFileSync(join(n.a, 'poison'), 'the fake container keeps restarting while this is in the volume');
  const failed = n.run(['restore', posix(n.copyOf(n.a, 'multitap-data-1.0.51-20261110-073000.tar.gz')), '--yes']);
  assert.equal(failed.status, 1);
  assert.match(failed.out, /Went back to the state before the restore\. The server is running again\./);
  assert.deepEqual(files(n.volume), before);
});

test('a copy made before an update carries no installer files and still goes back exactly: the files in the volume stay, the list is the copy\'s', { skip: !bash && 'no bash' }, async t => {
  const m = await machine(t);
  const downloads = join(m.volume, 'downloads');
  writeFileSync(join(downloads, 'releases.json'), '[{"listed":"at the time of the copy"}]'); writeFileSync(join(downloads, 'Multitap-1.0.0-android-arm64-v8a.apk'), 'apk of the earlier release');
  writeFileSync(join(downloads, 'MultitapClient.exe.receipt.json'), '{"receipt":1}');
  // The copy `multitap update` makes (the update itself fails here: the new release never answers).
  const updated = m.run(['update', '9.9.9'], { FAKE_BROKEN_RELEASE: '9.9.9' });
  assert.equal(updated.status, 1); assert.match(updated.out, /9\.9\.9 did not start\. Went back to/);
  const [copy] = m.backups().filter(name => name.endsWith('-before-update.tar.gz'));
  assert.ok(copy, m.backups().join());
  const listed = spawnSync(bash, ['-c', 'tar tzf "$1"', 'tar', posix(join(m.opt, 'backups', copy))], { encoding: 'utf8' }).stdout.split('\n').filter(Boolean);
  assert.ok(listed.includes('./downloads/releases.json') && listed.includes('./downloads/MultitapClient.exe.receipt.json') && listed.includes('./broker.sqlite') && listed.includes('./vault.key'), listed.join());
  assert.deepEqual(listed.filter(name => /\.(exe|apk)$/.test(name)), [], 'no installer file in the copy');
  // The newer release had already added its own files and listed them.
  writeFileSync(join(downloads, 'releases.json'), '[{"listed":"by the newer release"}]'); writeFileSync(join(downloads, 'Multitap-9.9.9-windows-x64.exe'), 'client of the newer release');
  const restored = m.run(['restore', posix(join(m.opt, 'backups', copy)), '--yes']);
  assert.equal(restored.status, 0, restored.out);
  assert.match(restored.out, /A copy made before an update: it carries no app or Host setup files\./);
  assert.equal(readFileSync(join(downloads, 'releases.json'), 'utf8'), '[{"listed":"at the time of the copy"}]');
  for (const [name, text] of [['MultitapClient.exe', 'client of B'], ['Multitap-1.0.0-android-arm64-v8a.apk', 'apk of the earlier release'], ['Multitap-9.9.9-windows-x64.exe', 'client of the newer release']]) assert.equal(readFileSync(join(downloads, name), 'utf8'), text, name);
  assert.equal(files(m.volume)['broker.sqlite'], m.original['broker.sqlite']);
  // A copy someone asked for holds everything.
  assert.equal(m.run(['backup']).status, 0);
  const [whole] = m.backups().filter(name => /^multitap-data-[0-9.]+-\d{8}-\d{6}\.tar\.gz$/.test(name));
  assert.ok(spawnSync(bash, ['-c', 'tar tzf "$1"', 'tar', posix(join(m.opt, 'backups', whole))], { encoding: 'utf8' }).stdout.includes('./downloads/MultitapClient.exe\n'));
});
