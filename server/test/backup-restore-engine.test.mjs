import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdirSync, readFileSync, writeFileSync, existsSync, readdirSync, copyFileSync, symlinkSync, utimesSync } from 'node:fs';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createHash } from 'node:crypto';
import { DatabaseSync } from 'node:sqlite';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { snapshotDatabase, sanitizeSnapshot, rebuildFromBackup } from '../src/backup-database.mjs';
import { ServerRestoreEngine } from '../update/restore-engine.mjs';
import { ServerRestoreDeployment, restoreStagingPath, restoreLiveFiles } from '../update/restore-deployment.mjs';
import { listenUpdateAgent } from '../update/agent.mjs';
import { ServerRestoreClient } from '../src/server-restore-client.mjs';
import { seedServer, temporaryDirectory, travellingRows } from './backup-fixture.mjs';

const sha = bytes => createHash('sha256').update(bytes).digest('hex');
const token = 'e'.repeat(64);
const operation = n => `0000000${n}-1234-4234-8234-123456789abc`;
const quiet = { protocol: 'activity-v1', sessions: { total: 0, stream: 0, attached: 0, tickets: 0 }, channels: { clipboard: 0, clipboardTransfers: 0, drive: 0, driveShares: 0, driveRequests: 0, usb: 0, usbDevices: 0, print: 0, printTransfers: 0 }, operations: { display: 0, sessionCleanup: 0, hostUpdates: 0, power: 0 }, hosts: { connected: 0 }, live: { channels: 0 }, sinceActivityMs: 5, uptimeMs: 10 };
const report = (change = {}) => ({ protocol: 'server-access-activity-v1', fence: true, activity: { ...quiet, sessions: { ...quiet.sessions, ...(change.sessions ?? {}) }, operations: { ...quiet.operations, ...(change.operations ?? {}) } }, connections: { people: change.sessions?.total ?? 0, sessions: change.sessions?.total ?? 0, consoles: 0 } });
const memory = () => { const store = { value: null, writes: [], initialize: async () => {}, read: async () => store.value, write: async state => { store.value = structuredClone(state); store.writes.push(structuredClone(state)); } }; return store; };
const input = (id, extra = {}) => ({ operationId: id, database: { sha256: 'a'.repeat(64), size: 4096 }, vault: { sha256: 'b'.repeat(64) }, schemaVersion: 19, confirm: true, ...extra });
const context = id => ({ fence: { token, operationId: id } });

// ---- the state machine, with a scripted adapter -------------------------
function scripted(overrides = {}) {
  const calls = [];
  const adapter = { calls, reports: [], initialize: async () => {}, current: async () => ({ revision: 'r'.repeat(64) }),
    activity: async (_current, value) => { calls.push('activity'); assert.equal(value, token); return adapter.reports.length ? adapter.reports.shift() : report(); },
    endConnections: async (_current, value) => { calls.push('end'); assert.equal(value, token); return { protocol: 'server-access-connections-v1', fence: true, ended: { people: 1, sessions: 1, consoles: 0 } }; },
    prepare: async value => { calls.push('prepare'); return { schema: 1, operationId: value.operationId, planSha256: 'c'.repeat(64) }; },
    apply: async () => { calls.push('apply'); }, verify: async () => { calls.push('verify'); return true; },
    rollback: async () => { calls.push('rollback'); return true; }, unchanged: async () => { calls.push('unchanged'); return true; }, ...overrides };
  return adapter;
}
const engineOf = (adapter, options = {}) => { const storage = options.storage ?? memory(); return { storage, engine: new ServerRestoreEngine({ adapter, storage, sleep: async () => {}, pollMs: 1, ...options }) }; };

test('a restore runs fence, connection end, durable plan, apply and health check in that order', async () => {
  const adapter = scripted(), { engine, storage } = engineOf(adapter);
  await engine.initialize();
  assert.deepEqual(engine.status(), { protocol: 'server-restore-v1', supported: true, state: 'idle', busy: false, rollbackVerified: false, unchangedVerified: false });
  adapter.reports.push(report(), report({ sessions: { total: 2, stream: 2, attached: 2 } }));
  const accepted = await engine.apply(input(operation(1)), context(operation(1)));
  assert.deepEqual(accepted, { accepted: true, operationId: operation(1), state: 'waiting_idle' }); assert.equal(engine.busy, true);
  await engine.task;
  assert.deepEqual(adapter.calls, ['activity', 'activity', 'end', 'activity', 'activity', 'prepare', 'activity', 'apply', 'verify']);
  assert.equal(engine.status().state, 'succeeded'); assert.equal(engine.busy, false);
  // The plan was in the journal, in state "applying", before apply was called.
  const applying = storage.writes.findIndex(state => state.state === 'applying');
  assert.ok(applying >= 0 && storage.writes[applying].job.operationId === operation(1));
  assert.deepEqual(storage.writes.map(state => state.state).filter((state, index, list) => state !== list[index - 1]), ['waiting_idle', 'applying', 'health_check', 'succeeded']);
  assert.equal(JSON.stringify(storage.writes).includes(token), false, 'the fence token is never written');
});

test('work the broker started is waited for and named; a wait that runs out changes nothing', async () => {
  const adapter = scripted(), { engine } = engineOf(adapter);
  adapter.reports.push(report({ operations: { hostUpdates: 1 } }), report({ operations: { power: 2 } }));
  await engine.apply(input(operation(1)), context(operation(1))); await engine.task;
  assert.equal(engine.status().state, 'succeeded');
  let now = 1000;
  const stuck = scripted({ activity: async () => report({ operations: { hostUpdates: 1 } }) }), second = engineOf(stuck, { now: () => (now += 50000), maxWaitMs: 100000 });
  await second.engine.apply(input(operation(2)), context(operation(2))); await second.engine.task;
  const status = second.engine.status();
  assert.deepEqual({ state: status.state, errorCode: status.errorCode, blockers: status.blockers, unchangedVerified: status.unchangedVerified }, { state: 'failed', errorCode: 'restore_idle_timeout', blockers: [{ code: 'restore_host_updates_busy', count: 1 }], unchangedVerified: true });
  assert.equal(stuck.calls.includes('prepare'), false); assert.equal(stuck.calls.includes('apply'), false);
});

test('a lost fence, a failed preparation and new activity stop before anything is replaced', async () => {
  for (const [name, overrides, code] of [
    ['fence', { activity: async () => ({ protocol: 'server-access-activity-v1', fence: false }) }, 'restore_fence_lost'],
    ['prepare', { prepare: async () => { throw Error('restore_staging_changed'); } }, 'restore_staging_changed'],
    ['unknown error', { prepare: async () => { throw Error('ENOENT: /var/lib/multitap/secret'); } }, 'restore_failed'],
  ]) {
    const adapter = scripted(overrides), { engine } = engineOf(adapter);
    await engine.apply(input(operation(1)), context(operation(1))); await engine.task;
    assert.deepEqual({ state: engine.status().state, errorCode: engine.status().errorCode }, { state: 'failed', errorCode: code }, name);
    assert.equal(adapter.calls.includes('apply'), false, name); assert.equal(adapter.calls.includes('rollback'), false, name);
  }
});

test('a failed health check rolls back; a failed rollback needs the operator and blocks the next restore', async () => {
  const adapter = scripted({ verify: async () => false }), { engine } = engineOf(adapter);
  await engine.apply(input(operation(1)), context(operation(1))); await engine.task;
  assert.deepEqual({ state: engine.status().state, errorCode: engine.status().errorCode, rollbackVerified: engine.status().rollbackVerified }, { state: 'rolled_back', errorCode: 'restore_health_failed', rollbackVerified: true });
  assert.deepEqual(adapter.calls.slice(-2), ['apply', 'rollback']);
  await engine.apply(input(operation(2)), context(operation(2))); await engine.task;
  const broken = scripted({ apply: async () => { throw Error('boom'); }, rollback: async () => false }), second = engineOf(broken);
  await second.engine.apply(input(operation(3)), context(operation(3))); await second.engine.task;
  assert.deepEqual({ state: second.engine.status().state, errorCode: second.engine.status().errorCode }, { state: 'recovery_required', errorCode: 'restore_recovery_required' });
  await assert.rejects(second.engine.apply(input(operation(4)), context(operation(4))), /restore_recovery_required/);
  // The next start of the agent tries the rollback again.
  const retried = scripted(), third = new ServerRestoreEngine({ adapter: retried, storage: second.storage, sleep: async () => {} });
  await third.initialize();
  assert.deepEqual(retried.calls, ['rollback']); assert.equal(third.status().state, 'rolled_back');
});

test('an interrupted restore is rolled back when the agent starts again; one that had not started is simply over', async () => {
  for (const [state, job, expected, calls] of [['waiting_idle', null, 'failed', []], ['applying', { operationId: operation(1) }, 'rolled_back', ['rollback']], ['health_check', { operationId: operation(1) }, 'rolled_back', ['rollback']]]) {
    const storage = memory(); storage.value = { state, operationId: operation(1), job };
    const adapter = scripted(), engine = new ServerRestoreEngine({ adapter, storage, sleep: async () => {} });
    await engine.initialize();
    assert.deepEqual({ state: engine.status().state, errorCode: engine.status().errorCode }, { state: expected, errorCode: 'restore_interrupted' }, state); assert.deepEqual(adapter.calls, calls, state);
  }
});

test('commands are exact: confirmation, hashes, the fence of the same operation, one operation at a time', async () => {
  const adapter = scripted(), { engine } = engineOf(adapter);
  for (const [value, ctx] of [[{ ...input(operation(1)), confirm: false }, context(operation(1))], [{ ...input(operation(1)), path: '/etc/passwd' }, context(operation(1))], [input('not-an-id'), context('not-an-id')],
    [input(operation(1), { database: { sha256: 'a'.repeat(64), size: 4096, path: 'x' } }), context(operation(1))], [input(operation(1), { vault: { sha256: 'zz' } }), context(operation(1))]])
    await assert.rejects(engine.apply(value, ctx), /restore_command_invalid/);
  await assert.rejects(engine.apply(input(operation(1)), context(operation(2))), /restore_fence_invalid/);
  await assert.rejects(engine.apply(input(operation(1)), { fence: { token: 'short', operationId: operation(1) } }), /restore_fence_invalid/);
  await assert.rejects(engineOf(scripted(), { otherBusy: () => true }).engine.apply(input(operation(1)), context(operation(1))), /restore_busy/);
  let release; adapter.activity = () => new Promise(resolve => { release = () => resolve(report()); });
  await engine.apply(input(operation(1)), context(operation(1)));
  await assert.rejects(engine.apply(input(operation(2)), context(operation(2))), /restore_busy/);
  adapter.activity = async () => report(); release(); await engine.task;
  await assert.rejects(engine.apply(input(operation(1)), context(operation(1))), /restore_operation_reused/);
  assert.deepEqual(adapter.calls, ['prepare', 'apply', 'verify']);
});

// ---- the file work, on real files -----------------------------------------
// A stand-in for the access deployment and the service manager: "starting the
// broker" opens the live database with the real Store and the live vault key.
async function installation(t, { unhealthy = () => false } = {}) {
  const root = temporaryDirectory(t, 'multitap-restore-'), data = join(root, 'data'), directory = join(root, 'operations');
  mkdirSync(data); const live = seedServer(data, { seed: 'a' }); live.store.close();
  const state = { running: true, commands: [], seenAtStop: null, seenAtStart: null, startError: null };
  const bytesNow = () => Object.fromEntries(restoreLiveFiles.filter(name => existsSync(join(data, name))).map(name => [name, sha(readFileSync(join(data, name)))]));
  const start = () => {
    if (state.startError) throw state.startError;
    const store = new Store(join(data, 'broker.sqlite'));
    try { const sealed = store.setting('proxmox'); state.opened = { owner: store.db.prepare('SELECT username FROM users WHERE is_primary_admin=1').get().username, secret: sealed ? new Vault(readFileSync(join(data, 'vault.key'))).open(sealed).secret : null }; }
    finally { store.close(); }
    state.running = true;
  };
  const access = { kind: 'systemd', serviceName: 'multitap-broker', data, current: async () => ({ revision: 'r'.repeat(64), envSha256: 'd'.repeat(64), accessSha256: null, transport: null, config: null, installationId: 'i'.repeat(64) }),
    activity: async () => report(), endConnections: async () => ({ protocol: 'server-access-connections-v1', fence: true, ended: { people: 0, sessions: 0, consoles: 0 } }), unchanged: async () => true,
    unchangedAfterRestart: async previous => { assert.equal(previous.revision, 'r'.repeat(64)); return state.running && !unhealthy(state); } };
  const run = async (program, args) => {
    state.commands.push([program, ...args].join(' '));
    assert.equal(program, '/bin/systemctl'); assert.equal(args[1], 'multitap-broker');
    if (args[0] === 'stop') { state.running = false; state.seenAtStop = Object.fromEntries(restoreLiveFiles.filter(name => existsSync(join(data, name))).map(name => [name, sha(readFileSync(join(data, name)))])); }
    else if (args[0] === 'start') { state.seenAtStart = bytesNow(); start(); } else assert.fail('unexpected command');
  };
  const deployment = new ServerRestoreDeployment({ access, directory, run, sleep: async () => {}, healthAttempts: 3 });
  await deployment.initialize();
  // What a broker stages for a restore of server "b": the rebuilt database and its vault key.
  const stage = async (id, { seed = 'b' } = {}) => {
    const other = join(root, 'other-' + id); mkdirSync(other); const source = seedServer(other, { seed }), snapshot = join(other, 'snapshot.sqlite');
    await snapshotDatabase(source.store.db, snapshot); sanitizeSnapshot(snapshot); const key = Buffer.from(source.vault.key); source.store.close();
    const staging = restoreStagingPath(data, id); mkdirSync(staging, { recursive: true });
    rebuildFromBackup({ source: snapshot, work: join(other, 'work.sqlite'), output: join(staging, 'broker.sqlite'), vaultKey: key });
    writeFileSync(join(staging, 'vault.key'), key);
    const database = readFileSync(join(staging, 'broker.sqlite'));
    return { staging, snapshot, key, input: { operationId: id, database: { sha256: sha(database), size: database.length }, vault: { sha256: sha(key) }, schemaVersion: 19 } };
  };
  const liveBytes = () => Object.fromEntries(restoreLiveFiles.filter(name => existsSync(join(data, name))).map(name => [name, sha(readFileSync(join(data, name)))]));
  return { root, data, directory, state, access, deployment, stage, liveBytes };
}

test('a restore replaces the database and the vault key of a different, populated server, and keeps a safety copy', async t => {
  const site = await installation(t), before = site.liveBytes(), staged = await site.stage(operation(1));
  // A leftover write-ahead file of the old database must not survive next to the new one.
  writeFileSync(join(site.data, 'broker.sqlite-wal'), 'old write-ahead bytes'); const withWal = site.liveBytes();
  const job = await site.deployment.prepare(staged.input, { previous: await site.access.current() });
  assert.deepEqual(site.state.commands, [], 'preparing stops nothing'); assert.deepEqual(site.liveBytes(), withWal, 'and changes nothing');
  await site.deployment.apply(job);
  assert.deepEqual(site.state.commands, ['/bin/systemctl stop multitap-broker', '/bin/systemctl start multitap-broker']);
  assert.deepEqual(site.state.seenAtStop, withWal, 'nothing was touched before the broker had stopped');
  assert.equal(await site.deployment.verify(job), true);
  assert.deepEqual(site.state.opened, { owner: 'ownerb', secret: '0f6c2a8e-5b1d-4c3a-9e7f-112233445566b' }, 'the started broker reads the restored accounts and opens the sealed secret');
  assert.deepEqual(readFileSync(join(site.data, 'vault.key')), staged.key);
  assert.deepEqual(travellingRows(join(site.data, 'broker.sqlite')), travellingRows(staged.snapshot));
  // The safety copy is the stopped server's files, byte for byte, with their hashes on record.
  const safety = JSON.parse(readFileSync(join(job.operation, 'safety.json'), 'utf8'));
  assert.equal(safety.files['broker.sqlite'].sha256, before['broker.sqlite']); assert.equal(safety.files['vault.key'].sha256, before['vault.key']);
  assert.equal(safety.files['broker.sqlite-wal'].sha256, withWal['broker.sqlite-wal']); assert.equal(safety.files['broker.sqlite-shm'], null);
  assert.equal(sha(readFileSync(join(job.operation, 'before', 'broker.sqlite'))), before['broker.sqlite']);
  assert.deepEqual(readdirSync(site.data).filter(name => name.includes('.tmp')), []);
});

test('when the restored server does not come up healthy, the engine puts every file back and starts the old server', async t => {
  const site = await installation(t, { unhealthy: state => state.opened?.owner === 'ownerb' });
  writeFileSync(join(site.data, 'broker.sqlite-wal'), 'old write-ahead bytes');
  const before = site.liveBytes(), staged = await site.stage(operation(1)), storage = memory();
  const engine = new ServerRestoreEngine({ adapter: site.deployment, storage, sleep: async () => {}, pollMs: 1 }); await engine.initialize();
  await engine.apply({ ...staged.input, confirm: true }, context(operation(1))); await engine.task;
  assert.deepEqual({ state: engine.status().state, errorCode: engine.status().errorCode, rollbackVerified: engine.status().rollbackVerified }, { state: 'rolled_back', errorCode: 'restore_health_failed', rollbackVerified: true });
  assert.deepEqual(site.state.seenAtStart, before, 'when the old server is started again, database, write-ahead file and vault key are the old ones, byte for byte');
  assert.equal(site.state.opened.owner, 'ownera'); assert.equal(site.state.running, true);
  assert.deepEqual(site.state.commands, ['stop', 'start', 'stop', 'start'].map(verb => `/bin/systemctl ${verb} multitap-broker`));
  // A second attempt is possible afterwards and succeeds when the server is healthy.
  const healthy = await installation(t), again = await healthy.stage(operation(2)), second = new ServerRestoreEngine({ adapter: healthy.deployment, storage: memory(), sleep: async () => {}, pollMs: 1 });
  await second.initialize(); await second.apply({ ...again.input, confirm: true }, context(operation(2))); await second.task;
  assert.equal(second.status().state, 'succeeded'); assert.equal(healthy.state.opened.owner, 'ownerb');
});

test('an agent that died in the middle of a restore puts the safety copy back when it starts again', async t => {
  const site = await installation(t), before = site.liveBytes(), staged = await site.stage(operation(1)), storage = memory();
  site.state.startError = Error('agent killed here');
  const engine = new ServerRestoreEngine({ adapter: site.deployment, storage, sleep: async () => {}, pollMs: 1 }); await engine.initialize();
  // The first agent got as far as replacing the files; its journal says "applying".
  const job = await site.deployment.prepare(staged.input, { previous: await site.access.current() });
  storage.value = { state: 'applying', operationId: operation(1), job };
  await assert.rejects(site.deployment.apply(job), /agent killed here/);
  assert.notDeepEqual(site.liveBytes(), before); assert.equal(site.state.running, false);
  site.state.startError = null;
  const next = new ServerRestoreEngine({ adapter: site.deployment, storage, sleep: async () => {} }); await next.initialize();
  assert.deepEqual({ state: next.status().state, errorCode: next.status().errorCode }, { state: 'rolled_back', errorCode: 'restore_interrupted' });
  assert.deepEqual(site.state.seenAtStart, before); assert.equal(site.state.opened.owner, 'ownera');
});

test('what the broker staged is checked again by the agent: changed bytes, a wrong size, not a database, another schema', async t => {
  const site = await installation(t), before = site.liveBytes(), previous = await site.access.current();
  const one = await site.stage(operation(1));
  const bytes = readFileSync(join(one.staging, 'broker.sqlite')); bytes[bytes.length - 1] ^= 1; writeFileSync(join(one.staging, 'broker.sqlite'), bytes);
  await assert.rejects(site.deployment.prepare(one.input, { previous }), /restore_staging_changed/);
  const two = await site.stage(operation(2));
  await assert.rejects(site.deployment.prepare({ ...two.input, vault: { sha256: 'f'.repeat(64) } }, { previous }), /restore_staging_changed/);
  const three = await site.stage(operation(3)); writeFileSync(join(three.staging, 'vault.key'), Buffer.alloc(33));
  await assert.rejects(site.deployment.prepare(three.input, { previous }), /restore_staging_invalid/);
  const four = await site.stage(operation(4)), junk = Buffer.alloc(8192, 7); writeFileSync(join(four.staging, 'broker.sqlite'), junk);
  await assert.rejects(site.deployment.prepare({ ...four.input, database: { sha256: sha(junk), size: junk.length } }, { previous }), /restore_staged_database_invalid/);
  const five = await site.stage(operation(5));
  await assert.rejects(site.deployment.prepare({ ...five.input, schemaVersion: 18 }, { previous }), /restore_staged_database_invalid/);
  await assert.rejects(site.deployment.prepare({ ...five.input, operationId: operation(6) }, { previous }), /restore_staging_invalid/, 'nothing staged under that operation');
  await assert.rejects(site.deployment.prepare({ ...five.input, operationId: '../../etc' }, { previous }), /restore_command_invalid/);
  await assert.rejects(site.deployment.prepare({ ...five.input, schemaVersion: 19 }, { previous }), /restore_operation_reused/, 'an operation directory is used once');
  assert.deepEqual(site.state.commands, []); assert.deepEqual(site.liveBytes(), before);
});

test('a link planted in the staging directory is not followed', async t => {
  const site = await installation(t), previous = await site.access.current(), staged = await site.stage(operation(1));
  const elsewhere = join(site.root, 'elsewhere.sqlite'); copyFileSync(join(staged.staging, 'broker.sqlite'), elsewhere);
  await rm(join(staged.staging, 'broker.sqlite'));
  try { symlinkSync(elsewhere, join(staged.staging, 'broker.sqlite')); } catch { t.skip('this account cannot create symbolic links here'); return; }
  await assert.rejects(site.deployment.prepare(staged.input, { previous }), /restore_staging_invalid/);
  assert.deepEqual(site.state.commands, []);
});

test('a plan or a safety copy that was changed is not used', async t => {
  const site = await installation(t), staged = await site.stage(operation(1)), previous = await site.access.current();
  const job = await site.deployment.prepare(staged.input, { previous });
  await assert.rejects(site.deployment.apply({ ...job, operation: join(site.root, 'data') }), /restore_recovery_required/);
  await assert.rejects(site.deployment.apply({ ...job, files: { ...job.files, 'broker.sqlite': { sha256: 'a'.repeat(64), size: 1 } } }), /restore_recovery_required/);
  await site.deployment.apply(job);
  writeFileSync(join(job.operation, 'before', 'vault.key'), Buffer.alloc(32, 1));
  await assert.rejects(site.deployment.rollback(job), /restore_recovery_required/);
  assert.equal(site.state.commands.at(-1), '/bin/systemctl start multitap-broker', 'a rollback that cannot trust its copy stops nothing');
});

test('only the two most recent operations keep their files', async t => {
  const site = await installation(t), previous = await site.access.current();
  for (const n of [1, 2, 3]) {
    const staged = await site.stage(operation(n)); await site.deployment.prepare(staged.input, { previous });
    const time = new Date(Date.now() - (10 - n) * 60000); utimesSync(join(site.directory, operation(n)), time, time);
  }
  assert.deepEqual(readdirSync(site.directory).sort(), [operation(2), operation(3)]);
});

test('actual Unix control socket carries a restore and keeps updates out meanwhile', { skip: process.platform !== 'linux' ? 'Linux Unix-socket boundary; not a Windows pass' : false }, async t => {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-restore-uds-')), socket = join(directory, 'control.sock'), calls = [];
  t.after(() => rm(directory, { recursive: true, force: true }));
  const restore = { busy: false, state: { state: 'idle' }, status: () => ({ protocol: 'server-restore-v1', supported: true, state: restore.state.state, busy: restore.busy, rollbackVerified: false, unchangedVerified: false }),
    apply: async (value, ctx) => { calls.push({ value, ctx }); if (value.operationId === operation(9)) throw Error('restore_recovery_required'); restore.busy = true; return { accepted: true, operationId: value.operationId, state: 'waiting_idle' }; } };
  const updates = { busy: false, state: { phase: 'idle' }, status: async () => ({ state: 'idle', feedConfigured: true, applySupported: true }), check: () => assert.fail('a restore blocks new update work') };
  const server = await listenUpdateAgent(updates, socket, { restoreEngine: restore }); t.after(() => new Promise(resolve => server.close(resolve)));
  const client = new ServerRestoreClient(socket);
  assert.equal((await client.status()).state, 'idle');
  await assert.rejects(client.apply(input(operation(9)), context(operation(9))), error => error.status === 409 && error.code === 'restore_recovery_required');
  assert.deepEqual(await client.apply(input(operation(1)), context(operation(1))), { accepted: true, operationId: operation(1), state: 'waiting_idle' });
  assert.deepEqual(calls.at(-1), { value: input(operation(1)), ctx: context(operation(1)) });
  assert.equal(JSON.stringify(await client.status()).includes(token), false);
  const { ServerUpdateClient } = await import('../src/server-update.mjs');
  await assert.rejects(new ServerUpdateClient(socket).check(), error => error.code === 'update_busy');
  const bare = await listenUpdateAgent(updates, join(directory, 'bare.sock')); t.after(() => new Promise(resolve => bare.close(resolve)));
  await assert.rejects(new ServerRestoreClient(join(directory, 'bare.sock')).status(), error => error.status === 503 && error.code === 'restore_unavailable');
});

test('the journal and the plan hold hashes and paths of the installation only', async t => {
  const site = await installation(t), staged = await site.stage(operation(1)), storage = memory();
  const engine = new ServerRestoreEngine({ adapter: site.deployment, storage, sleep: async () => {}, pollMs: 1 }); await engine.initialize();
  await engine.apply({ ...staged.input, confirm: true }, context(operation(1))); await engine.task;
  const text = JSON.stringify(storage.writes) + readFileSync(join(site.directory, operation(1), 'plan.json'), 'utf8') + readFileSync(join(site.directory, operation(1), 'safety.json'), 'utf8');
  for (const secret of [token, staged.key.toString('base64'), staged.key.toString('hex'), '0f6c2a8e-5b1d-4c3a-9e7f-112233445566', 'ownerb', 'scrypt-v1']) assert.equal(text.includes(secret), false, secret);
  const db = new DatabaseSync(join(site.data, 'broker.sqlite'), { readOnly: true }); try { assert.equal(db.prepare('SELECT count(*) AS n FROM sessions').get().n, 0); } finally { db.close(); }
});
