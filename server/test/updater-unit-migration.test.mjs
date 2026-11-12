import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile, writeFile, mkdtemp, mkdir, chmod, lstat, realpath, rm, symlink } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import {
  planManagedUpdaterUnit, prepareUpdaterUnitMigration,
  applyUpdaterUnitMigration, verifyUpdaterUnitMigration,
  migrateUpdaterUnitAtStartup,
} from '../update/systemd.mjs';

const installer = (await readFile(new URL('../../deploy/linux/install-updater.sh', import.meta.url), 'utf8')).replaceAll('\r\n', '\n');
const currentUnit = installer.split("<<'SERVICE'\n")[1]?.split('\nSERVICE\n')[0] + '\n';
const legacyUnit = currentUnit.replace(' /etc/multitap /etc/systemd/system', ' /etc/systemd/system');
const migratedUnit = legacyUnit.replace(/^ReadWritePaths=[^\n]*/m, '$& /etc/multitap');

test('owned legacy updater gains only its configuration write path; all other security settings and CRLF bytes remain exact', () => {
  assert.match(currentUnit, /^# Multitap managed updater\n/);
  assert.notEqual(legacyUnit, currentUnit);
  for (const newline of ['\n', '\r\n']) {
    const before = legacyUnit.replaceAll('\n', newline), expected = migratedUnit.replaceAll('\n', newline);
    const plan = planManagedUpdaterUnit(before);
    assert.equal(plan.before, before);
    assert.equal(plan.after, expected);
    assert.equal(plan.changed, true);
    assert.equal(planManagedUpdaterUnit(expected).changed, false);
    assert.equal(planManagedUpdaterUnit(expected).after, expected);
  }
});

test('foreign, conflicting or customized updater identity is rejected before a unit write', () => {
  for (const text of [
    legacyUnit.replace('# Multitap managed updater', '# foreign service'),
    legacyUnit.replace('User=root', 'User=other'),
    legacyUnit.replace('Group=multitap', 'Group=other'),
    legacyUnit.replace('ProtectSystem=strict', 'ProtectSystem=full'),
    legacyUnit.replace('Restart=always', 'Restart=on-failure'),
    legacyUnit.replace('/opt/multitap/current/update/agent.mjs', '/tmp/agent.mjs'),
    legacyUnit + 'ReadWritePaths=/tmp\n',
    legacyUnit.replace('/etc/systemd/system', '/etc/../etc/systemd/system'),
    legacyUnit.replace('/etc/systemd/system', '/etc/other'),
    legacyUnit.replace('User=root', 'User=root\nUser=root'),
  ]) assert.throws(() => planManagedUpdaterUnit(text), /update_updater_unit_invalid/);
});

const linuxRoot = { skip: process.platform !== 'linux' || process.getuid?.() !== 0
  ? 'Requires an isolated Linux root-owned temporary directory; never calls systemctl.' : false };

async function fixture(t) {
  const base = await realpath(await mkdtemp(join(tmpdir(), 'multitap-updater-unit-')));
  await chmod(base, 0o700);
  t.after(() => rm(base, { recursive: true, force: true }));
  const stage = join(base, 'operation'), unitPath = join(base, 'multitap-updater.service');
  await mkdir(stage, { mode: 0o700 });
  await writeFile(unitPath, legacyUnit, { mode: 0o640 });
  await chmod(unitPath, 0o640);
  return { base, stage, unitPath };
}

test('exact root-owned unit backup, metadata and unrelated bytes survive apply and restoration without an updater restart', linuxRoot, async t => {
  const { stage, unitPath } = await fixture(t), before = await lstat(unitPath);
  const descriptor = await prepareUpdaterUnitMigration(stage, { unitPath });
  const calls = [], reload = async () => { calls.push('daemon-reload'); };
  assert.equal(descriptor.changed, true);
  await applyUpdaterUnitMigration(stage, { unitPath, reload });
  assert.equal(await readFile(unitPath, 'utf8'), migratedUnit);
  await verifyUpdaterUnitMigration(descriptor, { unitPath, expected: 'after' });
  assert.equal((await lstat(unitPath)).mode & 0o777, before.mode & 0o777);
  assert.equal((await lstat(unitPath)).gid, before.gid);
  assert.equal(await readFile(descriptor.backupPath, 'utf8'), legacyUnit);
  assert.equal((await lstat(descriptor.backupPath)).mode & 0o777, 0o600);
  await applyUpdaterUnitMigration(stage, { unitPath, restore: true, reload });
  assert.equal(await readFile(unitPath, 'utf8'), legacyUnit);
  assert.equal((await lstat(unitPath)).mode & 0o777, before.mode & 0o777);
  assert.equal((await lstat(unitPath)).gid, before.gid);
  assert.deepEqual(calls, ['daemon-reload', 'daemon-reload']);
  assert.equal(await readFile(descriptor.backupPath, 'utf8'), legacyUnit);
});

test('bootstrap refresh is required once, skips already-current installs, and never opens or restarts work', linuxRoot, async t => {
  const { base, unitPath } = await fixture(t);
  const calls = [], reload = async () => { calls.push('daemon-reload'); };
  assert.equal(await migrateUpdaterUnitAtStartup(base, { unitPath, reload }), true);
  assert.equal(await readFile(unitPath, 'utf8'), migratedUnit);
  assert.equal(await migrateUpdaterUnitAtStartup(base, { unitPath, reload }), false);
  assert.deepEqual(calls, ['daemon-reload']);
  const other = join(base, 'fresh'); await mkdir(other, { mode: 0o700 });
  assert.equal(await migrateUpdaterUnitAtStartup(other, { unitPath, reload }), false);
  await assert.rejects(lstat(join(other, 'updater-unit-migration-v1')), { code: 'ENOENT' });
  assert.deepEqual(calls, ['daemon-reload']);
});

test('crash after unit rename without durable reload proof retries reload and requires the startup refresh', linuxRoot, async t => {
  const { base, unitPath } = await fixture(t), stage = join(base, 'updater-unit-migration-v1');
  await mkdir(stage, { mode: 0o700 });
  await prepareUpdaterUnitMigration(stage, { unitPath });
  // Simulates durable candidate bytes with no startup completion checkpoint.
  await writeFile(unitPath, migratedUnit); await chmod(unitPath, 0o640);
  let reloads = 0;
  assert.equal(await migrateUpdaterUnitAtStartup(base, { unitPath, reload: async () => { reloads++; } }), true);
  assert.equal(reloads, 1);
  assert.equal(await migrateUpdaterUnitAtStartup(base, { unitPath, reload: async () => { reloads++; } }), false);
  assert.equal(reloads, 1);
});

test('failed daemon reload restores exact unit and metadata; no completion is reported and a later startup can retry', linuxRoot, async t => {
  const { base, unitPath } = await fixture(t), before = await lstat(unitPath);
  let reloads = 0;
  const reload = async () => { if (++reloads === 1) throw Error('injected_reload_failure'); };
  await assert.rejects(migrateUpdaterUnitAtStartup(base, { unitPath, reload }), /update_updater_unit_migration_failed/);
  assert.equal(reloads, 2, 'restoration reloads the old exact definition');
  assert.equal(await readFile(unitPath, 'utf8'), legacyUnit);
  assert.equal((await lstat(unitPath)).mode & 0o777, before.mode & 0o777);
  assert.equal((await lstat(unitPath)).gid, before.gid);
  const stage = join(base, 'updater-unit-migration-v1');
  assert.equal(await readFile(join(stage, 'updater-unit.before'), 'utf8'), legacyUnit);
  await assert.rejects(lstat(join(stage, 'updater-unit.reloaded.json')), { code: 'ENOENT' });
  assert.equal(await migrateUpdaterUnitAtStartup(base, { unitPath, reload }), true);
  assert.equal(await migrateUpdaterUnitAtStartup(base, { unitPath, reload }), false);
  assert.equal(reloads, 3);
});

test('foreign edits and symlinked units are never replaced or treated as a verified rollback', linuxRoot, async t => {
  const { base, stage, unitPath } = await fixture(t);
  await prepareUpdaterUnitMigration(stage, { unitPath });
  const edited = legacyUnit + '# operator edit\n'; await writeFile(unitPath, edited);
  let reloads = 0;
  await assert.rejects(applyUpdaterUnitMigration(stage, { unitPath, reload: async () => { reloads++; } }), /update_updater_unit_changed/);
  await assert.rejects(applyUpdaterUnitMigration(stage, { unitPath, restore: true, reload: async () => { reloads++; } }), /update_updater_unit_changed/);
  assert.equal(await readFile(unitPath, 'utf8'), edited);
  assert.equal(reloads, 0);
  const alias = join(base, 'alias.service'); await symlink(unitPath, alias);
  await assert.rejects(prepareUpdaterUnitMigration(stage, { unitPath: alias }), /update_installation_invalid/);
});

test('an administrator edit during reload is preserved and reported as recovery required', linuxRoot, async t => {
  const { base, unitPath } = await fixture(t), edited = migratedUnit + '# operator edit during reload\n';
  await assert.rejects(migrateUpdaterUnitAtStartup(base, { unitPath, reload: async () => { await writeFile(unitPath, edited); } }), /update_updater_unit_recovery_required/);
  assert.equal(await readFile(unitPath, 'utf8'), edited);
  assert.equal(await readFile(join(base, 'updater-unit-migration-v1', 'updater-unit.before'), 'utf8'), legacyUnit);
});
